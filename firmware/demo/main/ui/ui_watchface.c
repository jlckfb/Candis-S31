/*
 * Candis-S31 watch demo - watchface (home screen).
 *
 * Pure-black AMOLED face: large Montserrat time, date line and a battery
 * arc ring fed by the power service. The screen lives for the whole run,
 * so its 1 s timer never needs teardown. Swipe up (custom detection, the
 * LVGL gesture recognizer is disabled in sdkconfig) or tap opens the menu.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_watchface.h"

#include <stdio.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"

#include "services/svc_power.h"
#include "ui_manager.h"

/* Up-swipe thresholds in pixels. */
#define WF_SWIPE_MIN_DY 50
#define WF_CLICK_MAX_DRAG 20

static lv_obj_t *s_time_lbl;
static lv_obj_t *s_sec_lbl;
static lv_obj_t *s_date_lbl;
static lv_obj_t *s_batt_arc;
static lv_obj_t *s_batt_lbl;

/* Clock cache: skip label rewrites when the value did not change. */
static int s_last_hm = -1;
static int s_last_date = -1;
static bool s_last_rtc_ok;

/* Battery arc cache. */
static int s_last_batt_key = -1; /* percent | charging<<8 | present<<9 */

/* Swipe-up detection state. */
static int s_press_x;
static int s_press_y;
static int s_max_drag;
static bool s_press_valid;

static const char *weekday_name(int weekday)
{
    static const char *names[] = { "日", "一", "二", "三", "四", "五", "六" };
    return (weekday >= 0 && weekday <= 6) ? names[weekday] : "";
}

static void battery_refresh(void)
{
    svc_power_status_t pw;
    svc_power_get_status(&pw);

    int key = -1;
    if (pw.present && pw.percent >= 0 && pw.percent <= 100) {
        key = pw.percent | ((int)pw.charging << 8);
    }
    if (key == s_last_batt_key) {
        return;
    }
    s_last_batt_key = key;

    if (key < 0) {
        /* Battery absent or unknown: dim ring, no value. */
        lv_arc_set_value(s_batt_arc, 0);
        lv_obj_set_style_arc_color(s_batt_arc, lv_color_hex(0x303038),
                                   LV_PART_INDICATOR);
        lv_label_set_text(s_batt_lbl, "--");
        lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
        return;
    }

    lv_arc_set_value(s_batt_arc, pw.percent);
    lv_label_set_text_fmt(s_batt_lbl, "%d%%", pw.percent);
    uint32_t color = UI_COLOR_ACCENT;
    if (pw.charging) {
        color = UI_COLOR_OK;
    } else if (pw.percent <= 20) {
        color = UI_COLOR_ERR;
    }
    lv_obj_set_style_arc_color(s_batt_arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(color), 0);
}

static void clock_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_time_lbl) {
        return;
    }

    bsp_rtc_time_t now;
    bsp_rtc_status_t status;
    if (bsp_rtc_get_time(&now, &status) != ESP_OK || !status.time_valid) {
        lv_label_set_text(s_time_lbl, "--:--");
        lv_label_set_text(s_sec_lbl, "");
        lv_label_set_text(s_date_lbl, "RTC 未设置");
        s_last_hm = -1;
        s_last_date = -1;
        if (s_last_rtc_ok) {
            s_last_rtc_ok = false;
            ui_status_set_time(-1, -1);
        }
        battery_refresh();
        return;
    }
    s_last_rtc_ok = true;

    int hm = now.hour * 100 + now.minute;
    if (hm != s_last_hm) {
        s_last_hm = hm;
        lv_label_set_text_fmt(s_time_lbl, "%02d:%02d", now.hour, now.minute);
        ui_status_set_time(now.hour, now.minute);
    }
    lv_label_set_text_fmt(s_sec_lbl, "%02d", now.second);

    int ymd = now.year * 10000 + now.month * 100 + now.day;
    if (ymd != s_last_date) {
        s_last_date = ymd;
        lv_label_set_text_fmt(s_date_lbl, "%04d-%02d-%02d 周%s",
                              now.year, now.month, now.day,
                              weekday_name(now.weekday));
    }
    battery_refresh();
}

/* ------------------------------------------------------------------ */
/* Input: swipe-up opens the menu, a clean tap does the same.          */
/* ------------------------------------------------------------------ */

static void watchface_pressed_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_x = p.x;
    s_press_y = p.y;
    s_max_drag = 0;
    s_press_valid = true;
}

static void watchface_pressing_cb(lv_event_t *event)
{
    if (!s_press_valid) {
        return;
    }
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = abs(p.x - s_press_x);
    int dy = abs(p.y - s_press_y);
    s_max_drag = dx > dy ? dx : dy;
}

static void watchface_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_press_valid) {
        return;
    }
    s_press_valid = false;

    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dy = s_press_y - p.y; /* positive = moved up */
    int dx = abs(p.x - s_press_x);
    if (dy > WF_SWIPE_MIN_DY && dy > dx) {
        ui_nav_open_menu();
    }
}

static void watchface_click_cb(lv_event_t *event)
{
    (void)event;
    if (s_max_drag <= WF_CLICK_MAX_DRAG) {
        ui_nav_open_menu();
    }
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

lv_obj_t *ui_watchface_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_time_lbl = lv_label_create(screen);
    lv_obj_set_style_text_font(s_time_lbl, ui_font_big(), 0);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_label_set_text(s_time_lbl, "--:--");
    lv_obj_align(s_time_lbl, LV_ALIGN_CENTER, 0, -42);

    s_sec_lbl = lv_label_create(screen);
    lv_obj_set_style_text_font(s_sec_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_sec_lbl, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_label_set_text(s_sec_lbl, "");
    lv_obj_align(s_sec_lbl, LV_ALIGN_CENTER, 112, -36);

    s_date_lbl = lv_label_create(screen);
    lv_obj_set_style_text_color(s_date_lbl, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(s_date_lbl, "");
    lv_obj_align(s_date_lbl, LV_ALIGN_CENTER, 0, 16);

    /* Battery ring: 270 deg arc with the gap at the bottom. */
    s_batt_arc = lv_arc_create(screen);
    lv_obj_set_size(s_batt_arc, 108, 108);
    lv_obj_align(s_batt_arc, LV_ALIGN_CENTER, 0, 118);
    lv_arc_set_rotation(s_batt_arc, 135);
    lv_arc_set_bg_angles(s_batt_arc, 0, 270);
    lv_arc_set_range(s_batt_arc, 0, 100);
    lv_arc_set_value(s_batt_arc, 0);
    lv_obj_remove_style(s_batt_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_batt_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_batt_arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_batt_arc, lv_color_hex(0x232328), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_batt_arc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_batt_arc, lv_color_hex(0x303038), LV_PART_INDICATOR);

    s_batt_lbl = lv_label_create(screen);
    lv_obj_set_style_text_font(s_batt_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(s_batt_lbl, "--");
    lv_obj_align_to(s_batt_lbl, s_batt_arc, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = lv_label_create(screen);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(hint, "上滑打开应用");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_add_event_cb(screen, watchface_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen, watchface_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(screen, watchface_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(screen, watchface_click_cb, LV_EVENT_CLICKED, NULL);

    /* Refresh once immediately, then once per second. */
    s_last_hm = -1;
    s_last_date = -1;
    s_last_batt_key = -1;
    s_last_rtc_ok = true; /* force first invalid state to be posted */
    lv_timer_t *timer = lv_timer_create(clock_tick, 1000, NULL);
    lv_timer_ready(timer);
    return screen;
}
