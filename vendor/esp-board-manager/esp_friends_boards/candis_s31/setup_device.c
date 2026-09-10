/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "dev_power_ctrl.h"
#include "esp_board_device.h"
#include "esp_board_entry.h"
#include "esp_board_extra_func_entry.h"
#include "esp_board_periph.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gen_board_device_custom.h"
#include "periph_gpio.h"
#include "rx8130ce.h"
#include "tg28_sw.h"

/* Optional chip driver headers: guard the include and the matching weak
 * factory body together so a project or amend built without that driver
 * component still compiles and links. This is the weak + __has_include
 * convention from docs/programming-guide/board-directory (reference:
 * esp32_s3_korvo_2_3/setup_device.c). */
#if __has_include(<esp_lcd_co5300.h>)
#define HAS_CO5300  1
#include "esp_lcd_co5300.h"
#endif  /* __has_include(<esp_lcd_co5300.h>) */
#if __has_include(<esp_lcd_touch_cst820.h>)
#define HAS_CST820  1
#include "esp_lcd_touch_cst820.h"
#endif  /* __has_include(<esp_lcd_touch_cst820.h>) */

static const char *TAG = "CANDIS_S31_SETUP";

#if defined(HAS_CO5300)
static const co5300_lcd_init_cmd_t s_panel_init[] = {
    {0xFE, (uint8_t[]) {0x00}, 1, 0},
    {0xC4, (uint8_t[]) {0x80}, 1, 0},
    {0x3A, (uint8_t[]) {0x55}, 1, 0},
    {0x35, (uint8_t[]) {0x00}, 1, 0},
    {0x53, (uint8_t[]) {0x20}, 1, 0},
    /* Keep the panel optically dark until the first application frame replaces
     * unknown GRAM. The display is installed upside down, so the YAML mirror
     * flags and this shared factory path must stay in agreement.
     * 0x63 (WRHBMDISBV) stays at 0xFF because it only applies in HBM mode,
     * which this board never enables. */
    {0x51, (uint8_t[]) {0x00}, 1, 0},
    {0x63, (uint8_t[]) {0xFF}, 1, 0},
    {0x2A, (uint8_t[]) {0x00, 0x0A, 0x01, 0xD5}, 4, 0},
    {0x2B, (uint8_t[]) {0x00, 0x00, 0x01, 0xCB}, 4, 0},
    {0x11, NULL, 0, 60},
    {0x29, NULL, 0, 0},
};

static const co5300_vendor_config_t s_panel_vendor_config = {
    .init_cmds                = s_panel_init,
    .init_cmds_size           = sizeof(s_panel_init) / sizeof(s_panel_init[0]),
    .flags.use_qspi_interface = 1,
};

/* Weak hook: an amend directory can override it with a strong symbol. */
__attribute__((weak)) esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                                          const esp_lcd_panel_dev_config_t *panel_config,
                                                          esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t config = {0};
    memcpy(&config, panel_config, sizeof(config));
    config.vendor_config = (void *)&s_panel_vendor_config;

    esp_err_t ret = esp_lcd_new_panel_co5300(io, &config, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create CO5300 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    /* The supplier's active 460-pixel window starts at column 10. */
    ret = esp_lcd_panel_set_gap(*ret_panel, 10, 0);
    if (ret != ESP_OK) {
        esp_lcd_panel_del(*ret_panel);
        *ret_panel = NULL;
    }
    return ret;
}
#endif  /* defined(HAS_CO5300) */

#if defined(HAS_CST820)
/* Weak hook: an amend directory can override it with a strong symbol. */
__attribute__((weak)) esp_err_t lcd_touch_factory_entry_t(const esp_lcd_panel_io_handle_t io,
                                                          const esp_lcd_touch_config_t *config,
                                                          esp_lcd_touch_handle_t *ret_touch)
{
    return esp_lcd_touch_new_i2c_cst820(io, config, ret_touch);
}
#endif  /* defined(HAS_CST820) */

static int custom_ref_peripheral(const char *name, void **handle)
{
    int ret = esp_board_periph_ref_handle(name, handle);
    if (ret != 0 || *handle == NULL) {
        ESP_LOGE(TAG, "Failed to reference peripheral %s: %d", name, ret);
        return ret != 0 ? ret : ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void custom_unref_peripherals(const char *const *names, uint8_t count)
{
    for (uint8_t index = 0; index < count; ++index) {
        if (names[index] != NULL) {
            esp_board_periph_unref_handle(names[index]);
        }
    }
}

static int pmic_init(void *cfg, int cfg_size, void **device_handle)
{
    if (cfg == NULL || device_handle == NULL ||
        cfg_size != sizeof(dev_custom_pmic_config_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    const dev_custom_pmic_config_t *config = cfg;
    i2c_master_bus_handle_t bus = NULL;
    void *interrupt = NULL;

    int ret = custom_ref_peripheral(config->peripheral_names[0], (void **)&bus);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = custom_ref_peripheral(config->peripheral_names[1], &interrupt);
    if (ret != ESP_OK) {
        esp_board_periph_unref_handle(config->peripheral_names[0]);
        return ret;
    }

    const tg28_sw_config_t driver_config = {
        .device_address = config->i2c_address,
        .scl_speed_hz = config->frequency_hz,
    };
    ret = tg28_sw_create(bus, &driver_config, (tg28_sw_handle_t *)device_handle);
    if (ret == ESP_OK) {
        /* Clear latched interrupt status before enabling the power-key
         * IRQs, so stale events from the boot ROM or a previous reset do
         * not fire immediately (same order as the BSP). */
        uint8_t pending[3] = {0};
        ret = tg28_sw_get_and_clear_interrupts(
                  (tg28_sw_handle_t)*device_handle, pending);
    }
    if (ret == ESP_OK) {
        ret = tg28_sw_configure_power_key_interrupts(
                  (tg28_sw_handle_t)*device_handle, TG28_SW_POWER_KEY_IRQ_ALL);
    }
    if (ret == ESP_OK) {
        /* The board battery has no NTC resistor: the TS pin is the external
         * fixed input and its current source stays off. */
        ret = tg28_sw_set_ts_config((tg28_sw_handle_t)*device_handle,
                                    TG28_SW_TS_MODE_EXTERNAL_FIXED,
                                    TG28_SW_TS_CURRENT_SOURCE_OFF, 50);
    }
    if (ret != ESP_OK) {
        if (*device_handle != NULL) {
            tg28_sw_delete((tg28_sw_handle_t)*device_handle);
            *device_handle = NULL;
        }
        custom_unref_peripherals(config->peripheral_names, config->peripheral_count);
    }
    return ret;
}

static int pmic_deinit(void *device_handle)
{
    dev_custom_pmic_config_t *config = NULL;
    esp_board_device_get_config_by_handle(device_handle, (void **)&config);
    int ret = tg28_sw_delete((tg28_sw_handle_t)device_handle);
    if (config != NULL) {
        custom_unref_peripherals(config->peripheral_names, config->peripheral_count);
    }
    return ret;
}

static int rtc_init(void *cfg, int cfg_size, void **device_handle)
{
    if (cfg == NULL || device_handle == NULL ||
        cfg_size != sizeof(dev_custom_rtc_config_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    const dev_custom_rtc_config_t *config = cfg;
    i2c_master_bus_handle_t bus = NULL;
    void *interrupt = NULL;

    int ret = custom_ref_peripheral(config->peripheral_names[0], (void **)&bus);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = custom_ref_peripheral(config->peripheral_names[1], &interrupt);
    if (ret != ESP_OK) {
        esp_board_periph_unref_handle(config->peripheral_names[0]);
        return ret;
    }

    const rx8130ce_config_t driver_config = {
        .device_address = config->i2c_address,
        .scl_speed_hz = config->frequency_hz,
        /* Candis-S31 uses a primary backup cell: never charge it. */
        .backup_charge_enable = false,
        .backup_charge_cutoff = RX8130CE_CHARGE_CUTOFF_3_02V,
        .backup_voltage_low_detect = true,
    };
    ret = rx8130ce_create(bus, &driver_config, (rx8130ce_handle_t *)device_handle);
    if (ret != ESP_OK) {
        custom_unref_peripherals(config->peripheral_names, config->peripheral_count);
    }
    return ret;
}

static int rtc_deinit(void *device_handle)
{
    dev_custom_rtc_config_t *config = NULL;
    esp_board_device_get_config_by_handle(device_handle, (void **)&config);
    int ret = rx8130ce_delete((rx8130ce_handle_t)device_handle);
    if (config != NULL) {
        custom_unref_peripherals(config->peripheral_names, config->peripheral_count);
    }
    return ret;
}


typedef struct {
    tg28_sw_handle_t      pmic;
    i2c_master_bus_handle_t i2c_main;
    periph_gpio_handle_t *display_vbat;
    periph_gpio_handle_t *display_vci;
    periph_gpio_handle_t *sd_power;
    periph_gpio_handle_t *pa_control;
    gpio_num_t            camera_reset;
    gpio_num_t            camera_pwdn;
    bool                  audio_dac_on;
    bool                  audio_adc_on;
} candis_power_context_t;

/* The codec configuration uses the 8-bit write address 0x20, whereas
 * i2c_master_probe takes the 7-bit address. */
#define CANDIS_CODEC_I2C_ADDRESS 0x10

static esp_err_t set_gpio(periph_gpio_handle_t *handle, int level)
{
    return handle != NULL ? gpio_set_level(handle->gpio_num, level) : ESP_ERR_INVALID_ARG;
}

static esp_err_t set_regulator(tg28_sw_handle_t pmic, tg28_sw_regulator_t regulator,
                               uint16_t millivolts, bool enable)
{
    if (enable) {
        esp_err_t ret = tg28_sw_regulator_set_voltage(pmic, regulator, millivolts);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return tg28_sw_regulator_enable(pmic, regulator, enable);
}

static esp_err_t set_camera_safe_state(candis_power_context_t *context)
{
    esp_err_t ret = gpio_set_level(context->camera_pwdn, 1);
    if (ret == ESP_OK) {
        ret = gpio_set_level(context->camera_reset, 0);
    }
    return ret;
}

/* The codec answers on I2C only after its rail has settled. Poll the control
 * port instead of relying on a fixed delay, so the first register access of
 * the codec driver cannot race the power-up ramp. */
static esp_err_t wait_for_codec_ready(candis_power_context_t *context)
{
    if (context->i2c_main == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t deadline_us = esp_timer_get_time() + 200000;
    while (esp_timer_get_time() < deadline_us) {
        if (i2c_master_probe(context->i2c_main, CANDIS_CODEC_I2C_ADDRESS,
                             50) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGE(TAG, "codec did not answer at 0x10 within 200 ms");
    return ESP_ERR_TIMEOUT;
}
#define CANDIS_PIN_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define CANDIS_TOUCH_RESET_GPIO GPIO_NUM_17

/* Park peripheral signal pins as pure inputs before their rail is switched
 * off, so a pad left driving high cannot back-feed the sinking rail through
 * the peripheral's ESD/protection diodes.
 *
 * Caller contract, NOT a framework guarantee: Board Manager never calls
 * set_power(name, false) during device deinit, and the public
 * esp_board_device_power_ctrl(name, false) may be invoked at any time. The
 * application must stop or deinitialize the device before requesting power
 * off; otherwise this tri-states pins an active driver still owns (for
 * example the SPI/QSPI display bus, an I2S channel, or a mounted SDMMC
 * slot, including its internal pull-ups).
 *
 * Camera PWDN/RST are deliberately NOT parked: they are configured as
 * outputs only once in board_power_init, so parking them would leave
 * set_camera_safe_state() writing to input-mode pads on every later power
 * cycle; they keep driving their safe levels (PWDN high, RST low) instead.
 * Pins that are never outputs (display TE, SD card detect) carry no
 * back-feed risk and stay out of the lists. Pin numbers mirror
 * board_peripherals.yaml / board_devices.yaml and hardware/facts.md; keep
 * them in sync. */
static const gpio_num_t s_display_pins[] = {
    15, 10, 11, 13, 14, 9, 12,        /* RST, CS, SIO0-3, CLK */
};
static const gpio_num_t s_audio_pins[] = {
    35, 18, 19, 8, 44,               /* MCLK, BCLK, LRCK, DOUT, DIN */
};
static const gpio_num_t s_camera_pins[] = {
    46, 47, 48, 49, 50, 51, 52, 53,  /* D0-D7 */
    54, 55, 56, 57,                  /* PCLK, XCLK, VSYNC, HSYNC */
};
static const gpio_num_t s_sd_pins[] = {
    24, 25, 20, 21, 22, 23,          /* CLK, CMD, D0-D3 */
};

static esp_err_t park_pins_input(const gpio_num_t *pins, size_t count)
{
    uint64_t mask = 0;
    for (size_t index = 0; index < count; ++index) {
        mask |= 1ULL << pins[index];
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

static int board_power_deinit(void *context)
{
    candis_power_context_t *power = context;
    if (power->i2c_main != NULL) {
        esp_err_t error = esp_board_periph_unref_handle("i2c_main");
        if (error != ESP_OK) {
            return error;
        }
    }
    free(power);
    return ESP_OK;
}

static int board_power_init(const dev_power_ctrl_config_t *config, void **context)
{
    if (config == NULL || context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const dev_power_ctrl_custom_sub_config_t *custom = &config->sub_cfg.custom;
    const dev_custom_board_power_ctrl_custom_config_t *power_config = custom->user_cfg;
    if (power_config == NULL || custom->periph_count != 4) {
        return ESP_ERR_INVALID_ARG;
    }

    candis_power_context_t *power = calloc(1, sizeof(*power));
    if (power == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int ret = esp_board_device_get_handle("pmic", (void **)&power->pmic);
    if (ret == ESP_OK) {
        ret = esp_board_periph_ref_handle("i2c_main", (void **)&power->i2c_main);
    }
    if (ret == ESP_OK) {
        ret = esp_board_periph_get_handle(custom->periph_names[0],
                                          (void **)&power->display_vbat);
    }
    if (ret == ESP_OK) {
        ret = esp_board_periph_get_handle(custom->periph_names[1],
                                          (void **)&power->display_vci);
    }
    if (ret == ESP_OK) {
        ret = esp_board_periph_get_handle(custom->periph_names[2],
                                          (void **)&power->sd_power);
    }
    if (ret == ESP_OK) {
        ret = esp_board_periph_get_handle(custom->periph_names[3],
                                          (void **)&power->pa_control);
    }
    if (ret != ESP_OK) {
        board_power_deinit(power);
        return ret;
    }

    power->camera_reset = power_config->camera_reset_gpio;
    power->camera_pwdn = power_config->camera_pwdn_gpio;
    const gpio_config_t camera_gpio_config = {
        .pin_bit_mask = (1ULL << power->camera_reset) | (1ULL << power->camera_pwdn),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&camera_gpio_config);
    if (ret == ESP_OK) {
        ret = set_camera_safe_state(power);
    }
    if (ret != ESP_OK) {
        board_power_deinit(power);
        return ret;
    }

    *context = power;
    return ESP_OK;
}


static int board_power_set_display(candis_power_context_t *context, bool power_on)
{
    if (power_on) {
        esp_err_t ret = set_regulator(context->pmic, TG28_SW_ALDO1, 3300, true);
        if (ret == ESP_OK) {
            ret = set_gpio(context->display_vbat, 1);
        }
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            ret = set_gpio(context->display_vci, 1);
        }
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return ret;
    }

    esp_err_t ret = park_pins_input(s_display_pins, CANDIS_PIN_COUNT(s_display_pins));
    if (ret == ESP_OK) {
        ret = set_gpio(context->display_vci, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    if (ret == ESP_OK) {
        ret = set_gpio(context->display_vbat, 0);
    }
    if (ret == ESP_OK) {
        ret = set_regulator(context->pmic, TG28_SW_ALDO1, 3300, false);
    }
    return ret;
}

static int board_power_set_audio(candis_power_context_t *context,
                                 const char *device_name, bool power_on)
{
    const bool dac_on = strcmp(device_name, "audio_dac") == 0
                        ? power_on : context->audio_dac_on;
    const bool adc_on = strcmp(device_name, "audio_adc") == 0
                        ? power_on : context->audio_adc_on;
    const bool rail_on = dac_on || adc_on;
    esp_err_t ret = rail_on ? ESP_OK
                  : park_pins_input(s_audio_pins, CANDIS_PIN_COUNT(s_audio_pins));
    if (ret == ESP_OK) {
        ret = set_regulator(context->pmic, TG28_SW_ALDO3, 3300, rail_on);
    }
    if (ret == ESP_OK && rail_on) {
        vTaskDelay(pdMS_TO_TICKS(5) + 1);
        ret = wait_for_codec_ready(context);
    }
    if (ret == ESP_OK && !dac_on) {
        ret = set_gpio(context->pa_control, 0);
    }
    if (ret == ESP_OK) {
        context->audio_dac_on = dac_on;
        context->audio_adc_on = adc_on;
    }
    return ret;
}

static int board_power_set_camera(candis_power_context_t *context, bool power_on)
{
    esp_err_t ret = set_camera_safe_state(context);
    if (ret != ESP_OK) {
        return ret;
    }
    if (power_on) {
        ret = set_regulator(context->pmic, TG28_SW_BLDO1, 2800, true);
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1));
            ret = set_regulator(context->pmic, TG28_SW_ALDO4, 2800, true);
        }
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1));
            ret = set_regulator(context->pmic, TG28_SW_DCDC2, 1500, true);
        }
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        return ret;
    }
    ret = park_pins_input(s_camera_pins, CANDIS_PIN_COUNT(s_camera_pins));
    if (ret == ESP_OK) {
        ret = set_regulator(context->pmic, TG28_SW_DCDC2, 1500, false);
    }
    if (ret == ESP_OK) {
        ret = set_regulator(context->pmic, TG28_SW_ALDO4, 2800, false);
    }
    if (ret == ESP_OK) {
        ret = set_regulator(context->pmic, TG28_SW_BLDO1, 2800, false);
    }
    return ret;
}

static int board_power_set_touch(candis_power_context_t *context, bool power_on)
{
    if (!power_on) {
        esp_err_t ret = gpio_reset_pin(CANDIS_TOUCH_RESET_GPIO);
        if (ret != ESP_OK) {
            return ret;
        }
        return set_regulator(context->pmic, TG28_SW_ALDO2, 3300, false);
    }

    esp_err_t ret = set_regulator(context->pmic, TG28_SW_ALDO2, 3300, true);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Board Manager probes the configured I2C address before constructing the
     * CST820 driver. Wake the freshly powered controller first; the driver
     * repeats this reset after the probe when it takes ownership of the pin. */
    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << CANDIS_TOUCH_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&reset_config);
    if (ret == ESP_OK) {
        ret = gpio_set_level(CANDIS_TOUCH_RESET_GPIO, 0);
    }
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(5) + 1);
        ret = gpio_set_level(CANDIS_TOUCH_RESET_GPIO, 1);
    }
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100) + 1);
    }
    return ret;
}

static int board_power_set(void *context, const char *device_name, bool power_on)
{
    candis_power_context_t *power = context;
    if (power == NULL || device_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(device_name, "display_lcd") == 0) {
        return board_power_set_display(power, power_on);
    }
    if (strcmp(device_name, "lcd_touch") == 0) {
        return board_power_set_touch(power, power_on);
    }
    if (strcmp(device_name, "audio_dac") == 0 || strcmp(device_name, "audio_adc") == 0) {
        return board_power_set_audio(power, device_name, power_on);
    }
    if (strcmp(device_name, "camera") == 0) {
        return board_power_set_camera(power, power_on);
    }
    if (strcmp(device_name, "fs_sdcard") == 0) {
        int ret = power_on ? ESP_OK
                  : park_pins_input(s_sd_pins, CANDIS_PIN_COUNT(s_sd_pins));
        if (ret == ESP_OK) {
            ret = set_gpio(power->sd_power, power_on ? 0 : 1);
        }
        if (ret == ESP_OK && power_on) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return ret;
    }
    if (strcmp(device_name, "led_strip") == 0) {
        /* The DLDO1 pin is OTP-strapped as the DC1SW load switch, which
         * passes DCDC1 (3.3 V) straight through: use the switch API, no
         * voltage programming applies. It is OTP-off and must be enabled
         * before the WS2812E (U24) is driven. */
        return tg28_sw_switch_enable(power->pmic, TG28_SW_SWITCH_DC1SW, power_on);
    }

    ESP_LOGW(TAG, "No power sequence for device %s", device_name);
    return ESP_ERR_NOT_SUPPORTED;
}

static const dev_power_ctrl_custom_ops_t s_board_power_ops = {
    .init      = board_power_init,
    .deinit    = board_power_deinit,
    .set_power = board_power_set,
};

CUSTOM_DEVICE_IMPLEMENT(pmic, pmic_init, pmic_deinit);
CUSTOM_DEVICE_IMPLEMENT(rtc, rtc_init, rtc_deinit);
DEVICE_EXTRA_FUNC_REGISTER(board_power_ctrl, &s_board_power_ops);
