/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_littlefs.h"
#include "esp_board_entry.h"
#include "esp_board_periph.h"
#include "esp_board_device.h"
#include "periph_spi.h"
#include "dev_littlefs.h"

static const char *TAG = "DEV_LITTLEFS_SUB_SPI";

#ifdef CONFIG_LITTLEFS_SDMMC_SUPPORT
static void dev_littlefs_call_host_deinit(const sdmmc_host_t *host_config)
{
    if (host_config->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host_config->deinit_p(host_config->slot);
    } else {
        host_config->deinit();
    }
}
#endif  /* CONFIG_LITTLEFS_SDMMC_SUPPORT */

int dev_littlefs_sub_spi_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg_size;
#ifndef CONFIG_LITTLEFS_SDMMC_SUPPORT
    (void)cfg;
    (void)device_handle;
    ESP_LOGE(TAG, "CONFIG_LITTLEFS_SDMMC_SUPPORT must be enabled for LittleFS SPI subtype");
    return -1;
#else
    esp_err_t ret = ESP_FAIL;
    bool spi_bus_refed = false;
    bool device_initialized = false;
    bool littlefs_registered = false;
    const dev_littlefs_config_t *config = (const dev_littlefs_config_t *)cfg;
    const dev_littlefs_spi_sub_config_t *spi_cfg = &config->sub_cfg.spi;

    dev_littlefs_handle_t *handle = calloc(1, sizeof(dev_littlefs_handle_t));
    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LittleFS SPI handle");
        return -1;
    }

    handle->card = calloc(1, sizeof(sdmmc_card_t));
    if (handle->card == NULL) {
        ESP_LOGE(TAG, "Failed to allocate SD card handle");
        goto clean_up;
    }

    periph_spi_handle_t *spi_handle = NULL;
    if (spi_cfg->spi_bus_name && spi_cfg->spi_bus_name[0]) {
        int bmgr_ret = esp_board_periph_ref_handle(spi_cfg->spi_bus_name, (void **)&spi_handle);
        if (bmgr_ret != 0) {
            ESP_LOGE(TAG, "Failed to get SPI peripheral handle: %d", bmgr_ret);
            goto clean_up;
        }
        spi_bus_refed = true;
    } else {
        ESP_LOGE(TAG, "Invalid SPI bus name");
        goto clean_up;
    }

    handle->host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    handle->host.max_freq_khz = config->frequency;
    handle->host.slot = spi_handle->spi_port;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = spi_cfg->cs_gpio_num;
    slot_config.host_id = handle->host.slot;

    ESP_LOGD(TAG, "host: flags=0x%" PRIx32 ", slot=%d, max_freq_khz=%d",
             handle->host.flags, handle->host.slot, handle->host.max_freq_khz);
    ESP_LOGI(TAG, "slot_config: host_id=%d, gpio_cs=%d", slot_config.host_id, slot_config.gpio_cs);

    ret = handle->host.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SDSPI host: %s", esp_err_to_name(ret));
        goto clean_up;
    }

    sdspi_dev_handle_t sdspi_handle = 0;
    ret = sdspi_host_init_device(&slot_config, &sdspi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SDSPI device: %s", esp_err_to_name(ret));
        goto clean_up;
    }
    device_initialized = true;
    handle->host.slot = sdspi_handle;

    ret = sdmmc_card_init(&handle->host, handle->card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        goto clean_up;
    }

    esp_vfs_littlefs_conf_t vfs_config = config->vfs_config;
    vfs_config.partition_label = NULL;
    vfs_config.partition = NULL;
    vfs_config.sdcard = handle->card;
    ret = esp_vfs_littlefs_register(&vfs_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register LittleFS on SDSPI card: %s", esp_err_to_name(ret));
        goto clean_up;
    }
    littlefs_registered = true;

    handle->mount_point = (char *)vfs_config.base_path;
    *device_handle = handle;

    if (!vfs_config.dont_mount) {
        size_t total = 0;
        size_t used = 0;
        ret = esp_littlefs_sdmmc_info(handle->card, &total, &used);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SDSPI card LittleFS size: total: %u, used: %u", (unsigned int)total, (unsigned int)used);
        }
    }
    return 0;

clean_up:
    if (littlefs_registered) {
        esp_vfs_littlefs_unregister_sdmmc(handle->card);
    }
    if (device_initialized) {
        dev_littlefs_call_host_deinit(&handle->host);
    }
    if (spi_bus_refed) {
        esp_board_periph_unref_handle(spi_cfg->spi_bus_name);
    }
    free(handle->card);
    free(handle);
    return -1;
#endif  /* CONFIG_LITTLEFS_SDMMC_SUPPORT */
}

int dev_littlefs_sub_spi_deinit(void *device_handle)
{
    if (device_handle == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }
#ifndef CONFIG_LITTLEFS_SDMMC_SUPPORT
    ESP_LOGE(TAG, "CONFIG_LITTLEFS_SDMMC_SUPPORT must be enabled for LittleFS SPI subtype");
    return -1;
#else
    dev_littlefs_handle_t *handle = (dev_littlefs_handle_t *)device_handle;
    if (handle->card) {
        esp_err_t ret = esp_vfs_littlefs_unregister_sdmmc(handle->card);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to unregister LittleFS SDSPI card: %s", esp_err_to_name(ret));
        }
    }

    dev_littlefs_call_host_deinit(&handle->host);

    dev_littlefs_config_t *cfg = NULL;
    esp_board_device_get_config_by_handle(device_handle, (void **)&cfg);
    if (cfg) {
        int ret = esp_board_periph_unref_handle(cfg->sub_cfg.spi.spi_bus_name);
        if (ret != 0) {
            ESP_LOGW(TAG, "Failed to unref SPI peripheral handle: %d", ret);
        }
    }

    free(handle->card);
    free(handle);
    return 0;
#endif  /* CONFIG_LITTLEFS_SDMMC_SUPPORT */
}

ESP_BOARD_SUBTYPE_ENTRY_IMPLEMENT(littlefs, spi, dev_littlefs_sub_spi_init, dev_littlefs_sub_spi_deinit);
