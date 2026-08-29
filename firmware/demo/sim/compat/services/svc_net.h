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

/**
 * Start the shared network task. A return error means both WiFi and BLE were
 * unavailable; a single-capability failure is logged and the other stack is
 * still usable. Capability state is fixed until reboot.
 */
esp_err_t svc_net_start(void);

/** Async scan; cb receives a sorted-by-RSSI list (max 20). One at a time. */
esp_err_t svc_wifi_scan(svc_wifi_scan_cb_t cb, void *user);

esp_err_t svc_wifi_connect(const char *ssid, const char *password,
                           svc_wifi_conn_cb_t cb, void *user);
esp_err_t svc_wifi_disconnect(void);

/** true while a connect attempt is in flight (between kickoff and the
 * CONNECTED/FAILED outcome). */
bool svc_wifi_is_connecting(void);

/** true when connected; ip_out (if non-NULL) receives "192.168.x.x". */
bool svc_wifi_is_connected(char *ip_out, size_t ip_len);

/**
 * Credentials persisted after a successful connection (NVS namespace
 * "demo", keys wifi_ssid/wifi_pass). Returns true when a saved SSID was
 * loaded into ssid_out (pass_out receives the password, "" for open
 * networks; either pointer may be NULL to skip). Buffers: SSID 33, password
 * 64 (63 + NUL).
 */
bool svc_net_wifi_saved(char *ssid_out, size_t ssid_cap,
                        char *pass_out, size_t pass_cap);

/** Erase the saved credentials (explicit user "forget"). */
void svc_net_wifi_saved_forget(void);

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

/** true while a connect attempt is in flight (callback slot held, no link
 * yet). */
bool svc_ble_is_connecting(void);

/** true once a link is established and CONNECTED was reported. */
bool svc_ble_is_connected(void);

/** Connect by address; discovery runs automatically, then SERVICES_DONE. */
esp_err_t svc_ble_connect(const uint8_t addr[6], uint8_t addr_type,
                          svc_ble_conn_cb_t cb, void *user);
esp_err_t svc_ble_disconnect(void);

/* ---------------- RF suspend/resume (screen-off orchestration) ---------------- */

/**
 * Park the radios for the screen-off period: abort any WiFi connect/link
 * without callbacks, stop the WiFi driver, and cancel a running BLE scan.
 * Serialized on the network task through its command queue; safe from any
 * task. Link events trailing the shutdown are absorbed by the service -
 * they never reach the UI while the screen is dark.
 */
esp_err_t svc_net_suspend_rf(void);

/**
 * Undo svc_net_suspend_rf(): restart the WiFi driver and, when NVS
 * credentials exist, reconnect (no callback, so a failing AP stays
 * silent on wake); restart the BLE scan exactly when one was running at
 * suspend time, with its original callback.
 */
esp_err_t svc_net_resume_rf(void);

#ifdef __cplusplus
}
#endif

/* ---------------- Simulator extensions ---------------- */

/**
 * Inject preview state (e.g. from a URL state parameter): force the WiFi
 * link and/or a BLE scan to appear active. Not part of the firmware API.
 */
void sim_net_set_state(bool wifi_connected, bool ble_scanning);
