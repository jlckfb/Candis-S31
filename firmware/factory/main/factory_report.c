/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "factory_report.h"

#define FACTORY_DETAIL_LENGTH 96

typedef struct {
    const char *name;
    factory_status_t status;
    char detail[FACTORY_DETAIL_LENGTH];
} factory_result_t;

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
    [FACTORY_TEST_CHARGE] = {.name = "charge"},
    [FACTORY_TEST_RTC_ALARM] = {.name = "rtc_alarm"},
    [FACTORY_TEST_BUTTONS] = {.name = "buttons"},
    [FACTORY_TEST_WIFI] = {.name = "wifi"},
    [FACTORY_TEST_BLE] = {.name = "ble"},
    [FACTORY_TEST_DISPLAY_SLEEP] = {.name = "display_sleep"},
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
