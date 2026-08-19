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

static bool test_accepts_manual_result(factory_test_id_t test)
{
    return test == FACTORY_TEST_DISPLAY || test == FACTORY_TEST_RGB_LED ||
           test == FACTORY_TEST_SPEAKER || test == FACTORY_TEST_BUTTONS ||
           test == FACTORY_TEST_DISPLAY_SLEEP;
}

static int command_sys_tasks(int argc, char **argv)
{
    uint32_t period_ms = 1000;
    if (argc >= 2) {
        char *end = NULL;
        const unsigned long value = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || value < 100 || value > 10000) {
            printf("usage: sys_tasks [PERIOD_MS 100-10000]\n");
            return ESP_ERR_INVALID_ARG;
        }
        period_ms = (uint32_t)value;
    }

    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *before = malloc(capacity * sizeof(TaskStatus_t));
    TaskStatus_t *after = malloc(capacity * sizeof(TaskStatus_t));
    if (before == NULL || after == NULL) {
        free(before);
        free(after);
        return ESP_ERR_NO_MEM;
    }

    uint32_t run_before = 0;
    uint32_t run_after = 0;
    uxTaskGetSystemState(before, capacity, &run_before);
    vTaskDelay(pdMS_TO_TICKS(period_ms));
    const UBaseType_t count = uxTaskGetSystemState(after, capacity, &run_after);
    const uint32_t span = run_after - run_before;
    if (span == 0) {
        printf("run-time stats clock did not advance\n");
        free(before);
        free(after);
        return ESP_FAIL;
    }

    printf("%-16s %8s %10s\n", "task", "cpu", "stack_hwm");
    for (UBaseType_t i = 0; i < count; ++i) {
        uint32_t delta = 0;
        for (UBaseType_t j = 0; j < capacity; ++j) {
            if (before[j].xHandle == after[i].xHandle) {
                delta = after[i].ulRunTimeCounter - before[j].ulRunTimeCounter;
                break;
            }
        }
        const uint32_t pct_x100 = delta * 10000U / span;
        printf("%-16s %3u.%02u%% %10u\n", after[i].pcTaskName,
               (unsigned)(pct_x100 / 100), (unsigned)(pct_x100 % 100),
               (unsigned)after[i].usStackHighWaterMark);
    }
    printf("FACTORY_SYS {\"period_ms\":%u,\"span_us\":%u,\"tasks\":%u}\n",
           (unsigned)period_ms, (unsigned)span, (unsigned)count);
    free(before);
    free(after);
    return ESP_OK;
}

static int command_mark(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: mark TEST pass|fail|skip [detail]\n");
        return ESP_ERR_INVALID_ARG;
    }
    const factory_test_id_t test = factory_report_find(argv[1]);
    if (test == FACTORY_TEST_COUNT) {
        printf("unknown test: %s\n", argv[1]);
        return ESP_ERR_INVALID_ARG;
    }
    const factory_status_t status = strcmp(argv[2], "pass") == 0 ? FACTORY_STATUS_PASS :
                                            strcmp(argv[2], "fail") == 0 ? FACTORY_STATUS_FAIL :
                                            strcmp(argv[2], "skip") == 0 ? FACTORY_STATUS_SKIP :
                                                                            FACTORY_STATUS_NOT_RUN;
    if (status == FACTORY_STATUS_NOT_RUN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (status != FACTORY_STATUS_SKIP && !test_accepts_manual_result(test)) {
        printf("%s is software-scored; run its test command instead\n", argv[1]);
        return ESP_ERR_INVALID_ARG;
    }

    char detail[96] = "operator marked";
    if (argc > 3) {
        detail[0] = '\0';
        for (int index = 3; index < argc; ++index) {
            const size_t used = strlen(detail);
            snprintf(detail + used, sizeof(detail) - used, "%s%s",
                     used > 0 ? " " : "", argv[index]);
            if (strlen(detail) == sizeof(detail) - 1) {
                break;
            }
        }
    }
    factory_report_set(test, status, detail);
    factory_report_print_one(test);
    return ESP_OK;
}


esp_err_t factory_diag_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "sys_tasks", .help = "Print per-task CPU usage and stack high-water marks: sys_tasks [PERIOD_MS 100-10000].", .func = command_sys_tasks},
        {.command = "mark", .help = "Record a manual result: mark TEST pass|fail|skip [detail].", .func = command_mark},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
