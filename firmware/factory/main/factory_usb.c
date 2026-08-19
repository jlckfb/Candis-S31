/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fusb303b.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

#define TYPE_C_ATTACH_SETTLE_MS  1000
#define TYPE_C_POLL_INTERVAL_MS  50

static const char *type_c_role_name(bsp_type_c_role_t role)
{
    switch (role) {
    case BSP_TYPE_C_ROLE_DISABLED: return "disabled";
    case BSP_TYPE_C_ROLE_SINK: return "sink";
    case BSP_TYPE_C_ROLE_SOURCE: return "source";
    case BSP_TYPE_C_ROLE_DRP: return "drp";
    default: return "invalid";
    }
}

static const char *type_c_current_name(bsp_type_c_current_t current)
{
    switch (current) {
    case BSP_TYPE_C_CURRENT_1_5_A: return "1.5A";
    case BSP_TYPE_C_CURRENT_3_0_A: return "3.0A";
    case BSP_TYPE_C_CURRENT_DEFAULT:
    default: return "default";
    }
}

static esp_err_t type_c_read_settled_status(bsp_type_c_status_t *status)
{
    esp_err_t error = bsp_type_c_get_status(status, false);
    const int64_t deadline = esp_timer_get_time() +
                             TYPE_C_ATTACH_SETTLE_MS * INT64_C(1000);
    while (error == ESP_OK && !status->attached &&
            esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(TYPE_C_POLL_INTERVAL_MS));
        error = bsp_type_c_get_status(status, false);
    }
    if (error == ESP_OK) {
        error = bsp_type_c_get_status(status, true);
    }
    return error;
}

static int command_type_c_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_type_c_status_t status;
    const esp_err_t error = type_c_read_settled_status(&status);
    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_TYPE_C, error, "FUSB303B read failed");
        return error;
    }
    printf("addr=0x%02x id=0x%02x device_type=0x%02x type=0x%02x "
           "attached=%s vbus=%s "
           "safe0v=%s fault=%s remedy=%s orientation=%u role=%s "
           "peer_current=%s\n",
           status.i2c_address, status.device_id, status.device_type,
           status.type, status.attached ? "yes" : "no",
           status.vbus_ok ? "yes" : "no",
           status.vbus_safe_0v ? "yes" : "no",
           status.fault ? "yes" : "no",
           status.remedy_active ? "yes" : "no", status.orientation,
           type_c_role_name(status.role),
           type_c_current_name(status.advertised_current));

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
             "%s (type=0x%02x role=%s peer_current=%s fault=%u remedy=%u)",
             verdict, status.type, type_c_role_name(status.role),
             type_c_current_name(status.advertised_current),
             (unsigned)status.fault, (unsigned)status.remedy_active);
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
#define USB_MSC_TEST_DEFAULT_SECONDS 300
#define USB_MSC_TEST_MIN_SECONDS    10
#define USB_MSC_TEST_MAX_SECONDS    1800
#define USB_MSC_BUFFER_SIZE         8192
#define USB_MSC_MAX_TEST_BYTES      (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define USB_MSC_MIN_FREE_BYTES      (16ULL * 1024ULL * 1024ULL)
#define USB_MSC_FREE_RESERVE_BYTES  (8ULL * 1024ULL * 1024ULL)
#define USB_MSC_MOUNT_PATH          "/usb0"
#define USB_MSC_TEST_PATH           USB_MSC_MOUNT_PATH "/CANDIS_USB_STRESS.BIN"

static volatile bool s_msc_connected;
static volatile bool s_msc_disconnected;
static volatile uint8_t s_msc_device_address;

static void usb_host_test_event_cb(const usb_host_client_event_msg_t *message,
                                   void *arg)
{
    if (message->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        *(volatile uint8_t *)arg = message->new_dev.address;
    }
}

static void usb_msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (event->event == MSC_DEVICE_CONNECTED) {
        s_msc_device_address = event->device.address;
        s_msc_connected = true;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        s_msc_connected = false;
        s_msc_disconnected = true;
    }
}

static void usb_msc_fill_block(uint8_t *data, uint32_t block)
{
    uint32_t state = UINT32_C(0x43414e44) ^ (block * UINT32_C(0x9e3779b1));
    for (size_t index = 0; index < USB_MSC_BUFFER_SIZE; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[index] = (uint8_t)state;
    }
}

static int command_usb_msc_test(int argc, char **argv)
{
    long seconds = USB_MSC_TEST_DEFAULT_SECONDS;
    bool overwrite = false;
    if (argc > 3) {
        printf("usage: usb_msc_test [SECONDS %d-%d] [overwrite]\n",
               USB_MSC_TEST_MIN_SECONDS, USB_MSC_TEST_MAX_SECONDS);
        return ESP_ERR_INVALID_ARG;
    }
    if (argc >= 2) {
        char *end = NULL;
        seconds = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' ||
                seconds < USB_MSC_TEST_MIN_SECONDS ||
                seconds > USB_MSC_TEST_MAX_SECONDS) {
            printf("usage: usb_msc_test [SECONDS %d-%d] [overwrite]\n",
                   USB_MSC_TEST_MIN_SECONDS, USB_MSC_TEST_MAX_SECONDS);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (argc == 3) {
        if (strcmp(argv[2], "overwrite") != 0) {
            printf("usage: usb_msc_test [SECONDS %d-%d] [overwrite]\n",
                   USB_MSC_TEST_MIN_SECONDS, USB_MSC_TEST_MAX_SECONDS);
            return ESP_ERR_INVALID_ARG;
        }
        overwrite = true;
    }

    esp_err_t error = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    msc_host_device_handle_t device = NULL;
    msc_host_vfs_handle_t vfs = NULL;
    FILE *file = NULL;
    uint8_t *buffer = NULL;
    uint8_t *expected = NULL;
    uint32_t blocks = 0;
    uint64_t bytes = 0;
    uint64_t max_test_bytes = 0;
    int64_t write_end_us = 0;
    int64_t write_start_us = 0;
    bool msc_driver_installed = false;

    const msc_host_driver_config_t driver_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = usb_msc_event_cb,
    };
    if (error == ESP_OK) {
        s_msc_connected = false;
        s_msc_disconnected = false;
        s_msc_device_address = 0;
        error = msc_host_install(&driver_config);
        msc_driver_installed = error == ESP_OK;
    }

    const int64_t connect_deadline = esp_timer_get_time() +
                                      USB_HOST_ENUM_TIMEOUT_S * INT64_C(1000000);
    while (error == ESP_OK && !s_msc_connected && !s_msc_disconnected &&
            esp_timer_get_time() < connect_deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (error == ESP_OK && !s_msc_connected) {
        error = s_msc_disconnected ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    if (error == ESP_OK) {
        error = msc_host_install_device(s_msc_device_address, &device);
    }

    msc_host_device_info_t info = {0};
    if (error == ESP_OK) {
        error = msc_host_get_device_info(device, &info);
    }
    if (error == ESP_OK) {
        const esp_vfs_fat_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 1,
            .allocation_unit_size = 0,
            .disk_status_check_enable = true,
            .use_one_fat = false,
        };
        error = msc_host_vfs_register(device, USB_MSC_MOUNT_PATH,
                                      &mount_config, &vfs);
    }
    if (error == ESP_OK) {
        printf("msc: vid=0x%04x pid=0x%04x sector=%lu count=%lu "
               "capacity=%llu MiB\n",
               info.idVendor, info.idProduct, (unsigned long)info.sector_size,
               (unsigned long)info.sector_count,
               (unsigned long long)(((uint64_t)info.sector_size *
                                     info.sector_count) / UINT64_C(1048576)));
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (error == ESP_OK) {
        error = esp_vfs_fat_info(USB_MSC_MOUNT_PATH, &total_bytes,
                                 &free_bytes);
    }
    if (error == ESP_OK && free_bytes < USB_MSC_MIN_FREE_BYTES) {
        printf("msc: only %llu KiB free\n",
               (unsigned long long)(free_bytes / UINT64_C(1024)));
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        max_test_bytes = USB_MSC_MAX_TEST_BYTES;
        if (free_bytes - USB_MSC_FREE_RESERVE_BYTES < max_test_bytes) {
            max_test_bytes = free_bytes - USB_MSC_FREE_RESERVE_BYTES;
        }
        printf("msc: total=%llu KiB free=%llu KiB max_test=%llu KiB\n",
               (unsigned long long)(total_bytes / UINT64_C(1024)),
               (unsigned long long)(free_bytes / UINT64_C(1024)),
               (unsigned long long)(max_test_bytes / UINT64_C(1024)));
    }

    struct stat file_stat = {0};
    if (error == ESP_OK && stat(USB_MSC_TEST_PATH, &file_stat) == 0 &&
            !overwrite) {
        printf("refusing to overwrite %s; pass 'overwrite' only for a "
               "dedicated test drive\n", USB_MSC_TEST_PATH);
        error = ESP_ERR_INVALID_STATE;
    }

    if (error == ESP_OK) {
        buffer = malloc(USB_MSC_BUFFER_SIZE);
        expected = malloc(USB_MSC_BUFFER_SIZE);
        if (buffer == NULL || expected == NULL) {
            error = ESP_ERR_NO_MEM;
        }
    }
    if (error == ESP_OK) {
        file = fopen(USB_MSC_TEST_PATH, "wb");
        if (file == NULL) {
            printf("msc: open for write failed: %s\n", strerror(errno));
            error = ESP_FAIL;
        } else {
            setvbuf(file, NULL, _IOFBF, USB_MSC_BUFFER_SIZE);
            write_start_us = esp_timer_get_time();
            const int64_t write_deadline = write_start_us +
                                           seconds * INT64_C(1000000);
            int64_t last_progress_us = write_start_us;
            while (esp_timer_get_time() < write_deadline &&
                    bytes < max_test_bytes && error == ESP_OK &&
                    !s_msc_disconnected) {
                usb_msc_fill_block(buffer, blocks);
                if (fwrite(buffer, USB_MSC_BUFFER_SIZE, 1, file) != 1) {
                    printf("msc: write failed at block %lu: %s\n",
                           (unsigned long)blocks, strerror(errno));
                    error = ESP_FAIL;
                    break;
                }
                ++blocks;
                bytes += USB_MSC_BUFFER_SIZE;
                const int64_t now = esp_timer_get_time();
                if (now - last_progress_us >= INT64_C(10000000)) {
                    const unsigned elapsed_s =
                        (unsigned)((now - write_start_us) / UINT64_C(1000000));
                    const unsigned rate_kib_s =
                        (unsigned)(bytes / UINT64_C(1024) /
                                   ((now - write_start_us) / UINT64_C(1000000)));
                    printf("FACTORY_USB_MSC_PROGRESS {\"elapsed_s\":%u,"
                           "\"blocks\":%lu,\"bytes\":%llu,"
                           "\"write_kib_s\":%u,\"disconnected\":%s}\n",
                           elapsed_s, (unsigned long)blocks,
                           (unsigned long long)bytes, rate_kib_s,
                           s_msc_disconnected ? "true" : "false");
                    fflush(stdout);
                    last_progress_us = now;
                }
            }
            write_end_us = esp_timer_get_time();
        }
    }

    if (file != NULL) {
        if (fflush(file) != 0 && error == ESP_OK) {
            printf("msc: flush failed: %s\n", strerror(errno));
            error = ESP_FAIL;
        }
        if (fsync(fileno(file)) != 0 && error == ESP_OK) {
            printf("msc: fsync failed: %s\n", strerror(errno));
            error = ESP_FAIL;
        }
        if (fclose(file) != 0 && error == ESP_OK) {
            printf("msc: close failed: %s\n", strerror(errno));
            error = ESP_FAIL;
        }
        file = NULL;
    }

    uint32_t verified_blocks = 0;
    if (error == ESP_OK && !s_msc_disconnected) {
        file = fopen(USB_MSC_TEST_PATH, "rb");
        if (file == NULL) {
            printf("msc: open for verify failed: %s\n", strerror(errno));
            error = ESP_FAIL;
        } else {
            setvbuf(file, NULL, _IOFBF, USB_MSC_BUFFER_SIZE);
            for (uint32_t block = 0; block < blocks && error == ESP_OK;
                    ++block) {
                usb_msc_fill_block(expected, block);
                if (fread(buffer, USB_MSC_BUFFER_SIZE, 1, file) != 1 ||
                        memcmp(buffer, expected, USB_MSC_BUFFER_SIZE) != 0) {
                    printf("msc: verify failed at block %lu\n",
                           (unsigned long)block);
                    error = ESP_FAIL;
                } else {
                    ++verified_blocks;
                }
            }
            fclose(file);
            file = NULL;
        }
    }

    bool cleanup_failed = false;
    if (error == ESP_OK && remove(USB_MSC_TEST_PATH) != 0) {
        printf("msc: cleanup failed: %s\n", strerror(errno));
        cleanup_failed = true;
    }

    if (vfs != NULL) {
        const esp_err_t unregister_error = msc_host_vfs_unregister(vfs);
        if (error == ESP_OK && unregister_error != ESP_OK) {
            error = unregister_error;
        }
    }
    if (device != NULL) {
        const esp_err_t device_error = msc_host_uninstall_device(device);
        if (error == ESP_OK && device_error != ESP_OK) {
            error = device_error;
        }
    }
    if (msc_driver_installed) {
        const esp_err_t driver_error = msc_host_uninstall();
        if (error == ESP_OK && driver_error != ESP_OK) {
            error = driver_error;
        }
    }
    const esp_err_t stop_error = bsp_usb_host_stop();
    if (error == ESP_OK && stop_error != ESP_OK) {
        error = stop_error;
    }
    free(buffer);
    free(expected);

    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_USB_HOST, error,
                             "USB MSC stress test failed");
        return error;
    }

    const int64_t write_elapsed_us = write_end_us - write_start_us;
    const unsigned rate_kib_s = write_elapsed_us > 0 ?
        (unsigned)(bytes / UINT64_C(1024) /
                   ((write_elapsed_us + UINT64_C(999999)) /
                    UINT64_C(1000000))) : 0;
    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "msc:%lus %lluKiB %uKiB/s verify=%lu/%lu%s",
             (unsigned long)seconds, (unsigned long long)(bytes / UINT64_C(1024)),
             rate_kib_s, (unsigned long)verified_blocks,
             (unsigned long)blocks, cleanup_failed ? " cleanup=warn" : "");
    factory_report_set(FACTORY_TEST_USB_HOST,
                       cleanup_failed ? FACTORY_STATUS_WARN : FACTORY_STATUS_PASS,
                       detail);
    factory_report_print_one(FACTORY_TEST_USB_HOST);
    return ESP_OK;
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
        {.command = "usb_msc_test", .help = "Write and verify a FAT USB drive for a bounded interval: usb_msc_test [SECONDS 10-1800] [overwrite].", .func = command_usb_msc_test},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
