/*
 * Candis-S31 watch demo - STORAGE domain test suite.
 *
 * Ports the factory storage commands (spec E.4): TF write-verify goes
 * through the svc_storage lease (the service owns the mount lifecycle,
 * unlike the factory); USB MSC/enum tests drive the host stack directly
 * after checking the app-side arbitration flag (spec C.5).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#include "demo_apps.h"
#include "services/svc_storage.h"
#include "test_registry.h"

/* FUSB303B_DEVICE_TYPE_VALUE (0x03), mirrored from the BSP-private
 * fusb303b component header (not on the demo include path). */
#define FUSB303B_DEVICE_TYPE 0x03

#define USB_ENUM_TIMEOUT_S   20
#define USB_MSC_MOUNT_PATH   "/usb0"
#define USB_MSC_TEST_PATH    USB_MSC_MOUNT_PATH "/CANDIS_USB_STRESS.BIN"
#define USB_MSC_BUFFER_SIZE  8192U
#define USB_MSC_TEST_BLOCKS  32U          /* 32 x 8 KiB = 256 KiB */
#define USB_MSC_MIN_FREE     (16ULL * 1024ULL * 1024ULL)

static void set_result(test_result_t *out, test_status_t st,
                       const char *evidence)
{
    out->st = st;
    snprintf(out->evidence, sizeof(out->evidence), "%s", evidence);
}

/* ------------------------------------------------------------------ */
/* TF write-verify (factory sdcard_test, lease instead of mount)        */
/* ------------------------------------------------------------------ */

static void test_sd_rw(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    svc_storage_lease_t lease = {0};
    if (svc_storage_lease_acquire(&lease) != ESP_OK) {
        set_result(out, TEST_ST_SKIP, "no SD card");
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/.candis_demo_test",
             svc_storage_mount_point());
    const char payload[] = "Candis-S31 SDMMC demo test\n";
    char readback[sizeof(payload)] = {0};
    bool passed = true;
    const char *phase = "write";
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        passed = false;
    } else {
        if (fwrite(payload, 1, sizeof(payload), file) != sizeof(payload)) {
            passed = false;
        }
        if (fclose(file) != 0) {
            passed = false;
        }
    }
    if (passed) {
        phase = "read";
        file = fopen(path, "rb");
        if (file == NULL) {
            passed = false;
        } else {
            if (fread(readback, 1, sizeof(readback), file) !=
                    sizeof(readback)) {
                passed = false;
            }
            if (fclose(file) != 0) {
                passed = false;
            }
        }
    }
    if (passed) {
        phase = "verify";
        passed = memcmp(readback, payload, sizeof(payload)) == 0;
    }
    /* Remove the artifact even after a failed phase (factory rule). */
    if (unlink(path) != 0 && passed) {
        phase = "delete";
        passed = false;
    }
    svc_storage_lease_release(&lease);

    if (passed) {
        set_result(out, TEST_ST_PASS, "write read verify delete ok");
    } else {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence), "%s failed", phase);
    }
}

/* ------------------------------------------------------------------ */
/* Shared USB MSC plumbing (factory usb_msc_test)                       */
/* ------------------------------------------------------------------ */

static volatile bool s_msc_connected;
static volatile bool s_msc_disconnected;
static volatile uint8_t s_msc_address;

static void usb_msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (event->event == MSC_DEVICE_CONNECTED) {
        s_msc_address = event->device.address;
        s_msc_connected = true;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        s_msc_connected = false;
        s_msc_disconnected = true;
    }
}

/* Deterministic block pattern, verbatim from factory_usb.c. */
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

/* ------------------------------------------------------------------ */
/* USB MSC pattern verify (factory usb_msc_test, short bounded run)     */
/* ------------------------------------------------------------------ */

static void test_usb_msc_rw(const test_ctx_t *ctx, test_result_t *out)
{
    if (app_usb_host_busy()) {
        set_result(out, TEST_ST_SKIP, "USB app busy");
        return;
    }
    ctx->progress(ctx, 0, "Insert a USB disk");

    esp_err_t error = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV,
                                         true);
    msc_host_device_handle_t device = NULL;
    msc_host_vfs_handle_t vfs = NULL;
    FILE *file = NULL;
    uint8_t *buffer = NULL;
    uint8_t *expected = NULL;
    bool msc_driver_installed = false;
    bool disk_seen = false;

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
        s_msc_address = 0;
        error = msc_host_install(&driver_config);
        msc_driver_installed = (error == ESP_OK);
    }
    /* Host-stack failures before the wait are real errors, not "no disk". */
    const esp_err_t stack_error = error;

    const int64_t connect_deadline =
        esp_timer_get_time() + USB_ENUM_TIMEOUT_S * INT64_C(1000000);
    int64_t last_report = 0;
    while (error == ESP_OK && !s_msc_connected && !s_msc_disconnected &&
            !ctx->cancel_requested(ctx) &&
            esp_timer_get_time() < connect_deadline) {
        const int64_t now = esp_timer_get_time();
        if (now - last_report >= INT64_C(1000000)) {
            last_report = now;
            const int remain = (int)((connect_deadline - now) / 1000000);
            char stage[40];
            snprintf(stage, sizeof(stage), "Insert a USB disk (%d s)",
                     remain);
            ctx->progress(ctx,
                          (USB_ENUM_TIMEOUT_S - remain) * 25 /
                          USB_ENUM_TIMEOUT_S, stage);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (error == ESP_OK && !s_msc_connected) {
        error = s_msc_disconnected ? ESP_ERR_INVALID_STATE :
                ESP_ERR_NOT_FOUND;
    }
    disk_seen = s_msc_connected;

    uint16_t vid = 0, pid = 0;
    if (error == ESP_OK) {
        error = msc_host_install_device(s_msc_address, &device);
    }
    msc_host_device_info_t info = {0};
    if (error == ESP_OK) {
        error = msc_host_get_device_info(device, &info);
        vid = info.idVendor;
        pid = info.idProduct;
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

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint32_t blocks = 0;
    uint32_t verified_blocks = 0;
    bool disk_too_small = false;
    bool left_file = false;
    if (error == ESP_OK) {
        error = esp_vfs_fat_info(USB_MSC_MOUNT_PATH, &total_bytes,
                                 &free_bytes);
    }
    if (error == ESP_OK && free_bytes < USB_MSC_MIN_FREE) {
        disk_too_small = true;
        error = ESP_FAIL; /* flow breaker; verdict decided below */
    }
    if (error == ESP_OK) {
        buffer = malloc(USB_MSC_BUFFER_SIZE);
        expected = malloc(USB_MSC_BUFFER_SIZE);
        if (buffer == NULL || expected == NULL) {
            error = ESP_ERR_NO_MEM;
        }
    }
    if (error == ESP_OK) {
        /* A leftover from an interrupted previous run is ours: remove. */
        struct stat st = {0};
        if (stat(USB_MSC_TEST_PATH, &st) == 0 && remove(USB_MSC_TEST_PATH) != 0) {
            error = ESP_FAIL;
            left_file = true;
        }
    }
    if (error == ESP_OK) {
        file = fopen(USB_MSC_TEST_PATH, "wb");
        if (file == NULL) {
            error = ESP_FAIL;
        }
    }
    if (error == ESP_OK) {
        ctx->progress(ctx, 30, "Writing pattern");
        setvbuf(file, NULL, _IOFBF, USB_MSC_BUFFER_SIZE);
        for (blocks = 0; blocks < USB_MSC_TEST_BLOCKS &&
                error == ESP_OK && !s_msc_disconnected &&
                !ctx->cancel_requested(ctx); ++blocks) {
            usb_msc_fill_block(buffer, blocks);
            if (fwrite(buffer, USB_MSC_BUFFER_SIZE, 1, file) != 1) {
                error = ESP_FAIL;
            }
        }
        if (fflush(file) != 0 && error == ESP_OK) {
            error = ESP_FAIL;
        }
        if (fsync(fileno(file)) != 0 && error == ESP_OK) {
            error = ESP_FAIL;
        }
        if (fclose(file) != 0 && error == ESP_OK) {
            error = ESP_FAIL;
        }
        file = NULL;
        if (ctx->cancel_requested(ctx)) {
            error = ESP_FAIL; /* flow breaker; runner records the abort */
        }
    }
    if (error == ESP_OK) {
        ctx->progress(ctx, 70, "Verifying pattern");
        file = fopen(USB_MSC_TEST_PATH, "rb");
        if (file == NULL) {
            error = ESP_FAIL;
        }
    }
    if (error == ESP_OK) {
        setvbuf(file, NULL, _IOFBF, USB_MSC_BUFFER_SIZE);
        for (uint32_t block = 0; block < blocks && error == ESP_OK;
                ++block) {
            usb_msc_fill_block(expected, block);
            if (fread(buffer, USB_MSC_BUFFER_SIZE, 1, file) != 1 ||
                    memcmp(buffer, expected, USB_MSC_BUFFER_SIZE) != 0) {
                error = ESP_FAIL;
            } else {
                ++verified_blocks;
            }
        }
        fclose(file);
        file = NULL;
    }
    if (remove(USB_MSC_TEST_PATH) != 0 && error == ESP_OK) {
        left_file = true; /* cleanup failure degrades PASS -> WARN */
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

    if (ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records SKIP "aborted" */
    }
    if (!disk_seen) {
        if (stack_error != ESP_OK) {
            out->st = TEST_ST_FAIL;
            snprintf(out->evidence, sizeof(out->evidence),
                     "host stack failed: %s", esp_err_to_name(stack_error));
        } else {
            set_result(out, TEST_ST_SKIP, "no USB disk inserted in 20 s");
        }
        return;
    }
    if (disk_too_small) {
        set_result(out, TEST_ST_SKIP, "disk nearly full (<16 MiB free)");
        return;
    }
    if (error != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "msc failed: %s (verify %lu/%lu)", esp_err_to_name(error),
                 (unsigned long)verified_blocks, (unsigned long)blocks);
        return;
    }
    out->st = left_file ? TEST_ST_WARN : TEST_ST_PASS;
    snprintf(out->evidence, sizeof(out->evidence),
             "%lu KiB verify %lu/%lu vid %04x:%04x%s",
             (unsigned long)(USB_MSC_TEST_BLOCKS * USB_MSC_BUFFER_SIZE /
                             1024U),
             (unsigned long)verified_blocks, (unsigned long)blocks,
             (unsigned)vid, (unsigned)pid,
             left_file ? " cleanup fail" : "");
}

/* ------------------------------------------------------------------ */
/* Type-C status (factory typec_test)                                   */
/* ------------------------------------------------------------------ */

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

static void test_typec_status(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    /* Read, then poll up to 1 s for an attach to settle (factory). */
    bsp_type_c_status_t status;
    esp_err_t error = bsp_type_c_get_status(&status, false);
    const int64_t deadline = esp_timer_get_time() + INT64_C(1000000);
    while (error == ESP_OK && !status.attached &&
            esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
        error = bsp_type_c_get_status(&status, false);
    }
    if (error == ESP_OK) {
        error = bsp_type_c_get_status(&status, true);
    }
    if (error != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "FUSB303B read failed: %s", esp_err_to_name(error));
        return;
    }

    test_status_t verdict = TEST_ST_PASS;
    const char *text;
    if (status.device_type != FUSB303B_DEVICE_TYPE) {
        verdict = TEST_ST_FAIL;
        text = "FUSB303B identity mismatch";
    } else if (status.role != BSP_TYPE_C_ROLE_DRP) {
        verdict = TEST_ST_FAIL;
        text = "not in DRP role";
    } else if (status.fault || status.remedy_active) {
        verdict = TEST_ST_FAIL;
        text = status.fault ? "CC fault active" : "remedy state active";
    } else if (status.attached &&
               (status.orientation == 1 || status.orientation == 2)) {
        text = status.vbus_ok ? "cable attached, vbus ok" :
               "cable attached, no vbus";
    } else if (!status.attached && status.orientation == 0) {
        text = "no cable attached";
    } else {
        verdict = TEST_ST_WARN;
        text = status.attached ? "attached, orientation unknown" :
               "detached, orientation stuck";
    }
    out->st = verdict;
    snprintf(out->evidence, sizeof(out->evidence), "%s (role %s, %s)",
             text, type_c_role_name(status.role),
             status.advertised_current == BSP_TYPE_C_CURRENT_1_5_A ? "1.5A" :
             status.advertised_current == BSP_TYPE_C_CURRENT_3_0_A ? "3.0A" :
             "default");
}

/* ------------------------------------------------------------------ */
/* USB device enumeration (factory usb_host_test)                       */
/* ------------------------------------------------------------------ */

static volatile uint8_t s_enum_address;

static void usb_enum_event_cb(const usb_host_client_event_msg_t *message,
                              void *arg)
{
    if (message->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        *(volatile uint8_t *)arg = message->new_dev.address;
    }
}

/* Best-effort UTF-16LE descriptor to printable ASCII (factory helper). */
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

static void test_usb_enum(const test_ctx_t *ctx, test_result_t *out)
{
    if (app_usb_host_busy()) {
        set_result(out, TEST_ST_SKIP, "USB app busy");
        return;
    }
    ctx->progress(ctx, 0, "Insert a USB device");

    esp_err_t error = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV,
                                         true);
    usb_host_client_handle_t client = NULL;
    usb_device_handle_t device = NULL;
    if (error == ESP_OK) {
        s_enum_address = 0;
        const usb_host_client_config_t client_config = {
            .is_synchronous = false,
            .max_num_event_msg = 5,
            .async = {
                .client_event_callback = usb_enum_event_cb,
                .callback_arg = (void *)&s_enum_address,
            },
        };
        error = usb_host_client_register(&client_config, &client);
    }

    const int64_t deadline =
        esp_timer_get_time() + USB_ENUM_TIMEOUT_S * INT64_C(1000000);
    int64_t last_report = 0;
    while (error == ESP_OK && s_enum_address == 0 &&
            !ctx->cancel_requested(ctx) &&
            esp_timer_get_time() < deadline) {
        const esp_err_t event_error = usb_host_client_handle_events(
                                          client, pdMS_TO_TICKS(200));
        if (event_error != ESP_OK && event_error != ESP_ERR_TIMEOUT) {
            error = event_error;
        }
        const int64_t now = esp_timer_get_time();
        if (now - last_report >= INT64_C(1000000)) {
            last_report = now;
            const int remain = (int)((deadline - now) / 1000000);
            char stage[40];
            snprintf(stage, sizeof(stage), "Insert a USB device (%d s)",
                     remain);
            ctx->progress(ctx,
                          (USB_ENUM_TIMEOUT_S - remain) * 25 /
                          USB_ENUM_TIMEOUT_S, stage);
        }
    }

    usb_device_info_t info = {0};
    const usb_device_desc_t *descriptor = NULL;
    if (error == ESP_OK && s_enum_address != 0) {
        error = usb_host_device_open(client, s_enum_address, &device);
    }
    if (error == ESP_OK && device != NULL) {
        error = usb_host_device_info(device, &info);
    }
    if (error == ESP_OK && device != NULL) {
        error = usb_host_get_device_descriptor(device, &descriptor);
    }

    char product[24] = {0};
    bool enumerated = false;
    if (error == ESP_OK && descriptor != NULL) {
        usb_string_to_ascii(info.str_desc_product, product,
                            sizeof(product));
        enumerated = true;
    }

    /* Full teardown on every path (factory contract). */
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

    if (ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records SKIP "aborted" */
    }
    if (error != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "enum failed: %s", esp_err_to_name(error));
        return;
    }
    if (!enumerated) {
        set_result(out, TEST_ST_WARN, "no device attached in 20 s");
        return;
    }
    out->st = TEST_ST_PASS;
    snprintf(out->evidence, sizeof(out->evidence),
             "vid %04x:%04x spd %u %.16s", descriptor->idVendor,
             descriptor->idProduct, (unsigned)info.speed, product);
}

/* ------------------------------------------------------------------ */

void test_storage_register(void)
{
    static const test_case_t cases[] = {
        {
            .id = "storage.sd_rw", .name = "TF write-verify",
            .domain = TEST_DOM_STORAGE, .flags = TEST_F_NEEDS_SD,
            .timeout_ms = 10000, .run = test_sd_rw,
        },
        {
            .id = "storage.usb_msc_rw", .name = "USB disk verify",
            .domain = TEST_DOM_STORAGE,
            .flags = TEST_F_INTERACTIVE | TEST_F_NEEDS_USB_DISK,
            .timeout_ms = 45000, .run = test_usb_msc_rw,
        },
        {
            .id = "storage.typec_status", .name = "Type-C status",
            .domain = TEST_DOM_STORAGE, .flags = 0,
            .timeout_ms = 8000, .run = test_typec_status,
        },
        {
            .id = "storage.usb_enum", .name = "USB device enum",
            .domain = TEST_DOM_STORAGE, .flags = TEST_F_INTERACTIVE,
            .timeout_ms = 30000, .run = test_usb_enum,
        },
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
            ++index) {
        test_register(&cases[index]);
    }
}
