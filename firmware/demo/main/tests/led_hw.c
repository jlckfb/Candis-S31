/*
 * Candis-S31 watch demo - WS2812B ownership module (spec C.5).
 *
 * One atomic ownership slot wraps the BSP led_indicator lifecycle so the
 * LED app and the sys.led_rgb test can never drive the strip at the same
 * time. Acquire creates the indicator (the BSP opens the DC1SW switch on
 * that path); release turns the output off and deletes it, so no blink
 * context survives into the shutdown path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_hw.h"

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static led_indicator_handle_t s_led;

esp_err_t led_hw_acquire(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_led != NULL) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_lock);

    led_indicator_handle_t handles[BSP_LED_NUM] = {0};
    int count = 0;
    const esp_err_t err = bsp_led_indicator_create(handles, &count,
                                                   BSP_LED_NUM);
    if (err != ESP_OK || count != BSP_LED_NUM) {
        return err != ESP_OK ? err : ESP_FAIL;
    }

    /* Publish only if the slot is still free; a racing acquirer wins and
     * this instance is torn down again (the BSP supports re-create). */
    portENTER_CRITICAL(&s_lock);
    if (s_led != NULL) {
        portEXIT_CRITICAL(&s_lock);
        led_indicator_delete(handles[BSP_LED_1]);
        return ESP_ERR_INVALID_STATE;
    }
    s_led = handles[BSP_LED_1];
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void led_hw_release(void)
{
    portENTER_CRITICAL(&s_lock);
    led_indicator_handle_t led = s_led;
    s_led = NULL;
    portEXIT_CRITICAL(&s_lock);
    if (led == NULL) {
        return;
    }
    led_indicator_set_on_off(led, false);
    led_indicator_delete(led);
}

bool led_hw_owned(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool owned = s_led != NULL;
    portEXIT_CRITICAL(&s_lock);
    return owned;
}

esp_err_t led_hw_set_rgb(uint32_t irgb)
{
    portENTER_CRITICAL(&s_lock);
    led_indicator_handle_t led = s_led;
    portEXIT_CRITICAL(&s_lock);
    if (led == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_indicator_set_rgb(led, irgb);
}

esp_err_t led_hw_start_effect(bsp_led_effect_t effect)
{
    portENTER_CRITICAL(&s_lock);
    led_indicator_handle_t led = s_led;
    portEXIT_CRITICAL(&s_lock);
    if (led == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_indicator_start(led, effect);
}

esp_err_t led_hw_stop_effect(bsp_led_effect_t effect)
{
    portENTER_CRITICAL(&s_lock);
    led_indicator_handle_t led = s_led;
    portEXIT_CRITICAL(&s_lock);
    if (led == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_indicator_stop(led, effect);
}
