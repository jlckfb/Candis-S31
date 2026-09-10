/*
 * Candis-S31 RGB LED example.
 *
 * WS2812B on GPIO4, fed through the TG28 DC1SW switch which the BSP opens
 * inside bsp_led_indicator_create(). Cycles three effects on a steady white
 * and prints the current effect name on each switch.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp-bsp.h"
#include "example_board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_indicator.h"

static const char *TAG = "led";

#define EFFECT_DWELL_MS 4000

/* SET_IRGB-ready value, moderate intensity (factory_input.c pattern). */
#define LED_WHITE SET_IRGB(0, 44, 44, 44)

typedef struct {
    const char *name;
    bsp_led_effect_t effect;
} led_effect_t;

static const led_effect_t s_effects[] = {
    { "on (steady)", BSP_LED_ON },
    { "breathe (slow)", BSP_LED_BREATHE_SLOW },
    { "blink (slow)", BSP_LED_BLINK_SLOW },
};

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    }));

    led_indicator_handle_t handles[BSP_LED_NUM] = {0};
    int count = 0;
    ESP_ERROR_CHECK(bsp_led_indicator_create(handles, &count, BSP_LED_NUM));
    if (count != BSP_LED_NUM) {
        ESP_LOGE(TAG, "expected %d LED, got %d", BSP_LED_NUM, count);
        return;
    }
    const led_indicator_handle_t led = handles[BSP_LED_1];
    ESP_ERROR_CHECK(led_indicator_set_rgb(led, LED_WHITE));
    ESP_LOGI(TAG, "RGB LED ready (WS2812B on GPIO4, powered via DC1SW)");

    size_t current = 0;
    ESP_ERROR_CHECK(led_indicator_start(led, s_effects[current].effect));
    ESP_LOGI(TAG, "effect: %s", s_effects[current].name);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(EFFECT_DWELL_MS));
        const size_t next = (current + 1) % (sizeof(s_effects) / sizeof(s_effects[0]));
        ESP_ERROR_CHECK(led_indicator_stop(led, s_effects[current].effect));
        ESP_ERROR_CHECK(led_indicator_start(led, s_effects[next].effect));
        ESP_LOGI(TAG, "effect: %s", s_effects[next].name);
        current = next;
    }
}
