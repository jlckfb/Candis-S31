/*
 * Candis-S31 simulator - FreeRTOS semaphore shim (POSIX).
 *
 * Binary semaphores and recursive-capable mutexes over pthread primitives;
 * take supports tick timeouts (1 tick = 1 ms).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_sem *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);

#ifdef __cplusplus
}
#endif
