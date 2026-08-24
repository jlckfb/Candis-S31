/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

static lv_display_t *s_display;

static void create_display_pattern(void)
{
    static const lv_color_t colors[] = {
        LV_COLOR_MAKE(255, 0, 0), LV_COLOR_MAKE(0, 255, 0),
        LV_COLOR_MAKE(0, 0, 255), LV_COLOR_MAKE(255, 255, 255),
    };
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    for (unsigned index = 0; index < 4; ++index) {
        lv_obj_t *bar = lv_obj_create(screen);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, BSP_LCD_H_RES / 2, BSP_LCD_V_RES / 2);
        lv_obj_set_pos(bar, (index & 1U) ? BSP_LCD_H_RES / 2 : 0,
                       (index & 2U) ? BSP_LCD_V_RES / 2 : 0);
        lv_obj_set_style_bg_color(bar, colors[index], LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    }
}

esp_err_t factory_display_ensure_started(void)
{
    if (s_display == NULL) {
        s_display = bsp_display_start();
    }
    return s_display != NULL ? ESP_OK : ESP_FAIL;
}

esp_err_t factory_display_show_pattern(void)
{
    if (factory_display_ensure_started() != ESP_OK) {
        return ESP_FAIL;
    }
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    create_display_pattern();
    bsp_display_unlock();
    return ESP_OK;
}


bool factory_display_started(void)
{
    return s_display != NULL;
}

esp_err_t factory_display_stop(void)
{
    if (s_display == NULL) {
        return ESP_OK;
    }
    const esp_err_t error = bsp_display_stop();
    if (error == ESP_OK) {
        s_display = NULL;
    }
    return error;
}

static int command_display_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const esp_err_t error = factory_display_show_pattern();
    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_DISPLAY, error, "display start/lock failed");
        return error;
    }
    const char answer = factory_console_ask_operator(
                            "display", "Four-color pattern visible and correct?",
                            OPERATOR_PROMPT_TIMEOUT_S);
    factory_report_operator_verdict(FACTORY_TEST_DISPLAY, answer,
                            "operator confirmed color pattern",
                            "operator rejected color pattern",
                            "color pattern active; inspect panel then use mark");
    return ESP_OK;
}

static int command_display_brightness(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: display_brightness PERCENT\n");
        return ESP_ERR_INVALID_ARG;
    }
    char *end = NULL;
    const long brightness = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || brightness < 0 || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(factory_display_ensure_started(), "factory_display",
                        "display initialization failed");
    return bsp_display_brightness_set((int)brightness);
}

static int command_display_sleep(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "deep") != 0)) {
        printf("usage: display_sleep [deep]\n");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(factory_display_ensure_started(), "factory_display",
                        "display initialization failed");
    return argc == 2 ? bsp_display_enter_deep_standby() :
           bsp_display_enter_sleep();
}

static int command_display_wake(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "deep") != 0)) {
        printf("usage: display_wake [deep]\n");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(factory_display_ensure_started(), "factory_display",
                        "display initialization failed");
    return argc == 2 ? bsp_display_exit_deep_standby() :
           bsp_display_exit_sleep();
}

static int command_display_sleep_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (factory_display_ensure_started() != ESP_OK) {
        factory_report_error(FACTORY_TEST_DISPLAY_SLEEP, ESP_FAIL,
                     "display initialization failed");
        return ESP_FAIL;
    }

    static const struct {
        esp_err_t (*enter)(void);
        esp_err_t (*leave)(void);
        const char *name;
    } stages[] = {
        { bsp_display_enter_sleep, bsp_display_exit_sleep, "sleep" },
        { bsp_display_enter_deep_standby, bsp_display_exit_deep_standby,
          "deep standby" },
    };

    unsigned confirmed = 0;
    for (unsigned index = 0; index < sizeof(stages) / sizeof(stages[0]); ++index) {
        esp_err_t error = factory_display_show_pattern();
        if (error == ESP_OK) {
            error = stages[index].enter();
        }
        if (error != ESP_OK) {
            factory_report_error(FACTORY_TEST_DISPLAY_SLEEP, error,
                         stages[index].name);
            return error;
        }

        char question[96];
        snprintf(question, sizeof(question), "%s: did the panel go dark?",
                 stages[index].name);
        char answer = factory_console_ask_operator("display_sleep", question,
                                                   OPERATOR_PROMPT_TIMEOUT_S);
        error = stages[index].leave();
        if (error != ESP_OK) {
            factory_report_error(FACTORY_TEST_DISPLAY_SLEEP, error,
                         stages[index].name);
            return error;
        }
        if (answer == 'n') {
            char detail[96];
            snprintf(detail, sizeof(detail),
                     "operator reports panel stayed on in %s",
                     stages[index].name);
            factory_report_set(FACTORY_TEST_DISPLAY_SLEEP, FACTORY_STATUS_FAIL,
                               detail);
            factory_report_print_one(FACTORY_TEST_DISPLAY_SLEEP);
            return ESP_FAIL;
        }
        if (answer == 'y') {
            ++confirmed;
        }

        error = factory_display_show_pattern();
        if (error != ESP_OK) {
            factory_report_error(FACTORY_TEST_DISPLAY_SLEEP, error, "pattern redraw failed");
            return error;
        }
        snprintf(question, sizeof(question), "%s: did the pattern return?",
                 stages[index].name);
        answer = factory_console_ask_operator("display_sleep", question,
                                              OPERATOR_PROMPT_TIMEOUT_S);
        if (answer == 'n') {
            char detail[96];
            snprintf(detail, sizeof(detail),
                     "operator reports no recovery after %s", stages[index].name);
            factory_report_set(FACTORY_TEST_DISPLAY_SLEEP, FACTORY_STATUS_FAIL,
                               detail);
            factory_report_print_one(FACTORY_TEST_DISPLAY_SLEEP);
            return ESP_FAIL;
        }
        if (answer == 'y') {
            ++confirmed;
        }
    }

    char detail[96];
    if (confirmed == 4) {
        snprintf(detail, sizeof(detail),
                 "sleep and deep standby cycles visually confirmed");
        factory_report_set(FACTORY_TEST_DISPLAY_SLEEP, FACTORY_STATUS_PASS, detail);
    } else {
        snprintf(detail, sizeof(detail), "confirmed=%u/4; use mark", confirmed);
        factory_report_set(FACTORY_TEST_DISPLAY_SLEEP, FACTORY_STATUS_NOT_RUN,
                           detail);
    }
    factory_report_print_one(FACTORY_TEST_DISPLAY_SLEEP);
    return ESP_OK;
}

/* --- Read-only TE (tearing-effect) probe -------------------------------- */

/* The CO5300 TE line (BSP_LCD_TE) carries one pulse per panel scan. The
 * display is created by esp_lvgl_adapter, whose TE observer is not exposed as
 * a public API. Sample the GPIO directly instead: GPIO inputs can be observed
 * without changing the adapter-owned interrupt configuration or the LVGL
 * object tree. */

#define DISPLAY_TE_WINDOW_MS_DEFAULT 1000
#define DISPLAY_TE_WINDOW_MS_MIN     100
#define DISPLAY_TE_WINDOW_MS_MAX     10000

/* min/avg/max accumulator for one class of complete pulses or periods.
 * Only spans whose both edges fall inside the measurement window are
 * counted, so a window that opens or closes mid-pulse never pollutes the
 * statistics. All arithmetic is 64-bit: the sums are bounded by the
 * window length (at most 10 s = 1e7 us), far from any overflow. */
typedef struct {
    uint32_t count;
    uint64_t total_us;
    int64_t min_us;
    int64_t max_us;
} te_span_t;

typedef struct {
    int64_t last_rise_us;
    int64_t last_fall_us;
    uint32_t rising_edges;
    uint32_t falling_edges;
    te_span_t period; /* rising edge to rising edge */
    te_span_t high;   /* rising edge to falling edge */
    te_span_t low;    /* falling edge to rising edge */
    bool level;       /* level after the most recently observed edge */
    bool have_rise;
    bool have_fall;
} te_probe_t;

static te_probe_t s_te_probe;

static void te_span_reset(te_span_t *span)
{
    span->count = 0;
    span->total_us = 0;
    span->min_us = 0;
    span->max_us = 0;
}

static void te_span_update(te_span_t *span, int64_t delta_us)
{
    ++span->count;
    span->total_us += (uint64_t)delta_us;
    if (span->count == 1 || delta_us < span->min_us) {
        span->min_us = delta_us;
    }
    if (span->count == 1 || delta_us > span->max_us) {
        span->max_us = delta_us;
    }
}

static void display_te_record_edge(bool level, int64_t now_us,
                                   te_probe_t *probe)
{
    const bool rising = level;
    probe->level = rising;
    if (rising) {
        ++probe->rising_edges;
        if (probe->have_fall) {
            te_span_update(&probe->low, now_us - probe->last_fall_us);
        }
        if (probe->have_rise) {
            te_span_update(&probe->period, now_us - probe->last_rise_us);
        }
        probe->last_rise_us = now_us;
        probe->have_rise = true;
    } else {
        ++probe->falling_edges;
        if (probe->have_rise) {
            te_span_update(&probe->high, now_us - probe->last_rise_us);
        }
        probe->last_fall_us = now_us;
        probe->have_fall = true;
    }
}

static void te_span_print(const char *label, const te_span_t *span)
{
    if (span->count == 0) {
        printf("%s_us count=0 min=none avg=none max=none\n", label);
        return;
    }
    printf("%s_us count=%" PRIu32 " min=%" PRId64 " avg=%" PRIu64
           " max=%" PRId64 "\n",
           label, span->count, span->min_us,
           span->total_us / span->count, span->max_us);
}

static void te_span_print_json(const char *label, const te_span_t *span)
{
    if (span->count == 0) {
        printf("\"%s_count\":0,\"%s_min_us\":null,\"%s_avg_us\":null,"
               "\"%s_max_us\":null",
               label, label, label, label);
        return;
    }
    printf("\"%s_count\":%" PRIu32 ",\"%s_min_us\":%" PRId64
           ",\"%s_avg_us\":%" PRIu64 ",\"%s_max_us\":%" PRId64,
           label, span->count, label, span->min_us, label,
           span->total_us / span->count, label, span->max_us);
}

/* Read-only measurement of the TE line; the result is data, not a factory
 * verdict, so nothing is filed into the persistent report. */
static int command_display_te(int argc, char **argv)
{
    if (argc > 2) {
        printf("usage: display_te [WINDOW_MS]\n");
        return ESP_ERR_INVALID_ARG;
    }
    long window_ms = DISPLAY_TE_WINDOW_MS_DEFAULT;
    if (argc == 2) {
        char *end = NULL;
        window_ms = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0') {
            printf("usage: display_te [WINDOW_MS]\n");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (window_ms < DISPLAY_TE_WINDOW_MS_MIN ||
            window_ms > DISPLAY_TE_WINDOW_MS_MAX) {
        printf("usage: display_te [WINDOW_MS]\n");
        printf("WINDOW_MS is milliseconds, %d-%d, default %d\n",
               DISPLAY_TE_WINDOW_MS_MIN, DISPLAY_TE_WINDOW_MS_MAX,
               DISPLAY_TE_WINDOW_MS_DEFAULT);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(factory_display_ensure_started(), "factory_display_te",
                        "display start failed");

    s_te_probe.last_rise_us = 0;
    s_te_probe.last_fall_us = 0;
    s_te_probe.rising_edges = 0;
    s_te_probe.falling_edges = 0;
    te_span_reset(&s_te_probe.period);
    te_span_reset(&s_te_probe.high);
    te_span_reset(&s_te_probe.low);
    s_te_probe.have_rise = false;
    s_te_probe.have_fall = false;
    const int initial_level = gpio_get_level(BSP_LCD_TE);
    s_te_probe.level = initial_level != 0;

    int sampled_level = initial_level;
    const int64_t deadline_us =
        esp_timer_get_time() + window_ms * INT64_C(1000);
    int64_t last_yield_us = esp_timer_get_time();
    int64_t now_us = last_yield_us;
    while (now_us < deadline_us) {
        const int level = gpio_get_level(BSP_LCD_TE);
        if (level != sampled_level) {
            sampled_level = level;
            display_te_record_edge(level != 0, now_us, &s_te_probe);
        }
        now_us = esp_timer_get_time();
        /* Let the idle task run often enough to service the task watchdog.
         * A one-tick gap every 50 ms may omit an individual pulse, which is
         * visible as a doubled period instead of corrupting GPIO ownership. */
        if (now_us - last_yield_us >= 50000) {
            vTaskDelay(1);
            last_yield_us = esp_timer_get_time();
            now_us = last_yield_us;
        }
    }
    const int final_level = gpio_get_level(BSP_LCD_TE);
    const te_probe_t probe = s_te_probe;

    printf("TE probe on GPIO%d, window=%ld ms\n", (int)BSP_LCD_TE, window_ms);
    printf("initial_level=%d final_level=%d\n", initial_level, final_level);
    printf("rising_edges=%" PRIu32 " falling_edges=%" PRIu32 "\n",
           probe.rising_edges, probe.falling_edges);
    te_span_print("period", &probe.period);
    te_span_print("high", &probe.high);
    te_span_print("low", &probe.low);
    /* Duty over the counted complete pulses: a window that ends mid-pulse
     * excludes that partial time, so short windows carry up to one pulse
     * of duty bias. */
    const uint64_t high_us = probe.high.total_us;
    const uint64_t low_us = probe.low.total_us;
    const bool have_duty = high_us + low_us != 0;
    if (have_duty) {
        printf("duty_milli_percent=%" PRIu32 "\n",
               (uint32_t)(high_us * 100000U / (high_us + low_us)));
    } else {
        printf("duty_milli_percent=none\n");
    }

    fputs("FACTORY_TE {\"gpio\":", stdout);
    printf("%d,\"window_ms\":%ld,\"initial_level\":%d,\"final_level\":%d,"
           "\"rising_edges\":%" PRIu32 ",\"falling_edges\":%" PRIu32 ",",
           (int)BSP_LCD_TE, window_ms, initial_level, final_level,
           probe.rising_edges, probe.falling_edges);
    te_span_print_json("period", &probe.period);
    putchar(',');
    te_span_print_json("high", &probe.high);
    putchar(',');
    te_span_print_json("low", &probe.low);
    fputs(",\"duty_milli_percent\":", stdout);
    if (have_duty) {
        printf("%" PRIu32, (uint32_t)(high_us * 100000U / (high_us + low_us)));
    } else {
        fputs("null", stdout);
    }
    fputs("}\n", stdout);
    return ESP_OK;
}

/* ---- display_motion: continuous-motion smoothness/tearing demonstration ----
 *
 * Two modes exercise the two refresh regimes of the TE-synchronized QSPI
 * path: "full" scrolls high-contrast horizontal stripes (every pixel dirty
 * each frame, DMA-bound) while "ball" moves only a small block over a static
 * background (partial updates). Refresh cycles are counted through the
 * LVGL LV_EVENT_REFR_FINISH event and printed once per second, so the
 * console log doubles as the objective frame-rate record. The command is a
 * diagnostic demo: it files no factory report entry. */

#define DISPLAY_MOTION_STRIPE_PX      20
#define DISPLAY_MOTION_BALL_PX        48
#define DISPLAY_MOTION_SCROLL_PX_S    180
#define DISPLAY_MOTION_BALL_X_PX_S    240
#define DISPLAY_MOTION_BALL_Y_PX_S    170
#define DISPLAY_MOTION_SECONDS_DEFAULT 10
#define DISPLAY_MOTION_SECONDS_MIN    2
#define DISPLAY_MOTION_SECONDS_MAX    300
#define DISPLAY_MOTION_LOCK_TIMEOUT_MS 1000
#define DISPLAY_MOTION_CLEANUP_LOCK_ATTEMPTS 3

#define DISPLAY_SLEEP_STRESS_CYCLES_DEFAULT 5
#define DISPLAY_SLEEP_STRESS_CYCLES_MIN     1
#define DISPLAY_SLEEP_STRESS_CYCLES_MAX     100
#define DISPLAY_SLEEP_STRESS_WARMUP_MS      500
#define DISPLAY_SLEEP_STRESS_ASLEEP_MS      250
#define DISPLAY_SLEEP_STRESS_AWAKE_MS       500
#define DISPLAY_SLEEP_STRESS_RECOVERY_MS    50

typedef struct {
    lv_obj_t *ball;
    lv_obj_t *fps_label;
    lv_timer_t *anim_timer;
    lv_timer_t *report_timer;
    uint32_t frames_window; /* refresh cycles since the last report */
    uint32_t frames_total;
    uint32_t cycles_min;    /* min frames in any 1 s window */
    uint32_t cycles_max;
    int64_t next_report_us;
    int64_t last_anim_us;
    int64_t scroll_position_ups; /* micro-pixels, preserves sub-pixel motion */
    int64_t ball_x_position_ups;
    int64_t ball_y_position_ups;
    int scroll_offset_px;
    int ball_x_px;
    int ball_y_px;
    int ball_dir_x;
    int ball_dir_y;
    bool scroll;
    bool running;
    bool flushed_in_cycle;
} display_motion_ctx_t;

typedef struct {
    uint32_t frames_total;
    uint32_t cycles_min;
    uint32_t cycles_max;
} display_motion_result_t;

static display_motion_ctx_t s_motion;

static void display_motion_refr_cb(lv_event_t *event)
{
    /* LV_EVENT_REFR_READY also fires on cycles that rendered nothing, so a
     * frame only counts when at least one flush happened in the cycle. */
    switch (lv_event_get_code(event)) {
    case LV_EVENT_FLUSH_START:
        s_motion.flushed_in_cycle = true;
        break;
    case LV_EVENT_REFR_READY:
        if (s_motion.flushed_in_cycle) {
            s_motion.flushed_in_cycle = false;
            s_motion.frames_window++;
            s_motion.frames_total++;
        }
        break;
    default:
        break;
    }
}

/* High-contrast horizontal stripes drawn straight into the screen's draw
 * event: one cheap rectangle fill per stripe instead of a large child-object
 * tree, so the full-screen render stays well inside one frame budget. */
static void display_motion_draw_cb(lv_event_t *event)
{
    static const lv_color_t stripe_colors[] = {
        LV_COLOR_MAKE(255, 255, 255), LV_COLOR_MAKE(0, 0, 0),
        LV_COLOR_MAKE(255, 0, 0), LV_COLOR_MAKE(0, 255, 0),
        LV_COLOR_MAKE(0, 0, 255), LV_COLOR_MAKE(255, 255, 0),
    };
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *screen = lv_event_get_target_obj(event);
    lv_area_t screen_area;
    lv_obj_get_coords(screen, &screen_area);

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_COVER;

    const int stripe_px = DISPLAY_MOTION_STRIPE_PX;
    const int offset = s_motion.scroll_offset_px;
    for (int y = -(offset % stripe_px); y < BSP_LCD_V_RES; y += stripe_px) {
        const int content_row = (y + offset) / stripe_px;
        rect.bg_color = stripe_colors[content_row % 6];
        lv_area_t area = {
            .x1 = screen_area.x1,
            .x2 = screen_area.x2,
            .y1 = (int32_t)(screen_area.y1 + y),
            .y2 = (int32_t)(screen_area.y1 + y + stripe_px - 1),
        };
        lv_draw_rect(layer, &rect, &area);
    }
}

static void display_motion_anim_cb(lv_timer_t *timer)
{
    display_motion_ctx_t *ctx = lv_timer_get_user_data(timer);
    const int64_t now_us = esp_timer_get_time();
    if (ctx->last_anim_us == 0) {
        ctx->last_anim_us = now_us;
        return;
    }
    const int32_t elapsed_us = (int32_t)(now_us - ctx->last_anim_us);
    ctx->last_anim_us = now_us;

    if (ctx->scroll) {
        ctx->scroll_position_ups +=
            (int64_t)DISPLAY_MOTION_SCROLL_PX_S * elapsed_us;
        /* The stripe pattern repeats every 6 stripes; wrap within one
         * period so the offset stays small. */
        const int64_t period_ups =
            (int64_t)(6 * DISPLAY_MOTION_STRIPE_PX) * 1000000;
        while (ctx->scroll_position_ups >= period_ups) {
            ctx->scroll_position_ups -= period_ups;
        }
        ctx->scroll_offset_px = (int)(ctx->scroll_position_ups / 1000000);
        /* Stripe geometry changed: repaint the whole screen this frame. */
        lv_obj_invalidate(lv_screen_active());
    }

    ctx->ball_x_position_ups += ctx->ball_dir_x *
        (int64_t)DISPLAY_MOTION_BALL_X_PX_S * elapsed_us;
    ctx->ball_y_position_ups += ctx->ball_dir_y *
        (int64_t)DISPLAY_MOTION_BALL_Y_PX_S * elapsed_us;
    const int64_t max_x_ups =
        (int64_t)(BSP_LCD_H_RES - DISPLAY_MOTION_BALL_PX) * 1000000;
    const int64_t max_y_ups =
        (int64_t)(BSP_LCD_V_RES - DISPLAY_MOTION_BALL_PX) * 1000000;
    if (ctx->ball_x_position_ups <= 0 ||
            ctx->ball_x_position_ups >= max_x_ups) {
        ctx->ball_dir_x = -ctx->ball_dir_x;
        ctx->ball_x_position_ups =
            ctx->ball_x_position_ups <= 0 ? 0 : max_x_ups;
    }
    if (ctx->ball_y_position_ups <= 0 ||
            ctx->ball_y_position_ups >= max_y_ups) {
        ctx->ball_dir_y = -ctx->ball_dir_y;
        ctx->ball_y_position_ups =
            ctx->ball_y_position_ups <= 0 ? 0 : max_y_ups;
    }
    ctx->ball_x_px = (int)(ctx->ball_x_position_ups / 1000000);
    ctx->ball_y_px = (int)(ctx->ball_y_position_ups / 1000000);
    lv_obj_set_pos(ctx->ball, ctx->ball_x_px, ctx->ball_y_px);
}

static void display_motion_report_cb(lv_timer_t *timer)
{
    display_motion_ctx_t *ctx = lv_timer_get_user_data(timer);
    const uint32_t frames = ctx->frames_window;
    ctx->frames_window = 0;
    if (ctx->cycles_min == 0 || frames < ctx->cycles_min) {
        ctx->cycles_min = frames;
    }
    if (frames > ctx->cycles_max) {
        ctx->cycles_max = frames;
    }
    lv_label_set_text_fmt(ctx->fps_label, "%" PRIu32 " fps", frames);

    /* LVGL task CPU occupancy over the past report second: this callback
     * runs inside taskLVGL, so the delta covers render + flush + timers. */
    static uint32_t s_prev_lvgl_run, s_prev_total_run;
    static TaskStatus_t s_statuses[24];
    uint32_t total_run = 0;
    const UBaseType_t count = uxTaskGetSystemState(s_statuses,
                                                   sizeof(s_statuses) / sizeof(s_statuses[0]),
                                                   &total_run);
    uint32_t lvgl_run = 0;
    for (UBaseType_t i = 0; i < count; ++i) {
        if (strcmp(s_statuses[i].pcTaskName, "taskLVGL") == 0) {
            lvgl_run = s_statuses[i].ulRunTimeCounter;
            break;
        }
    }
    const uint32_t total_delta = total_run - s_prev_total_run;
    if (s_prev_total_run != 0 && total_delta != 0) {
        const uint32_t pct_x100 = (lvgl_run - s_prev_lvgl_run) * 10000U / total_delta;
        printf("display_motion: fps=%" PRIu32 " total=%" PRIu32 " lvgl_cpu=%u.%02u%%\n",
               frames, ctx->frames_total, (unsigned)(pct_x100 / 100), (unsigned)(pct_x100 % 100));
    } else {
        printf("display_motion: fps=%" PRIu32 " total=%" PRIu32 "\n", frames,
               ctx->frames_total);
    }
    s_prev_lvgl_run = lvgl_run;
    s_prev_total_run = total_run;
}

/* All LVGL object/event/timer ownership stays behind the BSP LVGL lock.
 * The animation callbacks themselves execute in taskLVGL.  Keeping this
 * setup shared prevents the sleep stress command from drifting away from
 * the standalone motion test that it is intended to exercise. */
static esp_err_t display_motion_start(bool scroll)
{
    if (s_motion.running) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(factory_display_ensure_started(), "factory_display",
                        "display initialization failed");
    if (!bsp_display_lock(DISPLAY_MOTION_LOCK_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    memset(&s_motion, 0, sizeof(s_motion));
    s_motion.scroll = scroll;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);

    /* High-contrast horizontal stripes scrolling vertically make a tear
     * line visible immediately: any scan/write race shows as a horizontal
     * offset in the stripe edges. Drawn in the screen draw event. */
    if (scroll) {
        lv_obj_add_event_cb(screen, display_motion_draw_cb, LV_EVENT_DRAW_MAIN,
                            NULL);
    }

    s_motion.ball = lv_obj_create(screen);
    lv_obj_remove_style_all(s_motion.ball);
    lv_obj_set_size(s_motion.ball, DISPLAY_MOTION_BALL_PX,
                    DISPLAY_MOTION_BALL_PX);
    lv_obj_set_style_bg_color(s_motion.ball, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_motion.ball, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_motion.ball, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_motion.ball, 2, LV_PART_MAIN);
    s_motion.ball_x_px = 40;
    s_motion.ball_y_px = 60;
    s_motion.ball_x_position_ups = (int64_t)s_motion.ball_x_px * 1000000;
    s_motion.ball_y_position_ups = (int64_t)s_motion.ball_y_px * 1000000;
    s_motion.ball_dir_x = 1;
    s_motion.ball_dir_y = 1;
    lv_obj_set_pos(s_motion.ball, s_motion.ball_x_px, s_motion.ball_y_px);

    s_motion.fps_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_motion.fps_label,
                                lv_color_make(0xFF, 0x40, 0x40), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_motion.fps_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_motion.fps_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(s_motion.fps_label, 8, 8);
    lv_label_set_text(s_motion.fps_label, "-- fps");

    lv_display_add_event_cb(s_display, display_motion_refr_cb,
                            LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(s_display, display_motion_refr_cb,
                            LV_EVENT_REFR_READY, NULL);
    s_motion.anim_timer = lv_timer_create(display_motion_anim_cb, 5, &s_motion);
    s_motion.report_timer = lv_timer_create(display_motion_report_cb, 1000,
                                            &s_motion);
    if (s_motion.anim_timer == NULL || s_motion.report_timer == NULL) {
        if (s_motion.anim_timer != NULL) {
            lv_timer_delete(s_motion.anim_timer);
        }
        if (s_motion.report_timer != NULL) {
            lv_timer_delete(s_motion.report_timer);
        }
        lv_display_remove_event_cb_with_user_data(s_display,
                                                  display_motion_refr_cb, NULL);
        if (scroll) {
            lv_obj_remove_event_cb_with_user_data(screen,
                                                  display_motion_draw_cb, NULL);
        }
        create_display_pattern();
        memset(&s_motion, 0, sizeof(s_motion));
        bsp_display_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_motion.running = true;
    bsp_display_unlock();
    return ESP_OK;
}

static bool display_motion_cleanup_lock(void)
{
    for (unsigned attempt = 0;
            attempt < DISPLAY_MOTION_CLEANUP_LOCK_ATTEMPTS; ++attempt) {
        if (bsp_display_lock(DISPLAY_MOTION_LOCK_TIMEOUT_MS)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static esp_err_t display_motion_stop(display_motion_result_t *result)
{
    if (!s_motion.running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!display_motion_cleanup_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    s_motion.running = false;
    if (s_motion.anim_timer != NULL) {
        lv_timer_delete(s_motion.anim_timer);
        s_motion.anim_timer = NULL;
    }
    if (s_motion.report_timer != NULL) {
        lv_timer_delete(s_motion.report_timer);
        s_motion.report_timer = NULL;
    }
    lv_display_remove_event_cb_with_user_data(s_display, display_motion_refr_cb,
                                              NULL);
    if (s_motion.scroll) {
        lv_obj_remove_event_cb_with_user_data(lv_screen_active(),
                                              display_motion_draw_cb, NULL);
    }
    if (result != NULL) {
        result->frames_total = s_motion.frames_total;
        result->cycles_min = s_motion.cycles_min;
        result->cycles_max = s_motion.cycles_max;
    }
    create_display_pattern();
    bsp_display_unlock();
    return ESP_OK;
}

static esp_err_t display_motion_snapshot(uint32_t *frames_total)
{
    if (frames_total == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_display_lock(DISPLAY_MOTION_LOCK_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_motion.running) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    *frames_total = s_motion.frames_total;
    bsp_display_unlock();
    return ESP_OK;
}

static int command_display_motion(int argc, char **argv)
{
    long seconds = DISPLAY_MOTION_SECONDS_DEFAULT;
    bool scroll = true;
    if (argc > 3) {
        printf("usage: display_motion [SECONDS] [full|ball]\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (argc >= 2) {
        char *end = NULL;
        seconds = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || seconds < DISPLAY_MOTION_SECONDS_MIN ||
                seconds > DISPLAY_MOTION_SECONDS_MAX) {
            printf("usage: display_motion [SECONDS %d-%d] [full|ball]\n",
                   DISPLAY_MOTION_SECONDS_MIN, DISPLAY_MOTION_SECONDS_MAX);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (argc == 3) {
        if (strcmp(argv[2], "full") == 0) {
            scroll = true;
        } else if (strcmp(argv[2], "ball") == 0) {
            scroll = false;
        } else {
            printf("usage: display_motion [SECONDS] [full|ball]\n");
            return ESP_ERR_INVALID_ARG;
        }
    }
    const esp_err_t start_error = display_motion_start(scroll);
    if (start_error != ESP_OK) {
        if (start_error == ESP_ERR_INVALID_STATE) {
            printf("display_motion already running\n");
        }
        return start_error;
    }

    printf("display_motion: %s mode for %ld s; watch the stripe edges for "
           "tearing\n", scroll ? "full-screen scroll" : "ball-only", seconds);
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));

    display_motion_result_t result = {0};
    const esp_err_t stop_error = display_motion_stop(&result);
    if (stop_error != ESP_OK) {
        return stop_error;
    }
    const float avg_fps = (float)result.frames_total / (float)seconds;

    printf("display_motion done: avg_fps=%.1f min=%" PRIu32 " max=%" PRIu32
           " frames=%" PRIu32 "\n", avg_fps, result.cycles_min,
           result.cycles_max, result.frames_total);
    fputs("FACTORY_TE_MOTION {\"avg_fps\":", stdout);
    printf("%.1f,\"min_fps\":%" PRIu32 ",\"max_fps\":%" PRIu32
           ",\"frames\":%" PRIu32 ",\"seconds\":%ld,\"mode\":\"%s\"}\n",
           avg_fps, result.cycles_min, result.cycles_max,
           result.frames_total, seconds,
           scroll ? "full" : "ball");
    return ESP_OK;
}

/* Recovery is intentionally more defensive for a failed deep-standby entry:
 * enter_deep_standby() first completes normal sleep and only then sends 0x4F.
 * If that final step fails, the BSP remains in normal sleep and rejects the
 * deep exit with ESP_ERR_INVALID_STATE.  In that case a successful normal
 * wake is the authoritative recovery result. */
static esp_err_t display_sleep_stress_recover(bool deep)
{
    if (!deep) {
        return bsp_display_exit_sleep();
    }

    const esp_err_t deep_error = bsp_display_exit_deep_standby();
    if (deep_error == ESP_OK) {
        return ESP_OK;
    }

    const esp_err_t sleep_error = bsp_display_exit_sleep();
    if (sleep_error == ESP_OK) {
        return ESP_OK;
    }
    /* A concrete deep-wake failure is more useful than the fallback error.
     * INVALID_STATE only says the panel was not in deep standby, so retain
     * the normal-wake failure in that expected fallback case. */
    return deep_error != ESP_ERR_INVALID_STATE ? deep_error : sleep_error;
}

static int command_display_sleep_stress(int argc, char **argv)
{
    long cycles = DISPLAY_SLEEP_STRESS_CYCLES_DEFAULT;
    bool deep = false;
    if (argc > 3) {
        printf("usage: display_sleep_stress [CYCLES 1-100] [deep]\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (argc >= 2) {
        char *end = NULL;
        cycles = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' ||
                cycles < DISPLAY_SLEEP_STRESS_CYCLES_MIN ||
                cycles > DISPLAY_SLEEP_STRESS_CYCLES_MAX) {
            printf("usage: display_sleep_stress [CYCLES %d-%d] [deep]\n",
                   DISPLAY_SLEEP_STRESS_CYCLES_MIN,
                   DISPLAY_SLEEP_STRESS_CYCLES_MAX);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (argc == 3) {
        if (strcmp(argv[2], "deep") != 0) {
            printf("usage: display_sleep_stress [CYCLES] [deep]\n");
            return ESP_ERR_INVALID_ARG;
        }
        deep = true;
    }

    const char *mode = deep ? "deep" : "normal";
    const esp_err_t start_error = display_motion_start(false);
    if (start_error != ESP_OK) {
        if (start_error == ESP_ERR_INVALID_STATE) {
            printf("display_sleep_stress: another motion test is running\n");
        }
        return start_error;
    }

    printf("display_sleep_stress: mode=%s cycles=%ld; keep sliding on the "
           "touch panel while the ball moves\n", mode, cycles);
    const int64_t test_start_us = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_SLEEP_STRESS_WARMUP_MS));

    esp_err_t first_error = ESP_OK;
    uint32_t frames_before = 0;
    esp_err_t snapshot_error = display_motion_snapshot(&frames_before);
    if (snapshot_error != ESP_OK) {
        first_error = snapshot_error;
    }

    unsigned attempted = 0;
    unsigned completed = 0;
    bool panel_awake = true;
    for (long cycle = 1; cycle <= cycles && first_error == ESP_OK; ++cycle) {
        ++attempted;
        const int64_t cycle_start_us = esp_timer_get_time();
        const int64_t enter_start_us = cycle_start_us;
        const esp_err_t enter_error = deep ?
                                      bsp_display_enter_deep_standby() :
                                      bsp_display_enter_sleep();
        const int64_t enter_ms =
            (esp_timer_get_time() - enter_start_us) / INT64_C(1000);

        /* Even an error can occur after SLPIN succeeded (for example while
         * putting touch to sleep), so conservatively require wake recovery. */
        panel_awake = false;
        bool leave_attempted = false;
        esp_err_t leave_error = ESP_OK;
        int64_t leave_ms = 0;
        unsigned asleep_hold_ms = 0;
        if (enter_error == ESP_OK) {
            asleep_hold_ms = DISPLAY_SLEEP_STRESS_ASLEEP_MS;
            vTaskDelay(pdMS_TO_TICKS(asleep_hold_ms));
            leave_attempted = true;
            const int64_t leave_start_us = esp_timer_get_time();
            leave_error = deep ? bsp_display_exit_deep_standby() :
                          bsp_display_exit_sleep();
            leave_ms = (esp_timer_get_time() - leave_start_us) /
                       INT64_C(1000);
            panel_awake = leave_error == ESP_OK;
        }

        bool recovery_attempted = false;
        esp_err_t recovery_error = ESP_OK;
        if (!panel_awake) {
            recovery_attempted = true;
            vTaskDelay(pdMS_TO_TICKS(DISPLAY_SLEEP_STRESS_RECOVERY_MS));
            recovery_error = display_sleep_stress_recover(deep);
            panel_awake = recovery_error == ESP_OK;
        }

        unsigned awake_hold_ms = 0;
        uint32_t frames_after = frames_before;
        bool frames_valid = false;
        snapshot_error = ESP_OK;
        if (panel_awake) {
            awake_hold_ms = DISPLAY_SLEEP_STRESS_AWAKE_MS;
            vTaskDelay(pdMS_TO_TICKS(awake_hold_ms));
            snapshot_error = display_motion_snapshot(&frames_after);
            frames_valid = snapshot_error == ESP_OK;
        }
        const uint32_t frames_delta = frames_valid ?
                                      frames_after - frames_before : 0;

        esp_err_t cycle_error = enter_error;
        if (cycle_error == ESP_OK && leave_error != ESP_OK) {
            cycle_error = leave_error;
        }
        if (cycle_error == ESP_OK && recovery_error != ESP_OK) {
            cycle_error = recovery_error;
        }
        if (cycle_error == ESP_OK && snapshot_error != ESP_OK) {
            cycle_error = snapshot_error;
        }
        if (cycle_error == ESP_OK && frames_valid && frames_delta == 0) {
            /* A successful API wake is insufficient if LVGL produced no
             * actual flush during the full awake observation window. */
            cycle_error = ESP_ERR_TIMEOUT;
        }
        const bool passed = cycle_error == ESP_OK;
        if (passed) {
            ++completed;
        }

        const int64_t cycle_ms =
            (esp_timer_get_time() - cycle_start_us) / INT64_C(1000);
        printf("display_sleep_stress: cycle=%ld/%ld mode=%s enter=%s "
               "leave=%s recovery=%s frames_delta=%" PRIu32
               " cycle_error=%s status=%s\n",
               cycle, cycles, mode, esp_err_to_name(enter_error),
               leave_attempted ? esp_err_to_name(leave_error) : "not-run",
               recovery_attempted ? esp_err_to_name(recovery_error) :
               "not-run", frames_delta, esp_err_to_name(cycle_error),
               passed ? "pass" : "fail");
        printf("FACTORY_DISPLAY_SLEEP_STRESS_CYCLE {\"cycle\":%ld,"
               "\"requested\":%ld,\"mode\":\"%s\",\"enter_err\":%d,"
               "\"enter_ms\":%" PRId64 ",\"asleep_hold_ms\":%u,"
               "\"leave_attempted\":%s,\"leave_err\":%d,"
               "\"leave_ms\":%" PRId64 ",\"recovery_attempted\":%s,"
               "\"recovery_err\":%d,\"awake_hold_ms\":%u,"
               "\"frames_valid\":%s,\"frames_delta\":%" PRIu32 ","
               "\"frames_total\":%" PRIu32 ",\"cycle_err\":%d,"
               "\"cycle_ms\":%" PRId64 ","
               "\"status\":\"%s\"}\n",
               cycle, cycles, mode, (int)enter_error, enter_ms,
               asleep_hold_ms, leave_attempted ? "true" : "false",
               (int)leave_error, leave_ms,
               recovery_attempted ? "true" : "false", (int)recovery_error,
               awake_hold_ms, frames_valid ? "true" : "false", frames_delta,
               frames_after, (int)cycle_error, cycle_ms,
               passed ? "pass" : "fail");

        if (frames_valid) {
            frames_before = frames_after;
        }
        if (!passed) {
            first_error = cycle_error;
            break;
        }
    }

    bool final_recovery_attempted = false;
    esp_err_t final_recovery_error = ESP_OK;
    if (!panel_awake) {
        final_recovery_attempted = true;
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_SLEEP_STRESS_RECOVERY_MS));
        final_recovery_error = display_sleep_stress_recover(deep);
        panel_awake = final_recovery_error == ESP_OK;
        if (first_error == ESP_OK && final_recovery_error != ESP_OK) {
            first_error = final_recovery_error;
        }
    }

    display_motion_result_t result = {0};
    esp_err_t cleanup_error = display_motion_stop(&result);
    const esp_err_t first_cleanup_error = cleanup_error;
    if (cleanup_error != ESP_OK) {
        /* A panel recovery can unblock a flush that owned the LVGL mutex.
         * Make one final bounded cleanup attempt so a failed diagnostic does
         * not normally leave its timers or event callbacks installed. */
        if (!panel_awake) {
            final_recovery_attempted = true;
            final_recovery_error = display_sleep_stress_recover(deep);
            panel_awake = final_recovery_error == ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_SLEEP_STRESS_RECOVERY_MS));
        cleanup_error = display_motion_stop(&result);
    }
    if (first_error == ESP_OK && first_cleanup_error != ESP_OK) {
        first_error = first_cleanup_error;
    }

    esp_err_t redraw_error = ESP_OK;
    if (cleanup_error == ESP_OK && !panel_awake) {
        final_recovery_attempted = true;
        final_recovery_error = display_sleep_stress_recover(deep);
        panel_awake = final_recovery_error == ESP_OK;
        if (panel_awake) {
            redraw_error = factory_display_show_pattern();
        }
    }
    if (first_error == ESP_OK && final_recovery_error != ESP_OK) {
        first_error = final_recovery_error;
    }
    if (first_error == ESP_OK && cleanup_error != ESP_OK) {
        first_error = cleanup_error;
    }
    if (first_error == ESP_OK && redraw_error != ESP_OK) {
        first_error = redraw_error;
    }

    const int64_t elapsed_ms =
        (esp_timer_get_time() - test_start_us) / INT64_C(1000);
    const bool passed = first_error == ESP_OK && completed == (unsigned)cycles &&
                        cleanup_error == ESP_OK && panel_awake;
    printf("display_sleep_stress done: mode=%s completed=%u/%ld frames=%" PRIu32
           " cleanup=%s awake=%s elapsed_ms=%" PRId64 " status=%s\n",
           mode, completed, cycles, result.frames_total,
           esp_err_to_name(cleanup_error), panel_awake ? "yes" : "no",
           elapsed_ms, passed ? "pass" : "fail");
    printf("FACTORY_DISPLAY_SLEEP_STRESS {\"mode\":\"%s\","
           "\"requested\":%ld,\"attempted\":%u,\"completed\":%u,"
           "\"frames\":%" PRIu32 ",\"min_fps\":%" PRIu32 ","
           "\"max_fps\":%" PRIu32 ",\"first_err\":%d,"
           "\"first_cleanup_err\":%d,\"cleanup_err\":%d,"
           "\"final_recovery_attempted\":%s,"
           "\"final_recovery_err\":%d,\"redraw_err\":%d,"
           "\"panel_awake\":%s,\"elapsed_ms\":%" PRId64 ","
           "\"status\":\"%s\"}\n",
           mode, cycles, attempted, completed, result.frames_total,
           result.cycles_min, result.cycles_max, (int)first_error,
           (int)first_cleanup_error, (int)cleanup_error,
           final_recovery_attempted ? "true" : "false",
           (int)final_recovery_error, (int)redraw_error,
           panel_awake ? "true" : "false", elapsed_ms,
           passed ? "pass" : "fail");
    if (passed) {
        return ESP_OK;
    }
    return first_error != ESP_OK ? first_error : ESP_FAIL;
}


esp_err_t factory_display_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "display_test", .help = "Show a four-color AMOLED inspection pattern.", .func = command_display_test},
        {.command = "display_brightness", .help = "Set AMOLED brightness from 0 to 100 percent.", .func = command_display_brightness},
        {.command = "display_sleep", .help = "Enter AMOLED sleep or deep standby: display_sleep [deep].", .func = command_display_sleep},
        {.command = "display_wake", .help = "Wake AMOLED from sleep or deep standby: display_wake [deep].", .func = command_display_wake},
        {.command = "display_sleep_test", .help = "Cycle AMOLED sleep and deep standby with operator checks.", .func = command_display_sleep_test},
        {.command = "display_te", .help = "Measure panel TE edges, starting the display if needed: display_te [WINDOW_MS 100-10000].", .func = command_display_te},
        {.command = "display_motion", .help = "Run a continuous-motion tearing/FPS demo: display_motion [SECONDS 2-300] [full|ball].", .func = command_display_motion},
        {.command = "display_sleep_stress", .help = "Stress motion/touch during repeated panel sleep/wake: display_sleep_stress [CYCLES 1-100] [deep].", .func = command_display_sleep_stress},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
