/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

/** Maximum length of one result detail string, including the terminator. */
#define FACTORY_DETAIL_LENGTH 96

typedef enum {
    FACTORY_TEST_SAFE_STATE = 0,
    FACTORY_TEST_FLASH,
    FACTORY_TEST_PSRAM,
    FACTORY_TEST_I2C_MAIN,
    FACTORY_TEST_I2C_LOW_POWER,
    FACTORY_TEST_PMIC,
    FACTORY_TEST_RTC,
    FACTORY_TEST_SHARED_IRQ,
    FACTORY_TEST_TYPE_C,
    FACTORY_TEST_DISPLAY,
    FACTORY_TEST_TOUCH,
    FACTORY_TEST_RGB_LED,
    FACTORY_TEST_SDCARD,
    FACTORY_TEST_SPEAKER,
    FACTORY_TEST_MICROPHONE,
    FACTORY_TEST_CAMERA,
    FACTORY_TEST_JPEG,
    FACTORY_TEST_CORDIC,
    FACTORY_TEST_CHARGE,
    FACTORY_TEST_RTC_ALARM,
    FACTORY_TEST_BUTTONS,
    FACTORY_TEST_WIFI,
    FACTORY_TEST_BLE,
    FACTORY_TEST_DISPLAY_SLEEP,
    FACTORY_TEST_USB_HOST,
    FACTORY_TEST_COUNT,
} factory_test_id_t;

typedef enum {
    FACTORY_STATUS_NOT_RUN = 0,
    FACTORY_STATUS_PASS,
    FACTORY_STATUS_FAIL,
    FACTORY_STATUS_SKIP,
    FACTORY_STATUS_WARN,
    FACTORY_STATUS_COUNT,
} factory_status_t;

/** Reset every test result to NOT_RUN and drop the persisted copy. */
void factory_report_init(void);

/** Load persisted results from NVS; falls back to all-NOT_RUN when no valid
 *  copy exists. Requires nvs_flash_init() to have run for persistence. */
void factory_report_load(void);

/** Store one test result and a short printable detail string; the result is
 *  also persisted to NVS on a best-effort basis so power-cycle test flows
 *  keep their history. */
esp_err_t factory_report_set(factory_test_id_t test, factory_status_t status, const char *detail);

/** Read back one stored result. */
esp_err_t factory_report_get(factory_test_id_t test, factory_status_t *status, char *detail, size_t detail_size);

/** Print one result in human-readable and JSON-lines forms. */
void factory_report_print_one(factory_test_id_t test);

/** Print all results and a machine-readable summary. */
void factory_report_print(void);

/** Resolve the stable report name used by the console and JSON output. */
factory_test_id_t factory_report_find(const char *name);

/** Print one string as an escaped JSON string literal, quotes included. */
void factory_report_print_json_string(const char *value);

/** Record a FAIL result whose detail is "action: error-name" and print it. */
void factory_report_error(factory_test_id_t test, esp_err_t error, const char *action);

/** File an operator yes/no/skip answer as PASS/FAIL/NOT_RUN and print it. */
void factory_report_operator_verdict(factory_test_id_t test, char answer,
                                     const char *pass_detail,
                                     const char *fail_detail,
                                     const char *pending_detail);
