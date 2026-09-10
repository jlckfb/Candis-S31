/*
 * Candis-S31 PMIC example.
 *
 * Read-only TG28_SW dump (status, rails, OTP switches, SAR ADC channels)
 * plus a single off->on->restore toggle of the BSP_POWER_AUDIO_PA domain.
 * Safety contract: this example NEVER writes a charge
 * register (no set_charge_current / set_input_current_limit /
 * set_charge_voltage) and never calls bsp_pmic_power_off().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp-bsp.h"
#include "example_board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pmic";

static const char *const s_adc_names[BSP_PMIC_ADC_COUNT] = {
    [BSP_PMIC_ADC_VBAT] = "VBAT",
    [BSP_PMIC_ADC_TS]   = "TS",
    [BSP_PMIC_ADC_VBUS] = "VBUS",
    [BSP_PMIC_ADC_VSYS] = "VSYS",
    [BSP_PMIC_ADC_TDIE] = "TDIE",
};

/* DLDO1/DLDO2 are OTP-strapped load switches on this board, not adjustable
 * LDOs (factory_power.c rail_uses_otp_switch). */
static bool rail_uses_otp_switch(bsp_pmic_regulator_t regulator,
                                 bsp_pmic_switch_t *sw)
{
    if (regulator == BSP_PMIC_DLDO1) {
        *sw = BSP_PMIC_SWITCH_DC1SW;
        return true;
    }
    if (regulator == BSP_PMIC_DLDO2) {
        *sw = BSP_PMIC_SWITCH_DC4SW;
        return true;
    }
    return false;
}

static void dump_status(void)
{
    bsp_pmic_status_t status;
    uint8_t power_on_source = 0;
    uint16_t charge_current = 0;
    uint16_t input_limit = 0;
    uint16_t charge_voltage = 0;
    ESP_ERROR_CHECK(bsp_pmic_get_status(&status));
    ESP_ERROR_CHECK(bsp_pmic_get_power_on_source(&power_on_source));
    ESP_ERROR_CHECK(bsp_pmic_get_charge_current(&charge_current));
    ESP_ERROR_CHECK(bsp_pmic_get_input_current_limit(&input_limit));
    ESP_ERROR_CHECK(bsp_pmic_get_charge_voltage(&charge_voltage));

    ESP_LOGI(TAG, "status: id=0x%02x vbat=%umV soc=%u%%(%s) bat=%s vbus=%s charging=%s done=%s",
             status.chip_id, status.battery_mv, status.battery_percent,
             status.fuel_gauge_valid ? (status.fuel_gauge_reference_model ? "ref" : "custom") : "invalid",
             status.battery_present ? "yes" : "no",
             status.vbus_present ? "yes" : "no",
             status.charging ? "yes" : "no",
             status.charge_done ? "yes" : "no");
    ESP_LOGI(TAG, "charger: src=0x%02x current=%umA input_limit=%umA voltage=%umV",
             power_on_source, charge_current, input_limit, charge_voltage);
}

static void dump_rails(void)
{
    int enabled_count = 0;
    int total = 0;
    for (int index = 0; index < BSP_PMIC_REGULATOR_COUNT; ++index) {
        const bsp_pmic_regulator_t regulator = (bsp_pmic_regulator_t)index;
        bsp_pmic_switch_t unused_switch = BSP_PMIC_SWITCH_COUNT;
        if (rail_uses_otp_switch(regulator, &unused_switch)) {
            continue;
        }
        ++total;
        bool enabled = false;
        uint16_t millivolts = 0;
        if (bsp_pmic_regulator_is_enabled(regulator, &enabled) != ESP_OK ||
            bsp_pmic_regulator_get_voltage(regulator, &millivolts) != ESP_OK) {
            ESP_LOGW(TAG, "rail %s: read failed", bsp_pmic_regulator_name(regulator));
            continue;
        }
        if (enabled) {
            ++enabled_count;
        }
        ESP_LOGI(TAG, "rail %s: %s %umV", bsp_pmic_regulator_name(regulator),
                 enabled ? "on " : "off", millivolts);
    }
    ESP_LOGI(TAG, "rails: %d/%d enabled", enabled_count, total);

    static const bsp_pmic_switch_t switches[] = {
        BSP_PMIC_SWITCH_DC1SW, BSP_PMIC_SWITCH_DC4SW,
    };
    for (size_t index = 0; index < sizeof(switches) / sizeof(switches[0]); ++index) {
        bool enabled = false;
        if (bsp_pmic_switch_is_enabled(switches[index], &enabled) == ESP_OK) {
            ESP_LOGI(TAG, "switch %s: %s",
                     switches[index] == BSP_PMIC_SWITCH_DC1SW ? "DC1SW" : "DC4SW",
                     enabled ? "on" : "off");
        }
    }
}

static void dump_adc(void)
{
    for (int channel = 0; channel < BSP_PMIC_ADC_COUNT; ++channel) {
        uint16_t millivolts = 0;
        const esp_err_t err =
            bsp_pmic_read_adc_mv((bsp_pmic_adc_channel_t)channel, &millivolts);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "adc %s: %umV", s_adc_names[channel], millivolts);
        } else {
            ESP_LOGW(TAG, "adc %s: read failed (%s)", s_adc_names[channel],
                     esp_err_to_name(err));
        }
    }
}

static void audio_pa_toggle_once(void)
{
    bool original = false;
    ESP_ERROR_CHECK(bsp_power_domain_get(BSP_POWER_AUDIO_PA, &original));
    ESP_LOGI(TAG, "power domain %s: original=%s",
             bsp_power_domain_name(BSP_POWER_AUDIO_PA), original ? "on" : "off");

    ESP_ERROR_CHECK(bsp_power_domain_set(BSP_POWER_AUDIO_PA, false));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "power domain %s: off", bsp_power_domain_name(BSP_POWER_AUDIO_PA));

    ESP_ERROR_CHECK(bsp_power_domain_set(BSP_POWER_AUDIO_PA, true));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "power domain %s: on", bsp_power_domain_name(BSP_POWER_AUDIO_PA));

    ESP_ERROR_CHECK(bsp_power_domain_set(BSP_POWER_AUDIO_PA, original));
    ESP_LOGI(TAG, "power domain %s: restored=%s",
             bsp_power_domain_name(BSP_POWER_AUDIO_PA), original ? "on" : "off");
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    }));

    dump_status();
    dump_rails();
    dump_adc();
    audio_pa_toggle_once();
}
