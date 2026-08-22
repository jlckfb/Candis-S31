/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ESP LVGL port display
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#if LVGL_VERSION_MAJOR == 8
#include "esp_lvgl_port_compatibility.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Rotation configuration
 */
typedef struct {
    bool swap_xy;  /*!< LCD Screen swapped X and Y (in esp_lcd driver) */
    bool mirror_x; /*!< LCD Screen mirrored X (in esp_lcd driver) */
    bool mirror_y; /*!< LCD Screen mirrored Y (in esp_lcd driver) */
} lvgl_port_rotation_cfg_t;

/**
 * @brief Rounder callback
 */
typedef void (*lvgl_port_rounder_cb_t)(lv_area_t *area);

#if LVGL_VERSION_MAJOR >= 9
/**
 * @brief Optional TE edge observer
 *
 * The callback runs in GPIO ISR context. It must not block and may only call
 * ISR-safe functions. Registering an observer temporarily changes the TE GPIO
 * trigger to both edges; removing it restores rising-edge-only operation.
 */
typedef void (*lvgl_port_te_observer_cb_t)(bool level, int64_t timestamp_us,
        void *user_ctx);
#endif

/**
 * @brief Configuration display structure
 */
typedef struct {
    esp_lcd_panel_io_handle_t io_handle;    /*!< LCD panel IO handle */
    esp_lcd_panel_handle_t panel_handle;    /*!< LCD panel handle */
    esp_lcd_panel_handle_t control_handle;  /*!< LCD panel control handle */

    uint32_t    buffer_size;        /*!< Size of the buffer for the screen in pixels */
    bool        double_buffer;      /*!< True, if should be allocated two buffers */
    uint32_t    trans_size;         /*!< Allocated buffer will be in SRAM to move framebuf (optional) */

    uint32_t    hres;           /*!< LCD display horizontal resolution */
    uint32_t    vres;           /*!< LCD display vertical resolution */

    bool        monochrome;     /*!< True, if display is monochrome and using 1bit for 1px */

    int
    te_gpio_num;    /*!< GPIO number of the panel TE (tearing effect) output. Used only when flags.te_sync is set (LVGL 9 only) */

    lvgl_port_rotation_cfg_t
    rotation;      /*!< Default values of the screen rotation (Only HW state. Not supported for default SW rotation!) */
    lvgl_port_rounder_cb_t   rounder_cb;      /*!< Rounder callback for display area */
#if LVGL_VERSION_MAJOR >= 9
    lv_color_format_t        color_format;  /*!< The color format of the display */
#endif
    struct {
        unsigned int buff_dma: 1;    /*!< Allocated LVGL buffer will be DMA capable */
        unsigned int buff_spiram: 1; /*!< Allocated LVGL buffer will be in PSRAM */
        unsigned int sw_rotate: 1;   /*!< Use software rotation (slower) or PPA if available */
#if LVGL_VERSION_MAJOR >= 9
        unsigned int swap_bytes: 1;  /*!< Swap bytes in RGB565 (16-bit) color format before send to LCD driver */
#endif
        unsigned int full_refresh: 1;/*!< 1: Always make the whole screen redrawn */
        unsigned int direct_mode: 1; /*!< 1: Use screen-sized buffers and draw to absolute coordinates */
unsigned int te_sync:
        1;     /*!< 1: Gate the first data transfer of every LVGL refresh cycle on a TE rising edge (SPI/I80 GRAM displays, LVGL 9 only). Aligns writes with panel scan timing; adds up to one frame of pipeline latency. Requires the panel TE output to be enabled (0x35) and wired to te_gpio_num */
    } flags;
} lvgl_port_display_cfg_t;

/**
 * @brief Configuration RGB display structure
 */
typedef struct {
    struct {
        unsigned int bb_mode: 1;        /*!< 1: Use bounce buffer mode */
unsigned int avoid_tearing:
        1;  /*!< 1: Use internal RGB buffers as a LVGL draw buffers to avoid tearing effect, enabling this option requires over two LCD buffers and may reduce the frame rate */
    } flags;
} lvgl_port_display_rgb_cfg_t;

/**
 * @brief Configuration MIPI-DSI display structure
 */
typedef struct {
    struct {
unsigned int avoid_tearing:
        1;  /*!< 1: Use internal MIPI-DSI buffers as a LVGL draw buffers to avoid tearing effect, enabling this option requires over two LCD buffers and may reduce the frame rate */
    } flags;
} lvgl_port_display_dsi_cfg_t;

/**
 * @brief Add I2C/SPI/I8080 display handling to LVGL
 *
 * @note Allocated memory in this function is not free in deinit. You must call lvgl_port_remove_disp for free all memory!
 *
 * @param disp_cfg Display configuration structure
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *disp_cfg);

/**
 * @brief Add MIPI-DSI display handling to LVGL
 *
 * @note Allocated memory in this function is not free in deinit. You must call lvgl_port_remove_disp for free all memory!
 *
 * @param disp_cfg Display configuration structure
 * @param dsi_cfg MIPI-DSI display specific configuration structure
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *lvgl_port_add_disp_dsi(const lvgl_port_display_cfg_t *disp_cfg,
                                     const lvgl_port_display_dsi_cfg_t *dsi_cfg);

/**
 * @brief Add RGB display handling to LVGL
 *
 * @note Allocated memory in this function is not free in deinit. You must call lvgl_port_remove_disp for free all memory!
 *
 * @param disp_cfg Display configuration structure
 * @param rgb_cfg RGB display specific configuration structure
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *lvgl_port_add_disp_rgb(const lvgl_port_display_cfg_t *disp_cfg,
                                     const lvgl_port_display_rgb_cfg_t *rgb_cfg);

/**
 * @brief Remove display handling from LVGL
 *
 * @note Free all memory used for this display.
 *
 * @return
 *      - ESP_OK                    on success
 */
esp_err_t lvgl_port_remove_disp(lv_display_t *disp);

/**
 * @brief Enable or disable TE synchronization at runtime
 *
 * Only valid for displays created with flags.te_sync set. Disable before
 * entering a panel sleep mode that stops the TE output, re-enable after wake.
 *
 * @param disp LVGL display handle
 * @param enable True to gate refresh cycles on the TE edge, false to flush immediately
 * @return
 *      - ESP_OK                    on success
 *      - ESP_ERR_INVALID_STATE     if the display was not created with TE sync
 */
esp_err_t lvgl_port_display_te_sync_enable(lv_display_t *disp, bool enable);

#if LVGL_VERSION_MAJOR >= 9
/**
 * @brief Register or remove a non-owning observer for TE edges
 *
 * The LVGL port remains the sole owner of the GPIO ISR. A non-NULL callback
 * observes both rising and falling edges while normal TE synchronization keeps
 * consuming rising edges only. Passing NULL restores the rising-edge trigger.
 * The caller must keep user_ctx valid until the observer is removed.
 *
 * @param disp LVGL display handle
 * @param callback ISR-context observer, or NULL to remove it
 * @param user_ctx Context passed to callback
 * @return
 *      - ESP_OK                    on success
 *      - ESP_ERR_INVALID_ARG       if the display handle is NULL
 *      - ESP_ERR_INVALID_STATE     if the display was not created with TE sync
 */
esp_err_t lvgl_port_display_te_observer_set(lv_display_t *disp,
        lvgl_port_te_observer_cb_t callback, void *user_ctx);
#endif

#ifdef __cplusplus
}
#endif
