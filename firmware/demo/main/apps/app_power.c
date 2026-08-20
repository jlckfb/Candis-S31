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
#include "services/svc_power.h"
#include "ui/ui_manager.h"

#define POWER_REFRESH_MS 1000

static const int s_timeout_options[] = { 10, 30, 60, 120, 0 };
#define TIMEOUT_OPTION_COUNT 5

static struct {
    bool active;
    lv_obj_t *lbl_voltage;
    lv_obj_t *lbl_charge;
    lv_obj_t *lbl_limit;
    lv_obj_t *timeout_btns[TIMEOUT_OPTION_COUNT];
    lv_timer_t *timer;
} s_power;

/* ------------------------------------------------------------------ */
/* Battery info refresh                                                */
/* ------------------------------------------------------------------ */

static const char *charge_state_text(const svc_power_status_t *st)
{
    if (!st->present) {
        return "未检测到电池";
    }
    if (st->charge_done) {
        return "已充满";
    }
    if (st->charging) {
        return "充电中";
    }
    if (st->vbus) {
        return "USB 连接,未在充电";
    }
    return "未充电(电池供电)";
}

static void power_refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_power.active) {
        return;
    }
    svc_power_status_t st;
    svc_power_get_status(&st);

    if (st.present) {
        lv_label_set_text_fmt(s_power.lbl_voltage, "电池电压:%d mV    电量:%d%%",
                              st.battery_mv, st.percent >= 0 ? st.percent : 0);
    } else {
        lv_label_set_text_fmt(s_power.lbl_voltage, "电池电压:%d mV    电量:--",
                              st.battery_mv);
    }
    lv_label_set_text_fmt(s_power.lbl_charge, "充电状态:%s%s",
                          charge_state_text(&st), st.vbus ? "    VBUS 在位" : "");

    uint16_t limit_ma = 0;
    if (bsp_pmic_get_input_current_limit(&limit_ma) == ESP_OK) {
        lv_label_set_text_fmt(s_power.lbl_limit, "输入限流:%u mA", limit_ma);
    } else {
        lv_label_set_text(s_power.lbl_limit, "输入限流:读取失败");
    }
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
    ui_toast(seconds == 0 ? "息屏超时:常亮" : "息屏超时已更新");
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
    (void)err;
    ui_toast("深度睡眠失败");
}

static void deep_sleep_btn_cb(lv_event_t *event)
{
    if (!s_power.active) {
        return;
    }
    const int minutes = (int)(intptr_t)lv_event_get_user_data(event);
    char text[96];
    if (minutes > 0) {
        snprintf(text, sizeof(text), "进入深度睡眠?\n%d 分钟后由 RTC 闹钟唤醒\n(也可随时按 PWR 键唤醒)",
                 minutes);
    } else {
        snprintf(text, sizeof(text), "进入深度睡眠?\n仅 PWR 键可唤醒");
    }
    ui_msgbox("深度睡眠", text, deep_sleep_confirm_cb, (void *)(intptr_t)minutes);
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
        ui_toast("请先拔掉 USB 线再关机");
        return;
    }
    const esp_err_t err = svc_power_shutdown();
    /* Only reached when shutdown was refused or failed. */
    if (err == ESP_ERR_INVALID_STATE) {
        ui_toast("关机被拒绝:USB 已连接");
    } else if (err != ESP_OK) {
        ui_toast("关机失败");
    }
}

static void shutdown_btn_cb(lv_event_t *event)
{
    (void)event;
    if (!s_power.active) {
        return;
    }
    ui_msgbox("关机", "确定关闭设备电源?\n关机后需长按 PWR 键开机",
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
    lv_obj_t *root = ui_app_scaffold("电源", &content);
    lv_obj_add_event_cb(root, power_delete_cb, LV_EVENT_DELETE, NULL);

    /* Battery detail block. */
    s_power.lbl_voltage = lv_label_create(content);
    lv_obj_set_pos(s_power.lbl_voltage, 2, 0);
    s_power.lbl_charge = lv_label_create(content);
    lv_obj_set_pos(s_power.lbl_charge, 2, 24);
    s_power.lbl_limit = lv_label_create(content);
    lv_obj_set_pos(s_power.lbl_limit, 2, 48);
    lv_obj_set_style_text_color(s_power.lbl_limit,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    /* Screen-off timeout selector. */
    lv_obj_t *cap_timeout = lv_label_create(content);
    lv_label_set_text(cap_timeout, "息屏超时");
    lv_obj_set_pos(cap_timeout, 2, 78);
    lv_obj_set_style_text_color(cap_timeout, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    static const char *timeout_names[TIMEOUT_OPTION_COUNT] = {
        "10秒", "30秒", "60秒", "120秒", "常亮",
    };
    for (int i = 0; i < TIMEOUT_OPTION_COUNT; ++i) {
        s_power.timeout_btns[i] =
            power_make_button(content, i * 90, 102, 86, 40, timeout_names[i],
                              timeout_btn_cb, (void *)(intptr_t)i);
    }
    timeout_style_refresh();

    /* Immediate actions. */
    power_make_button(content, 8, 158, 200, 46, "立即息屏",
                      screen_off_btn_cb, NULL);
    power_make_button(content, 240, 158, 200, 46, "关机",
                      shutdown_btn_cb, NULL);

    /* Deep sleep presets. */
    lv_obj_t *cap_deep = lv_label_create(content);
    lv_label_set_text(cap_deep, "深度睡眠(唤醒后系统重启)");
    lv_obj_set_pos(cap_deep, 2, 220);
    lv_obj_set_style_text_color(cap_deep, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    static const struct {
        const char *name;
        int minutes;
    } deep_presets[] = {
        { "1 分钟后", 1 },
        { "5 分钟后", 5 },
        { "30 分钟后", 30 },
        { "仅按键", 0 },
    };
    for (size_t i = 0; i < sizeof(deep_presets) / sizeof(deep_presets[0]); ++i) {
        power_make_button(content, (int)i * 112, 244, 108, 44,
                          deep_presets[i].name, deep_sleep_btn_cb,
                          (void *)(intptr_t)deep_presets[i].minutes);
    }

    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, "提示:深度睡眠期间屏幕与外设全部断电,\n"
                            "RTC 闹钟或 PWR 短按唤醒后重新开机。");
    lv_obj_set_pos(hint, 2, 300);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    power_refresh_cb(NULL);
    s_power.timer = lv_timer_create(power_refresh_cb, POWER_REFRESH_MS, NULL);
    return root;
}
