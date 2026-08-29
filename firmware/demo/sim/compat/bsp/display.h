/*
 * Candis-S31 simulator - bsp/display.h shim.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stores the value for the status/UI; the SDL/wasm canvas has no real
 * backlight, so this is bookkeeping only. */
esp_err_t bsp_display_brightness_set(int brightness_percent);

#ifdef __cplusplus
}
#endif
