/*
 * Candis-S31 watch demo - board bootstrap and persistent settings.
 *
 * Boot order follows firmware/factory/main/factory_main.c: safe-off the
 * direct power domains (VCI before VBAT) before bsp_board_init(), then
 * bring up PMIC, clear latched PMIC/RTC interrupt flags (a stale flag on
 * the shared GPIO2 line would instantly re-trigger a deep-sleep wake
 * source), NVS, settings, and finally the display.
 *
 * The demo keeps the BSP 500 mA input-current default (2026-08-18 user
 * decision: this is the shipping setting, not a workaround). The factory
 * 2000 mA lab override is deliberately not inherited.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "demo_board.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define DEMO_NVS_NAMESPACE "demo"
#define DEMO_NVS_KEY_CFG   "cfg"

static const char *TAG = "demo_board";

static demo_settings_t s_settings;
static bool s_settings_loaded;

static void settings_apply_defaults(void)
{
    s_settings.brightness = 60;
    s_settings.volume = 20;       /* factory-validated comfortable level */
    s_settings.mic_gain_db = 24;
    s_settings.screen_timeout_s = 30;
}

static void settings_load(void)
{
    settings_apply_defaults();
    nvs_handle_t handle;
    if (nvs_open(DEMO_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    demo_settings_t stored;
    size_t length = sizeof(stored);
    if (nvs_get_blob(handle, DEMO_NVS_KEY_CFG, &stored, &length) == ESP_OK &&
            length == sizeof(stored) &&
            stored.brightness >= 10 && stored.brightness <= 100 &&
            stored.volume >= 0 && stored.volume <= 100 &&
            stored.mic_gain_db >= 0 && stored.mic_gain_db <= 36 &&
            stored.screen_timeout_s >= 0 && stored.screen_timeout_s <= 3600) {
        s_settings = stored;
        s_settings_loaded = true;
    }
    nvs_close(handle);
}

demo_settings_t *demo_settings(void)
{
    return &s_settings;
}

void demo_settings_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(DEMO_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_blob(handle, DEMO_NVS_KEY_CFG, &s_settings, sizeof(s_settings));
    nvs_commit(handle);
    nvs_close(handle);
    s_settings_loaded = true;
}

esp_err_t demo_board_init(void)
{
    /* Remove every direct GPIO-controlled supply first, in the only safe
     * display order (VCI, 2 ms, VBAT), so even a software reset restarts
     * from a known power state. */
    static const bsp_power_domain_t shutdown_order[] = {
        BSP_POWER_DISPLAY_VCI,
        BSP_POWER_DISPLAY_VBAT,
        BSP_POWER_TYPE_C_CONTROL,
        BSP_POWER_SDCARD,
        BSP_POWER_AUDIO_PA,
        BSP_POWER_USB_OTG,
    };
    for (size_t index = 0;
            index < sizeof(shutdown_order) / sizeof(shutdown_order[0]);
            ++index) {
        if (index == 1) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        bsp_power_domain_set(shutdown_order[index], false);
    }

    ESP_RETURN_ON_ERROR(bsp_board_init(), TAG, "board init failed");

    /* PMIC: LP-I2C up, input limit clamped to the 500 mA BSP default. */
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "pmic init failed");
    uint16_t input_limit = 0;
    if (bsp_pmic_get_input_current_limit(&input_limit) == ESP_OK) {
        ESP_LOGI(TAG, "input current limit: %u mA (BSP default)", input_limit);
    }

    /* Clear latched PMIC and RTC interrupt flags, then drain the shared
     * GPIO2 line. A stale flag would instantly re-fire the deep-sleep wake
     * source and loop the next sleep attempt. */
    uint8_t pmic_irq[3] = {0};
    bsp_pmic_get_and_clear_interrupts(pmic_irq);
    bsp_rtc_clear_interrupt_flags(NULL);
    bsp_shared_irq_status_t irq_status;
    bsp_shared_irq_service(&irq_status);

    /* RTC online check (non-fatal: watchface shows "--:--" without it). */
    bsp_rtc_time_t rtc_time;
    bsp_rtc_status_t rtc_status;
    if (bsp_rtc_get_time(&rtc_time, &rtc_status) != ESP_OK || !rtc_status.time_valid) {
        ESP_LOGW(TAG, "RTC time not valid; watchface falls back to placeholder");
    }

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(nvs_err, TAG, "nvs init failed");
    settings_load();
    ESP_LOGI(TAG, "settings %s: brightness=%d volume=%d mic_gain=%d timeout=%ds",
             s_settings_loaded ? "loaded" : "defaulted",
             s_settings.brightness, s_settings.volume, s_settings.mic_gain_db,
             s_settings.screen_timeout_s);

    /* Display + touch + LVGL (touch failure degrades to NULL indev). */
    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "display start failed");
        return ESP_FAIL;
    }
    bsp_display_brightness_set(s_settings.brightness);

    ESP_LOGI(TAG, "board init done (BSP rev %s)", CANDIS_S31_BSP_GIT_REV);
    return ESP_OK;
}
