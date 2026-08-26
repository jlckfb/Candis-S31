/*
 * Candis-S31 player demo - board bootstrap.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board_init.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "player_board";

#define PLAYER_NVS_NAMESPACE "player"
#define PLAYER_NVS_KEY_CFG   "cfg"

static player_settings_t s_settings;
static bool s_settings_loaded;

static void settings_apply_defaults(void)
{
    s_settings.brightness = 60;
    s_settings.volume = 20;
}

static void settings_load(void)
{
    settings_apply_defaults();
    nvs_handle_t handle;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    player_settings_t stored;
    size_t length = sizeof(stored);
    if (nvs_get_blob(handle, PLAYER_NVS_KEY_CFG, &stored, &length) == ESP_OK &&
            length == sizeof(stored) &&
            stored.brightness >= 0 && stored.brightness <= 100 &&
            stored.volume >= 0 && stored.volume <= 100) {
        s_settings = stored;
        s_settings_loaded = true;
    }
    nvs_close(handle);
}

player_settings_t *player_settings(void)
{
    return &s_settings;
}

void player_settings_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(PLAYER_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_blob(handle, PLAYER_NVS_KEY_CFG, &s_settings, sizeof(s_settings));
    nvs_commit(handle);
    nvs_close(handle);
    s_settings_loaded = true;
}

esp_err_t player_board_init(void)
{
    /* Safe-off switchable supplies before board init. */
    static const bsp_power_domain_t shutdown_order[] = {
        BSP_POWER_DISPLAY_VCI,
        BSP_POWER_DISPLAY_VBAT,
        BSP_POWER_SDCARD,
        BSP_POWER_AUDIO_PA,
    };
    for (size_t i = 0; i < sizeof(shutdown_order) / sizeof(shutdown_order[0]); ++i) {
        if (i == 1) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        bsp_power_domain_set(shutdown_order[i], false);
    }

    ESP_RETURN_ON_ERROR(bsp_board_init(), TAG, "board init failed");
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "pmic init failed");

    /* Clear latched PMIC/RTC interrupt flags. */
    uint8_t pmic_irq[3] = {0};
    bsp_pmic_get_and_clear_interrupts(pmic_irq);
    bsp_rtc_clear_interrupt_flags(NULL);
    bsp_shared_irq_status_t irq_status;
    bsp_shared_irq_service(&irq_status);

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(nvs_err, TAG, "nvs init failed");
    settings_load();

    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "display start failed");
        return ESP_FAIL;
    }
    bsp_display_brightness_set(s_settings.brightness);

    ESP_LOGI(TAG, "board init done (BSP rev %s)", CANDIS_S31_BSP_GIT_REV);
    return ESP_OK;
}
