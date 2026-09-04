/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include "esp_log.h"
#include "esp_board_entry.h"
#include "dev_button.h"
#include "esp_board_extra_func_entry.h"

static const char *TAG = "DEV_BUTTON_SUB_CUSTOM";

int dev_button_sub_custom_init(void *cfg, int cfg_size, void **device_handle)
{
    const dev_button_config_t *config = (const dev_button_config_t *)cfg;
    button_handle_t btn_handle = NULL;

    // validate config size
    if (cfg_size != sizeof(dev_button_config_t)) {
        ESP_LOGE(TAG, "Invalid config size");
        return -1;
    }

    // Lookup the custom driver creator function
    void *extra_func = NULL;
    if (esp_board_extra_func_get(config->name, &extra_func) != 0) {
        ESP_LOGE(TAG, "Custom driver creator '%s' not found", config->name);
        return -1;
    }

    // Call the creator function to get the button driver
    dev_button_custom_driver_create_t create_func = (dev_button_custom_driver_create_t)extra_func;
    button_driver_t *driver = NULL;
    driver = create_func(config);
    if (!driver) {
        ESP_LOGE(TAG, "Failed to create custom button driver");
        return -1;
    }

    // Create custom button
    esp_err_t ret = iot_button_create(&config->button_timing_cfg, driver, &btn_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create custom button: %s", esp_err_to_name(ret));
        if (driver->del) {
            driver->del(driver);  // cleanup if creation fails
        }
        return -1;
    }

    // Allocate generic button handles structure
    dev_button_handles_t *button_handles = calloc(1, sizeof(dev_button_handles_t));
    if (button_handles == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for dev_button_handles");
        iot_button_delete(btn_handle);
        return -1;
    }
    button_handles->num_buttons = 1;
    button_handles->button_handles[0] = btn_handle;

    *device_handle = button_handles;
    return 0;
}

int dev_button_sub_custom_deinit(void *device_handle)
{
    if (device_handle == NULL) {
        return -1;
    }

    dev_button_handles_t *handle = (dev_button_handles_t *)device_handle;
    if (handle->button_handles[0]) {
        iot_button_delete(handle->button_handles[0]);
    }

    free(handle);
    ESP_LOGI(TAG, "Successfully deinitialized custom button");
    return 0;
}

ESP_BOARD_SUBTYPE_ENTRY_IMPLEMENT(button, custom, dev_button_sub_custom_init, dev_button_sub_custom_deinit);
