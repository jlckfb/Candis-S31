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
#include "esp_lv_adapter.h"
#endif

static const char *TAG = "candis_display";

/* EVT bring-up starts the panel at 30 % brightness and must never flash
 * 100 % on the first Display-On. The CO5300 brightness register (WRDISBV,
 * 0x51) takes percent * 255 / 100, so 30 % is 0x4C. The init sequence below
 * and the saved-level default here must stay in sync. The 0x63 (WRHBMDISBV)
 * init value only applies in HBM mode, which this board never enables. */
#define CO5300_FIRST_BRIGHTNESS_PERCENT  30
#define CO5300_FIRST_BRIGHTNESS_HW       0x4C

static bsp_lcd_handles_t s_display;
static bool s_spi_initialized;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_touch_io;
static bool s_deep_standby;
/* Last brightness chosen through bsp_display_brightness_set(); restored by
 * bsp_display_backlight_on() so a wake returns to the operator's level
 * instead of forcing 100 %. Defaults to the same 30 % the panel init
 * sequence programs, so first light never exceeds the EVT cap. */
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
static bool s_lvgl_initialized;
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
    /* WRDISBV capped at the EVT first-light level; 0x51 percent scaling is
     * percent * 255 / 100. bsp_display_brightness_set() owns later changes. */
    {0x51, (uint8_t[]){CO5300_FIRST_BRIGHTNESS_HW}, 1, 0},
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
    ESP_RETURN_ON_ERROR(brightness_hw_write(brightness_percent), TAG,
                        "brightness update failed");
    s_brightness_percent = (uint8_t)brightness_percent;
    return ESP_OK;
}

esp_err_t bsp_display_backlight_off(void)
{
    ESP_RETURN_ON_ERROR(brightness_hw_write(0), TAG,
                        "brightness update failed");
    return esp_lcd_panel_disp_on_off(s_display.panel, false);
}

esp_err_t bsp_display_backlight_on(void)
{
    /* Re-assert the saved level BEFORE Display-On: the panel must never
     * light at a stale value. First light therefore comes up at the 30 %
     * programmed by the init sequence, and a wake restores whatever the
     * operator last set through bsp_display_brightness_set(). */
    ESP_RETURN_ON_ERROR(brightness_hw_write(s_brightness_percent), TAG,
                        "brightness restore failed");
    return esp_lcd_panel_disp_on_off(s_display.panel, true);
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
    return first_error;
}

esp_lcd_touch_handle_t bsp_touch_get_handle(void)
{
    return s_touch;
}

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_display_t *display_lvgl_init(const bsp_display_cfg_t *config)
{
    const bsp_display_config_t panel_config = {
        .max_transfer_sz = (int)(config->buffer_size * sizeof(uint16_t)),
    };
    if (bsp_display_new(&panel_config, &s_display.panel, &s_display.io) != ESP_OK) {
        return NULL;
    }
    if (bsp_display_backlight_on() != ESP_OK) {
        return NULL;
    }

    /* Official esp_lvgl_adapter (>=0.5.2): QSPI/OTHER + PSRAM + TE sync.
     * The adapter owns the draw buffers, the CPU-to-DMA cache writeback
     * (the 0.5.2 fix that our manual msync patch used to replicate), the
     * TE-gated flush pacing and the LVGL task. CO5300 contract: 460x460,
     * RGB565 byte order handled by the panel driver, TE on GPIO16. */
#if CONFIG_BSP_LCD_TE_SYNC
    esp_lv_adapter_display_config_t display_config =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_TE_DEFAULT_CONFIG(
            s_display.panel, s_display.io,
            BSP_LCD_H_RES, BSP_LCD_V_RES,
            ESP_LV_ADAPTER_ROTATE_0,
            BSP_LCD_TE,
            CONFIG_BSP_LCD_PIXEL_CLOCK_MHZ * 1000000,
            4 /* QSPI data lines */,
            16 /* bits per pixel */);
#else
    esp_lv_adapter_display_config_t display_config =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
            s_display.panel, s_display.io,
            BSP_LCD_H_RES, BSP_LCD_V_RES,
            ESP_LV_ADAPTER_ROTATE_0);
#endif
    /* The TE profile selects TEAR_AVOID_MODE_TE_SYNC, which the adapter
     * hard-maps to LVGL RENDER_MODE_FULL with a single full-frame PSRAM
     * buffer: every invalidate merges into one TE-gated full redraw (see
     * display_manager_pick_render_mode). SPI/QSPI (IF_OTHER) exposes no
     * PARTIAL + TE double-buffer mode on this adapter version, so
     * smoothness comes from keeping invalidations sparse and local plus
     * PPA-accelerated fills/blends, not from partial redraws. */
    /* PPA route: the adapter's SW blend/fill acceleration, NOT LVGL's
     * native PPA draw unit. The native unit cannot compile on ESP32-S31:
     * lv_draw_ppa_private.h requires CONFIG_LV_DRAW_BUF_ALIGN ==
     * CONFIG_CACHE_L2_CACHE_LINE_SIZE, a Kconfig symbol that only exists
     * on ESP32-P4, and LVGL's esp.cmake links esp_driver_ppa only for
     * esp32p4. The adapter path is compile-clean here (its CMake adds
     * esp_driver_ppa for esp32s31) and the two paths are mutually
     * exclusive, so enable exactly this one. Needs
     * CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT == 1. */
    display_config.profile.enable_ppa_accel = true;
    return esp_lv_adapter_register_display(&display_config);
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t config = {
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
    return bsp_display_start_with_config(&config);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *config)
{
    if (config == NULL || s_lvgl_initialized) {
        return NULL;
    }
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    /* Keep the board-tuned LVGL task parameters: 1 ms timebase for
     * responsive touch, priority 5 below system-critical services.
     * task_core_id pins the LVGL worker to core 1; the net/NimBLE services
     * are pinned to core 0, so rendering and radio work do not preempt each
     * other. The adapter honors task_core_id through
     * xTaskCreatePinnedToCoreWithCaps. */
    adapter_cfg.tick_period_ms = 1;
    adapter_cfg.task_priority = 5;
    adapter_cfg.task_core_id = 1;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
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
        esp_lv_adapter_touch_config_t touch_cfg =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_lvgl_display, s_touch);
        s_lvgl_touch = esp_lv_adapter_register_touch(&touch_cfg);
        if (s_lvgl_touch == NULL) {
            ESP_LOGW(TAG, "touch registration failed, continuing without touch");
        }
    } else {
        s_touch = NULL;
        ESP_LOGW(TAG, "touch init failed, continuing without touch");
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        bsp_display_stop();
        return NULL;
    }
    return s_lvgl_display;
}

esp_err_t bsp_display_stop(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_lvgl_touch != NULL) {
        const esp_err_t error = esp_lv_adapter_unregister_touch(s_lvgl_touch);
        s_lvgl_touch = NULL;
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "LVGL touch removal failed: %s",
                     esp_err_to_name(error));
            first_error = error;
        }
    }
    if (s_lvgl_display != NULL) {
        const esp_err_t error = esp_lv_adapter_unregister_display(s_lvgl_display);
        s_lvgl_display = NULL;
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "LVGL display removal failed: %s",
                     esp_err_to_name(error));
            if (first_error == ESP_OK) {
                first_error = error;
            }
        }
    }
    if (s_lvgl_initialized) {
        const esp_err_t error = esp_lv_adapter_deinit();
        s_lvgl_initialized = false;
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "LVGL adapter deinit failed: %s",
                     esp_err_to_name(error));
            if (first_error == ESP_OK) {
                first_error = error;
            }
        }
    }

    /* Power safety is not conditional on LVGL cleanup succeeding. */
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
    /* BSP contract: 0 means "wait forever", the adapter uses -1. */
    const int32_t adapter_timeout = timeout_ms == 0 ? -1 : (int32_t)timeout_ms;
    return esp_lv_adapter_lock(adapter_timeout) == ESP_OK;
}

void bsp_display_unlock(void)
{
    esp_lv_adapter_unlock();
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

esp_err_t bsp_display_enter_sleep_panel(void)
{
    ESP_RETURN_ON_FALSE(s_display.panel != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display is not initialized");
    /* SLPIN stops the panel TE output; suspend the TE gate first so LVGL
     * flushes fall back to unsynchronized writes instead of timing out. */
    /* The official adapter owns the TE gate and bounds its wait; a panel
     * that stopped emitting TE (SLPIN) degrades to unsynchronized flushes
     * instead of hanging (adapter 0.5.2 fix). No manual gate toggle. */
    ESP_RETURN_ON_ERROR(bsp_display_backlight_off(), TAG, "display off failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_display.io,
                        CO5300_QSPI_WRITE_CMD(LCD_CMD_SLPIN),
                        NULL, 0), TAG, "display sleep-in failed");
    vTaskDelay(pdMS_TO_TICKS(CO5300_SLEEP_TRANSITION_MS));
    return ESP_OK;
}

esp_err_t bsp_display_exit_sleep_panel(void)
{
    ESP_RETURN_ON_FALSE(s_display.panel != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display is not initialized");
    ESP_RETURN_ON_FALSE(!s_deep_standby, ESP_ERR_INVALID_STATE, TAG,
                        "display is in deep standby");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_display.io,
                        CO5300_QSPI_WRITE_CMD(LCD_CMD_SLPOUT),
                        NULL, 0),
                        TAG, "display sleep-out failed");
    vTaskDelay(pdMS_TO_TICKS(CO5300_SLEEP_TRANSITION_MS));
    /* SLPOUT restarts TE; the adapter re-syncs on the next flush. */
    return bsp_display_backlight_on();
}

esp_err_t bsp_display_enter_sleep(void)
{
    /* Panel first, then the touch controller: a touch-sleep failure can no
     * longer skip the panel SLPIN (the old order left the panel awake with
     * the backlight off). The panel and the touch controller sit on
     * independent buses, so the relative order carries no hardware hazard. */
    ESP_RETURN_ON_ERROR(bsp_display_enter_sleep_panel(), TAG,
                        "panel sleep-in failed");
    if (s_touch != NULL) {
        const esp_err_t touch_error = esp_lcd_touch_enter_sleep(s_touch);
        if (touch_error != ESP_OK && touch_error != ESP_ERR_NOT_SUPPORTED) {
            return touch_error;
        }
    }
    return ESP_OK;
}

esp_err_t bsp_display_exit_sleep(void)
{
    ESP_RETURN_ON_ERROR(bsp_display_exit_sleep_panel(), TAG,
                        "panel sleep-out failed");
    if (s_touch != NULL) {
        const esp_err_t touch_error = esp_lcd_touch_exit_sleep(s_touch);
        if (touch_error != ESP_OK && touch_error != ESP_ERR_NOT_SUPPORTED) {
            return touch_error;
        }
    }
    return ESP_OK;
}

esp_err_t bsp_display_enter_deep_standby(void)
{
    ESP_RETURN_ON_FALSE(s_display.panel != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display is not initialized");
    if (s_deep_standby) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_display_enter_sleep(), TAG,
                        "display sleep-in before deep standby failed");
    const uint8_t parameter = CO5300_DEEP_STANDBY_PARAMETER;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_display.io,
                        CO5300_QSPI_WRITE_CMD(CO5300_CMD_DEEP_STANDBY_ON),
                        &parameter, sizeof(parameter)), TAG,
                        "display deep-standby command failed");
    s_deep_standby = true;
    return ESP_OK;
}

esp_err_t bsp_display_exit_deep_standby(void)
{
    ESP_RETURN_ON_FALSE(s_display.panel != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display is not initialized");
    if (!s_deep_standby) {
        return ESP_OK;
    }

    /* Cycle RESX through the driver's own reset routine (10ms low, 150ms
     * high) instead of hand-rolled 5ms/5ms timing: deep standby drops the
     * panel's internal state and the wake reset needs the same margins the
     * driver applies at first power-on. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_display.panel), TAG,
                        "display deep-wake reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_display.panel), TAG,
                        "display reinitialization failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_display.panel,
                        BSP_LCD_X_GAP, BSP_LCD_Y_GAP), TAG,
                        "display gap restore failed");
    if (s_touch != NULL) {
        const esp_err_t touch_error = esp_lcd_touch_exit_sleep(s_touch);
        if (touch_error != ESP_OK && touch_error != ESP_ERR_NOT_SUPPORTED) {
            return touch_error;
        }
    }
    s_deep_standby = false;
    /* The panel re-initialization above re-enabled TE output (0x35).
     * After the backlight, force one complete redraw so no stale GRAM
     * from before deep standby survives the wake. */
    if (s_lvgl_display != NULL && bsp_display_lock(0)) {
        lv_obj_invalidate(lv_screen_active());
        bsp_display_unlock();
        esp_lv_adapter_refresh_now(s_lvgl_display);
    }
    return bsp_display_backlight_on();
}
#endif
