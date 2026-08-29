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

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

#include "demo_apps.h"
#include "services/svc_net.h"
#include "ui/ui_keyboard.h"
#include "ui/ui_manager.h"

#define APP_MAX_APS 20
#define WF_FALLBACK_POLL_MS 100

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
    lv_timer_t *fallback_timer;
    uint32_t session;
    uint32_t last_conn_seq;
    char ssid[33];      /* AP selected for connection */
    char ip[16];
    int ap_count;
    svc_wifi_ap_t aps[APP_MAX_APS];
    char saved_ssid[33];   /* NVS credentials loaded at create */
    char saved_pass[65];
} wf_app_t;

static wf_app_t s;

/* ---------------- async payloads ---------------- */

typedef struct {
    uint32_t session;
    int count;                    /* -1 = scan failed */
    svc_wifi_ap_t aps[APP_MAX_APS];
} wf_scan_result_t;

typedef struct {
    uint32_t session;
    uint32_t seq;
    svc_wifi_event_t ev;
    char detail[48];
} wf_conn_result_t;

typedef enum {
    WF_FALLBACK_NONE = 0,
    WF_FALLBACK_SCAN_BUSY,
    WF_FALLBACK_CONNECTED,
    WF_FALLBACK_CONNECT_FAILED,
    WF_FALLBACK_DISCONNECTED,
} wf_fallback_event_t;

static atomic_uint s_session_seq;
static atomic_uint s_live_session;
static atomic_uint s_conn_seq;
static portMUX_TYPE s_fallback_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t session;
    uint32_t seq;
    wf_fallback_event_t event;
} s_fallback;

static bool wf_session_is_live(uint32_t session)
{
    return session != 0 &&
           atomic_load_explicit(&s_live_session, memory_order_acquire) ==
               session;
}

static void wf_fallback_post(uint32_t session, uint32_t seq,
                             wf_fallback_event_t event)
{
    if (!wf_session_is_live(session)) {
        return;
    }
    portENTER_CRITICAL(&s_fallback_lock);
    if (wf_session_is_live(session)) {
        s_fallback.session = session;
        s_fallback.seq = seq;
        s_fallback.event = event;
    }
    portEXIT_CRITICAL(&s_fallback_lock);
}

static wf_fallback_event_t wf_fallback_take(uint32_t *session, uint32_t *seq)
{
    portENTER_CRITICAL(&s_fallback_lock);
    const wf_fallback_event_t event = s_fallback.event;
    *session = s_fallback.session;
    *seq = s_fallback.seq;
    s_fallback.event = WF_FALLBACK_NONE;
    s_fallback.session = 0;
    s_fallback.seq = 0;
    portEXIT_CRITICAL(&s_fallback_lock);
    return event;
}

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

static void wf_row_clicked(lv_event_t *event);
static void wf_saved_row_cb(lv_event_t *event);

static void wf_rebuild_list(void)
{
    if (s.list == NULL) {
        return;
    }
    lv_obj_clean(s.list);
    /* Saved-network direct reconnect stays at the top of every scan
     * result, so the one-tap path survives rescans. */
    if (s.saved_ssid[0] != '\0') {
        lv_obj_t *row = lv_button_create(s.list);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_SURFACE), 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(UI_COL_ACCENT), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_add_event_cb(row, wf_saved_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text_fmt(lbl, "Saved: %s", s.saved_ssid);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, 300);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t *go = lv_label_create(row);
        lv_label_set_text(go, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(go, lv_color_hex(UI_COL_ACCENT), 0);
        lv_obj_align(go, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    if (s.ap_count == 0) {
        lv_obj_t *empty = lv_label_create(s.list);
        lv_label_set_text(empty, "No networks found");
        lv_obj_set_style_text_color(empty, lv_color_hex(UI_COL_TEXT_DIM), 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 40, 0);
        return;
    }
    for (int i = 0; i < s.ap_count; ++i) {
        const svc_wifi_ap_t *ap = &s.aps[i];
        lv_obj_t *row = lv_button_create(s.list);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_SURFACE), 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_style_pad_right(row, 12, 0);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, wf_row_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl_ssid = lv_label_create(row);
        lv_label_set_text(lbl_ssid, ap->ssid[0] ? ap->ssid : "(hidden)");
        lv_label_set_long_mode(lbl_ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_ssid, 200);

        if (ap->authmode != 0 /* WIFI_AUTH_OPEN */) {
            lv_obj_t *lock = lv_label_create(row);
            lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE);
            lv_obj_set_style_text_color(lock, lv_color_hex(UI_COL_TEXT_DIM), 0);
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
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_ACCENT),
                                  LV_PART_INDICATOR);

        lv_obj_t *lbl_dbm = lv_label_create(row);
        lv_label_set_text_fmt(lbl_dbm, "%d", ap->rssi);
        lv_obj_set_style_text_color(lbl_dbm, lv_color_hex(UI_COL_TEXT_DIM), 0);
        lv_obj_set_width(lbl_dbm, 48);
        lv_obj_set_style_text_align(lbl_dbm, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* ---------------- landing functions (LVGL thread) ---------------- */

static void wf_apply_scan(void *arg)
{
    wf_scan_result_t *res = arg;
    if (s.active && res->session == s.session &&
            s.state == WF_STATE_SCANNING) {
        s.state = WF_STATE_IDLE;
        if (res->count < 0) {
            wf_status_set("Scan failed", UI_COL_FAIL);
        } else {
            s.ap_count = res->count;
            memcpy(s.aps, res->aps, sizeof(s.aps[0]) * res->count);
            wf_status_set(res->count ? "Select network" : "No networks found",
                          UI_COL_TEXT);
            wf_rebuild_list();
        }
        wf_spinner_show(false, NULL);
        wf_action_button("Scan", true);
    }
    heap_caps_free(res);
}

static void wf_apply_conn_event(uint32_t seq, svc_wifi_event_t event,
                                const char *detail)
{
    if (seq <= s.last_conn_seq) {
        return;
    }
    s.last_conn_seq = seq;
    switch (event) {
    case SVC_WIFI_EV_CONNECTED:
        s.state = WF_STATE_CONNECTED;
        snprintf(s.ip, sizeof(s.ip), "%.15s", detail ? detail : "");
        if (s.ip[0] == '\0') {
            svc_wifi_is_connected(s.ip, sizeof(s.ip));
        }
        wf_spinner_show(false, NULL);
        wf_status_set("Connected", UI_COL_PASS);
        if (s.lbl_status != NULL) {
            lv_label_set_text_fmt(s.lbl_status, "Connected, IP %s",
                                  s.ip[0] ? s.ip : "Fetching");
        }
        wf_action_button("Disconnect", true);
        ui_status_set_wifi(2);
        break;
    case SVC_WIFI_EV_CONNECT_FAILED:
        s.state = WF_STATE_IDLE;
        wf_spinner_show(false, NULL);
        wf_status_set("Connect failed", UI_COL_FAIL);
        wf_action_button("Scan", true);
        ui_status_set_wifi(0);
        ui_msgbox("Connect failed", detail && detail[0] ? detail : "Retry",
                  NULL, NULL);
        break;
    case SVC_WIFI_EV_DISCONNECTED:
    default:
        s.state = WF_STATE_IDLE;
        s.ip[0] = '\0';
        wf_spinner_show(false, NULL);
        wf_status_set("Disconnected", UI_COL_TEXT_DIM);
        wf_action_button("Scan", true);
        ui_status_set_wifi(0);
        break;
    }
}

static void wf_apply_conn(void *arg)
{
    wf_conn_result_t *res = arg;
    if (s.active && res->session == s.session) {
        wf_apply_conn_event(res->seq, res->ev, res->detail);
    }
    heap_caps_free(res);
}

static void wf_fallback_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t session = 0;
    uint32_t seq = 0;
    const wf_fallback_event_t event = wf_fallback_take(&session, &seq);
    if (!s.active) {
        return;
    }
    if (event == WF_FALLBACK_SCAN_BUSY && session == s.session) {
        if (s.state == WF_STATE_SCANNING) {
            s.state = WF_STATE_IDLE;
            wf_spinner_show(false, NULL);
            wf_status_set("Busy, retry after disconnect", UI_COL_WARN);
            wf_action_button("Scan", true);
        }
    } else if (event != WF_FALLBACK_NONE && session == s.session) {
        const svc_wifi_event_t wifi_event =
            event == WF_FALLBACK_CONNECTED ? SVC_WIFI_EV_CONNECTED :
            event == WF_FALLBACK_CONNECT_FAILED ? SVC_WIFI_EV_CONNECT_FAILED :
                                                  SVC_WIFI_EV_DISCONNECTED;
        wf_apply_conn_event(seq, wifi_event,
                            wifi_event == SVC_WIFI_EV_CONNECT_FAILED ?
                                "Event queue busy, retry" : NULL);
    }

    /* A disconnect requested while leaving the previous page can complete
     * after a new page restores the old connected snapshot. CONNECTING is
     * covered too: when the CONNECTED/FAILED event is lost (queue drop,
     * host reset mid-flight) the page would otherwise spin "Connecting"
     * forever - the svc-layer state is the ground truth here. */
    if (s.state == WF_STATE_IDLE || s.state == WF_STATE_CONNECTED ||
            s.state == WF_STATE_CONNECTING) {
        char ip[16] = { 0 };
        const bool connected = svc_wifi_is_connected(ip, sizeof(ip));
        if (connected && s.state != WF_STATE_CONNECTED) {
            s.state = WF_STATE_CONNECTED;
            snprintf(s.ip, sizeof(s.ip), "%s", ip);
            wf_spinner_show(false, NULL);
            wf_status_set("Connected", UI_COL_PASS);
            lv_label_set_text_fmt(s.lbl_status, "Connected, IP %s",
                                  s.ip[0] ? s.ip : "Fetching");
            wf_action_button("Disconnect", true);
        } else if (!connected && s.state == WF_STATE_CONNECTED) {
            s.state = WF_STATE_IDLE;
            s.ip[0] = '\0';
            wf_status_set("Disconnected", UI_COL_TEXT_DIM);
            wf_action_button("Scan", true);
        } else if (!connected && s.state == WF_STATE_CONNECTING &&
                   !svc_wifi_is_connecting()) {
            /* The service is no longer attempting a connect either: the
             * outcome event was lost, converge back to IDLE. */
            s.state = WF_STATE_IDLE;
            wf_spinner_show(false, NULL);
            wf_status_set("Connect failed", UI_COL_FAIL);
            wf_action_button("Scan", true);
        }
        ui_status_set_wifi(connected ? 2 : 0);
    }
}

/* ---------------- service callbacks (network task) ---------------- */

static void wf_scan_cb(const svc_wifi_ap_t *aps, int count, void *user)
{
    const uint32_t session = (uint32_t)(uintptr_t)user;
    if (!wf_session_is_live(session)) {
        return;
    }
    wf_scan_result_t *res = wf_alloc(sizeof(*res));
    if (res == NULL) {
        wf_fallback_post(session, 0, WF_FALLBACK_SCAN_BUSY);
        return;
    }
    res->session = session;
    if (aps == NULL) {
        res->count = -1;
    } else {
        res->count = count < 0 ? 0 :
                     count > APP_MAX_APS ? APP_MAX_APS : count;
        memcpy(res->aps, aps, sizeof(aps[0]) * res->count);
    }
    if (!ui_async(wf_apply_scan, res)) {
        heap_caps_free(res);
        wf_fallback_post(session, 0, WF_FALLBACK_SCAN_BUSY);
    }
}

static void wf_conn_cb(svc_wifi_event_t ev, const char *detail, void *user)
{
    const uint32_t session = (uint32_t)(uintptr_t)user;
    if (!wf_session_is_live(session)) {
        return;
    }
    wf_conn_result_t *res = wf_alloc(sizeof(*res));
    const uint32_t seq = atomic_fetch_add_explicit(
                             &s_conn_seq, 1, memory_order_relaxed) + 1;
    if (res == NULL) {
        wf_fallback_post(session, seq,
                         ev == SVC_WIFI_EV_CONNECTED ? WF_FALLBACK_CONNECTED :
                         ev == SVC_WIFI_EV_CONNECT_FAILED ?
                             WF_FALLBACK_CONNECT_FAILED :
                             WF_FALLBACK_DISCONNECTED);
        return;
    }
    res->session = session;
    res->seq = seq;
    res->ev = ev;
    snprintf(res->detail, sizeof(res->detail), "%s", detail ? detail : "");
    if (!ui_async(wf_apply_conn, res)) {
        heap_caps_free(res);
        wf_fallback_post(session, seq,
                         ev == SVC_WIFI_EV_CONNECTED ? WF_FALLBACK_CONNECTED :
                         ev == SVC_WIFI_EV_CONNECT_FAILED ?
                             WF_FALLBACK_CONNECT_FAILED :
                             WF_FALLBACK_DISCONNECTED);
    }
}

/* ---------------- user actions (LVGL context) ---------------- */

static void wf_start_connect(void)
{
    if (s.ssid[0] == '\0') {
        return;
    }
    s.state = WF_STATE_CONNECTING;
    char line[48];
    snprintf(line, sizeof(line), "Connecting %.28s...", s.ssid);
    wf_spinner_show(true, line);
    wf_status_set("Connecting", UI_COL_WARN);
    wf_action_button(NULL, false);
    ui_status_set_wifi(1);
}

static void wf_try_connect(const char *password)
{
    if (svc_wifi_connect(s.ssid, password, wf_conn_cb,
                         (void *)(uintptr_t)s.session) != ESP_OK) {
        s.state = WF_STATE_IDLE;
        wf_spinner_show(false, NULL);
        wf_status_set("Connect failed", UI_COL_FAIL);
        wf_action_button("Scan", true);
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

/* One-tap reconnect with the credentials persisted by the service
 * after the last successful connection. */
static void wf_saved_row_cb(lv_event_t *event)
{
    (void)event;
    if (!s.active || s.state != WF_STATE_IDLE || s.saved_ssid[0] == '\0') {
        return;
    }
    snprintf(s.ssid, sizeof(s.ssid), "%s", s.saved_ssid);
    wf_try_connect(s.saved_pass);
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
    if (svc_wifi_scan(wf_scan_cb, (void *)(uintptr_t)s.session) != ESP_OK) {
        ui_toast("Scan unavailable");
        return;
    }
    s.state = WF_STATE_SCANNING;
    s.ap_count = 0;
    wf_action_button(NULL, false);
    wf_status_set("Scanning...", UI_COL_TEXT_DIM);
    wf_spinner_show(true, "Scanning...");
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
    wf_status_set("Disconnected", UI_COL_TEXT_DIM);
    wf_action_button("Scan", true);
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
    if (s.fallback_timer != NULL) {
        lv_timer_delete(s.fallback_timer);
        s.fallback_timer = NULL;
    }
    unsigned int expected = s.session;
    atomic_compare_exchange_strong_explicit(
        &s_live_session, &expected, 0, memory_order_acq_rel,
        memory_order_acquire);
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
    s.session = atomic_fetch_add_explicit(&s_session_seq, 1,
                                          memory_order_relaxed) + 1;
    portENTER_CRITICAL(&s_fallback_lock);
    s_fallback = (typeof(s_fallback)){ 0 };
    portEXIT_CRITICAL(&s_fallback_lock);
    atomic_store_explicit(&s_live_session, s.session, memory_order_release);
    atomic_store_explicit(&s_conn_seq, 0, memory_order_relaxed);

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("WiFi", &content);
    s.root = root;
    lv_obj_add_event_cb(root, wf_on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_text_font(content, ui_font_body(), 0);
    s.fallback_timer = lv_timer_create(wf_fallback_timer_cb,
                                       WF_FALLBACK_POLL_MS, NULL);

    s.lbl_status = lv_label_create(content);
    lv_label_set_text(s.lbl_status, "Not connected");
    lv_obj_set_pos(s.lbl_status, 8, 6);
    lv_obj_set_width(s.lbl_status, 280);
    lv_label_set_long_mode(s.lbl_status, LV_LABEL_LONG_DOT);

    s.btn_action = lv_button_create(content);
    lv_obj_set_size(s.btn_action, 132, UI_TOUCH_MIN);
    lv_obj_align(s.btn_action, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s.btn_action, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_radius(s.btn_action, 12, 0);
    lv_obj_add_event_cb(s.btn_action, wf_on_action, LV_EVENT_CLICKED, NULL);
    s.lbl_action = lv_label_create(s.btn_action);
    lv_label_set_text(s.lbl_action, "Scan");
    lv_obj_center(s.lbl_action);

    s.list = lv_obj_create(content);
    lv_obj_set_size(s.list, LV_PCT(100), 272);
    lv_obj_set_pos(s.list, 0, 68);
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
                                lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_add_flag(s.lbl_connecting, LV_OBJ_FLAG_HIDDEN);

    /* Saved credentials are loaded up front so the direct-reconnect row
     * can also reappear on scan results after a mid-session disconnect. */
    svc_net_wifi_saved(s.saved_ssid, sizeof(s.saved_ssid),
                       s.saved_pass, sizeof(s.saved_pass));

    /* Restore view if a previous session left the link up. */
    char ip[16];
    if (svc_wifi_is_connected(ip, sizeof(ip))) {
        s.state = WF_STATE_CONNECTED;
        snprintf(s.ip, sizeof(s.ip), "%s", ip);
        lv_label_set_text_fmt(s.lbl_status, "Connected, IP %s", ip);
        lv_obj_set_style_text_color(s.lbl_status, lv_color_hex(UI_COL_PASS), 0);
        wf_action_button("Disconnect", true);
        ui_status_set_wifi(2);
    } else if (s.saved_ssid[0] != '\0') {
        /* Saved credentials: one-tap reconnect instead of scan+keyboard. */
        lv_obj_t *row = lv_button_create(s.list);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_SURFACE), 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(UI_COL_ACCENT), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_add_event_cb(row, wf_saved_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text_fmt(lbl, "Saved: %s", s.saved_ssid);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, 300);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t *go = lv_label_create(row);
        lv_label_set_text(go, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(go, lv_color_hex(UI_COL_ACCENT), 0);
        lv_obj_align(go, LV_ALIGN_RIGHT_MID, -12, 0);

        lv_obj_t *hint = lv_label_create(s.list);
        lv_label_set_text(hint, "Tap top-right to scan for others");
        lv_obj_set_style_text_color(hint, lv_color_hex(UI_COL_TEXT_DIM), 0);
    } else {
        lv_obj_t *hint = lv_label_create(s.list);
        lv_label_set_text(hint, "Tap top-right to scan");
        lv_obj_set_style_text_color(hint, lv_color_hex(UI_COL_TEXT_DIM), 0);
    }

    return root;
}
