/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"

#include "factory_report.h"

typedef struct {
    const char *name;
    factory_status_t status;
    char detail[FACTORY_DETAIL_LENGTH];
} factory_result_t;

/* Results persist across the power-cycle steps of the bring-up flow. The
 * magic/version/count triple rejects blobs written by another firmware. */
#define FACTORY_NVS_NAMESPACE "factory"
#define FACTORY_NVS_KEY       "results"
#define FACTORY_NVS_MAGIC     UINT32_C(0xca5d15e1)
#define FACTORY_NVS_VERSION   1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    struct {
        uint8_t status;
        char detail[FACTORY_DETAIL_LENGTH];
    } tests[FACTORY_TEST_COUNT];
} factory_nvs_blob_t;

static factory_result_t s_results[FACTORY_TEST_COUNT] = {
    [FACTORY_TEST_SAFE_STATE] = {.name = "safe_state"},
    [FACTORY_TEST_FLASH] = {.name = "flash"},
    [FACTORY_TEST_PSRAM] = {.name = "psram"},
    [FACTORY_TEST_I2C_MAIN] = {.name = "i2c_main"},
    [FACTORY_TEST_I2C_LOW_POWER] = {.name = "i2c_low_power"},
    [FACTORY_TEST_PMIC] = {.name = "pmic"},
    [FACTORY_TEST_RTC] = {.name = "rtc"},
    [FACTORY_TEST_SHARED_IRQ] = {.name = "shared_irq"},
    [FACTORY_TEST_TYPE_C] = {.name = "type_c"},
    [FACTORY_TEST_DISPLAY] = {.name = "display"},
    [FACTORY_TEST_TOUCH] = {.name = "touch"},
    [FACTORY_TEST_RGB_LED] = {.name = "rgb_led"},
    [FACTORY_TEST_SDCARD] = {.name = "sdcard"},
    [FACTORY_TEST_SPEAKER] = {.name = "speaker"},
    [FACTORY_TEST_MICROPHONE] = {.name = "microphone"},
    [FACTORY_TEST_CAMERA] = {.name = "camera"},
    [FACTORY_TEST_JPEG] = {.name = "jpeg"},
    [FACTORY_TEST_CORDIC] = {.name = "cordic"},
    [FACTORY_TEST_PPA] = {.name = "ppa"},
    [FACTORY_TEST_BITSCRAMBLER] = {.name = "bitscrambler"},
    [FACTORY_TEST_ASRC] = {.name = "asrc"},
    [FACTORY_TEST_ACCEL] = {.name = "accel"},
    [FACTORY_TEST_CHARGE] = {.name = "charge"},
    [FACTORY_TEST_RTC_ALARM] = {.name = "rtc_alarm"},
    [FACTORY_TEST_BUTTONS] = {.name = "buttons"},
    [FACTORY_TEST_WIFI] = {.name = "wifi"},
    [FACTORY_TEST_BLE] = {.name = "ble"},
    [FACTORY_TEST_DISPLAY_SLEEP] = {.name = "display_sleep"},
    [FACTORY_TEST_USB_HOST] = {.name = "usb_host"},
};

static const char *status_name(factory_status_t status)
{
    switch (status) {
    case FACTORY_STATUS_PASS:
        return "PASS";
    case FACTORY_STATUS_FAIL:
        return "FAIL";
    case FACTORY_STATUS_SKIP:
        return "SKIP";
    case FACTORY_STATUS_WARN:
        return "WARN";
    case FACTORY_STATUS_NOT_RUN:
    default:
        return "NOT_RUN";
    }
}

static bool test_is_valid(factory_test_id_t test)
{
    return test >= 0 && test < FACTORY_TEST_COUNT;
}

/* Best-effort persistence: a missing or unusable NVS never blocks a test. */
static void factory_report_persist(void)
{
    factory_nvs_blob_t *blob = calloc(1, sizeof(*blob));
    if (blob == NULL) {
        return;
    }
    blob->magic = FACTORY_NVS_MAGIC;
    blob->version = FACTORY_NVS_VERSION;
    blob->count = FACTORY_TEST_COUNT;
    for (int test = 0; test < FACTORY_TEST_COUNT; ++test) {
        blob->tests[test].status = (uint8_t)s_results[test].status;
        memcpy(blob->tests[test].detail, s_results[test].detail,
               FACTORY_DETAIL_LENGTH);
    }
    nvs_handle_t handle;
    if (nvs_open(FACTORY_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, FACTORY_NVS_KEY, blob, sizeof(*blob));
        nvs_commit(handle);
        nvs_close(handle);
    }
    free(blob);
}

void factory_report_print_json_string(const char *value)
{
    putchar('"');
    for (const unsigned char *cursor = (const unsigned char *)value;
            *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\b':
            fputs("\\b", stdout);
            break;
        case '\f':
            fputs("\\f", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*cursor < 0x20) {
                printf("\\u%04x", *cursor);
            } else {
                putchar(*cursor);
            }
            break;
        }
    }
    putchar('"');
}

void factory_report_init(void)
{
    for (int test = 0; test < FACTORY_TEST_COUNT; ++test) {
        s_results[test].status = FACTORY_STATUS_NOT_RUN;
        snprintf(s_results[test].detail, sizeof(s_results[test].detail), "not executed");
    }
    /* Drop the persisted copy as well, so a later reboot stays cleared. */
    nvs_handle_t handle;
    if (nvs_open(FACTORY_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, FACTORY_NVS_KEY);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void factory_report_load(void)
{
    factory_nvs_blob_t *blob = calloc(1, sizeof(*blob));
    if (blob == NULL) {
        factory_report_init();
        return;
    }
    size_t length = sizeof(*blob);
    nvs_handle_t handle;
    bool loaded = false;
    if (nvs_open(FACTORY_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        const esp_err_t error = nvs_get_blob(handle, FACTORY_NVS_KEY, blob,
                                             &length);
        nvs_close(handle);
        loaded = error == ESP_OK && length == sizeof(*blob) &&
                 blob->magic == FACTORY_NVS_MAGIC &&
                 blob->version == FACTORY_NVS_VERSION &&
                 blob->count == FACTORY_TEST_COUNT;
    }
    if (!loaded) {
        factory_report_init();
        free(blob);
        return;
    }
    for (int test = 0; test < FACTORY_TEST_COUNT; ++test) {
        s_results[test].status = blob->tests[test].status < FACTORY_STATUS_COUNT ?
                                 (factory_status_t)blob->tests[test].status :
                                 FACTORY_STATUS_NOT_RUN;
        memcpy(s_results[test].detail, blob->tests[test].detail,
               FACTORY_DETAIL_LENGTH);
        s_results[test].detail[FACTORY_DETAIL_LENGTH - 1] = '\0';
    }
    free(blob);
}

esp_err_t factory_report_set(factory_test_id_t test, factory_status_t status, const char *detail)
{
    if (!test_is_valid(test) || status < FACTORY_STATUS_NOT_RUN ||
            status >= FACTORY_STATUS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    s_results[test].status = status;
    snprintf(s_results[test].detail, sizeof(s_results[test].detail), "%s",
             detail != NULL ? detail : "");
    factory_report_persist();
    return ESP_OK;
}

esp_err_t factory_report_get(factory_test_id_t test, factory_status_t *status,
                             char *detail, size_t detail_size)
{
    if (!test_is_valid(test) || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = s_results[test].status;
    if (detail != NULL && detail_size > 0) {
        snprintf(detail, detail_size, "%s", s_results[test].detail);
    }
    return ESP_OK;
}

void factory_report_print_one(factory_test_id_t test)
{
    if (!test_is_valid(test)) {
        return;
    }

    const factory_result_t *result = &s_results[test];
    printf("%-16s %-8s %s\n", result->name, status_name(result->status), result->detail);
    fputs("FACTORY_RESULT {\"test\":", stdout);
    factory_report_print_json_string(result->name);
    fputs(",\"status\":", stdout);
    factory_report_print_json_string(status_name(result->status));
    fputs(",\"detail\":", stdout);
    factory_report_print_json_string(result->detail);
    fputs("}\n", stdout);
}

void factory_report_print(void)
{
    unsigned counts[FACTORY_STATUS_COUNT] = {0};
    for (int test = 0; test < FACTORY_TEST_COUNT; ++test) {
        ++counts[s_results[test].status];
        factory_report_print_one((factory_test_id_t)test);
    }

    factory_status_t overall = FACTORY_STATUS_PASS;
    if (counts[FACTORY_STATUS_FAIL] > 0) {
        overall = FACTORY_STATUS_FAIL;
    } else if (counts[FACTORY_STATUS_WARN] > 0) {
        overall = FACTORY_STATUS_WARN;
    } else if (counts[FACTORY_STATUS_NOT_RUN] > 0) {
        overall = FACTORY_STATUS_NOT_RUN;
    }

    printf("FACTORY_SUMMARY {\"overall\":\"%s\",\"pass\":%u,\"fail\":%u,"
           "\"warn\":%u,\"skip\":%u,\"not_run\":%u}\n",
           status_name(overall),
           counts[FACTORY_STATUS_PASS],
           counts[FACTORY_STATUS_FAIL],
           counts[FACTORY_STATUS_WARN],
           counts[FACTORY_STATUS_SKIP],
           counts[FACTORY_STATUS_NOT_RUN]);
}

factory_test_id_t factory_report_find(const char *name)
{
    if (name == NULL) {
        return FACTORY_TEST_COUNT;
    }
    for (int test = 0; test < FACTORY_TEST_COUNT; ++test) {
        if (strcmp(name, s_results[test].name) == 0) {
            return (factory_test_id_t)test;
        }
    }
    return FACTORY_TEST_COUNT;
}

void factory_report_error(factory_test_id_t test, esp_err_t error, const char *action)
{
    char detail[96];
    snprintf(detail, sizeof(detail), "%s: %s", action, esp_err_to_name(error));
    factory_report_set(test, FACTORY_STATUS_FAIL, detail);
    factory_report_print_one(test);
}

void factory_report_operator_verdict(factory_test_id_t test, char answer,
                                     const char *pass_detail,
                                     const char *fail_detail,
                                     const char *pending_detail)
{
    if (answer == 'y') {
        factory_report_set(test, FACTORY_STATUS_PASS, pass_detail);
    } else if (answer == 'n') {
        factory_report_set(test, FACTORY_STATUS_FAIL, fail_detail);
    } else {
        factory_report_set(test, FACTORY_STATUS_NOT_RUN, pending_detail);
    }
    factory_report_print_one(test);
}
