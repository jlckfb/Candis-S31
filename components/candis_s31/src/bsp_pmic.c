/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#include "tg28_sw.h"

#include "bsp/candis_s31.h"

static const char *TAG = "candis_pmic";
static tg28_sw_handle_t s_pmic;
static bsp_pmic_early_snapshot_t s_early_snapshot;
static bool s_early_snapshot_attempted;
/* Battery-model validity: the REGA1 128-byte model governs whether the
 * TG28 SOC estimate means anything, and which model is active: the BSP
 * reference default or a caller-supplied override. Both false unless a
 * programming attempt succeeded this boot; never survives a failed
 * init/retry or deinit. */
static bool s_fuel_gauge_valid;
static bool s_fuel_gauge_reference_model;

_Static_assert((int)BSP_PMIC_REGULATOR_COUNT == (int)TG28_SW_REGULATOR_COUNT,
               "BSP and TG28_SW regulator lists must stay aligned");
_Static_assert((int)BSP_PMIC_ADC_COUNT == (int)TG28_SW_ADC_CHANNEL_COUNT,
               "BSP and TG28_SW ADC channel lists must stay aligned");
_Static_assert((int)BSP_PMIC_SWITCH_COUNT == (int)TG28_SW_SWITCH_COUNT,
               "BSP and TG28_SW switch lists must stay aligned");
_Static_assert((int)BSP_PMIC_SWITCH_DC1SW == (int)TG28_SW_SWITCH_DC1SW &&
               (int)BSP_PMIC_SWITCH_DC4SW == (int)TG28_SW_SWITCH_DC4SW,
               "BSP and TG28_SW switch order must stay aligned");

/* EVT1 measurements show that TG28's low-speed ADC does not update every
 * newly enabled channel within the old 50 ms delay: VSYS first became valid
 * at 50 ms, TDIE at 100 ms, and VBUS only at 1000 ms. The compatibility
 * single-channel API must favor correct data over a short blocking time. */
#define BSP_PMIC_ADC_DEFAULT_SETTLE_MS 200
#define BSP_PMIC_ADC_VBUS_SETTLE_MS    1000

#define BSP_PMIC_REG_STATUS0            0x00
#define BSP_PMIC_REG_POWER_ON_SOURCE    0x20
#define BSP_PMIC_REG_ADC_CONTROL        0x30
#define BSP_PMIC_REG_ADC_VBAT_H         0x34
#define BSP_PMIC_REG_IRQ_STATUS0        0x48
#define BSP_PMIC_ADC_DIAGNOSTIC_MASK    0x1D

static const uint8_t s_adc_result_registers[BSP_PMIC_ADC_COUNT] = {
    [BSP_PMIC_ADC_VBAT] = BSP_PMIC_REG_ADC_VBAT_H,
    [BSP_PMIC_ADC_TS] = BSP_PMIC_REG_ADC_VBAT_H + 2,
    [BSP_PMIC_ADC_VBUS] = BSP_PMIC_REG_ADC_VBAT_H + 4,
    [BSP_PMIC_ADC_VSYS] = BSP_PMIC_REG_ADC_VBAT_H + 6,
    [BSP_PMIC_ADC_TDIE] = BSP_PMIC_REG_ADC_VBAT_H + 8,
};

static const uint32_t s_adc_diagnostic_times_ms[
    BSP_PMIC_ADC_DIAGNOSTIC_SAMPLE_COUNT] = {
    0, 50, 100, 200, 500, 1000, 2000,
};

static tg28_sw_regulator_t to_tg28_regulator(bsp_pmic_regulator_t regulator)
{
    return (tg28_sw_regulator_t)regulator;
}

static tg28_sw_power_switch_t to_tg28_switch(bsp_pmic_switch_t sw)
{
    return (tg28_sw_power_switch_t)sw;
}

static uint32_t adc_settle_time_ms(bsp_pmic_adc_channel_t channel)
{
    return channel == BSP_PMIC_ADC_VBUS ?
           BSP_PMIC_ADC_VBUS_SETTLE_MS : BSP_PMIC_ADC_DEFAULT_SETTLE_MS;
}

static void capture_early_snapshot(void)
{
    if (s_early_snapshot_attempted) {
        return;
    }
    s_early_snapshot_attempted = true;
    memset(&s_early_snapshot, 0, sizeof(s_early_snapshot));

    if (tg28_sw_read_registers(s_pmic, BSP_PMIC_REG_STATUS0,
                               s_early_snapshot.status,
                               sizeof(s_early_snapshot.status)) == ESP_OK) {
        s_early_snapshot.valid_mask |= BSP_PMIC_EARLY_STATUS_VALID;
    }
    if (tg28_sw_read_registers(s_pmic, BSP_PMIC_REG_POWER_ON_SOURCE,
                               s_early_snapshot.power_source,
                               sizeof(s_early_snapshot.power_source)) == ESP_OK) {
        s_early_snapshot.valid_mask |= BSP_PMIC_EARLY_POWER_SOURCE_VALID;
    }
    if (tg28_sw_read_registers(s_pmic, BSP_PMIC_REG_ADC_CONTROL,
                               &s_early_snapshot.adc_control,
                               sizeof(s_early_snapshot.adc_control)) == ESP_OK) {
        s_early_snapshot.valid_mask |= BSP_PMIC_EARLY_ADC_CONTROL_VALID;
    }
    if (tg28_sw_read_registers(s_pmic, BSP_PMIC_REG_IRQ_STATUS0,
                               s_early_snapshot.irq_status,
                               sizeof(s_early_snapshot.irq_status)) == ESP_OK) {
        s_early_snapshot.valid_mask |= BSP_PMIC_EARLY_IRQ_STATUS_VALID;
    }

    if (s_early_snapshot.valid_mask !=
            (BSP_PMIC_EARLY_STATUS_VALID |
             BSP_PMIC_EARLY_POWER_SOURCE_VALID |
             BSP_PMIC_EARLY_ADC_CONTROL_VALID |
             BSP_PMIC_EARLY_IRQ_STATUS_VALID)) {
        ESP_LOGW(TAG, "early PMIC snapshot incomplete: valid_mask=0x%02x",
                 s_early_snapshot.valid_mask);
    }
}

esp_err_t bsp_pmic_init(void)
{
    if (s_pmic != NULL) {
        return ESP_OK;
    }
    s_fuel_gauge_valid = false;
    s_fuel_gauge_reference_model = false;

    i2c_master_bus_handle_t bus = bsp_lp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_FAIL, TAG, "low-power I2C init failed");

    const tg28_sw_config_t config = {
        .device_address = BSP_TG28_SW_I2C_ADDRESS,
        .scl_speed_hz = TG28_SW_I2C_CLOCK_HZ,
    };
    esp_err_t error = tg28_sw_create(bus, &config, &s_pmic);
    if (error == ESP_OK) {
        /* Preserve the actual incoming state before any BSP write, fuel-gauge
         * reset/programming sequence, or IRQ clear can destroy evidence. */
        capture_early_snapshot();
    }
    uint16_t previous_input_limit = 0;
    if (error == ESP_OK) {
        /* REG62 outlives an ESP-only reset, so any charge target a
         * previous session raised is still active here. Force the safe
         * default and verify the exact readback before anything else:
         * a weak source plugged in later must never inherit it. */
        error = tg28_sw_set_charge_current(
                    s_pmic, BSP_PMIC_SAFE_CHARGE_CURRENT_MA);
    }
    uint16_t charge_current_ma = 0;
    if (error == ESP_OK) {
        error = tg28_sw_get_charge_current(s_pmic, &charge_current_ma);
    }
    if (error == ESP_OK &&
            charge_current_ma != BSP_PMIC_SAFE_CHARGE_CURRENT_MA) {
        ESP_LOGE(TAG, "charge current readback %u mA != %u mA",
                 charge_current_ma, BSP_PMIC_SAFE_CHARGE_CURRENT_MA);
        error = ESP_FAIL;
    }
    uint16_t charge_voltage_mv = 0;
    if (error == ESP_OK) {
        error = tg28_sw_set_charge_voltage(s_pmic,
                                           BSP_PMIC_SAFE_CHARGE_VOLTAGE_MV);
    }
    if (error == ESP_OK) {
        error = tg28_sw_get_charge_voltage(s_pmic, &charge_voltage_mv);
    }
    if (error == ESP_OK &&
            charge_voltage_mv != BSP_PMIC_SAFE_CHARGE_VOLTAGE_MV) {
        ESP_LOGE(TAG, "charge voltage readback %u mV != %u mV",
                 charge_voltage_mv, BSP_PMIC_SAFE_CHARGE_VOLTAGE_MV);
        error = ESP_FAIL;
    }
    uint16_t precharge_ma = 0;
    if (error == ESP_OK) {
        error = tg28_sw_set_precharge_current(
                    s_pmic, BSP_PMIC_SAFE_PRECHARGE_CURRENT_MA);
    }
    if (error == ESP_OK) {
        error = tg28_sw_get_precharge_current(s_pmic, &precharge_ma);
    }
    if (error == ESP_OK &&
            precharge_ma != BSP_PMIC_SAFE_PRECHARGE_CURRENT_MA) {
        ESP_LOGE(TAG, "precharge readback %u mA != %u mA",
                 precharge_ma, BSP_PMIC_SAFE_PRECHARGE_CURRENT_MA);
        error = ESP_FAIL;
    }
    uint16_t term_ma = 0;
    bool term_enabled = false;
    if (error == ESP_OK) {
        error = tg28_sw_set_termination_current(
                    s_pmic, BSP_PMIC_SAFE_TERMINATION_CURRENT_MA, true);
    }
    if (error == ESP_OK) {
        error = tg28_sw_get_termination_current(s_pmic, &term_ma,
                                                &term_enabled);
    }
    if (error == ESP_OK &&
            (term_ma != BSP_PMIC_SAFE_TERMINATION_CURRENT_MA ||
             !term_enabled)) {
        ESP_LOGE(TAG, "termination readback %u mA enabled=%d != %u mA enabled=1",
                 term_ma, term_enabled, BSP_PMIC_SAFE_TERMINATION_CURRENT_MA);
        error = ESP_FAIL;
    }
    uint16_t input_limit_ma = 0;
    if (error == ESP_OK) {
        error = tg28_sw_get_input_current_limit(s_pmic,
                                                &previous_input_limit);
    }
    if (error == ESP_OK) {
        /* Type-C1 has fixed Rd resistors but no CC-current detector, so
         * the input limit is forced to the board default ceiling
         * (BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA, 2000 mA) instead of the
         * 1500 mA POR value - strictly BEFORE the ~1 s battery-model
         * download, so the source limit is in force while the gauge
         * subsystem draws its programming current. The register outlives
         * an ESP-only reset, so a limit raised by a previous session is
         * collapsed here before a weak source can be plugged in. */
        error = tg28_sw_set_input_current_limit(
                    s_pmic, BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA);
    }
    if (error == ESP_OK) {
        error = tg28_sw_get_input_current_limit(s_pmic, &input_limit_ma);
    }
    if (error == ESP_OK &&
            input_limit_ma != BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA) {
        ESP_LOGE(TAG, "input limit readback %u mA != %u mA",
                 input_limit_ma, BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA);
        error = ESP_FAIL;
    }
    if (error == ESP_OK &&
            previous_input_limit != BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA) {
        ESP_LOGW(TAG, "input current limit clamped from %u mA to %u mA",
                 previous_input_limit, BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA);
    }
    if (error == ESP_OK) {
        /* One-shot afternoon evidence: every safe-profile register read
         * back exactly as written. */
        ESP_LOGI(TAG,
                 "safe profile verified: input=%u mA (was %u), charge=%u mA, "
                 "precharge=%u mA, termination=%u mA/en, Vchg=%u mV",
                 input_limit_ma, previous_input_limit, charge_current_ma,
                 precharge_ma, term_ma, charge_voltage_mv);
    }
    if (error == ESP_OK) {
        /* Use the model already stored in the TG28 silicon. Reading the ROM
         * verifies that the gauge model is accessible without redistributing
         * vendor-owned model bytes in this Apache-2.0 BSP. Applications with
         * a licensed battery-specific model can still call the runtime
         * override API. */
        uint8_t rom_model[TG28_SW_BATTERY_MODEL_SIZE] = {0};
        const esp_err_t model_err = tg28_sw_read_battery_model(
                s_pmic, TG28_SW_BATTERY_MODEL_ROM, rom_model,
                sizeof(rom_model));
        if (model_err == ESP_OK) {
            s_fuel_gauge_valid = true;
            s_fuel_gauge_reference_model = true;
            ESP_LOGI(TAG, "TG28 factory ROM battery model verified (%u bytes)",
                     (unsigned)sizeof(rom_model));
        } else {
            ESP_LOGW(TAG, "TG28 factory ROM battery model read failed: %s "
                     "(fuel gauge stays invalid; charging continues)",
                     esp_err_to_name(model_err));
        }
    }
    if (error == ESP_OK) {
        /* Clear any latched interrupt status before enabling the power-key
         * IRQs, like the vendor axp-core driver does at irq-chip init
         * (write 1 to clear every pending bit), so stale events from the
         * boot ROM or a previous reset do not fire immediately. REG48-4A
         * were already preserved by capture_early_snapshot(), before the
         * charge profile or fuel-gauge programming could alter them. */
        uint8_t pending[3] = {0};
        error = tg28_sw_get_and_clear_interrupts(s_pmic, pending);
    }
    if (error == ESP_OK) {
        error = tg28_sw_configure_power_key_interrupts(s_pmic,
                TG28_SW_POWER_KEY_IRQ_ALL);
    }
    if (error == ESP_OK) {
        /* Board-level choice: the Candis-S31 battery has no NTC resistor,
         * so the TS pin is the external fixed input and its current source
         * stays off. The 50uA value is the power-on default and is
         * irrelevant while the current source is off. */
        error = tg28_sw_set_ts_config(s_pmic, TG28_SW_TS_MODE_EXTERNAL_FIXED,
                                      TG28_SW_TS_CURRENT_SOURCE_OFF, 50);
    }
    if (error != ESP_OK && s_pmic != NULL) {
        tg28_sw_delete(s_pmic);
        s_pmic = NULL;
        /* A failed init/retry must never inherit model validity. The early
         * register snapshot intentionally survives: a retry would otherwise
         * replace the incoming power-loss evidence with state changed by the
         * failed attempt itself. */
        s_fuel_gauge_valid = false;
        s_fuel_gauge_reference_model = false;
    }
    return error;
}

esp_err_t bsp_pmic_deinit(void)
{
    if (s_pmic == NULL) {
        return ESP_OK;
    }
    const esp_err_t error = tg28_sw_delete(s_pmic);
    if (error == ESP_OK) {
        s_pmic = NULL;
        s_fuel_gauge_valid = false;
        s_fuel_gauge_reference_model = false;
    }
    return error;
}

esp_err_t bsp_pmic_set_precharge_current(uint16_t milliamps)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_set_precharge_current(s_pmic, milliamps);
}

esp_err_t bsp_pmic_get_precharge_current(uint16_t *milliamps)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_get_precharge_current(s_pmic, milliamps);
}

esp_err_t bsp_pmic_set_termination_current(uint16_t milliamps, bool enable)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_set_termination_current(s_pmic, milliamps, enable);
}

esp_err_t bsp_pmic_get_termination_current(uint16_t *milliamps, bool *enabled)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_get_termination_current(s_pmic, milliamps, enabled);
}

esp_err_t bsp_pmic_get_status(bsp_pmic_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");

    tg28_sw_status_t device_status = {0};
    ESP_RETURN_ON_ERROR(tg28_sw_get_status(s_pmic, &device_status), TAG,
                        "TG28_SW status read failed");
    status->chip_id = device_status.chip_id;
    status->common_status0 = device_status.common_status0;
    status->common_status1 = device_status.common_status1;
    status->battery_mv = device_status.battery_mv;
    status->battery_percent = device_status.battery_percent;
    status->fuel_gauge_valid = s_fuel_gauge_valid;
    status->fuel_gauge_reference_model = s_fuel_gauge_reference_model;
    status->battery_present = device_status.battery_present;
    status->vbus_present = device_status.vbus_present;
    status->charging = device_status.charging;
    status->charge_done = device_status.charge_done;
    return ESP_OK;
}

esp_err_t bsp_pmic_get_power_on_source(uint8_t *source)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_get_power_on_source(s_pmic, source);
}

esp_err_t bsp_pmic_power_off(void)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_power_off(s_pmic);
}

esp_err_t bsp_pmic_set_charge_current(uint16_t milliamps)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_set_charge_current(s_pmic, milliamps);
}

esp_err_t bsp_pmic_get_charge_current(uint16_t *milliamps)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_get_charge_current(s_pmic, milliamps);
}

esp_err_t bsp_pmic_set_input_current_limit(uint16_t milliamps)
{
    /* With fixed Rd and no Rp detector the board cannot prove a 1.5 A/3 A
     * source, so levels above the 500 mA boot default are accepted here
     * only because callers must have independently verified the connected
     * source (factory console: source_verified token, current probe, VBUS
     * droop watch). 100 mA remains selectable for lab use but starves RF
     * bursts on this board; 900/1000/1500/2000 exist for diagnosis. */
    switch (milliamps) {
    case 100: case 500: case 900: case 1000: case 1500: case 2000:
        break;
    default:
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG,
                            "unsupported input limit %u mA", milliamps);
    }
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_set_input_current_limit(s_pmic, milliamps);
}

esp_err_t bsp_pmic_get_power_off_source(uint8_t *source)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_get_power_off_source(s_pmic, source);
}

esp_err_t bsp_pmic_get_input_current_limit(uint16_t *milliamps)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_get_input_current_limit(s_pmic, milliamps);
}

esp_err_t bsp_pmic_set_vindpm(uint16_t millivolts)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_set_vindpm(s_pmic, millivolts);
}

esp_err_t bsp_pmic_get_vindpm(uint16_t *millivolts)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_get_vindpm(s_pmic, millivolts);
}

esp_err_t bsp_pmic_set_charge_voltage(uint16_t millivolts)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_set_charge_voltage(s_pmic, millivolts);
}

esp_err_t bsp_pmic_get_charge_voltage(uint16_t *millivolts)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_get_charge_voltage(s_pmic, millivolts);
}

esp_err_t bsp_pmic_read_registers(uint8_t register_address, uint8_t *values,
                                  size_t count)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_read_registers(s_pmic, register_address, values, count);
}

esp_err_t bsp_pmic_read_battery_model(bool from_sram, uint8_t *model, size_t size)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_read_battery_model(s_pmic,
                                      from_sram ? TG28_SW_BATTERY_MODEL_SRAM
                                      : TG28_SW_BATTERY_MODEL_ROM,
                                      model, size);
}

esp_err_t bsp_pmic_program_battery_model(const uint8_t *model, size_t size)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    /* Custom override: drop validity first so a failed or partial write
     * can never leave the previous (reference) model reported as the
     * active valid one. */
    s_fuel_gauge_valid = false;
    s_fuel_gauge_reference_model = false;
    const esp_err_t error = tg28_sw_program_battery_model(s_pmic, model,
                            size);
    if (error == ESP_OK) {
        /* A full custom model is now verified in the gauge: valid, and
         * explicitly not the BSP reference default. */
        s_fuel_gauge_valid = true;
        s_fuel_gauge_reference_model = false;
    }
    return error;
}

esp_err_t bsp_pmic_regulator_set_voltage(bsp_pmic_regulator_t regulator,
        uint16_t millivolts)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_regulator_set_voltage(s_pmic, to_tg28_regulator(regulator),
                                         millivolts);
}

esp_err_t bsp_pmic_regulator_get_voltage(bsp_pmic_regulator_t regulator,
        uint16_t *millivolts)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_regulator_get_voltage(s_pmic, to_tg28_regulator(regulator),
                                         millivolts);
}

esp_err_t bsp_pmic_regulator_enable(bsp_pmic_regulator_t regulator, bool enable)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_regulator_enable(s_pmic, to_tg28_regulator(regulator), enable);
}

esp_err_t bsp_pmic_regulator_is_enabled(bsp_pmic_regulator_t regulator, bool *enabled)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_regulator_is_enabled(s_pmic, to_tg28_regulator(regulator), enabled);
}

esp_err_t bsp_pmic_switch_enable(bsp_pmic_switch_t sw, bool enable)
{
    ESP_RETURN_ON_FALSE(sw < BSP_PMIC_SWITCH_COUNT, ESP_ERR_INVALID_ARG, TAG,
                        "invalid load switch");
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_switch_enable(s_pmic, to_tg28_switch(sw), enable);
}

esp_err_t bsp_pmic_switch_is_enabled(bsp_pmic_switch_t sw, bool *enabled)
{
    ESP_RETURN_ON_FALSE(sw < BSP_PMIC_SWITCH_COUNT && enabled != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid load switch request");
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_switch_is_enabled(s_pmic, to_tg28_switch(sw), enabled);
}

const char *bsp_pmic_switch_name(bsp_pmic_switch_t sw)
{
    return tg28_sw_switch_name(to_tg28_switch(sw));
}

esp_err_t bsp_pmic_get_and_clear_interrupts(uint8_t status[3])
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    return tg28_sw_get_and_clear_interrupts(s_pmic, status);
}

esp_err_t bsp_pmic_get_boot_irq_snapshot(uint8_t status[3], bool *valid)
{
    ESP_RETURN_ON_FALSE(status != NULL && valid != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "status/valid is NULL");
    /* This compatibility accessor now returns the IRQ part of the stronger
     * post-create/pre-configuration snapshot. */
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    memcpy(status, s_early_snapshot.irq_status,
           sizeof(s_early_snapshot.irq_status));
    *valid = (s_early_snapshot.valid_mask &
              BSP_PMIC_EARLY_IRQ_STATUS_VALID) != 0;
    return ESP_OK;
}

esp_err_t bsp_pmic_get_early_snapshot(bsp_pmic_early_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "snapshot is NULL");
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    *snapshot = s_early_snapshot;
    return ESP_OK;
}

const char *bsp_pmic_regulator_name(bsp_pmic_regulator_t regulator)
{
    return tg28_sw_regulator_name(to_tg28_regulator(regulator));
}

esp_err_t bsp_pmic_read_adc_mv(bsp_pmic_adc_channel_t channel,
                               uint16_t *millivolts)
{
    ESP_RETURN_ON_FALSE(millivolts != NULL && channel < BSP_PMIC_ADC_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ADC channel request");
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    const tg28_sw_adc_channel_t adc = (tg28_sw_adc_channel_t)channel;
    bool was_enabled = false;
    ESP_RETURN_ON_ERROR(tg28_sw_get_adc_channel_enable(s_pmic, adc,
                        &was_enabled),
                        TAG, "ADC channel state read failed");
    if (!was_enabled) {
        ESP_RETURN_ON_ERROR(tg28_sw_set_adc_channel_enable(s_pmic, adc, true),
                            TAG, "ADC channel enable failed");
        /* The result register only refreshes after a conversion cycle. */
        vTaskDelay(pdMS_TO_TICKS(adc_settle_time_ms(channel)));
    }
    const esp_err_t error = tg28_sw_read_adc_channel(s_pmic, adc, millivolts);
    if (!was_enabled) {
        tg28_sw_set_adc_channel_enable(s_pmic, adc, false);
    }
    return error;
}

static esp_err_t read_adc_diagnostic_sample(
    bsp_pmic_adc_diagnostic_sample_t *sample)
{
    for (int channel = 0; channel < BSP_PMIC_ADC_COUNT; ++channel) {
        uint8_t value[2] = {0};
        ESP_RETURN_ON_ERROR(
            tg28_sw_read_registers(s_pmic, s_adc_result_registers[channel],
                                   value, sizeof(value)),
            TAG, "ADC raw channel %d read failed", channel);
        /* Datasheet 6.10: read high first, then low; high[5:0] are bits
         * 13:8. Preserve values through 0x3fff so the FAQ's negative-input
         * overflow signature remains visible. */
        sample->raw[channel] =
            ((uint16_t)(value[0] & 0x3F) << 8) | value[1];
    }
    return ESP_OK;
}

esp_err_t bsp_pmic_run_adc_diagnostic(bsp_pmic_adc_diagnostic_t *diagnostic)
{
    ESP_RETURN_ON_FALSE(diagnostic != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "diagnostic is NULL");
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    memset(diagnostic, 0, sizeof(*diagnostic));

    esp_err_t first_error = tg28_sw_read_registers(
                                s_pmic, BSP_PMIC_REG_ADC_CONTROL,
                                &diagnostic->reg30_original, 1);
    bool control_write_attempted = false;
    if (first_error == ESP_OK) {
        diagnostic->reg30_enabled = diagnostic->reg30_original |
                                    BSP_PMIC_ADC_DIAGNOSTIC_MASK;
        control_write_attempted = true;
        first_error = tg28_sw_write_register(s_pmic,
                                             BSP_PMIC_REG_ADC_CONTROL,
                                             diagnostic->reg30_enabled);
    }
    if (first_error == ESP_OK) {
        first_error = tg28_sw_read_registers(
                          s_pmic, BSP_PMIC_REG_ADC_CONTROL,
                          &diagnostic->reg30_enabled, 1);
        diagnostic->enable_verified = first_error == ESP_OK &&
            (diagnostic->reg30_enabled & BSP_PMIC_ADC_DIAGNOSTIC_MASK) ==
            BSP_PMIC_ADC_DIAGNOSTIC_MASK;
        if (first_error == ESP_OK && !diagnostic->enable_verified) {
            first_error = ESP_FAIL;
        }
    }

    const int64_t start_us = esp_timer_get_time();
    for (size_t index = 0;
            first_error == ESP_OK &&
            index < BSP_PMIC_ADC_DIAGNOSTIC_SAMPLE_COUNT;
            ++index) {
        const int64_t target_us = start_us +
            (int64_t)s_adc_diagnostic_times_ms[index] * 1000;
        for (;;) {
            const int64_t remaining_us = target_us - esp_timer_get_time();
            if (remaining_us <= 0) {
                break;
            }
            TickType_t delay_ticks = pdMS_TO_TICKS(
                (uint32_t)((remaining_us + 999) / 1000));
            if (delay_ticks == 0) {
                delay_ticks = 1;
            }
            vTaskDelay(delay_ticks);
        }

        bsp_pmic_adc_diagnostic_sample_t *sample =
            &diagnostic->samples[index];
        sample->elapsed_ms = (uint32_t)(
            (esp_timer_get_time() - start_us) / 1000);
        first_error = read_adc_diagnostic_sample(sample);
        if (first_error == ESP_OK) {
            diagnostic->sample_count = index + 1;
        }
    }

    /* REG30 belongs to the caller. Restore it even after a failed enable or
     * sample: an I2C write can have reached the PMIC despite a timeout. */
    if (control_write_attempted) {
        const esp_err_t restore_write_error = tg28_sw_write_register(
            s_pmic, BSP_PMIC_REG_ADC_CONTROL,
            diagnostic->reg30_original);
        esp_err_t restore_read_error = restore_write_error;
        if (restore_write_error == ESP_OK) {
            restore_read_error = tg28_sw_read_registers(
                s_pmic, BSP_PMIC_REG_ADC_CONTROL,
                &diagnostic->reg30_restored, 1);
        }
        diagnostic->restore_verified = restore_read_error == ESP_OK &&
            diagnostic->reg30_restored == diagnostic->reg30_original;
        if (first_error == ESP_OK && !diagnostic->restore_verified) {
            first_error = restore_read_error == ESP_OK ?
                          ESP_FAIL : restore_read_error;
        } else if (!diagnostic->restore_verified) {
            ESP_LOGE(TAG, "REG30 restore failed after ADC diagnostic: %s",
                     esp_err_to_name(restore_read_error));
        }
    }

    return first_error;
}
