/*
 * Candis-S31 watch demo - board bootstrap and persistent settings.
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

/** Persistent user settings, stored in NVS namespace "demo". */
typedef struct {
    int brightness;       /**< 10..100 percent, applied at boot and from settings app */
    int volume;           /**< 0..100 speaker volume, applied on playback start */
    int mic_gain_db;      /**< 0..36 dB in 3 dB steps (ES8389 PGA real range) */
    int screen_timeout_s; /**< idle seconds before the display sleeps; 0 = never */
} demo_settings_t;

/**
 * Board bootstrap, called once from app_main before any service starts.
 *
 * Sequence: bsp_board_init() -> PMIC init (500 mA input limit clamp) ->
 * clear latched PMIC/RTC interrupt flags (deep-sleep re-wake guard) ->
 * NVS init -> settings load -> display start + brightness restore.
 *
 * On return the LVGL task is running and the display is unlocked.
 */
esp_err_t demo_board_init(void);

/** Global settings, loaded by demo_board_init(). Never NULL after init. */
demo_settings_t *demo_settings(void);

/** Persist the global settings to NVS. */
void demo_settings_save(void);

#ifdef __cplusplus
}
#endif
