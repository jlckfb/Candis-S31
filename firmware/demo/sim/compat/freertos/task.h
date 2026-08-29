/*
 * Candis-S31 simulator - FreeRTOS task shim (POSIX threads).
 *
 * Tasks map 1:1 to pthreads; direct task notifications are a per-task
 * counting semaphore. Tick resolution is 1 ms (pdMS_TO_TICKS is the
 * identity mapping from FreeRTOS.h).
 *
 * Under __EMSCRIPTEN__ the binary is built with -sUSE_PTHREADS, so the
 * same pthread implementation applies to the web preview.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*TaskFunction_t)(void *arg);
#define tskIDLE_PRIORITY 0U

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                       uint32_t stack_depth, void *arg, UBaseType_t prio,
                       TaskHandle_t *out);
/* The core id is accepted and ignored; the sim runs on host cores. */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   uint32_t stack_depth, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out,
                                   UBaseType_t core_id);

/* Only self-deletion (handle == NULL or own handle) is supported, matching
 * every demo call site. */
void vTaskDelete(TaskHandle_t task);

void vTaskDelay(TickType_t ticks);

TickType_t xTaskGetTickCount(void);

TaskHandle_t xTaskGetCurrentTaskHandle(void);

/* Suspending another task is not needed by the demo; logs and no-ops. */
void vTaskSuspend(TaskHandle_t task);

/* Direct task notification (counting mode, the only mode the demo uses). */
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit,
                          TickType_t ticks_to_wait);
/* Task snapshot for the sysinfo page. The shim keeps a registry of live
 * tasks; high-water marks are estimated as half the requested stack. */
typedef struct {
    TaskHandle_t xHandle;
    char pcTaskName[16];
    UBaseType_t uxCurrentPriority;
    uint16_t usStackHighWaterMark;
} TaskStatus_t;

UBaseType_t uxTaskGetNumberOfTasks(void);
UBaseType_t uxTaskGetSystemState(TaskStatus_t *task_status_array,
                                 UBaseType_t array_size,
                                 uint32_t *total_run_time);

#ifdef __cplusplus
}
#endif
