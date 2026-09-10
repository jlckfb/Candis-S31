/*
 * Candis-S31 standalone examples - shared board bootstrap.
 *
 * Generalized from the player example's board_init.c: safe-off of the
 * switchable power domains, board + PMIC init, latched interrupt cleanup,
 * and NVS init with the erase-and-retry fallback. Optional PSRAM and
 * display startup are selected by the caller.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool require_psram;     /*< Fail unless 32 MB PSRAM is present with a >= 2 MB free block */
    bool start_display;     /*< Start the AMOLED and apply brightness_percent */
    int  brightness_percent;/*< Backlight brightness 0..100, used when start_display is true */
} example_board_cfg_t;

/*
 * Power down the switchable domains, initialize board + PMIC, clear latched
 * PMIC/RTC/shared-IRQ flags, and initialize NVS (erase + retry on full or
 * version-mismatch). When cfg->require_psram is set, verifies the 32 MB
 * PSRAM and its largest free block; when cfg->start_display is set, starts
 * the display and applies cfg->brightness_percent.
 *
 * cfg may be NULL, equivalent to { false, false, 0 }.
 */
esp_err_t example_board_init(const example_board_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
