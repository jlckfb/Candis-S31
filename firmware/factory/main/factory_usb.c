/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fusb303b.h"
#include "usb/usb_host.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

static int command_type_c_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_type_c_status_t status;
    const esp_err_t error = bsp_type_c_get_status(&status, true);
    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_TYPE_C, error, "FUSB303B read failed");
        return error;
    }
    printf("addr=0x%02x id=0x%02x type=0x%02x attached=%s vbus=%s "
           "safe0v=%s fault=%s remedy=%s orientation=%u role=%u "
           "peer_current=%u\n",
           status.i2c_address, status.device_id, status.device_type,
           status.attached ? "yes" : "no", status.vbus_ok ? "yes" : "no",
           status.vbus_safe_0v ? "yes" : "no",
           status.fault ? "yes" : "no",
           status.remedy_active ? "yes" : "no", status.orientation,
           (unsigned)status.role, (unsigned)status.advertised_current);

    factory_status_t result = FACTORY_STATUS_PASS;
    const char *verdict;
    if (status.device_type != FUSB303B_DEVICE_TYPE_VALUE) {
        result = FACTORY_STATUS_FAIL;
        verdict = "FUSB303B identity mismatch";
    } else if (status.role != BSP_TYPE_C_ROLE_DRP) {
        result = FACTORY_STATUS_FAIL;
        verdict = "FUSB303B is not in DRP role";
    } else if (status.fault || status.remedy_active) {
        result = FACTORY_STATUS_FAIL;
        verdict = status.fault ? "CC fault active" : "remedy state active";
    } else if (status.attached &&
               (status.orientation == 1 || status.orientation == 2)) {
        verdict = status.vbus_ok ? "cable attached, vbus ok"
                  : "cable attached, no vbus";
    } else if (!status.attached && status.orientation == 0) {
        verdict = "no cable attached";
    } else {
        result = FACTORY_STATUS_WARN;
        verdict = status.attached ? "attached but orientation unknown"
                  : "detached but orientation not cleared";
    }

    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "%s (type=0x%02x role=%u peer_current=%u fault=%u remedy=%u)",
             verdict, status.device_type, (unsigned)status.role,
             (unsigned)status.advertised_current, (unsigned)status.fault,
             (unsigned)status.remedy_active);
    factory_report_set(FACTORY_TEST_TYPE_C, result, detail);
    factory_report_print_one(FACTORY_TEST_TYPE_C);
    return result == FACTORY_STATUS_FAIL ? ESP_FAIL : ESP_OK;
}

static int command_otg(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        return bsp_usb_otg_power_set(false, BSP_TYPE_C_CURRENT_DEFAULT);
    }
    if (argc != 2 || strcmp(argv[1], "on") != 0) {
        printf("usage: otg on | otg off\n");
        return ESP_ERR_INVALID_ARG;
    }
    printf("WARNING: enabling the USB OTG boost rail; verify VBUS before connecting a load\n");
    /* Type-C2 only advertises the USB 500 mA default; high-current source
     * requests are rejected by the BSP. */
    return bsp_usb_otg_power_set(true, BSP_TYPE_C_CURRENT_DEFAULT);
}

#define USB_HOST_ENUM_TIMEOUT_S 20

static void usb_host_test_event_cb(const usb_host_client_event_msg_t *message,
                                   void *arg)
{
    if (message->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        *(volatile uint8_t *)arg = message->new_dev.address;
    }
}

/* Best-effort UTF-16LE descriptor to printable ASCII for the log line. */
static void usb_string_to_ascii(const usb_str_desc_t *descriptor, char *out,
                                size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (descriptor == NULL || descriptor->bLength < 2 ||
            (descriptor->bLength & 1U) != 0) {
        return;
    }
    const size_t chars = (descriptor->bLength - 2U) / 2U;
    size_t used = 0;
    for (size_t index = 0; index < chars && used + 1 < out_size; ++index) {
        const uint16_t code = descriptor->wData[index];
        out[used++] = code >= 0x20 && code < 0x7f ? (char)code : '?';
    }
    out[used] = '\0';
}

static int command_usb_host_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Host start also arms the Type-C2 5 V boost at the 500 mA default
     * advertisement; this board never advertises 1.5 A/3 A. Enumeration
     * below performs real control transfers on EP0, so a PASS is data-path
     * evidence, not just "5 V present". */
    esp_err_t error = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_USB_HOST, error, "USB Host start failed");
        return error;
    }

    volatile uint8_t new_address = 0;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = usb_host_test_event_cb,
            .callback_arg = (void *)&new_address,
        },
    };
    usb_host_client_handle_t client = NULL;
    if (error == ESP_OK) {
        error = usb_host_client_register(&client_config, &client);
    }

    printf("Attach a USB device to Type-C2 within %u s (500 mA budget)\n",
           USB_HOST_ENUM_TIMEOUT_S);
    usb_device_handle_t device = NULL;
    usb_device_info_t info = {0};
    const usb_device_desc_t *descriptor = NULL;
    const int64_t deadline = esp_timer_get_time() +
                             (int64_t)USB_HOST_ENUM_TIMEOUT_S * 1000000;
    while (error == ESP_OK && new_address == 0 &&
            esp_timer_get_time() < deadline) {
        const esp_err_t event_error = usb_host_client_handle_events(
                                          client, pdMS_TO_TICKS(200));
        if (event_error != ESP_OK && event_error != ESP_ERR_TIMEOUT) {
            error = event_error;
        }
    }
    if (error == ESP_OK && new_address != 0) {
        error = usb_host_device_open(client, new_address, &device);
    }
    if (error == ESP_OK && device != NULL) {
        error = usb_host_device_info(device, &info);
    }
    if (error == ESP_OK && device != NULL) {
        error = usb_host_get_device_descriptor(device, &descriptor);
    }

    char manufacturer[24] = {0};
    char product[24] = {0};
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    unsigned device_speed = 0;
    bool enumerated = false;
    if (error == ESP_OK && descriptor != NULL) {
        usb_string_to_ascii(info.str_desc_manufacturer, manufacturer,
                            sizeof(manufacturer));
        usb_string_to_ascii(info.str_desc_product, product, sizeof(product));
        vendor_id = descriptor->idVendor;
        product_id = descriptor->idProduct;
        device_speed = (unsigned)info.speed;
        enumerated = true;
        printf("addr=%u vid=0x%04x pid=0x%04x speed=%u config=%u "
               "manufacturer=\"%s\" product=\"%s\"\n",
               info.dev_addr, vendor_id, product_id, device_speed,
               (unsigned)info.bConfigurationValue, manufacturer, product);
    }

    /* Full teardown on every path: close the device, deregister the client,
     * then stop the stack and the 5 V boost. */
    if (device != NULL) {
        const esp_err_t close_error = usb_host_device_close(client, device);
        if (error == ESP_OK && close_error != ESP_OK) {
            error = close_error;
        }
    }
    if (client != NULL) {
        const esp_err_t dereg_error = usb_host_client_deregister(client);
        if (error == ESP_OK && dereg_error != ESP_OK) {
            error = dereg_error;
        }
    }
    const esp_err_t stop_error = bsp_usb_host_stop();
    if (error == ESP_OK && stop_error != ESP_OK) {
        error = stop_error;
    }

    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_USB_HOST, error, "USB Host enumeration failed");
        return error;
    }
    if (!enumerated) {
        factory_report_set(FACTORY_TEST_USB_HOST, FACTORY_STATUS_WARN,
                           "no device attached within timeout");
        factory_report_print_one(FACTORY_TEST_USB_HOST);
        return ESP_ERR_NOT_FOUND;
    }
    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail), "vid=0x%04x pid=0x%04x speed=%u",
             vendor_id, product_id, device_speed);
    factory_report_set(FACTORY_TEST_USB_HOST, FACTORY_STATUS_PASS, detail);
    factory_report_print_one(FACTORY_TEST_USB_HOST);
    return ESP_OK;
}


esp_err_t factory_usb_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "typec_test", .help = "Read FUSB303B connection state without changing its role.", .func = command_type_c_test},
        {.command = "otg", .help = "Explicitly enable or disable USB source power.", .func = command_otg},
        {.command = "usb_host_test", .help = "Install the USB Host stack and enumerate one Type-C2 device (500 mA budget).", .func = command_usb_host_test},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
