/*
 * Candis-S31 watch demo - network service (WiFi STA + BLE central via NimBLE).
 *
 * One network task (stack 6144, prio 4) serializes WiFi and BLE operations;
 * UI apps submit requests and receive results through the callbacks below.
 * Callbacks run on the network task context; UI work must go through
 * ui_async().
 *
 * WiFi: driver started once at svc_net_start and kept up; scans are the
 * blocking kind executed on the network task. Connect uses the IDF event
 * pattern (WIFI_EVENT/IP_EVENT), with auth-failure reasons surfaced.
 *
 * BLE: NimBLE host. Scan results are de-duplicated by address for the
 * session and delivered per device. Connect performs GATT service
 * discovery and reports the UUID list.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- WiFi ---------------- */

typedef struct {
    char ssid[33];
    int rssi;
    uint8_t authmode;  /**< wifi_auth_mode_t */
    uint8_t channel;
} svc_wifi_ap_t;

typedef enum {
    SVC_WIFI_EV_CONNECTED = 0,
    SVC_WIFI_EV_CONNECT_FAILED,  /**< detail = reason string */
    SVC_WIFI_EV_DISCONNECTED,
} svc_wifi_event_t;

typedef void (*svc_wifi_scan_cb_t)(const svc_wifi_ap_t *aps, int count, void *user);
typedef void (*svc_wifi_conn_cb_t)(svc_wifi_event_t ev, const char *detail, void *user);

esp_err_t svc_net_start(void);

/** Async scan; cb receives a sorted-by-RSSI list (max 20). One at a time. */
esp_err_t svc_wifi_scan(svc_wifi_scan_cb_t cb, void *user);

esp_err_t svc_wifi_connect(const char *ssid, const char *password,
                           svc_wifi_conn_cb_t cb, void *user);
esp_err_t svc_wifi_disconnect(void);

/** true when connected; ip_out (if non-NULL) receives "192.168.x.x". */
bool svc_wifi_is_connected(char *ip_out, size_t ip_len);

/* ---------------- BLE ---------------- */

typedef struct {
    char name[32];       /**< complete/local name, "" when absent */
    uint8_t addr[6];
    uint8_t addr_type;
    int rssi;
} svc_ble_dev_t;

typedef enum {
    SVC_BLE_EV_CONNECTED = 0,
    SVC_BLE_EV_CONNECT_FAILED,
    SVC_BLE_EV_DISCONNECTED,
    SVC_BLE_EV_SERVICES_DONE,  /**< svcs list delivered with this event */
} svc_ble_event_t;

typedef struct {
    char uuid[40];       /**< string form, 16/32/128-bit */
    uint16_t start_handle;
    uint16_t end_handle;
} svc_ble_svc_t;

typedef void (*svc_ble_scan_cb_t)(const svc_ble_dev_t *dev, void *user);
typedef void (*svc_ble_conn_cb_t)(svc_ble_event_t ev, const svc_ble_svc_t *svcs,
                                  int svc_count, void *user);

esp_err_t svc_ble_scan_start(svc_ble_scan_cb_t cb, void *user);
esp_err_t svc_ble_scan_stop(void);

/** Connect by address; discovery runs automatically, then SERVICES_DONE. */
esp_err_t svc_ble_connect(const uint8_t addr[6], uint8_t addr_type,
                          svc_ble_conn_cb_t cb, void *user);
esp_err_t svc_ble_disconnect(void);

#ifdef __cplusplus
}
#endif
