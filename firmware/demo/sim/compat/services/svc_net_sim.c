/*
 * Candis-S31 simulator - network service mock (WiFi STA + BLE central).
 *
 * Scripted stand-in for firmware/main/services/svc_net.c: no radios, no
 * NVS, just canned results delivered on a dedicated "svc_net" task so UI
 * code sees the same callback threading as on hardware. All public entry
 * points only stash requests under a mutex; the sim task polls every
 * 50 ms and fires whatever is due. This file never touches LVGL.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "services/svc_net.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "svc_net"

#define WIFI_SCAN_DELAY_MS     1500
#define WIFI_CONNECT_DELAY_MS  2500
#define BLE_SCAN_PERIOD_MS     500
#define BLE_CONNECT_DELAY_MS   1500
#define BLE_DISCOVER_DELAY_MS  1000
#define NET_TASK_POLL_MS       50

#define SIM_IP "192.168.43.15"

/* Canned scan results, already sorted by RSSI (best first). */
static const svc_wifi_ap_t s_fake_aps[] = {
    {"LEENIXP",        -40, 3, 6},
    {"任平生",          -48, 3, 11},
    {"Candis-Office",  -55, 4, 1},
    {"TP-Link_5G",     -61, 3, 36},
    {"Xiaomi_Home",    -67, 3, 3},
    {"咖啡图书馆",      -72, 0, 7},
    {"HUAWEI-9F3A",    -78, 4, 13},
    {"CMCC-Free",      -82, 0, 9},
    {"Starbucks_Guest", -86, 0, 4},
    {"Redmi-U26",      -90, 3, 10},
};
#define FAKE_AP_COUNT ((int)(sizeof(s_fake_aps) / sizeof(s_fake_aps[0])))

static const svc_ble_dev_t s_fake_ble_devs[] = {
    {"离线蓝牙音响小程序", {0x24, 0x6F, 0x28, 0xA1, 0xB2, 0x01}, 0, -45},
    {"MI Band 7",        {0xC4, 0x7C, 0x8D, 0x3E, 0x55, 0x02}, 1, -52},
    {"AirPods Pro",      {0xA8, 0x51, 0xAB, 0x0C, 0x77, 0x03}, 1, -58},
    {"Candis-U26",       {0x30, 0xAE, 0xA4, 0xF0, 0x12, 0x04}, 0, -63},
    {"HUAWEI WATCH",     {0xD8, 0x63, 0x75, 0x9A, 0x20, 0x05}, 0, -69},
    {"JBL GO 3",         {0xF0, 0x5E, 0xCD, 0x61, 0x8B, 0x06}, 0, -74},
    {"XM-Keyboard",      {0x5C, 0xF3, 0x70, 0x2D, 0x94, 0x07}, 0, -80},
    {"心率带 H7",        {0xE0, 0x98, 0x06, 0xB7, 0x3C, 0x08}, 0, -85},
};
#define FAKE_BLE_DEV_COUNT ((int)(sizeof(s_fake_ble_devs) / sizeof(s_fake_ble_devs[0])))

static const svc_ble_svc_t s_fake_gatt_svcs[] = {
    {"0x1800", 1, 5},
    {"0x1801", 6, 9},
    {"0xFFE0", 10, 14},
};
#define FAKE_GATT_SVC_COUNT ((int)(sizeof(s_fake_gatt_svcs) / sizeof(s_fake_gatt_svcs[0])))

/* One pending timed action on the sim task. */
typedef enum {
    EV_NONE = 0,
    EV_WIFI_SCAN_DONE,
    EV_WIFI_CONN_DONE,   /* CONNECTED or CONNECT_FAILED */
    EV_WIFI_DISCONNECTED,
    EV_BLE_CONN_DONE,    /* CONNECTED */
    EV_BLE_SVCS_DONE,
    EV_BLE_DISCONNECTED,
} sim_ev_kind_t;

typedef struct {
    sim_ev_kind_t kind;
    TickType_t due;      /* tick (ms) when the event fires */
    bool fail;           /* EV_WIFI_CONN_DONE: report CONNECT_FAILED */
    union {
        struct {
            svc_wifi_scan_cb_t cb;
            void *user;
        } scan;
        struct {
            svc_wifi_conn_cb_t cb;
            void *user;
        } conn;
        struct {
            svc_ble_conn_cb_t cb;
            void *user;
        } ble_conn;
    } u;
} sim_ev_t;

#define SIM_EV_MAX 4

static SemaphoreHandle_t s_lock;
static bool s_started;

static sim_ev_t s_events[SIM_EV_MAX];

/* WiFi link state. */
static bool s_wifi_connecting;
static bool s_wifi_connected;
static svc_wifi_conn_cb_t s_wifi_cb;   /* kept for DISCONNECTED reports */
static void *s_wifi_cb_user;

/* Saved credentials (fixed canned pair until "forgotten"). */
static bool s_saved_valid = true;

/* BLE state. */
static bool s_ble_scanning;
static svc_ble_scan_cb_t s_ble_scan_cb;
static void *s_ble_scan_user;
static int s_ble_scan_idx;             /* next fake device index */
static TickType_t s_ble_scan_next;     /* tick of next scan report */
static bool s_ble_scan_was_active;     /* remembered across suspend */
static bool s_ble_connecting;
static bool s_ble_connected;
static svc_ble_conn_cb_t s_ble_conn_cb;
static void *s_ble_conn_user;

static bool s_rf_suspended;

static TickType_t now_ticks(void)
{
    return xTaskGetTickCount();
}

/* Caller must hold s_lock. Returns NULL when no slot is free. */
static sim_ev_t *ev_alloc(void)
{
    for (int i = 0; i < SIM_EV_MAX; i++) {
        if (s_events[i].kind == EV_NONE) {
            return &s_events[i];
        }
    }
    return NULL;
}

/* Drop every pending event of the given kinds (suspend / disconnect path). */
static void ev_clear_kind(sim_ev_kind_t kind)
{
    for (int i = 0; i < SIM_EV_MAX; i++) {
        if (s_events[i].kind == kind) {
            s_events[i].kind = EV_NONE;
        }
    }
}

static void sim_net_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* Snapshot everything due; callbacks run after the lock drops. */
        sim_ev_t fired[SIM_EV_MAX];
        int fired_count = 0;
        bool scan_hit = false;
        svc_ble_dev_t scan_dev;
        svc_ble_scan_cb_t scan_cb = NULL;
        void *scan_user = NULL;

        TickType_t now = now_ticks();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < SIM_EV_MAX; i++) {
            if (s_events[i].kind != EV_NONE &&
                (int32_t)(now - s_events[i].due) >= 0) {
                fired[fired_count++] = s_events[i];
                s_events[i].kind = EV_NONE;
            }
        }
        if (s_ble_scanning && !s_rf_suspended &&
            (int32_t)(now - s_ble_scan_next) >= 0) {
            scan_hit = true;
            scan_dev = s_fake_ble_devs[s_ble_scan_idx];
            scan_cb = s_ble_scan_cb;
            scan_user = s_ble_scan_user;
            s_ble_scan_idx = (s_ble_scan_idx + 1) % FAKE_BLE_DEV_COUNT;
            s_ble_scan_next = now + BLE_SCAN_PERIOD_MS;
        }
        xSemaphoreGive(s_lock);

        for (int i = 0; i < fired_count; i++) {
            sim_ev_t *ev = &fired[i];
            switch (ev->kind) {
            case EV_WIFI_SCAN_DONE:
                if (ev->u.scan.cb) {
                    ev->u.scan.cb(s_fake_aps, FAKE_AP_COUNT, ev->u.scan.user);
                }
                break;
            case EV_WIFI_CONN_DONE:
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_wifi_connecting = false;
                s_wifi_connected = !ev->fail;
                xSemaphoreGive(s_lock);
                if (ev->u.conn.cb) {
                    ev->u.conn.cb(ev->fail ? SVC_WIFI_EV_CONNECT_FAILED
                                           : SVC_WIFI_EV_CONNECTED,
                                  ev->fail ? "auth failure (15)" : NULL,
                                  ev->u.conn.user);
                }
                break;
            case EV_BLE_CONN_DONE:
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_ble_connecting = false;
                s_ble_connected = true;
                xSemaphoreGive(s_lock);
                if (ev->u.ble_conn.cb) {
                    ev->u.ble_conn.cb(SVC_BLE_EV_CONNECTED, NULL, 0,
                                      ev->u.ble_conn.user);
                }
                /* Queue automatic GATT discovery completion. */
                xSemaphoreTake(s_lock, portMAX_DELAY);
                sim_ev_t *next = ev_alloc();
                if (next) {
                    next->kind = EV_BLE_SVCS_DONE;
                    next->due = now_ticks() + BLE_DISCOVER_DELAY_MS;
                    next->fail = false;
                    next->u.ble_conn = ev->u.ble_conn;
                }
                xSemaphoreGive(s_lock);
                break;
            case EV_WIFI_DISCONNECTED:
                if (ev->u.conn.cb) {
                    ev->u.conn.cb(SVC_WIFI_EV_DISCONNECTED, NULL,
                                  ev->u.conn.user);
                }
                break;
            case EV_BLE_SVCS_DONE:
                if (ev->u.ble_conn.cb) {
                    ev->u.ble_conn.cb(SVC_BLE_EV_SERVICES_DONE,
                                      s_fake_gatt_svcs, FAKE_GATT_SVC_COUNT,
                                      ev->u.ble_conn.user);
                }
                break;
            case EV_BLE_DISCONNECTED:
                if (ev->u.ble_conn.cb) {
                    ev->u.ble_conn.cb(SVC_BLE_EV_DISCONNECTED, NULL, 0,
                                      ev->u.ble_conn.user);
                }
                break;
            default:
                break;
            }
        }

        if (scan_hit && scan_cb) {
            scan_cb(&scan_dev, scan_user);
        }

        vTaskDelay(NET_TASK_POLL_MS);
    }
}

esp_err_t svc_net_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(sim_net_task, "svc_net", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create svc_net sim task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "sim network service started");
    return ESP_OK;
}

esp_err_t svc_wifi_scan(svc_wifi_scan_cb_t cb, void *user)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sim_ev_t *ev = ev_alloc();
    if (ev) {
        ev->kind = EV_WIFI_SCAN_DONE;
        ev->due = now_ticks() + WIFI_SCAN_DELAY_MS;
        ev->fail = false;
        ev->u.scan.cb = cb;
        ev->u.scan.user = user;
    } else {
        err = ESP_ERR_INVALID_STATE;  /* a scan is already pending */
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_wifi_connect(const char *ssid, const char *password,
                           svc_wifi_conn_cb_t cb, void *user)
{
    if (!s_started || !ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    bool fail = strstr(ssid, "fail") != NULL;
    if (!fail && (!password || strlen(password) < 8)) {
        /* Short passwords only pass on open networks. */
        bool open_ap = false;
        for (int i = 0; i < FAKE_AP_COUNT; i++) {
            if (strcmp(ssid, s_fake_aps[i].ssid) == 0 &&
                s_fake_aps[i].authmode == 0) {
                open_ap = true;
                break;
            }
        }
        fail = !open_ap;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_wifi_connecting || s_rf_suspended) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    sim_ev_t *ev = ev_alloc();
    if (!ev) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ev->kind = EV_WIFI_CONN_DONE;
    ev->due = now_ticks() + WIFI_CONNECT_DELAY_MS;
    ev->fail = fail;
    ev->u.conn.cb = cb;
    ev->u.conn.user = user;
    s_wifi_connecting = true;
    s_wifi_cb = cb;
    s_wifi_cb_user = user;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "sim wifi connecting to \"%s\"", ssid);
    return ESP_OK;
}

esp_err_t svc_wifi_disconnect(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_up = s_wifi_connected || s_wifi_connecting;
    svc_wifi_conn_cb_t cb = s_wifi_cb;
    void *user = s_wifi_cb_user;
    s_wifi_connecting = false;
    s_wifi_connected = false;
    ev_clear_kind(EV_WIFI_CONN_DONE);
    s_wifi_cb = NULL;
    s_wifi_cb_user = NULL;
    /* DISCONNECTED is queued, never called from the caller's thread. */
    sim_ev_t *ev = (was_up && cb) ? ev_alloc() : NULL;
    if (ev) {
        ev->kind = EV_WIFI_DISCONNECTED;
        ev->due = now_ticks();
        ev->fail = false;
        ev->u.conn.cb = cb;
        ev->u.conn.user = user;
    } else if (was_up && cb) {
        ESP_LOGW(TAG, "disconnect event slot busy, callback dropped");
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool svc_wifi_is_connecting(void)
{
    if (!s_started) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_wifi_connecting && !s_rf_suspended;
    xSemaphoreGive(s_lock);
    return v;
}

bool svc_wifi_is_connected(char *ip_out, size_t ip_len)
{
    if (!s_started) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_wifi_connected && !s_rf_suspended;
    xSemaphoreGive(s_lock);
    if (v && ip_out && ip_len > 0) {
        snprintf(ip_out, ip_len, "%s", SIM_IP);
    }
    return v;
}

bool svc_net_wifi_saved(char *ssid_out, size_t ssid_cap,
                        char *pass_out, size_t pass_cap)
{
    if (!s_saved_valid) {
        return false;
    }
    if (ssid_out && ssid_cap > 0) {
        snprintf(ssid_out, ssid_cap, "%s", "任平生");
    }
    if (pass_out && pass_cap > 0) {
        snprintf(pass_out, pass_cap, "%s", "lp113366");
    }
    return true;
}

void svc_net_wifi_saved_forget(void)
{
    s_saved_valid = false;
    ESP_LOGI(TAG, "sim saved wifi credentials forgotten");
}

esp_err_t svc_ble_scan_start(svc_ble_scan_cb_t cb, void *user)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ble_scanning) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_ble_scanning = true;
        s_ble_scan_cb = cb;
        s_ble_scan_user = user;
        s_ble_scan_idx = 0;
        s_ble_scan_next = now_ticks() + BLE_SCAN_PERIOD_MS;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t svc_ble_scan_stop(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ble_scanning = false;
    s_ble_scan_cb = NULL;
    s_ble_scan_user = NULL;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool svc_ble_is_connecting(void)
{
    if (!s_started) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_ble_connecting;
    xSemaphoreGive(s_lock);
    return v;
}

bool svc_ble_is_connected(void)
{
    if (!s_started) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_ble_connected;
    xSemaphoreGive(s_lock);
    return v;
}

esp_err_t svc_ble_connect(const uint8_t addr[6], uint8_t addr_type,
                          svc_ble_conn_cb_t cb, void *user)
{
    (void)addr_type;
    if (!s_started || !addr) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ble_connecting || s_ble_connected) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    sim_ev_t *ev = ev_alloc();
    if (!ev) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ev->kind = EV_BLE_CONN_DONE;
    ev->due = now_ticks() + BLE_CONNECT_DELAY_MS;
    ev->fail = false;
    ev->u.ble_conn.cb = cb;
    ev->u.ble_conn.user = user;
    s_ble_connecting = true;
    s_ble_conn_cb = cb;
    s_ble_conn_user = user;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "sim ble connecting to %02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return ESP_OK;
}

esp_err_t svc_ble_disconnect(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_up = s_ble_connected || s_ble_connecting;
    svc_ble_conn_cb_t cb = s_ble_conn_cb;
    void *user = s_ble_conn_user;
    s_ble_connecting = false;
    s_ble_connected = false;
    ev_clear_kind(EV_BLE_CONN_DONE);
    ev_clear_kind(EV_BLE_SVCS_DONE);
    s_ble_conn_cb = NULL;
    s_ble_conn_user = NULL;
    /* DISCONNECTED is queued, never called from the caller's thread. */
    sim_ev_t *ev = (was_up && cb) ? ev_alloc() : NULL;
    if (ev) {
        ev->kind = EV_BLE_DISCONNECTED;
        ev->due = now_ticks();
        ev->fail = false;
        ev->u.ble_conn.cb = cb;
        ev->u.ble_conn.user = user;
    } else if (was_up && cb) {
        ESP_LOGW(TAG, "ble disconnect event slot busy, callback dropped");
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t svc_net_suspend_rf(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rf_suspended = true;
    s_wifi_connecting = false;
    s_wifi_connected = false;
    ev_clear_kind(EV_WIFI_CONN_DONE);
    s_ble_scan_was_active = s_ble_scanning;
    s_ble_scanning = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "sim rf suspended");
    return ESP_OK;
}

esp_err_t svc_net_resume_rf(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rf_suspended = false;
    if (s_saved_valid) {
        s_wifi_connected = true;  /* silent reconnect, no callback */
    }
    if (s_ble_scan_was_active) {
        s_ble_scanning = true;
        s_ble_scan_idx = 0;
        s_ble_scan_next = now_ticks() + BLE_SCAN_PERIOD_MS;
    }
    s_ble_scan_was_active = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "sim rf resumed");
    return ESP_OK;
}

void sim_net_set_state(bool wifi_connected, bool ble_scanning)
{
    if (!s_started) {
        ESP_LOGW(TAG, "sim_net_set_state before svc_net_start, ignored");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_connected = wifi_connected;
    if (ble_scanning && !s_ble_scanning) {
        s_ble_scanning = true;
        s_ble_scan_cb = NULL;  /* no listener; state only */
        s_ble_scan_user = NULL;
        s_ble_scan_idx = 0;
        s_ble_scan_next = now_ticks() + BLE_SCAN_PERIOD_MS;
    } else if (!ble_scanning) {
        s_ble_scanning = false;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "sim state injected: wifi_connected=%d ble_scanning=%d",
             wifi_connected, ble_scanning);
}
