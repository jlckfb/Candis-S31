/*
 * Candis-S31 watch demo - watchface root screen.
 *
 * Navigation root (s_stack[0]); the launcher menu is the persistent second
 * layer above it. ui_watchface_tick() is driven by the UI manager's single
 * 1 s clock and only while the watchface is the active screen, so a hidden
 * watchface costs zero LVGL work (that is the pause/resume contract).
 *
 * Every widget write is last-value guarded: the display pipeline is
 * RENDER_MODE_FULL with TE gating, so any invalidate becomes one
 * full-frame redraw. Refresh granularity is therefore minute-level time
 * plus change-only battery; deliberately no second hand (DESIGN.md motion
 * budget).
 *
 * Input: tap or swipe up opens the launcher menu. LVGL 9 still delivers
 * LV_EVENT_CLICKED after a recognized gesture on release, so the click
 * handler drops releases that already carried a gesture.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_watchface.h"

#include <limits.h>

#include "services/svc_power.h"
#include "ui_manager.h"
#include "ui_theme.h"

#define WF_TIME_Y    118 /* HH:MM baseline band, upper-middle */
#define WF_DATE_Y    196 /* date + weekday row */
#define WF_ARC_SIZE  120 /* battery ring diameter */
#define WF_ARC_Y     244
#define WF_ARC_W     6   /* thin 360-degree ring */
#define WF_BRAND_Y   424

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *lbl_time;
    lv_obj_t *lbl_date;
    lv_obj_t *arc;
    lv_obj_t *lbl_batt;
    bool active;       /* screen-loaded gate, set by SCREEN events */
    bool have_time;    /* at least one valid RTC reading this boot */
    /* Last rendered values: no LVGL write while unchanged. */
    int r_hour;
    int r_minute;
    int r_year;
    int r_month;
    int r_day;
    int r_weekday;
    int r_batt_percent;
    bool r_batt_charging;
} wf_state_t;

static wf_state_t s = {
    .r_hour = -1,
    .r_minute = -1,
    .r_year = -1,
    .r_month = -1,
    .r_day = -1,
    .r_weekday = -1,
    .r_batt_percent = INT_MIN,
};

/* RX8130CE weekday encoding: 0 = Sunday. */
static const char *const wf_weekday_zh[7] = {
    "周日", "周一", "周二", "周三", "周四", "周五", "周六",
};

static void wf_time_apply(const bsp_rtc_time_t *t)
{
    if ((int)t->hour != s.r_hour || (int)t->minute != s.r_minute) {
        s.r_hour = t->hour;
        s.r_minute = t->minute;
        lv_label_set_text_fmt(s.lbl_time, "%02d:%02d", (int)t->hour,
                              (int)t->minute);
    }
    if ((int)t->year != s.r_year || (int)t->month != s.r_month ||
        (int)t->day != s.r_day || (int)t->weekday != s.r_weekday) {
        s.r_year = t->year;
        s.r_month = t->month;
        s.r_day = t->day;
        s.r_weekday = t->weekday;
        const char *week = wf_weekday_zh[t->weekday < 7 ? t->weekday : 0];
        lv_label_set_text_fmt(s.lbl_date, "%d年%d月%d日 %s",
                              (int)t->year, (int)t->month, (int)t->day,
                              week);
    }
}

static void wf_battery_apply(void)
{
    /* Snapshot copy, critical-section safe on the LVGL thread; the PMIC
     * poll (2 s) is the only writer. */
    svc_power_status_t ps;
    svc_power_get_status(&ps);

    const int percent = (ps.present && ps.percent >= 0) ? ps.percent : -1;
    const bool charging = ps.present && ps.charging;

    if (percent == s.r_batt_percent && charging == s.r_batt_charging) {
        return; /* unchanged: no arc/label write, no full-frame redraw */
    }
    s.r_batt_percent = percent;
    s.r_batt_charging = charging;

    lv_arc_set_value(s.arc, percent < 0 ? 0 : percent);
    lv_obj_set_style_arc_color(
        s.arc, lv_color_hex(charging ? UI_COL_PASS : UI_COL_ACCENT),
        LV_PART_INDICATOR);

    if (percent < 0) {
        lv_label_set_text(s.lbl_batt, "--");
        lv_obj_set_style_text_color(s.lbl_batt,
                                    lv_color_hex(UI_COL_TEXT_DIM), 0);
    } else if (charging) {
        lv_label_set_text_fmt(s.lbl_batt, LV_SYMBOL_CHARGE " %d%%", percent);
        lv_obj_set_style_text_color(s.lbl_batt, lv_color_hex(UI_COL_PASS), 0);
    } else {
        lv_label_set_text_fmt(s.lbl_batt, "%d%%", percent);
        lv_obj_set_style_text_color(s.lbl_batt, lv_color_hex(UI_COL_TEXT), 0);
    }
}

/* Swipe up enters the launcher menu. */
static void wf_gesture_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev && lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        ui_nav_open_menu();
    }
}

/* Tap enters the launcher menu. A swipe also ends in a CLICKED event in
 * LVGL 9; suppress that duplicate so one gesture opens exactly one menu. */
static void wf_clicked_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL) {
        /* A non-pointer source must not open the launcher menu. */
        return;
    }
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) {
        return;
    }
    ui_nav_open_menu();
}

static void wf_screen_loaded_cb(lv_event_t *event)
{
    (void)event;
    s.active = true;
}

static void wf_screen_unload_cb(lv_event_t *event)
{
    (void)event;
    s.active = false;
}

lv_obj_t *ui_watchface_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);

    s.screen = screen;
    s.active = false;
    s.have_time = false;

    /* HH:MM, Montserrat 48, upper-middle. */
    s.lbl_time = lv_label_create(screen);
    lv_label_set_text(s.lbl_time, "--:--");
    lv_obj_set_style_text_font(s.lbl_time, ui_font_big(), 0);
    lv_obj_set_style_text_color(s.lbl_time, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_remove_flag(s.lbl_time, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s.lbl_time, LV_ALIGN_TOP_MID, 0, WF_TIME_Y);

    /* Date + weekday, project CJK 20. */
    s.lbl_date = lv_label_create(screen);
    lv_label_set_text(s.lbl_date, "--");
    lv_obj_set_style_text_font(s.lbl_date, ui_font_body_lg(), 0);
    lv_obj_set_style_text_color(s.lbl_date, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_remove_flag(s.lbl_date, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s.lbl_date, LV_ALIGN_TOP_MID, 0, WF_DATE_Y);

    /* Battery ring: 360-degree thin arc, accent color, green while
     * charging. Starts at 12 o'clock (rotation 270). */
    s.arc = lv_arc_create(screen);
    lv_obj_set_size(s.arc, WF_ARC_SIZE, WF_ARC_SIZE);
    lv_obj_align(s.arc, LV_ALIGN_TOP_MID, 0, WF_ARC_Y);
    lv_arc_set_rotation(s.arc, 270);
    lv_arc_set_bg_angles(s.arc, 0, 360);
    lv_arc_set_range(s.arc, 0, 100);
    lv_arc_set_value(s.arc, 0);
    lv_obj_remove_flag(s.arc, LV_OBJ_FLAG_CLICKABLE);
    /* Keep the ring center open (screen black shows through). */
    lv_obj_set_style_bg_opa(s.arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.arc, 0, 0);
    /* Track. */
    lv_obj_set_style_arc_width(s.arc, WF_ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s.arc, lv_color_hex(UI_COL_TRACK),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s.arc, LV_OPA_COVER, LV_PART_MAIN);
    /* Indicator. */
    lv_obj_set_style_arc_width(s.arc, WF_ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s.arc, lv_color_hex(UI_COL_ACCENT),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s.arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s.arc, false, LV_PART_INDICATOR);
    /* No knob: invisible zero-pad cap. */
    lv_obj_set_style_bg_opa(s.arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(s.arc, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s.arc, 0, LV_PART_KNOB);

    /* Percent inside the ring. */
    s.lbl_batt = lv_label_create(s.arc);
    lv_label_set_text(s.lbl_batt, "--");
    lv_obj_set_style_text_font(s.lbl_batt, ui_font_body(), 0);
    lv_obj_set_style_text_color(s.lbl_batt, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_remove_flag(s.lbl_batt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s.lbl_batt);

    /* Board name, small dim line at the bottom. */
    lv_obj_t *brand = lv_label_create(screen);
    lv_label_set_text(brand, "Candis-S31");
    lv_obj_set_style_text_font(brand, ui_font_text(), 0);
    lv_obj_set_style_text_color(brand, lv_color_hex(UI_COL_TEXT_WEAK), 0);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, WF_BRAND_Y);

    lv_obj_add_event_cb(screen, wf_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(screen, wf_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(screen, wf_screen_loaded_cb, LV_EVENT_SCREEN_LOADED,
                        NULL);
    lv_obj_add_event_cb(screen, wf_screen_unload_cb,
                        LV_EVENT_SCREEN_UNLOAD_START, NULL);

    return screen;
}

void ui_watchface_tick(const bsp_rtc_time_t *t)
{
    if (!s.screen || !s.active) {
        return; /* hidden or not (yet) the active screen */
    }

    if (t != NULL) {
        s.have_time = true;
    }
    if (s.have_time && t != NULL) {
        wf_time_apply(t);
    }

    /* Battery state is independent of RTC validity. */
    wf_battery_apply();
}
