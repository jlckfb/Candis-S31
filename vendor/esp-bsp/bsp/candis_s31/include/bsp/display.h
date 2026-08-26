/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

#define ESP_LCD_COLOR_FORMAT_RGB565            1
#define ESP_LCD_COLOR_FORMAT_RGB888            2
#define BSP_LCD_COLOR_FORMAT                    ESP_LCD_COLOR_FORMAT_RGB565
/* CO5300 expects RGB565 MSB-first on the wire. The LVGL path selects
 * RGB565_SWAPPED so its renderer writes that byte order directly instead of
 * swapping each flushed block in esp_lvgl_port. The NoGLIB direct-draw path
 * sends framebuffer bytes as-is, so NoGLIB users must likewise provide
 * big-endian RGB565 buffers. */
#define BSP_LCD_BIGENDIAN                       1
#define BSP_LCD_INVERT_COLORS                   0
#define BSP_LCD_BITS_PER_PIXEL                  16
#define BSP_LCD_COLOR_SPACE                     LCD_RGB_ELEMENT_ORDER_RGB
#define BSP_LCD_H_RES                           460
#define BSP_LCD_V_RES                           460

/** @addtogroup g04_display
 *  @{
 */
typedef struct {
    int max_transfer_sz;
} bsp_display_config_t;

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_handle_t control;
} bsp_lcd_handles_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_display_new(const bsp_display_config_t *config,
                          esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io);
esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config,
                                       bsp_lcd_handles_t *ret_handles);
void bsp_display_delete(void);

esp_err_t bsp_display_brightness_init(void);
esp_err_t bsp_display_brightness_deinit(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
esp_err_t bsp_display_backlight_on(void);
esp_err_t bsp_display_backlight_off(void);
esp_lcd_panel_handle_t bsp_display_get_panel_handle(void);
/** @} */

#ifdef __cplusplus
}
#endif
