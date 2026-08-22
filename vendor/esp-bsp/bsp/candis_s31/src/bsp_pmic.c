/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#include "tg28_sw.h"

#include "bsp/candis_s31.h"
#include "bsp_pmic_reference_model.h"

static const char *TAG = "candis_pmic";
static tg28_sw_handle_t s_pmic;
static uint8_t s_boot_irq_snapshot[3];
static bool s_boot_irq_valid;
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

/* Allow one conversion cycle when a channel had to be enabled first. */
#define BSP_PMIC_ADC_SETTLE_MS 50

static tg28_sw_regulator_t to_tg28_regulator(bsp_pmic_regulator_t regulator)
{
    return (tg28_sw_regulator_t)regulator;
}

static tg28_sw_power_switch_t to_tg28_switch(bsp_pmic_switch_t sw)
{
    return (tg28_sw_power_switch_t)sw;
}

esp_err_t bsp_pmic_init(void)
{
    if (s_pmic != NULL) {
        return ESP_OK;
    }
    s_fuel_gauge_valid = false;
    s_fuel_gauge_reference_model = false;
    memset(s_boot_irq_snapshot, 0, sizeof(s_boot_irq_snapshot));
    s_boot_irq_valid = false;

    i2c_master_bus_handle_t bus = bsp_lp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_FAIL, TAG, "low-power I2C init failed");

    const tg28_sw_config_t config = {
        .device_address = BSP_TG28_SW_I2C_ADDRESS,
        .scl_speed_hz = TG28_SW_I2C_CLOCK_HZ,
    };
    esp_err_t error = tg28_sw_create(bus, &config, &s_pmic);
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
        /* Default fuel-gauge model, best-effort: the vendor generic
         * 4.2 V-class reference table (see bsp_pmic_reference_model.c for
         * provenance and license review status). Programmed after the
         * charge baseline and the input-limit clamp so the gauge has a
         * model before any charge stepping begins. The driver-level
         * create-time battery_model hook is deliberately unused: it fails
         * the whole create on a download error (tg28_sw.c:605-610), while
         * here a failure must only leave the fuel gauge invalid -
         * charging and the PMIC stay alive. A caller-supplied/custom
         * model can still be programmed via bsp_pmic_program_battery_model(),
         * which re-verifies and replaces this reference. SOC accuracy
         * with this generic model is reference-grade; a different battery
         * SKU/chemistry needs a new model, not per-unit calibration. */
        const esp_err_t model_err = tg28_sw_program_battery_model(
                                        s_pmic, bsp_pmic_reference_battery_model,
                                        BSP_PMIC_REFERENCE_BATTERY_MODEL_SIZE);
        if (model_err == ESP_OK) {
            s_fuel_gauge_valid = true;
            s_fuel_gauge_reference_model = true;
            ESP_LOGI(TAG,
                     "reference battery model verified (%d bytes, gauge on)",
                     BSP_PMIC_REFERENCE_BATTERY_MODEL_SIZE);
        } else {
            ESP_LOGW(TAG, "reference battery model download failed: %s "
                     "(fuel gauge stays invalid; charging continues)",
                     esp_err_to_name(model_err));
        }
    }
    if (error == ESP_OK) {
        /* Clear any latched interrupt status before enabling the power-key
         * IRQs, like the vendor axp-core driver does at irq-chip init
         * (write 1 to clear every pending bit), so stale events from the
         * boot ROM or a previous reset do not fire immediately.
         * Keep a snapshot for post-mortem diagnosis: events latched before
         * this clear (e.g. an over-current lockout that killed the SoC while
         * the TG28 stayed alive) are otherwise lost forever. */
        uint8_t pending[3] = {0};
        error = tg28_sw_get_and_clear_interrupts(s_pmic, pending);
        if (error == ESP_OK) {
            memcpy(s_boot_irq_snapshot, pending, sizeof(s_boot_irq_snapshot));
            s_boot_irq_valid = true;
        }
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
        /* A failed init/retry must never inherit model validity or stale
         * boot-IRQ evidence from the attempt that just died. */
        s_fuel_gauge_valid = false;
        s_fuel_gauge_reference_model = false;
        memset(s_boot_irq_snapshot, 0, sizeof(s_boot_irq_snapshot));
        s_boot_irq_valid = false;
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
    /* Only meaningful on a boot where bsp_pmic_init() ran; the snapshot was
     * captured before the init-time clear, so it preserves events latched
     * across an SoC power collapse. */
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "TG28_SW is unavailable");
    memcpy(status, s_boot_irq_snapshot, sizeof(s_boot_irq_snapshot));
    *valid = s_boot_irq_valid;
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
        vTaskDelay(pdMS_TO_TICKS(BSP_PMIC_ADC_SETTLE_MS));
    }
    const esp_err_t error = tg28_sw_read_adc_channel(s_pmic, adc, millivolts);
    if (!was_enabled) {
        tg28_sw_set_adc_channel_enable(s_pmic, adc, false);
    }
    return error;
}
