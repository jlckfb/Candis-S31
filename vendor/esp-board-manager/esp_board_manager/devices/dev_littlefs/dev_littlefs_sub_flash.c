/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_board_entry.h"
#include "esp_board_device.h"
#include "dev_littlefs.h"

static const char *TAG = "DEV_LITTLEFS_SUB_FLASH";

int dev_littlefs_sub_flash_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg_size;
    const dev_littlefs_config_t *config = (const dev_littlefs_config_t *)cfg;

    dev_littlefs_handle_t *handle = calloc(1, sizeof(dev_littlefs_handle_t));
    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LittleFS flash handle");
        return -1;
    }

    esp_vfs_littlefs_conf_t vfs_config = config->vfs_config;
    vfs_config.partition = NULL;

    esp_err_t ret = esp_vfs_littlefs_register(&vfs_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register LittleFS flash partition: %s", esp_err_to_name(ret));
        free(handle);
        return -1;
    }

    handle->mount_point = (char *)vfs_config.base_path;
    *device_handle = handle;

    if (!vfs_config.dont_mount && vfs_config.partition_label) {
        size_t total = 0;
        size_t used = 0;
        ret = esp_littlefs_info(vfs_config.partition_label, &total, &used);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Partition size: total: %u, used: %u", (unsigned int)total, (unsigned int)used);
        }
    }
    return 0;
}

int dev_littlefs_sub_flash_deinit(void *device_handle)
{
    if (device_handle == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }

    dev_littlefs_config_t *cfg = NULL;
    esp_board_device_get_config_by_handle(device_handle, (void **)&cfg);
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Failed to find LittleFS flash config by handle");
        return -1;
    }

    esp_err_t ret = esp_vfs_littlefs_unregister(cfg->vfs_config.partition_label);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unregister LittleFS flash partition: %s", esp_err_to_name(ret));
        return -1;
    }

    free(device_handle);
    return 0;
}

ESP_BOARD_SUBTYPE_ENTRY_IMPLEMENT(littlefs, flash, dev_littlefs_sub_flash_init, dev_littlefs_sub_flash_deinit);
