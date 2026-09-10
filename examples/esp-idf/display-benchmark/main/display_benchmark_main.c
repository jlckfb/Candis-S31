/*
 * Candis-S31 display benchmark example.
 *
 * Alternates two refresh regimes on the AMOLED, five seconds each, and
 * prints the achieved average frame rate for every phase:
 *   - "fullscreen": high-contrast scrolling stripes repaint the whole
 *     460x460 panel every frame;
 *   - "partial": a bouncing 48 px ball repaints a bounded region only.
 *
 * Frame counting: LV_EVENT_REFR_READY counts a frame only when at least one
 * LV_EVENT_FLUSH_START happened in that refresh cycle.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp-bsp.h"
#include "example_board.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "display_benchmark";

#define MOTION_STRIPE_PX   20
#define MOTION_BALL_PX     48
#define MOTION_SCROLL_PX_S 180
#define MOTION_BALL_X_PX_S 240
#define MOTION_BALL_Y_PX_S 170
#define PHASE_MS           5000

static struct {
    lv_obj_t *ball;
    lv_obj_t *fps_label;
    lv_timer_t *anim_timer;
    uint32_t frames;
    int64_t first_frame_us;
    int64_t last_frame_us;
    uint32_t frames_window;        /* frames counted since the last stats tick */
    bool flushed_in_cycle;
    int64_t scroll_position_ups;   /* micro-pixels, sub-pixel precision */
    int64_t ball_x_ups;
    int64_t ball_y_ups;
    int64_t last_animation_us;
    int scroll_offset_px;
    int ball_dir_x;
    int ball_dir_y;
    bool scroll;
    bool te_sync;
} s_motion;

/* LVGL display event: REFR_READY counts only cycles that flushed. */
static void refr_cb(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
    case LV_EVENT_FLUSH_START:
        s_motion.flushed_in_cycle = true;
        break;
    case LV_EVENT_REFR_READY:
        if (s_motion.flushed_in_cycle) {
            s_motion.flushed_in_cycle = false;
            ++s_motion.frames;
            ++s_motion.frames_window;
            const int64_t now_us = esp_timer_get_time();
            if (s_motion.frames == 1) {
                s_motion.first_frame_us = now_us;
            }
            s_motion.last_frame_us = now_us;
        }
        break;
    default:
        break;
    }
}

/* High-contrast horizontal stripes drawn straight into the parent draw
 * event, so the full-area render stays inside one frame budget. */
static void stripes_draw_cb(lv_event_t *event)
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

    const int offset = s_motion.scroll_offset_px;
    const int height = obj_area.y2 - obj_area.y1 + 1;
    for (int y = -(offset % MOTION_STRIPE_PX); y < height;
            y += MOTION_STRIPE_PX) {
        const int content_row = (y + offset) / MOTION_STRIPE_PX;
        rect.bg_color = stripe_colors[content_row % 6];
        lv_area_t area = {
            .x1 = obj_area.x1,
            .x2 = obj_area.x2,
            .y1 = (int32_t)(obj_area.y1 + y),
            .y2 = (int32_t)(obj_area.y1 + y + MOTION_STRIPE_PX - 1),
        };
        lv_draw_rect(layer, &rect, &area);
    }
}

/* 1 s on-screen statistics tick. */
static void stats_cb(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t frames = s_motion.frames_window;
    s_motion.frames_window = 0;
    lv_label_set_text_fmt(s_motion.fps_label, "%s | TE %s\n%" PRIu32 " LVGL fps",
                          s_motion.scroll ? "FULL" : "PARTIAL",
                          s_motion.te_sync ? "ON" : "OFF", frames);
}

/* Request work at the RTOS tick resolution. TE and transfer completion,
 * rather than a 30/60 fps timer, determine the measured throughput. */
static void anim_cb(lv_timer_t *timer)
{
    (void)timer;
    const int64_t now_us = esp_timer_get_time();
    if (s_motion.last_animation_us == 0) {
        s_motion.last_animation_us = now_us;
        return;
    }
    const int32_t elapsed_us = (int32_t)(now_us - s_motion.last_animation_us);
    s_motion.last_animation_us = now_us;

    lv_obj_t *screen = lv_screen_active();
    if (s_motion.scroll) {
        s_motion.scroll_position_ups +=
            (int64_t)MOTION_SCROLL_PX_S * elapsed_us;
        const int64_t period_ups =
            (int64_t)(6 * MOTION_STRIPE_PX) * 1000000;
        while (s_motion.scroll_position_ups >= period_ups) {
            s_motion.scroll_position_ups -= period_ups;
        }
        s_motion.scroll_offset_px =
            (int)(s_motion.scroll_position_ups / 1000000);
        lv_obj_invalidate(screen);
    }

    const int32_t range_x =
        lv_obj_get_content_width(screen) - MOTION_BALL_PX;
    const int32_t range_y =
        lv_obj_get_content_height(screen) - MOTION_BALL_PX;
    int64_t x = s_motion.ball_x_ups
            + s_motion.ball_dir_x * (int64_t)MOTION_BALL_X_PX_S * elapsed_us;
    int64_t y = s_motion.ball_y_ups
            + s_motion.ball_dir_y * (int64_t)MOTION_BALL_Y_PX_S * elapsed_us;
    const int64_t max_x = (int64_t)range_x * 1000000;
    const int64_t max_y = (int64_t)range_y * 1000000;
    if (x <= 0 || x >= max_x) {
        s_motion.ball_dir_x = -s_motion.ball_dir_x;
        x = x <= 0 ? 0 : max_x;
    }
    if (y <= 0 || y >= max_y) {
        s_motion.ball_dir_y = -s_motion.ball_dir_y;
        y = y <= 0 ? 0 : max_y;
    }
    s_motion.ball_x_ups = x;
    s_motion.ball_y_ups = y;
    lv_obj_set_pos(s_motion.ball, (int32_t)(x / 1000000),
                   (int32_t)(y / 1000000));
    if (!s_motion.scroll) {
        /* Keep the bounded region dirty even between integer-pixel moves. */
        lv_obj_invalidate(s_motion.ball);
    }
}

/* LVGL-thread phase teardown: the timer list and the event list must not
 * outlive the phase. */
static void phase_teardown(void)
{
    if (s_motion.anim_timer != NULL) {
        lv_timer_delete(s_motion.anim_timer);
        s_motion.anim_timer = NULL;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    if (s_motion.scroll) {
        lv_obj_remove_event_cb_with_user_data(screen, stripes_draw_cb, NULL);
    }
}

typedef struct {
    uint32_t frames;
    int64_t elapsed_us;
    double fps;
} phase_result_t;

static phase_result_t phase_run(bool scroll, bool te_sync)
{
    ESP_ERROR_CHECK(bsp_display_lock(1000) ? ESP_OK : ESP_ERR_TIMEOUT);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101014), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_motion.frames = 0;
    s_motion.frames_window = 0;
    s_motion.flushed_in_cycle = false;
    s_motion.scroll = scroll;
    s_motion.te_sync = te_sync;
    s_motion.scroll_position_ups = 0;
    s_motion.scroll_offset_px = 0;
    s_motion.last_animation_us = 0;

    lv_display_t *disp = lv_display_get_default();
    lv_display_add_event_cb(disp, refr_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, refr_cb, LV_EVENT_REFR_READY, NULL);
    if (scroll) {
        lv_obj_add_event_cb(screen, stripes_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    }

    s_motion.ball = lv_obj_create(screen);
    lv_obj_remove_style_all(s_motion.ball);
    lv_obj_set_size(s_motion.ball, MOTION_BALL_PX, MOTION_BALL_PX);
    lv_obj_set_style_bg_color(s_motion.ball, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_motion.ball, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_motion.ball, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_motion.ball, 2, LV_PART_MAIN);
    lv_obj_remove_flag(s_motion.ball,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_motion.ball, 40, 60);
    s_motion.ball_x_ups = INT64_C(40000000);
    s_motion.ball_y_ups = INT64_C(60000000);
    s_motion.ball_dir_x = 1;
    s_motion.ball_dir_y = 1;

    s_motion.fps_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_motion.fps_label,
                                lv_color_hex(0xFF4040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_motion.fps_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_motion.fps_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(s_motion.fps_label, 8, 8);
    lv_label_set_text_fmt(s_motion.fps_label, "%s | TE %s\n-- LVGL fps",
                          scroll ? "FULL" : "PARTIAL", te_sync ? "ON" : "OFF");

    s_motion.anim_timer = lv_timer_create(anim_cb, 1, NULL);
    lv_timer_t *stats_timer = lv_timer_create(stats_cb, 1000, NULL);
    bsp_display_unlock();

    /* Let the first redraw and TE pipeline settle before starting the
     * measured window; otherwise the setup frame systematically costs one
     * refresh and makes the 60 Hz partial path print 59.8 fps. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_ERROR_CHECK(bsp_display_lock(1000) ? ESP_OK : ESP_ERR_TIMEOUT);
    s_motion.frames = 0;
    s_motion.frames_window = 0;
    s_motion.flushed_in_cycle = false;
    bsp_display_unlock();

    vTaskDelay(pdMS_TO_TICKS(PHASE_MS));

    ESP_ERROR_CHECK(bsp_display_lock(1000) ? ESP_OK : ESP_ERR_TIMEOUT);
    phase_result_t result = {
        .frames = s_motion.frames,
        .elapsed_us = s_motion.last_frame_us - s_motion.first_frame_us,
    };
    phase_teardown();
    lv_timer_delete(stats_timer);
    lv_display_remove_event_cb_with_user_data(disp, refr_cb, NULL);
    bsp_display_unlock();

    if (result.frames > 1 && result.elapsed_us > 0) {
        result.fps = (double)(result.frames - 1) * 1000000.0 / result.elapsed_us;
    }
    return result;
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = true,
        .start_display = true,
        .brightness_percent = 60,
    }));

    ESP_ERROR_CHECK(bsp_display_lock(1000) ? ESP_OK : ESP_ERR_TIMEOUT);
    lv_display_t *display = lv_display_get_default();
    lv_timer_set_period(lv_display_get_refr_timer(display), 1);
    bsp_display_unlock();

    bool te_sync = false;
    for (;;) {
        ESP_ERROR_CHECK(bsp_display_lock(1000) ? ESP_OK : ESP_ERR_TIMEOUT);
        ESP_ERROR_CHECK(lvgl_port_display_te_sync_enable(display, te_sync));
        bsp_display_unlock();
        ESP_LOGI(TAG, "PHASE_BEGIN fullscreen te=%s duration_ms=%u",
                 te_sync ? "on" : "off", PHASE_MS + 500U);
        const phase_result_t fullscreen = phase_run(true, te_sync);
        ESP_LOGI(TAG, "fullscreen te=%s fps=%.3f frames=%" PRIu32
                 " interval_us=%" PRId64 " target=30 result=%s",
                 te_sync ? "on" : "off", fullscreen.fps, fullscreen.frames, fullscreen.elapsed_us,
                 fullscreen.fps >= 30.0 ? "PASS" : "BELOW_TARGET");
        ESP_LOGI(TAG, "PHASE_BEGIN partial te=%s duration_ms=%u",
                 te_sync ? "on" : "off", PHASE_MS + 500U);
        const phase_result_t partial = phase_run(false, te_sync);
        ESP_LOGI(TAG, "partial te=%s fps=%.3f frames=%" PRIu32
                 " interval_us=%" PRId64 " target=60 result=%s",
                 te_sync ? "on" : "off", partial.fps, partial.frames, partial.elapsed_us,
                 partial.fps >= 60.0 ? "PASS" : "BELOW_TARGET");
        te_sync = !te_sync;
    }
}
