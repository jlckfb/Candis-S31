/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_board_periph.h"
#include "esp_board_entry.h"
#include "esp_board_extra_func_entry.h"
#include "esp_board_device.h"
#include "dev_power_ctrl.h"

static const char *TAG = "DEV_POWER_CTRL_SUB_CUSTOM";

static void dev_power_ctrl_sub_custom_unref_peripherals(void *device_handle)
{
    dev_power_ctrl_config_t *cfg = NULL;
    if (esp_board_device_get_config_by_handle(device_handle, (void **)&cfg) != 0 || cfg == NULL) {
        return;
    }

    const dev_power_ctrl_custom_sub_config_t *custom_cfg = &cfg->sub_cfg.custom;
    if (custom_cfg->periph_names == NULL) {
        return;
    }

    for (uint8_t i = 0; i < custom_cfg->periph_count; i++) {
        if (custom_cfg->periph_names[i] == NULL) {
            continue;
        }
        int ret = esp_board_periph_unref_handle(custom_cfg->periph_names[i]);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to unref peripheral %s: %d", custom_cfg->periph_names[i], ret);
        }
    }
}

static int dev_power_ctrl_sub_custom_power_control(void *dev_handle, const char *device_name, bool power_on)
{
    dev_power_ctrl_handle_t *handle = (dev_power_ctrl_handle_t *)dev_handle;
    if (handle == NULL || handle->custom_ops == NULL || handle->custom_ops->set_power == NULL) {
        ESP_LOGE(TAG, "Custom power controller is not initialized");
        return -1;
    }

    return handle->custom_ops->set_power(handle->custom_context, device_name, power_on);
}

int dev_power_ctrl_sub_custom_init(void *cfg, int cfg_size, void **device_handle)
{
    if (cfg == NULL || device_handle == NULL || cfg_size != sizeof(dev_power_ctrl_config_t)) {
        ESP_LOGE(TAG, "Invalid config size");
        return -1;
    }
    const dev_power_ctrl_config_t *config = (const dev_power_ctrl_config_t *)cfg;
    const dev_power_ctrl_custom_sub_config_t *custom_cfg = &config->sub_cfg.custom;
    if (custom_cfg->periph_count > 0 && custom_cfg->periph_names == NULL) {
        ESP_LOGE(TAG, "Custom power control peripheral names are NULL");
        return -1;
    }

    void *extra_func = NULL;
    if (esp_board_extra_func_get(config->name, &extra_func) != 0 || extra_func == NULL) {
        ESP_LOGE(TAG, "Custom power controller '%s' is not registered", config->name);
        return -1;
    }
    const dev_power_ctrl_custom_ops_t *ops = (const dev_power_ctrl_custom_ops_t *)extra_func;
    if (ops->set_power == NULL) {
        ESP_LOGE(TAG, "Custom power controller '%s' has no set_power operation", config->name);
        return -1;
    }

    dev_power_ctrl_handle_t *handle = calloc(1, sizeof(dev_power_ctrl_handle_t));
    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for custom power control");
        return -1;
    }
    handle->custom_ops = ops;

    for (uint8_t i = 0; i < custom_cfg->periph_count; i++) {
        void *periph_handle = NULL;
        int ret = esp_board_periph_ref_handle(custom_cfg->periph_names[i], &periph_handle);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to ref peripheral %s: %d", custom_cfg->periph_names[i], ret);
            for (uint8_t j = 0; j < i; j++) {
                esp_board_periph_unref_handle(custom_cfg->periph_names[j]);
            }
            free(handle);
            return -1;
        }
    }

    if (ops->init != NULL) {
        int ret = ops->init(config, &handle->custom_context);
        if (ret != 0) {
            ESP_LOGE(TAG, "Custom power controller '%s' init failed: %d", config->name, ret);
            for (uint8_t i = 0; i < custom_cfg->periph_count; i++) {
                esp_board_periph_unref_handle(custom_cfg->periph_names[i]);
            }
            free(handle);
            return ret;
        }
    }

    *device_handle = handle;
    ESP_LOGI(TAG, "Custom power control initialized successfully");
    return 0;
}

int dev_power_ctrl_sub_custom_deinit(void *device_handle)
{
    dev_power_ctrl_handle_t *handle = (dev_power_ctrl_handle_t *)device_handle;

    if (handle == NULL) {
        return -1;
    }

    int ret = 0;
    if (handle->custom_ops != NULL && handle->custom_ops->deinit != NULL) {
        ret = handle->custom_ops->deinit(handle->custom_context);
        if (ret != 0) {
            ESP_LOGE(TAG, "Custom power controller deinit failed: %d", ret);
            return ret;
        }
    }
    dev_power_ctrl_sub_custom_unref_peripherals(device_handle);
    free(handle);
    return ret;
}

ESP_BOARD_SUBTYPE_ENTRY_IMPLEMENT(power_ctrl, custom, dev_power_ctrl_sub_custom_init, dev_power_ctrl_sub_custom_deinit);
DEVICE_EXTRA_FUNC_REGISTER(custom_power_ctrl, dev_power_ctrl_sub_custom_power_control);
