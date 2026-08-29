/*
 * Candis-S31 simulator - led_hw shim.
 *
 * Ownership semantics match the hardware module (tests/led_hw.h);
 * color/effect state is logged and kept for page display only - the host
 * has no WS2812B to drive.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_hw_acquire(void);
void      led_hw_release(void);
bool      led_hw_owned(void);
esp_err_t led_hw_set_rgb(uint32_t irgb);
esp_err_t led_hw_start_effect(bsp_led_effect_t effect);
esp_err_t led_hw_stop_effect(bsp_led_effect_t effect);

/* Sim extension: last requested color/effect, for the LED page badge. */
uint32_t sim_led_get_rgb(void);
int      sim_led_get_effect(void);

#ifdef __cplusplus
}
#endif
