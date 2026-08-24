/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

#define TOUCH_TEST_SECONDS        15
/* Corner acceptance zone for the ordered test; the reference crosses are
 * inset from the edges because the panel corners are physically rounded. */
#define TOUCH_TEST_ZONE_PX        (BSP_LCD_H_RES / 4)
#define TOUCH_CORNER_INSET_PX     24

#define TOUCH_DRAW_DEFAULT_SECONDS 60
#define TOUCH_DRAW_MIN_SECONDS    5
#define TOUCH_DRAW_MAX_SECONDS    300
/* Re-print coordinates only after this much movement or time, so a held
 * finger does not flood the console. */
#define TOUCH_DRAW_PRINT_DELTA_PX 8
#define TOUCH_DRAW_PRINT_PERIOD_US 150000
#define TOUCH_DRAW_MARKER_RADIUS  12
#define TOUCH_LATENCY_RESERVOIR_SAMPLES 1024
#define TOUCH_LATENCY_PENDING_TE_MAX 8

typedef struct {
    uint32_t count;
    uint32_t stored;
    uint32_t min_us;
    uint32_t max_us;
    uint64_t sum_us;
    uint32_t rng;
    uint32_t samples[TOUCH_LATENCY_RESERVOIR_SAMPLES];
} touch_latency_stat_t;

typedef struct {
    int64_t flush_us;
    int64_t irq_us;
} touch_latency_te_pending_t;

typedef struct {
    portMUX_TYPE mux;
    bool active;
    bool te_observer_attached;
    bool display_event_attached;
    lv_indev_t *input;
    lv_display_t *display;
    lv_obj_t *screen;
    lv_obj_t *marker;
    lv_event_dsc_t *screen_event;
    uint32_t lvgl_event_count;
    uint32_t flush_match_count;
    uint32_t te_rising_count;
    uint32_t collapsed_events;
    uint32_t dropped_te_chains;
    uint32_t invalid_timestamps;
    bool event_pending;
    int64_t event_us;
    int64_t event_irq_us;
    lv_area_t marker_area;
    uint32_t te_pending_count;
    touch_latency_te_pending_t te_pending[TOUCH_LATENCY_PENDING_TE_MAX];
    touch_latency_stat_t event_to_flush;
    touch_latency_stat_t flush_to_te;
    touch_latency_stat_t irq_to_te;
} touch_latency_ctx_t;

static const char *const s_corner_name[] = {"top-left", "top-right",
                                            "bottom-right", "bottom-left"};

static void touch_latency_stat_init(touch_latency_stat_t *stat, uint32_t seed)
{
    stat->rng = seed;
}

/* Called from both the LVGL task and the TE ISR while ctx->mux is held. */
static void IRAM_ATTR touch_latency_stat_add(touch_latency_stat_t *stat,
                                             int64_t delta_us)
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
    stat->count++;
    stat->sum_us += value;
    if (stat->stored < TOUCH_LATENCY_RESERVOIR_SAMPLES) {
        stat->samples[stat->stored++] = value;
        return;
    }

    /* Deterministic reservoir sampling bounds ISR-safe storage while keeping
     * p95 representative during a long (up to five minute) run. */
    uint32_t random = stat->rng;
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    stat->rng = random;
    const uint32_t slot = random % stat->count;
    if (slot < TOUCH_LATENCY_RESERVOIR_SAMPLES) {
        stat->samples[slot] = value;
    }
}

static bool touch_area_intersects(const lv_area_t *left, const lv_area_t *right)
{
    return left->x1 <= right->x2 && left->x2 >= right->x1 &&
           left->y1 <= right->y2 && left->y2 >= right->y1;
}

static void IRAM_ATTR touch_latency_te_observer(bool level,
                                                 int64_t timestamp_us,
                                                 void *user_ctx)
{
    if (!level) {
        return;
    }
    touch_latency_ctx_t *ctx = user_ctx;
    portENTER_CRITICAL_ISR(&ctx->mux);
    if (ctx->active) {
        ctx->te_rising_count++;
        for (uint32_t index = 0; index < ctx->te_pending_count; ++index) {
            const touch_latency_te_pending_t *pending =
                &ctx->te_pending[index];
            if (timestamp_us >= pending->flush_us &&
                    timestamp_us >= pending->irq_us) {
                touch_latency_stat_add(&ctx->flush_to_te,
                                       timestamp_us - pending->flush_us);
                touch_latency_stat_add(&ctx->irq_to_te,
                                       timestamp_us - pending->irq_us);
            } else {
                ctx->invalid_timestamps++;
            }
        }
        ctx->te_pending_count = 0;
    }
    portEXIT_CRITICAL_ISR(&ctx->mux);
}

static void touch_draw_input_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    touch_latency_ctx_t *ctx = lv_event_get_user_data(event);
    if ((code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) ||
            lv_event_get_indev(event) != ctx->input) {
        return;
    }

    const int64_t event_us = esp_timer_get_time();
    bool active;
    portENTER_CRITICAL(&ctx->mux);
    active = ctx->active;
    if (active) {
        ctx->lvgl_event_count++;
    }
    portEXIT_CRITICAL(&ctx->mux);
    if (!active) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(ctx->input, &point);
    lv_obj_clear_flag(ctx->marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(ctx->marker, point.x - TOUCH_DRAW_MARKER_RADIUS,
                   point.y - TOUCH_DRAW_MARKER_RADIUS);
    lv_area_t marker_area;
    lv_obj_get_coords(ctx->marker, &marker_area);

    portENTER_CRITICAL(&ctx->mux);
    if (ctx->active) {
        if (ctx->event_pending) {
            ctx->collapsed_events++;
        }
        ctx->event_pending = true;
        ctx->event_us = event_us;
        /* The official touch component does not expose its private IRQ
         * callback. Keep the latency chain board-local and measure from the
         * first LVGL event that moves the marker. */
        ctx->event_irq_us = event_us;
        ctx->marker_area = marker_area;
    }
    portEXIT_CRITICAL(&ctx->mux);
}

static void touch_latency_flush_event_cb(lv_event_t *event)
{
    const lv_area_t *flush_area = lv_event_get_param(event);
    touch_latency_ctx_t *ctx = lv_event_get_user_data(event);
    if (flush_area == NULL) {
        return;
    }
    const int64_t flush_us = esp_timer_get_time();

    portENTER_CRITICAL(&ctx->mux);
    if (ctx->active && ctx->event_pending &&
            touch_area_intersects(flush_area, &ctx->marker_area)) {
        if (flush_us >= ctx->event_us) {
            touch_latency_stat_add(&ctx->event_to_flush,
                                   flush_us - ctx->event_us);
            ctx->flush_match_count++;
            if (ctx->te_pending_count < TOUCH_LATENCY_PENDING_TE_MAX) {
                touch_latency_te_pending_t *pending =
                    &ctx->te_pending[ctx->te_pending_count++];
                pending->flush_us = flush_us;
                pending->irq_us = ctx->event_irq_us;
            } else {
                ctx->dropped_te_chains++;
            }
        } else {
            ctx->invalid_timestamps++;
        }
        ctx->event_pending = false;
    }
    portEXIT_CRITICAL(&ctx->mux);
}

static int touch_latency_compare_u32(const void *left, const void *right)
{
    const uint32_t a = *(const uint32_t *)left;
    const uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static void touch_latency_print(const char *name, touch_latency_stat_t *stat)
{
    uint32_t p95_us = 0;
    uint64_t average_us = 0;
    if (stat->count > 0) {
        average_us = (stat->sum_us + stat->count / 2U) / stat->count;
    }
    if (stat->stored > 0) {
        qsort(stat->samples, stat->stored, sizeof(stat->samples[0]),
              touch_latency_compare_u32);
        const uint32_t rank = (stat->stored * 95U + 99U) / 100U;
        p95_us = stat->samples[rank - 1U];
    }

    printf("FACTORY_TOUCH_LATENCY {\"metric\":\"%s\",\"count\":%" PRIu32
           ",\"min_us\":%" PRIu32 ",\"avg_us\":%" PRIu64
           ",\"p95_us\":%" PRIu32 ",\"max_us\":%" PRIu32
           ",\"p95_samples\":%" PRIu32 ",\"p95_approx\":%s}\n",
           name, stat->count, stat->count > 0 ? stat->min_us : 0,
           average_us, p95_us, stat->count > 0 ? stat->max_us : 0,
           stat->stored,
           stat->count > TOUCH_LATENCY_RESERVOIR_SAMPLES ? "true" : "false");
}

/* Caller holds the LVGL lock. Never hold ctx->mux while entering either port
 * observer lock: both ISR observers acquire their port lock before ctx->mux. */
static esp_err_t touch_latency_detach_locked(touch_latency_ctx_t *ctx)
{
    esp_err_t first_error = ESP_OK;
    bool safe_to_free = true;
    portENTER_CRITICAL(&ctx->mux);
    ctx->active = false;
    portEXIT_CRITICAL(&ctx->mux);

    if (ctx->te_observer_attached) {
        const esp_err_t error =
            lvgl_port_display_te_observer_set(ctx->display, NULL, NULL);
        if (error == ESP_OK) {
            ctx->te_observer_attached = false;
        } else {
            if (first_error == ESP_OK) {
                first_error = error;
            }
            safe_to_free = false;
        }
    }

    if (ctx->screen_event != NULL) {
        if (lv_obj_remove_event_dsc(ctx->screen, ctx->screen_event)) {
            ctx->screen_event = NULL;
        } else {
            safe_to_free = false;
            if (first_error == ESP_OK) {
                first_error = ESP_FAIL;
            }
        }
    }
    if (ctx->display_event_attached) {
        if (lv_display_remove_event_cb_with_user_data(
                ctx->display, touch_latency_flush_event_cb, ctx) > 0) {
            ctx->display_event_attached = false;
        } else {
            safe_to_free = false;
            if (first_error == ESP_OK) {
                first_error = ESP_FAIL;
            }
        }
    }
    return safe_to_free ? ESP_OK : first_error;
}

static esp_err_t touch_latency_detach(touch_latency_ctx_t *ctx)
{
    /* Blocking acquisition makes the object/display callback lifetime a
     * single transaction. A failure leaves ctx allocated by the caller. */
    if (!bsp_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t error = touch_latency_detach_locked(ctx);
    bsp_display_unlock();
    return error;
}

/* One arm of a reference cross: a thin rectangle centered on (cx, cy). */
static void touch_cross_arm(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                            lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *arm = lv_obj_create(parent);
    lv_obj_remove_style_all(arm);
    lv_obj_set_size(arm, width, height);
    lv_obj_set_pos(arm, x, y);
    lv_obj_set_style_bg_color(arm, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arm, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(arm, LV_OBJ_FLAG_CLICKABLE);
}

static void touch_cross(lv_obj_t *parent, lv_coord_t cx, lv_coord_t cy)
{
    const lv_coord_t reach = 10;
    touch_cross_arm(parent, cx - reach, cy - 1, 2 * reach + 1, 2);
    touch_cross_arm(parent, cx - 1, cy - reach, 2, 2 * reach + 1);
}

static lv_coord_t touch_corner_x(unsigned corner)
{
    return (corner == 1 || corner == 2) ? BSP_LCD_H_RES - 1 - TOUCH_CORNER_INSET_PX
                                        : TOUCH_CORNER_INSET_PX;
}

static lv_coord_t touch_corner_y(unsigned corner)
{
    return corner >= 2 ? BSP_LCD_V_RES - 1 - TOUCH_CORNER_INSET_PX
                       : TOUCH_CORNER_INSET_PX;
}

static bool touch_point_in_corner(unsigned corner, lv_coord_t x, lv_coord_t y)
{
    const bool near_x = x < TOUCH_TEST_ZONE_PX;
    const bool far_x = x >= BSP_LCD_H_RES - TOUCH_TEST_ZONE_PX;
    const bool near_y = y < TOUCH_TEST_ZONE_PX;
    const bool far_y = y >= BSP_LCD_V_RES - TOUCH_TEST_ZONE_PX;
    switch (corner) {
    case 0: return near_x && near_y;
    case 1: return far_x && near_y;
    case 2: return far_x && far_y;
    case 3: return near_x && far_y;
    }
    return false;
}

static lv_indev_t *touch_require_input(void)
{
    if (!factory_display_started() &&
            factory_display_show_pattern() != ESP_OK) {
        factory_report_error(FACTORY_TEST_TOUCH, ESP_FAIL,
                             "display initialization failed");
        return NULL;
    }
    lv_indev_t *input = bsp_display_get_input_dev();
    if (input == NULL) {
        factory_report_error(FACTORY_TEST_TOUCH, ESP_ERR_INVALID_STATE,
                             "touch input unavailable");
    }
    return input;
}

/* Ordered guided test: the screen prompts one corner at a time and the press
 * must land inside that corner's zone before the next prompt appears. The
 * legacy unordered quadrant check passed under any swap/mirror combination
 * because the four-corner set is symmetric; the ordered sequence fails every
 * wrong orientation mapping on at least one step. */
static int command_touch_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lv_indev_t *input = touch_require_input();
    if (input == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    printf("Touch the highlighted corners in order: top-left, top-right, "
           "bottom-right, bottom-left (%d s each)\n", TOUCH_TEST_SECONDS);

    char detail[FACTORY_DETAIL_LENGTH] = {0};
    size_t used = 0;
    for (unsigned corner = 0; corner < 4; ++corner) {
        if (!bsp_display_lock(1000)) {
            return ESP_ERR_TIMEOUT;
        }
        lv_obj_t *screen = lv_screen_active();
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
        touch_cross(screen, touch_corner_x(corner), touch_corner_y(corner));
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text_fmt(label, "Touch %s corner", s_corner_name[corner]);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        bsp_display_unlock();

        /* Require a fresh press per corner: a finger still down from the
         * previous step must not carry over. */
        const int64_t release_deadline = esp_timer_get_time() + INT64_C(2000000);
        while (esp_timer_get_time() < release_deadline) {
            bool still_down = false;
            if (bsp_display_lock(100)) {
                still_down = lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED;
                bsp_display_unlock();
            }
            if (!still_down) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        bool hit = false;
        lv_point_t last = {.x = -1, .y = -1};
        const int64_t deadline = esp_timer_get_time() +
                                 TOUCH_TEST_SECONDS * INT64_C(1000000);
        while (esp_timer_get_time() < deadline && !hit) {
            if (bsp_display_lock(100)) {
                if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) {
                    lv_indev_get_point(input, &last);
                    hit = touch_point_in_corner(corner, last.x, last.y);
                }
                bsp_display_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!hit) {
            snprintf(detail, sizeof(detail),
                     "%s corner not hit (last x=%ld y=%ld)%s",
                     s_corner_name[corner], (long)last.x, (long)last.y,
                     used > 0 ? " after earlier corners passed" : "");
            factory_report_set(FACTORY_TEST_TOUCH, FACTORY_STATUS_FAIL, detail);
            factory_report_print_one(FACTORY_TEST_TOUCH);
            factory_display_show_pattern();
            return ESP_FAIL;
        }
        used += snprintf(detail + used, sizeof(detail) - used, "%s%s(%ld,%ld)",
                         corner == 0 ? "ordered:" : ",",
                         (const char *[]){"TL", "TR", "BR", "BL"}[corner],
                         (long)last.x, (long)last.y);
        printf("touch_test: %s corner hit at (%ld, %ld)\n",
               s_corner_name[corner], (long)last.x, (long)last.y);
    }

    factory_report_set(FACTORY_TEST_TOUCH, FACTORY_STATUS_PASS, detail);
    factory_report_print_one(FACTORY_TEST_TOUCH);
    factory_display_show_pattern();
    return ESP_OK;
}

/* Interactive orientation/linearity tool: reference crosses sit inset at the
 * four logical corners and the center, and a marker tracks the reported touch
 * point while the console logs throttled coordinates plus the raw CTP_INT
 * (BSP_TOUCH_INT) level. The operator confirms the marker lands under the
 * finger; wrong swap/mirror flags are immediately visible. Diagnostic only:
 * files no report entry. */
static int command_touch_draw(int argc, char **argv)
{
    long seconds = TOUCH_DRAW_DEFAULT_SECONDS;
    bool quiet = false;
    if (argc > 3) {
        printf("usage: touch_draw [SECONDS %d-%d] [quiet]\n",
               TOUCH_DRAW_MIN_SECONDS, TOUCH_DRAW_MAX_SECONDS);
        return ESP_ERR_INVALID_ARG;
    }
    if (argc > 1) {
        char *end = NULL;
        seconds = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || seconds < TOUCH_DRAW_MIN_SECONDS ||
                seconds > TOUCH_DRAW_MAX_SECONDS) {
            printf("usage: touch_draw [SECONDS %d-%d] [quiet]\n",
                   TOUCH_DRAW_MIN_SECONDS, TOUCH_DRAW_MAX_SECONDS);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (argc > 2) {
        if (strcmp(argv[2], "quiet") != 0) {
            printf("usage: touch_draw [SECONDS %d-%d] [quiet]\n",
                   TOUCH_DRAW_MIN_SECONDS, TOUCH_DRAW_MAX_SECONDS);
            return ESP_ERR_INVALID_ARG;
        }
        quiet = true;
    }
    lv_indev_t *input = touch_require_input();
    if (input == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    touch_latency_ctx_t *latency = heap_caps_calloc(
        1, sizeof(*latency), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (latency == NULL) {
        printf("touch_draw: insufficient internal memory for latency capture\n");
        return ESP_ERR_NO_MEM;
    }
    portMUX_INITIALIZE(&latency->mux);
    latency->input = input;
    touch_latency_stat_init(&latency->event_to_flush, UINT32_C(0x2468ACE1));
    touch_latency_stat_init(&latency->flush_to_te, UINT32_C(0xA5A5C3C3));
    touch_latency_stat_init(&latency->irq_to_te, UINT32_C(0x5A5A3C3C));

    if (!bsp_display_lock(1000)) {
        heap_caps_free(latency);
        return ESP_ERR_TIMEOUT;
    }
    latency->display = lv_indev_get_display(input);
    if (latency->display == NULL) {
        bsp_display_unlock();
        heap_caps_free(latency);
        return ESP_ERR_INVALID_STATE;
    }
    lv_obj_t *screen = lv_screen_active();
    latency->screen = screen;
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned corner = 0; corner < 4; ++corner) {
        touch_cross(screen, touch_corner_x(corner), touch_corner_y(corner));
    }
    touch_cross(screen, BSP_LCD_H_RES / 2, BSP_LCD_V_RES / 2);
    const char *corner_text[] = {"(0,0)", "(459,0)", "(459,459)", "(0,459)"};
    for (unsigned corner = 0; corner < 4; ++corner) {
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text(label, corner_text[corner]);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(label,
                       corner == 1 || corner == 2 ? BSP_LCD_H_RES - 82 : 34,
                       corner >= 2 ? BSP_LCD_V_RES - 40 : 34);
    }
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "marker must sit under the finger");
    lv_obj_set_style_text_color(hint, lv_color_make(255, 255, 0), LV_PART_MAIN);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_t *marker = lv_obj_create(screen);
    latency->marker = marker;
    lv_obj_remove_style_all(marker);
    lv_obj_set_size(marker, 2 * TOUCH_DRAW_MARKER_RADIUS,
                    2 * TOUCH_DRAW_MARKER_RADIUS);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(marker, lv_color_make(255, 0, 0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(marker, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(marker, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
    latency->screen_event = lv_obj_add_event_cb(
        screen, touch_draw_input_event_cb, LV_EVENT_ALL, latency);
    if (latency->screen_event == NULL) {
        bsp_display_unlock();
        heap_caps_free(latency);
        factory_display_show_pattern();
        return ESP_ERR_NO_MEM;
    }
    lv_display_add_event_cb(latency->display, touch_latency_flush_event_cb,
                            LV_EVENT_FLUSH_START, latency);
    latency->display_event_attached = true;

    /* Keep the LVGL lock across TE observer installation, so no refresh can
     * run while the TE interrupt mode is being switched. */
    esp_err_t observer_error = lvgl_port_display_te_observer_set(
        latency->display, touch_latency_te_observer, latency);
    if (observer_error == ESP_OK) {
        latency->te_observer_attached = true;
        portENTER_CRITICAL(&latency->mux);
        latency->active = true;
        portEXIT_CRITICAL(&latency->mux);
        bsp_display_unlock();
    } else {
        const esp_err_t detach_error = touch_latency_detach_locked(latency);
        bsp_display_unlock();
        factory_display_show_pattern();
        if (detach_error == ESP_OK) {
            heap_caps_free(latency);
        }
        printf("touch_draw: latency observer setup failed: %s\n",
               esp_err_to_name(observer_error));
        return observer_error;
    }

    printf("touch_draw: %ld s window%s; touch the crosses, press any console "
           "key to exit early\n", seconds,
           quiet ? ", quiet/event-output-disabled" : "");
    /* Drop anything typed while the scene was being built. */
    char discard[32];
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval no_wait = {.tv_sec = 0, .tv_usec = 0};
        if (select(STDIN_FILENO + 1, &read_set, NULL, NULL, &no_wait) <= 0 ||
                read(STDIN_FILENO, discard, sizeof(discard)) <= 0) {
            break;
        }
    }

    bool was_pressed = false;
    unsigned presses = 0;
    unsigned samples = 0;
    int32_t min_x = INT32_MAX, min_y = INT32_MAX;
    int32_t max_x = -1, max_y = -1;
    int32_t last_print_x = 0, last_print_y = 0;
    bool last_print_valid = false;
    int64_t last_print_us = 0;
    const int64_t start_us = esp_timer_get_time();
    const int64_t deadline = start_us + (int64_t)seconds * 1000000;
    bool exit_key = false;
    while (esp_timer_get_time() < deadline) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval no_wait = {.tv_sec = 0, .tv_usec = 0};
        if (select(STDIN_FILENO + 1, &read_set, NULL, NULL, &no_wait) > 0) {
            /* One readiness indication authorizes one read only. Repeating a
             * blocking read here hangs after the available byte is drained. */
            (void)read(STDIN_FILENO, discard, sizeof(discard));
            exit_key = true;
            break;
        }
        const char *event_state = NULL;
        lv_point_t event_point = {.x = 0, .y = 0};
        int event_int_level = 1;
        if (bsp_display_lock(100)) {
            const bool pressed =
                lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED;
            const int int_level = gpio_get_level(BSP_TOUCH_INT);
            if (pressed) {
                lv_point_t point;
                lv_indev_get_point(input, &point);
                ++samples;
                min_x = point.x < min_x ? point.x : min_x;
                min_y = point.y < min_y ? point.y : min_y;
                max_x = point.x > max_x ? point.x : max_x;
                max_y = point.y > max_y ? point.y : max_y;
                /* Quiet mode avoids even calculating print-only movement;
                 * this keeps the baseline path as light as possible. */
                if (!quiet) {
                    const int64_t now = esp_timer_get_time();
                    const bool moved = last_print_valid &&
                        (llabs((int64_t)point.x - last_print_x) >=
                             TOUCH_DRAW_PRINT_DELTA_PX ||
                         llabs((int64_t)point.y - last_print_y) >=
                             TOUCH_DRAW_PRINT_DELTA_PX);
                    if (!was_pressed || !last_print_valid || moved ||
                            now - last_print_us >=
                                TOUCH_DRAW_PRINT_PERIOD_US) {
                        event_state = was_pressed ? "move" : "down";
                        event_point = point;
                        event_int_level = int_level;
                        last_print_x = point.x;
                        last_print_y = point.y;
                        last_print_us = now;
                        last_print_valid = true;
                    }
                }
                if (!was_pressed) {
                    ++presses;
                }
            } else if (was_pressed && !quiet) {
                event_state = "up";
                event_int_level = int_level;
            }
            was_pressed = pressed;
            bsp_display_unlock();
        }
        /* UART output is intentionally outside the LVGL global lock. At
         * 115200 baud a diagnostic line can occupy the console for several
         * milliseconds; printing while locked used to create the very touch
         * latency this command is intended to inspect. */
        if (event_state != NULL) {
            if (strcmp(event_state, "up") == 0) {
                printf("FACTORY_TOUCH {\"int\":%d,\"state\":\"up\"}\n",
                       event_int_level);
            } else {
                printf("FACTORY_TOUCH {\"x\":%ld,\"y\":%ld,\"int\":%d,"
                       "\"state\":\"%s\"}\n",
                       (long)event_point.x, (long)event_point.y,
                       event_int_level, event_state);
            }
            fflush(stdout);
        }
        vTaskDelay(pdMS_TO_TICKS(quiet ? 5 : 10));
    }

    const esp_err_t detach_error = touch_latency_detach(latency);
    factory_display_show_pattern();
    const unsigned elapsed_s = (unsigned)((esp_timer_get_time() - start_us) / 1000000);
    printf("touch_draw done: %s, presses=%u samples=%u\n",
           exit_key ? "exit key" : "timeout", presses, samples);
    fputs("FACTORY_TOUCH_SUMMARY {\"presses\":", stdout);
    printf("%u,\"samples\":%u", presses, samples);
    if (samples > 0) {
        printf(",\"min_x\":%ld,\"max_x\":%ld,\"min_y\":%ld,\"max_y\":%ld",
               (long)min_x, (long)max_x, (long)min_y, (long)max_y);
    }
    printf(",\"seconds\":%u}\n", elapsed_s);
    touch_latency_print("lvgl_event_to_marker_flush",
                        &latency->event_to_flush);
    touch_latency_print("marker_flush_to_te_rising", &latency->flush_to_te);
    touch_latency_print("lvgl_event_to_te_rising", &latency->irq_to_te);
    printf("FACTORY_TOUCH_LATENCY_META {\"lvgl_events\":%" PRIu32
           ",\"marker_flush_matches\":%" PRIu32
           ",\"te_rising_edges\":%" PRIu32 ",\"collapsed_events\":%" PRIu32
           ",\"dropped_te_chains\":%" PRIu32
           ",\"pending_event_at_stop\":%s,\"pending_te_at_stop\":%" PRIu32
           ",\"invalid_timestamps\":%" PRIu32 "}\n",
           latency->lvgl_event_count, latency->flush_match_count,
           latency->te_rising_count,
           latency->collapsed_events, latency->dropped_te_chains,
           latency->event_pending ? "true" : "false",
           latency->te_pending_count, latency->invalid_timestamps);
    if (detach_error == ESP_OK) {
        heap_caps_free(latency);
    } else {
        printf("touch_draw: callback cleanup failed: %s; context retained\n",
               esp_err_to_name(detach_error));
        return detach_error;
    }
    return samples > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t factory_touch_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "touch_test", .help = "Guide an ordered four-corner touch and verify each landing zone.", .func = command_touch_test},
        {.command = "touch_draw", .help = "Track touches with an on-screen marker: touch_draw [SECONDS 5-300] [quiet].", .func = command_touch_draw},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
