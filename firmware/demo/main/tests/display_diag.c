/*
 * Candis-S31 watch demo - shared display diagnostics (spec E.3).
 * See display_diag.h for the module contract.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "display_diag.h"

#include <string.h>
#include <inttypes.h>

#include "bsp/esp-bsp.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_diag";

/* ------------------------------------------------------------------ */
/* TE observer statistics                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    portMUX_TYPE mux;
    bool owned;
    int64_t last_rise_us;
    int64_t last_fall_us;
    bool have_rise;
    bool have_fall;
    display_diag_te_stats_t stats;
    display_diag_te_rising_cb_t rising_cb;
    void *rising_user;
} te_ctx_t;

static te_ctx_t s_te = { .mux = portMUX_INITIALIZER_UNLOCKED };

/* Runs inside the LVGL port TE ISR (observer lock already held by the
 * port). Keep it IRAM-safe and short: no logging, no allocation. */
static void IRAM_ATTR diag_te_observer_cb(bool level, int64_t timestamp_us,
                                          void *user_ctx)
{
    te_ctx_t *ctx = user_ctx;
    portENTER_CRITICAL_ISR(&ctx->mux);
    display_diag_te_stats_t *st = &ctx->stats;
    if (level) {
        ++st->rising_edges;
        if (ctx->have_rise) {
            const int64_t delta = timestamp_us - ctx->last_rise_us;
            ++st->period_count;
            st->period_total_us += (uint64_t)delta;
            if (st->period_count == 1 || delta < st->period_min_us) {
                st->period_min_us = delta;
            }
            if (st->period_count == 1 || delta > st->period_max_us) {
                st->period_max_us = delta;
            }
        }
        ctx->last_rise_us = timestamp_us;
        ctx->have_rise = true;
        const display_diag_te_rising_cb_t hook = ctx->rising_cb;
        void *hook_user = ctx->rising_user;
        portEXIT_CRITICAL_ISR(&ctx->mux);
        /* The hook runs outside the accumulator lock so it can take its
         * own; it is still inside the port's observer lock (ISR). */
        if (hook != NULL) {
            hook(timestamp_us, hook_user);
        }
        return;
    } else {
        ++st->falling_edges;
        if (ctx->have_rise) {
            const int64_t delta = timestamp_us - ctx->last_rise_us;
            ++st->high_count;
            st->high_total_us += (uint64_t)delta;
            if (st->high_count == 1 || delta < st->high_min_us) {
                st->high_min_us = delta;
            }
            if (st->high_count == 1 || delta > st->high_max_us) {
                st->high_max_us = delta;
            }
        }
        ctx->last_fall_us = timestamp_us;
        ctx->have_fall = true;
    }
    portEXIT_CRITICAL_ISR(&ctx->mux);
}

esp_err_t display_diag_te_attach(void)
{
    portENTER_CRITICAL(&s_te.mux);
    const bool busy = s_te.owned;
    if (!busy) {
        s_te.owned = true;
    }
    portEXIT_CRITICAL(&s_te.mux);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Hold the LVGL lock across the observer install so no refresh can
     * run while the TE interrupt mode is being switched (factory_touch.c
     * precedent). */
    esp_err_t error = ESP_ERR_TIMEOUT;
    if (bsp_display_lock(1000)) {
        lv_display_t *disp = lv_display_get_default();
        if (disp != NULL) {
            error = lvgl_port_display_te_observer_set(
                disp, diag_te_observer_cb, &s_te);
        } else {
            error = ESP_ERR_INVALID_STATE;
        }
        bsp_display_unlock();
    }
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_te.mux);
        s_te.owned = false;
        portEXIT_CRITICAL(&s_te.mux);
    }
    return error;
}

void display_diag_te_detach(void)
{
    portENTER_CRITICAL(&s_te.mux);
    const bool owned = s_te.owned;
    s_te.rising_cb = NULL;
    s_te.rising_user = NULL;
    portEXIT_CRITICAL(&s_te.mux);
    if (!owned) {
        return;
    }
    if (bsp_display_lock(1000)) {
        lv_display_t *disp = lv_display_get_default();
        if (disp != NULL) {
            lvgl_port_display_te_observer_set(disp, NULL, NULL);
        }
        bsp_display_unlock();
    }
    portENTER_CRITICAL(&s_te.mux);
    s_te.owned = false;
    portEXIT_CRITICAL(&s_te.mux);
}

void display_diag_te_reset(void)
{
    portENTER_CRITICAL(&s_te.mux);
    memset(&s_te.stats, 0, sizeof(s_te.stats));
    s_te.last_rise_us = 0;
    s_te.last_fall_us = 0;
    s_te.have_rise = false;
    s_te.have_fall = false;
    portEXIT_CRITICAL(&s_te.mux);
}

void display_diag_te_snapshot(display_diag_te_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_te.mux);
    *out = s_te.stats;
    portEXIT_CRITICAL(&s_te.mux);
}
void display_diag_te_set_rising_hook(display_diag_te_rising_cb_t cb,
                                     void *user)
{
    portENTER_CRITICAL(&s_te.mux);
    s_te.rising_cb = cb;
    s_te.rising_user = user;
    portEXIT_CRITICAL(&s_te.mux);
}

esp_err_t display_diag_te_stats(uint32_t window_ms,
                                display_diag_te_stats_t *out)
{
    if (out == NULL || window_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(display_diag_te_attach(), TAG,
                        "TE observer busy or unavailable");
    display_diag_te_reset();
    uint32_t waited = 0;
    while (waited < window_ms) {
        const uint32_t slice = window_ms - waited > 100 ? 100 : window_ms - waited;
        vTaskDelay(pdMS_TO_TICKS(slice));
        waited += slice;
    }
    display_diag_te_snapshot(out);
    display_diag_te_detach();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Motion context (factory_display.c port, parent-scoped)              */
/* ------------------------------------------------------------------ */

#define DIAG_MOTION_STRIPE_PX   20
#define DIAG_MOTION_BALL_PX     48
#define DIAG_MOTION_SCROLL_PX_S 180
#define DIAG_MOTION_BALL_X_PX_S 240
#define DIAG_MOTION_BALL_Y_PX_S 170

typedef struct {
    lv_obj_t *parent;
    lv_obj_t *ball;
    lv_obj_t *fps_label;
    lv_timer_t *anim_timer;
    lv_timer_t *stats_timer;
    display_diag_motion_stats_t stats;
    uint32_t frames_window; /* refresh cycles since the last stats tick */
    int64_t last_anim_us;
    int64_t scroll_position_ups; /* micro-pixels, sub-pixel precision */
    int64_t ball_x_position_ups;
    int64_t ball_y_position_ups;
    int scroll_offset_px;
    int ball_dir_x;
    int ball_dir_y;
    bool scroll;
    bool running;
    bool flushed_in_cycle;
    volatile bool stop_requested;
    bool parent_dying;      /* DELETE hook path: skip parent cb removal */
} diag_motion_ctx_t;

static diag_motion_ctx_t s_motion;

static void diag_motion_refr_cb(lv_event_t *event)
{
    /* LV_EVENT_REFR_READY also fires on cycles that rendered nothing, so
     * a frame only counts when at least one flush happened in the cycle. */
    switch (lv_event_get_code(event)) {
    case LV_EVENT_FLUSH_START:
        s_motion.flushed_in_cycle = true;
        break;
    case LV_EVENT_REFR_READY:
        if (s_motion.flushed_in_cycle) {
            s_motion.flushed_in_cycle = false;
            ++s_motion.frames_window;
            ++s_motion.stats.frames_total;
        }
        break;
    default:
        break;
    }
}

/* High-contrast horizontal stripes drawn straight into the parent's draw
 * event: one cheap rectangle fill per stripe instead of a child-object
 * tree, so the full-area render stays inside one frame budget. */
static void diag_motion_draw_cb(lv_event_t *event)
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

    const int stripe_px = DIAG_MOTION_STRIPE_PX;
    const int offset = s_motion.scroll_offset_px;
    const int height = obj_area.y2 - obj_area.y1 + 1;
    for (int y = -(offset % stripe_px); y < height; y += stripe_px) {
        const int content_row = (y + offset) / stripe_px;
        rect.bg_color = stripe_colors[content_row % 6];
        lv_area_t area = {
            .x1 = obj_area.x1,
            .x2 = obj_area.x2,
            .y1 = (int32_t)(obj_area.y1 + y),
            .y2 = (int32_t)(obj_area.y1 + y + stripe_px - 1),
        };
        lv_draw_rect(layer, &rect, &area);
    }
}

static void diag_motion_anim_cb(lv_timer_t *timer)
{
    (void)timer;
    const int64_t now_us = esp_timer_get_time();
    if (s_motion.last_anim_us == 0) {
        s_motion.last_anim_us = now_us;
        return;
    }
    const int32_t elapsed_us = (int32_t)(now_us - s_motion.last_anim_us);
    s_motion.last_anim_us = now_us;

    if (s_motion.scroll) {
        s_motion.scroll_position_ups +=
            (int64_t)DIAG_MOTION_SCROLL_PX_S * elapsed_us;
        /* The stripe pattern repeats every 6 stripes; wrap within one
         * period so the offset stays small. */
        const int64_t period_ups =
            (int64_t)(6 * DIAG_MOTION_STRIPE_PX) * 1000000;
        while (s_motion.scroll_position_ups >= period_ups) {
            s_motion.scroll_position_ups -= period_ups;
        }
        s_motion.scroll_offset_px = (int)(s_motion.scroll_position_ups / 1000000);
        /* Stripe geometry changed: repaint the whole parent this frame. */
        lv_obj_invalidate(s_motion.parent);
    }

    const int32_t range_x =
        lv_obj_get_content_width(s_motion.parent) - DIAG_MOTION_BALL_PX;
    const int32_t range_y =
        lv_obj_get_content_height(s_motion.parent) - DIAG_MOTION_BALL_PX;
    s_motion.ball_x_position_ups +=
        s_motion.ball_dir_x * (int64_t)DIAG_MOTION_BALL_X_PX_S * elapsed_us;
    s_motion.ball_y_position_ups +=
        s_motion.ball_dir_y * (int64_t)DIAG_MOTION_BALL_Y_PX_S * elapsed_us;
    const int64_t max_x_ups = (int64_t)range_x * 1000000;
    const int64_t max_y_ups = (int64_t)range_y * 1000000;
    if (s_motion.ball_x_position_ups <= 0 ||
            s_motion.ball_x_position_ups >= max_x_ups) {
        s_motion.ball_dir_x = -s_motion.ball_dir_x;
        s_motion.ball_x_position_ups =
            s_motion.ball_x_position_ups <= 0 ? 0 : max_x_ups;
    }
    if (s_motion.ball_y_position_ups <= 0 ||
            s_motion.ball_y_position_ups >= max_y_ups) {
        s_motion.ball_dir_y = -s_motion.ball_dir_y;
        s_motion.ball_y_position_ups =
            s_motion.ball_y_position_ups <= 0 ? 0 : max_y_ups;
    }
    lv_obj_set_pos(s_motion.ball,
                   (int)(s_motion.ball_x_position_ups / 1000000),
                   (int)(s_motion.ball_y_position_ups / 1000000));
}

/* LVGL-thread teardown shared by the stats timer, the parent DELETE hook
 * and display_diag_motion_stop(). Object children die with the parent;
 * only timers and display event callbacks need explicit removal. */
static void diag_motion_teardown_locked(void)
{
    if (!s_motion.running) {
        return;
    }
    s_motion.running = false;
    if (s_motion.anim_timer != NULL) {
        lv_timer_delete(s_motion.anim_timer);
        s_motion.anim_timer = NULL;
    }
    if (s_motion.stats_timer != NULL) {
        lv_timer_delete(s_motion.stats_timer);
        s_motion.stats_timer = NULL;
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL) {
        lv_display_remove_event_cb_with_user_data(disp, diag_motion_refr_cb,
                                                  NULL);
    }
    /* Mutating a dying object's event list while LVGL dispatches its
     * DELETE event is unsafe; the list is freed with the object. */
    if (s_motion.scroll && s_motion.parent != NULL && !s_motion.parent_dying) {
        lv_obj_remove_event_cb_with_user_data(s_motion.parent,
                                              diag_motion_draw_cb, NULL);
    }
}

static void diag_motion_stats_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_motion.stop_requested) {
        diag_motion_teardown_locked();
        return;
    }
    const uint32_t frames = s_motion.frames_window;
    s_motion.frames_window = 0;
    s_motion.stats.fps_last = frames;
    ++s_motion.stats.seconds;
    if (s_motion.stats.cycles_min == 0 || frames < s_motion.stats.cycles_min) {
        s_motion.stats.cycles_min = frames;
    }
    if (frames > s_motion.stats.cycles_max) {
        s_motion.stats.cycles_max = frames;
    }
    if (s_motion.fps_label != NULL) {
        lv_label_set_text_fmt(s_motion.fps_label, "%" PRIu32 " fps", frames);
    }
}

static void diag_motion_parent_delete_cb(lv_event_t *event)
{
    (void)event;
    /* The host (run-view canvas, app stage, screen) is dying: tear down
     * now so no timer or display callback outlives the objects. */
    s_motion.parent_dying = true;
    diag_motion_teardown_locked();
}

esp_err_t display_diag_motion_start(lv_obj_t *parent, bool scroll)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_motion.running) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (parent == NULL) {
        parent = lv_screen_active();
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_motion.stats, 0, sizeof(s_motion.stats));
    s_motion.parent = parent;
    s_motion.scroll = scroll;
    s_motion.frames_window = 0;
    s_motion.last_anim_us = 0;
    s_motion.scroll_position_ups = 0;
    s_motion.scroll_offset_px = 0;
    s_motion.flushed_in_cycle = false;
    s_motion.stop_requested = false;
    s_motion.parent_dying = false;

    if (scroll) {
        lv_obj_add_event_cb(parent, diag_motion_draw_cb, LV_EVENT_DRAW_MAIN,
                            NULL);
    }

    s_motion.ball = lv_obj_create(parent);
    lv_obj_remove_style_all(s_motion.ball);
    lv_obj_set_size(s_motion.ball, DIAG_MOTION_BALL_PX, DIAG_MOTION_BALL_PX);
    lv_obj_set_style_bg_color(s_motion.ball, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_motion.ball, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_motion.ball, lv_color_black(),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(s_motion.ball, 2, LV_PART_MAIN);
    lv_obj_remove_flag(s_motion.ball,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_motion.ball_x_position_ups = INT64_C(40) * 1000000;
    s_motion.ball_y_position_ups = INT64_C(60) * 1000000;
    s_motion.ball_dir_x = 1;
    s_motion.ball_dir_y = 1;
    lv_obj_set_pos(s_motion.ball, 40, 60);

    s_motion.fps_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_motion.fps_label,
                                lv_color_make(0xFF, 0x40, 0x40), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_motion.fps_label, lv_color_black(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_motion.fps_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_motion.fps_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_motion.fps_label, 8, 8);
    lv_label_set_text(s_motion.fps_label, "-- fps");

    lv_display_add_event_cb(disp, diag_motion_refr_cb, LV_EVENT_FLUSH_START,
                            NULL);
    lv_display_add_event_cb(disp, diag_motion_refr_cb, LV_EVENT_REFR_READY,
                            NULL);
    s_motion.anim_timer = lv_timer_create(diag_motion_anim_cb, 5, NULL);
    s_motion.stats_timer = lv_timer_create(diag_motion_stats_cb, 1000, NULL);
    if (s_motion.anim_timer == NULL || s_motion.stats_timer == NULL) {
        diag_motion_teardown_locked();
        lv_obj_delete(s_motion.ball);
        lv_obj_delete(s_motion.fps_label);
        bsp_display_unlock();
        return ESP_ERR_NO_MEM;
    }
    /* Last line of defense: a dying host always tears the context down. */
    lv_obj_add_event_cb(parent, diag_motion_parent_delete_cb,
                        LV_EVENT_DELETE, NULL);

    s_motion.running = true;
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t display_diag_motion_stop(display_diag_motion_stats_t *out)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_motion.running) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (out != NULL) {
        *out = s_motion.stats;
    }
    diag_motion_teardown_locked();
    bsp_display_unlock();
    return ESP_OK;
}

void display_diag_motion_request_stop(void)
{
    s_motion.stop_requested = true;
}

bool display_diag_motion_wait_stopped(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (s_motion.running && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    return !s_motion.running;
}

bool display_diag_motion_running(void)
{
    return s_motion.running;
}

void display_diag_motion_snapshot(display_diag_motion_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_motion.stats;
}

/* ------------------------------------------------------------------ */
/* Flush counter + invalidation helpers                                */
/* ------------------------------------------------------------------ */

static portMUX_TYPE s_flush_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_flush_count;
static bool s_flush_attached;

static void diag_flush_count_cb(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&s_flush_mux);
    ++s_flush_count;
    portEXIT_CRITICAL(&s_flush_mux);
}

esp_err_t display_diag_flush_counter_attach(void)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL || s_flush_attached) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    lv_display_add_event_cb(disp, diag_flush_count_cb, LV_EVENT_FLUSH_START,
                            NULL);
    s_flush_attached = true;
    portENTER_CRITICAL(&s_flush_mux);
    s_flush_count = 0;
    portEXIT_CRITICAL(&s_flush_mux);
    bsp_display_unlock();
    return ESP_OK;
}

void display_diag_flush_counter_detach(void)
{
    if (!bsp_display_lock(1000)) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL && s_flush_attached) {
        lv_display_remove_event_cb_with_user_data(disp, diag_flush_count_cb,
                                                  NULL);
    }
    s_flush_attached = false;
    bsp_display_unlock();
}

uint32_t display_diag_flush_counter_read(void)
{
    portENTER_CRITICAL(&s_flush_mux);
    const uint32_t count = s_flush_count;
    portEXIT_CRITICAL(&s_flush_mux);
    return count;
}

void display_diag_set_invalidation(bool enable)
{
    if (!bsp_display_lock(1000)) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL) {
        lv_display_enable_invalidation(disp, enable);
    }
    bsp_display_unlock();
}

void display_diag_invalidate_all(void)
{
    if (!bsp_display_lock(1000)) {
        return;
    }
    lv_obj_invalidate(lv_screen_active());
    bsp_display_unlock();
}
