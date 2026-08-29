/*
 * Candis-S31 simulator - display_diag mock.
 *
 * The motion test reproduces the firmware's scrolling-stripe visual
 * (tests/display_diag.c draw path) so the Screen test page looks and
 * animates like the real board; TE/flush statistics are synthesized
 * (60 Hz panel, no dropped frames).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tests/display_diag.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

/* ------------------------------------------------------------------ */
/* TE statistics: fixed 60 Hz panel rhythm.                            */
/* ------------------------------------------------------------------ */

static bool s_te_attached;

esp_err_t display_diag_te_attach(void)
{
    s_te_attached = true;
    return ESP_OK;
}

void display_diag_te_detach(void)
{
    s_te_attached = false;
}

void display_diag_te_reset(void)
{
}

void display_diag_te_snapshot(display_diag_te_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_te_attached) {
        return;
    }
    /* Synthesize a plausible 59.8 Hz TE stream. */
    out->rising_edges = 598;
    out->falling_edges = 598;
    out->period_count = 597;
    out->period_min_us = 16500;
    out->period_max_us = 16900;
    out->period_total_us = (uint64_t)597 * 16717;
    out->high_count = 598;
    out->high_min_us = 120;
    out->high_max_us = 180;
    out->high_total_us = (uint64_t)598 * 150;
}

esp_err_t display_diag_te_stats(uint32_t window_ms,
                                display_diag_te_stats_t *out)
{
    (void)window_ms;
    display_diag_te_snapshot(out);
    return s_te_attached ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void display_diag_te_set_rising_hook(display_diag_te_rising_cb_t cb,
                                     void *user)
{
    (void)cb;
    (void)user;
}

/* ------------------------------------------------------------------ */
/* Motion test: scrolling high-contrast stripes (same visual as the     */
/* firmware draw path) driven by an LVGL timer on the caller's thread.  */
/* ------------------------------------------------------------------ */

#define SIM_MOTION_STRIPE_PX   16
#define SIM_MOTION_SCROLL_PX_S 240

typedef struct {
    lv_obj_t *parent;
    lv_timer_t *timer;
    bool scroll;
    int scroll_offset_px;
    int64_t last_anim_us;
    display_diag_motion_stats_t stats;
    uint32_t window_frames;
    int64_t window_start_us;
    bool running;
} sim_motion_ctx_t;

static sim_motion_ctx_t s_motion;

static void sim_motion_draw_cb(lv_event_t *event)
{
    static const lv_color_t stripe_colors[] = {
        LV_COLOR_MAKE(255, 255, 255), LV_COLOR_MAKE(0, 0, 0),
        LV_COLOR_MAKE(255, 0, 0), LV_COLOR_MAKE(0, 255, 0),
        LV_COLOR_MAKE(0, 0, 255), LV_COLOR_MAKE(255, 255, 0),
    };
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_area_t obj_area;
    lv_obj_get_coords(obj, &obj_area);

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_COVER;

    const int stripe_px = SIM_MOTION_STRIPE_PX;
    const int offset = s_motion.scroll_offset_px;
    const int height = obj_area.y2 - obj_area.y1 + 1;
    for (int y = -(offset % stripe_px); y < height; y += stripe_px) {
        const int content_row = (y + offset) / stripe_px;
        rect.bg_color = stripe_colors[content_row % 6 < 0 ?
                                      0 : content_row % 6];
        lv_area_t area = {
            .x1 = obj_area.x1,
            .x2 = obj_area.x2,
            .y1 = (int32_t)(obj_area.y1 + y),
            .y2 = (int32_t)(obj_area.y1 + y + stripe_px - 1),
        };
        lv_draw_rect(layer, &rect, &area);
    }
}

static void sim_motion_anim_cb(lv_timer_t *timer)
{
    (void)timer;
    const int64_t now_us = esp_timer_get_time();
    if (s_motion.last_anim_us == 0) {
        s_motion.last_anim_us = now_us;
        s_motion.window_start_us = now_us;
        return;
    }
    const int32_t elapsed_us = (int32_t)(now_us - s_motion.last_anim_us);
    s_motion.last_anim_us = now_us;

    if (s_motion.scroll) {
        s_motion.scroll_offset_px +=
            (int)(SIM_MOTION_SCROLL_PX_S * elapsed_us / 1000000);
        s_motion.scroll_offset_px %= SIM_MOTION_STRIPE_PX * 6;
    } else {
        /* Non-scroll mode still cycles the pattern for visible motion. */
        s_motion.scroll_offset_px =
            (s_motion.scroll_offset_px + 1) % (SIM_MOTION_STRIPE_PX * 6);
    }
    if (s_motion.parent != NULL) {
        lv_obj_invalidate(s_motion.parent);
    }

    /* Frame accounting: one "frame" per animation tick. */
    ++s_motion.stats.frames_total;
    ++s_motion.window_frames;
    const int64_t window_us = now_us - s_motion.window_start_us;
    if (window_us >= 1000000) {
        s_motion.stats.fps_last = s_motion.window_frames;
        if (s_motion.stats.cycles_min == 0 ||
            s_motion.window_frames < s_motion.stats.cycles_min) {
            s_motion.stats.cycles_min = s_motion.window_frames;
        }
        if (s_motion.window_frames > s_motion.stats.cycles_max) {
            s_motion.stats.cycles_max = s_motion.window_frames;
        }
        ++s_motion.stats.seconds;
        s_motion.window_frames = 0;
        s_motion.window_start_us = now_us;
    }
}

esp_err_t display_diag_motion_start(lv_obj_t *parent, bool scroll)
{
    if (s_motion.running || parent == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_motion, 0, sizeof(s_motion));
    s_motion.parent = parent;
    s_motion.scroll = scroll;
    s_motion.running = true;
    lv_obj_add_event_cb(parent, sim_motion_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_motion.timer = lv_timer_create(sim_motion_anim_cb, 16, NULL);
    return ESP_OK;
}

esp_err_t display_diag_motion_stop(display_diag_motion_stats_t *out)
{
    if (!s_motion.running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out != NULL) {
        *out = s_motion.stats;
    }
    if (s_motion.timer != NULL) {
        lv_timer_delete(s_motion.timer);
        s_motion.timer = NULL;
    }
    if (s_motion.parent != NULL) {
        lv_obj_remove_event_cb(s_motion.parent, sim_motion_draw_cb);
        s_motion.parent = NULL;
    }
    s_motion.running = false;
    return ESP_OK;
}

void display_diag_motion_request_stop(void)
{
    (void)display_diag_motion_stop(NULL);
}

bool display_diag_motion_wait_stopped(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return !s_motion.running;
}

bool display_diag_motion_running(void)
{
    return s_motion.running;
}

void display_diag_motion_snapshot(display_diag_motion_stats_t *out)
{
    if (out != NULL) {
        *out = s_motion.stats;
    }
}

/* ------------------------------------------------------------------ */
/* Flush counter: meaningless without the panel flush hook; fixed zero. */
/* ------------------------------------------------------------------ */

esp_err_t display_diag_flush_counter_attach(void)
{
    return ESP_OK;
}

void display_diag_flush_counter_detach(void)
{
}

uint32_t display_diag_flush_counter_read(void)
{
    return 0;
}

void display_diag_set_invalidation(bool enable)
{
    (void)enable;
}

void display_diag_invalidate_all(void)
{
    lv_obj_invalidate(lv_screen_active());
}
