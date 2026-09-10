/*
 * Candis-S31 WiFi example.
 *
 * Brings the STA interface up, scans and prints the strongest APs, and —
 * when CONFIG_EXAMPLE_WIFI_SSID is set — connects and prints the IP
 * address. With an empty SSID the example stays in scan-only mode.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdatomic.h>
#include <string.h>

#include "example_board.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

#define SCAN_PRINT_COUNT 10
#define CONNECT_TIMEOUT_MS 20000

static atomic_bool s_got_ip;
static atomic_int s_disconnect_reason;
static char s_ip_string[16];

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        atomic_store(&s_disconnect_reason, ev ? ev->reason : 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        if (ev != NULL) {
            esp_ip4addr_ntoa(&ev->ip_info.ip, s_ip_string, sizeof(s_ip_string));
        }
        atomic_store(&s_got_ip, true);
    }
}

static void scan_and_print(void)
{
    ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true)); /* blocking */
    uint16_t found = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&found));
    uint16_t got = found < SCAN_PRINT_COUNT ? found : SCAN_PRINT_COUNT;
    wifi_ap_record_t records[SCAN_PRINT_COUNT];
    if (got > 0) {
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&got, records));
        for (uint16_t index = 0; index < got; ++index) {
            ESP_LOGI(TAG, "ap %2u: %-*s rssi %3d ch %2u",
                     index + 1, 32, (const char *)records[index].ssid,
                     records[index].rssi, records[index].primary);
        }
    }
    ESP_LOGI(TAG, "scan: %u APs visible, %u printed", found, got);
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    }));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() == NULL ?
                    ESP_FAIL : ESP_OK);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA up");
    scan_and_print();
    if (strlen(CONFIG_EXAMPLE_WIFI_SSID) > 0) {
        wifi_config_t wifi_config = {0};
        strlcpy((char *)wifi_config.sta.ssid, CONFIG_EXAMPLE_WIFI_SSID,
                sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, CONFIG_EXAMPLE_WIFI_PASSWORD,
                sizeof(wifi_config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        atomic_store(&s_got_ip, false);
        atomic_store(&s_disconnect_reason, 0);
        ESP_ERROR_CHECK(esp_wifi_connect());
        ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_EXAMPLE_WIFI_SSID);

        bool connected = false;
        for (int waited = 0; waited < CONNECT_TIMEOUT_MS; waited += 250) {
            if (atomic_load(&s_got_ip)) {
                connected = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (connected) {
            ESP_LOGI(TAG, "connected, ip: %s", s_ip_string);
        } else {
            const int reason = atomic_load(&s_disconnect_reason);
            ESP_LOGE(TAG, "no IP within %d ms (disconnect reason %d)",
                     CONNECT_TIMEOUT_MS, reason);
        }
    } else {
        ESP_LOGI(TAG, "no SSID configured (CONFIG_EXAMPLE_WIFI_SSID empty); scan only");
    }
}
