/*
 * Candis-S31 watch demo - watchface root screen.
 *
 * The watchface is the navigation stack root (s_stack[0]); the launcher
 * menu opens above it as the second layer. Refresh policy: the UI manager
 * owns one 1 s clock and calls ui_watchface_tick() only while the
 * watchface is the active screen; every widget write is last-value
 * guarded, because FULL render mode merges any invalidate into one
 * TE-gated full-frame redraw (DESIGN.md). Minute-granularity time plus
 * change-only battery updates; deliberately no second hand.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"
#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the watchface screen (not loaded); called once by the manager. */
lv_obj_t *ui_watchface_create(void);

/**
 * 1 s refresh from the UI manager's clock, LVGL thread only, and only
 * while the watchface is the active screen. t == NULL means the RTC has
 * not returned a valid time yet this boot (placeholders stay). A later
 * transient RTC failure keeps the last good rendering.
 */
void ui_watchface_tick(const bsp_rtc_time_t *t);

#ifdef __cplusplus
}
#endif
