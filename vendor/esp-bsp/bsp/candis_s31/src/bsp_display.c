/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_touch_cst820.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/candis_s31.h"

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
#include "esp_lvgl_port.h"
#endif

static const char *TAG = "candis_display";

/* EVT bring-up exposes the panel at 30 % brightness, but initialization and
 * deep-wake keep WRDISBV at zero until a complete black/current UI frame has
 * reached GRAM. This preserves TE output while preventing random GRAM or a
 * fixed 30 % flash from becoming visible. The 0x63 (WRHBMDISBV) init value
 * only applies in HBM mode, which this board never enables. */
#define CO5300_FIRST_BRIGHTNESS_PERCENT  30
#define CO5300_HIDDEN_BRIGHTNESS_HW      0x00

static bsp_lcd_handles_t s_display;
static bool s_spi_initialized;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_touch_io;
static bool s_deep_standby;
static bool s_panel_sleeping;
static bool s_touch_sleeping;
static bool s_recovery_required;
#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static bool s_lvgl_invalidation_suspended;
#endif
static void touch_clamp_coordinates(esp_lcd_touch_handle_t touch,
                                    uint16_t *x, uint16_t *y,
                                    uint16_t *strength, uint8_t *point_num,
                                    uint8_t max_point_num);
/* Last brightness chosen through bsp_display_brightness_set(); restored by
 * bsp_display_backlight_on() so a wake returns to the operator's level
 * instead of forcing 100 %. The init sequence remains dark at 0 %, then this
 * saved default is applied only after the first complete redraw. */
static uint8_t s_brightness_percent = CO5300_FIRST_BRIGHTNESS_PERCENT;

#define CO5300_CMD_DEEP_STANDBY_ON       0x4F
#define CO5300_DEEP_STANDBY_PARAMETER    0x01
#define CO5300_SLEEP_TRANSITION_MS       120

/* QSPI panel IO is configured with lcd_cmd_bits=32: commands go on the wire
 * as (0x02 << 24) | (cmd << 8). This mirrors the tx_param() encoding inside
 * the esp_lcd_co5300 driver and must be used for every command sent directly
 * through esp_lcd_panel_io_tx_param(). */
#define CO5300_QSPI_WRITE_CMD(cmd)       ((0x02UL << 24) | (((uint32_t)(cmd) & 0xFF) << 8))

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_display_t *s_lvgl_display;
static lv_indev_t *s_lvgl_touch;
static lv_indev_read_cb_t s_lvgl_touch_read_cb;
static bool s_lvgl_initialized;
static bool s_lvgl_touch_suspended;
static esp_err_t display_transition_lock(bool *locked);
static void display_transition_unlock(bool locked);
#endif

/* Park peripheral-facing pins as floating inputs (no pull-up/pull-down) so
 * they cannot back-feed a supply that is about to be removed. */
static esp_err_t pins_floating(const gpio_num_t *pins, size_t count)
{
    uint64_t mask = 0;
    for (size_t index = 0; index < count; ++index) {
        mask |= BIT64(pins[index]);
    }
    const gpio_config_t config = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

/* The CST820 driver leaves RST/INT with internal pull-ups enabled when it
 * deletes them. */
static esp_err_t touch_pins_floating(void)
{
    const gpio_num_t pins[] = { BSP_TOUCH_RST, BSP_TOUCH_INT };
    return pins_floating(pins, sizeof(pins) / sizeof(pins[0]));
}

/* EVT1 keeps panel VBAT on TG28_VSYS while VCI/IOVCC are switchable. Float
 * every QSPI/control pin before rail removal, not only RESX. */
static esp_err_t display_pins_floating(void)
{
    const gpio_num_t pins[] = {
        BSP_LCD_RST, BSP_LCD_TE, BSP_LCD_CS, BSP_LCD_QSPI_CLK,
        BSP_LCD_QSPI_DATA0, BSP_LCD_QSPI_DATA1,
        BSP_LCD_QSPI_DATA2, BSP_LCD_QSPI_DATA3,
    };
    return pins_floating(pins, sizeof(pins) / sizeof(pins[0]));
}

/* AM200Q460460LK supplier initialization sequence, converted to esp_lcd. */
static const co5300_lcd_init_cmd_t s_panel_init[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    /* Keep Display-On optically dark until LVGL has replaced unknown GRAM.
     * Brightness 0 % was verified on EVT1 to leave the 60 Hz TE waveform
     * running, so the hidden first redraw can still use normal TE sync. */
    {0x51, (uint8_t[]){CO5300_HIDDEN_BRIGHTNESS_HW}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    /* The active 460-pixel window starts at column 10. */
    {0x2A, (uint8_t[]){0x00, 0x0A, 0x01, 0xD5}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xCB}, 4, 0},
    {0x11, NULL, 0, CO5300_SLEEP_TRANSITION_MS},
    /* SLPOUT reloads command defaults during its first 5 ms. Enable TE only
     * after the 120 ms wake transition so GPIO16 actually emits Mode 1. */
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x29, NULL, 0, 0},
};

esp_err_t bsp_display_brightness_init(void)
{
    return s_display.panel != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t bsp_display_brightness_deinit(void)
{
    return ESP_OK;
}

/* Write the hardware brightness register without touching the saved level,
 * so a temporary 0 % (backlight_off) does not erase the operator's choice. */
static esp_err_t brightness_hw_write(int brightness_percent)
{
    ESP_RETURN_ON_FALSE(s_display.panel != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display is not initialized");
    return esp_lcd_panel_co5300_set_brightness(s_display.panel,
            (uint8_t)brightness_percent);
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    ESP_RETURN_ON_FALSE(brightness_percent >= 0 && brightness_percent <= 100,
                        ESP_ERR_INVALID_ARG, TAG, "brightness must be 0..100");

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
    bool locked = false;
    esp_err_t error = display_transition_lock(&locked);
    if (error != ESP_OK) {
        return error;
    }
#else
    esp_err_t error = ESP_OK;
#endif

    /* A sleeping CO5300 must not receive WRDISBV, and deep wake must remain
     * optically dark until its hidden full redraw has completed. Remember a
     * concurrent caller's requested level and apply it at the normal wake
     * commit point instead of exposing stale/unknown GRAM mid-transition. */
    if (s_panel_sleeping || s_deep_standby || s_recovery_required) {
        s_brightness_percent = (uint8_t)brightness_percent;
    } else {
        error = brightness_hw_write(brightness_percent);
        if (error == ESP_OK) {
            s_brightness_percent = (uint8_t)brightness_percent;
        } else {
            ESP_LOGE(TAG, "brightness update failed: %s",
                     esp_err_to_name(error));
        }
    }

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
    display_transition_unlock(locked);
#endif
    return error;
}

static esp_err_t display_backlight_off_locked(void)
{
    ESP_RETURN_ON_ERROR(brightness_hw_write(0), TAG,
                        "brightness update failed");
    return esp_lcd_panel_disp_on_off(s_display.panel, false);
}

static esp_err_t display_backlight_on_locked(void)
{
    /* Re-assert the saved level BEFORE Display-On: the panel must never
     * light at a stale value. The LVGL startup/deep-wake paths separately
     * redraw at 0 % before restoring this saved operator level. */
    ESP_RETURN_ON_ERROR(brightness_hw_write(s_brightness_percent), TAG,
                        "brightness restore failed");
    return esp_lcd_panel_disp_on_off(s_display.panel, true);
}

esp_err_t bsp_display_backlight_off(void)
{
#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
    bool locked = false;
    esp_err_t error = display_transition_lock(&locked);
    if (error != ESP_OK) {
        return error;
    }
#else
    esp_err_t error = ESP_OK;
#endif

    if (s_display.panel == NULL) {
        error = ESP_ERR_INVALID_STATE;
    } else if (s_panel_sleeping || s_deep_standby) {
        /* Both sleep modes are already optically dark. Avoid sending a DCS
         * command while the panel cannot accept ordinary register writes. */
        error = ESP_OK;
    } else if (s_recovery_required) {
        /* Recovery owns the panel's next command sequence. Do not change its
         * active/off assumption from an unrelated public API. */
        error = ESP_ERR_INVALID_STATE;
    } else {
        error = display_backlight_off_locked();
    }

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
    display_transition_unlock(locked);
#endif
    return error;
}

esp_err_t bsp_display_backlight_on(void)
{
#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
    bool locked = false;
    esp_err_t error = display_transition_lock(&locked);
    if (error != ESP_OK) {
        return error;
    }
#else
    esp_err_t error = ESP_OK;
#endif

    if (s_display.panel == NULL || s_panel_sleeping || s_deep_standby ||
            s_recovery_required) {
        /* Only the matching wake path may expose GRAM after sleep/reset. */
        error = ESP_ERR_INVALID_STATE;
    } else {
        error = display_backlight_on_locked();
    }

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
    display_transition_unlock(locked);
#endif
    return error;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config,
                          esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io)
{
    ESP_RETURN_ON_FALSE(ret_panel != NULL && ret_io != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "display return handle is NULL");
    bsp_lcd_handles_t handles = {0};
    ESP_RETURN_ON_ERROR(bsp_display_new_with_handles(config, &handles), TAG,
                        "display creation failed");
    *ret_panel = handles.panel;
    *ret_io = handles.io;
    return ESP_OK;
}

esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config,
                                       bsp_lcd_handles_t *ret_handles)
{
    ESP_RETURN_ON_FALSE(ret_handles != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "display return handles are NULL");
    ESP_RETURN_ON_FALSE(s_display.panel == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display is already initialized");

    const int max_transfer_size = config != NULL && config->max_transfer_sz > 0 ?
                                  config->max_transfer_sz :
                                  BSP_LCD_H_RES * 80 * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(bsp_peripheral_power_set(BSP_PERIPHERAL_DISPLAY, true),
                        TAG, "display power sequence failed");

    const spi_bus_config_t bus_config =
        CO5300_PANEL_BUS_QSPI_CONFIG(BSP_LCD_QSPI_CLK,
                                     BSP_LCD_QSPI_DATA0,
                                     BSP_LCD_QSPI_DATA1,
                                     BSP_LCD_QSPI_DATA2,
                                     BSP_LCD_QSPI_DATA3,
                                     max_transfer_size);
    esp_err_t error = spi_bus_initialize(BSP_LCD_SPI_NUM, &bus_config,
                                         SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        goto fail;
    }
    s_spi_initialized = true;

    esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    io_config.pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ;
    io_config.trans_queue_depth = 10;
    /* Avoid per-chunk internal bounce buffers when an application selects
     * full-screen LVGL buffers in PSRAM. Internal-RAM buffers are unchanged. */
    io_config.flags.psram_dma_direct = 1;
    error = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM,
                                     &io_config, &s_display.io);
    if (error != ESP_OK) {
        goto fail;
    }

    const co5300_vendor_config_t vendor_config = {
        .init_cmds = s_panel_init,
        .init_cmds_size = sizeof(s_panel_init) / sizeof(s_panel_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = (void *) &vendor_config,
    };
    error = esp_lcd_new_panel_co5300(s_display.io, &panel_config,
                                     &s_display.panel);
    if (error != ESP_OK) {
        goto fail;
    }

    error = esp_lcd_panel_reset(s_display.panel);
    if (error != ESP_OK) {
        goto fail;
    }
    error = esp_lcd_panel_init(s_display.panel);
    if (error != ESP_OK) {
        goto fail;
    }
    error = esp_lcd_panel_set_gap(s_display.panel, BSP_LCD_X_GAP, BSP_LCD_Y_GAP);
    if (error != ESP_OK) {
        goto fail;
    }
    error = esp_lcd_panel_disp_on_off(s_display.panel, false);
    if (error != ESP_OK) {
        goto fail;
    }

    /* TE is routed from the panel to GPIO16 through R55 (0 ohm) and tearing
     * output is enabled by the init sequence (0x35). With
     * CONFIG_BSP_LCD_TE_SYNC the LVGL port owns the pin's interrupt and
     * gates every refresh cycle on it; this gpio_config only keeps the pin
     * at a defined level for raw (non-LVGL) panel users. */
    const gpio_config_t te_gpio_config = {
        .pin_bit_mask = BIT64(BSP_LCD_TE),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    error = gpio_config(&te_gpio_config);
    if (error != ESP_OK) {
        goto fail;
    }

    *ret_handles = s_display;
    ESP_LOGI(TAG, "CO5300 initialized at %dx%d, QSPI %d MHz",
             BSP_LCD_H_RES, BSP_LCD_V_RES, CONFIG_BSP_LCD_PIXEL_CLOCK_MHZ);
    return ESP_OK;

fail:
    bsp_display_delete();
    return error;
}

void bsp_display_delete(void)
{
    s_deep_standby = false;
    s_panel_sleeping = false;
    s_touch_sleeping = false;
    s_recovery_required = false;
#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
    s_lvgl_invalidation_suspended = false;
    s_lvgl_touch_suspended = false;
    s_lvgl_touch_read_cb = NULL;
#endif
    if (s_display.panel != NULL) {
        /* Best-effort safe power-down: the CO5300 must see Display-Off and
         * Sleep-In while the display rail is still up, otherwise the panel
         * can latch into an undefined state when VCI/VBAT drops. Command
         * failures are logged, never fatal: this API is void and the rail
         * must still come down. Once the panel was created, always honor
         * the SLPIN transition window before deleting it, even if a
         * command above failed. */
        const esp_err_t disp_off_error =
            esp_lcd_panel_disp_on_off(s_display.panel, false);
        if (disp_off_error != ESP_OK) {
            ESP_LOGW(TAG, "display-off before delete failed: %s",
                     esp_err_to_name(disp_off_error));
        }
        if (s_display.io != NULL) {
            const esp_err_t slpin_error = esp_lcd_panel_io_tx_param(
                                              s_display.io,
                                              CO5300_QSPI_WRITE_CMD(LCD_CMD_SLPIN),
                                              NULL, 0);
            if (slpin_error != ESP_OK) {
                ESP_LOGW(TAG, "display sleep-in before delete failed: %s",
                         esp_err_to_name(slpin_error));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CO5300_SLEEP_TRANSITION_MS));
        esp_lcd_panel_del(s_display.panel);
        s_display.panel = NULL;
    }
    if (s_display.io != NULL) {
        esp_lcd_panel_io_del(s_display.io);
        s_display.io = NULL;
    }
    if (s_spi_initialized) {
        spi_bus_free(BSP_LCD_SPI_NUM);
        s_spi_initialized = false;
    }
    /* The CO5300 driver and SPI teardown may leave output latches or pulls on
     * panel-facing pins. Float the whole interface before VCI/IOVCC fall:
     * EVT1 keeps VBAT tied to TG28_VSYS, so a driven QSPI input can otherwise
     * back-feed the partially powered panel. */
    const esp_err_t pins_error = display_pins_floating();
    if (pins_error != ESP_OK) {
        ESP_LOGW(TAG, "LCD interface pins hi-Z failed: %s",
                 esp_err_to_name(pins_error));
    }
    bsp_peripheral_power_set(BSP_PERIPHERAL_DISPLAY, false);
}

esp_err_t bsp_touch_new(const bsp_touch_config_t *config,
                        esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(ret_touch != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "touch return handle is NULL");
    if (s_touch != NULL) {
        *ret_touch = s_touch;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_peripheral_power_set(BSP_PERIPHERAL_TOUCH, true),
                        TAG, "touch power sequence failed");
    const esp_err_t i2c_error = bsp_i2c_init();
    if (i2c_error != ESP_OK) {
        const esp_err_t power_error =
            bsp_peripheral_power_set(BSP_PERIPHERAL_TOUCH, false);
        if (power_error != ESP_OK) {
            ESP_LOGE(TAG, "touch power rollback failed: %s",
                     esp_err_to_name(power_error));
        }
        return i2c_error;
    }

    esp_lcd_panel_io_i2c_config_t io_config =
        ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    io_config.scl_speed_hz = 400000;
    esp_err_t error = esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(),
                      &io_config, &s_touch_io);
    if (error != ESP_OK) {
        bsp_peripheral_power_set(BSP_PERIPHERAL_TOUCH, false);
        return error;
    }

    const bsp_touch_config_t default_config = {0};
    const bsp_touch_config_t *orientation = config != NULL ? config : &default_config;
    const esp_lcd_touch_config_t touch_config = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_RST,
        .int_gpio_num = BSP_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = orientation->swap_xy,
            .mirror_x = orientation->mirror_x,
            .mirror_y = orientation->mirror_y,
        },
        .process_coordinates = touch_clamp_coordinates,
    };
    error = esp_lcd_touch_new_i2c_cst820(s_touch_io, &touch_config, &s_touch);
    if (error != ESP_OK) {
        esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = NULL;
        /* Even a failed CST820 create leaves RST/INT with the internal
         * pull-up enabled (its del() runs gpio_reset_pin); park both as
         * floating inputs before ALDO2 comes down. The creation failure is
         * the first error and stays the return value; cleanup errors are
         * logged. */
        const esp_err_t pins_error = touch_pins_floating();
        if (pins_error != ESP_OK) {
            ESP_LOGE(TAG, "touch pins hi-Z failed: %s",
                     esp_err_to_name(pins_error));
        }
        bsp_peripheral_power_set(BSP_PERIPHERAL_TOUCH, false);
        return error;
    }
    s_touch_sleeping = false;
    *ret_touch = s_touch;
    return ESP_OK;
}

esp_err_t bsp_touch_delete(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_touch != NULL) {
        const esp_err_t error = esp_lcd_touch_del(s_touch);
        if (error == ESP_OK) {
            s_touch = NULL;
        } else {
            ESP_LOGE(TAG, "touch delete failed: %s", esp_err_to_name(error));
            first_error = error;
        }
    }
    /* The touch object owns the panel IO. Do not delete that IO underneath a
     * touch object whose delete failed; retain both handles for diagnostics.
     * Supply removal below is still mandatory. */
    if (s_touch == NULL && s_touch_io != NULL) {
        const esp_err_t error = esp_lcd_panel_io_del(s_touch_io);
        if (error == ESP_OK) {
            s_touch_io = NULL;
        } else {
            ESP_LOGE(TAG, "touch panel IO delete failed: %s",
                     esp_err_to_name(error));
            if (first_error == ESP_OK) {
                first_error = error;
            }
        }
    }
    /* The CST820 driver's del() left RST/INT with the internal pull-up
     * enabled; park both as floating inputs before ALDO2 comes down so
     * neither can back-feed the unpowered controller. Teardown remains
     * best-effort: a handle-release failure never skips pin or rail safety. */
    const esp_err_t pins_error = touch_pins_floating();
    if (pins_error != ESP_OK) {
        ESP_LOGE(TAG, "touch pins hi-Z failed: %s",
                 esp_err_to_name(pins_error));
        if (first_error == ESP_OK) {
            first_error = pins_error;
        }
    }
    const esp_err_t power_error =
        bsp_peripheral_power_set(BSP_PERIPHERAL_TOUCH, false);
    if (power_error != ESP_OK) {
        ESP_LOGE(TAG, "touch power-down failed: %s",
                 esp_err_to_name(power_error));
        if (first_error == ESP_OK) {
            first_error = power_error;
        }
    }
    if (s_touch == NULL) {
        s_touch_sleeping = false;
    }
    return first_error;
}

esp_lcd_touch_handle_t bsp_touch_get_handle(void)
{
    return s_touch;
}

static void touch_clamp_coordinates(esp_lcd_touch_handle_t touch,
                                    uint16_t *x, uint16_t *y,
                                    uint16_t *strength, uint8_t *point_num,
                                    uint8_t max_point_num)
{
    (void)touch;
    (void)strength;
    const uint8_t count = *point_num < max_point_num ? *point_num : max_point_num;
    for (uint8_t index = 0; index < count; ++index) {
        if (x[index] >= BSP_LCD_H_RES) {
            x[index] = BSP_LCD_H_RES - 1;
        }
        if (y[index] >= BSP_LCD_V_RES) {
            y[index] = BSP_LCD_V_RES - 1;
        }
    }
}

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static void co5300_rounder_cb(lv_area_t *area)
{
    /* The panel vendor requires every partial RAM-write window to start on
     * an even coordinate and end on an odd coordinate (2 x 2 alignment). */
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
    if (area->x2 >= BSP_LCD_H_RES) {
        area->x2 = BSP_LCD_H_RES - 1;
    }
    if (area->y2 >= BSP_LCD_V_RES) {
        area->y2 = BSP_LCD_V_RES - 1;
    }
}

static lv_display_t *display_lvgl_init(const bsp_display_cfg_t *config)
{
    const bsp_display_config_t panel_config = {
        .max_transfer_sz = (int)(config->buffer_size * sizeof(uint16_t)),
    };
    if (bsp_display_new(&panel_config, &s_display.panel, &s_display.io) != ESP_OK) {
        return NULL;
    }
    /* bsp_display_new() deliberately returns with Display-Off. Turn scanning
     * and TE back on while WRDISBV is still zero, so the first hidden redraw
     * remains synchronized without exposing unknown GRAM. */
    if (esp_lcd_panel_disp_on_off(s_display.panel, true) != ESP_OK) {
        return NULL;
    }

    /* Keep the panel in LVGL PARTIAL mode.  Only the first chunk of each
     * refresh cycle waits for the rising TE edge; the remaining chunks are
     * pipelined through the second DMA buffer.  This preserves 60 Hz local
     * updates while a 460x460 update remains bounded by the 48 MHz QSPI bus. */
    const lvgl_port_display_cfg_t display_config = {
        .io_handle = s_display.io,
        .panel_handle = s_display.panel,
        .buffer_size = config->buffer_size,
        .double_buffer = config->double_buffer,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rounder_cb = co5300_rounder_cb,
        .te_gpio_num = BSP_LCD_TE,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = BSP_LCD_BIGENDIAN ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma = config->flags.buff_dma,
            .buff_spiram = config->flags.buff_spiram,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = false,
#endif
            .sw_rotate = config->flags.sw_rotate,
            .direct_mode = config->flags.direct_mode,
#if CONFIG_BSP_LCD_TE_SYNC
            .te_sync = true,
#endif
        },
    };
    lv_display_t *display = lvgl_port_add_disp(&display_config);
    if (display == NULL) {
        return NULL;
    }

    /* The init table has already enabled TE and Display-On at brightness 0.
     * Replace unknown panel GRAM with a complete black default screen before
     * restoring the saved visible level. */
    if (!bsp_display_lock(0)) {
        lvgl_port_remove_disp(display);
        return NULL;
    }
    lv_obj_t *screen = lv_display_get_screen_active(display);
    if (screen != NULL) {
        /* The default LVGL theme may style a newly-created root screen light
         * gray. Give the screen an explicit opaque black local style while
         * WRDISBV is still zero so the first visible frame cannot flash gray. */
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_invalidate(screen);
    }
    lv_refr_now(display);
    bsp_display_unlock();

    if (brightness_hw_write(s_brightness_percent) != ESP_OK) {
        lvgl_port_remove_disp(display);
        return NULL;
    }
    return display;
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
#if CONFIG_BSP_LCD_DIRECT_MODE
        /* Optional full-frame DIRECT path for applications that require a
         * persistent framebuffer. The normal PARTIAL path below has lower
         * latency and pipelines rendering with QSPI DMA. */
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = false,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = false,
            .direct_mode = true,
        },
#else
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
#if CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
        .double_buffer = true,
#else
        .double_buffer = false,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .direct_mode = false,
        },
#endif
    };
    /* A 1 ms LVGL timebase keeps animation phases and interrupt-driven touch
     * handling responsive; priority 5 keeps UI work ahead of normal factory
     * tasks without outranking system-critical services. */
    config.lvgl_port_cfg.timer_period_ms = 1;
    config.lvgl_port_cfg.task_priority = 5;
    config.lvgl_port_cfg.task_affinity = 1;
    return bsp_display_start_with_config(&config);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *config)
{
    if (config == NULL || s_lvgl_initialized) {
        return NULL;
    }
    if (lvgl_port_init(&config->lvgl_port_cfg) != ESP_OK) {
        return NULL;
    }
    s_lvgl_initialized = true;
    s_lvgl_display = display_lvgl_init(config);
    if (s_lvgl_display == NULL) {
        bsp_display_stop();
        return NULL;
    }
    /* Touch is optional: during EVT a loose touch FPC must not keep the
     * screen dark, so a touch failure only disables the input device. */
    if (bsp_touch_new(NULL, &s_touch) == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_config = {
            .disp = s_lvgl_display,
            .handle = s_touch,
        };
        s_lvgl_touch = lvgl_port_add_touch(&touch_config);
        if (s_lvgl_touch == NULL) {
            ESP_LOGW(TAG, "touch registration failed, continuing without touch");
        } else {
            s_lvgl_touch_suspended = false;
            s_lvgl_touch_read_cb = NULL;
        }
    } else {
        s_touch = NULL;
        ESP_LOGW(TAG, "touch init failed, continuing without touch");
    }
    return s_lvgl_display;
}

esp_err_t bsp_display_stop(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_lvgl_touch != NULL) {
        const esp_err_t error = lvgl_port_remove_touch(s_lvgl_touch);
        /* Teardown is terminal. A removal failure may leave LVGL internals
         * allocated, but the BSP must never expose a handle whose hardware is
         * about to be deinitialized and unpowered. */
        s_lvgl_touch = NULL;
        s_lvgl_touch_suspended = false;
        s_lvgl_touch_read_cb = NULL;
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "LVGL touch removal failed: %s",
                     esp_err_to_name(error));
            first_error = error;
        }
    }
    if (s_lvgl_display != NULL) {
        const esp_err_t error = lvgl_port_remove_disp(s_lvgl_display);
        s_lvgl_display = NULL;
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "LVGL display removal failed: %s",
                     esp_err_to_name(error));
            if (first_error == ESP_OK) {
                first_error = error;
            }
        }
    }
    if (s_lvgl_initialized && s_lvgl_touch == NULL &&
            s_lvgl_display == NULL) {
        const esp_err_t error = lvgl_port_deinit();
        if (error == ESP_OK) {
            s_lvgl_initialized = false;
        } else {
            ESP_LOGE(TAG, "LVGL port deinit failed: %s",
                     esp_err_to_name(error));
            if (first_error == ESP_OK) {
                first_error = error;
            }
        }
    }

    /* Power safety is not conditional on LVGL cleanup succeeding. A failed
     * remove may leak LVGL state until reboot, but every BSP handle above is
     * invalidated before panel/touch objects and rails are torn down. */
    const esp_err_t touch_error = bsp_touch_delete();
    if (first_error == ESP_OK && touch_error != ESP_OK) {
        first_error = touch_error;
    }
    bsp_display_delete();
    return first_error;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return s_lvgl_touch;
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

void bsp_display_rotate(lv_display_t *display, lv_display_rotation_t rotation)
{
    /* Touch mapping is deliberately left untouched: LVGL 9.5 rotates pointer
     * input in the core (lv_display_rotate_point() from lv_indev.c) and
     * esp_lvgl_port feeds raw panel coordinates, so changing the touch
     * driver's swap/mirror flags here would rotate touches twice.
     * Display rotation itself is applied to the panel MADCTL by esp_lvgl_port
     * (sw_rotate is disabled in the BSP default config).
     * Note: at 90/180 degrees the active GRAM window is row/column asymmetric
     * (column offset 10, row offset 0), so a 10-20 px shift is possible and
     * must be measured during EVT. */
    lv_display_set_rotation(display, rotation);
}

/* Serialize every public panel power transition with taskLVGL. The port lock
 * is recursive, so these APIs remain safe when an application already owns
 * the BSP display lock. Raw-panel users have no LVGL task and need no lock. */
static esp_err_t display_transition_lock(bool *locked)
{
    *locked = false;
    if (s_lvgl_display != NULL) {
        if (!bsp_display_lock(0)) {
            return ESP_ERR_TIMEOUT;
        }
        *locked = true;
    }
    return ESP_OK;
}

static void display_transition_unlock(bool locked)
{
    if (locked) {
        bsp_display_unlock();
    }
}

static esp_err_t display_te_sync_set_locked(bool enable)
{
#if CONFIG_BSP_LCD_TE_SYNC
    if (s_lvgl_display != NULL) {
        const esp_err_t error =
            lvgl_port_display_te_sync_enable(s_lvgl_display, enable);
        if (error == ESP_ERR_INVALID_STATE ||
                error == ESP_ERR_NOT_SUPPORTED) {
            /* Match display bring-up semantics: if the TE ISR/semaphore was
             * unavailable (or this LVGL major has no runtime gate), keep the
             * panel usable with unsynchronized transfers. */
            ESP_LOGW(TAG, "TE sync unavailable; continuing unsynchronized");
            return ESP_OK;
        }
        return error;
    }
#else
    (void)enable;
#endif
    return ESP_OK;
}

static void display_lvgl_suspend_locked(void)
{
    if (s_lvgl_display != NULL && !s_lvgl_invalidation_suspended) {
        /* Finish invalid areas already accepted by LVGL while TE is still
         * running. Disabling invalidation only blocks future areas; it does
         * not discard areas already queued for the refresh timer. */
        /* Refresh timers still consume areas accepted before an outer caller
         * disabled invalidation, so drain unconditionally before sleep. */
        lv_refr_now(s_lvgl_display);
        lv_display_enable_invalidation(s_lvgl_display, false);
        s_lvgl_invalidation_suspended = true;
    }
}

/* Restore the pre-transition invalidation state when sleep entry fails. */
static void display_lvgl_rollback_locked(void)
{
    if (s_lvgl_display == NULL || !s_lvgl_invalidation_suspended) {
        return;
    }
    /* LVGL invalidation enable is a nesting counter, not an absolute bool.
     * Balance exactly the one disable performed by this transition. */
    lv_display_enable_invalidation(s_lvgl_display, true);
    if (lv_display_is_invalidation_enabled(s_lvgl_display)) {
        lv_obj_t *screen = lv_display_get_screen_active(s_lvgl_display);
        if (screen != NULL) {
            lv_obj_invalidate(screen);
        }
        lv_refr_now(s_lvgl_display);
    }
    s_lvgl_invalidation_suspended = false;
}

/* A successful wake releases this BSP transition's invalidation freeze. When
 * no outer caller freeze remains, replace potentially stale panel GRAM before
 * Display-On. lv_refr_now() uses the port's normal flush-wait protocol; this
 * code never touches its private transfer semaphore. */
static bool display_lvgl_resume_locked(void)
{
    bool redraw_completed = true;
    if (s_lvgl_display != NULL) {
        if (s_lvgl_invalidation_suspended) {
            /* Balance only this BSP transition's disable; a caller's outer
             * invalidation freeze remains intact until that caller releases
             * it, preserving LVGL's nesting contract. */
            lv_display_enable_invalidation(s_lvgl_display, true);
        }
        if (lv_display_is_invalidation_enabled(s_lvgl_display)) {
            lv_obj_t *screen = lv_display_get_screen_active(s_lvgl_display);
            if (screen != NULL) {
                lv_obj_invalidate(screen);
            } else {
                redraw_completed = false;
            }
            if (redraw_completed) {
                lv_refr_now(s_lvgl_display);
            }
        } else {
            /* An application-owned outer invalidation freeze is still active.
             * Normal sleep retains GRAM, but deep wake must remain optically
             * dark until a complete redraw can run. */
            redraw_completed = false;
        }
    }
    s_lvgl_invalidation_suspended = false;
    return redraw_completed;
}

/* Event-mode touch IRQs can already be queued when the controller enters
 * sleep. Disable the LVGL input device while holding the same recursive lock
 * used by taskLVGL, so a delayed event can never issue I2C to a sleeping
 * CST820. Reset also clears a pressed/scroll target before power transition. */
static void display_lvgl_touch_suspended_read_cb(lv_indev_t *indev,
                                                  lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
}

static esp_err_t display_lvgl_touch_suspend_locked(void)
{
    if (s_lvgl_touch != NULL && !s_lvgl_touch_suspended) {
        s_lvgl_touch_read_cb = lv_indev_get_read_cb(s_lvgl_touch);
        ESP_RETURN_ON_FALSE(s_lvgl_touch_read_cb != NULL,
                            ESP_ERR_INVALID_STATE, TAG,
                            "LVGL touch has no read callback");
        /* taskLVGL and this transition share the recursive BSP lock. Any
         * already-queued event therefore reaches this safe callback only
         * after the hardware controller has entered sleep. */
        lv_indev_set_read_cb(s_lvgl_touch,
                             display_lvgl_touch_suspended_read_cb);
        lv_indev_reset(s_lvgl_touch, NULL);
        s_lvgl_touch_suspended = true;
    }
    return ESP_OK;
}

/* Ignore a finger that remained down across sleep until it is released. This
 * prevents a stale PRESSED state from becoming a click immediately on wake. */
static esp_err_t display_lvgl_touch_resume_locked(void)
{
    if (s_lvgl_touch != NULL && s_lvgl_touch_suspended) {
        ESP_RETURN_ON_FALSE(s_lvgl_touch_read_cb != NULL,
                            ESP_ERR_INVALID_STATE, TAG,
                            "LVGL touch saved callback is missing");

        /* wait_release must only be armed when a finger is physically still
         * down. In EVENT mode arming it unconditionally would consume the
         * first real press after a no-finger wake. */
        bool pressed = false;
        if (s_touch != NULL) {
            uint8_t point_count = 0;
            esp_lcd_touch_point_data_t point = {0};
            ESP_RETURN_ON_ERROR(esp_lcd_touch_read_data(s_touch), TAG,
                                "touch state read after wake failed");
            ESP_RETURN_ON_ERROR(
                esp_lcd_touch_get_data(s_touch, &point, &point_count, 1), TAG,
                "touch state decode after wake failed");
            pressed = point_count > 0;
        }
        if (pressed) {
            lv_indev_wait_release(s_lvgl_touch);
        }
        lv_indev_set_read_cb(s_lvgl_touch, s_lvgl_touch_read_cb);
        s_lvgl_touch_read_cb = NULL;
        s_lvgl_touch_suspended = false;
    }
    return ESP_OK;
}

/* Caller owns the recursive LVGL lock when one exists. The first tx_param
 * issued by backlight_off() after refresh submission stops is documented by
 * IDF to wait for all queued color transactions, so it is also the DMA drain
 * barrier without reaching into esp_lvgl_port internals. */
static esp_err_t display_enter_sleep_panel_locked(void)
{
    if (s_deep_standby) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_panel_sleeping) {
        return ESP_OK;
    }

    display_lvgl_suspend_locked();

    esp_err_t error = display_te_sync_set_locked(false);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "display TE sync disable failed: %s",
                 esp_err_to_name(error));
        display_lvgl_rollback_locked();
        return error;
    }

    error = display_backlight_off_locked();
    if (error != ESP_OK) {
        /* SLPIN was not attempted. Restore the active path best-effort while
         * retaining the first failure as the API result. */
        const esp_err_t te_error = display_te_sync_set_locked(true);
        if (te_error != ESP_OK) {
            ESP_LOGE(TAG, "display TE sync rollback failed: %s",
                     esp_err_to_name(te_error));
        }
        display_lvgl_rollback_locked();
        const esp_err_t light_error = display_backlight_on_locked();
        if (light_error != ESP_OK) {
            ESP_LOGE(TAG, "display-on rollback failed: %s",
                     esp_err_to_name(light_error));
        }
        return error;
    }

    error = esp_lcd_panel_io_tx_param(
                s_display.io, CO5300_QSPI_WRITE_CMD(LCD_CMD_SLPIN),
                NULL, 0);
    /* Once 0x10 was attempted, an IO error cannot prove whether the panel
     * accepted it. Observe the complete transition window and leave the
     * state frozen/dark so the public caller can recover through SLPOUT. */
    vTaskDelay(pdMS_TO_TICKS(CO5300_SLEEP_TRANSITION_MS));
    s_panel_sleeping = true;
    return error;
}

static esp_err_t display_enter_touch_sleep_locked(void)
{
    if (s_touch == NULL || s_touch_sleeping) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(display_lvgl_touch_suspend_locked(), TAG,
                        "LVGL touch suspend failed");
    const esp_err_t error = esp_lcd_touch_enter_sleep(s_touch);
    if (error == ESP_OK || error == ESP_ERR_NOT_SUPPORTED) {
        /* NOT_SUPPORTED is a successful no-op for the BSP contract and must
         * still be remembered so a repeated enter remains idempotent. */
        s_touch_sleeping = true;
        return ESP_OK;
    }
    /* The controller state is unknown after a failed sleep command. Keep LVGL
     * reads blocked and force the normal exit path to reset it before use. */
    s_touch_sleeping = true;
    return error;
}

/* Prepare an ordinary SLPIN wake but keep Display-Off and LVGL frozen. This
 * lets the full variant reset the CST820 before any UI/input processing can
 * resume, and lets the caller redraw before exposing stale GRAM. */
static esp_err_t display_prepare_sleep_exit_locked(void)
{
    if (s_deep_standby) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_panel_sleeping) {
        return ESP_OK;
    }

    esp_err_t error = esp_lcd_panel_io_tx_param(
                          s_display.io,
                          CO5300_QSPI_WRITE_CMD(LCD_CMD_SLPOUT), NULL, 0);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(CO5300_SLEEP_TRANSITION_MS));

    error = display_te_sync_set_locked(true);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "display TE sync restore failed: %s",
                 esp_err_to_name(error));
        return error;
    }
    return brightness_hw_write(s_brightness_percent);
}

static esp_err_t display_exit_sleep_locked(bool wake_touch)
{
    const bool panel_was_sleeping = s_panel_sleeping;
    const bool force_active_path = s_recovery_required &&
                                   !panel_was_sleeping;
    esp_err_t error = display_prepare_sleep_exit_locked();
    if (error != ESP_OK) {
        return error;
    }

    if (wake_touch && s_touch != NULL && s_touch_sleeping) {
        error = esp_lcd_touch_exit_sleep(s_touch);
        if (error != ESP_OK && error != ESP_ERR_NOT_SUPPORTED) {
            return error;
        }
    }

    if (force_active_path) {
        /* A failed Display-Off/brightness command may have been accepted
         * before its transport reported failure. Reassert the entire active
         * path; doing only a no-op wake when panel_sleeping=false would leave
         * an uncertain panel permanently dark. */
        error = display_te_sync_set_locked(true);
        if (error != ESP_OK) {
            return error;
        }
        (void)display_lvgl_resume_locked();
        error = brightness_hw_write(s_brightness_percent);
        if (error == ESP_OK) {
            error = esp_lcd_panel_disp_on_off(s_display.panel, true);
        }
        if (error != ESP_OK) {
            display_lvgl_suspend_locked();
            return error;
        }
    }

    if (panel_was_sleeping) {
        (void)display_lvgl_resume_locked();
        /* tx_param inside disp_on_off drains the redraw DMA before exposing
         * the panel. If Display-On fails, freeze invalidation again so a
         * retry cannot race new frames against the unfinished transition. */
        error = esp_lcd_panel_disp_on_off(s_display.panel, true);
        if (error != ESP_OK) {
            display_lvgl_suspend_locked();
            return error;
        }
        s_panel_sleeping = false;
    }
    if (wake_touch) {
        ESP_RETURN_ON_ERROR(display_lvgl_touch_resume_locked(), TAG,
                            "LVGL touch resume failed");
        s_touch_sleeping = false;
    }
    return ESP_OK;
}

esp_err_t bsp_display_enter_sleep_panel(void)
{
    bool locked = false;
    bool transition_started = false;
    esp_err_t error = display_transition_lock(&locked);
    if (error == ESP_OK && s_display.panel == NULL) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK && s_recovery_required) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK && s_deep_standby) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        transition_started = true;
        error = display_enter_sleep_panel_locked();
    }
    if (error != ESP_OK && transition_started) {
        s_recovery_required = true;
        const esp_err_t recovery_error = display_exit_sleep_locked(false);
        s_recovery_required = recovery_error != ESP_OK;
        if (recovery_error != ESP_OK) {
            ESP_LOGE(TAG, "panel sleep-entry recovery failed: %s",
                     esp_err_to_name(recovery_error));
        }
    }
    display_transition_unlock(locked);
    return error;
}

esp_err_t bsp_display_exit_sleep_panel(void)
{
    bool locked = false;
    bool wake_started = false;
    esp_err_t error = display_transition_lock(&locked);
    if (error == ESP_OK && s_display.panel == NULL) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        wake_started = true;
        error = display_exit_sleep_locked(false);
    }
    if (error != ESP_OK && wake_started && !s_deep_standby) {
        s_recovery_required = true;
    } else if (error == ESP_OK && !s_touch_sleeping &&
               !s_lvgl_touch_suspended) {
        s_recovery_required = false;
    }
    display_transition_unlock(locked);
    return error;
}

esp_err_t bsp_display_enter_sleep(void)
{
    bool locked = false;
    bool transition_started = false;
    esp_err_t error = display_transition_lock(&locked);
    if (error == ESP_OK && s_display.panel == NULL) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK && s_deep_standby) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK && s_recovery_required) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        transition_started = true;
        /* Block queued/event-driven LVGL reads before changing either
         * hardware device. The official esp_lvgl_port component remains
         * untouched; this BSP only swaps the public LVGL indev callback. */
        error = display_lvgl_touch_suspend_locked();
    }
    if (error == ESP_OK) {
        error = display_enter_sleep_panel_locked();
    }
    if (error == ESP_OK) {
        error = display_enter_touch_sleep_locked();
    }
    if (error != ESP_OK && transition_started && !s_deep_standby) {
        /* A failed CST820 sleep command leaves its state unknown, while a
         * panel-entry failure may leave only the LVGL callback suspended.
         * The normal wake path safely repairs both cases; retain the entry
         * failure as the caller-visible result. */
        s_recovery_required = true;
        const esp_err_t recovery_error = display_exit_sleep_locked(true);
        s_recovery_required = recovery_error != ESP_OK;
        if (recovery_error != ESP_OK) {
            ESP_LOGE(TAG, "sleep-entry recovery failed: %s",
                     esp_err_to_name(recovery_error));
        }
    }
    display_transition_unlock(locked);
    return error;
}

esp_err_t bsp_display_exit_sleep(void)
{
    bool locked = false;
    bool wake_started = false;
    esp_err_t error = display_transition_lock(&locked);
    if (error == ESP_OK && s_display.panel == NULL) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        wake_started = true;
        error = display_exit_sleep_locked(true);
    }
    if (error == ESP_OK) {
        s_recovery_required = false;
    } else if (wake_started && !s_deep_standby) {
        s_recovery_required = true;
    }
    display_transition_unlock(locked);
    return error;
}

esp_err_t bsp_display_enter_deep_standby(void)
{
    bool locked = false;
    bool recover_uncertain_deep_entry = false;
    esp_err_t error = display_transition_lock(&locked);
    if (error == ESP_OK && s_display.panel == NULL) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK && s_recovery_required) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK && !s_deep_standby) {
        error = display_lvgl_touch_suspend_locked();
        if (error == ESP_OK) {
            error = display_enter_sleep_panel_locked();
        }
        if (error == ESP_OK) {
            error = display_enter_touch_sleep_locked();
        }
        if (error == ESP_OK) {
            const uint8_t parameter = CO5300_DEEP_STANDBY_PARAMETER;
            error = esp_lcd_panel_io_tx_param(
                        s_display.io,
                        CO5300_QSPI_WRITE_CMD(CO5300_CMD_DEEP_STANDBY_ON),
                        &parameter, sizeof(parameter));
            /* A transport error cannot prove whether CO5300 accepted 0x4F.
             * Treat the panel as deep once the command was attempted and
             * force the reset/init recovery path before it can be exposed. */
            s_deep_standby = true;
            recover_uncertain_deep_entry = error != ESP_OK;
            /* Close the interval between releasing the transition lock and
             * the forced reset/init recovery below: no concurrent enter may
             * report this uncertain hardware state as a successful deep
             * standby. */
            s_recovery_required = error != ESP_OK;
        } else {
            s_recovery_required = true;
            const esp_err_t recovery_error = display_exit_sleep_locked(true);
            s_recovery_required = recovery_error != ESP_OK;
            if (recovery_error != ESP_OK) {
                ESP_LOGE(TAG, "deep-entry rollback failed: %s",
                         esp_err_to_name(recovery_error));
            }
        }
    }
    display_transition_unlock(locked);

    if (recover_uncertain_deep_entry) {
        const esp_err_t recovery_error = bsp_display_exit_deep_standby();
        if (recovery_error != ESP_OK) {
            ESP_LOGE(TAG, "uncertain deep-entry recovery failed: %s",
                     esp_err_to_name(recovery_error));
        }
    }
    return error;
}

esp_err_t bsp_display_exit_deep_standby(void)
{
    bool locked = false;
    bool wake_started = false;
    esp_err_t error = display_transition_lock(&locked);
    if (error == ESP_OK && s_display.panel == NULL) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK && !s_deep_standby && s_panel_sleeping) {
        /* A normal SLPIN sleep must be paired with the normal wake API;
         * silently accepting a deep-wake request would leave the panel dark. */
        error = ESP_ERR_INVALID_STATE;
    }
    if (error != ESP_OK || !s_deep_standby) {
        display_transition_unlock(locked);
        return error;
    }
    wake_started = true;

    /* Unlike normal SLPIN entry, deep wake resets RESX without first sending
     * a panel command. Use the documented no-command tx_param form as an
     * explicit barrier so no queued color DMA can still target the panel
     * while reset is asserted. */
    error = esp_lcd_panel_io_tx_param(s_display.io, -1, NULL, 0);
    if (error == ESP_OK) {
        error = esp_lcd_panel_reset(s_display.panel);
    }
    if (error == ESP_OK) {
        error = esp_lcd_panel_init(s_display.panel);
    }
    if (error == ESP_OK) {
        error = esp_lcd_panel_set_gap(s_display.panel,
                                      BSP_LCD_X_GAP, BSP_LCD_Y_GAP);
    }
    if (error == ESP_OK && s_touch != NULL && s_touch_sleeping) {
        const esp_err_t touch_error = esp_lcd_touch_exit_sleep(s_touch);
        if (touch_error != ESP_OK && touch_error != ESP_ERR_NOT_SUPPORTED) {
            error = touch_error;
        }
    }
    if (error == ESP_OK) {
        error = display_te_sync_set_locked(true);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "display TE sync restore failed: %s",
                     esp_err_to_name(error));
        }
    }
    if (error == ESP_OK) {
        /* panel_init() leaves Display-On at brightness 0. A caller-owned
         * invalidation freeze can prevent the required full redraw; in that
         * case keep the panel dark and preserve a retryable deep state. */
        if (!display_lvgl_resume_locked()) {
            display_lvgl_suspend_locked();
            error = ESP_ERR_INVALID_STATE;
            ESP_LOGE(TAG, "deep wake redraw is still externally frozen");
        }
    }
    if (error == ESP_OK) {
        error = display_lvgl_touch_resume_locked();
        if (error != ESP_OK) {
            display_lvgl_suspend_locked();
            ESP_LOGE(TAG, "LVGL touch resume after deep wake failed: %s",
                     esp_err_to_name(error));
        }
    }
    if (error == ESP_OK) {
        error = brightness_hw_write(s_brightness_percent);
        if (error != ESP_OK) {
            /* Keep both producers quiescent if the final optical commit
             * fails. A later deep-wake retry will reset/init and redraw. */
            const esp_err_t dark_error = brightness_hw_write(0);
            if (dark_error != ESP_OK) {
                ESP_LOGE(TAG, "deep-wake dark rollback failed: %s",
                         esp_err_to_name(dark_error));
            }
            const esp_err_t touch_error =
                display_lvgl_touch_suspend_locked();
            if (touch_error != ESP_OK) {
                ESP_LOGE(TAG, "deep-wake touch rollback failed: %s",
                         esp_err_to_name(touch_error));
            }
            display_lvgl_suspend_locked();
        }
    }
    if (error == ESP_OK) {
        /* Clear the retry state only after panel redraw, touch restoration
         * and brightness commit have all succeeded. */
        s_panel_sleeping = false;
        s_touch_sleeping = false;
        s_deep_standby = false;
    }

    if (wake_started) {
        s_recovery_required = error != ESP_OK;
    }

    display_transition_unlock(locked);
    return error;
}
#endif

/* Application-level accessor for direct panel rendering (e.g. video).
 * This is intentionally read-only and does not change the panel state
 * managed by the BSP/LVGL port. */
esp_lcd_panel_handle_t bsp_display_get_panel_handle(void)
{
    return s_display.panel;
}
