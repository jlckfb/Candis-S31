/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdint.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

static const char *TAG = "candis_s31";

void app_main(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "Candis-S31 getting started");
    ESP_LOGI(TAG, "target=%s cores=%u silicon_revision=%u.%u",
             CONFIG_IDF_TARGET,
             (unsigned)chip_info.cores,
             (unsigned)(chip_info.revision / 100),
             (unsigned)(chip_info.revision % 100));

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "flash=%" PRIu32 " MiB", flash_size / (1024U * 1024U));
    } else {
        ESP_LOGW(TAG, "unable to read the flash size");
    }

    ESP_LOGI(TAG, "free_heap=%" PRIu32 " bytes", esp_get_free_heap_size());

    /*
     * Keep every board GPIO untouched until the EVT1 power sequence and
     * peripheral control signals have been verified on real hardware.
     */
    ESP_LOGI(TAG, "board peripherals are not initialized by this example");
}
