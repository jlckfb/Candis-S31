/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

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
    FACTORY_TEST_CHARGE,
    FACTORY_TEST_RTC_ALARM,
    FACTORY_TEST_BUTTONS,
    FACTORY_TEST_WIFI,
    FACTORY_TEST_BLE,
    FACTORY_TEST_DISPLAY_SLEEP,
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

/** Reset every test result to NOT_RUN. */
void factory_report_init(void);

/** Store one test result and a short printable detail string. */
esp_err_t factory_report_set(factory_test_id_t test, factory_status_t status, const char *detail);

/** Print one result in human-readable and JSON-lines forms. */
void factory_report_print_one(factory_test_id_t test);

/** Print all results and a machine-readable summary. */
void factory_report_print(void);

/** Resolve the stable report name used by the console and JSON output. */
factory_test_id_t factory_report_find(const char *name);

/** Print one string as an escaped JSON string literal, quotes included. */
void factory_report_print_json_string(const char *value);
