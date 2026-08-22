/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "bsp/candis_s31.h"

static const char *TAG = "candis_storage";
static sdmmc_card_t *s_card;

bool bsp_sdcard_is_inserted(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BSP_SD_DET,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
        return false;
    }
    return gpio_get_level(BSP_SD_DET) == BSP_SD_DET_ACTIVE_LEVEL;
}

void bsp_sdcard_get_sdmmc_host(int slot, sdmmc_host_t *config)
{
    assert(config != NULL);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = slot;
    memcpy(config, &host, sizeof(host));
}

void bsp_sdcard_sdmmc_get_slot(int slot, sdmmc_slot_config_t *config)
{
    (void)slot;
    assert(config != NULL);
    memset(config, 0, sizeof(*config));
    config->clk = BSP_SD_CLK;
    config->cmd = BSP_SD_CMD;
    config->d0 = BSP_SD_D0;
    config->d1 = BSP_SD_D1;
    config->d2 = BSP_SD_D2;
    config->d3 = BSP_SD_D3;
    config->cd = BSP_SD_DET;
    config->wp = SDMMC_SLOT_NO_WP;
    config->width = 4;
}

void bsp_sdcard_sdspi_get_slot(spi_host_device_t spi_host,
                               sdspi_device_config_t *config)
{
    (void)spi_host;
    assert(config != NULL);
    memset(config, 0, sizeof(*config));
    config->gpio_cs = SDSPI_SLOT_NO_CS;
    config->gpio_cd = BSP_SD_DET;
    config->gpio_wp = SDSPI_SLOT_NO_WP;
    config->gpio_int = GPIO_NUM_NC;
    config->host_id = spi_host;
}

esp_err_t bsp_sdcard_sdmmc_mount(bsp_sdcard_cfg_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "mount configuration is NULL");
    ESP_RETURN_ON_FALSE(s_card == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "SD card is already mounted");

    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
#if defined(CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL) && CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    /* Use stack-local defaults when the caller did not supply them, but never
     * write the stack addresses back into cfg: that would leave dangling
     * pointers after this function returns. Resolve the effective pointers
     * here and pass them directly to the mount call. */
    if (cfg->host == NULL) {
        bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
    }
    if (cfg->slot.sdmmc == NULL) {
        bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    }
    const sdmmc_host_t *const use_host = cfg->host != NULL ? cfg->host : &host;
    const sdmmc_slot_config_t *const use_slot =
        cfg->slot.sdmmc != NULL ? cfg->slot.sdmmc : &slot;
    const esp_vfs_fat_sdmmc_mount_config_t *const use_mount =
        cfg->mount != NULL ? cfg->mount : &mount;

    ESP_RETURN_ON_ERROR(bsp_peripheral_power_set(BSP_PERIPHERAL_SDCARD, true),
                        TAG, "SD card power-up failed");
    esp_err_t error = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, use_host,
                      use_slot, use_mount, &s_card);
    if (error != ESP_OK) {
        s_card = NULL;
        bsp_peripheral_power_set(BSP_PERIPHERAL_SDCARD, false);
        return error;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t bsp_sdcard_sdspi_mount(bsp_sdcard_cfg_t *cfg)
{
    (void)cfg;
    ESP_LOGE(TAG, "TF card is wired for SDMMC mode, not SDSPI mode");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_mount(void)
{
    bsp_sdcard_cfg_t config = {0};
    return bsp_sdcard_sdmmc_mount(&config);
}

esp_err_t bsp_sdcard_unmount(void)
{
    if (s_card == NULL) {
        return ESP_OK;
    }

    sdmmc_card_t *card = s_card;
    /* The IDF unmount path can release the card before a later VFS
     * unregister error is returned.  Once unmount starts, the handle must no
     * longer be exposed as a mounted card. */
    s_card = NULL;
    esp_err_t unmount_error =
        esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, card);
    esp_err_t power_error =
        bsp_peripheral_power_set(BSP_PERIPHERAL_SDCARD, false);
    if (unmount_error != ESP_OK) {
        if (power_error != ESP_OK) {
            ESP_LOGE(TAG, "SD card power-down also failed (%s)",
                     esp_err_to_name(power_error));
        }
        return unmount_error;
    }
    return power_error;
}

sdmmc_card_t *bsp_sdcard_get_handle(void)
{
    return s_card;
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#if defined(CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL) && CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG,
                        "SPIFFS mount failed");

    size_t total = 0, used = 0;
    esp_err_t error = esp_spiffs_info(conf.partition_label, &total, &used);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)",
                 esp_err_to_name(error));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted at %s: total %d, used %d",
                 CONFIG_BSP_SPIFFS_MOUNT_POINT, total, used);
    }
    return error;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}
