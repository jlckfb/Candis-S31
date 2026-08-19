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
#include "esp_console.h"
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

static const char *const s_corner_name[] = {"top-left", "top-right",
                                            "bottom-right", "bottom-left"};

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
    if (argc > 1) {
        char *end = NULL;
        seconds = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || seconds < TOUCH_DRAW_MIN_SECONDS ||
                seconds > TOUCH_DRAW_MAX_SECONDS) {
            printf("usage: touch_draw [SECONDS %d-%d]\n", TOUCH_DRAW_MIN_SECONDS,
                   TOUCH_DRAW_MAX_SECONDS);
            return ESP_ERR_INVALID_ARG;
        }
    }
    lv_indev_t *input = touch_require_input();
    if (input == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    for (unsigned corner = 0; corner < 4; ++corner) {
        touch_cross(screen, touch_corner_x(corner), touch_corner_y(corner));
    }
    touch_cross(screen, BSP_LCD_H_RES / 2, BSP_LCD_V_RES / 2);
    const char *corner_text[] = {"(0,0)", "(459,0)", "(459,459)", "(0,459)"};
    for (unsigned corner = 0; corner < 4; ++corner) {
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text(label, corner_text[corner]);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_pos(label,
                       corner == 1 || corner == 2 ? BSP_LCD_H_RES - 82 : 34,
                       corner >= 2 ? BSP_LCD_V_RES - 40 : 34);
    }
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "marker must sit under the finger");
    lv_obj_set_style_text_color(hint, lv_color_make(255, 255, 0), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_t *marker = lv_obj_create(screen);
    lv_obj_remove_style_all(marker);
    lv_obj_set_size(marker, 2 * TOUCH_DRAW_MARKER_RADIUS,
                    2 * TOUCH_DRAW_MARKER_RADIUS);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(marker, lv_color_make(255, 0, 0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(marker, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(marker, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();

    printf("touch_draw: %ld s window; touch the crosses, press any console "
           "key to exit early\n", seconds);
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
    int32_t last_print_x = INT32_MIN, last_print_y = INT32_MIN;
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
            while (read(STDIN_FILENO, discard, sizeof(discard)) > 0) {
            }
            exit_key = true;
            break;
        }
        if (bsp_display_lock(100)) {
            const bool pressed =
                lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED;
            const int int_level = gpio_get_level(BSP_TOUCH_INT);
            if (pressed) {
                lv_point_t point;
                lv_indev_get_point(input, &point);
                lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(marker, point.x - TOUCH_DRAW_MARKER_RADIUS,
                               point.y - TOUCH_DRAW_MARKER_RADIUS);
                ++samples;
                min_x = point.x < min_x ? point.x : min_x;
                min_y = point.y < min_y ? point.y : min_y;
                max_x = point.x > max_x ? point.x : max_x;
                max_y = point.y > max_y ? point.y : max_y;
                const int64_t now = esp_timer_get_time();
                const bool moved =
                    abs(point.x - last_print_x) >= TOUCH_DRAW_PRINT_DELTA_PX ||
                    abs(point.y - last_print_y) >= TOUCH_DRAW_PRINT_DELTA_PX;
                if (!was_pressed || moved ||
                        now - last_print_us >= TOUCH_DRAW_PRINT_PERIOD_US) {
                    printf("FACTORY_TOUCH {\"x\":%ld,\"y\":%ld,\"int\":%d,"
                           "\"state\":\"%s\"}\n",
                           (long)point.x, (long)point.y, int_level,
                           was_pressed ? "move" : "down");
                    fflush(stdout);
                    last_print_x = point.x;
                    last_print_y = point.y;
                    last_print_us = now;
                }
                if (!was_pressed) {
                    ++presses;
                }
            } else if (was_pressed) {
                printf("FACTORY_TOUCH {\"int\":%d,\"state\":\"up\"}\n",
                       int_level);
                fflush(stdout);
            }
            was_pressed = pressed;
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

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
    return samples > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t factory_touch_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "touch_test", .help = "Guide an ordered four-corner touch and verify each landing zone.", .func = command_touch_test},
        {.command = "touch_draw", .help = "Track touches with an on-screen marker and log coordinates: touch_draw [SECONDS 5-300].", .func = command_touch_draw},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
