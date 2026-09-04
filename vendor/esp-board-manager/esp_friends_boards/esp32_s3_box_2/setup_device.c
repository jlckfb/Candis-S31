/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_lcd_panel_st7789.h"
#include "dev_display_lcd.h"
#include "dev_button.h"
#include "esp_board_extra_func_entry.h"
#include "button_interface.h"
#include "esp_board_device.h"

#define XIO_KEY_L  5
#define XIO_KEY_M  7

typedef struct {
    button_driver_t  base;
    uint16_t  pin_num;
    uint8_t  active_level;
    esp_io_expander_handle_t  io_handle;
} custom_button_driver_t;
static const char *TAG = "ESP32S3_BOX2_SETUP_DEVICE";

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle, const uint16_t dev_addr, esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_i2c_tca95xx_16bit(i2c_handle, dev_addr, handle_ret);
    // printf("handle_ret->io_count: %d\n", (*handle_ret)->config.io_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create IO expander handle\n");
        return ret;
    }
    return ESP_OK;
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    esp_lcd_panel_io_tx_param(io, 0xCF, (uint8_t[]) {0x00, 0x83, 0x30}, 3);
    esp_lcd_panel_io_tx_param(io, 0xED, (uint8_t[]) {0x64, 0x03, 0x12, 0x81}, 4);
    esp_lcd_panel_io_tx_param(io, 0xE8, (uint8_t[]) {0x85, 0x01, 0x79}, 3);
    esp_lcd_panel_io_tx_param(io, 0xCB, (uint8_t[]) {0x39, 0x2C, 0x00, 0x34, 0x02}, 5);
    esp_lcd_panel_io_tx_param(io, 0xF7, (uint8_t[]) {0x20}, 1);
    esp_lcd_panel_io_tx_param(io, 0xEA, (uint8_t[]) {0x00, 0x00}, 2);
    esp_lcd_panel_io_tx_param(io, 0xbb, (uint8_t[]) {0x20}, 1);
    esp_lcd_panel_io_tx_param(io, 0xc3, (uint8_t[]) {0x00}, 1);
    esp_lcd_panel_io_tx_param(io, 0xC4, (uint8_t[]) {0x20}, 1);
    esp_lcd_panel_io_tx_param(io, 0xC5, (uint8_t[]) {0x20}, 1);
    esp_lcd_panel_io_tx_param(io, 0xC6, (uint8_t[]) {0x10}, 1);
    esp_lcd_panel_io_tx_param(io, 0xC7, (uint8_t[]) {0xB0}, 1);
    esp_lcd_panel_io_tx_param(io, 0x36, (uint8_t[]) {0x60}, 1);
    esp_lcd_panel_io_tx_param(io, 0x3A, (uint8_t[]) {0x55}, 1);
    esp_lcd_panel_io_tx_param(io, 0xB1, (uint8_t[]) {0x00, 0x1B}, 2);
    esp_lcd_panel_io_tx_param(io, 0xF2, (uint8_t[]) {0x08}, 1);
    esp_lcd_panel_io_tx_param(io, 0x26, (uint8_t[]) {0x01}, 1);
    esp_lcd_panel_io_tx_param(io, 0xE0, (uint8_t[]) {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}, 14);
    esp_lcd_panel_io_tx_param(io, 0xE1, (uint8_t[]) {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E}, 14);
    esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t[]) {0x07}, 1);

    int ret = esp_lcd_new_panel_st7789(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New ili9341 panel failed");
        return ret;
    }
    return ESP_OK;
}

static uint8_t custom_button_get_level(button_driver_t *driver)
{
    custom_button_driver_t *custom = (custom_button_driver_t *)driver;
    if (!custom->io_handle) {
        return 0;
    }
    uint32_t level = 0;
    uint32_t pin_mask = (1U << custom->pin_num);
    esp_err_t ret = esp_io_expander_get_level(custom->io_handle, pin_mask, &level);
    if (ret != ESP_OK) {
        return 0;
    }
    return (!!(level & pin_mask) == custom->active_level);
}

esp_err_t custom_button_del(button_driver_t *button_driver)
{
    custom_button_driver_t *custom = (custom_button_driver_t *)button_driver;
    free(custom);
    return ESP_OK;
}

static button_driver_t *button_l_create(const dev_button_config_t *config)
{
    custom_button_driver_t *custom = calloc(1, sizeof(custom_button_driver_t));
    esp_io_expander_handle_t *io_handle = NULL;
    if (!custom) {
        return NULL;
    }
    if (esp_board_device_get_handle("gpio_expander", (void *)&io_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get gpio_expander handle for button_l");
        free(custom);
        return NULL;
    }
    custom->io_handle = *io_handle;
    custom->base.get_key_level = custom_button_get_level;
    custom->base.del = custom_button_del;
    custom->base.enable_power_save = false;
    custom->pin_num = XIO_KEY_L;
    custom->active_level = 0;  // Active LOW
    return &custom->base;
}

static button_driver_t *button_m_create(const dev_button_config_t *config)
{
    custom_button_driver_t *custom = calloc(1, sizeof(custom_button_driver_t));
    esp_io_expander_handle_t *io_handle = NULL;
    if (!custom) {
        return NULL;
    }
    if (esp_board_device_get_handle("gpio_expander", (void *)&io_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get gpio_expander handle for button_m");
        free(custom);
        return NULL;
    }
    custom->io_handle = *io_handle;
    custom->base.get_key_level = custom_button_get_level;
    custom->base.del = custom_button_del;
    custom->base.enable_power_save = false;
    custom->pin_num = XIO_KEY_M;
    custom->active_level = 1;  // Active HIGH
    return &custom->base;
}

DEVICE_EXTRA_FUNC_REGISTER(button_l, button_l_create);
DEVICE_EXTRA_FUNC_REGISTER(button_m, button_m_create);
