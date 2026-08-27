/*
 * Candis-S31 watch demo - DISPLAY_TOUCH (touch) domain test suite.
 *
 * Two interactive tests (spec D.1/E.3, ported from factory_touch.c):
 *  - touch.corners      ordered four-corner guided touch (release-first,
 *                       60 px corner zones around guide crosses)
 *  - touch.draw_latency marker-follows-finger plus event->flush and
 *                       flush->TE latency statistics
 *
 * Threading: the test functions run on the svc_test task and never call
 * LVGL APIs (spec C.7). All on-screen objects live inside the run-view
 * canvas; the build callback runs on the LVGL thread and wires every
 * LVGL-side resource (event callbacks, timers, TE observer) to a static
 * shared block. Static storage (tests are strictly serial) removes the
 * whole class of canvas-death vs. test-exit lifetime races. The touch
 * fast path does zero logging and zero formatting (spec B.5-8); the
 * on-screen statistics line is written by a low-frequency LVGL timer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display_diag.h"
#include "test_registry.h"

/* Canvas geometry: the run-view content area is 460x364 with 16/8/16/16
 * padding; the full-bleed container below covers it exactly. */
#define CANVAS_W        460
#define CANVAS_H        364
#define CANVAS_OFS_X    (-16)
#define CANVAS_OFS_Y    (-8)

/* Guide crosses are inset from the edges because the panel corners are
 * physically rounded (factory_touch.c). */
#define TOUCH_CORNER_INSET_PX 24
/* Corner acceptance zone (spec E.3: 60 px). */
#define TOUCH_ZONE_PX   60

#define CORNER_WAIT_RELEASE_MS  2000
#define CORNER_HIT_TIMEOUT_MS   20000

#define DRAW_BEGIN_TIMEOUT_MS   30000
#define DRAW_WINDOW_MS          15000

#define TOUCH_MARKER_RADIUS     12
#define TOUCH_TE_PENDING_MAX    8
#define TOUCH_RESERVOIR         256

typedef enum {
    TOUCH_MODE_NONE = 0,
    TOUCH_MODE_CORNERS,
    TOUCH_MODE_DRAW,
} touch_mode_t;

/* min/avg/max + reservoir for p95 (factory_touch.c, ISR-safe, bounded). */
typedef struct {
    uint32_t count;
    uint32_t stored;
    uint32_t min_us;
    uint32_t max_us;
    uint64_t sum_us;
    uint32_t rng;
    uint32_t samples[TOUCH_RESERVOIR];
} touch_stat_t;

typedef struct {
    int64_t flush_us;
} touch_te_pending_t;

/* Shared LVGL<->svc_test state. Static: tests are serial and the canvas
 * DELETE hook can therefore never dereference freed memory. */
typedef struct {
    portMUX_TYPE mux;
    touch_mode_t mode;
    /* test task -> LVGL */
    int target_corner;      /* 0..3 guide target, -1 hides the cross */
    volatile bool finished; /* test done: LVGL side self-cleans */
    /* LVGL -> test task */
    volatile bool cleaned;  /* LVGL-side teardown complete */
    bool pressed;
    int32_t x, y;           /* last touch point, container coords */
    uint32_t samples;       /* pressed samples seen by the event cb */
    /* LVGL-side object handles (LVGL thread only) */
    lv_obj_t *container;
    lv_obj_t *cross_h;
    lv_obj_t *cross_v;
    lv_obj_t *prompt;
    lv_obj_t *marker;
    lv_obj_t *stats_label;
    lv_timer_t *timer;
    lv_display_t *display;
    int applied_corner;
    /* latency chain (draw mode) */
    bool latency_active;
    bool flush_attached;
    bool te_attached;
    bool event_pending;
    int64_t event_us;
    lv_area_t marker_area;
    uint32_t lvgl_events;
    uint32_t flush_matches;
    uint32_t te_rising;
    uint32_t te_pending_count;
    touch_te_pending_t te_pending[TOUCH_TE_PENDING_MAX];
    touch_stat_t event_to_flush;
    touch_stat_t flush_to_te;
} touch_shared_t;

static touch_shared_t s_touch;

static const char *const s_corner_name[] = {
    "top-left", "top-right", "bottom-right", "bottom-left",
};
static const char *const s_corner_abbr[] = { "TL", "TR", "BR", "BL" };

/* ------------------------------------------------------------------ */
/* Statistics (factory_touch.c reservoir sampler)                      */
/* ------------------------------------------------------------------ */

static void touch_stat_init(touch_stat_t *stat, uint32_t seed)
{
    memset(stat, 0, sizeof(*stat));
    stat->rng = seed;
}

/* Called from the LVGL task and the TE ISR while ctx->mux is held. */
static void IRAM_ATTR touch_stat_add(touch_stat_t *stat, int64_t delta_us)
{
    if (delta_us < 0 || delta_us > UINT32_MAX) {
        return;
    }
    const uint32_t value = (uint32_t)delta_us;
    if (stat->count == 0) {
        stat->min_us = value;
        stat->max_us = value;
    } else {
        stat->min_us = value < stat->min_us ? value : stat->min_us;
        stat->max_us = value > stat->max_us ? value : stat->max_us;
    }
    ++stat->count;
    stat->sum_us += value;
    if (stat->stored < TOUCH_RESERVOIR) {
        stat->samples[stat->stored++] = value;
        return;
    }
    /* Deterministic reservoir sampling keeps p95 representative while
     * bounding ISR-side storage. */
    uint32_t random = stat->rng;
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    stat->rng = random;
    const uint32_t slot = random % stat->count;
    if (slot < TOUCH_RESERVOIR) {
        stat->samples[slot] = value;
    }
}

static int touch_stat_compare(const void *left, const void *right)
{
    const uint32_t a = *(const uint32_t *)left;
    const uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static void touch_stat_summary(touch_stat_t *stat, uint32_t *avg_us,
                               uint32_t *p95_us, uint32_t *max_us)
{
    *avg_us = stat->count > 0
              ? (uint32_t)((stat->sum_us + stat->count / 2U) / stat->count)
              : 0;
    *max_us = stat->count > 0 ? stat->max_us : 0;
    *p95_us = 0;
    if (stat->stored > 0) {
        qsort(stat->samples, stat->stored, sizeof(stat->samples[0]),
              touch_stat_compare);
        const uint32_t rank = (stat->stored * 95U + 99U) / 100U;
        *p95_us = stat->samples[rank - 1U];
    }
}

/* ------------------------------------------------------------------ */
/* Canvas event callbacks (LVGL thread; zero logging/formatting)       */
/* ------------------------------------------------------------------ */

static bool touch_area_intersects(const lv_area_t *left,
                                  const lv_area_t *right)
{
    return left->x1 <= right->x2 && left->x2 >= right->x1 &&
           left->y1 <= right->y2 && left->y2 >= right->y1;
}

static void touch_canvas_event_cb(lv_event_t *event)
{
    touch_shared_t *sh = lv_event_get_user_data(event);
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_event_get_indev(event);
        if (indev == NULL) {
            return;
        }
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        lv_area_t cont_area;
        lv_obj_get_coords(sh->container, &cont_area);
        const int32_t cx = point.x - cont_area.x1;
        const int32_t cy = point.y - cont_area.y1;
        portENTER_CRITICAL(&sh->mux);
        sh->pressed = true;
        sh->x = cx;
        sh->y = cy;
        ++sh->samples;
        portEXIT_CRITICAL(&sh->mux);

        if (sh->mode == TOUCH_MODE_DRAW && sh->marker != NULL) {
            const int64_t event_us = esp_timer_get_time();
            lv_obj_remove_flag(sh->marker, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(sh->marker, cx - TOUCH_MARKER_RADIUS,
                           cy - TOUCH_MARKER_RADIUS);
            lv_area_t marker_area;
            lv_obj_get_coords(sh->marker, &marker_area);
            portENTER_CRITICAL(&sh->mux);
            if (sh->latency_active) {
                ++sh->lvgl_events;
                sh->event_pending = true;
                sh->event_us = event_us;
                sh->marker_area = marker_area;
            }
            portEXIT_CRITICAL(&sh->mux);
        }
    } else if (code == LV_EVENT_RELEASED) {
        portENTER_CRITICAL(&sh->mux);
        sh->pressed = false;
        portEXIT_CRITICAL(&sh->mux);
        if (sh->mode == TOUCH_MODE_DRAW && sh->marker != NULL) {
            lv_obj_add_flag(sh->marker, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void touch_flush_event_cb(lv_event_t *event)
{
    const lv_area_t *flush_area = lv_event_get_param(event);
    touch_shared_t *sh = lv_event_get_user_data(event);
    if (flush_area == NULL) {
        return;
    }
    const int64_t flush_us = esp_timer_get_time();
    portENTER_CRITICAL(&sh->mux);
    if (sh->latency_active && sh->event_pending &&
            touch_area_intersects(flush_area, &sh->marker_area)) {
        if (flush_us >= sh->event_us) {
            touch_stat_add(&sh->event_to_flush, flush_us - sh->event_us);
            ++sh->flush_matches;
            if (sh->te_pending_count < TOUCH_TE_PENDING_MAX) {
                sh->te_pending[sh->te_pending_count++].flush_us = flush_us;
            }
        }
        sh->event_pending = false;
    }
    portEXIT_CRITICAL(&sh->mux);
}

/* TE rising edge (ISR context, via the display_diag observer hook). */
static void IRAM_ATTR touch_te_rising_cb(int64_t timestamp_us, void *user)
{
    touch_shared_t *sh = user;
    portENTER_CRITICAL_ISR(&sh->mux);
    if (sh->latency_active) {
        ++sh->te_rising;
        for (uint32_t i = 0; i < sh->te_pending_count; ++i) {
            if (timestamp_us >= sh->te_pending[i].flush_us) {
                touch_stat_add(&sh->flush_to_te,
                               timestamp_us - sh->te_pending[i].flush_us);
            }
        }
        sh->te_pending_count = 0;
    }
    portEXIT_CRITICAL_ISR(&sh->mux);
}

/* ------------------------------------------------------------------ */
/* LVGL-side timers and teardown                                       */
/* ------------------------------------------------------------------ */

/* LVGL context. Idempotent; safe from the container DELETE hook. */
static void touch_draw_cleanup_locked(touch_shared_t *sh)
{
    portENTER_CRITICAL(&sh->mux);
    sh->latency_active = false;
    portEXIT_CRITICAL(&sh->mux);
    if (sh->flush_attached && sh->display != NULL) {
        lv_display_remove_event_cb_with_user_data(
            sh->display, touch_flush_event_cb, sh);
        sh->flush_attached = false;
    }
    if (sh->te_attached) {
        display_diag_te_set_rising_hook(NULL, NULL);
        display_diag_te_detach();
        sh->te_attached = false;
    }
}

static void touch_container_delete_cb(lv_event_t *event)
{
    touch_shared_t *sh = lv_event_get_user_data(event);
    /* The canvas is dying: kill the (globally-living) timer first so it
     * never dereferences dead objects, then detach LVGL-side hooks. */
    if (sh->timer != NULL) {
        lv_timer_delete(sh->timer);
        sh->timer = NULL;
    }
    touch_draw_cleanup_locked(sh);
    portENTER_CRITICAL(&sh->mux);
    sh->cleaned = true;
    portEXIT_CRITICAL(&sh->mux);
}

static void touch_corners_timer(lv_timer_t *timer)
{
    touch_shared_t *sh = lv_timer_get_user_data(timer);
    portENTER_CRITICAL(&sh->mux);
    const bool finished = sh->finished;
    const int target = sh->target_corner;
    portEXIT_CRITICAL(&sh->mux);
    if (finished) {
        sh->timer = NULL;
        lv_timer_delete(timer);
        portENTER_CRITICAL(&sh->mux);
        sh->cleaned = true;
        portEXIT_CRITICAL(&sh->mux);
        return;
    }
    if (target == sh->applied_corner) {
        return;
    }
    sh->applied_corner = target;
    if (target < 0 || target > 3) {
        lv_obj_add_flag(sh->cross_h, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sh->cross_v, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(sh->prompt, "");
        return;
    }
    const lv_coord_t cx = (target == 1 || target == 2)
                          ? CANVAS_W - 1 - TOUCH_CORNER_INSET_PX
                          : TOUCH_CORNER_INSET_PX;
    const lv_coord_t cy = target >= 2 ? CANVAS_H - 1 - TOUCH_CORNER_INSET_PX
                                      : TOUCH_CORNER_INSET_PX;
    lv_obj_set_pos(sh->cross_h, cx - 10, cy - 1);
    lv_obj_set_pos(sh->cross_v, cx - 1, cy - 10);
    lv_obj_remove_flag(sh->cross_h, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(sh->cross_v, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(sh->prompt, "Touch %s corner",
                          s_corner_name[target]);
}

static void touch_draw_stats_timer(lv_timer_t *timer)
{
    touch_shared_t *sh = lv_timer_get_user_data(timer);
    portENTER_CRITICAL(&sh->mux);
    const bool finished = sh->finished;
    portEXIT_CRITICAL(&sh->mux);
    if (finished) {
        touch_draw_cleanup_locked(sh);
        sh->timer = NULL;
        lv_timer_delete(timer);
        portENTER_CRITICAL(&sh->mux);
        sh->cleaned = true;
        portEXIT_CRITICAL(&sh->mux);
        return;
    }

    /* Low-frequency statistics landing: the ONLY place text is formatted
     * (spec B.5-8: the touch fast path stays silent). */
    touch_stat_t e2f, f2t;
    uint32_t samples, te_rising;
    portENTER_CRITICAL(&sh->mux);
    e2f = sh->event_to_flush;
    f2t = sh->flush_to_te;
    samples = sh->samples;
    te_rising = sh->te_rising;
    portEXIT_CRITICAL(&sh->mux);
    uint32_t e2f_avg, e2f_p95, e2f_max, f2t_avg, f2t_p95, f2t_max;
    touch_stat_summary(&e2f, &e2f_avg, &e2f_p95, &e2f_max);
    touch_stat_summary(&f2t, &f2t_avg, &f2t_p95, &f2t_max);
    lv_label_set_text_fmt(
        sh->stats_label,
        "e2f avg %lu us p95 %lu us | te %lu us | n %lu",
        (unsigned long)e2f_avg, (unsigned long)e2f_p95,
        (unsigned long)f2t_avg, (unsigned long)samples);
    (void)te_rising;
}

/* ------------------------------------------------------------------ */
/* Shared canvas helpers                                               */
/* ------------------------------------------------------------------ */

static void touch_cross_arm(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                            lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *arm = lv_obj_create(parent);
    lv_obj_remove_style_all(arm);
    lv_obj_set_size(arm, w, h);
    lv_obj_set_pos(arm, x, y);
    lv_obj_set_style_bg_color(arm, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arm, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(arm, LV_OBJ_FLAG_CLICKABLE);
}

static void touch_cross(lv_obj_t *parent, lv_coord_t cx, lv_coord_t cy)
{
    touch_cross_arm(parent, cx - 10, cy - 1, 21, 2);
    touch_cross_arm(parent, cx - 1, cy - 10, 2, 21);
}

/* Full-bleed black container at the bottom of the z-order so the run
 * view chrome (stage text, ask panel, result chip) stays on top. */
static lv_obj_t *touch_canvas_container(lv_obj_t *parent,
                                        touch_shared_t *sh)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, CANVAS_W, CANVAS_H);
    lv_obj_set_pos(cont, CANVAS_OFS_X, CANVAS_OFS_Y);
    lv_obj_set_style_bg_color(cont, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_to_index(cont, 0);
    lv_obj_add_event_cb(cont, touch_canvas_event_cb, LV_EVENT_ALL, sh);
    lv_obj_add_event_cb(cont, touch_container_delete_cb, LV_EVENT_DELETE,
                        sh);
    sh->container = cont;
    return cont;
}

static void corners_build(lv_obj_t *parent, void *user)
{
    touch_shared_t *sh = user;
    sh->mode = TOUCH_MODE_CORNERS;
    sh->applied_corner = -2; /* force the timer to apply the first target */
    lv_obj_t *cont = touch_canvas_container(parent, sh);

    sh->cross_h = lv_obj_create(cont);
    lv_obj_remove_style_all(sh->cross_h);
    lv_obj_set_size(sh->cross_h, 21, 2);
    lv_obj_set_style_bg_color(sh->cross_h, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sh->cross_h, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(sh->cross_h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sh->cross_h, LV_OBJ_FLAG_HIDDEN);

    sh->cross_v = lv_obj_create(cont);
    lv_obj_remove_style_all(sh->cross_v);
    lv_obj_set_size(sh->cross_v, 2, 21);
    lv_obj_set_style_bg_color(sh->cross_v, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sh->cross_v, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(sh->cross_v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sh->cross_v, LV_OBJ_FLAG_HIDDEN);

    sh->prompt = lv_label_create(cont);
    lv_obj_set_style_text_color(sh->prompt, lv_color_white(), LV_PART_MAIN);
    lv_obj_clear_flag(sh->prompt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(sh->prompt, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(sh->prompt, "");

    sh->timer = lv_timer_create(touch_corners_timer, 50, sh);
}

static void draw_build(lv_obj_t *parent, void *user)
{
    touch_shared_t *sh = user;
    sh->mode = TOUCH_MODE_DRAW;
    lv_obj_t *cont = touch_canvas_container(parent, sh);

    /* Reference crosses: four canvas corners + center (factory layout). */
    for (unsigned corner = 0; corner < 4; ++corner) {
        const lv_coord_t cx = (corner == 1 || corner == 2)
                              ? CANVAS_W - 1 - TOUCH_CORNER_INSET_PX
                              : TOUCH_CORNER_INSET_PX;
        const lv_coord_t cy = corner >= 2
                              ? CANVAS_H - 1 - TOUCH_CORNER_INSET_PX
                              : TOUCH_CORNER_INSET_PX;
        touch_cross(cont, cx, cy);
    }
    touch_cross(cont, CANVAS_W / 2, CANVAS_H / 2);

    lv_obj_t *hint = lv_label_create(cont);
    lv_label_set_text(hint, "Marker must sit under the finger");
    lv_obj_set_style_text_color(hint, lv_color_make(255, 255, 0),
                                LV_PART_MAIN);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 24);

    sh->stats_label = lv_label_create(cont);
    lv_obj_set_style_text_color(sh->stats_label,
                                lv_color_make(0x9A, 0x9A, 0xA2),
                                LV_PART_MAIN);
    lv_obj_clear_flag(sh->stats_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(sh->stats_label, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(sh->stats_label, "e2f avg -- us p95 -- us");

    sh->marker = lv_obj_create(cont);
    lv_obj_remove_style_all(sh->marker);
    lv_obj_set_size(sh->marker, 2 * TOUCH_MARKER_RADIUS,
                    2 * TOUCH_MARKER_RADIUS);
    lv_obj_set_style_radius(sh->marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sh->marker, lv_color_make(255, 0, 0),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sh->marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sh->marker, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(sh->marker, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(sh->marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(sh->marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sh->marker, LV_OBJ_FLAG_HIDDEN);

    /* Latency chain: display flush events + TE rising via display_diag. */
    sh->display = lv_display_get_default();
    if (sh->display != NULL) {
        lv_display_add_event_cb(sh->display, touch_flush_event_cb,
                                LV_EVENT_FLUSH_START, sh);
        sh->flush_attached = true;
    }
    if (display_diag_te_attach() == ESP_OK) {
        sh->te_attached = true;
        display_diag_te_set_rising_hook(touch_te_rising_cb, sh);
    }
    portENTER_CRITICAL(&sh->mux);
    sh->latency_active = true;
    portEXIT_CRITICAL(&sh->mux);

    sh->timer = lv_timer_create(touch_draw_stats_timer, 500, sh);
}

/* ------------------------------------------------------------------ */
/* Test-side helpers (svc_test task context)                           */
/* ------------------------------------------------------------------ */

static void touch_shared_reset(touch_shared_t *sh, touch_mode_t mode)
{
    memset(sh, 0, sizeof(*sh));
    portMUX_INITIALIZE(&sh->mux);
    sh->mode = mode;
    sh->target_corner = -1;
    sh->applied_corner = -1;
    touch_stat_init(&sh->event_to_flush, UINT32_C(0x2468ACE1));
    touch_stat_init(&sh->flush_to_te, UINT32_C(0xA5A5C3C3));
}

static void touch_shared_finish(touch_shared_t *sh)
{
    portENTER_CRITICAL(&sh->mux);
    sh->finished = true;
    sh->target_corner = -1;
    portEXIT_CRITICAL(&sh->mux);
    /* The LVGL side acknowledges through cleaned (timer self-cleanup or
     * the canvas DELETE hook, whichever comes first). */
    for (int wait = 0; wait < 50 && !sh->cleaned; ++wait) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool touch_point_in_corner(unsigned corner, int32_t x, int32_t y)
{
    const bool near_x = x < TOUCH_ZONE_PX;
    const bool far_x = x >= CANVAS_W - TOUCH_ZONE_PX;
    const bool near_y = y < TOUCH_ZONE_PX;
    const bool far_y = y >= CANVAS_H - TOUCH_ZONE_PX;
    switch (corner) {
    case 0: return near_x && near_y;
    case 1: return far_x && near_y;
    case 2: return far_x && far_y;
    case 3: return near_x && far_y;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* touch.corners: ordered guided four-corner test                      */
/* ------------------------------------------------------------------ */

static void run_corners(const test_ctx_t *ctx, test_result_t *out)
{
    touch_shared_reset(&s_touch, TOUCH_MODE_CORNERS);
    ctx->progress(ctx, 0, "Touch the highlighted corners");
    ctx->request_canvas(ctx, corners_build, &s_touch);

    char detail[96] = { 0 };
    size_t used = 0;
    for (unsigned corner = 0; corner < 4; ++corner) {
        if (ctx->cancel_requested(ctx)) {
            touch_shared_finish(&s_touch);
            return;
        }
        portENTER_CRITICAL(&s_touch.mux);
        s_touch.target_corner = (int)corner;
        portEXIT_CRITICAL(&s_touch.mux);
        ctx->progress(ctx, (int)corner * 25, "Follow the guide cross");

        /* Release-first: a finger still down from the previous step must
         * not carry over (factory_touch.c). */
        const int64_t release_deadline =
            esp_timer_get_time() +
            CORNER_WAIT_RELEASE_MS * INT64_C(1000);
        while (esp_timer_get_time() < release_deadline) {
            portENTER_CRITICAL(&s_touch.mux);
            const bool still_down = s_touch.pressed;
            portEXIT_CRITICAL(&s_touch.mux);
            if (!still_down) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        bool hit = false;
        int32_t last_x = -1, last_y = -1;
        const int64_t deadline =
            esp_timer_get_time() + CORNER_HIT_TIMEOUT_MS * INT64_C(1000);
        while (esp_timer_get_time() < deadline && !hit) {
            if (ctx->cancel_requested(ctx)) {
                touch_shared_finish(&s_touch);
                return;
            }
            portENTER_CRITICAL(&s_touch.mux);
            const bool pressed = s_touch.pressed;
            const int32_t x = s_touch.x;
            const int32_t y = s_touch.y;
            portEXIT_CRITICAL(&s_touch.mux);
            if (pressed) {
                last_x = x;
                last_y = y;
                hit = touch_point_in_corner(corner, x, y);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!hit) {
            touch_shared_finish(&s_touch);
            out->st = TEST_ST_FAIL;
            snprintf(out->evidence, sizeof(out->evidence),
                     "%s corner not hit (last x=%ld y=%ld)",
                     s_corner_name[corner], (long)last_x, (long)last_y);
            return;
        }
        used += snprintf(detail + used, sizeof(detail) - used, "%s%s(%ld,%ld)",
                         corner == 0 ? "ordered:" : ",",
                         s_corner_abbr[corner], (long)last_x, (long)last_y);
    }

    touch_shared_finish(&s_touch);
    ctx->progress(ctx, 100, "Corners done");
    out->st = TEST_ST_PASS;
    strlcpy(out->evidence, detail, sizeof(out->evidence));
}

/* ------------------------------------------------------------------ */
/* touch.draw_latency: marker follow + latency statistics              */
/* ------------------------------------------------------------------ */

static void run_draw_latency(const test_ctx_t *ctx, test_result_t *out)
{
    touch_shared_reset(&s_touch, TOUCH_MODE_DRAW);
    ctx->progress(ctx, 0, "Touch and draw to begin");
    ctx->request_canvas(ctx, draw_build, &s_touch);

    /* Phase 1: wait for the first touch sample. */
    int64_t deadline = esp_timer_get_time() +
                       DRAW_BEGIN_TIMEOUT_MS * INT64_C(1000);
    while (esp_timer_get_time() < deadline) {
        if (ctx->cancel_requested(ctx)) {
            touch_shared_finish(&s_touch);
            return;
        }
        portENTER_CRITICAL(&s_touch.mux);
        const uint32_t samples = s_touch.samples;
        portEXIT_CRITICAL(&s_touch.mux);
        if (samples > 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    portENTER_CRITICAL(&s_touch.mux);
    const bool began = s_touch.samples > 0;
    portEXIT_CRITICAL(&s_touch.mux);
    if (!began) {
        touch_shared_finish(&s_touch);
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "no touch input", sizeof(out->evidence));
        return;
    }

    /* Phase 2: free-drawing window with a countdown. */
    int elapsed_ms = 0;
    while (elapsed_ms < DRAW_WINDOW_MS) {
        if (ctx->cancel_requested(ctx)) {
            touch_shared_finish(&s_touch);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        elapsed_ms += 500;
        char stage[32];
        snprintf(stage, sizeof(stage), "Draw freely... %d s left",
                 (DRAW_WINDOW_MS - elapsed_ms) / 1000);
        ctx->progress(ctx, elapsed_ms * 100 / DRAW_WINDOW_MS, stage);
    }

    /* Freeze the statistics, then confirm with the operator. */
    touch_shared_finish(&s_touch);

    uint32_t e2f_avg = 0, e2f_p95 = 0, e2f_max = 0;
    uint32_t f2t_avg = 0, f2t_p95 = 0, f2t_max = 0;
    touch_stat_summary(&s_touch.event_to_flush, &e2f_avg, &e2f_p95,
                       &e2f_max);
    touch_stat_summary(&s_touch.flush_to_te, &f2t_avg, &f2t_p95, &f2t_max);

    bool timed_out = false;
    const bool yes = ctx->ask_operator(
        ctx, "Did the marker follow your finger?", 30000, &timed_out);
    if (ctx->cancel_requested(ctx)) {
        return;
    }

    char latency_part[64];
    if (s_touch.flush_matches > 0) {
        snprintf(latency_part, sizeof(latency_part),
                 " e2f avg %luus p95 %luus%s",
                 (unsigned long)e2f_avg, (unsigned long)e2f_p95,
                 s_touch.te_attached ? "" : " (no TE)");
    } else {
        strlcpy(latency_part, " no flush matches", sizeof(latency_part));
    }

    if (yes) {
        out->st = TEST_ST_PASS;
        snprintf(out->evidence, sizeof(out->evidence), "marker ok:%s",
                 latency_part);
    } else if (timed_out) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "operator timeout", sizeof(out->evidence));
    } else {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "marker rejected:%s", latency_part);
    }
    (void)f2t_avg;
    (void)f2t_p95;
    (void)f2t_max;
    (void)e2f_max;
}

/* ------------------------------------------------------------------ */

void test_touch_register(void)
{
    static const test_case_t cases[] = {
        { "touch.corners", "Touch 4 corners", TEST_DOM_DISPLAY_TOUCH,
          TEST_F_INTERACTIVE, 120000, run_corners },
        { "touch.draw_latency", "Draw + latency", TEST_DOM_DISPLAY_TOUCH,
          TEST_F_INTERACTIVE, 120000, run_draw_latency },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        test_register(&cases[i]);
    }
}
