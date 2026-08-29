/*
 * Candis-S31 watch demo - shared display diagnostics (spec E.3).
 *
 * Hardware diagnostics used by both the test suites (tests/test_display.c,
 * tests/test_touch.c) and the feature app (apps/app_display_test.c), so
 * the display pipeline logic exists exactly once:
 *
 *  - TE statistics through the esp_lvgl_port TE observer
 *    (lvgl_port_display_te_observer_set). This is the ONLY legal way to
 *    watch GPIO16: the GPIO ISR stays owned by the LVGL port (spec C3);
 *    no gpio_isr_handler_add, no gpio_get_level polling.
 *  - A port of the factory motion context (factory_display.c): a 5 ms
 *    animation timer, a 1 s statistics timer and frame counting via
 *    LV_EVENT_FLUSH_START / LV_EVENT_REFR_READY display events.
 *  - A FLUSH_START counter plus invalidation helpers for the panel-only
 *    sleep-cycle test.
 *
 * Threading: every public function is callable from ANY task. LVGL-object
 * work is bracketed internally with bsp_display_lock() (the recursive
 * LVGL mutex - the same pattern factory_display.c/factory_touch.c used
 * from the console task). Test run functions therefore never call LVGL
 * APIs themselves (spec C.7); on-screen hosting for interactive tests
 * still comes from the run-view canvas (ctx->request_canvas), whose
 * build callback runs on the LVGL thread and passes the parent object in.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* TE edge statistics (observer-based, spec C3)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t rising_edges;
    uint32_t falling_edges;
    /* Complete rise->rise periods whose both edges were observed. */
    uint32_t period_count;
    int64_t  period_min_us;
    int64_t  period_max_us;
    uint64_t period_total_us;
    /* Complete rise->fall pulse widths. */
    uint32_t high_count;
    int64_t  high_min_us;
    int64_t  high_max_us;
    uint64_t high_total_us;
} display_diag_te_stats_t;

/** Attach the TE observer and start accumulating. One consumer at a time
 *  (the observer is a single BSP slot): ESP_ERR_INVALID_STATE when busy,
 *  ESP_ERR_INVALID_STATE from the port when TE sync is unavailable. */
esp_err_t display_diag_te_attach(void);

/** Detach the observer (restores the rising-edge-only TE sync trigger).
 *  No-op when not attached. */
void      display_diag_te_detach(void);

/** Zero the accumulators (attach first, then reset for a clean window). */
void      display_diag_te_reset(void);

/** Copy the current accumulators. */
void      display_diag_te_snapshot(display_diag_te_stats_t *out);

/** Convenience blocking measurement: attach + reset, wait window_ms
 *  (sleeps in 100 ms slices), snapshot, detach. Fails with
 *  ESP_ERR_INVALID_STATE when the observer is busy. */
esp_err_t display_diag_te_stats(uint32_t window_ms,
                                display_diag_te_stats_t *out);
/* Optional rising-edge hook, invoked from the TE ISR (in addition to the
 * built-in accumulators) while the observer is attached. The hook must be
 * IRAM-safe: no logging, no allocation, no LVGL. Used by the touch
 * draw-latency test to chain marker flushes to the next TE rising edge.
 * Register after display_diag_te_attach(); cleared by
 * display_diag_te_detach(). */
typedef void (*display_diag_te_rising_cb_t)(int64_t timestamp_us, void *user);
void      display_diag_te_set_rising_hook(display_diag_te_rising_cb_t cb,
                                          void *user);

/* ------------------------------------------------------------------ */
/* Motion context (ported from factory_display.c)                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t frames_total;  /* flushed refresh cycles since start */
    uint32_t cycles_min;    /* min frames in any completed 1 s window */
    uint32_t cycles_max;
    uint32_t fps_last;      /* frames in the most recent 1 s window */
    uint32_t seconds;       /* completed 1 s windows */
} display_diag_motion_stats_t;

/** Start the motion demo on parent (a moving 48 px ball; scroll=true
 *  additionally repaints high-contrast stripes on the parent area every
 *  frame for the full-screen/tear regime). Objects become children of
 *  parent; a DELETE hook on parent tears the context down automatically,
 *  so a dying canvas or screen never leaves timers pointing at freed
 *  objects. ESP_ERR_INVALID_STATE when already running. */
esp_err_t display_diag_motion_start(lv_obj_t *parent, bool scroll);

/** Immediate stop (LVGL context: app code or canvas DELETE hook).
 *  out may be NULL. ESP_ERR_INVALID_STATE when not running. */
esp_err_t display_diag_motion_stop(display_diag_motion_stats_t *out);

/** Cooperative stop from a non-LVGL task: the 1 s statistics timer
 *  performs the teardown on the LVGL thread (worst-case latency 1 s). */
void      display_diag_motion_request_stop(void);

/** Poll until stopped (or timeout). Returns true when stopped. */
bool      display_diag_motion_wait_stopped(uint32_t timeout_ms);

bool      display_diag_motion_running(void);

/** Copy the current counters. */
void      display_diag_motion_snapshot(display_diag_motion_stats_t *out);

/* ------------------------------------------------------------------ */
/* Flush counter + invalidation helpers (panel sleep-cycle test)       */
/* ------------------------------------------------------------------ */

/** Start counting LV_EVENT_FLUSH_START on the default display. */
esp_err_t display_diag_flush_counter_attach(void);
void      display_diag_flush_counter_detach(void);
uint32_t  display_diag_flush_counter_read(void);

/** Freeze/unfreeze LVGL rendering (svc_power screen-off pattern). */
void      display_diag_set_invalidation(bool enable);

/** Invalidate the whole active screen (forces a full redraw). */
void      display_diag_invalidate_all(void);

#ifdef __cplusplus
}
#endif
