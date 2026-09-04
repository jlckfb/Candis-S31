/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_board_device.h"
#include "esp_board_entry.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#ifdef CONFIG_ESP_BOARD_DEV_GPIO_EXPANDER_SUPPORT
#include "esp_io_expander.h"
#endif  /* CONFIG_ESP_BOARD_DEV_GPIO_EXPANDER_SUPPORT */
#include "dev_display_lcd.h"

static const char *TAG = "DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI";

extern esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel);

static bool dev_display_lcd_rgb_3wire_spi_has_user_fbs_func(const dev_display_lcd_config_t *lcd_cfg)
{
    return lcd_cfg->sub_cfg.rgb_3wire_spi.user_fbs_func && strlen(lcd_cfg->sub_cfg.rgb_3wire_spi.user_fbs_func) > 0;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
static int dev_display_lcd_rgb_3wire_spi_call_user_fbs_func(const dev_display_lcd_config_t *lcd_cfg,
                                                            dev_display_lcd_rgb_user_fbs_action_t action,
                                                            void *user_fbs[ESP_RGB_LCD_PANEL_MAX_FB_NUM])
{
    if (!dev_display_lcd_rgb_3wire_spi_has_user_fbs_func(lcd_cfg)) {
        return 0;
    }

    void *extra_func = NULL;
    if (esp_board_extra_func_get(lcd_cfg->sub_cfg.rgb_3wire_spi.user_fbs_func, &extra_func) != 0) {
        ESP_LOGE(TAG, "RGB user frame buffer function '%s' not found", lcd_cfg->sub_cfg.rgb_3wire_spi.user_fbs_func);
        return -1;
    }

    dev_display_lcd_rgb_user_fbs_func_t user_fbs_func = (dev_display_lcd_rgb_user_fbs_func_t)extra_func;
    return user_fbs_func(lcd_cfg, action, user_fbs);
}
#endif  /* ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) */

int dev_display_lcd_sub_rgb_3wire_spi_init(void *cfg, int cfg_size, void **device_handle)
{
    (void)cfg_size;
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)cfg;
    dev_display_lcd_handles_t *lcd_handles = calloc(1, sizeof(dev_display_lcd_handles_t));
    if (lcd_handles == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for dev_display_lcd_sub_rgb_3wire_spi");
        return -1;
    }

    ESP_LOGI(TAG, "Initializing RGB + 3-wire SPI LCD display: %s, chip: %s", lcd_cfg->name, lcd_cfg->chip);

    esp_lcd_panel_io_3wire_spi_config_t io_config = lcd_cfg->sub_cfg.rgb_3wire_spi.io_3wire_spi_config;
    esp_lcd_rgb_panel_config_t rgb_panel_config = lcd_cfg->sub_cfg.rgb_3wire_spi.rgb_panel_config;
    esp_lcd_panel_dev_config_t panel_config = lcd_cfg->sub_cfg.rgb_3wire_spi.panel_config;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    bool user_fbs_acquired = false;
#endif  /* ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) */
    bool io_expander_ref_acquired = false;

    const char *io_expander_name = lcd_cfg->sub_cfg.rgb_3wire_spi.io_expander_name;
    if (io_expander_name && strlen(io_expander_name) > 0) {
#ifndef CONFIG_ESP_BOARD_DEV_GPIO_EXPANDER_SUPPORT
        ESP_LOGE(TAG, "IO expander '%s' configured but GPIO expander support is disabled", io_expander_name);
        goto cleanup;
#else
        esp_err_t dev_ret = esp_board_device_init(io_expander_name);
        if (dev_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init IO expander device '%s': %s", io_expander_name, esp_err_to_name(dev_ret));
            goto cleanup;
        }
        io_expander_ref_acquired = true;

        void *io_expander_dev = NULL;
        dev_ret = esp_board_device_get_handle(io_expander_name, &io_expander_dev);
        if (dev_ret != ESP_OK || io_expander_dev == NULL) {
            ESP_LOGE(TAG, "Failed to get IO expander device handle '%s': %s", io_expander_name, esp_err_to_name(dev_ret));
            goto cleanup_io_expander;
        }
        io_config.line_config.io_expander = *(esp_io_expander_handle_t *)io_expander_dev;
#endif  /* CONFIG_ESP_BOARD_DEV_GPIO_EXPANDER_SUPPORT */
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    if (dev_display_lcd_rgb_3wire_spi_has_user_fbs_func(lcd_cfg)) {
        int ret = dev_display_lcd_rgb_3wire_spi_call_user_fbs_func(lcd_cfg, DEV_DISPLAY_LCD_RGB_USER_FBS_GET,
                                                                   rgb_panel_config.user_fbs);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to get RGB user frame buffers: %d", ret);
            goto cleanup_io_expander;
        }
        memcpy(lcd_handles->rgb_user_fbs, rgb_panel_config.user_fbs, sizeof(lcd_handles->rgb_user_fbs));
        user_fbs_acquired = true;
    }
#else
    if (dev_display_lcd_rgb_3wire_spi_has_user_fbs_func(lcd_cfg)) {
        ESP_LOGE(TAG, "RGB user frame buffers require ESP-IDF v6.0 or later");
        goto cleanup_io_expander;
    }
#endif  /* ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) */

    esp_err_t ret = esp_lcd_new_panel_io_3wire_spi(&io_config, &lcd_handles->io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create 3-wire SPI panel IO: %s", esp_err_to_name(ret));
        goto cleanup_user_fbs;
    }

    ret = lcd_panel_factory_entry_t(lcd_handles->io_handle, &panel_config, &lcd_handles->panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RGB + 3-wire SPI LCD panel: %s", esp_err_to_name(ret));
        if (!lcd_cfg->sub_cfg.rgb_3wire_spi.auto_del_panel_io) {
            esp_lcd_panel_io_del(lcd_handles->io_handle);
        } else {
            ESP_LOGW(TAG, "Skip panel IO cleanup after factory failure because auto_del_panel_io is enabled");
        }
        lcd_handles->io_handle = NULL;
        goto cleanup_user_fbs;
    }

    if (lcd_cfg->sub_cfg.rgb_3wire_spi.auto_del_panel_io) {
        lcd_handles->io_handle = NULL;
    }

    *device_handle = lcd_handles;
    return 0;

cleanup_user_fbs:
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    if (user_fbs_acquired) {
        int release_ret = dev_display_lcd_rgb_3wire_spi_call_user_fbs_func(lcd_cfg, DEV_DISPLAY_LCD_RGB_USER_FBS_RELEASE,
                                                                           lcd_handles->rgb_user_fbs);
        if (release_ret != 0) {
            ESP_LOGW(TAG, "Failed to release RGB user frame buffers after init failure: %d", release_ret);
        }
    }
#endif  /* ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) */
cleanup_io_expander:
    if (io_expander_ref_acquired) {
        esp_board_device_deinit(io_expander_name);
    }
cleanup:
    free(lcd_handles);
    return -1;
}

int dev_display_lcd_sub_rgb_3wire_spi_deinit_with_config(void *device_handle, const dev_display_lcd_config_t *cfg)
{
    if (device_handle == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }

    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)device_handle;

    if (lcd_handles->panel_handle) {
        esp_lcd_panel_del(lcd_handles->panel_handle);
        lcd_handles->panel_handle = NULL;
    }

    if (lcd_handles->io_handle) {
        esp_lcd_panel_io_del(lcd_handles->io_handle);
        lcd_handles->io_handle = NULL;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    if (cfg && dev_display_lcd_rgb_3wire_spi_has_user_fbs_func(cfg)) {
        int release_ret = dev_display_lcd_rgb_3wire_spi_call_user_fbs_func(cfg, DEV_DISPLAY_LCD_RGB_USER_FBS_RELEASE,
                                                                           lcd_handles->rgb_user_fbs);
        if (release_ret != 0) {
            ESP_LOGW(TAG, "Failed to release RGB user frame buffers: %d", release_ret);
        }
    }
#endif  /* ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) */

    if (cfg && cfg->sub_cfg.rgb_3wire_spi.io_expander_name && strlen(cfg->sub_cfg.rgb_3wire_spi.io_expander_name) > 0) {
        esp_board_device_deinit(cfg->sub_cfg.rgb_3wire_spi.io_expander_name);
    }

    free(device_handle);
    return cfg ? 0 : -1;
}

int dev_display_lcd_sub_rgb_3wire_spi_deinit(void *device_handle)
{
    dev_display_lcd_config_t *cfg = NULL;
    esp_board_device_get_config_by_handle(device_handle, (void **)&cfg);
    return dev_display_lcd_sub_rgb_3wire_spi_deinit_with_config(device_handle, cfg);
}

ESP_BOARD_SUBTYPE_ENTRY_IMPLEMENT(display_lcd, rgb_3wire_spi, dev_display_lcd_sub_rgb_3wire_spi_init, dev_display_lcd_sub_rgb_3wire_spi_deinit);
