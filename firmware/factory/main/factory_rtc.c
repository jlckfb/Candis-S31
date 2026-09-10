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

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

#define RTC_ALARM_POLL_MS 200

static int command_rtc_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_rtc_time_t time;
    bsp_rtc_status_t status;
    const esp_err_t error = bsp_rtc_get_time(&time, &status);
    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_RTC, error, "RX8130CE read failed");
        return error;
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "%04u-%02u-%02u %02u:%02u:%02u valid=%s flags=0x%02x",
             time.year, time.month, time.day, time.hour, time.minute, time.second,
             status.time_valid ? "yes" : "no", status.raw);
    factory_report_set(FACTORY_TEST_RTC,
                       status.time_valid ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_RTC);
    return status.time_valid ? ESP_OK : ESP_FAIL;
}

static bool parse_rtc_time(int argc, char **argv, bsp_rtc_time_t *time)
{
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    char trailing = '\0';

    if (argc != 4 ||
            sscanf(argv[1], "%u-%u-%u%c", &year, &month, &day, &trailing) != 3 ||
            sscanf(argv[2], "%u:%u:%u%c", &hour, &minute, &second, &trailing) != 3) {
        return false;
    }

    char *end = NULL;
    const long weekday = strtol(argv[3], &end, 10);
    /* Mirror the RX8130CE driver's rx8130ce_time_is_valid() rules so an
     * out-of-range value is rejected before it reaches the chip. */
    if (end == argv[3] || *end != '\0' || weekday < 0 || weekday > 6 ||
            year < 2000 || year > 2099 || month < 1 || month > 12 ||
            hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    static const uint8_t days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    unsigned days = days_per_month[month - 1];
    if (month == 2 && (year % 4) == 0) {
        ++days;
    }
    if (day < 1 || day > days) {
        return false;
    }

    *time = (bsp_rtc_time_t) {
        .year = (uint16_t)year,
        .month = (uint8_t)month,
        .day = (uint8_t)day,
        .weekday = (uint8_t)weekday,
        .hour = (uint8_t)hour,
        .minute = (uint8_t)minute,
        .second = (uint8_t)second,
    };
    return true;
}

static bool rtc_time_matches(const bsp_rtc_time_t *expected,
                             const bsp_rtc_time_t *actual)
{
    return expected->year == actual->year &&
           expected->month == actual->month &&
           expected->day == actual->day &&
           expected->weekday == actual->weekday &&
           expected->hour == actual->hour &&
           expected->minute == actual->minute &&
           expected->second == actual->second;
}

static int command_rtc_set(int argc, char **argv)
{
    bsp_rtc_time_t requested;
    if (!parse_rtc_time(argc, argv, &requested)) {
        printf("usage: rtc_set YYYY-MM-DD HH:MM:SS WEEKDAY\n");
        printf("WEEKDAY uses 0=Sunday through 6=Saturday\n");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = bsp_rtc_set_time(&requested);
    bsp_rtc_time_t actual = {0};
    bsp_rtc_status_t status = {0};
    if (error == ESP_OK) {
        error = bsp_rtc_get_time(&actual, &status);
    }

    const bool passed = error == ESP_OK && status.time_valid &&
                        rtc_time_matches(&requested, &actual);
    char detail[96];
    if (error == ESP_OK) {
        snprintf(detail, sizeof(detail),
                 "readback=%04u-%02u-%02u %02u:%02u:%02u weekday=%u valid=%s",
                 actual.year, actual.month, actual.day, actual.hour, actual.minute,
                 actual.second, actual.weekday, status.time_valid ? "yes" : "no");
    } else {
        snprintf(detail, sizeof(detail), "RTC set/readback failed: %s",
                 esp_err_to_name(error));
    }
    factory_report_set(FACTORY_TEST_RTC,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_RTC);
    return passed ? ESP_OK : (error != ESP_OK ? error : ESP_FAIL);
}

static int command_irq_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_shared_irq_status_t status = {0};
    const esp_err_t error = bsp_shared_irq_service(&status);
    const bool passed = error == ESP_OK && status.line_released;
    char detail[96];
    snprintf(detail, sizeof(detail),
             "passes=%u released=%s pmic=%02x:%02x:%02x rtc=0x%02x result=%s",
             status.service_passes, status.line_released ? "yes" : "no",
             status.pmic[0], status.pmic[1], status.pmic[2], status.rtc,
             esp_err_to_name(error));
    factory_report_set(FACTORY_TEST_SHARED_IRQ,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_SHARED_IRQ);
    return passed ? ESP_OK : (error != ESP_OK ? error : ESP_FAIL);
}

static void rtc_alarm_shared_irq(void *arg)
{
    *(volatile bool *)arg = true;
}

static int command_rtc_alarm_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_rtc_time_t now;
    bsp_rtc_status_t status;
    esp_err_t error = bsp_rtc_get_time(&now, &status);
    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_RTC_ALARM, error, "RX8130CE read failed");
        return error;
    }

    /* The RX8130CE compares minute/hour/day fields only; the fastest
     * self-contained check targets the next minute boundary. */
    bsp_rtc_alarm_t alarm = {0};
    alarm.minute_en = true;
    alarm.minute = (uint8_t)((now.minute + 1) % 60);

    volatile bool irq_seen = false;
    uint8_t cleared = 0;
    bsp_shared_irq_status_t serviced;
    /* Drain stale PMIC/RTC flags so a fresh alarm can assert the line. */
    error = bsp_shared_irq_service(&serviced);
    if (error == ESP_OK) {
        error = bsp_shared_irq_register_callback(rtc_alarm_shared_irq,
                                                 (void *)&irq_seen);
    }
    if (error == ESP_OK) {
        error = bsp_rtc_set_alarm(&alarm);
    }
    if (error == ESP_OK) {
        error = bsp_rtc_alarm_irq_enable(true);
    }

    bool fired = false;
    unsigned wait_s = 0;
    if (error == ESP_OK) {
        /* Re-read the clock: some seconds may have passed since "now". */
        if (bsp_rtc_get_time(&now, &status) != ESP_OK) {
            now.second = 0;
        }
        wait_s = 60u - now.second + 8u;
        printf("Waiting up to %u s for the alarm at minute=%02u\n",
               wait_s, alarm.minute);
        const int64_t deadline = esp_timer_get_time() + (int64_t)wait_s * 1000000;
        while (esp_timer_get_time() < deadline) {
            if (bsp_rtc_get_status(&status) == ESP_OK && status.alarm) {
                fired = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(RTC_ALARM_POLL_MS));
        }
    }

    const bsp_rtc_alarm_t disarm = {0};
    bsp_rtc_alarm_irq_enable(false);
    bsp_rtc_set_alarm(&disarm);
    bsp_rtc_clear_interrupt_flags(&cleared);
    bsp_shared_irq_register_callback(NULL, NULL);

    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_RTC_ALARM, error, "RX8130CE alarm setup failed");
        return error;
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "minute=%02u %s shared_irq_notified=%s",
             alarm.minute, fired ? "fired" : "no flag within timeout",
             irq_seen ? "yes" : "no");
    factory_report_set(FACTORY_TEST_RTC_ALARM,
                       fired ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_RTC_ALARM);
    return fired ? ESP_OK : ESP_FAIL;
}


esp_err_t factory_rtc_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "rtc_test", .help = "Read RX8130CE time and retained status flags.", .func = command_rtc_test},
        {.command = "rtc_set", .help = "Set and verify RX8130CE time: rtc_set YYYY-MM-DD HH:MM:SS WEEKDAY.", .func = command_rtc_set},
        {.command = "rtc_alarm", .help = "Fire an RX8130CE alarm at the next minute and check the flag.", .func = command_rtc_alarm_test},
        {.command = "irq_test", .help = "Service and verify the shared PMIC/RTC interrupt line.", .func = command_irq_test},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
