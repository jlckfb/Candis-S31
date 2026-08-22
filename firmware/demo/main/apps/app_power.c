/*
 * Candis-S31 watch demo - power app.
 *
 * Battery detail page (voltage, percentage, charge state, input-current
 * limit readback), screen-off timeout selector (persisted through
 * demo_settings + svc_power_set_screen_timeout), immediate screen-off,
 * deep sleep with RTC-alarm presets (1/5/30 min) or PWR-key-only wake,
 * and shutdown behind a confirm box with the VBUS guard.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "demo_apps.h"
#include "demo_board.h"
#include "services/svc_audio.h"
#include "services/svc_power.h"
#include "ui/ui_manager.h"

#define POWER_REFRESH_MS 1000

static const int s_timeout_options[] = { 10, 30, 60, 120, 0 };
#define TIMEOUT_OPTION_COUNT 5

static struct {
    bool active;
    lv_obj_t *lbl_voltage;
    lv_obj_t *lbl_charge;
    lv_obj_t *timeout_btns[TIMEOUT_OPTION_COUNT];
    lv_obj_t *lbl_limit;
    lv_obj_t *lbl_source_mode;
    lv_timer_t *timer;
} s_power;

/* ------------------------------------------------------------------ */
/* Battery info refresh                                                */
/* ------------------------------------------------------------------ */

static const char *charge_state_text(const svc_power_status_t *st)
{
    if (!st->present) {
        return "No battery detected";
    }
    if (st->charge_done) {
        return "Full";
    }
    if (st->charging) {
        return "Charging";
    }
    if (st->vbus) {
        return "USB attached, not charging";
    }
    return "Not charging (battery)";
}

/* Wording limited to glyphs the UI font subset already carries. The
 * target is the verified REG62 ceiling, not a measured current. */
static const char *charge_phase_text(svc_power_charge_phase_t phase)
{
    switch (phase) {
    case SVC_POWER_CHARGE_TRICKLE: return "50mA observe";
    case SVC_POWER_CHARGE_RAMP:    return "Ramping";
    case SVC_POWER_CHARGE_HOLD:    return "At target";
    case SVC_POWER_CHARGE_FAULT:   return "Degraded";
    default:                       return "";
    }
}


static void power_refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_power.active) {
        return;
    }
    svc_power_status_t st;
    svc_power_get_status(&st);

    if (st.present && st.percent >= 0) {
        /* Reference model = vendor generic SOC (参考); any runtime
         * override is a custom model of unproven cell specificity -
         * neutral 自定, never a calibration claim. */
        lv_label_set_text_fmt(s_power.lbl_voltage,
                              "Battery: %d mV  SOC: %d%% (%s)",
                              st.battery_mv, st.percent,
                              st.fuel_gauge_reference_model ? "ref" : "custom");
    } else if (st.present) {
        /* Battery fitted but no model active this boot: honest empty
         * state, never a voltage-derived percent. */
        lv_label_set_text_fmt(s_power.lbl_voltage,
                              "Battery: %d mV  SOC model not loaded",
                              st.battery_mv);
    } else {
        lv_label_set_text_fmt(s_power.lbl_voltage, "Battery: %d mV  SOC: --",
                              st.battery_mv);
    }
    if (st.vbus && st.charge_target_ma > 0) {
        /* Target = verified REG62 ceiling; the real current can be lower
         * at any moment (VINDPM back-off) and is not measured here. */
        lv_label_set_text_fmt(s_power.lbl_charge, "Charge: %s  target: %d mA (%s)",
                              charge_state_text(&st), st.charge_target_ma,
                              charge_phase_text(st.charge_phase));
    } else {
        lv_label_set_text_fmt(s_power.lbl_charge, "Charge: %s%s",
                              charge_state_text(&st),
                              st.vbus ? "  VBUS present" : "");
    }

    uint16_t limit_ma = 0;
    if (bsp_pmic_get_input_current_limit(&limit_ma) == ESP_OK) {
        lv_label_set_text_fmt(s_power.lbl_limit, "Input limit: %u mA", limit_ma);
    } else {
        lv_label_set_text(s_power.lbl_limit, "Input limit: read failed");
    }
    /* Charge-ceiling mode: PC/unknown sessions default to a 200 mA
     * REG62 ceiling (risk reduction, NOT an input-limit guarantee -
     * REG16 stays 2000 mA); the 500 mA ceiling needs the explicit
     * per-session confirmation below. */
    lv_label_set_text_fmt(s_power.lbl_source_mode,
                          st.source_verified ?
                          "External source confirmed, charge ceiling 500mA" :
                          "PC debug, charge ceiling 200mA");
}

/* ------------------------------------------------------------------ */
/* Screen-off timeout                                                  */
/* ------------------------------------------------------------------ */

static void timeout_style_refresh(void)
{
    const int current = demo_settings()->screen_timeout_s;
    for (int i = 0; i < TIMEOUT_OPTION_COUNT; ++i) {
        const bool selected = s_timeout_options[i] == current;
        lv_obj_set_style_bg_color(s_power.timeout_btns[i],
                                  lv_color_hex(selected ? UI_COLOR_ACCENT
                                                        : UI_COLOR_SURFACE), 0);
    }
}

static void timeout_btn_cb(lv_event_t *event)
{
    if (!s_power.active) {
        return;
    }
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= TIMEOUT_OPTION_COUNT) {
        return;
    }
    const int seconds = s_timeout_options[index];
    demo_settings()->screen_timeout_s = seconds;
    demo_settings_save();
    svc_power_set_screen_timeout(seconds);
    timeout_style_refresh();
    ui_toast(seconds == 0 ? "Screen timeout: always on" : "Screen timeout updated");
}

/* ------------------------------------------------------------------ */
/* Immediate screen off                                                */
/* ------------------------------------------------------------------ */

static void screen_off_btn_cb(lv_event_t *event)
{
    (void)event;
    if (!s_power.active) {
        return;
    }
    svc_power_screen_off();
}

/* ------------------------------------------------------------------ */
/* Deep sleep                                                          */
/* ------------------------------------------------------------------ */

/* user = minutes (0 = PWR-key-only wake). Runs on the LVGL thread; on
 * success svc_power_deep_sleep() never returns. */
static void deep_sleep_confirm_cb(bool ok, void *user)
{
    if (!ok || !s_power.active) {
        return;
    }
    const int minutes = (int)(intptr_t)user;
    const esp_err_t err = svc_power_deep_sleep(minutes);
    /* Only reached when deep sleep was refused. */
    if (err == ESP_ERR_INVALID_STATE &&
            (svc_audio_is_recording() || svc_audio_is_playing())) {
        ui_toast("Stop recording/playback first");
    } else if (err == ESP_ERR_INVALID_STATE) {
        ui_toast("Deep sleep refused: busy");
    } else {
        ui_toast("Deep sleep failed");
    }
}

static void deep_sleep_btn_cb(lv_event_t *event)
{
    if (!s_power.active) {
        return;
    }
    const int minutes = (int)(intptr_t)lv_event_get_user_data(event);
    char text[96];
    if (minutes > 0) {
        snprintf(text, sizeof(text), "Enter deep sleep?\nRTC alarm wakes after %d min\n(PWR key also wakes)",
                 minutes);
    } else {
        snprintf(text, sizeof(text), "Enter deep sleep?\nPWR key wake only");
    }
    ui_msgbox("Deep sleep", text, deep_sleep_confirm_cb, (void *)(intptr_t)minutes);
}

/* ------------------------------------------------------------------ */
/* Shutdown                                                            */
/* ------------------------------------------------------------------ */

static void shutdown_confirm_cb(bool ok, void *user)
{
    (void)user;
    if (!ok || !s_power.active) {
        return;
    }
    svc_power_status_t st;
    svc_power_get_status(&st);
    if (st.vbus) {
        ui_toast("Unplug USB before power-off");
        return;
    }
    const esp_err_t err = svc_power_shutdown();
    /* Only reached when shutdown was refused or failed. */
    if (err == ESP_ERR_INVALID_STATE) {
        svc_power_get_status(&st);
        if (st.vbus) {
            ui_toast("Power-off refused: USB attached");
        } else if (svc_audio_is_recording() || svc_audio_is_playing()) {
            ui_toast("Stop recording/playback first");
        } else {
            ui_toast("Power-off refused: busy");
        }
    } else if (err != ESP_OK) {
        ui_toast("Power-off failed");
    }
}

/* Runs on the LVGL thread after the msgbox confirm. */
static void source_confirm_cb(bool ok, void *user)
{
    (void)user;
    if (!ok || !s_power.active) {
        return;
    }
    if (svc_power_set_external_source_verified(true) != ESP_OK) {
        ui_toast("Power service unavailable");
    }
}

static void source_confirm_btn_cb(lv_event_t *event)
{
    if (!s_power.active) {
        return;
    }
    ui_msgbox("External source",
              "Confirm external source?\nCharge ceiling 500mA\nInput limit 2000mA, PC USB power NOT guaranteed\nDo not enable on a PC port",
              source_confirm_cb, NULL);
}

static void source_safe_btn_cb(lv_event_t *event)
{
    if (!s_power.active) {
        return;
    }
    if (svc_power_set_external_source_verified(false) == ESP_OK) {
        ui_toast("Restore 200mA");
    }
}

static void shutdown_btn_cb(lv_event_t *event)
{
    (void)event;
    if (!s_power.active) {
        return;
    }
    ui_msgbox("Power off", "Power off the device?\nHold PWR to boot again",
              shutdown_confirm_cb, NULL);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static lv_obj_t *power_make_button(lv_obj_t *parent, int x, int y, int w,
                                   int h, const char *text,
                                   lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_body(), 0);
    lv_obj_center(label);
    return btn;
}

static void power_delete_cb(lv_event_t *event)
{
    (void)event;
    s_power.active = false;
    if (s_power.timer != NULL) {
        lv_timer_delete(s_power.timer);
        s_power.timer = NULL;
    }
}

lv_obj_t *app_power_create(void)
{
    memset(&s_power, 0, sizeof(s_power));
    s_power.active = true;

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("Power", &content);
    lv_obj_add_event_cb(root, power_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_text_font(content, ui_font_body(), 0);

    /* Every section of the page sits inside the first viewport (the
     * scaffold content area, 460 x 364 minus padding): the old layout
     * parked the external-source confirmation at y=372, below the fold,
     * so the 500 mA path was unreachable. The deep-sleep note rides the
     * scroll tail, reachable via the built-in scrollbar. Rows are
     * stacked top-down with no overlap: battery detail, source control,
     * timeout selector, immediate actions, deep sleep. */

    /* Battery detail block. */
    s_power.lbl_voltage = lv_label_create(content);
    lv_obj_set_pos(s_power.lbl_voltage, 2, 0);
    s_power.lbl_charge = lv_label_create(content);
    lv_obj_set_pos(s_power.lbl_charge, 2, 19);
    s_power.lbl_limit = lv_label_create(content);
    lv_obj_set_pos(s_power.lbl_limit, 2, 38);
    lv_obj_set_style_text_color(s_power.lbl_limit,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    s_power.lbl_source_mode = lv_label_create(content);
    lv_obj_set_pos(s_power.lbl_source_mode, 2, 57);
    lv_obj_set_style_text_color(s_power.lbl_source_mode,
                                lv_color_hex(UI_COLOR_ACCENT), 0);

    /* Smallest complete source control: explicit confirm (with warning)
     * and explicit return to the PC-safe ceiling. Directly below the
     * battery block so it is visible without scrolling. */
    power_make_button(content, 0, 81, 208, 56, "Ext source 500mA",
                      source_confirm_btn_cb, NULL);
    power_make_button(content, 214, 81, 208, 56, "Restore 200",
                      source_safe_btn_cb, NULL);

    /* Screen-off timeout selector. */
    lv_obj_t *cap_timeout = lv_label_create(content);
    lv_label_set_text(cap_timeout, "Screen timeout");
    lv_obj_set_pos(cap_timeout, 2, 141);
    lv_obj_set_style_text_color(cap_timeout, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    static const char *timeout_names[TIMEOUT_OPTION_COUNT] = {
        "10s", "30s", "60s", "120s", "Always",
    };
    for (int i = 0; i < TIMEOUT_OPTION_COUNT; ++i) {
        s_power.timeout_btns[i] =
            power_make_button(content, i * 87, 159, 80, 56, timeout_names[i],
                              timeout_btn_cb, (void *)(intptr_t)i);
    }
    timeout_style_refresh();

    /* Immediate actions. */
    power_make_button(content, 0, 221, 208, 56, "Screen off now",
                      screen_off_btn_cb, NULL);
    power_make_button(content, 214, 221, 208, 56, "Power off",
                      shutdown_btn_cb, NULL);

    /* Deep sleep presets, one row of four. */
    lv_obj_t *cap_deep = lv_label_create(content);
    lv_label_set_text(cap_deep, "Deep sleep (reboots on wake)");
    lv_obj_set_pos(cap_deep, 2, 281);
    lv_obj_set_style_text_color(cap_deep, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    static const struct {
        const char *name;
        int minutes;
    } deep_presets[] = {
        { "1 min", 1 },
        { "5 min", 5 },
        { "30 min", 30 },
        { "Key only", 0 },
    };
    for (size_t i = 0; i < sizeof(deep_presets) / sizeof(deep_presets[0]); ++i) {
        power_make_button(content, (int)i * 107, 299, 100, 56,
                          deep_presets[i].name, deep_sleep_btn_cb,
                          (void *)(intptr_t)deep_presets[i].minutes);
    }

    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, "Deep sleep powers off the screen; RTC alarm or a short PWR press reboots.");
    lv_obj_set_pos(hint, 2, 361);
    lv_obj_set_width(hint, 420);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    power_refresh_cb(NULL);
    s_power.timer = lv_timer_create(power_refresh_cb, POWER_REFRESH_MS, NULL);
    return root;
}
