/*
 * Candis-S31 watch demo - WS2812B ownership module (redesign spec C.5).
 *
 * Single-owner arbitration for the RGB LED: app_led and the sys.led_rgb
 * test both go through led_hw_acquire()/led_hw_release(); the later
 * consumer is refused with ESP_ERR_INVALID_STATE. The BSP opens the DC1SW
 * power switch inside bsp_led_indicator_create(), so power sequencing is
 * not handled here.
 *
 * Thread-safe (internal critical section); the hardware calls themselves
 * must be made by the owner only, from whichever task owns the slot.
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

/** Take ownership and create the BSP indicator (powers DC1SW via the BSP).
 *  ESP_ERR_INVALID_STATE when another consumer holds the LED. */
esp_err_t led_hw_acquire(void);

/** Turn the LED off, delete the indicator and free the slot. */
void      led_hw_release(void);

/** True while the slot is owned. */
bool      led_hw_owned(void);

/** Set a steady color (SET_IRGB-ready value); the owner only. */
esp_err_t led_hw_set_rgb(uint32_t irgb);

/** Start/stop a BSP blink effect on the owned LED (app_led effects). */
esp_err_t led_hw_start_effect(bsp_led_effect_t effect);
esp_err_t led_hw_stop_effect(bsp_led_effect_t effect);

#ifdef __cplusplus
}
#endif
