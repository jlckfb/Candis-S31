/*
 * Candis-S31 watch demo - NETWORK domain test suite.
 *
 * WiFi scan/connect and BLE scan smoke tests on top of svc_net (spec
 * E.4). svc_net is asynchronous and serialized internally; each test
 * wraps one operation with a binary semaphore and blocks on the svc_test
 * task, checking the cancellation flag between steps (spec C.3).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "services/svc_net.h"
#include "test_registry.h"

static void set_result(test_result_t *out, test_status_t st,
                       const char *evidence)
{
    out->st = st;
    snprintf(out->evidence, sizeof(out->evidence), "%s", evidence);
}

/* ------------------------------------------------------------------ */
/* WiFi scan (factory wifi_scan semantics, via svc_net)                 */
/* ------------------------------------------------------------------ */

typedef struct {
    SemaphoreHandle_t done;
    int count;
    int best_rssi;
    char best_ssid[33];
} wifi_scan_sync_t;

/* Runs on the network task. */
static void wifi_scan_cb(const svc_wifi_ap_t *aps, int count, void *user)
{
    wifi_scan_sync_t *sync = (wifi_scan_sync_t *)user;
    sync->count = count;
    if (count > 0) { /* sorted by RSSI, best first */
        sync->best_rssi = aps[0].rssi;
        snprintf(sync->best_ssid, sizeof(sync->best_ssid), "%s",
                 aps[0].ssid);
    }
    xSemaphoreGive(sync->done);
}

static void test_wifi_scan(const test_ctx_t *ctx, test_result_t *out)
{
    wifi_scan_sync_t sync;
    memset(&sync, 0, sizeof(sync));
    sync.done = xSemaphoreCreateBinary();
    if (sync.done == NULL) {
        set_result(out, TEST_ST_FAIL, "no memory for sync");
        return;
    }
    ctx->progress(ctx, -1, "Scanning WiFi");
    const esp_err_t err = svc_wifi_scan(wifi_scan_cb, &sync);
    if (err == ESP_ERR_INVALID_STATE) {
        set_result(out, TEST_ST_SKIP, "net busy");
    } else if (err != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "scan request failed: %s", esp_err_to_name(err));
    } else if (xSemaphoreTake(sync.done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        set_result(out, TEST_ST_FAIL, "scan timeout");
    } else if (sync.count == 0) {
        set_result(out, TEST_ST_WARN, "no APs found");
    } else {
        out->st = TEST_ST_PASS;
        snprintf(out->evidence, sizeof(out->evidence),
                 "%d APs, best %d dBm (%.16s)", sync.count, sync.best_rssi,
                 sync.best_ssid);
    }
    vSemaphoreDelete(sync.done);
}

/* ------------------------------------------------------------------ */
/* WiFi connect (saved credentials; WiFi app fallback hint)             */
/* ------------------------------------------------------------------ */

typedef struct {
    SemaphoreHandle_t done;
    svc_wifi_event_t ev;
    char detail[64];
} wifi_conn_sync_t;

/* Runs on the network task. */
static void wifi_conn_cb(svc_wifi_event_t ev, const char *detail,
                         void *user)
{
    wifi_conn_sync_t *sync = (wifi_conn_sync_t *)user;
    sync->ev = ev;
    if (detail != NULL) {
        snprintf(sync->detail, sizeof(sync->detail), "%s", detail);
    }
    xSemaphoreGive(sync->done);
}

static void test_wifi_connect(const test_ctx_t *ctx, test_result_t *out)
{
    char ssid[33] = {0};
    char pass[64] = {0};
    bool have = svc_net_wifi_saved(ssid, sizeof(ssid), pass, sizeof(pass));
    if (!have) {
        bool timed_out = false;
        ctx->ask_operator(ctx,
                          "No saved WiFi. Set it up in the WiFi app, then retry?",
                          20000, &timed_out);
        have = svc_net_wifi_saved(ssid, sizeof(ssid), pass, sizeof(pass));
    }
    if (!have) {
        set_result(out, TEST_ST_SKIP, "no saved credentials (use WiFi app)");
        return;
    }

    char ip[16] = {0};
    wifi_ap_record_t ap = {0};
    if (svc_wifi_is_connected(ip, sizeof(ip))) {
        /* Already associated (e.g. the WiFi app connected earlier): the
         * data path is proven, report the live link as evidence. */
        out->st = TEST_ST_PASS;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(out->evidence, sizeof(out->evidence),
                     "already on %.16s ip %s rssi %d", ssid, ip, ap.rssi);
        } else {
            snprintf(out->evidence, sizeof(out->evidence),
                     "already on %.16s ip %s", ssid, ip);
        }
        return;
    }

    wifi_conn_sync_t sync;
    memset(&sync, 0, sizeof(sync));
    sync.done = xSemaphoreCreateBinary();
    if (sync.done == NULL) {
        set_result(out, TEST_ST_FAIL, "no memory for sync");
        return;
    }
    char stage[48];
    snprintf(stage, sizeof(stage), "Connecting to %.24s", ssid);
    ctx->progress(ctx, -1, stage);
    const esp_err_t err = svc_wifi_connect(ssid, pass, wifi_conn_cb, &sync);
    if (err == ESP_ERR_INVALID_STATE) {
        set_result(out, TEST_ST_SKIP, "net busy");
    } else if (err != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "connect request failed: %s", esp_err_to_name(err));
    } else if (xSemaphoreTake(sync.done, pdMS_TO_TICKS(25000)) != pdTRUE) {
        set_result(out, TEST_ST_FAIL, "connect timeout (25 s)");
    } else if (sync.ev == SVC_WIFI_EV_CONNECTED &&
               svc_wifi_is_connected(ip, sizeof(ip))) {
        out->st = TEST_ST_PASS;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(out->evidence, sizeof(out->evidence),
                     "ip %s rssi %d dBm", ip, ap.rssi);
        } else {
            snprintf(out->evidence, sizeof(out->evidence), "ip %s", ip);
        }
    } else {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "connect failed: %.40s",
                 sync.detail[0] != '\0' ? sync.detail : "unknown reason");
    }
    vSemaphoreDelete(sync.done);
}

/* ------------------------------------------------------------------ */
/* BLE smoke + 10 s scan                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int count;
    int best_rssi;
} ble_scan_sync_t;

/* Runs on the network task; svc_net de-duplicates per scan session. */
static void ble_scan_cb(const svc_ble_dev_t *dev, void *user)
{
    ble_scan_sync_t *sync = (ble_scan_sync_t *)user;
    sync->count++;
    if (sync->count == 1 || dev->rssi > sync->best_rssi) {
        sync->best_rssi = dev->rssi;
    }
}

static void test_ble_smoke(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->progress(ctx, -1, "BLE scan start/stop");
    ble_scan_sync_t sync = { .count = 0, .best_rssi = -127 };
    esp_err_t err = svc_ble_scan_start(ble_scan_cb, &sync);
    if (err == ESP_ERR_INVALID_STATE) {
        set_result(out, TEST_ST_SKIP, "BLE busy");
        return;
    }
    if (err != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "scan start failed: %s", esp_err_to_name(err));
        return;
    }
    for (int ms = 0; ms < 1500 && !ctx->cancel_requested(ctx); ms += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const esp_err_t stop_err = svc_ble_scan_stop();
    if (ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records SKIP "aborted" */
    }
    if (stop_err != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "scan stop failed: %s", esp_err_to_name(stop_err));
        return;
    }
    out->st = TEST_ST_PASS;
    snprintf(out->evidence, sizeof(out->evidence),
             "start/stop ok (%d seen)", sync.count);
}

static void test_ble_scan(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->progress(ctx, 0, "BLE scanning 10 s");
    ble_scan_sync_t sync = { .count = 0, .best_rssi = -127 };
    esp_err_t err = svc_ble_scan_start(ble_scan_cb, &sync);
    if (err == ESP_ERR_INVALID_STATE) {
        set_result(out, TEST_ST_SKIP, "BLE busy");
        return;
    }
    if (err != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "scan start failed: %s", esp_err_to_name(err));
        return;
    }
    bool cancelled = false;
    for (int ms = 0; ms < 10000; ms += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (ctx->cancel_requested(ctx)) {
            cancelled = true;
            break;
        }
        ctx->progress(ctx, (ms + 200) / 100, "BLE scanning 10 s");
    }
    svc_ble_scan_stop();
    if (cancelled) {
        return; /* NOT_RUN -> runner records SKIP "aborted" */
    }
    if (sync.count == 0) {
        set_result(out, TEST_ST_WARN, "no BLE devices found");
        return;
    }
    out->st = TEST_ST_PASS;
    snprintf(out->evidence, sizeof(out->evidence), "%d devices, best %d dBm",
             sync.count, sync.best_rssi);
}

/* ------------------------------------------------------------------ */

void test_net_register(void)
{
    static const test_case_t cases[] = {
        {
            .id = "net.wifi_scan", .name = "WiFi scan",
            .domain = TEST_DOM_NETWORK, .flags = 0,
            .timeout_ms = 20000, .run = test_wifi_scan,
        },
        {
            .id = "net.wifi_connect", .name = "WiFi connect",
            .domain = TEST_DOM_NETWORK, .flags = TEST_F_INTERACTIVE,
            .timeout_ms = 60000, .run = test_wifi_connect,
        },
        {
            .id = "net.ble_smoke", .name = "BLE ctrl smoke",
            .domain = TEST_DOM_NETWORK, .flags = 0,
            .timeout_ms = 10000, .run = test_ble_smoke,
        },
        {
            .id = "net.ble_scan", .name = "BLE scan 10s",
            .domain = TEST_DOM_NETWORK, .flags = TEST_F_LONG,
            .timeout_ms = 20000, .run = test_ble_scan,
        },
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
            ++index) {
        test_register(&cases[index]);
    }
}
