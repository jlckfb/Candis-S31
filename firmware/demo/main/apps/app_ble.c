/*
 * Candis-S31 watch demo - BLE app: scan (live RSSI-sorted list), connect,
 * GATT service list, disconnect.
 *
 * All svc_net callbacks run on the network task; UI updates are forwarded
 * with ui_async() using heap-allocated payloads. The root screen's
 * LV_EVENT_DELETE stops the scan and drops any connection this app opened.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

#include "demo_apps.h"
#include "services/svc_net.h"
#include "ui/ui_manager.h"

#define APP_MAX_ROWS 30
#define APP_MAX_SVCS 16
#define BLE_FALLBACK_POLL_MS 100

typedef struct {
    bool used;
    uint8_t addr[6];
    uint8_t addr_type;
    int rssi;
    char name[32];
    lv_obj_t *btn;
    lv_obj_t *lbl_rssi;
} ble_row_t;

typedef struct {
    bool active;
    bool scanning;
    bool connecting;
    bool connected;
    lv_obj_t *root;
    lv_obj_t *lbl_status;
    lv_obj_t *btn_scan;
    lv_obj_t *lbl_scan;
    lv_obj_t *list;
    lv_obj_t *page_conn;
    lv_obj_t *lbl_peer;
    lv_obj_t *lbl_svc_hint;
    lv_obj_t *svc_list;
    lv_timer_t *fallback_timer;
    uint32_t session;
    uint32_t last_conn_seq;
    ble_row_t rows[APP_MAX_ROWS];
    int row_count;
    uint8_t sel_addr[6];
    uint8_t sel_addr_type;
    char sel_name[40];
} ble_app_t;

static ble_app_t s;

/* ---------------- async payloads ---------------- */

typedef struct {
    uint32_t session;
    uint32_t seq;
    svc_ble_event_t ev;
    int count;
    svc_ble_svc_t svcs[APP_MAX_SVCS];
} ble_conn_result_t;

typedef struct {
    uint32_t session;
    svc_ble_dev_t dev;
} ble_dev_result_t;

typedef enum {
    BLE_FALLBACK_NONE = 0,
    BLE_FALLBACK_CONNECTED,
    BLE_FALLBACK_SERVICES_BUSY,
    BLE_FALLBACK_CONNECT_FAILED,
    BLE_FALLBACK_DISCONNECTED,
} ble_fallback_event_t;

static atomic_uint s_session_seq;
static atomic_uint s_live_session;
static atomic_uint s_conn_seq;
static portMUX_TYPE s_fallback_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t session;
    uint32_t seq;
    ble_fallback_event_t event;
} s_fallback;

static bool ble_session_is_live(uint32_t session)
{
    return session != 0 &&
           atomic_load_explicit(&s_live_session, memory_order_acquire) ==
               session;
}

static void ble_fallback_post(uint32_t session, uint32_t seq,
                              ble_fallback_event_t event)
{
    if (!ble_session_is_live(session)) {
        return;
    }
    portENTER_CRITICAL(&s_fallback_lock);
    if (ble_session_is_live(session) && seq >= s_fallback.seq) {
        s_fallback.session = session;
        s_fallback.seq = seq;
        s_fallback.event = event;
    }
    portEXIT_CRITICAL(&s_fallback_lock);
}

static ble_fallback_event_t ble_fallback_take(uint32_t *session,
                                               uint32_t *seq)
{
    portENTER_CRITICAL(&s_fallback_lock);
    const ble_fallback_event_t event = s_fallback.event;
    *session = s_fallback.session;
    *seq = s_fallback.seq;
    s_fallback = (typeof(s_fallback)){ 0 };
    portEXIT_CRITICAL(&s_fallback_lock);
    return event;
}

static void *ble_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ---------------- helpers (LVGL context) ---------------- */

static void ble_status_set(const char *text, uint32_t color)
{
    if (!s.active || s.lbl_status == NULL) {
        return;
    }
    lv_label_set_text(s.lbl_status, text);
    lv_obj_set_style_text_color(s.lbl_status, lv_color_hex(color), 0);
}

static void ble_mac_str(const uint8_t addr[6], char *out, size_t cap)
{
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static void ble_show_page(bool connected_page)
{
    lv_obj_set_flag(s.page_conn, LV_OBJ_FLAG_HIDDEN, !connected_page);
    lv_obj_set_flag(s.list, LV_OBJ_FLAG_HIDDEN, connected_page);
    lv_obj_set_flag(s.btn_scan, LV_OBJ_FLAG_HIDDEN, connected_page);
}

static void ble_sort_rows(void)
{
    /* Insertion sort by RSSI, strongest first; n is tiny. */
    for (int i = 1; i < s.row_count; ++i) {
        ble_row_t key = s.rows[i];
        int j = i - 1;
        while (j >= 0 && s.rows[j].rssi < key.rssi) {
            s.rows[j + 1] = s.rows[j];
            --j;
        }
        s.rows[j + 1] = key;
    }
    for (int i = 0; i < s.row_count; ++i) {
        lv_obj_move_to_index(s.rows[i].btn, i);
        /* Keep the click-handler index in sync with the reordered rows. */
        lv_obj_set_user_data(s.rows[i].btn, (void *)(intptr_t)i);
    }
}

static void ble_show_connected_state(void)
{
    s.connecting = false;
    s.connected = true;
    ble_show_page(true);
    char mac[20];
    ble_mac_str(s.sel_addr, mac, sizeof(mac));
    if (s.lbl_peer != NULL) {
        lv_label_set_text_fmt(s.lbl_peer, "%s\n%s",
                              s.sel_name[0] ? s.sel_name : "(unnamed)", mac);
    }
    ble_status_set("Connected", UI_COLOR_OK);
    ui_status_set_ble(true);
}

/* ---------------- service callbacks (network task) ---------------- */

static void ble_dev_cb(const svc_ble_dev_t *dev, void *user);
static void ble_conn_cb(svc_ble_event_t ev, const svc_ble_svc_t *svcs,
                        int svc_count, void *user);

/* ---------------- user actions (LVGL context) ---------------- */

static void ble_do_connect(int idx)
{
    const ble_row_t *row = &s.rows[idx];
    memcpy(s.sel_addr, row->addr, 6);
    s.sel_addr_type = row->addr_type;
    snprintf(s.sel_name, sizeof(s.sel_name), "%s",
             row->name[0] ? row->name : "");

    if (s.scanning) {
        svc_ble_scan_stop();
        s.scanning = false;
        ui_status_set_ble(false);
    }
    if (svc_ble_connect(row->addr, row->addr_type, ble_conn_cb,
                        (void *)(uintptr_t)s.session) != ESP_OK) {
        ui_toast("Cannot start connection");
        ble_status_set("Connect failed", UI_COLOR_ERR);
        return;
    }
    s.connecting = true;
    ble_status_set("Connecting...", UI_COLOR_WARN);
}

static void ble_row_clicked(lv_event_t *event)
{
    lv_obj_t *btn = lv_event_get_target_obj(event);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (!s.active || s.connecting || s.connected || idx < 0 ||
            idx >= s.row_count) {
        return;
    }
    ble_do_connect(idx);
}

static void ble_on_scan_toggle(lv_event_t *event)
{
    (void)event;
    if (!s.active || s.connecting || s.connected) {
        return;
    }
    if (!s.scanning) {
        if (svc_ble_scan_start(ble_dev_cb,
                               (void *)(uintptr_t)s.session) != ESP_OK) {
            ui_toast("Bluetooth unavailable");
            return;
        }
        s.scanning = true;
        s.row_count = 0;
        lv_obj_clean(s.list);
        memset(s.rows, 0, sizeof(s.rows));
        lv_label_set_text(s.lbl_scan, "Stop scan");
        ble_status_set("Scanning...", UI_COLOR_TEXT_DIM);
        ui_status_set_ble(true);
    } else {
        svc_ble_scan_stop();
        s.scanning = false;
        lv_label_set_text(s.lbl_scan, "Start scan");
        char status[32];
        snprintf(status, sizeof(status), "%d devices found", s.row_count);
        ble_status_set(status, UI_COLOR_TEXT);
        ui_status_set_ble(false);
    }
}

static void ble_on_disconnect(lv_event_t *event)
{
    (void)event;
    if (!s.active || !s.connected) {
        return;
    }
    svc_ble_disconnect();
    ble_status_set("Disconnecting...", UI_COLOR_TEXT_DIM);
}

static void ble_on_delete(lv_event_t *event)
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
    if (s.scanning) {
        svc_ble_scan_stop();
    }
    if (s.connecting || s.connected) {
        svc_ble_disconnect();
    }
    ui_status_set_ble(false);
}

/* ---------------- landing functions (LVGL thread) ---------------- */

static void ble_apply_dev(void *arg)
{
    ble_dev_result_t *res = arg;
    if (!s.active || res->session != s.session) {
        heap_caps_free(res);
        return;
    }
    const svc_ble_dev_t *dev = &res->dev;
    ble_row_t *row = NULL;
    for (int i = 0; i < s.row_count; ++i) {
        if (memcmp(s.rows[i].addr, dev->addr, 6) == 0) {
            row = &s.rows[i];
            break;
        }
    }
    if (row == NULL) {
        if (s.row_count >= APP_MAX_ROWS) {
            heap_caps_free(res);
            return;
        }
        row = &s.rows[s.row_count];
        memset(row, 0, sizeof(*row));
        row->used = true;
        memcpy(row->addr, dev->addr, 6);
        row->addr_type = dev->addr_type;
        snprintf(row->name, sizeof(row->name), "%s", dev->name);

        row->btn = lv_button_create(s.list);
        lv_obj_set_size(row->btn, LV_PCT(100), 60);
        lv_obj_set_style_bg_color(row->btn, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_radius(row->btn, 12, 0);
        lv_obj_set_layout(row->btn, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row->btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row->btn, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(row->btn, 12, 0);
        lv_obj_set_style_pad_right(row->btn, 12, 0);
        lv_obj_set_user_data(row->btn, (void *)(intptr_t)s.row_count);
        lv_obj_add_event_cb(row->btn, ble_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s.row_count);

        lv_obj_t *col = lv_obj_create(row->btn);
        lv_obj_set_size(col, 290, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_layout(col, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);

        lv_obj_t *lbl_name = lv_label_create(col);
        lv_label_set_text(lbl_name, dev->name[0] ? dev->name : "(unnamed)");
        lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_name, 280);

        char mac[20];
        ble_mac_str(dev->addr, mac, sizeof(mac));
        lv_obj_t *lbl_mac = lv_label_create(col);
        lv_label_set_text(lbl_mac, mac);
        lv_obj_set_style_text_color(lbl_mac, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

        row->lbl_rssi = lv_label_create(row->btn);
        lv_obj_set_style_text_color(row->lbl_rssi,
                                    lv_color_hex(UI_COLOR_ACCENT), 0);

        ++s.row_count;
    }
    row->rssi = dev->rssi;
    lv_label_set_text_fmt(row->lbl_rssi, "%d dBm", dev->rssi);
    ble_sort_rows();
    char status[32];
    snprintf(status, sizeof(status), "%d devices found", s.row_count);
    ble_status_set(status, UI_COLOR_TEXT);
    heap_caps_free(res);
}

static void ble_apply_conn_event(uint32_t seq, svc_ble_event_t event,
                                 const svc_ble_svc_t *svcs, int count,
                                 bool payload_busy)
{
    if (seq <= s.last_conn_seq) {
        return;
    }
    s.last_conn_seq = seq;
    switch (event) {
    case SVC_BLE_EV_CONNECTED: {
        ble_show_connected_state();
        lv_obj_clean(s.svc_list);
        lv_obj_set_flag(s.lbl_svc_hint, LV_OBJ_FLAG_HIDDEN, false);
        lv_label_set_text(s.lbl_svc_hint, "Reading services...");
        break;
    }
    case SVC_BLE_EV_SERVICES_DONE:
        if (!s.connected) {
            ble_show_connected_state();
        }
        lv_obj_clean(s.svc_list);
        if (payload_busy) {
            lv_obj_set_flag(s.lbl_svc_hint, LV_OBJ_FLAG_HIDDEN, false);
            lv_label_set_text(s.lbl_svc_hint, "Busy, disconnect and retry");
        } else if (count == 0) {
            lv_obj_set_flag(s.lbl_svc_hint, LV_OBJ_FLAG_HIDDEN, false);
            lv_label_set_text(s.lbl_svc_hint, "No services found");
        } else {
            lv_obj_set_flag(s.lbl_svc_hint, LV_OBJ_FLAG_HIDDEN, true);
            for (int i = 0; i < count; ++i) {
                lv_obj_t *row = lv_obj_create(s.svc_list);
                lv_obj_set_size(row, LV_PCT(100), 52);
                lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE), 0);
                lv_obj_set_style_radius(row, 10, 0);
                lv_obj_set_style_border_width(row, 0, 0);
                lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE |
                                         LV_OBJ_FLAG_CLICKABLE);
                lv_obj_t *lbl_uuid = lv_label_create(row);
                lv_label_set_text(lbl_uuid, svcs[i].uuid);
                lv_obj_align(lbl_uuid, LV_ALIGN_LEFT_MID, 10, 0);
                lv_obj_t *lbl_handle = lv_label_create(row);
                lv_label_set_text_fmt(lbl_handle, "0x%04X-0x%04X",
                                      svcs[i].start_handle,
                                      svcs[i].end_handle);
                lv_obj_set_style_text_color(
                    lbl_handle, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
                lv_obj_align(lbl_handle, LV_ALIGN_RIGHT_MID, -10, 0);
            }
        }
        break;
    case SVC_BLE_EV_CONNECT_FAILED:
        s.connecting = false;
        s.connected = false;
        ble_show_page(false);
        ble_status_set("Connect failed", UI_COLOR_ERR);
        ui_toast("Connect failed");
        break;
    case SVC_BLE_EV_DISCONNECTED:
    default:
        s.connecting = false;
        s.connected = false;
        ble_show_page(false);
        ble_status_set("Disconnected", UI_COLOR_TEXT_DIM);
        ui_status_set_ble(s.scanning);
        break;
    }
}

static void ble_apply_conn(void *arg)
{
    ble_conn_result_t *res = arg;
    if (s.active && res->session == s.session) {
        ble_apply_conn_event(res->seq, res->ev, res->svcs, res->count, false);
    }
    heap_caps_free(res);
}

static void ble_fallback_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s.active) {
        return;
    }
    uint32_t session = 0;
    uint32_t seq = 0;
    const ble_fallback_event_t event = ble_fallback_take(&session, &seq);
    if (session == s.session && event != BLE_FALLBACK_NONE) {
        const svc_ble_event_t ble_event =
            event == BLE_FALLBACK_CONNECTED ? SVC_BLE_EV_CONNECTED :
            event == BLE_FALLBACK_SERVICES_BUSY ? SVC_BLE_EV_SERVICES_DONE :
            event == BLE_FALLBACK_CONNECT_FAILED ? SVC_BLE_EV_CONNECT_FAILED :
                                                    SVC_BLE_EV_DISCONNECTED;
        ble_apply_conn_event(seq, ble_event, NULL, 0,
                             event == BLE_FALLBACK_SERVICES_BUSY);
    }

    /* CONNECTING reconciliation, symmetric with the WiFi app: when the
     * CONNECTED/FAILED event is lost the page would otherwise spin
     * "Connecting" forever. The service state is the ground truth;
     * converge both directions (the peer name/address were captured at
     * connect kickoff, so a lost CONNECTED event can still restore the
     * connected view). */
    if (s.connecting && !svc_ble_is_connecting()) {
        if (svc_ble_is_connected()) {
            ble_show_connected_state();
        } else {
            s.connecting = false;
            s.connected = false;
            ble_show_page(false);
            ble_status_set("Connect failed", UI_COLOR_ERR);
        }
    }
}

/* ---------------- service callbacks (network task) ---------------- */

static void ble_dev_cb(const svc_ble_dev_t *dev, void *user)
{
    const uint32_t session = (uint32_t)(uintptr_t)user;
    if (!ble_session_is_live(session) || dev == NULL) {
        return;
    }
    ble_dev_result_t *res = ble_alloc(sizeof(*res));
    if (res == NULL) {
        return;
    }
    res->session = session;
    memcpy(&res->dev, dev, sizeof(res->dev));
    if (!ui_async(ble_apply_dev, res)) {
        heap_caps_free(res);
    }
}

static void ble_conn_cb(svc_ble_event_t ev, const svc_ble_svc_t *svcs,
                        int svc_count, void *user)
{
    const uint32_t session = (uint32_t)(uintptr_t)user;
    if (!ble_session_is_live(session)) {
        return;
    }
    const uint32_t seq = atomic_fetch_add_explicit(
                             &s_conn_seq, 1, memory_order_relaxed) + 1;
    ble_conn_result_t *res = ble_alloc(sizeof(*res));
    if (res == NULL) {
        ble_fallback_post(
            session, seq,
            ev == SVC_BLE_EV_CONNECTED ? BLE_FALLBACK_CONNECTED :
            ev == SVC_BLE_EV_SERVICES_DONE ? BLE_FALLBACK_SERVICES_BUSY :
            ev == SVC_BLE_EV_CONNECT_FAILED ? BLE_FALLBACK_CONNECT_FAILED :
                                              BLE_FALLBACK_DISCONNECTED);
        return;
    }
    memset(res, 0, sizeof(*res));
    res->session = session;
    res->seq = seq;
    res->ev = ev;
    if (svcs != NULL && svc_count > 0) {
        res->count = svc_count > APP_MAX_SVCS ? APP_MAX_SVCS : svc_count;
        memcpy(res->svcs, svcs, sizeof(svcs[0]) * res->count);
    }
    if (!ui_async(ble_apply_conn, res)) {
        heap_caps_free(res);
        ble_fallback_post(
            session, seq,
            ev == SVC_BLE_EV_CONNECTED ? BLE_FALLBACK_CONNECTED :
            ev == SVC_BLE_EV_SERVICES_DONE ? BLE_FALLBACK_SERVICES_BUSY :
            ev == SVC_BLE_EV_CONNECT_FAILED ? BLE_FALLBACK_CONNECT_FAILED :
                                              BLE_FALLBACK_DISCONNECTED);
    }
}

/* ---------------- create ---------------- */

lv_obj_t *app_ble_create(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.session = atomic_fetch_add_explicit(&s_session_seq, 1,
                                          memory_order_relaxed) + 1;
    portENTER_CRITICAL(&s_fallback_lock);
    s_fallback = (typeof(s_fallback)){ 0 };
    portEXIT_CRITICAL(&s_fallback_lock);
    atomic_store_explicit(&s_live_session, s.session, memory_order_release);
    atomic_store_explicit(&s_conn_seq, 0, memory_order_relaxed);

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("Bluetooth", &content);
    s.root = root;
    lv_obj_add_event_cb(root, ble_on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_text_font(content, ui_font_body(), 0);
    s.fallback_timer = lv_timer_create(ble_fallback_timer_cb,
                                       BLE_FALLBACK_POLL_MS, NULL);

    s.lbl_status = lv_label_create(content);
    lv_label_set_text(s.lbl_status, "Not scanned");
    lv_obj_set_pos(s.lbl_status, 8, 6);
    lv_obj_set_width(s.lbl_status, 280);
    lv_label_set_long_mode(s.lbl_status, LV_LABEL_LONG_DOT);

    s.btn_scan = lv_button_create(content);
    lv_obj_set_size(s.btn_scan, 140, UI_TOUCH_MIN);
    lv_obj_align(s.btn_scan, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s.btn_scan, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_radius(s.btn_scan, 12, 0);
    lv_obj_add_event_cb(s.btn_scan, ble_on_scan_toggle, LV_EVENT_CLICKED, NULL);
    s.lbl_scan = lv_label_create(s.btn_scan);
    lv_label_set_text(s.lbl_scan, "Start scan");
    lv_obj_center(s.lbl_scan);

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

    /* Connected page: peer info + GATT service list + disconnect button. */
    s.page_conn = lv_obj_create(content);
    lv_obj_set_size(s.page_conn, LV_PCT(100), 272);
    lv_obj_set_pos(s.page_conn, 0, 68);
    lv_obj_set_style_bg_opa(s.page_conn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.page_conn, 0, 0);
    lv_obj_set_style_pad_all(s.page_conn, 0, 0);
    lv_obj_add_flag(s.page_conn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s.page_conn, LV_OBJ_FLAG_SCROLLABLE);

    s.lbl_peer = lv_label_create(s.page_conn);
    lv_label_set_text(s.lbl_peer, "");
    lv_obj_set_pos(s.lbl_peer, 4, 0);
    lv_obj_set_width(s.lbl_peer, 300);

    lv_obj_t *btn_disc = lv_button_create(s.page_conn);
    lv_obj_set_size(btn_disc, 120, UI_TOUCH_MIN);
    lv_obj_align(btn_disc, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_disc, lv_color_hex(UI_COLOR_ERR), 0);
    lv_obj_set_style_radius(btn_disc, 12, 0);
    lv_obj_add_event_cb(btn_disc, ble_on_disconnect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_disc = lv_label_create(btn_disc);
    lv_label_set_text(lbl_disc, "Disconnect");
    lv_obj_center(lbl_disc);

    s.lbl_svc_hint = lv_label_create(s.page_conn);
    lv_label_set_text(s.lbl_svc_hint, "");
    lv_obj_set_pos(s.lbl_svc_hint, 4, 58);
    lv_obj_set_style_text_color(s.lbl_svc_hint,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s.svc_list = lv_obj_create(s.page_conn);
    lv_obj_set_size(s.svc_list, LV_PCT(100), 204);
    lv_obj_set_pos(s.svc_list, 0, 68);
    lv_obj_set_style_bg_opa(s.svc_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.svc_list, 0, 0);
    lv_obj_set_style_pad_all(s.svc_list, 0, 0);
    lv_obj_set_style_pad_row(s.svc_list, 6, 0);
    lv_obj_set_layout(s.svc_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s.svc_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s.svc_list, LV_SCROLLBAR_MODE_AUTO);

    return root;
}
