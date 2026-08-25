/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif
#if CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#endif

#include "factory_console.h"
#include "factory_peripherals.h"
#include "factory_report.h"

#define FACTORY_EXPECTED_FLASH_SIZE (16U * 1024U * 1024U)
#define FACTORY_PSRAM_TEST_SIZE     (64U * 1024U)
#define FACTORY_I2C_PROBE_TIMEOUT_MS 20

#ifndef CANDIS_S31_BSP_GIT_REV
#define CANDIS_S31_BSP_GIT_REV "unknown"
#endif

/* GPIO0 (TF card-detect) level sampled by app_main before the console
 * started; -1 until then. */
static int s_sd_detect_boot_level = -1;

void factory_console_note_sd_detect_boot_level(int level)
{
    s_sd_detect_boot_level = level;
}

static const char *sd_detect_boot_level_text(void)
{
    return s_sd_detect_boot_level < 0 ? "unsampled" :
           s_sd_detect_boot_level ? "high" : "low";
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return "other";
    }
}

static int command_board_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    uint8_t base_mac[6] = {0};
    esp_chip_info(&chip_info);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_err_t flash_err = esp_flash_get_size(NULL, &flash_size);
    const esp_err_t mac_err = esp_read_mac(base_mac, ESP_MAC_WIFI_STA);

    char mac_text[18] = "unavailable";
    if (mac_err == ESP_OK) {
        snprintf(mac_text, sizeof(mac_text), "%02x:%02x:%02x:%02x:%02x:%02x",
                 base_mac[0], base_mac[1], base_mac[2],
                 base_mac[3], base_mac[4], base_mac[5]);
    }

    printf("board=%s\n", BSP_BOARD_NAME);
    printf("board_revision=%s\n", BSP_BOARD_REVISION);
    printf("idf_target=%s\n", CONFIG_IDF_TARGET);
    printf("idf_version=%s\n", app->idf_ver);
    printf("app_version=%s\n", app->version);
    printf("bsp_revision=%s\n", CANDIS_S31_BSP_GIT_REV);
    printf("base_mac=%s\n", mac_text);
    printf("cores=%u\n", (unsigned)chip_info.cores);
    printf("silicon_revision=%u.%u\n",
           (unsigned)(chip_info.revision / 100),
           (unsigned)(chip_info.revision % 100));
    printf("reset_reason=%s\n", reset_reason_name(esp_reset_reason()));
    printf("sd_detect_boot_level=%s\n", sd_detect_boot_level_text());
    printf("flash_bytes=%" PRIu32 "\n", flash_err == ESP_OK ? flash_size : 0);
    printf("psram_bytes=%zu\n", esp_psram_get_size());
    printf("free_internal_heap=%zu\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    fputs("FACTORY_INFO {\"board\":", stdout);
    factory_report_print_json_string(BSP_BOARD_NAME);
    fputs(",\"board_revision\":", stdout);
    factory_report_print_json_string(BSP_BOARD_REVISION);
    fputs(",\"target\":", stdout);
    factory_report_print_json_string(CONFIG_IDF_TARGET);
    fputs(",\"idf\":", stdout);
    factory_report_print_json_string(app->idf_ver);
    fputs(",\"app\":", stdout);
    factory_report_print_json_string(app->version);
    fputs(",\"bsp\":", stdout);
    factory_report_print_json_string(CANDIS_S31_BSP_GIT_REV);
    fputs(",\"mac\":", stdout);
    factory_report_print_json_string(mac_text);
    fputs(",\"reset\":", stdout);
    factory_report_print_json_string(reset_reason_name(esp_reset_reason()));
    fputs(",\"sd_detect_boot_level\":", stdout);
    factory_report_print_json_string(sd_detect_boot_level_text());
    fputs("}\n", stdout);
    return ESP_OK;
}

char factory_console_ask_operator(const char *test_name, const char *question,
                                  unsigned timeout_s)
{
    /* Discard anything typed while the test itself was running. */
    char discard[32];
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval no_wait = {.tv_sec = 0, .tv_usec = 0};
        if (select(STDIN_FILENO + 1, &read_set, NULL, NULL, &no_wait) <= 0) {
            break;
        }
        if (read(STDIN_FILENO, discard, sizeof(discard)) <= 0) {
            break;
        }
    }

    fputs("FACTORY_PROMPT {\"test\":", stdout);
    factory_report_print_json_string(test_name);
    fputs(",\"question\":", stdout);
    factory_report_print_json_string(question);
    printf(",\"timeout_s\":%u}\n", timeout_s);
    printf("%s [y=yes n=no s=skip, %us] ", question, timeout_s);
    fflush(stdout);

    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    while (esp_timer_get_time() < deadline) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval slice = {.tv_sec = 0, .tv_usec = 100000};
        if (select(STDIN_FILENO + 1, &read_set, NULL, NULL, &slice) <= 0) {
            continue;
        }
        char answer = '\0';
        if (read(STDIN_FILENO, &answer, 1) != 1) {
            continue;
        }
        switch (answer) {
        case 'y':
        case 'Y':
        case 'n':
        case 'N':
        case 's':
        case 'S':
            answer = (char)(answer | 0x20);
            printf("%c\n", answer);
            fflush(stdout);
            return answer;
        default:
            break;
        }
    }
    printf("no answer within %us\n", timeout_s);
    return 0;
}

esp_err_t factory_console_ensure_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
            error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() == ESP_OK) {
            error = nvs_flash_init();
        }
    }
    return error;
}

static int command_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t error = factory_console_ensure_nvs();
    if (error == ESP_OK) {
        /* esp_netif_deinit() is not supported by esp-netif, so the stack is
         * left up; a repeat esp_netif_init() is a harmless no-op. */
        error = esp_netif_init();
    }
    bool event_loop_created = false;
    if (error == ESP_OK) {
        error = esp_event_loop_create_default();
        if (error == ESP_ERR_INVALID_STATE) {
            error = ESP_OK; /* another test already created the loop */
        } else {
            event_loop_created = error == ESP_OK;
        }
    }
    esp_netif_t *station = NULL;
    if (error == ESP_OK) {
        station = esp_netif_create_default_wifi_sta();
        if (station == NULL) {
            error = ESP_FAIL;
        }
    }
    bool started = false;
    bool wifi_inited = false;
    if (error == ESP_OK) {
        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        error = esp_wifi_init(&init_config);
        wifi_inited = error == ESP_OK;
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
        started = error == ESP_OK;
    }

    uint16_t ap_count = 0;
    if (error == ESP_OK) {
        error = esp_wifi_scan_start(NULL, true);
    }
    if (error == ESP_OK) {
        error = esp_wifi_scan_get_ap_num(&ap_count);
    }
    wifi_ap_record_t *records = NULL;
    if (error == ESP_OK && ap_count > 0) {
        const uint16_t listed = ap_count < 10 ? ap_count : 10;
        records = calloc(listed, sizeof(*records));
        uint16_t fetched = listed;
        if (records != NULL &&
                esp_wifi_scan_get_ap_records(&fetched, records) == ESP_OK) {
            for (uint16_t index = 0; index < fetched; ++index) {
                /* ssid is a fixed 33-byte field, not always NUL-terminated */
                printf("ap[%u] ssid=%.*s rssi=%d channel=%u\n", index, 32,
                       (const char *)records[index].ssid, records[index].rssi,
                       records[index].primary);
            }
        }
        free(records);
    }
    if (started) {
        esp_wifi_stop();
    }
    if (wifi_inited) {
        esp_wifi_deinit();
    }
    if (station != NULL) {
        esp_netif_destroy_default_wifi(station);
    }
    if (event_loop_created) {
        /* wifi deinit and the netif destroy above unregister every handler
         * this command put on the default loop, so the loop itself can be
         * deleted; a later wifi_scan then starts from the same clean state
         * as the first call. */
        esp_event_loop_delete_default();
    }

    char detail[64];
    if (error != ESP_OK) {
        snprintf(detail, sizeof(detail), "scan failed: %s", esp_err_to_name(error));
        factory_report_set(FACTORY_TEST_WIFI, FACTORY_STATUS_FAIL, detail);
        factory_report_print_one(FACTORY_TEST_WIFI);
        return error;
    }
    snprintf(detail, sizeof(detail), "aps_found=%u", ap_count);
    factory_report_set(FACTORY_TEST_WIFI,
                       ap_count > 0 ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_WIFI);
    return ap_count > 0 ? ESP_OK : ESP_FAIL;
}

static int command_ble_smoke(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#if CONFIG_BT_ENABLED
    esp_err_t error = factory_console_ensure_nvs();
    printf("ble: nvs=%s, controller init\n", esp_err_to_name(error));
    fflush(stdout);
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (error == ESP_OK) {
        error = esp_bt_controller_init(&config);
    }
    printf("ble: init=%s, enable(BLE mode)\n", esp_err_to_name(error));
    fflush(stdout);
    if (error == ESP_OK) {
        error = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    }
    printf("ble: enable=%s, disable\n", esp_err_to_name(error));
    fflush(stdout);
    if (error == ESP_OK) {
        esp_bt_controller_disable();
        printf("ble: disabled, deinit\n");
        fflush(stdout);
    }
    esp_bt_controller_deinit();
    printf("ble: deinit done\n");
    fflush(stdout);

    if (error != ESP_OK) {
        char detail[96];
        snprintf(detail, sizeof(detail), "BLE controller init/enable failed: %s",
                 esp_err_to_name(error));
        factory_report_set(FACTORY_TEST_BLE, FACTORY_STATUS_FAIL, detail);
        factory_report_print_one(FACTORY_TEST_BLE);
        return error;
    }
    factory_report_set(FACTORY_TEST_BLE, FACTORY_STATUS_PASS,
                       "BLE controller init enable disable deinit passed");
    factory_report_print_one(FACTORY_TEST_BLE);
    return ESP_OK;
#else
    factory_report_set(FACTORY_TEST_BLE, FACTORY_STATUS_SKIP,
                       "built without CONFIG_BT_ENABLED");
    factory_report_print_one(FACTORY_TEST_BLE);
    return ESP_OK;
#endif
}

/* ---- M5 matrix: wifi_connect / ble_scan (station + BLE observer) ---- */

static volatile bool s_wifi_got_ip;
static volatile bool s_wifi_disconnected;
static volatile int32_t s_wifi_disconnect_reason;
static esp_netif_ip_info_t s_wifi_ip_info;

static void wifi_connect_event_handler(void *arg, esp_event_base_t base,
                                       int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        s_wifi_disconnect_reason = event ? event->reason : -1;
        s_wifi_disconnected = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        if (event) {
            s_wifi_ip_info = event->ip_info;
        }
        s_wifi_got_ip = true;
    }
}

static int wifi_connect_core(const char *ssid, size_t ssid_len,
                             const char *password, int hold_seconds)
{
    if (ssid_len > 31 || strlen(password) > 63) {
        printf("ssid/password too long\n");
        return ESP_ERR_INVALID_ARG;
    }
    char ssid_print[33];
    memcpy(ssid_print, ssid, ssid_len);
    ssid_print[ssid_len] = '\0';

    esp_err_t error = factory_console_ensure_nvs();
    if (error == ESP_OK) {
        error = esp_netif_init(); /* repeat init is a harmless no-op */
    }
    bool event_loop_created = false;
    if (error == ESP_OK) {
        error = esp_event_loop_create_default();
        if (error == ESP_ERR_INVALID_STATE) {
            error = ESP_OK; /* another test already created the loop */
        } else {
            event_loop_created = error == ESP_OK;
        }
    }
    esp_netif_t *station = NULL;
    if (error == ESP_OK) {
        station = esp_netif_create_default_wifi_sta();
        if (station == NULL) {
            error = ESP_FAIL;
        }
    }
    bool wifi_inited = false;
    if (error == ESP_OK) {
        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        error = esp_wifi_init(&init_config);
        wifi_inited = error == ESP_OK;
    }
    if (error == ESP_OK) {
        error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                           wifi_connect_event_handler, NULL);
    }
    if (error == ESP_OK) {
        error = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                           wifi_connect_event_handler, NULL);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    wifi_config_t config = {0};
    if (error == ESP_OK) {
        memcpy(config.sta.ssid, ssid, ssid_len);
        memcpy(config.sta.password, password, strlen(password));
        config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
        error = esp_wifi_set_config(WIFI_IF_STA, &config);
    }

    bool connected = false;
    int64_t elapsed_ms = 0;
    if (error == ESP_OK) {
        s_wifi_got_ip = false;
        s_wifi_disconnected = false;
        s_wifi_disconnect_reason = 0;
        const int64_t start_us = esp_timer_get_time();
        error = esp_wifi_start();
        const int64_t deadline_us = start_us + 25LL * 1000 * 1000;
        while (error == ESP_OK && !s_wifi_got_ip &&
               esp_timer_get_time() < deadline_us) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
        connected = s_wifi_got_ip;
        if (connected && hold_seconds > 0) {
            printf("wifi_connect: holding connection for %d s...\n",
                   hold_seconds);
            const int64_t hold_deadline_us =
                esp_timer_get_time() + (int64_t)hold_seconds * 1000000;
            while (esp_timer_get_time() < hold_deadline_us) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
        if (connected) {
            wifi_ap_record_t ap = {0};
            const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ?
                             ap.rssi : 0;
            printf("wifi_connect: ssid=%s ip=" IPSTR " gw=" IPSTR
                   " netmask=" IPSTR " rssi=%d elapsed=%lld ms\n", ssid_print,
                   IP2STR(&s_wifi_ip_info.ip), IP2STR(&s_wifi_ip_info.gw),
                   IP2STR(&s_wifi_ip_info.netmask), rssi,
                   (long long)elapsed_ms);
        } else {
            printf("wifi_connect: %s after %lld ms (last disconnect reason=%ld)\n",
                   s_wifi_disconnected ? "disconnected" : "timeout",
                   (long long)elapsed_ms, (long)s_wifi_disconnect_reason);
        }
    }

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                 wifi_connect_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                 wifi_connect_event_handler);
    if (wifi_inited) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    if (station != NULL) {
        esp_netif_destroy_default_wifi(station);
    }
    if (event_loop_created) {
        esp_event_loop_delete_default();
    }

    char detail[96];
    if (error == ESP_OK && connected) {
        snprintf(detail, sizeof(detail), "ssid=%s ip=" IPSTR " elapsed=%lldms",
                 ssid_print, IP2STR(&s_wifi_ip_info.ip), (long long)elapsed_ms);
        factory_report_set(FACTORY_TEST_WIFI, FACTORY_STATUS_PASS, detail);
    } else {
        snprintf(detail, sizeof(detail), "connect failed: %s reason=%ld",
                 esp_err_to_name(error), (long)s_wifi_disconnect_reason);
        factory_report_set(FACTORY_TEST_WIFI, FACTORY_STATUS_FAIL, detail);
    }
    factory_report_print_one(FACTORY_TEST_WIFI);
    return connected ? ESP_OK : ESP_FAIL;
}

static int command_wifi_connect(int argc, char **argv)
{
    const int hold = argc > 3 ? atoi(argv[3]) : 0;
    if (argc < 3 || argc > 4 || hold < 0 || hold > 120) {
        printf("usage: wifi_connect SSID PASSWORD [HOLD_S 0-120]\n");
        return ESP_ERR_INVALID_ARG;
    }
    return wifi_connect_core(argv[1], strlen(argv[1]), argv[2], hold);
}

/* SSIDs with non-ASCII bytes (e.g. CJK) cannot be typed through the serial
 * console line editor intact; this variant takes the SSID as hex bytes. */
static int command_wifi_connect_hex(int argc, char **argv)
{
    const int hold = argc > 3 ? atoi(argv[3]) : 0;
    if (argc < 3 || argc > 4 || hold < 0 || hold > 120) {
        printf("usage: wifi_connect_hex SSID_HEX PASSWORD [HOLD_S 0-120]\n");
        return ESP_ERR_INVALID_ARG;
    }
    const size_t hex_len = strlen(argv[1]);
    if (hex_len == 0 || hex_len > 64 || hex_len % 2 != 0) {
        printf("ssid hex length must be even and 2..64\n");
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t ssid[32];
    for (size_t i = 0; i < hex_len / 2; ++i) {
        char byte[3] = {argv[1][2 * i], argv[1][2 * i + 1], '\0'};
        char *end = NULL;
        const long value = strtol(byte, &end, 16);
        if (end != byte + 2 || value < 0 || value > 0xff) {
            printf("invalid hex byte '%s'\n", byte);
            return ESP_ERR_INVALID_ARG;
        }
        ssid[i] = (uint8_t)value;
    }
    return wifi_connect_core((const char *)ssid, hex_len / 2, argv[2], hold);
}

#if CONFIG_BT_NIMBLE_ENABLED
static volatile bool s_ble_host_synced;
static unsigned s_ble_scan_found;

static volatile bool s_ble_host_task_done;

static void ble_scan_on_sync(void)
{
    s_ble_host_synced = true;
}

/* nimble_port_freertos_init() passes its argument straight to
 * xTaskCreatePinnedToCore() as the task entry; NULL crashes with an
 * instruction-access fault at PC=0 once the "nimble_host" task runs. */
static void ble_scan_host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns only after nimble_port_stop() */
    s_ble_host_task_done = true;
    nimble_port_freertos_deinit(); /* vTaskDelete()s this task from inside */
}

static int ble_scan_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }
    struct ble_hs_adv_fields fields;
    char name[BLE_HS_ADV_MAX_SZ] = {0};
    if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data) == 0 &&
        fields.name_len > 0) {
        const size_t copy = fields.name_len < sizeof(name) - 1 ?
                            fields.name_len : sizeof(name) - 1;
        memcpy(name, fields.name, copy);
    }
    ++s_ble_scan_found;
    printf("ble_scan: addr=%02x:%02x:%02x:%02x:%02x:%02x addr_type=%u rssi=%d name=\"%s\"\n",
           event->disc.addr.val[5], event->disc.addr.val[4],
           event->disc.addr.val[3], event->disc.addr.val[2],
           event->disc.addr.val[1], event->disc.addr.val[0],
           event->disc.addr.type, event->disc.rssi, name);
    return 0;
}

static int command_ble_scan(int argc, char **argv)
{
    const int seconds = argc > 1 ? atoi(argv[1]) : 8;
    if (seconds < 1 || seconds > 60) {
        printf("usage: ble_scan [SECONDS 1-60]\n");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = factory_console_ensure_nvs();
    /* nimble_port_init() brings up the BT controller, the VHCI transport and
     * the host in one call on this IDF (nimble_port.c); the legacy
     * esp_bt_controller/esp_nimble_hci sequence does not apply to the S31
     * esp_ipc transport and its headers are not exported there. */
    bool host_up = false;
    if (error == ESP_OK) {
        error = nimble_port_init();
        host_up = error == ESP_OK;
    }
    if (error == ESP_OK) {
        s_ble_host_synced = false;
        s_ble_host_task_done = false;
        s_ble_scan_found = 0;
        ble_hs_cfg.sync_cb = ble_scan_on_sync;
        nimble_port_freertos_init(ble_scan_host_task);
        const int64_t deadline_us = esp_timer_get_time() + 5LL * 1000 * 1000;
        while (!s_ble_host_synced && esp_timer_get_time() < deadline_us) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!s_ble_host_synced) {
            error = ESP_ERR_TIMEOUT;
        }
    }
    if (error == ESP_OK) {
        struct ble_gap_disc_params disc_params = {0};
        disc_params.passive = 0; /* active scan: also get scan response names */
        disc_params.filter_duplicates = 1;
        error = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, seconds * 1000,
                             &disc_params, ble_scan_gap_event, NULL);
    }
    if (error == ESP_OK) {
        const int64_t deadline_us =
            esp_timer_get_time() + (int64_t)seconds * 1000 * 1000 + 1500000;
        while (esp_timer_get_time() < deadline_us && ble_gap_disc_active()) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (ble_gap_disc_active()) {
            ble_gap_disc_cancel();
        }
        printf("ble_scan: done, %u unique advertiser(s) in %d s\n",
               s_ble_scan_found, seconds);
    }

    if (host_up) {
        nimble_port_stop();
        const int64_t exit_deadline_us = esp_timer_get_time() + 1000000;
        while (!s_ble_host_task_done && esp_timer_get_time() < exit_deadline_us) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        nimble_port_deinit();
    }

    char detail[96];
    if (error != ESP_OK) {
        snprintf(detail, sizeof(detail), "ble scan failed: %s",
                 esp_err_to_name(error));
        factory_report_set(FACTORY_TEST_BLE, FACTORY_STATUS_FAIL, detail);
        factory_report_print_one(FACTORY_TEST_BLE);
        return error;
    }
    snprintf(detail, sizeof(detail), "advertisers=%u in %ds",
             s_ble_scan_found, seconds);
    factory_report_set(FACTORY_TEST_BLE,
                       s_ble_scan_found > 0 ? FACTORY_STATUS_PASS : FACTORY_STATUS_WARN,
                       detail);
    factory_report_print_one(FACTORY_TEST_BLE);
    return ESP_OK;
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */

esp_err_t factory_console_capture_otp_boot_snapshot(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    esp_err_t error = bsp_pmic_init();
    if (error != ESP_OK) {
        return error;
    }

    bool truncated = false;
    size_t used = 0;
    for (int index = 0; index < BSP_PMIC_REGULATOR_COUNT; ++index) {
        const bsp_pmic_regulator_t regulator = (bsp_pmic_regulator_t)index;
        /* Candis-S31 OTP repurposes the DLDO pins as DC1SW/DC4SW. Their LDO
         * voltage registers are inert, so report them only through the
         * switch loop below instead of presenting a fake programmed voltage. */
        if (regulator == BSP_PMIC_DLDO1 || regulator == BSP_PMIC_DLDO2) {
            continue;
        }
        bool enabled = false;
        uint16_t millivolts = 0;
        error = bsp_pmic_regulator_is_enabled(regulator, &enabled);
        if (error == ESP_OK) {
            error = bsp_pmic_regulator_get_voltage(regulator, &millivolts);
        }
        if (error != ESP_OK) {
            return error;
        }
        const int written = snprintf(out + used, out_size - used, "%s%s=%s@%umV",
                                     used > 0 ? " " : "",
                                     bsp_pmic_regulator_name(regulator),
                                     enabled ? "on" : "off",
                                     (unsigned)millivolts);
        if (written < 0) {
            return ESP_FAIL;
        }
        if ((size_t)written >= out_size - used) {
            truncated = true;
            /* snprintf already truncated the output; stop appending. */
            used = out_size - 1;
            break;
        }
        used += (size_t)written;
    }
    for (int index = 0; index < BSP_PMIC_SWITCH_COUNT && !truncated; ++index) {
        const bsp_pmic_switch_t sw = (bsp_pmic_switch_t)index;
        bool enabled = false;
        error = bsp_pmic_switch_is_enabled(sw, &enabled);
        if (error != ESP_OK) {
            return error;
        }
        const int written = snprintf(out + used, out_size - used, "%s%s=%s",
                                     used > 0 ? " " : "",
                                     bsp_pmic_switch_name(sw),
                                     enabled ? "closed" : "open");
        if (written < 0) {
            return ESP_FAIL;
        }
        if ((size_t)written >= out_size - used) {
            truncated = true;
            used = out_size - 1;
            break;
        }
        used += (size_t)written;
    }
    return truncated ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static int command_otp_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* This is a live read. After boot the safe state has already disabled
     * the optional rails, so a mismatch with the OTP expectations is only
     * meaningful on the boot snapshot captured before bsp_board_init(). */
    char snapshot[FACTORY_OTP_SNAPSHOT_LENGTH];
    const esp_err_t error = factory_console_capture_otp_boot_snapshot(
                                snapshot, sizeof(snapshot));
    if (error == ESP_ERR_INVALID_SIZE) {
        printf("otp_status report truncated; buffer too small\n");
        return error;
    }
    if (error != ESP_OK) {
        printf("otp_status read failed: %s\n", esp_err_to_name(error));
        return error;
    }
    printf("TG28_SW rails: %s\n", snapshot);
    printf("note: optional rails were disabled by the boot safe state; "
           "compare with the boot-log OTP snapshot, not this live read\n");
    return ESP_OK;
}

static int command_safe_state(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const esp_err_t err = factory_peripherals_power_all_off();
    char detail[64];
    snprintf(detail, sizeof(detail), "owned activity stopped, safe state: %s",
             esp_err_to_name(err));
    factory_report_set(FACTORY_TEST_SAFE_STATE,
                       err == ESP_OK ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_SAFE_STATE);
    return err;
}

static int command_power_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    for (int domain = 0; domain < BSP_POWER_DOMAIN_COUNT; ++domain) {
        bool enabled = false;
        const esp_err_t err = bsp_power_domain_get((bsp_power_domain_t)domain, &enabled);
        printf("%-16s %s%s%s\n",
               bsp_power_domain_name((bsp_power_domain_t)domain),
               err == ESP_OK ? (enabled ? "ENABLED" : "disabled") : "unknown",
               err == ESP_OK ? "" : ": ",
               err == ESP_OK ? "" : esp_err_to_name(err));
    }
    return ESP_OK;
}

static int command_flash_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t flash_size = 0;
    const esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    char detail[64];
    if (err != ESP_OK) {
        snprintf(detail, sizeof(detail), "read failed: %s", esp_err_to_name(err));
        factory_report_set(FACTORY_TEST_FLASH, FACTORY_STATUS_FAIL, detail);
    } else {
        snprintf(detail, sizeof(detail), "size=%" PRIu32 " expected=%u",
                 flash_size, FACTORY_EXPECTED_FLASH_SIZE);
        factory_report_set(FACTORY_TEST_FLASH,
                           flash_size == FACTORY_EXPECTED_FLASH_SIZE ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                           detail);
    }
    factory_report_print_one(FACTORY_TEST_FLASH);
    return err != ESP_OK ? err :
           (flash_size == FACTORY_EXPECTED_FLASH_SIZE ? ESP_OK : ESP_FAIL);
}

static int command_psram_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!esp_psram_is_initialized()) {
        factory_report_set(FACTORY_TEST_PSRAM, FACTORY_STATUS_FAIL, "PSRAM is not initialized");
        factory_report_print_one(FACTORY_TEST_PSRAM);
        return ESP_FAIL;
    }

    uint32_t *buffer = heap_caps_malloc(FACTORY_PSRAM_TEST_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        factory_report_set(FACTORY_TEST_PSRAM, FACTORY_STATUS_FAIL, "64 KiB allocation failed");
        factory_report_print_one(FACTORY_TEST_PSRAM);
        return ESP_ERR_NO_MEM;
    }

    const size_t words = FACTORY_PSRAM_TEST_SIZE / sizeof(*buffer);
    for (size_t index = 0; index < words; ++index) {
        buffer[index] = UINT32_C(0xa5a50000) ^ (uint32_t)index;
    }

    size_t failed_index = words;
    for (size_t index = 0; index < words; ++index) {
        const uint32_t expected = UINT32_C(0xa5a50000) ^ (uint32_t)index;
        if (buffer[index] != expected) {
            failed_index = index;
            break;
        }
    }
    free(buffer);

    char detail[64];
    if (failed_index == words) {
        snprintf(detail, sizeof(detail), "tested=%u total=%zu",
                 FACTORY_PSRAM_TEST_SIZE, esp_psram_get_size());
        factory_report_set(FACTORY_TEST_PSRAM, FACTORY_STATUS_PASS, detail);
    } else {
        snprintf(detail, sizeof(detail), "mismatch_at_word=%zu", failed_index);
        factory_report_set(FACTORY_TEST_PSRAM, FACTORY_STATUS_FAIL, detail);
    }
    factory_report_print_one(FACTORY_TEST_PSRAM);
    return failed_index == words ? ESP_OK : ESP_FAIL;
}

static esp_err_t scan_i2c_bus(i2c_master_bus_handle_t handle,
                              bool addresses[128], unsigned *found)
{
    memset(addresses, 0, 128 * sizeof(addresses[0]));
    *found = 0;
    for (uint16_t address = 0x08; address <= 0x77; ++address) {
        const esp_err_t err = i2c_master_probe(handle, address, FACTORY_I2C_PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            printf("found I2C device at 0x%02x\n", address);
            addresses[address] = true;
            ++(*found);
        } else if (err != ESP_ERR_NOT_FOUND) {
            return err;
        }
    }
    return ESP_OK;
}

static int command_i2c_scan(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "main") != 0 && strcmp(argv[1], "lp") != 0)) {
        printf("usage: i2c_scan main|lp\n");
        return ESP_ERR_INVALID_ARG;
    }

    const bool use_lp_bus = strcmp(argv[1], "lp") == 0;
    const factory_test_id_t test = use_lp_bus ? FACTORY_TEST_I2C_LOW_POWER : FACTORY_TEST_I2C_MAIN;
    i2c_master_bus_handle_t handle = use_lp_bus ? bsp_lp_i2c_get_handle() : bsp_i2c_get_handle();
    if (handle == NULL) {
        factory_report_set(test, FACTORY_STATUS_FAIL, "bus initialization failed");
        factory_report_print_one(test);
        return ESP_FAIL;
    }

    unsigned found = 0;
    bool addresses[128];
    const esp_err_t err = scan_i2c_bus(handle, addresses, &found);
    char detail[96];
    if (err != ESP_OK) {
        snprintf(detail, sizeof(detail), "scan error: %s", esp_err_to_name(err));
        factory_report_set(test, FACTORY_STATUS_FAIL, detail);
        factory_report_print_one(test);
        return err;
    }
    if (use_lp_bus) {
        const bool present = addresses[BSP_RX8130CE_I2C_ADDRESS] &&
                             addresses[BSP_TG28_SW_I2C_ADDRESS];
        snprintf(detail, sizeof(detail), "devices=%u rtc=%s pmic=%s", found,
                 addresses[BSP_RX8130CE_I2C_ADDRESS] ? "yes" : "no",
                 addresses[BSP_TG28_SW_I2C_ADDRESS] ? "yes" : "no");
        factory_report_set(test, present ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                           detail);
        factory_report_print_one(test);
        return present ? ESP_OK : ESP_FAIL;
    }

    /* Main-bus expectations (hardware/bring-up.md): FUSB303B answers at
     * exactly one of 0x21/0x31 while BSP_POWER_TYPE_C_CONTROL is enabled;
     * it is expected to be silent after power_all_off. 0x31 means the
     * address strap mismatches the schematic and must be recorded. Devices
     * behind switched rails (CST820 0x15/ALDO2, ES8389 0x10/ALDO3, OV5640
     * 0x3C/camera rails) may stay silent while their rail is off; silent
     * while powered is a finding. ES8389 answers at 7-bit 0x10 on the wire
     * (AD0 and AD1 both strapped low: R63/R68 100kOhm pull-downs fitted,
     * R62/R65 DNP); 0x20 is its 8-bit write address and is what
     * esp_codec_dev and the BSP take (es8389_codec.h). This board has no
     * VCM driver, so any other answer — including the 0x0C slot a VCM
     * would use — is unexpected. */
    const bool fusb_21 = addresses[0x21];
    const bool fusb_31 = addresses[0x31];
    bool type_c_on = false;
    const esp_err_t type_c_power_error =
        bsp_power_domain_get(BSP_POWER_TYPE_C_CONTROL, &type_c_on);
    bool touch_on = false, audio_on = false;
    bool cam_dvdd = false, cam_avdd = false, cam_dovdd = false;
    /* A PMIC read failure means the rail power state is unknown, not "off":
     * a silent device behind an unreadable rail must not be graded as a
     * missing powered device. Record the failing rails and grade a WARN. */
    bool power_known = true;
    char unknown_rails[64] = {0};
    const struct {
        bsp_pmic_regulator_t reg;
        bool *enabled;
    } power_rails[] = {
        { BSP_PMIC_ALDO2, &touch_on },
        { BSP_PMIC_ALDO3, &audio_on },
        { BSP_PMIC_DCDC2, &cam_dvdd },
        { BSP_PMIC_ALDO4, &cam_avdd },
        { BSP_PMIC_BLDO1, &cam_dovdd },
    };
    for (size_t i = 0; i < sizeof(power_rails) / sizeof(power_rails[0]); ++i) {
        const esp_err_t perr = bsp_pmic_regulator_is_enabled(
            power_rails[i].reg, power_rails[i].enabled);
        if (perr != ESP_OK) {
            power_known = false;
            const size_t used = strlen(unknown_rails);
            snprintf(unknown_rails + used, sizeof(unknown_rails) - used,
                     "%s%s", used ? "," : "",
                     bsp_pmic_regulator_name(power_rails[i].reg));
        }
    }
    const bool camera_on = cam_dvdd && cam_avdd && cam_dovdd;

    unsigned missing = 0;
    if (power_known && touch_on && !addresses[0x15]) {
        missing |= 0x01;
    }
    if (power_known && audio_on && !addresses[0x10]) {
        missing |= 0x02;
    }
    if (power_known && camera_on && !addresses[0x3c]) {
        missing |= 0x04;
    }
    unsigned unexpected = 0;
    for (uint16_t address = 0x08; address <= 0x77; ++address) {
        if (addresses[address] && address != 0x15 && address != 0x10 &&
                address != 0x21 && address != 0x31 && address != 0x3c) {
            ++unexpected;
        }
    }

    factory_status_t verdict = FACTORY_STATUS_PASS;
    const char *note = type_c_on ? "expected set present"
                       : "FUSB303B off with control domain";
    if (type_c_power_error != ESP_OK) {
        verdict = FACTORY_STATUS_WARN;
        note = "Type-C control power state unknown";
    } else if (fusb_21 && fusb_31) {
        verdict = FACTORY_STATUS_FAIL;
        note = "FUSB303B at both 0x21/0x31";
    } else if (type_c_on && !fusb_21 && !fusb_31) {
        verdict = FACTORY_STATUS_FAIL;
        note = "powered FUSB303B missing";
    } else if (!type_c_on && (fusb_21 || fusb_31)) {
        verdict = FACTORY_STATUS_WARN;
        note = "FUSB303B answered while control disabled";
    } else if (fusb_31) {
        verdict = FACTORY_STATUS_WARN;
        note = "FUSB303B at 0x31: strap mismatch, record it";
    } else if (unexpected > 0) {
        verdict = FACTORY_STATUS_WARN;
        note = "unexpected address answered";
    } else if (!power_known) {
        verdict = FACTORY_STATUS_WARN;
        note = "rail power state unknown";
    } else if (missing != 0) {
        verdict = FACTORY_STATUS_WARN;
        note = "powered device silent";
    }
    /* Keep each snprintf provably within FACTORY_DETAIL_LENGTH (96) for
     * -Wformat-truncation: when a PMIC read failed the counts carry no
     * information (missing is gated by power_known), so that path reports
     * the failed rails instead of the counts. The rail list is at most
     * five 5-char names plus commas, which fits %.29s exactly. The longest
     * note is the FUSB303B 0x31 strap-mismatch finding (43 chars), so both
     * forms stay within budget: 43+36 with the counts, 43+20+29 with the
     * rails. */
    if (unknown_rails[0] != '\0') {
        snprintf(detail, sizeof(detail), "%s; PMIC read failed: %.29s",
                 note, unknown_rails);
    } else {
        snprintf(detail, sizeof(detail), "%s (devices=%u missing=0x%x extra=%u)",
                 note, found, missing, unexpected);
    }
    factory_report_set(test, verdict, detail);
    factory_report_print_one(test);
    return verdict == FACTORY_STATUS_FAIL ? ESP_FAIL : ESP_OK;
}

static int command_report(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    factory_report_print();
    return ESP_OK;
}

static int command_report_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* safe_state only runs at boot; keep its result across a manual reset. */
    factory_status_t boot_status = FACTORY_STATUS_NOT_RUN;
    char boot_detail[96] = {0};
    const bool keep = factory_report_get(FACTORY_TEST_SAFE_STATE, &boot_status,
                                         boot_detail, sizeof(boot_detail)) == ESP_OK &&
                      boot_status != FACTORY_STATUS_NOT_RUN;
    factory_report_init();
    if (keep) {
        factory_report_set(FACTORY_TEST_SAFE_STATE, boot_status, boot_detail);
    }
    printf("Factory results reset to NOT_RUN%s\n",
           keep ? " (safe_state kept: it only runs at boot)" : "");
    return ESP_OK;
}

static int command_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Restarting Candis-S31\n");
    fflush(stdout);
    esp_restart();
    return ESP_OK;
}

esp_err_t factory_console_start(void)
{
    esp_err_t err = esp_console_register_help_command();
    if (err != ESP_OK) {
        return err;
    }

    const esp_console_cmd_t commands[] = {
        {
            .command = "board_info",
            .help = "Print board, firmware, flash, and memory information.",
            .func = command_board_info,
        },
        {
            .command = "safe_state",
            .help = "Stop activity and switch every peripheral rail off safely.",
            .func = command_safe_state,
        },
        {
            .command = "power_status",
            .help = "Show direct power-domain control states.",
            .func = command_power_status,
        },
        {
            .command = "otp_status",
            .help = "Read every TG28_SW rail enable/voltage and switch state.",
            .func = command_otp_status,
        },
        {
            .command = "flash_test",
            .help = "Check the detected flash size without writing flash.",
            .func = command_flash_test,
        },
        {
            .command = "psram_test",
            .help = "Test a temporary 64 KiB PSRAM allocation.",
            .func = command_psram_test,
        },
        {
            .command = "i2c_scan",
            .help = "Scan one bus: i2c_scan main|lp.",
            .func = command_i2c_scan,
        },
        {
            .command = "wifi_scan",
            .help = "Scan for Wi-Fi access points in station mode.",
            .func = command_wifi_scan,
        },
        {
            .command = "wifi_connect",
            .help = "Join a WPA2 AP and report IP/RSSI: wifi_connect SSID PASSWORD.",
            .func = command_wifi_connect,
        },
        {.command = "wifi_connect_hex", .help = "Join a WPA2 AP with the SSID given as hex bytes: wifi_connect_hex SSID_HEX PASSWORD.", .func = command_wifi_connect_hex},
        {
            .command = "ble_smoke",
            .help = "Initialize and release the BLE controller.",
            .func = command_ble_smoke,
        },
#if CONFIG_BT_NIMBLE_ENABLED
        {
            .command = "ble_scan",
            .help = "Scan BLE advertisers: ble_scan [SECONDS 1-60].",
            .func = command_ble_scan,
        },
#endif
        {
            .command = "report",
            .help = "Print all test results and the JSON summary.",
            .func = command_report,
        },
        {
            .command = "report_reset",
            .help = "Reset every collected result to NOT_RUN.",
            .func = command_report_reset,
        },
        {
            .command = "reboot",
            .help = "Restart the SoC without erasing flash.",
            .func = command_reboot,
        },
    };

    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        err = esp_console_cmd_register(&commands[index]);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = factory_peripherals_register();
    if (err != ESP_OK) {
        return err;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "candis-factory>";
    repl_config.max_cmdline_length = 128;
    /* MSC/FATFS diagnostics make deeper calls than the default REPL stack. */
    repl_config.task_stack_size = 8192;

    esp_console_repl_t *repl = NULL;
    err = esp_console_new_repl_stdio(&repl_config, &repl);
    if (err != ESP_OK) {
        return err;
    }
    return esp_console_start_repl(repl);
}
