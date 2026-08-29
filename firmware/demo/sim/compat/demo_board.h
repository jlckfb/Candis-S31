/*
 * Candis-S31 simulator - demo_board shim.
 *
 * Settings live in a process-locat static; demo_board_init() is a no-op
 * because the sim entry performs its own LVGL/SDL bring-up.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Persistent user settings, stored in NVS namespace "demo" on hardware. */
typedef struct {
    int brightness;       /**< 10..100 percent */
    int volume;           /**< 0..100 speaker volume */
    int mic_gain_db;      /**< 0..36 dB in 3 dB steps */
    int screen_timeout_s; /**< idle seconds before the display sleeps */
} demo_settings_t;

esp_err_t demo_board_init(void);

/** Global settings. Never NULL. */
demo_settings_t *demo_settings(void);

/** Persist the global settings (sim: no-op). */
void demo_settings_save(void);

#ifdef __cplusplus
}
#endif
