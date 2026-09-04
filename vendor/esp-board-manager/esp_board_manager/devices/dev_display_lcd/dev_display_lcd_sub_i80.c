/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_board_entry.h"
#include "dev_display_lcd.h"

static const char *TAG = "DEV_DISPLAY_LCD_SUB_I80";

extern esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel);
int dev_display_lcd_sub_i80_deinit_with_config(void *device_handle, const dev_display_lcd_config_t *cfg);

typedef struct {
    dev_display_lcd_handles_t  base;
    esp_lcd_i80_bus_handle_t   bus_handle;
} dev_display_lcd_i80_handles_t;

int dev_display_lcd_sub_i80_init(void *cfg, int cfg_size, void **device_handle)
{
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)cfg;
    dev_display_lcd_i80_handles_t *i80_handles = calloc(1, sizeof(dev_display_lcd_i80_handles_t));
    if (i80_handles == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for dev_display_lcd_sub_i80");
        return -1;
    }
    dev_display_lcd_handles_t *lcd_handles = &i80_handles->base;

    ESP_LOGI(TAG, "Initializing I80 LCD display: %s, chip: %s", lcd_cfg->name, lcd_cfg->chip);

    esp_err_t ret = esp_lcd_new_i80_bus(&lcd_cfg->sub_cfg.i80.bus_config, &i80_handles->bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I80 bus: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = esp_lcd_new_panel_io_i80(i80_handles->bus_handle, &lcd_cfg->sub_cfg.i80.io_config, &lcd_handles->io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LCD panel IO: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = lcd_panel_factory_entry_t(lcd_handles->io_handle, &lcd_cfg->sub_cfg.i80.panel_config, &lcd_handles->panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LCD panel: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    *device_handle = lcd_handles;
    return 0;

cleanup:
    dev_display_lcd_sub_i80_deinit_with_config(i80_handles, lcd_cfg);
    return -1;
}

int dev_display_lcd_sub_i80_deinit(void *device_handle)
{
    return dev_display_lcd_sub_i80_deinit_with_config(device_handle, NULL);
}

int dev_display_lcd_sub_i80_deinit_with_config(void *device_handle, const dev_display_lcd_config_t *cfg)
{
    (void)cfg;

    if (device_handle == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }

    dev_display_lcd_i80_handles_t *i80_handles = (dev_display_lcd_i80_handles_t *)device_handle;
    dev_display_lcd_handles_t *lcd_handles = &i80_handles->base;

    if (lcd_handles->panel_handle) {
        esp_lcd_panel_del(lcd_handles->panel_handle);
        lcd_handles->panel_handle = NULL;
    }

    if (lcd_handles->io_handle) {
        esp_lcd_panel_io_del(lcd_handles->io_handle);
        lcd_handles->io_handle = NULL;
    }

    if (i80_handles->bus_handle) {
        esp_lcd_del_i80_bus(i80_handles->bus_handle);
        i80_handles->bus_handle = NULL;
    }

    free(device_handle);
    return 0;
}

ESP_BOARD_SUBTYPE_ENTRY_IMPLEMENT(display_lcd, i80, dev_display_lcd_sub_i80_init, dev_display_lcd_sub_i80_deinit);
