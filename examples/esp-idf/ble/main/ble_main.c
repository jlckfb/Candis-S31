/*
 * Candis-S31 BLE example (NimBLE).
 *
 * Advertises as connectable with the name "candis-example" for 10 s, then
 * actively scans for 10 s and prints up to 32 distinct peers with their
 * address, RSSI and advertised name.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdatomic.h>
#include <string.h>

#include "example_board.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "ble";

#define ADV_WINDOW_MS  10000
#define SCAN_WINDOW_MS 10000
#define DEVICE_NAME    "candis-example"
#define SEEN_MAX       32

static atomic_bool s_synced;
static atomic_bool s_scan_complete;
static uint8_t s_own_addr_type;

/* Per-session seen list for scan deduplication. */
static struct {
    uint8_t type;
    uint8_t val[6];
} s_seen[SEEN_MAX];
static unsigned s_seen_count;

static bool remember_peer(const ble_addr_t *addr)
{
    if (s_seen_count == SEEN_MAX) {
        return false;
    }
    for (unsigned index = 0; index < s_seen_count; ++index) {
        if (memcmp(s_seen[index].val, addr->val, 6) == 0 &&
                s_seen[index].type == addr->type) {
            return false;
        }
    }
    s_seen[s_seen_count].type = addr->type;
    memcpy(s_seen[s_seen_count].val, addr->val, 6);
    ++s_seen_count;
    return true;
}

static void print_peer(const struct ble_gap_disc_desc *disc)
{
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
             disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
             disc->addr.val[2], disc->addr.val[1], disc->addr.val[0]);
    char name[32] = "-";
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0 &&
            fields.name != NULL && fields.name_len > 0) {
        size_t length = fields.name_len;
        if (length > sizeof(name) - 1) {
            length = sizeof(name) - 1;
        }
        memcpy(name, fields.name, length);
        name[length] = '\0';
    }
    ESP_LOGI(TAG, "peer %s rssi %d name \"%s\"", address, disc->rssi, name);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "central connected (handle %d)",
                     event->connect.conn_handle);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason %d", event->disconnect.reason);
        return 0;
    case BLE_GAP_EVENT_DISC:
        if (remember_peer(&event->disc.addr)) {
            print_peer(&event->disc);
        }
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "scan complete: listed %u peers (limit %u)",
                 s_seen_count, (unsigned)SEEN_MAX);
        atomic_store(&s_scan_complete, true);
        return 0;
    default:
        return 0;
    }
}

static bool advertise_start(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    const int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields: %d", rc);
        return false;
    }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc2 = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                                      &params, gap_event_cb, NULL);
    if (rc2 != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start: %d", rc2);
        return false;
    }
    ESP_LOGI(TAG, "advertising as \"%s\" for 10 s", DEVICE_NAME);
    return true;
}

static bool scan_start(void)
{
    s_seen_count = 0;
    atomic_store(&s_scan_complete, false);
    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.itvl = 0x0010; /* N * 0.625 ms */
    params.window = 0x0010;
    params.filter_policy = 0;
    params.limited = 0;
    params.passive = 0; /* active scan: fetch scan-response names */
    const int rc = ble_gap_disc(s_own_addr_type, SCAN_WINDOW_MS, &params,
                                gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc: %d", rc);
        return false;
    }
    ESP_LOGI(TAG, "scanning for 10 s");
    return true;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto: %d", rc);
        return;
    }
    atomic_store(&s_synced, true);
    ESP_LOGI(TAG, "BLE host synced");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
    atomic_store(&s_synced, false);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    }));

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_device_name_set(DEVICE_NAME);
    nimble_port_freertos_init(host_task);

    while (!atomic_load(&s_synced)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!advertise_start()) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ADV_WINDOW_MS));
    ble_gap_adv_stop();

    if (!scan_start()) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(SCAN_WINDOW_MS + 2000));
    if (!atomic_load(&s_scan_complete)) {
        ESP_LOGE(TAG, "scan completion event missing");
        return;
    }

    ESP_LOGI(TAG, "advertise + scan cycle done");
}
