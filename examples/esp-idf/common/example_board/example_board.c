/*
 * Candis-S31 standalone examples - shared board bootstrap.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "example_board.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "example_board";

esp_err_t example_board_init(const example_board_cfg_t *cfg)
{
    static const example_board_cfg_t defaults = {
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    };
    if (cfg == NULL) {
        cfg = &defaults;
    }

    /* Safe-off switchable supplies before board init. */
    static const bsp_power_domain_t shutdown_order[] = {
        BSP_POWER_DISPLAY_VCI,
        BSP_POWER_DISPLAY_VBAT,
        BSP_POWER_TYPE_C_CONTROL,
        BSP_POWER_SDCARD,
        BSP_POWER_AUDIO_PA,
        BSP_POWER_USB_OTG,
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

    if (cfg->require_psram) {
#if CONFIG_SPIRAM
        ESP_RETURN_ON_FALSE(esp_psram_is_initialized(), ESP_ERR_NOT_FOUND, TAG,
                            "32 MB PSRAM is required");
        const size_t psram_size = esp_psram_get_size();
        const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(psram_size >= 32U * 1024U * 1024U, ESP_ERR_INVALID_SIZE, TAG,
                            "unexpected PSRAM size: %u", (unsigned)psram_size);
        ESP_RETURN_ON_FALSE(largest_block >= 2U * 1024U * 1024U, ESP_ERR_NO_MEM, TAG,
                            "PSRAM is too fragmented: largest=%u", (unsigned)largest_block);
        ESP_LOGI(TAG, "PSRAM ready: %u MB, largest block %u KB",
                 (unsigned)(psram_size / (1024U * 1024U)),
                 (unsigned)(largest_block / 1024U));
#else
        ESP_LOGE(TAG, "PSRAM required but CONFIG_SPIRAM is disabled");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

    if (cfg->start_display) {
        if (bsp_display_start() == NULL) {
            ESP_LOGE(TAG, "display start failed");
            return ESP_FAIL;
        }
        bsp_display_brightness_set(cfg->brightness_percent);
    }

    ESP_LOGI(TAG, "board init done (BSP rev %s)", CANDIS_S31_BSP_GIT_REV);
    return ESP_OK;
}
