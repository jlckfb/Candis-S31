/*
 * Candis-S31 player demo - board bootstrap.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** User settings stored in NVS. */
typedef struct {
    int brightness;  /**< 0..100 display brightness */
    int volume;      /**< 0..100 speaker volume */
} player_settings_t;

/** One-time board bring-up: PMIC, NVS, display + touch + LVGL. */
esp_err_t player_board_init(void);

/** Persistent settings accessor. */
player_settings_t *player_settings(void);

/** Persist settings to NVS. */
void player_settings_save(void);

#ifdef __cplusplus
}
#endif
