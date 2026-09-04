/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdio.h>
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "esp_board_entry.h"
#include "esp_board_device.h"
#include "dev_littlefs.h"

static const char *TAG = "DEV_LITTLEFS";

int dev_littlefs_init(void *cfg, int cfg_size, void **device_handle)
{
    if (cfg == NULL || device_handle == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }
    if (cfg_size != sizeof(dev_littlefs_config_t)) {
        ESP_LOGE(TAG, "Invalid config size");
        return -1;
    }

    const dev_littlefs_config_t *config = (const dev_littlefs_config_t *)cfg;
    if (config->sub_type == NULL || config->vfs_config.base_path == NULL) {
        ESP_LOGE(TAG, "Invalid LittleFS configuration");
        return -1;
    }

    const esp_board_entry_desc_t *entry_desc = esp_board_entry_find_subtype_desc(ESP_BOARD_DEVICE_LITTLEFS_TYPE,
                                                                                 config->sub_type);
    if (entry_desc == NULL || entry_desc->init_func == NULL) {
        ESP_LOGE(TAG, "Failed to find LittleFS sub device: %s", config->sub_type);
        return -1;
    }

    dev_littlefs_handle_t *handle = NULL;
    int ret = entry_desc->init_func((void *)config, cfg_size, (void **)&handle);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to initialize LittleFS sub device: %s", config->sub_type);
        return -1;
    }

    if (handle->card) {
        sdmmc_card_print_info(stdout, handle->card);
    }
    ESP_LOGI(TAG, "LittleFS mounted, base path: %s", handle->mount_point);
    *device_handle = handle;
    return 0;
}

int dev_littlefs_deinit(void *device_handle)
{
    if (device_handle == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }

    dev_littlefs_config_t *cfg = NULL;
    esp_board_device_get_config_by_handle(device_handle, (void **)&cfg);
    if (cfg == NULL || cfg->sub_type == NULL) {
        ESP_LOGE(TAG, "Failed to find LittleFS config by handle");
        return -1;
    }

    const esp_board_entry_desc_t *desc = esp_board_entry_find_subtype_desc(ESP_BOARD_DEVICE_LITTLEFS_TYPE,
                                                                           cfg->sub_type);
    if (desc == NULL || desc->deinit_func == NULL) {
        ESP_LOGE(TAG, "No deinit function found for LittleFS sub type: %s", cfg->sub_type);
        return -1;
    }

    int ret = desc->deinit_func(device_handle);
    if (ret != 0) {
        ESP_LOGE(TAG, "LittleFS sub type %s deinit failed: %d", cfg->sub_type, ret);
        return -1;
    }

    ESP_LOGI(TAG, "LittleFS device deinitialized successfully, sub_type: %s", cfg->sub_type);
    return 0;
}
