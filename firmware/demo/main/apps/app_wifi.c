/*
 * Candis-S31 watch demo - WiFi app: scan, connect (on-screen keyboard for
 * secured networks), show IP, disconnect.
 *
 * All svc_net callbacks run on the network task; UI updates are forwarded
 * with ui_async() using heap-allocated payloads. The root screen's
 * LV_EVENT_DELETE tears the app down: it disconnects the link it created and
 * flips the status-bar icon off.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#include "demo_apps.h"
#include "services/svc_net.h"
#include "ui/ui_keyboard.h"
#include "ui/ui_manager.h"

#define APP_MAX_APS 20

typedef enum {
    WF_STATE_IDLE,
    WF_STATE_SCANNING,
    WF_STATE_CONNECTING,
    WF_STATE_CONNECTED,
} wf_state_t;

typedef struct {
    bool active;
    wf_state_t state;
    lv_obj_t *root;
    lv_obj_t *lbl_status;
    lv_obj_t *btn_action;
    lv_obj_t *lbl_action;
    lv_obj_t *list;
    lv_obj_t *spinner;
    lv_obj_t *lbl_connecting;
    char ssid[33];      /* AP selected for connection */
    char ip[16];
    int ap_count;
    svc_wifi_ap_t aps[APP_MAX_APS];
} wf_app_t;

static wf_app_t s;

/* ---------------- async payloads ---------------- */

typedef struct {
    int count;                    /* -1 = scan failed */
    svc_wifi_ap_t aps[APP_MAX_APS];
} wf_scan_result_t;

typedef struct {
    svc_wifi_event_t ev;
    char detail[48];
} wf_conn_result_t;

static void *wf_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ---------------- UI helpers (LVGL context only) ---------------- */

static void wf_status_set(const char *text, uint32_t color)
{
    if (!s.active || s.lbl_status == NULL) {
        return;
    }
    lv_label_set_text(s.lbl_status, text);
    lv_obj_set_style_text_color(s.lbl_status, lv_color_hex(color), 0);
}

static void wf_spinner_show(bool show, const char *text)
{
    if (s.spinner != NULL) {
        lv_obj_set_flag(s.spinner, LV_OBJ_FLAG_HIDDEN, !show);
    }
    if (s.lbl_connecting != NULL) {
        lv_obj_set_flag(s.lbl_connecting, LV_OBJ_FLAG_HIDDEN, !show);
        if (show && text != NULL) {
            lv_label_set_text(s.lbl_connecting, text);
        }
    }
    if (s.list != NULL) {
        lv_obj_set_flag(s.list, LV_OBJ_FLAG_HIDDEN, show);
    }
}

static void wf_action_button(const char *text, bool visible)
{
    if (s.btn_action == NULL) {
        return;
    }
    lv_obj_set_flag(s.btn_action, LV_OBJ_FLAG_HIDDEN, !visible);
    if (visible && text != NULL) {
        lv_label_set_text(s.lbl_action, text);
    }
}

static void wf_rebuild_list(void)
{
    if (s.list == NULL) {
        return;
    }
    lv_obj_clean(s.list);
    if (s.ap_count == 0) {
        lv_obj_t *empty = lv_label_create(s.list);
        lv_label_set_text(empty, "未找到网络");
        lv_obj_set_style_text_color(empty, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 40, 0);
        return;
    }
    for (int i = 0; i < s.ap_count; ++i) {
        const svc_wifi_ap_t *ap = &s.aps[i];
        lv_obj_t *row = lv_button_create(s.list);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_style_pad_right(row, 12, 0);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);

        lv_obj_t *lbl_ssid = lv_label_create(row);
        lv_label_set_text(lbl_ssid, ap->ssid[0] ? ap->ssid : "(隐藏)");
        lv_label_set_long_mode(lbl_ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_ssid, 200);

        if (ap->authmode != 0 /* WIFI_AUTH_OPEN */) {
            lv_obj_t *lock = lv_label_create(row);
            lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE);
            lv_obj_set_style_text_color(lock, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
        }

        lv_obj_t *spacer = lv_obj_create(row);
        lv_obj_set_size(spacer, 1, 1);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_grow(spacer, 1);

        lv_obj_t *bar = lv_bar_create(row);
        lv_obj_set_size(bar, 52, 8);
        lv_bar_set_range(bar, 0, 100);
        int pct = (ap->rssi + 90) * 100 / 60; /* -90..-30 dBm */
        if (pct < 0) {
            pct = 0;
        } else if (pct > 100) {
            pct = 100;
        }
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_ACCENT),
                                  LV_PART_INDICATOR);

        lv_obj_t *lbl_dbm = lv_label_create(row);
        lv_label_set_text_fmt(lbl_dbm, "%d", ap->rssi);
        lv_obj_set_style_text_color(lbl_dbm, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
        lv_obj_set_width(lbl_dbm, 48);
        lv_obj_set_style_text_align(lbl_dbm, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* ---------------- landing functions (LVGL thread) ---------------- */

static void wf_apply_scan(void *arg)
{
    wf_scan_result_t *res = arg;
    if (s.active && s.state == WF_STATE_SCANNING) {
        s.state = WF_STATE_IDLE;
        if (res->count < 0) {
            wf_status_set("扫描失败", UI_COLOR_ERR);
        } else {
            s.ap_count = res->count;
            memcpy(s.aps, res->aps, sizeof(s.aps[0]) * res->count);
            wf_status_set(res->count ? "选择网络" : "未找到网络",
                          UI_COLOR_TEXT);
            wf_rebuild_list();
        }
        wf_spinner_show(false, NULL);
        wf_action_button("扫描", true);
    }
    heap_caps_free(res);
}

static void wf_apply_conn(void *arg)
{
    wf_conn_result_t *res = arg;
    if (!s.active) {
        heap_caps_free(res);
        return;
    }
    switch (res->ev) {
    case SVC_WIFI_EV_CONNECTED:
        s.state = WF_STATE_CONNECTED;
        snprintf(s.ip, sizeof(s.ip), "%s", res->detail);
        wf_spinner_show(false, NULL);
        wf_status_set("已连接", UI_COLOR_OK);
        if (s.lbl_status != NULL) {
            lv_label_set_text_fmt(s.lbl_status, "已连接 IP %s", s.ip);
        }
        wf_action_button("断开", true);
        ui_status_set_wifi(2);
        break;
    case SVC_WIFI_EV_CONNECT_FAILED:
        s.state = WF_STATE_IDLE;
        wf_spinner_show(false, NULL);
        wf_status_set("连接失败", UI_COLOR_ERR);
        wf_action_button("扫描", true);
        ui_status_set_wifi(0);
        ui_msgbox("连接失败", res->detail, NULL, NULL);
        break;
    case SVC_WIFI_EV_DISCONNECTED:
    default:
        s.state = WF_STATE_IDLE;
        s.ip[0] = '\0';
        wf_spinner_show(false, NULL);
        wf_status_set("已断开", UI_COLOR_TEXT_DIM);
        wf_action_button("扫描", true);
        ui_status_set_wifi(0);
        break;
    }
    heap_caps_free(res);
}

/* ---------------- service callbacks (network task) ---------------- */

static void wf_scan_cb(const svc_wifi_ap_t *aps, int count, void *user)
{
    (void)user;
    if (!s.active) {
        return;
    }
    wf_scan_result_t *res = wf_alloc(sizeof(*res));
    if (res == NULL) {
        return;
    }
    if (aps == NULL) {
        res->count = -1;
    } else {
        res->count = count > APP_MAX_APS ? APP_MAX_APS : count;
        memcpy(res->aps, aps, sizeof(aps[0]) * res->count);
    }
    ui_async(wf_apply_scan, res);
}

static void wf_conn_cb(svc_wifi_event_t ev, const char *detail, void *user)
{
    (void)user;
    if (!s.active) {
        return;
    }
    wf_conn_result_t *res = wf_alloc(sizeof(*res));
    if (res == NULL) {
        return;
    }
    res->ev = ev;
    snprintf(res->detail, sizeof(res->detail), "%s", detail ? detail : "");
    ui_async(wf_apply_conn, res);
}

/* ---------------- user actions (LVGL context) ---------------- */

static void wf_start_connect(void)
{
    if (s.ssid[0] == '\0') {
        return;
    }
    s.state = WF_STATE_CONNECTING;
    char line[48];
    snprintf(line, sizeof(line), "正在连接 %s...", s.ssid);
    wf_spinner_show(true, line);
    wf_status_set("连接中", UI_COLOR_WARN);
    wf_action_button(NULL, false);
    ui_status_set_wifi(1);
}

static void wf_try_connect(const char *password)
{
    if (svc_wifi_connect(s.ssid, password, wf_conn_cb, NULL) != ESP_OK) {
        s.state = WF_STATE_IDLE;
        wf_spinner_show(false, NULL);
        wf_status_set("连接失败", UI_COLOR_ERR);
        wf_action_button("扫描", true);
        return;
    }
    wf_start_connect();
}

static void wf_keyboard_done(const char *text, void *user)
{
    (void)user;
    if (!s.active || text == NULL) {
        return; /* cancelled */
    }
    wf_try_connect(text);
}

static void wf_row_clicked(lv_event_t *event)
{
    lv_obj_t *row = lv_event_get_target_obj(event);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    if (!s.active || s.state != WF_STATE_IDLE || idx < 0 ||
            idx >= s.ap_count) {
        return;
    }
    const svc_wifi_ap_t *ap = &s.aps[idx];
    snprintf(s.ssid, sizeof(s.ssid), "%s", ap->ssid);
    if (ap->authmode == 0 /* WIFI_AUTH_OPEN */) {
        wf_try_connect("");
    } else {
        ui_keyboard_open(s.ssid, "", true, wf_keyboard_done, NULL);
    }
}

static void wf_on_scan(lv_event_t *event)
{
    (void)event;
    if (!s.active || s.state != WF_STATE_IDLE) {
        return;
    }
    if (svc_wifi_scan(wf_scan_cb, NULL) != ESP_OK) {
        ui_toast("扫描不可用");
        return;
    }
    s.state = WF_STATE_SCANNING;
    s.ap_count = 0;
    wf_action_button(NULL, false);
    wf_status_set("正在扫描...", UI_COLOR_TEXT_DIM);
    wf_spinner_show(true, "正在扫描...");
}

static void wf_on_disconnect(lv_event_t *event)
{
    (void)event;
    if (!s.active || s.state != WF_STATE_CONNECTED) {
        return;
    }
    svc_wifi_disconnect();
    s.state = WF_STATE_IDLE;
    s.ip[0] = '\0';
    wf_status_set("已断开", UI_COLOR_TEXT_DIM);
    wf_action_button("扫描", true);
    ui_status_set_wifi(0);
}

static void wf_on_action(lv_event_t *event)
{
    if (s.state == WF_STATE_CONNECTED) {
        wf_on_disconnect(event);
    } else {
        wf_on_scan(event);
    }
}

static void wf_on_delete(lv_event_t *event)
{
    (void)event;
    s.active = false;
    /* Stop the service operations this app started. */
    if (s.state == WF_STATE_CONNECTING || s.state == WF_STATE_CONNECTED) {
        svc_wifi_disconnect();
    }
    ui_status_set_wifi(0);
}

/* ---------------- create ---------------- */

lv_obj_t *app_wifi_create(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.state = WF_STATE_IDLE;

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("WiFi", &content);
    s.root = root;
    lv_obj_add_event_cb(root, wf_on_delete, LV_EVENT_DELETE, NULL);

    s.lbl_status = lv_label_create(content);
    lv_label_set_text(s.lbl_status, "未连接");
    lv_obj_set_pos(s.lbl_status, 8, 6);
    lv_obj_set_width(s.lbl_status, 300);
    lv_label_set_long_mode(s.lbl_status, LV_LABEL_LONG_DOT);

    s.btn_action = lv_button_create(content);
    lv_obj_set_size(s.btn_action, 120, 44);
    lv_obj_align(s.btn_action, LV_ALIGN_TOP_RIGHT, -4, 0);
    lv_obj_set_style_bg_color(s.btn_action, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_radius(s.btn_action, 12, 0);
    lv_obj_add_event_cb(s.btn_action, wf_on_action, LV_EVENT_CLICKED, NULL);
    s.lbl_action = lv_label_create(s.btn_action);
    lv_label_set_text(s.lbl_action, "扫描");
    lv_obj_center(s.lbl_action);

    s.list = lv_obj_create(content);
    lv_obj_set_size(s.list, 452, 324);
    lv_obj_set_pos(s.list, 0, 56);
    lv_obj_set_style_bg_opa(s.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.list, 0, 0);
    lv_obj_set_style_pad_all(s.list, 0, 0);
    lv_obj_set_style_pad_row(s.list, 8, 0);
    lv_obj_set_layout(s.list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s.list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s.list, LV_OBJ_FLAG_SCROLL_ONE);

    s.spinner = lv_spinner_create(content);
    lv_obj_set_size(s.spinner, 64, 64);
    lv_obj_align(s.spinner, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(s.spinner, LV_OBJ_FLAG_HIDDEN);

    s.lbl_connecting = lv_label_create(content);
    lv_label_set_text(s.lbl_connecting, "");
    lv_obj_align(s.lbl_connecting, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_text_color(s.lbl_connecting,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_add_flag(s.lbl_connecting, LV_OBJ_FLAG_HIDDEN);

    /* Restore view if a previous session left the link up. */
    char ip[16];
    if (svc_wifi_is_connected(ip, sizeof(ip))) {
        s.state = WF_STATE_CONNECTED;
        snprintf(s.ip, sizeof(s.ip), "%s", ip);
        lv_label_set_text_fmt(s.lbl_status, "已连接 IP %s", ip);
        lv_obj_set_style_text_color(s.lbl_status, lv_color_hex(UI_COLOR_OK), 0);
        wf_action_button("断开", true);
    } else {
        lv_obj_t *hint = lv_label_create(s.list);
        lv_label_set_text(hint, "点击右上角扫描网络");
        lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    }

    return root;
}
