/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "led_indicator_strips.h"

#include "bsp/candis_s31.h"

extern blink_step_t const *bsp_led_blink_defaults_lists[];

static const char *TAG = "candis_led";

/* The WS2812B is powered from the TG28 DLDO1 pin, which the TG28 confirmation
 * sheet V1.3 straps in SWITCH mode (DC1SW): the output passes DCDC1 (3.3V)
 * through directly, so the DLDO1 voltage register is inert and no voltage
 * programming applies. The rail is OFF after power-on (OTP default), so
 * software must open the switch explicitly before the LED is driven -
 * BSP_PMIC_SWITCH_DC1SW through bsp_pmic_switch_enable(), never the
 * bsp_pmic_regulator_* calls. */
static const led_strip_config_t s_strip_config = {
    .strip_gpio_num = BSP_LED_RGB_IO,
    .max_leds = 1,
    .led_model = LED_MODEL_WS2812,
    .flags.invert_out = false,
};

#if CONFIG_BSP_LED_RGB_BACKEND_RMT
static const led_strip_rmt_config_t s_rmt_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 10 * 1000 * 1000,
    .flags.with_dma = false,
};
#elif CONFIG_BSP_LED_RGB_BACKEND_SPI
static const led_strip_spi_config_t s_spi_config = {
    .spi_bus = SPI3_HOST,
    .flags.with_dma = true,
};
#else
#error "Select an RGB LED backend"
#endif

static led_indicator_strips_config_t s_rgb_config = {
    .led_strip_cfg = s_strip_config,
#if CONFIG_BSP_LED_RGB_BACKEND_RMT
    .led_strip_driver = LED_STRIP_RMT,
    .led_strip_rmt_cfg = s_rmt_config,
#else
    .led_strip_driver = LED_STRIP_SPI,
    .led_strip_spi_cfg = s_spi_config,
#endif
};

static const led_indicator_config_t s_indicator_config = {
    .blink_lists = bsp_led_blink_defaults_lists,
    .blink_list_num = BSP_LED_MAX,
};

esp_err_t bsp_led_indicator_create(led_indicator_handle_t led_array[],
                                   int *led_cnt, int led_array_size)
{
    ESP_RETURN_ON_FALSE(led_array != NULL && led_array_size >= BSP_LED_NUM,
                        ESP_ERR_INVALID_ARG, TAG, "LED array is too small");
    /* Open the DC1SW load switch that powers the RGB LED (OFF after boot). */
    ESP_RETURN_ON_ERROR(bsp_pmic_switch_enable(BSP_PMIC_SWITCH_DC1SW, true), TAG,
                        "RGB LED power switch enable failed");
    if (led_cnt != NULL) {
        *led_cnt = 0;
    }
    for (int index = 0; index < BSP_LED_NUM; ++index) {
        const esp_err_t led_error =
            led_indicator_new_strips_device(&s_indicator_config,
                                            &s_rgb_config,
                                            &led_array[index]);
        if (led_error != ESP_OK) {
            /* Roll back the DC1SW switch so a failed init does not leave
             * the RGB LED rail powered with no active consumer. */
            (void)bsp_pmic_switch_enable(BSP_PMIC_SWITCH_DC1SW, false);
            ESP_LOGE(TAG, "RGB LED creation failed: %s", esp_err_to_name(led_error));
            return led_error;
        }
        if (led_cnt != NULL) {
            ++(*led_cnt);
        }
    }
    return ESP_OK;
}

esp_err_t bsp_led_set(led_indicator_handle_t handle, bool on)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "LED handle is NULL");
    return led_indicator_start(handle, on ? BSP_LED_ON : BSP_LED_OFF);
}
