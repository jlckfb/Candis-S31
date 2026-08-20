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
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#include "demo_apps.h"
#include "services/svc_net.h"
#include "ui/ui_manager.h"

#define APP_MAX_ROWS 30
#define APP_MAX_SVCS 16

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
    ble_row_t rows[APP_MAX_ROWS];
    int row_count;
    uint8_t sel_addr[6];
    uint8_t sel_addr_type;
    char sel_name[40];
} ble_app_t;

static ble_app_t s;

/* ---------------- async payloads ---------------- */

typedef struct {
    svc_ble_event_t ev;
    int count;
    svc_ble_svc_t svcs[APP_MAX_SVCS];
} ble_conn_result_t;

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
    if (svc_ble_connect(row->addr, row->addr_type, ble_conn_cb, NULL) !=
            ESP_OK) {
        ui_toast("无法发起连接");
        ble_status_set("连接失败", UI_COLOR_ERR);
        return;
    }
    s.connecting = true;
    ble_status_set("正在连接...", UI_COLOR_WARN);
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
        if (svc_ble_scan_start(ble_dev_cb, NULL) != ESP_OK) {
            ui_toast("蓝牙暂不可用");
            return;
        }
        s.scanning = true;
        s.row_count = 0;
        lv_obj_clean(s.list);
        memset(s.rows, 0, sizeof(s.rows));
        lv_label_set_text(s.lbl_scan, "停止扫描");
        ble_status_set("正在扫描...", UI_COLOR_TEXT_DIM);
        ui_status_set_ble(true);
    } else {
        svc_ble_scan_stop();
        s.scanning = false;
        lv_label_set_text(s.lbl_scan, "开始扫描");
        char status[32];
        snprintf(status, sizeof(status), "发现 %d 台设备", s.row_count);
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
    ble_status_set("正在断开...", UI_COLOR_TEXT_DIM);
}

static void ble_on_delete(lv_event_t *event)
{
    (void)event;
    s.active = false;
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
    svc_ble_dev_t *dev = arg;
    if (!s.active) {
        heap_caps_free(dev);
        return;
    }
    ble_row_t *row = NULL;
    for (int i = 0; i < s.row_count; ++i) {
        if (memcmp(s.rows[i].addr, dev->addr, 6) == 0) {
            row = &s.rows[i];
            break;
        }
    }
    if (row == NULL) {
        if (s.row_count >= APP_MAX_ROWS) {
            heap_caps_free(dev);
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
        lv_label_set_text(lbl_name, dev->name[0] ? dev->name : "(未命名)");
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
    snprintf(status, sizeof(status), "发现 %d 台设备", s.row_count);
    ble_status_set(status, UI_COLOR_TEXT);
    heap_caps_free(dev);
}

static void ble_apply_conn(void *arg)
{
    ble_conn_result_t *res = arg;
    if (!s.active) {
        heap_caps_free(res);
        return;
    }
    switch (res->ev) {
    case SVC_BLE_EV_CONNECTED: {
        s.connecting = false;
        s.connected = true;
        ble_show_page(true);
        char mac[20];
        ble_mac_str(s.sel_addr, mac, sizeof(mac));
        if (s.lbl_peer != NULL) {
            lv_label_set_text_fmt(s.lbl_peer, "%s\n%s",
                                  s.sel_name[0] ? s.sel_name : "(未命名)", mac);
        }
        lv_obj_clean(s.svc_list);
        lv_obj_set_flag(s.lbl_svc_hint, LV_OBJ_FLAG_HIDDEN, false);
        lv_label_set_text(s.lbl_svc_hint, "正在获取服务...");
        ble_status_set("已连接", UI_COLOR_OK);
        break;
    }
    case SVC_BLE_EV_SERVICES_DONE:
        lv_obj_clean(s.svc_list);
        if (res->count == 0) {
            lv_obj_set_flag(s.lbl_svc_hint, LV_OBJ_FLAG_HIDDEN, false);
            lv_label_set_text(s.lbl_svc_hint, "未发现服务");
        } else {
            lv_obj_set_flag(s.lbl_svc_hint, LV_OBJ_FLAG_HIDDEN, true);
            for (int i = 0; i < res->count; ++i) {
                lv_obj_t *row = lv_obj_create(s.svc_list);
                lv_obj_set_size(row, LV_PCT(100), 44);
                lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE), 0);
                lv_obj_set_style_radius(row, 10, 0);
                lv_obj_set_style_border_width(row, 0, 0);
                lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE |
                                         LV_OBJ_FLAG_CLICKABLE);
                lv_obj_t *lbl_uuid = lv_label_create(row);
                lv_label_set_text(lbl_uuid, res->svcs[i].uuid);
                lv_obj_align(lbl_uuid, LV_ALIGN_LEFT_MID, 10, 0);
                lv_obj_t *lbl_handle = lv_label_create(row);
                lv_label_set_text_fmt(lbl_handle, "0x%04X-0x%04X",
                                      res->svcs[i].start_handle,
                                      res->svcs[i].end_handle);
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
        ble_status_set("连接失败", UI_COLOR_ERR);
        ui_toast("连接失败");
        break;
    case SVC_BLE_EV_DISCONNECTED:
    default:
        s.connecting = false;
        s.connected = false;
        ble_show_page(false);
        ble_status_set("已断开", UI_COLOR_TEXT_DIM);
        ui_status_set_ble(s.scanning);
        break;
    }
    heap_caps_free(res);
}

/* ---------------- service callbacks (network task) ---------------- */

static void ble_dev_cb(const svc_ble_dev_t *dev, void *user)
{
    (void)user;
    if (!s.active || dev == NULL) {
        return;
    }
    svc_ble_dev_t *copy = ble_alloc(sizeof(*copy));
    if (copy == NULL) {
        return;
    }
    memcpy(copy, dev, sizeof(*copy));
    ui_async(ble_apply_dev, copy);
}

static void ble_conn_cb(svc_ble_event_t ev, const svc_ble_svc_t *svcs,
                        int svc_count, void *user)
{
    (void)user;
    if (!s.active) {
        return;
    }
    ble_conn_result_t *res = ble_alloc(sizeof(*res));
    if (res == NULL) {
        return;
    }
    memset(res, 0, sizeof(*res));
    res->ev = ev;
    if (svcs != NULL && svc_count > 0) {
        res->count = svc_count > APP_MAX_SVCS ? APP_MAX_SVCS : svc_count;
        memcpy(res->svcs, svcs, sizeof(svcs[0]) * res->count);
    }
    ui_async(ble_apply_conn, res);
}

/* ---------------- create ---------------- */

lv_obj_t *app_ble_create(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("蓝牙", &content);
    s.root = root;
    lv_obj_add_event_cb(root, ble_on_delete, LV_EVENT_DELETE, NULL);

    s.lbl_status = lv_label_create(content);
    lv_label_set_text(s.lbl_status, "未扫描");
    lv_obj_set_pos(s.lbl_status, 8, 6);
    lv_obj_set_width(s.lbl_status, 280);
    lv_label_set_long_mode(s.lbl_status, LV_LABEL_LONG_DOT);

    s.btn_scan = lv_button_create(content);
    lv_obj_set_size(s.btn_scan, 140, 44);
    lv_obj_align(s.btn_scan, LV_ALIGN_TOP_RIGHT, -4, 0);
    lv_obj_set_style_bg_color(s.btn_scan, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_radius(s.btn_scan, 12, 0);
    lv_obj_add_event_cb(s.btn_scan, ble_on_scan_toggle, LV_EVENT_CLICKED, NULL);
    s.lbl_scan = lv_label_create(s.btn_scan);
    lv_label_set_text(s.lbl_scan, "开始扫描");
    lv_obj_center(s.lbl_scan);

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

    /* Connected page: peer info + GATT service list + disconnect button. */
    s.page_conn = lv_obj_create(content);
    lv_obj_set_size(s.page_conn, 452, 324);
    lv_obj_set_pos(s.page_conn, 0, 56);
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
    lv_obj_set_size(btn_disc, 120, 40);
    lv_obj_align(btn_disc, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_disc, lv_color_hex(UI_COLOR_ERR), 0);
    lv_obj_set_style_radius(btn_disc, 12, 0);
    lv_obj_add_event_cb(btn_disc, ble_on_disconnect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_disc = lv_label_create(btn_disc);
    lv_label_set_text(lbl_disc, "断开");
    lv_obj_center(lbl_disc);

    s.lbl_svc_hint = lv_label_create(s.page_conn);
    lv_label_set_text(s.lbl_svc_hint, "");
    lv_obj_set_pos(s.lbl_svc_hint, 4, 48);
    lv_obj_set_style_text_color(s.lbl_svc_hint,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s.svc_list = lv_obj_create(s.page_conn);
    lv_obj_set_size(s.svc_list, 444, 258);
    lv_obj_set_pos(s.svc_list, 0, 66);
    lv_obj_set_style_bg_opa(s.svc_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.svc_list, 0, 0);
    lv_obj_set_style_pad_all(s.svc_list, 0, 0);
    lv_obj_set_style_pad_row(s.svc_list, 6, 0);
    lv_obj_set_layout(s.svc_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s.svc_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s.svc_list, LV_SCROLLBAR_MODE_AUTO);

    return root;
}
