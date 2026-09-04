/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Low-level touch initialization for Candis-S31.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_touch.h"

/** @addtogroup g04_display
 *  @{
 */
typedef struct {
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
} bsp_touch_config_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_touch_new(const bsp_touch_config_t *config,
                        esp_lcd_touch_handle_t *ret_touch);
esp_lcd_touch_handle_t bsp_touch_get_handle(void);
esp_err_t bsp_touch_delete(void);
/** @} */

#ifdef __cplusplus
}
#endif
