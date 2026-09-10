/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"

#define CDC_MESSAGE_SIZE 256

typedef struct {
    uint8_t data[CDC_MESSAGE_SIZE];
    size_t length;
} cdc_message_t;

static const char *TAG = "candis_usb_cdc";
static QueueHandle_t s_cdc_queue;
static volatile bool s_dtr;
static bool s_banner_sent;
static led_indicator_handle_t s_led;
static atomic_bool s_rx_lost;

static const char *s_string_descriptor[] = {
    (const char[]){0x09, 0x04},
    "LeenixP",
    "Candis-S31 CDC Playground",
    "CANDIS-S31-CDC",
    "Candis-S31 CDC Interface",
};

static void cdc_send(const char *text)
{
    const size_t length = strlen(text);
    if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                   (const uint8_t *)text,
                                   length) == length) {
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
}

static void cdc_send_line(const char *text)
{
    cdc_send(text);
    cdc_send("\r\n");
}

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;
    cdc_message_t message = {0};
    esp_err_t error = tinyusb_cdcacm_read(itf, message.data,
                                           sizeof(message.data),
                                           &message.length);
    if (error == ESP_OK && message.length > 0 &&
            xQueueSend(s_cdc_queue, &message, 0) != pdPASS) {
        ESP_LOGE(TAG, "CDC receive queue full; input data lost");
        atomic_store(&s_rx_lost, true);
    }
}

static void cdc_line_state_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_dtr = event->line_state_changed_data.dtr != 0;
    if (!s_dtr) {
        s_banner_sent = false;
    }
    ESP_LOGI(TAG, "Host DTR=%d RTS=%d", s_dtr,
             event->line_state_changed_data.rts);
}

static void usb_device_event_callback(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "USB device %s",
             event->id == TINYUSB_EVENT_ATTACHED ? "configured" :
             event->id == TINYUSB_EVENT_DETACHED ? "detached" : "event");
}

static void send_status(void)
{
    bsp_type_c_status_t status = {0};
    const esp_err_t error = bsp_type_c_get_status(&status, false);
    if (error != ESP_OK) {
        char line[96];
        snprintf(line, sizeof(line), "status: Type-C read failed: %s",
                 esp_err_to_name(error));
        cdc_send_line(line);
        return;
    }

    const char *role = status.role == BSP_TYPE_C_ROLE_SINK ? "sink" :
                       status.role == BSP_TYPE_C_ROLE_SOURCE ? "source" :
                       status.role == BSP_TYPE_C_ROLE_DRP ? "drp" : "invalid";
    char line[160];
    snprintf(line, sizeof(line),
             "status: attached=%s vbus=%s role=%s type=0x%02x "
             "orientation=%u uptime=%lld ms",
             status.attached ? "yes" : "no", status.vbus_ok ? "yes" : "no",
             role,
             status.type, (unsigned)status.orientation,
             esp_timer_get_time() / INT64_C(1000));
    cdc_send_line(line);
}

static void process_command(char *command)
{
    if (strcmp(command, "ping") == 0) {
        cdc_send_line("pong");
    } else if (strcmp(command, "hello") == 0) {
        cdc_send_line("Hello from Candis-S31!");
    } else if (strcmp(command, "art") == 0) {
        cdc_send_line("  ___");
        cdc_send_line(" /   \\");
        cdc_send_line("| S31 |");
        cdc_send_line(" \\___/");
    } else if (strcmp(command, "status") == 0) {
        send_status();
    } else if (strcmp(command, "led on") == 0 ||
               strcmp(command, "led off") == 0) {
        const bool on = command[strlen("led ")] == 'o' &&
                        command[strlen("led ") + 1] == 'n';
        if (s_led == NULL) {
            cdc_send_line("led: unavailable");
        } else if (bsp_led_set(s_led, on) == ESP_OK) {
            cdc_send_line(on ? "led: on" : "led: off");
        } else {
            cdc_send_line("led: failed");
        }
    } else if (command[0] != '\0') {
        char echo[CDC_MESSAGE_SIZE + 16];
        snprintf(echo, sizeof(echo), "echo: %s", command);
        cdc_send_line(echo);
    }
}

/* CDC is a byte stream: a read can contain part of a line, multiple lines,
 * or all 256 bytes without a terminating NUL. Only delimiters execute commands. */
static void process_received_data(const cdc_message_t *message)
{
    static char command[CDC_MESSAGE_SIZE];
    static size_t length;
    static bool overflow;
    if (message == NULL) {
        /* Data was lost: reject the partial line until the next delimiter. */
        length = 0;
        overflow = true;
        return;
    }
    for (size_t index = 0; index < message->length; ++index) {
        const uint8_t byte = message->data[index];
        if (byte == '\r' || byte == '\n') {
            if (overflow) {
                cdc_send_line("error: invalid or oversized command (maximum 255 bytes)");
            } else if (length > 0) {
                command[length] = '\0';
                process_command(command);
            }
            length = 0;
            overflow = false;
        } else if (!overflow) {
            if (byte != '\0' && length < sizeof(command) - 1) {
                command[length++] = (char)byte;
            } else {
                overflow = true;
            }
        }
    }
}

static void send_banner(void)
{
    cdc_send_line("Candis-S31 CDC Playground");
    cdc_send_line("Commands: ping, hello, art, status, led on, led off");
    s_banner_sent = true;
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_board_init());
    ESP_ERROR_CHECK(bsp_i2c_init());
    /* The host is the Source; keep the board in Sink role and OTG 5 V off. */
    ESP_ERROR_CHECK(bsp_type_c_set_role(BSP_TYPE_C_ROLE_SINK,
                                         BSP_TYPE_C_CURRENT_DEFAULT));

    led_indicator_handle_t leds[BSP_LED_NUM] = {0};
    int led_count = 0;
    if (bsp_led_indicator_create(leds, &led_count, BSP_LED_NUM) == ESP_OK &&
            led_count == BSP_LED_NUM) {
        s_led = leds[BSP_LED_1];
    } else {
        ESP_LOGW(TAG, "RGB LED unavailable");
    }

    s_cdc_queue = xQueueCreate(8, sizeof(cdc_message_t));
    if (s_cdc_queue == NULL) {
        ESP_LOGE(TAG, "CDC queue allocation failed");
        return;
    }

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.string = s_string_descriptor;
    config.descriptor.string_count =
        sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);
    config.event_cb = usb_device_event_callback;
    ESP_ERROR_CHECK(tinyusb_driver_install(&config));

    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_callback,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc_config));

    ESP_LOGI(TAG, "USB CDC device ready; waiting for host on Type-C2");
    while (true) {
        cdc_message_t message;
        if (xQueueReceive(s_cdc_queue, &message, pdMS_TO_TICKS(100))) {
            if (atomic_exchange(&s_rx_lost, false)) {
                xQueueReset(s_cdc_queue);
                process_received_data(NULL);
                cdc_send_line("error: receive data lost; resend a complete line");
            } else {
                process_received_data(&message);
            }
        } else if (s_dtr && !s_banner_sent) {
            send_banner();
        }
    }
}
