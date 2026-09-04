/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_littlefs.h"
#include "esp_board_entry.h"
#include "dev_littlefs.h"
#ifdef SOC_GP_LDO_SUPPORTED
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif  /* SOC_GP_LDO_SUPPORTED */

static const char *TAG = "DEV_LITTLEFS_SUB_SDMMC";

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

int dev_littlefs_sub_sdmmc_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg_size;
#ifndef CONFIG_LITTLEFS_SDMMC_SUPPORT
    (void)cfg;
    (void)device_handle;
    ESP_LOGE(TAG, "CONFIG_LITTLEFS_SDMMC_SUPPORT must be enabled for LittleFS SDMMC subtype");
    return -1;
#else
    esp_err_t ret = ESP_FAIL;
    bool host_initialized = false;
    bool littlefs_registered = false;
    const dev_littlefs_config_t *config = (const dev_littlefs_config_t *)cfg;
    const dev_littlefs_sdmmc_sub_config_t *sdmmc_cfg = &config->sub_cfg.sdmmc;

    dev_littlefs_handle_t *handle = calloc(1, sizeof(dev_littlefs_handle_t));
    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LittleFS SDMMC handle");
        return -1;
    }

    handle->card = calloc(1, sizeof(sdmmc_card_t));
    if (handle->card == NULL) {
        ESP_LOGE(TAG, "Failed to allocate SDMMC card handle");
        goto clean_up;
    }

    handle->host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    handle->host.max_freq_khz = config->frequency;
    handle->host.slot = sdmmc_cfg->slot;

#ifdef SOC_GP_LDO_SUPPORTED
    if (sdmmc_cfg->ldo_chan_id != -1) {
        sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
        sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = sdmmc_cfg->ldo_chan_id,
        };
        ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create on-chip LDO power control driver: %s", esp_err_to_name(ret));
            goto clean_up;
        }
        handle->host.pwr_ctrl_handle = pwr_ctrl_handle;
    }
#endif  /* SOC_GP_LDO_SUPPORTED */

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.cd = sdmmc_cfg->pins.cd;
    slot_config.wp = sdmmc_cfg->pins.wp;
    slot_config.clk = sdmmc_cfg->pins.clk;
    slot_config.cmd = sdmmc_cfg->pins.cmd;
    slot_config.d0 = sdmmc_cfg->pins.d0;
    slot_config.d1 = sdmmc_cfg->pins.d1;
    slot_config.d2 = sdmmc_cfg->pins.d2;
    slot_config.d3 = sdmmc_cfg->pins.d3;
    slot_config.d4 = sdmmc_cfg->pins.d4;
    slot_config.d5 = sdmmc_cfg->pins.d5;
    slot_config.d6 = sdmmc_cfg->pins.d6;
    slot_config.d7 = sdmmc_cfg->pins.d7;
    slot_config.width = sdmmc_cfg->bus_width;
    slot_config.flags = sdmmc_cfg->slot_flags;

    ESP_LOGD(TAG, "host: flags=0x%" PRIx32 ", slot=%d, max_freq_khz=%d",
             handle->host.flags, handle->host.slot, handle->host.max_freq_khz);
    ESP_LOGI(TAG, "slot_config: cd=%d, wp=%d, clk=%d, cmd=%d, d0=%d, d1=%d, d2=%d, d3=%d, width=%d, flags=0x%" PRIx32,
             slot_config.cd, slot_config.wp, slot_config.clk, slot_config.cmd, slot_config.d0, slot_config.d1,
             slot_config.d2, slot_config.d3, slot_config.width, slot_config.flags);

    ret = handle->host.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SDMMC host: %s", esp_err_to_name(ret));
        goto clean_up;
    }
    host_initialized = true;

    ret = sdmmc_host_init_slot(handle->host.slot, &slot_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SDMMC slot: %s", esp_err_to_name(ret));
        goto clean_up;
    }

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
        ESP_LOGE(TAG, "Failed to register LittleFS on SDMMC card: %s", esp_err_to_name(ret));
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
            ESP_LOGI(TAG, "SD card LittleFS size: total: %u, used: %u", (unsigned int)total, (unsigned int)used);
        }
    }
    return 0;

clean_up:
    if (littlefs_registered) {
        esp_vfs_littlefs_unregister_sdmmc(handle->card);
    }
    if (host_initialized) {
        dev_littlefs_call_host_deinit(&handle->host);
    }
#ifdef SOC_GP_LDO_SUPPORTED
    if (handle->host.pwr_ctrl_handle) {
        sd_pwr_ctrl_del_on_chip_ldo(handle->host.pwr_ctrl_handle);
        handle->host.pwr_ctrl_handle = NULL;
    }
#endif  /* SOC_GP_LDO_SUPPORTED */
    free(handle->card);
    free(handle);
    return -1;
#endif  /* CONFIG_LITTLEFS_SDMMC_SUPPORT */
}

int dev_littlefs_sub_sdmmc_deinit(void *device_handle)
{
    if (device_handle == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }
#ifndef CONFIG_LITTLEFS_SDMMC_SUPPORT
    ESP_LOGE(TAG, "CONFIG_LITTLEFS_SDMMC_SUPPORT must be enabled for LittleFS SDMMC subtype");
    return -1;
#else
    dev_littlefs_handle_t *handle = (dev_littlefs_handle_t *)device_handle;
    if (handle->card) {
        esp_err_t ret = esp_vfs_littlefs_unregister_sdmmc(handle->card);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to unregister LittleFS SDMMC card: %s", esp_err_to_name(ret));
        }
    }

    dev_littlefs_call_host_deinit(&handle->host);
#ifdef SOC_GP_LDO_SUPPORTED
    if (handle->host.pwr_ctrl_handle) {
        esp_err_t ret = sd_pwr_ctrl_del_on_chip_ldo(handle->host.pwr_ctrl_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete on-chip LDO power control driver: %s", esp_err_to_name(ret));
        }
        handle->host.pwr_ctrl_handle = NULL;
    }
#endif  /* SOC_GP_LDO_SUPPORTED */
    free(handle->card);
    free(handle);
    return 0;
#endif  /* CONFIG_LITTLEFS_SDMMC_SUPPORT */
}

ESP_BOARD_SUBTYPE_ENTRY_IMPLEMENT(littlefs, sdmmc, dev_littlefs_sub_sdmmc_init, dev_littlefs_sub_sdmmc_deinit);
