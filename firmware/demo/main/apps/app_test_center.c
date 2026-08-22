/*
 * Candis-S31 watch demo - integrated EVT test-center overview.
 *
 * This page deliberately reports observable state instead of inventing pass
 * results. Human-observed and destructive tests remain inside their focused
 * apps until a persistent test-session model is added.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "demo_apps.h"
#include "services/svc_audio.h"
#include "services/svc_net.h"
#include "services/svc_power.h"
#include "services/svc_storage.h"
#include "ui/ui_manager.h"

#define TEST_REFRESH_MS 1000

typedef enum {
    TEST_ROW_DISPLAY = 0,
    TEST_ROW_AUDIO,
    TEST_ROW_WIFI,
    TEST_ROW_BLE,
    TEST_ROW_STORAGE,
    TEST_ROW_USB,
    TEST_ROW_POWER,
    TEST_ROW_CAMERA,
    TEST_ROW_COUNT,
} test_row_id_t;

typedef struct {
    lv_obj_t *status;
    const char *app_id;
} test_row_t;

static struct {
    bool active;
    lv_timer_t *timer;
    lv_obj_t *root;
    lv_obj_t *summary;
    test_row_t rows[TEST_ROW_COUNT];
} s_test;

static void set_status(test_row_id_t id, const char *text, uint32_t color)
{
    lv_obj_t *label = s_test.rows[id].status;
    if (!label) {
        return;
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static void open_app_cb(lv_event_t *event)
{
    const char *app_id = lv_event_get_user_data(event);
    if (app_id) {
        ui_nav_open(app_id);
    }
}

static lv_obj_t *test_row_create(lv_obj_t *parent, test_row_id_t id,
                                 const char *icon, const char *title,
                                 const char *app_id)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 68);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_RADIUS_CARD, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2C2C34), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x232329), LV_STATE_PRESSED);
    if (app_id) {
        lv_obj_add_event_cb(row, open_app_cb, LV_EVENT_CLICKED,
                            (void *)app_id);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *icon_lbl = lv_label_create(row);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_align(icon_lbl, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t *title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, ui_font_title(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 50, 0);

    lv_obj_t *status = lv_label_create(row);
    lv_label_set_text(status, "Untested");
    lv_obj_set_style_text_font(status, ui_font_body(), 0);
    lv_obj_set_width(status, 126);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(status, lv_color_hex(UI_COLOR_WARN), 0);
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, app_id ? -24 : 0, 0);

    if (app_id) {
        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(arrow, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 2, 0);
    }

    s_test.rows[id] = (test_row_t){ .status = status, .app_id = app_id };
    return row;
}

static void refresh_status(lv_timer_t *timer)
{
    (void)timer;
    if (!s_test.active || lv_screen_active() != s_test.root) {
        return;
    }

    set_status(TEST_ROW_DISPLAY, "Needs manual check", UI_COLOR_WARN);

    if (svc_audio_is_recording()) {
        set_status(TEST_ROW_AUDIO, "Recording", UI_COLOR_ACCENT);
    } else if (svc_audio_is_playing()) {
        set_status(TEST_ROW_AUDIO, "Playing", UI_COLOR_ACCENT);
    } else {
        set_status(TEST_ROW_AUDIO, "4-route pending", UI_COLOR_WARN);
    }

    char ip[20] = { 0 };
    if (svc_wifi_is_connected(ip, sizeof(ip))) {
        set_status(TEST_ROW_WIFI, "Connected", UI_COLOR_OK);
    } else {
        set_status(TEST_ROW_WIFI, "Not connected", UI_COLOR_WARN);
    }
    set_status(TEST_ROW_BLE, "Enter to test", UI_COLOR_WARN);

    if (svc_storage_mounted()) {
        set_status(TEST_ROW_STORAGE, "TF mounted", UI_COLOR_OK);
    } else {
        set_status(TEST_ROW_STORAGE, "No TF card", UI_COLOR_TEXT_DIM);
    }
    set_status(TEST_ROW_USB, "Enter to test", UI_COLOR_WARN);

    svc_power_status_t power;
    svc_power_get_status(&power);
    if (power.vbus && power.charge_target_ma > 0) {
        /* Target = verified REG62 ceiling written by the charge
         * controller; actual current is not measured. */
        char text[40];
        snprintf(text, sizeof(text), "%s - target %d/%d mA",
                 power.charging ? "Charging" : "USB powered",
                 power.charge_target_ma, power.charge_ceiling_ma);
        set_status(TEST_ROW_POWER, text,
                   power.charge_phase == SVC_POWER_CHARGE_FAULT ?
                   UI_COLOR_WARN : UI_COLOR_OK);
    } else if (power.present && !power.fuel_gauge_valid) {
        set_status(TEST_ROW_POWER, "SOC model not loaded", UI_COLOR_WARN);
    } else if (power.percent >= 0) {
        char text[32];
        snprintf(text, sizeof(text), "%d%% - %s", power.percent,
                 power.fuel_gauge_reference_model ? "ref" : "custom");
        set_status(TEST_ROW_POWER, text,
                   power.percent <= 20 ? UI_COLOR_ERR : UI_COLOR_OK);
    } else if (power.vbus) {
        set_status(TEST_ROW_POWER, "USB powered", UI_COLOR_OK);
    } else {
        set_status(TEST_ROW_POWER, "Untested", UI_COLOR_WARN);
    }

    set_status(TEST_ROW_CAMERA, "FPC adapter pending", UI_COLOR_WARN);
    lv_label_set_text(s_test.summary,
                      "8 test domains - camera blocked\nLive status and pending items, no fake passes");
}

static void root_delete_cb(lv_event_t *event)
{
    (void)event;
    s_test.active = false;
    if (s_test.timer) {
        lv_timer_delete(s_test.timer);
        s_test.timer = NULL;
    }
}

lv_obj_t *app_test_center_create(void)
{
    s_test = (typeof(s_test)){ .active = true };

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("Test center", &content);
    s_test.root = root;
    lv_obj_add_event_cb(root, root_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, UI_GAP, 0);

    lv_obj_t *summary_card = lv_obj_create(content);
    lv_obj_set_width(summary_card, LV_PCT(100));
    lv_obj_set_height(summary_card, 112);
    lv_obj_set_style_bg_color(summary_card, lv_color_hex(0x10243A), 0);
    lv_obj_set_style_bg_opa(summary_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(summary_card, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(summary_card, 1, 0);
    lv_obj_set_style_radius(summary_card, UI_RADIUS_CARD, 0);
    lv_obj_remove_flag(summary_card, LV_OBJ_FLAG_SCROLLABLE |
                                     LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *summary_title = lv_label_create(summary_card);
    lv_label_set_text(summary_title, "EVT self-check");
    lv_obj_set_style_text_font(summary_title, ui_font_title(), 0);
    lv_obj_set_style_text_color(summary_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(summary_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_test.summary = lv_label_create(summary_card);
    lv_obj_set_style_text_font(s_test.summary, ui_font_body(), 0);
    lv_obj_set_width(s_test.summary, LV_PCT(100));
    lv_label_set_long_mode(s_test.summary, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_test.summary,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(s_test.summary, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    test_row_create(content, TEST_ROW_DISPLAY, LV_SYMBOL_EYE_OPEN,
                    "Screen+touch", "display");
    test_row_create(content, TEST_ROW_AUDIO, LV_SYMBOL_AUDIO,
                    "Mics+speaker", "recorder");
    test_row_create(content, TEST_ROW_WIFI, LV_SYMBOL_WIFI,
                    "WiFi", "wifi");
    test_row_create(content, TEST_ROW_BLE, LV_SYMBOL_BLUETOOTH,
                    "Bluetooth", "ble");
    test_row_create(content, TEST_ROW_STORAGE, LV_SYMBOL_SD_CARD,
                    "TF storage", "files");
    test_row_create(content, TEST_ROW_USB, LV_SYMBOL_USB,
                    "USB OTG", "usb");
    test_row_create(content, TEST_ROW_POWER, LV_SYMBOL_POWER,
                    "Battery+low power", "power");
    test_row_create(content, TEST_ROW_CAMERA, LV_SYMBOL_IMAGE,
                    "Camera", "camera");

    s_test.timer = lv_timer_create(refresh_status, TEST_REFRESH_MS, NULL);
    refresh_status(s_test.timer);
    return root;
}
