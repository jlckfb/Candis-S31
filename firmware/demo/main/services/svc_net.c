/*
 * Candis-S31 watch demo - network service: WiFi STA + BLE central (NimBLE).
 *
 * A single network task (prio 4, stack 6144, queue depth 8) serializes every
 * WiFi and BLE operation. Public API calls only enqueue a message and return;
 * results travel back through the same queue and user callbacks are always
 * invoked on the network task, so apps must forward UI work via ui_async().
 * WiFi link events (STA disconnect, GOT_IP) travel on a dedicated small
 * queue that the net task drains first, so a link-state transition is never
 * dropped behind command or scan traffic (a dropped transition would leave
 * the s_wifi.connected view out of sync with the radio until the next one).
 *
 * WiFi: netif/driver come up once in svc_net_start() and stay up. Scans are
 * the blocking kind (executed on the network task, never on the UI thread).
 * Connect follows the standard IDF WIFI_EVENT/IP_EVENT pattern; the 802.11
 * disconnect reason is mapped to human-readable Chinese strings. There is no
 * automatic retry: a link drop is reported and the UI decides what to do.
 *
 * BLE: NimBLE host. The host stack runs on its own FreeRTOS task created by
 * nimble_port_freertos_init(); GAP/GATT callbacks therefore arrive on that
 * task. NimBLE host APIs are internally serialized by the ble_hs lock, so the
 * network task may call the ble_gap and ble_gattc entry points directly once
 * the host is synced; everything user-visible is still bounced through the
 * network queue so callbacks keep the documented task context. Scan results
 * are de-duplicated by advertiser address for the whole session.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc_net.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#define NET_TAG "svc_net"

#define NET_TASK_STACK   6144
#define NET_TASK_PRIO    4
#define NET_QUEUE_LEN    8
#define NET_ADV_QUEUE_LEN 16
#define NET_WIFI_EV_QUEUE_LEN 4   /* link transitions: drained first */
#define NET_READY_BIT    BIT0

#define WIFI_AP_MAX      20   /* contract: scan list cap */
#define WIFI_SCAN_RECS   25
#define WIFI_CONN_TIMEOUT_MS 15000

#define BLE_SEEN_MAX     32   /* session de-dup cache */
#define BLE_SVC_MAX      16   /* contract: service list cap */
#define BLE_CONNECT_TIMEOUT_MS 10000

/* HCI "connection terminated by local host"; not exposed by public headers. */
#define NET_HCI_ERR_LOCAL_TERM 0x13

typedef enum {
    MSG_WIFI_SCAN,
    MSG_WIFI_CONNECT,
    MSG_WIFI_DISCONNECT,
    MSG_WIFI_EVENT,      /* raw WIFI_EVENT/IP_EVENT from the event loop task */
    MSG_BLE_SCAN_START,
    MSG_BLE_SCAN_STOP,
    MSG_BLE_CONNECT,
    MSG_BLE_DISCONNECT,
    MSG_BLE_ADV,         /* de-duplicated advertisement from the host task */
    MSG_BLE_EVENT,       /* connect/disconnect/discovery-done notifications */
    MSG_NET_RF_SUSPEND,  /* screen-off: park the WiFi driver + BLE scan */
    MSG_NET_RF_RESUME,   /* screen-on: restore the radios */
} net_msg_type_t;

typedef enum {
    WIFI_EV_DISC,
    WIFI_EV_GOT_IP,
} net_wifi_sub_t;

typedef enum {
    BLE_EV_CONNECTED,
    BLE_EV_CONNECT_FAILED,
    BLE_EV_DISCONNECTED,
    BLE_EV_DISC_DONE,
    BLE_EV_HOST_RESET,
} net_ble_sub_t;

typedef struct {
    net_msg_type_t type;
    union {
        struct {
            svc_wifi_scan_cb_t cb;
            void *user;
        } wifi_scan;
        struct {
            char ssid[33];
            char password[65];
            svc_wifi_conn_cb_t cb;
            void *user;
        } wifi_connect;
        struct {
            net_wifi_sub_t sub;
            int reason;          /* WIFI_EVENT_STA_DISCONNECTED reason */
            uint32_t ip;         /* GOT_IP: raw ip4 addr (esp_ip4_addr_t.addr) */
        } wifi_event;
        struct {
            svc_ble_scan_cb_t cb;
            void *user;
        } ble_scan;
        struct {
            uint8_t addr[6];
            uint8_t addr_type;
            svc_ble_conn_cb_t cb;
            void *user;
        } ble_connect;
        svc_ble_dev_t adv;
        struct {
            net_ble_sub_t sub;
        } ble_event;
    };
} net_msg_t;

/* ---------------- shared state ---------------- */

static QueueHandle_t s_queue;
static QueueHandle_t s_adv_queue;
static QueueHandle_t s_wifi_ev_queue;  /* WiFi link events: drained first */
static SemaphoreHandle_t s_wifi_mutex; /* guards connected/ip for any-context reads */
static EventGroupHandle_t s_ready_event;
static bool s_started;

static struct {
    bool up;              /* driver initialized and started */
    volatile bool connecting;
    volatile bool connected;
    volatile bool user_disconnect;
    TickType_t conn_start;
    char ssid[33];
    char password[65];   /* RAM copy of the current attempt; saved to NVS
                          * on GOT_IP and zeroed right after */
    char ip[16];
    svc_wifi_conn_cb_t cb;
    void *cb_user;
} s_wifi;

static struct {
    bool ready;                 /* nimble_port_init() succeeded */
    volatile bool synced;       /* host synced with the controller */
    volatile bool scanning;
    uint16_t conn_handle;       /* net-task view of the live connection */
    bool connected;             /* CONNECTED already reported to the app */
    svc_ble_scan_cb_t scan_cb;
    void *scan_user;
    svc_ble_conn_cb_t conn_cb;
    void *conn_user;
} s_ble;

/* RF suspend state (network task only): what to restore on resume. */
static bool s_rf_suspended;
static bool s_rf_scan_was_running;
static svc_ble_scan_cb_t s_rf_scan_cb;
static void *s_rf_scan_user;

/* Written on the NimBLE host task only. */
static struct {
    bool used;
    uint8_t type;
    uint8_t val[6];
} s_seen[BLE_SEEN_MAX];
static int s_seen_next;

static svc_ble_svc_t s_disc_svcs[BLE_SVC_MAX];
static int s_disc_count;

/* ---------------- small helpers ---------------- */

static bool net_send(net_msg_t *msg, TickType_t timeout)
{
    return s_queue != NULL && xQueueSend(s_queue, msg, timeout) == pdPASS;
}

static bool net_send_adv(net_msg_t *msg)
{
    return s_adv_queue != NULL && xQueueSend(s_adv_queue, msg, 0) == pdPASS;
}

static void net_post_ble_event(net_ble_sub_t sub)
{
    net_msg_t msg = { .type = MSG_BLE_EVENT };
    msg.ble_event.sub = sub;
    /* Connection lifecycle transitions ride the drained-first priority
     * queue so they are never stranded behind a burst of scan reports or
     * advertisements in the depth-8 command queue (a dropped CONNECT /
     * DISCONNECT leaves the UI stuck in "connecting"). DISC_DONE keeps the
     * normal path with the scan traffic it belongs to. */
    if (sub != BLE_EV_DISC_DONE) {
        if (s_wifi_ev_queue != NULL &&
                xQueueSend(s_wifi_ev_queue, &msg, 0) == pdPASS) {
            return;
        }
        ESP_LOGW(NET_TAG, "priority queue full, BLE event %d dropped", sub);
        return;
    }
    if (!net_send(&msg, 0)) {
        ESP_LOGW(NET_TAG, "net queue full, BLE event %d dropped", sub);
    }
}

/* ---------------- WiFi: bring-up (called once, on the main task) ---------------- */

static void net_wifi_event_handler(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    (void)arg;
    net_msg_t msg = { .type = MSG_WIFI_EVENT };

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        msg.wifi_event.sub = WIFI_EV_DISC;
        msg.wifi_event.reason = ev ? ev->reason : 0;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        msg.wifi_event.sub = WIFI_EV_GOT_IP;
        msg.wifi_event.ip = ev ? ev->ip_info.ip.addr : 0;
    } else {
        return;
    }
    /* Link events go to their own queue, drained first by the net task:
     * a command queue full of BLE control traffic must never drop a
     * link-state transition. */
    if (s_wifi_ev_queue == NULL ||
            xQueueSend(s_wifi_ev_queue, &msg, 0) != pdPASS) {
        ESP_LOGW(NET_TAG, "WiFi event queue full, event dropped");
    }
}

static esp_err_t net_wifi_bringup(void)
{
    esp_err_t err = esp_netif_init(); /* idempotent */
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(NET_TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(NET_TAG, "create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        net_wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        net_wifi_event_handler, NULL, NULL);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(NET_TAG, "WiFi STA up");
    return ESP_OK;
}

/* Map an 802.11 disconnect reason to a UI-facing Chinese string. */
static const char *net_wifi_fail_text(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "Wrong password/auth failed";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "Network not found";
    case WIFI_REASON_ASSOC_FAIL:
        return "Association refused";
    case WIFI_REASON_CONNECTION_FAIL:
        return "Connect failed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Signal timeout";
    default:
        return "Connect failed";
    }
}

static int net_ap_rssi_cmp(const void *a, const void *b)
{
    const svc_wifi_ap_t *x = a, *y = b;
    return y->rssi - x->rssi; /* descending */
}

static void net_wifi_do_scan(const net_msg_t *msg)
{
    svc_wifi_ap_t aps[WIFI_AP_MAX];
    int count = 0;

    if (!s_wifi.up) {
        return;
    }
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        ESP_LOGW(NET_TAG, "scan_start failed");
        if (msg->wifi_scan.cb) {
            msg->wifi_scan.cb(NULL, 0, msg->wifi_scan.user);
        }
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found > WIFI_SCAN_RECS) {
        found = WIFI_SCAN_RECS;
    }
    static wifi_ap_record_t recs[WIFI_SCAN_RECS];
    uint16_t got = found;
    if (found > 0 && esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        got = 0;
    }

    for (uint16_t i = 0; i < got && count < WIFI_AP_MAX; ++i) {
        const wifi_ap_record_t *r = &recs[i];
        /* Collapse duplicate SSIDs (multi-BSS APs), keep the strongest. */
        int dup = -1;
        for (int k = 0; k < count; ++k) {
            if (strncmp(aps[k].ssid, (const char *)r->ssid, 32) == 0) {
                dup = k;
                break;
            }
        }
        if (dup >= 0) {
            if (r->rssi > aps[dup].rssi) {
                aps[dup].rssi = r->rssi;
                aps[dup].authmode = (uint8_t)r->authmode;
                aps[dup].channel = r->primary;
            }
            continue;
        }
        svc_wifi_ap_t *ap = &aps[count++];
        memset(ap, 0, sizeof(*ap));
        memcpy(ap->ssid, r->ssid, 32);
        ap->ssid[32] = '\0';
        ap->rssi = r->rssi;
        ap->authmode = (uint8_t)r->authmode;
        ap->channel = r->primary;
    }

    qsort(aps, count, sizeof(aps[0]), net_ap_rssi_cmp);
    if (msg->wifi_scan.cb) {
        msg->wifi_scan.cb(aps, count, msg->wifi_scan.user);
    }
}

static void net_wifi_do_connect(const net_msg_t *msg)
{
    if (!s_wifi.up || s_wifi.connecting || s_wifi.connected) {
        return;
    }
    s_wifi.cb = msg->wifi_connect.cb;
    s_wifi.cb_user = msg->wifi_connect.user;
    snprintf(s_wifi.ssid, sizeof(s_wifi.ssid), "%s", msg->wifi_connect.ssid);
    snprintf(s_wifi.password, sizeof(s_wifi.password), "%s",
             msg->wifi_connect.password);

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.sta.ssid, msg->wifi_connect.ssid, sizeof(cfg.sta.ssid));
    memcpy(cfg.sta.password, msg->wifi_connect.password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; /* allow WEP/legacy APs too */

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK || esp_wifi_connect() != ESP_OK) {
        ESP_LOGW(NET_TAG, "wifi connect kickoff failed: %s", esp_err_to_name(err));
        s_wifi.cb = NULL;
        if (msg->wifi_connect.cb) {
            msg->wifi_connect.cb(SVC_WIFI_EV_CONNECT_FAILED, "Connect failed",
                                 msg->wifi_connect.user);
        }
        return;
    }
    s_wifi.connecting = true;
    s_wifi.conn_start = xTaskGetTickCount();
    ESP_LOGI(NET_TAG, "connecting to \"%s\"", s_wifi.ssid);
}

/* ---------------- WiFi: persisted credentials ----------------
 * Namespace "demo" (shared with demo_board.c settings), keys wifi_ssid /
 * wifi_pass. Written once, after GOT_IP, so a failed attempt never
 * clobbers known-good credentials. Failures only warn - persistence must
 * not break the connection itself. */

#define NET_NVS_NAMESPACE "demo"
#define NET_NVS_KEY_SSID  "wifi_ssid"
#define NET_NVS_KEY_PASS  "wifi_pass"

static void net_wifi_cred_save(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    if (nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(NET_TAG, "wifi credentials: NVS open failed");
        return;
    }
    esp_err_t err = nvs_set_str(handle, NET_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NET_NVS_KEY_PASS, pass != NULL ? pass : "");
    }
    const esp_err_t commit_err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        err = commit_err;
    }
    if (err != ESP_OK) {
        ESP_LOGW(NET_TAG, "wifi credentials save failed: %s",
                 esp_err_to_name(err));
    }
}

static void net_wifi_cred_forget(void)
{
    nvs_handle_t handle;
    if (nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_erase_key(handle, NET_NVS_KEY_SSID);
    nvs_erase_key(handle, NET_NVS_KEY_PASS);
    nvs_commit(handle);
    nvs_close(handle);
}

static void net_wifi_on_event(const net_msg_t *msg)
{
    if (s_rf_suspended) {
        /* Screen-off RF park: the DISCONNECTED that trails
         * esp_wifi_disconnect()/esp_wifi_stop() belongs to the suspend
         * orchestration and must never surface as a UI event (no
         * toast while the screen is dark). */
        s_wifi.user_disconnect = false;
        s_wifi.connecting = false;
        return;
    }

    if (msg->wifi_event.sub == WIFI_EV_GOT_IP) {
        char ip[16];
        /* Same octet order as lwip's IP2STR(). */
        uint32_t a = msg->wifi_event.ip;
        snprintf(ip, sizeof(ip), "%lu.%lu.%lu.%lu",
                 (unsigned long)(a & 0xff),
                 (unsigned long)((a >> 8) & 0xff),
                 (unsigned long)((a >> 16) & 0xff),
                 (unsigned long)((a >> 24) & 0xff));
        if (s_wifi_mutex && xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(50)) == pdPASS) {
            s_wifi.connected = true;
            s_wifi.connecting = false;
            snprintf(s_wifi.ip, sizeof(s_wifi.ip), "%s", ip);
            xSemaphoreGive(s_wifi_mutex);
        } else {
            s_wifi.connected = true;
            s_wifi.connecting = false;
            snprintf(s_wifi.ip, sizeof(s_wifi.ip), "%s", ip);
        }
        ESP_LOGI(NET_TAG, "got ip %s", ip);
        /* Persist the credentials only after a full connection: a failed
         * attempt keeps whatever was saved before. */
        if (s_wifi.ssid[0] != '\0') {
            net_wifi_cred_save(s_wifi.ssid, s_wifi.password);
            s_wifi.password[0] = '\0';
        }
        if (s_wifi.cb) {
            s_wifi.cb(SVC_WIFI_EV_CONNECTED, ip, s_wifi.cb_user);
        }
        return;
    }

    /* WIFI_EV_DISC */
    s_wifi.password[0] = '\0';
    int reason = msg->wifi_event.reason;
    if (s_wifi.user_disconnect) {
        s_wifi.user_disconnect = false;
        s_wifi.connecting = false;
        if (s_wifi_mutex && xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(50)) == pdPASS) {
            s_wifi.connected = false;
            s_wifi.ip[0] = '\0';
            xSemaphoreGive(s_wifi_mutex);
        } else {
            s_wifi.connected = false;
            s_wifi.ip[0] = '\0';
        }
        s_wifi.cb = NULL;
        return;
    }
    if (s_wifi.connecting) {
        s_wifi.connecting = false;
        svc_wifi_conn_cb_t cb = s_wifi.cb;
        void *user = s_wifi.cb_user;
        s_wifi.cb = NULL;
        ESP_LOGW(NET_TAG, "connect failed, reason %d", reason);
        if (cb) {
            cb(SVC_WIFI_EV_CONNECT_FAILED, net_wifi_fail_text(reason), user);
        }
        return;
    }
    if (s_wifi.connected) {
        if (s_wifi_mutex && xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(50)) == pdPASS) {
            s_wifi.connected = false;
            s_wifi.ip[0] = '\0';
            xSemaphoreGive(s_wifi_mutex);
        } else {
            s_wifi.connected = false;
            s_wifi.ip[0] = '\0';
        }
        ESP_LOGW(NET_TAG, "link lost, reason %d", reason);
        if (s_wifi.cb) {
            s_wifi.cb(SVC_WIFI_EV_DISCONNECTED, net_wifi_fail_text(reason),
                      s_wifi.cb_user);
        }
    }
}

/* Called from the idle slice of the network task; catches APs that neither
 * complete nor fail (no DISCONNECTED event at all). */
static void net_wifi_poll_timeout(void)
{
    if (!s_wifi.connecting) {
        return;
    }
    if (xTaskGetTickCount() - s_wifi.conn_start <
            pdMS_TO_TICKS(WIFI_CONN_TIMEOUT_MS)) {
        return;
    }
    s_wifi.connecting = false;
    esp_wifi_disconnect(); /* cancel the attempt quietly */
    s_wifi.user_disconnect = false;
    s_wifi.password[0] = '\0'; /* close the RAM window for this attempt */
    svc_wifi_conn_cb_t cb = s_wifi.cb;
    void *user = s_wifi.cb_user;
    s_wifi.cb = NULL;
    ESP_LOGW(NET_TAG, "connect timeout");
    if (cb) {
        cb(SVC_WIFI_EV_CONNECT_FAILED, "Connect timeout", user);
    }
}

/* ---------------- BLE: NimBLE glue (host-task context) ---------------- */

void ble_store_config_init(void);

static void net_ble_host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static void net_ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(NET_TAG, "ble_hs_util_ensure_addr: %d", rc);
        return;
    }
    s_ble.synced = true;
    ESP_LOGI(NET_TAG, "BLE host synced");
}

static void net_ble_on_reset(int reason)
{
    ESP_LOGW(NET_TAG, "BLE host reset, reason %d", reason);
    s_ble.synced = false;
    if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        net_post_ble_event(BLE_EV_DISCONNECTED);
    } else if (s_ble.conn_cb != NULL) {
        /* Connection attempt pending but not yet established: no GAP
         * CONNECT event will ever arrive for it, because the host
         * restart discards the procedure. Without this delivery the
         * conn_cb slot stays occupied and every later svc_ble_connect()
         * is refused with INVALID_STATE until reboot. The failure is
         * delivered through the normal queue path so the callback runs
         * on the network task, exactly like a GAP-reported failure. */
        net_post_ble_event(BLE_EV_CONNECT_FAILED);
    }
    /* Scan state has the same stuck-slot hazard: after a host reset the
     * controller no longer reports advertisements, but the scanning flag
     * and scan_cb would keep occupying the session until the app stops
     * the scan itself. The net task owns those fields; tell it to clear
     * them (symmetrical to the conn_cb handling above). */
    net_post_ble_event(BLE_EV_HOST_RESET);
}

static bool net_ble_seen(const ble_addr_t *addr)
{
    for (int i = 0; i < BLE_SEEN_MAX; ++i) {
        if (s_seen[i].used && s_seen[i].type == addr->type &&
                memcmp(s_seen[i].val, addr->val, 6) == 0) {
            return true;
        }
    }
    int slot = s_seen_next;
    s_seen_next = (s_seen_next + 1) % BLE_SEEN_MAX;
    s_seen[slot].used = true;
    s_seen[slot].type = addr->type;
    memcpy(s_seen[slot].val, addr->val, 6);
    return false;
}

static void net_ble_format_uuid(const ble_uuid_t *uuid, char *out, size_t cap)
{
    if (uuid->type == BLE_UUID_TYPE_16) {
        snprintf(out, cap, "0x%04X", ((const ble_uuid16_t *)uuid)->value);
    } else if (uuid->type == BLE_UUID_TYPE_32) {
        snprintf(out, cap, "0x%08lX",
                 (unsigned long)((const ble_uuid32_t *)uuid)->value);
    } else {
        char tmp[40];
        ble_uuid_to_str(uuid, tmp);
        snprintf(out, cap, "%s", tmp);
    }
}

/* GATT service discovery callback (host task). Collects up to BLE_SVC_MAX
 * services, then signals completion through the network queue. */
static int net_ble_disc_svc_cb(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               const struct ble_gatt_svc *service, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == 0 && service != NULL) {
        if (s_disc_count < BLE_SVC_MAX) {
            svc_ble_svc_t *out = &s_disc_svcs[s_disc_count++];
            memset(out, 0, sizeof(*out));
            net_ble_format_uuid(&service->uuid.u, out->uuid, sizeof(out->uuid));
            out->start_handle = service->start_handle;
            out->end_handle = service->end_handle;
        }
        return 0;
    }
    /* BLE_HS_EDONE or an error: deliver whatever was collected. */
    net_post_ble_event(BLE_EV_DISC_DONE);
    return 0;
}

/* Single GAP event handler for scan reports, connect and disconnect. Runs on
 * the NimBLE host task; forwards everything interesting to the net queue. */
static int net_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *disc = &event->disc;
        if (!s_ble.scan_cb) {
            return 0; /* scan already stopped by the net task */
        }
        if (net_ble_seen(&disc->addr)) {
            return 0; /* session duplicate */
        }
        net_msg_t msg = { .type = MSG_BLE_ADV };
        svc_ble_dev_t *dev = &msg.adv;
        memset(dev, 0, sizeof(*dev));
        memcpy(dev->addr, disc->addr.val, 6);
        dev->addr_type = disc->addr.type;
        dev->rssi = disc->rssi;
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0 &&
                fields.name != NULL && fields.name_len > 0) {
            size_t n = fields.name_len;
            if (n > sizeof(dev->name) - 1) {
                n = sizeof(dev->name) - 1;
            }
            memcpy(dev->name, fields.name, n);
        }
        if (!net_send_adv(&msg)) {
            ESP_LOGD(NET_TAG, "adv queue drop");
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status == 0) {
            s_ble.conn_handle = event->connect.conn_handle;
            net_post_ble_event(BLE_EV_CONNECTED);
            s_disc_count = 0;
            int rc = ble_gattc_disc_all_svcs(s_ble.conn_handle,
                                             net_ble_disc_svc_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(NET_TAG, "svc discovery start failed: %d", rc);
                net_post_ble_event(BLE_EV_DISC_DONE);
            }
        } else {
            s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGW(NET_TAG, "connect failed, status %d", event->connect.status);
            net_post_ble_event(BLE_EV_CONNECT_FAILED);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        if (event->disconnect.conn.conn_handle == s_ble.conn_handle) {
            s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            net_post_ble_event(BLE_EV_DISCONNECTED);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
    default:
        return 0;
    }
}

static esp_err_t net_ble_bringup(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    s_ble.ready = true;
    s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;

    ble_hs_cfg.reset_cb = net_ble_on_reset;
    ble_hs_cfg.sync_cb = net_ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    nimble_port_freertos_init(net_ble_host_task);
    ESP_LOGI(NET_TAG, "NimBLE host started");
    return ESP_OK;
}

/* ---------------- BLE: command handling (network task) ---------------- */

static void net_ble_do_scan_start(const net_msg_t *msg)
{
    if (!s_ble.ready || !s_ble.synced) {
        ESP_LOGW(NET_TAG, "BLE not ready");
        return;
    }
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.passive = 1;
    /* Controller-side duplicate filtering is OFF on purpose. Observation
     * (2026-08-21 board log): with filter_duplicates=1 the LE Set Scan
     * Enable was rejected by the controller with hci_err=0x207 while the
     * same runtime had already exhausted the internal DMA pool for the
     * I2S path - correlation, root cause not proven (the controller is a
     * closed blob). The host already de-duplicates the session
     * (net_ble_seen, 32 entries, reset just below), so disabling the
     * controller filter is the minimal single-variable experiment/fix;
     * report volume stays bounded by the controller flow control and the
     * host pool lives in PSRAM. */
    params.filter_duplicates = 0;
    s_seen_next = 0;
    memset(s_seen, 0, sizeof(s_seen));
    if (s_adv_queue != NULL) {
        xQueueReset(s_adv_queue);
    }
    s_ble.scan_cb = msg->ble_scan.cb;
    s_ble.scan_user = msg->ble_scan.user;
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params,
                          net_ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(NET_TAG, "ble_gap_disc: %d", rc);
        s_ble.scan_cb = NULL;
        s_ble.scan_user = NULL;
        return;
    }
    s_ble.scanning = true;
    ESP_LOGI(NET_TAG, "BLE scan started");
}

static void net_ble_do_scan_stop(void)
{
    if (!s_ble.ready) {
        return;
    }
    if (s_ble.scanning) {
        int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(NET_TAG, "disc_cancel: %d", rc);
        }
        s_ble.scanning = false;
    }
    s_ble.scan_cb = NULL;
    s_ble.scan_user = NULL;
    if (s_adv_queue != NULL) {
        xQueueReset(s_adv_queue);
    }
}

static void net_ble_do_connect(const net_msg_t *msg)
{
    if (!s_ble.ready || !s_ble.synced) {
        ESP_LOGW(NET_TAG, "BLE not ready");
        return;
    }
    if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE || s_ble.conn_cb != NULL) {
        ESP_LOGW(NET_TAG, "BLE busy");
        return;
    }
    /* Scanning must stop before a connection can be initiated. */
    net_ble_do_scan_stop();

    s_ble.conn_cb = msg->ble_connect.cb;
    s_ble.conn_user = msg->ble_connect.user;

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    ble_addr_t peer;
    peer.type = msg->ble_connect.addr_type;
    memcpy(peer.val, msg->ble_connect.addr, 6);

    int rc = ble_gap_connect(own_addr_type, &peer, BLE_CONNECT_TIMEOUT_MS,
                             NULL, net_ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(NET_TAG, "ble_gap_connect: %d", rc);
        svc_ble_conn_cb_t cb = s_ble.conn_cb;
        void *user = s_ble.conn_user;
        s_ble.conn_cb = NULL;
        s_ble.conn_user = NULL;
        if (cb) {
            cb(SVC_BLE_EV_CONNECT_FAILED, NULL, 0, user);
        }
    }
}

static void net_ble_do_disconnect(void)
{
    if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(s_ble.conn_handle, NET_HCI_ERR_LOCAL_TERM);
        if (rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(NET_TAG, "ble_gap_terminate: %d", rc);
        }
        /* The DISCONNECT gap event reports completion. */
    }
}

static void net_ble_on_event(const net_msg_t *msg)
{
    switch (msg->ble_event.sub) {
    case BLE_EV_CONNECTED:
        s_ble.connected = true;
        if (s_ble.conn_cb) {
            s_ble.conn_cb(SVC_BLE_EV_CONNECTED, NULL, 0, s_ble.conn_user);
        }
        break;
    case BLE_EV_CONNECT_FAILED:
        if (s_ble.conn_cb) {
            s_ble.conn_cb(SVC_BLE_EV_CONNECT_FAILED, NULL, 0, s_ble.conn_user);
        }
        s_ble.conn_cb = NULL;
        s_ble.conn_user = NULL;
        break;
    case BLE_EV_DISC_DONE:
        if (s_ble.conn_cb) {
            s_ble.conn_cb(SVC_BLE_EV_SERVICES_DONE, s_disc_svcs, s_disc_count,
                          s_ble.conn_user);
        }
        break;
    case BLE_EV_DISCONNECTED:
        s_ble.connected = false;
        if (s_ble.conn_cb) {
            s_ble.conn_cb(SVC_BLE_EV_DISCONNECTED, NULL, 0, s_ble.conn_user);
        }
        s_ble.conn_cb = NULL;
        s_ble.conn_user = NULL;
        break;
    case BLE_EV_HOST_RESET:
        /* The host restarted: the controller-side scan procedure is gone
         * with it. Clear the session state the net task owns. There is no
         * scan-completion event in the public API, so scan_cb is simply
         * dropped, matching "the scan is over". */
        s_ble.scanning = false;
        s_ble.scan_cb = NULL;
        s_ble.scan_user = NULL;
        break;
    default:
        break;
    }
}

/* ---------------- RF suspend/resume (screen off/on) ---------------- */

/* Park the radios for the screen-off period. Network task only. WiFi:
 * abort any in-flight attempt without callbacks, disconnect, stop the
 * driver (the trailing DISCONNECTED is absorbed by the s_rf_suspended
 * gate in net_wifi_on_event). BLE: cancel a running scan, remembering
 * its callback for resume; GATT links are left alone - only the scan
 * is the screen-off drain. */
static void net_rf_do_suspend(void)
{
    if (s_rf_suspended) {
        return;
    }
    s_rf_suspended = true;

    s_rf_scan_was_running = s_ble.scanning;
    s_rf_scan_cb = s_ble.scan_cb;
    s_rf_scan_user = s_ble.scan_user;
    if (s_ble.scanning) {
        const int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(NET_TAG, "disc_cancel at suspend: %d", rc);
        }
        s_ble.scanning = false;
    }
    s_ble.scan_cb = NULL;
    s_ble.scan_user = NULL;
    if (s_adv_queue != NULL) {
        xQueueReset(s_adv_queue);
    }

    const bool wifi_was_active = s_wifi.connecting || s_wifi.connected;
    s_wifi.cb = NULL;
    s_wifi.cb_user = NULL;
    s_wifi.user_disconnect = false;
    s_wifi.connecting = false;
    s_wifi.password[0] = '\0';
    if (s_wifi_mutex != NULL &&
            xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(50)) == pdPASS) {
        s_wifi.connected = false;
        s_wifi.ip[0] = '\0';
        xSemaphoreGive(s_wifi_mutex);
    } else {
        s_wifi.connected = false;
        s_wifi.ip[0] = '\0';
    }
    if (s_wifi.up) {
        if (wifi_was_active) {
            esp_wifi_disconnect(); /* quiet: the event is gated */
        }
        const esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGW(NET_TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
        }
    }
    if (s_wifi_ev_queue != NULL) {
        xQueueReset(s_wifi_ev_queue);
    }
    ESP_LOGI(NET_TAG, "RF suspended (WiFi %s, BLE scan %s)",
             s_wifi.up ? "stopped" : "unavailable",
             s_rf_scan_was_running ? "stopped" : "idle");
}

/* Restore what net_rf_do_suspend() parked. Network task only: restart
 * the WiFi driver and reconnect from the persisted credentials (cb
 * stays NULL - a failing AP must not toast on wake), then restart a
 * BLE scan that was running at suspend time with its original
 * callback. */
static void net_rf_do_resume(void)
{
    if (!s_rf_suspended) {
        return;
    }
    s_rf_suspended = false;

    if (s_wifi.up) {
        const esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGW(NET_TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        } else {
            char ssid[33];
            char pass[65];
            if (svc_net_wifi_saved(ssid, sizeof(ssid), pass, sizeof(pass)) &&
                    ssid[0] != '\0') {
                net_msg_t msg = { .type = MSG_WIFI_CONNECT };
                snprintf(msg.wifi_connect.ssid,
                         sizeof(msg.wifi_connect.ssid), "%s", ssid);
                snprintf(msg.wifi_connect.password,
                         sizeof(msg.wifi_connect.password), "%s", pass);
                msg.wifi_connect.cb = NULL;
                msg.wifi_connect.user = NULL;
                net_wifi_do_connect(&msg);
            }
        }
    }

    if (s_rf_scan_was_running) {
        s_rf_scan_was_running = false;
        if (s_ble.ready && s_ble.synced && s_rf_scan_cb != NULL) {
            net_msg_t msg = { .type = MSG_BLE_SCAN_START };
            msg.ble_scan.cb = s_rf_scan_cb;
            msg.ble_scan.user = s_rf_scan_user;
            net_ble_do_scan_start(&msg);
        } else {
            ESP_LOGW(NET_TAG, "BLE scan not restored (host state changed)");
        }
        s_rf_scan_cb = NULL;
        s_rf_scan_user = NULL;
    }
    ESP_LOGI(NET_TAG, "RF resumed (WiFi driver %s)",
             s_wifi.up ? "up" : "unavailable");
}


/* ---------------- network task ---------------- */

static void net_task(void *arg)
{
    (void)arg;
    /* WiFi callbacks and the NimBLE host can enqueue messages while their
     * stacks are being brought up. Do not consume those messages until both
     * bring-up attempts have finished and the capability flags are stable. */
    if (s_ready_event != NULL) {
        xEventGroupWaitBits(s_ready_event, NET_READY_BIT, pdFALSE, pdTRUE,
                            portMAX_DELAY);
    }
    net_msg_t msg;
    for (;;) {
        /* WiFi link transitions first: they are rare and the radio state
         * view must follow them without loss, so they never wait behind
         * command or scan traffic. Then control commands; advertisements
         * last (a busy scan must not be able to starve lifecycle events). */
        BaseType_t received = xQueueReceive(s_wifi_ev_queue, &msg, 0);
        if (received != pdPASS) {
            received = xQueueReceive(s_queue, &msg, 0);
        }
        if (received != pdPASS) {
            received = xQueueReceive(s_adv_queue, &msg, pdMS_TO_TICKS(20));
        }
        if (received == pdPASS) {
            switch (msg.type) {
            case MSG_WIFI_SCAN:
                net_wifi_do_scan(&msg);
                break;
            case MSG_WIFI_CONNECT:
                net_wifi_do_connect(&msg);
                break;
            case MSG_WIFI_DISCONNECT:
                if (s_wifi.connecting || s_wifi.connected) {
                    s_wifi.user_disconnect = true;
                    esp_wifi_disconnect();
                }
                break;
            case MSG_WIFI_EVENT:
                net_wifi_on_event(&msg);
                break;
            case MSG_BLE_SCAN_START:
                net_ble_do_scan_start(&msg);
                break;
            case MSG_BLE_SCAN_STOP:
                net_ble_do_scan_stop();
                break;
            case MSG_BLE_CONNECT:
                net_ble_do_connect(&msg);
                break;
            case MSG_BLE_DISCONNECT:
                net_ble_do_disconnect();
                break;
            case MSG_BLE_ADV:
                if (s_ble.scan_cb) {
                    s_ble.scan_cb(&msg.adv, s_ble.scan_user);
                }
                break;
            case MSG_BLE_EVENT:
                net_ble_on_event(&msg);
                break;
            case MSG_NET_RF_SUSPEND:
                net_rf_do_suspend();
                break;
            case MSG_NET_RF_RESUME:
                net_rf_do_resume();
                break;
            default:
                break;
            }
        }
        net_wifi_poll_timeout();
    }
}

/* ---------------- public API ---------------- */

esp_err_t svc_net_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_wifi_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(NET_QUEUE_LEN, sizeof(net_msg_t));
    s_adv_queue = xQueueCreate(NET_ADV_QUEUE_LEN, sizeof(net_msg_t));
    s_wifi_ev_queue = xQueueCreate(NET_WIFI_EV_QUEUE_LEN, sizeof(net_msg_t));
    s_ready_event = xEventGroupCreate();
    if (s_wifi_mutex == NULL || s_queue == NULL || s_adv_queue == NULL ||
            s_wifi_ev_queue == NULL || s_ready_event == NULL) {
        ESP_LOGE(NET_TAG, "ipc alloc failed");
        if (s_ready_event != NULL) {
            vEventGroupDelete(s_ready_event);
            s_ready_event = NULL;
        }
        if (s_queue != NULL) {
            vQueueDelete(s_queue);
            s_queue = NULL;
        }
        if (s_adv_queue != NULL) {
            vQueueDelete(s_adv_queue);
            s_adv_queue = NULL;
        }
        if (s_wifi_ev_queue != NULL) {
            vQueueDelete(s_wifi_ev_queue);
            s_wifi_ev_queue = NULL;
        }
        if (s_wifi_mutex != NULL) {
            vSemaphoreDelete(s_wifi_mutex);
            s_wifi_mutex = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(net_task, "svc_net", NET_TASK_STACK, NULL,
                                NET_TASK_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(NET_TAG, "task create failed");
        vEventGroupDelete(s_ready_event);
        s_ready_event = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        vQueueDelete(s_adv_queue);
        s_adv_queue = NULL;
        vQueueDelete(s_wifi_ev_queue);
        s_wifi_ev_queue = NULL;
        vSemaphoreDelete(s_wifi_mutex);
        s_wifi_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t wifi_err = net_wifi_bringup();
    s_wifi.up = (wifi_err == ESP_OK);
    esp_err_t ble_err = net_ble_bringup();
    /* Mark the service started even when both capabilities failed: the task
     * and IPC are valid, and public APIs can consistently report the specific
     * unavailable capability until the next reboot rather than pretending a
     * retry-safe rollback exists for partially initialized IDF stacks. */
    s_started = true;
    xEventGroupSetBits(s_ready_event, NET_READY_BIT);
    if (wifi_err != ESP_OK && ble_err != ESP_OK) {
        ESP_LOGE(NET_TAG, "network capabilities unavailable (WiFi=%s, BLE=%s)",
                 esp_err_to_name(wifi_err), esp_err_to_name(ble_err));
        return wifi_err;
    }
    if (wifi_err != ESP_OK) {
        ESP_LOGW(NET_TAG, "WiFi unavailable; BLE remains enabled: %s",
                 esp_err_to_name(wifi_err));
    } else if (ble_err != ESP_OK) {
        ESP_LOGW(NET_TAG, "BLE unavailable; WiFi remains enabled: %s",
                 esp_err_to_name(ble_err));
    }
    return ESP_OK;
}

esp_err_t svc_wifi_scan(svc_wifi_scan_cb_t cb, void *user)
{
    if (!s_started || !s_wifi.up || cb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wifi.connecting) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t msg = { .type = MSG_WIFI_SCAN };
    msg.wifi_scan.cb = cb;
    msg.wifi_scan.user = user;
    return net_send(&msg, pdMS_TO_TICKS(100)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t svc_wifi_connect(const char *ssid, const char *password,
                           svc_wifi_conn_cb_t cb, void *user)
{
    if (!s_started || !s_wifi.up || ssid == NULL || cb == NULL ||
            ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wifi.connecting || s_wifi.connected) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t msg = { .type = MSG_WIFI_CONNECT };
    snprintf(msg.wifi_connect.ssid, sizeof(msg.wifi_connect.ssid), "%s", ssid);
    snprintf(msg.wifi_connect.password, sizeof(msg.wifi_connect.password),
             "%s", password ? password : "");
    msg.wifi_connect.cb = cb;
    msg.wifi_connect.user = user;
    return net_send(&msg, pdMS_TO_TICKS(100)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t svc_wifi_disconnect(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t msg = { .type = MSG_WIFI_DISCONNECT };
    return net_send(&msg, pdMS_TO_TICKS(100)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool svc_wifi_is_connecting(void)
{
    /* Single-writer flag (network task); a racy read is acceptable for a
     * UI convergence heuristic. */
    return s_started && s_wifi.connecting;
}

bool svc_wifi_is_connected(char *ip_out, size_t ip_len)
{
    bool connected = false;
    if (s_wifi_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(50)) == pdPASS) {
        connected = s_wifi.connected;
        if (ip_out != NULL && ip_len > 0) {
            snprintf(ip_out, ip_len, "%s", s_wifi.ip);
        }
        xSemaphoreGive(s_wifi_mutex);
    }
    return connected;
}

bool svc_net_wifi_saved(char *ssid_out, size_t ssid_cap,
                        char *pass_out, size_t pass_cap)
{
    if (ssid_out == NULL || ssid_cap == 0) {
        return false;
    }
    ssid_out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(NET_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    /* nvs_get_str's size query includes the trailing NUL. */
    size_t ssid_len = 0;
    bool ok = nvs_get_str(handle, NET_NVS_KEY_SSID, NULL, &ssid_len) ==
                  ESP_OK &&
              ssid_len >= 2 && ssid_len <= ssid_cap &&
              nvs_get_str(handle, NET_NVS_KEY_SSID, ssid_out, &ssid_len) ==
                  ESP_OK;
    if (!ok) {
        ssid_out[0] = '\0';
    }
    if (pass_out != NULL && pass_cap > 0) {
        pass_out[0] = '\0';
        size_t pass_len = 0;
        if (nvs_get_str(handle, NET_NVS_KEY_PASS, NULL, &pass_len) ==
                ESP_OK &&
                pass_len <= pass_cap) {
            nvs_get_str(handle, NET_NVS_KEY_PASS, pass_out, &pass_len);
        }
    }
    nvs_close(handle);
    return ok;
}

void svc_net_wifi_saved_forget(void)
{
    net_wifi_cred_forget();
}

bool svc_ble_is_connecting(void)
{
    /* Connecting == a connect callback slot is held but no link handle
     * exists yet. Both fields live on the network task; a racy read is
     * acceptable for the UI convergence heuristic. */
    return s_started && s_ble.conn_cb != NULL &&
           s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE;
}

bool svc_ble_is_connected(void)
{
    return s_started && s_ble.connected;
}

esp_err_t svc_ble_scan_start(svc_ble_scan_cb_t cb, void *user)
{
    if (!s_started || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ble.ready || !s_ble.synced) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ble.scanning) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t msg = { .type = MSG_BLE_SCAN_START };
    msg.ble_scan.cb = cb;
    msg.ble_scan.user = user;
    return net_send(&msg, pdMS_TO_TICKS(100)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t svc_ble_scan_stop(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t msg = { .type = MSG_BLE_SCAN_STOP };
    return net_send(&msg, pdMS_TO_TICKS(100)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t svc_ble_connect(const uint8_t addr[6], uint8_t addr_type,
                          svc_ble_conn_cb_t cb, void *user)
{
    if (!s_started || addr == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ble.ready || !s_ble.synced || s_ble.conn_cb != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t msg = { .type = MSG_BLE_CONNECT };
    memcpy(msg.ble_connect.addr, addr, 6);
    msg.ble_connect.addr_type = addr_type;
    msg.ble_connect.cb = cb;
    msg.ble_connect.user = user;
    return net_send(&msg, pdMS_TO_TICKS(100)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t svc_ble_disconnect(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t msg = { .type = MSG_BLE_DISCONNECT };
    return net_send(&msg, pdMS_TO_TICKS(100)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t svc_net_suspend_rf(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t msg = { .type = MSG_NET_RF_SUSPEND };
    return net_send(&msg, pdMS_TO_TICKS(100)) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t svc_net_resume_rf(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t msg = { .type = MSG_NET_RF_RESUME };
    return net_send(&msg, pdMS_TO_TICKS(100)) ? ESP_OK : ESP_ERR_TIMEOUT;
}
