/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_phy_cert_test.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_modules.h"

#define RF_TASK_STACK_SIZE (10 * 1024)

typedef struct {
    uint32_t channel;
    int32_t backoff;
    uint32_t length_byte;
    uint32_t packet_delay;
    uint32_t packet_num;
    esp_phy_wifi_rate_t rate;
} wifi_tx_args_t;

typedef struct {
    uint32_t channel;
    esp_phy_wifi_rate_t rate;
} wifi_rx_args_t;

typedef struct {
    uint32_t tx_power_level;
    uint32_t channel;
    uint32_t length_byte;
    uint32_t data_type;
    uint32_t syncword;
    uint32_t packet_num;
    esp_phy_ble_rate_t rate;
} ble_tx_args_t;

typedef struct {
    uint32_t channel;
    uint32_t syncword;
    esp_phy_ble_rate_t rate;
} ble_rx_args_t;

static bool s_rf_initialized;
static volatile bool s_rf_task_busy;
static volatile bool s_rf_tone_active;

static bool parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
                      uint32_t *value)
{
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_i32(const char *text, int32_t minimum, int32_t maximum,
                      int32_t *value)
{
    char *end = NULL;
    const long parsed = strtol(text, &end, 0);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (int32_t)parsed;
    return true;
}

static int command_rf_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_rf_initialized) {
        printf("rf_cert: already initialized\n");
        return ESP_OK;
    }

    esp_wifi_power_domain_on();
    esp_phy_rftest_config(1);
    esp_phy_rftest_init();
    s_rf_initialized = true;
    printf("rf_cert: initialized; use rf_stop after every TX/RX run\n");
    return ESP_OK;
}

static int command_rf_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_rf_initialized) {
        printf("rf_cert: not initialized\n");
        return ESP_OK;
    }
    esp_phy_test_start_stop(0);
    s_rf_tone_active = false;
    printf("rf_cert: TX/RX stopped\n");
    return ESP_OK;
}

static void wifi_tx_task(void *argument)
{
    const wifi_tx_args_t *args = argument;
    esp_phy_test_start_stop(3);
    esp_phy_wifi_tx(args->channel, args->rate, (int8_t)args->backoff,
                    args->length_byte, args->packet_delay, args->packet_num);
    s_rf_task_busy = false;
    vTaskDelete(NULL);
}

static void wifi_rx_task(void *argument)
{
    const wifi_rx_args_t *args = argument;
    esp_phy_test_start_stop(3);
    esp_phy_wifi_rx(args->channel, args->rate);
    s_rf_task_busy = false;
    vTaskDelete(NULL);
}

static void ble_tx_task(void *argument)
{
    const ble_tx_args_t *args = argument;
    esp_phy_test_start_stop(3);
    esp_phy_ble_tx(args->tx_power_level, args->channel, args->length_byte,
                   (esp_phy_ble_type_t)args->data_type, args->syncword,
                   args->rate, args->packet_num);
    s_rf_task_busy = false;
    vTaskDelete(NULL);
}

static void ble_rx_task(void *argument)
{
    const ble_rx_args_t *args = argument;
    esp_phy_test_start_stop(3);
    esp_phy_ble_rx(args->channel, args->syncword, args->rate);
    s_rf_task_busy = false;
    vTaskDelete(NULL);
}

static bool rf_task_ready(void)
{
    if (!s_rf_initialized) {
        printf("rf_cert: run rf_init first\n");
        return false;
    }
    if (s_rf_task_busy) {
        printf("rf_cert: another TX/RX task is active; run rf_stop and retry\n");
        return false;
    }
    if (s_rf_tone_active) {
        printf("rf_cert: a tone is active; disable it or run rf_stop first\n");
        return false;
    }
    return true;
}

static bool rf_task_start(TaskFunction_t task, const char *name, void *argument)
{
    s_rf_task_busy = true;
    if (xTaskCreate(task, name, RF_TASK_STACK_SIZE, argument, 2, NULL) != pdPASS) {
        printf("rf_cert: failed to create task\n");
        s_rf_task_busy = false;
        return false;
    }
    return true;
}

static int command_wifi_tx(int argc, char **argv)
{
    static wifi_tx_args_t args;
    if (argc != 7 ||
            !parse_u32(argv[1], 1, 14, &args.channel) ||
            !parse_u32(argv[2], 0, UINT8_MAX, (uint32_t *)&args.rate) ||
            !parse_i32(argv[3], 0, INT8_MAX, &args.backoff) ||
            !parse_u32(argv[4], 1, 4095, &args.length_byte) ||
            !parse_u32(argv[5], 0, 1000000, &args.packet_delay) ||
            !parse_u32(argv[6], 0, UINT32_MAX, &args.packet_num)) {
        printf("usage: wifi_tx CHANNEL 1-14 RATE_ID BACKOFF_QDB LENGTH_BYTE DELAY_US PACKET_NUM\n");
        printf("rate examples: 0=11b 1M, 0x0c=11g 54M, 0x17=11n MCS7, 0x29=11ax MCS9; packet_num 0=continuous\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (!rf_task_ready() || !rf_task_start(wifi_tx_task, "rf_wifi_tx", &args)) {
        return ESP_FAIL;
    }
    printf("wifi_tx: channel=%" PRIu32 " rate=0x%" PRIx32 " backoff=%" PRId32
           " length=%" PRIu32 " delay=%" PRIu32 " count=%" PRIu32 "\n",
           args.channel, (uint32_t)args.rate, args.backoff,
           args.length_byte, args.packet_delay, args.packet_num);
    return ESP_OK;
}

static int command_wifi_rx(int argc, char **argv)
{
    static wifi_rx_args_t args;
    if (argc != 3 ||
            !parse_u32(argv[1], 1, 14, &args.channel) ||
            !parse_u32(argv[2], 0, UINT8_MAX, (uint32_t *)&args.rate)) {
        printf("usage: wifi_rx CHANNEL 1-14 RATE_ID\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (!rf_task_ready() || !rf_task_start(wifi_rx_task, "rf_wifi_rx", &args)) {
        return ESP_FAIL;
    }
    printf("wifi_rx: channel=%" PRIu32 " rate=0x%" PRIx32
           "; run rf_stop before reading results\n",
           args.channel, (uint32_t)args.rate);
    return ESP_OK;
}

static int command_wifi_tone(int argc, char **argv)
{
    uint32_t enable = 0;
    uint32_t channel = 1;
    uint32_t backoff = 0;
    if (argc != 4 ||
            !parse_u32(argv[1], 0, 1, &enable) ||
            !parse_u32(argv[2], 1, 14, &channel) ||
            !parse_u32(argv[3], 0, UINT32_MAX / 4, &backoff)) {
        printf("usage: wifi_tone ENABLE 0-1 CHANNEL 1-14 BACKOFF_QDB\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (enable != 0 && !rf_task_ready()) {
        return ESP_FAIL;
    }
    if (enable != 0) {
        s_rf_tone_active = true;
    } else {
        s_rf_tone_active = false;
    }
    esp_phy_wifi_tx_tone(enable, channel, backoff);
    printf("wifi_tone: enable=%" PRIu32 " channel=%" PRIu32
           " backoff=%" PRIu32 "\n",
           enable, channel, backoff);
    return ESP_OK;
}

static int command_ble_tx(int argc, char **argv)
{
    static ble_tx_args_t args;
    if (argc != 8 ||
            !parse_u32(argv[1], 0, 15, &args.tx_power_level) ||
            !parse_u32(argv[2], 0, 39, &args.channel) ||
            !parse_u32(argv[3], 0, 255, &args.length_byte) ||
            !parse_u32(argv[4], 0, 4, &args.data_type) ||
            !parse_u32(argv[5], 0, UINT32_MAX, &args.syncword) ||
            !parse_u32(argv[6], 0, 3, (uint32_t *)&args.rate) ||
            !parse_u32(argv[7], 0, UINT32_MAX, &args.packet_num)) {
        printf("usage: ble_tx POWER_LEVEL 0-15 CHANNEL 0-39 LENGTH_BYTE TYPE SYNCWORD RATE_ID PACKET_NUM\n");
        printf("level is about (level-8)*3 dBm; 0=1M, 1=2M, 2=125k, 3=500k; packet_num 0=continuous\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (!rf_task_ready() || !rf_task_start(ble_tx_task, "rf_ble_tx", &args)) {
        return ESP_FAIL;
    }
    printf("ble_tx: level=%" PRIu32 " channel=%" PRIu32 " length=%" PRIu32
           " type=%" PRIu32 " sync=0x%08" PRIx32 " rate=%" PRIu32
           " count=%" PRIu32 "\n",
           args.tx_power_level, args.channel, args.length_byte,
           args.data_type, args.syncword, (uint32_t)args.rate, args.packet_num);
    return ESP_OK;
}

static int command_ble_rx(int argc, char **argv)
{
    static ble_rx_args_t args;
    if (argc != 4 ||
            !parse_u32(argv[1], 0, 39, &args.channel) ||
            !parse_u32(argv[2], 0, UINT32_MAX, &args.syncword) ||
            !parse_u32(argv[3], 0, 3, (uint32_t *)&args.rate)) {
        printf("usage: ble_rx CHANNEL 0-39 SYNCWORD RATE_ID\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (!rf_task_ready() || !rf_task_start(ble_rx_task, "rf_ble_rx", &args)) {
        return ESP_FAIL;
    }
    printf("ble_rx: channel=%" PRIu32 " sync=0x%08" PRIx32
           " rate=%" PRIu32 "; run rf_stop before reading results\n",
           args.channel, args.syncword, (uint32_t)args.rate);
    return ESP_OK;
}

static int command_bt_tone(int argc, char **argv)
{
    uint32_t enable = 0;
    uint32_t channel = 0;
    uint32_t backoff = 0;
    if (argc != 4 ||
            !parse_u32(argv[1], 0, 1, &enable) ||
            !parse_u32(argv[2], 0, 39, &channel) ||
            !parse_u32(argv[3], 0, UINT32_MAX / 4, &backoff)) {
        printf("usage: bt_tone ENABLE 0-1 CHANNEL 0-39 BACKOFF_QDB\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (enable != 0 && !rf_task_ready()) {
        return ESP_FAIL;
    }
    if (enable != 0) {
        s_rf_tone_active = true;
    } else {
        s_rf_tone_active = false;
    }
    esp_phy_bt_tx_tone(enable, channel, backoff);
    printf("bt_tone: enable=%" PRIu32 " channel=%" PRIu32
           " backoff=%" PRIu32 "\n",
           enable, channel, backoff);
    return ESP_OK;
}

static int command_rf_rx_result(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_phy_rx_result_t result = {0};
    esp_phy_get_rx_result(&result);
    printf("rf_rx: correct=%lu total=%lu rssi=%d flag=%lu\n",
           (unsigned long)result.phy_rx_correct_count,
           (unsigned long)result.phy_rx_total_count,
           result.phy_rx_rssi,
           (unsigned long)result.phy_rx_result_flag);
    return ESP_OK;
}

esp_err_t factory_rf_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "rf_init", .help = "Enter ESP PHY RF certification mode.", .func = command_rf_init},
        {.command = "rf_stop", .help = "Stop the current ESP PHY RF TX/RX test.", .func = command_rf_stop},
        {.command = "wifi_tx", .help = "Start Wi-Fi TX: wifi_tx CHANNEL RATE_ID BACKOFF_QDB LENGTH_BYTE DELAY_US PACKET_NUM.", .func = command_wifi_tx},
        {.command = "wifi_rx", .help = "Start Wi-Fi RX: wifi_rx CHANNEL RATE_ID.", .func = command_wifi_rx},
        {.command = "wifi_tone", .help = "Start or stop Wi-Fi CW TX: wifi_tone ENABLE CHANNEL BACKOFF_QDB.", .func = command_wifi_tone},
        {.command = "ble_tx", .help = "Start BLE TX: ble_tx POWER_LEVEL CHANNEL LENGTH_BYTE TYPE SYNCWORD RATE_ID PACKET_NUM.", .func = command_ble_tx},
        {.command = "ble_rx", .help = "Start BLE RX: ble_rx CHANNEL SYNCWORD RATE_ID.", .func = command_ble_rx},
        {.command = "bt_tone", .help = "Start or stop Bluetooth CW TX: bt_tone ENABLE CHANNEL BACKOFF_QDB.", .func = command_bt_tone},
        {.command = "rf_rx_result", .help = "Read the latest ESP PHY RF RX counters and RSSI.", .func = command_rf_rx_result},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
