/*
 * Candis-S31 watch demo - settings app.
 *
 * Brightness / volume / mic gain sliders, screen-off timeout, RTC time
 * editor (rollers + read-back verify) and an about overlay. Every change
 * is applied immediately and persisted through demo_settings_save() (NVS).
 * All handlers run on the LVGL thread; the RTC I2C transactions are short
 * single-shot transfers, so they run inline instead of through a service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp/candis_s31.h"
#include "bsp/display.h"
#include "esp_idf_version.h"

#include "demo_apps.h"
#include "demo_board.h"
#include "services/svc_audio.h"
#include "services/svc_power.h"
#include "ui/ui_manager.h"

#define SETTINGS_ROW_WIDTH  428
#define SETTINGS_YEAR_BASE  2020
#define SETTINGS_YEAR_COUNT 30

typedef struct {
    bool active;
    lv_obj_t *lbl_brightness;
    lv_obj_t *lbl_volume;
    lv_obj_t *lbl_gain;
    lv_obj_t *dd_timeout;
    lv_obj_t *roller_year;
    lv_obj_t *roller_month;
    lv_obj_t *roller_day;
    lv_obj_t *roller_hour;
    lv_obj_t *roller_min;
    lv_obj_t *about; /* overlay on the top layer, NULL when closed */
} settings_state_t;

static settings_state_t s;

/* ------------------------------------------------------------------ */
/* Layout helpers                                                      */
/* ------------------------------------------------------------------ */

static lv_obj_t *settings_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, SETTINGS_ROW_WIDTH);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *section_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *row = settings_row(parent);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_body(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 8, 0);
    return row;
}

/* ------------------------------------------------------------------ */
/* Sliders                                                             */
/* ------------------------------------------------------------------ */

static void brightness_change_cb(lv_event_t *event)
{
    if (!s.active) {
        return;
    }
    const int value = lv_slider_get_value(lv_event_get_target_obj(event));
    demo_settings()->brightness = value;
    bsp_display_brightness_set(value);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        demo_settings_save(); /* one NVS commit per gesture, not per tick */
    }
    lv_label_set_text_fmt(s.lbl_brightness, "%d%%", value);
}

static void volume_change_cb(lv_event_t *event)
{
    if (!s.active) {
        return;
    }
    const int value = lv_slider_get_value(lv_event_get_target_obj(event));
    demo_settings()->volume = value;
    if (svc_audio_is_playing()) {
        svc_audio_set_volume(value); /* live update of the running stream */
    }
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        demo_settings_save();
    }
    lv_label_set_text_fmt(s.lbl_volume, "%d%%", value);
}

static void gain_change_cb(lv_event_t *event)
{
    if (!s.active) {
        return;
    }
    lv_obj_t *slider = lv_event_get_target_obj(event);
    /* Snap the continuous slider onto the ES8389 3 dB grid. */
    const int value = (lv_slider_get_value(slider) / 3) * 3;
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    demo_settings()->mic_gain_db = value;
    if (svc_audio_is_recording()) {
        svc_audio_record_set_gain(value); /* re-applies the route register */
    }
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        demo_settings_save();
    }
    lv_label_set_text_fmt(s.lbl_gain, "%d dB", value);
}

static lv_obj_t *slider_row(lv_obj_t *parent, const char *name, int min,
                            int max, int value, lv_obj_t **value_out,
                            lv_event_cb_t cb)
{
    lv_obj_t *row = settings_row(parent);
    lv_obj_set_height(row, 92);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 8, 2);

    lv_obj_t *value_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(value_lbl, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_align(value_lbl, LV_ALIGN_TOP_RIGHT, -8, 2);
    *value_out = value_lbl;

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, SETTINGS_ROW_WIDTH - 24, 56);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_LEFT, 8, -2);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_RELEASED, NULL);
    return row;
}

/* ------------------------------------------------------------------ */
/* Screen-off timeout                                                  */
/* ------------------------------------------------------------------ */

static const int s_timeout_seconds[5] = { 10, 30, 60, 120, 0 };

static void timeout_change_cb(lv_event_t *event)
{
    if (!s.active) {
        return;
    }
    (void)event;
    const int index = lv_dropdown_get_selected(s.dd_timeout);
    if (index < 0 || index >= 5) {
        return;
    }
    demo_settings()->screen_timeout_s = s_timeout_seconds[index];
    svc_power_set_screen_timeout(s_timeout_seconds[index]);
    demo_settings_save();
}

/* ------------------------------------------------------------------ */
/* RTC time                                                            */
/* ------------------------------------------------------------------ */

/* Sakamoto's algorithm; returns 0 = Sunday to match the RX8130CE. */
static uint8_t rtc_weekday(int year, int month, int day)
{
    static const int table[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year;
    if (month < 3) {
        --y;
    }
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + table[month - 1] + day) % 7);
}

static void rtc_apply_cb(lv_event_t *event)
{
    if (!s.active) {
        return;
    }
    (void)event;

    bsp_rtc_time_t t = { 0 };
    t.year = (uint16_t)(SETTINGS_YEAR_BASE + lv_roller_get_selected(s.roller_year));
    t.month = (uint8_t)(1 + lv_roller_get_selected(s.roller_month));
    t.day = (uint8_t)(1 + lv_roller_get_selected(s.roller_day));
    t.hour = (uint8_t)lv_roller_get_selected(s.roller_hour);
    t.minute = (uint8_t)lv_roller_get_selected(s.roller_min);
    t.second = 0;
    t.weekday = rtc_weekday(t.year, t.month, t.day);

    /* The day roller always offers 31 days; validate against the month. */
    static const uint8_t days_per_month[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    uint8_t max_day = days_per_month[t.month - 1];
    if (t.month == 2 && (t.year % 4) == 0) {
        ++max_day;
    }
    if (t.day > max_day) {
        ui_toast("No such day this month");
        return;
    }

    if (bsp_rtc_set_time(&t) != ESP_OK) {
        ui_toast("Time set failed");
        return;
    }

    /* Read back and verify; seconds may have advanced, compare the rest. */
    bsp_rtc_time_t back = { 0 };
    bsp_rtc_status_t status = { 0 };
    if (bsp_rtc_get_time(&back, &status) == ESP_OK &&
            back.year == t.year && back.month == t.month &&
            back.day == t.day && back.hour == t.hour &&
            back.minute == t.minute && back.weekday == t.weekday) {
        ui_toast("Time updated");
    } else {
        ui_toast("Time check failed, retry");
    }
}

static lv_obj_t *rtc_roller(lv_obj_t *parent, const char *options,
                            lv_coord_t x, lv_coord_t width)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, 1);
    lv_obj_set_size(roller, width, 56);
    lv_obj_set_pos(roller, x, 30);
    return roller;
}

static void rtc_section_build(lv_obj_t *parent)
{
    lv_obj_t *row = settings_row(parent);
    lv_obj_set_height(row, 150);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, "RTC time (YmdHm)");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 0);

    char years[SETTINGS_YEAR_COUNT * 6];
    int pos = 0;
    for (int i = 0; i < SETTINGS_YEAR_COUNT; ++i) {
        pos += snprintf(years + pos, sizeof(years) - pos, "%s%d",
                        i ? "\n" : "", SETTINGS_YEAR_BASE + i);
    }
    char hours[24 * 4];
    pos = 0;
    for (int i = 0; i < 24; ++i) {
        pos += snprintf(hours + pos, sizeof(hours) - pos, "%s%d", i ? "\n" : "", i);
    }
    char minutes[60 * 4];
    pos = 0;
    for (int i = 0; i < 60; ++i) {
        pos += snprintf(minutes + pos, sizeof(minutes) - pos, "%s%d",
                        i ? "\n" : "", i);
    }

    s.roller_year = rtc_roller(row, years, 8, 92);
    s.roller_month = rtc_roller(row,
        "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12", 108, 72);
    s.roller_day = rtc_roller(row,
        "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n"
        "21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31", 188, 72);
    s.roller_hour = rtc_roller(row, hours, 268, 72);
    s.roller_min = rtc_roller(row, minutes, 348, 72);

    lv_obj_t *apply = lv_button_create(row);
    lv_obj_set_size(apply, 144, 56);
    lv_obj_align(apply, LV_ALIGN_BOTTOM_RIGHT, -8, -2);
    lv_obj_set_style_bg_color(apply, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_add_event_cb(apply, rtc_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apply_lbl = lv_label_create(apply);
    lv_label_set_text(apply_lbl, "Apply time");
    lv_obj_center(apply_lbl);

    /* Pre-fill the rollers with the current RTC reading. */
    bsp_rtc_time_t now = { 0 };
    bsp_rtc_status_t status = { 0 };
    if (bsp_rtc_get_time(&now, &status) == ESP_OK) {
        int year_index = (int)now.year - SETTINGS_YEAR_BASE;
        if (year_index < 0) {
            year_index = 0;
        }
        if (year_index >= SETTINGS_YEAR_COUNT) {
            year_index = SETTINGS_YEAR_COUNT - 1;
        }
        lv_roller_set_selected(s.roller_year, (uint32_t)year_index, LV_ANIM_OFF);
        if (now.month >= 1 && now.month <= 12) {
            lv_roller_set_selected(s.roller_month, now.month - 1, LV_ANIM_OFF);
        }
        if (now.day >= 1 && now.day <= 31) {
            lv_roller_set_selected(s.roller_day, now.day - 1, LV_ANIM_OFF);
        }
        if (now.hour <= 23) {
            lv_roller_set_selected(s.roller_hour, now.hour, LV_ANIM_OFF);
        }
        if (now.minute <= 59) {
            lv_roller_set_selected(s.roller_min, now.minute, LV_ANIM_OFF);
        }
    } else {
        ui_toast("RTC read failed");
    }
}

/* ------------------------------------------------------------------ */
/* About overlay                                                       */
/* ------------------------------------------------------------------ */

static void about_delete_cb(lv_event_t *event)
{
    (void)event;
    s.about = NULL;
}

static void about_close_cb(lv_event_t *event)
{
    (void)event;
    if (s.about) {
        lv_obj_delete(s.about); /* about_delete_cb clears s.about */
    }
}

static void about_open_cb(lv_event_t *event)
{
    if (!s.active || s.about) {
        return;
    }
    (void)event;

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, 460, 460);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, about_delete_cb, LV_EVENT_DELETE, NULL);
    s.about = overlay;

    lv_obj_t *close = lv_button_create(overlay);
    lv_obj_set_size(close, 72, 56);
    lv_obj_set_pos(close, 380, 8);
    lv_obj_set_style_bg_color(close, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_add_event_cb(close, about_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);

    lv_obj_t *title = lv_label_create(overlay);
    lv_label_set_text(title, "About");
    /* Montserrat has no CJK glyphs; keep Chinese labels on the bundled
     * Source Han Sans font instead of rendering placeholder squares. */
    lv_obj_set_style_text_font(title, ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 14);

    lv_obj_t *body = lv_label_create(overlay);
    lv_label_set_text_fmt(body,
                          "Candis-S31 watch demo\n"
                          "EVT1 peripheral demo\n\n"
                          "BSP: %s\n"
                          "IDF: %s\n\n"
                          "2.0in AMOLED - ESP32-S31\n"
                          "LVGL %d.%d",
                          CANDIS_S31_BSP_GIT_REV, esp_get_idf_version(),
                          LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);
    lv_obj_set_width(body, 400);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body, ui_font_body(), 0);
    lv_obj_set_style_pad_row(body, 4, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 24, 76);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static void settings_root_delete_cb(lv_event_t *event)
{
    (void)event;
    s.active = false;
    if (s.about) {
        lv_obj_delete(s.about); /* overlay lives on the top layer */
    }
}

lv_obj_t *app_settings_create(void)
{
    s = (settings_state_t){ .active = true };
    const demo_settings_t *cfg = demo_settings();

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("Settings", &content);
    lv_obj_add_event_cb(root, settings_root_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_text_font(content, ui_font_body(), 0);

    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);

    section_title(content, "Display");
    slider_row(content, "Brightness", 10, 100, cfg->brightness, &s.lbl_brightness,
               brightness_change_cb);
    lv_label_set_text_fmt(s.lbl_brightness, "%d%%", cfg->brightness);

    lv_obj_t *row = settings_row(content);
    lv_obj_set_height(row, 64);
    lv_obj_t *timeout_lbl = lv_label_create(row);
    lv_label_set_text(timeout_lbl, "Screen timeout");
    lv_obj_align(timeout_lbl, LV_ALIGN_LEFT_MID, 8, 0);
    s.dd_timeout = lv_dropdown_create(row);
    lv_dropdown_set_options(s.dd_timeout, "10 s\n30 s\n60 s\n120 s\nAlways on");
    lv_obj_set_size(s.dd_timeout, 176, 56);
    lv_obj_align(s.dd_timeout, LV_ALIGN_RIGHT_MID, -8, 0);
    int selected = 1; /* default 30 s */
    for (int i = 0; i < 5; ++i) {
        if (s_timeout_seconds[i] == cfg->screen_timeout_s) {
            selected = i;
            break;
        }
    }
    lv_dropdown_set_selected(s.dd_timeout, (uint16_t)selected);
    lv_obj_add_event_cb(s.dd_timeout, timeout_change_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    section_title(content, "Audio");
    slider_row(content, "Volume", 0, 100, cfg->volume, &s.lbl_volume,
               volume_change_cb);
    lv_label_set_text_fmt(s.lbl_volume, "%d%%", cfg->volume);
    slider_row(content, "Mic gain", 0, 36, cfg->mic_gain_db, &s.lbl_gain,
               gain_change_cb);
    lv_label_set_text_fmt(s.lbl_gain, "%d dB", cfg->mic_gain_db);

    section_title(content, "Clock");
    rtc_section_build(content);

    lv_obj_t *about_row = settings_row(content);
    lv_obj_set_height(about_row, 72);
    lv_obj_t *about_btn = lv_button_create(about_row);
    lv_obj_set_size(about_btn, SETTINGS_ROW_WIDTH - 16, 64);
    lv_obj_align(about_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(about_btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_add_event_cb(about_btn, about_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *about_lbl = lv_label_create(about_btn);
    lv_label_set_text(about_lbl, "About");
    lv_obj_center(about_lbl);

    return root;
}
