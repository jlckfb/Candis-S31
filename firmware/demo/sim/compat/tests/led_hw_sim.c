/*
 * Candis-S31 simulator - led_hw mock implementation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tests/led_hw.h"

#include "esp_log.h"

static bool s_owned;
static uint32_t s_rgb;
static int s_effect = -1;

esp_err_t led_hw_acquire(void)
{
    if (s_owned) {
        return ESP_ERR_INVALID_STATE;
    }
    s_owned = true;
    return ESP_OK;
}

void led_hw_release(void)
{
    s_owned = false;
}

bool led_hw_owned(void)
{
    return s_owned;
}

esp_err_t led_hw_set_rgb(uint32_t irgb)
{
    if (!s_owned) {
        return ESP_ERR_INVALID_STATE;
    }
    s_rgb = irgb & 0x00FFFFFFu;
    ESP_LOGI("sim_led", "RGB #%06x", (unsigned)s_rgb);
    return ESP_OK;
}

esp_err_t led_hw_start_effect(bsp_led_effect_t effect)
{
    if (!s_owned) {
        return ESP_ERR_INVALID_STATE;
    }
    s_effect = (int)effect;
    return ESP_OK;
}

esp_err_t led_hw_stop_effect(bsp_led_effect_t effect)
{
    if (!s_owned) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_effect == (int)effect) {
        s_effect = -1;
    }
    return ESP_OK;
}

uint32_t sim_led_get_rgb(void)
{
    return s_rgb;
}

int sim_led_get_effect(void)
{
    return s_effect;
}
