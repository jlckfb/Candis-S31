/*
 * Candis-S31 watch demo - page navigation timing instrumentation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the nav code around a page's create() to time it. */
void ui_perf_create_begin(const char *id);
void ui_perf_create_end(void);

/* Called just before lv_scr_load_anim() commits the new screen; the
 * instrumentation reports one ESP_LOG line at the first FLUSH_START that
 * follows (the first frame of the new page reaching the display path). */
void ui_perf_load_begin(const char *id);

/* Subscribe to LV_EVENT_FLUSH_START on the display (once, at init). */
void ui_perf_attach(lv_display_t *display);

#ifdef __cplusplus
}
#endif
