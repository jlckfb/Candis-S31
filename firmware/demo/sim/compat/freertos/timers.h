/*
 * Candis-S31 simulator - FreeRTOS software timer shim (POSIX).
 *
 * One pthread per active timer; callbacks run on that thread, which is
 * close enough to the FreeRTOS timer-daemon contract for the demo (the
 * callback context just must not be the LVGL thread).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_timer *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t timer);

TimerHandle_t xTimerCreate(const char *name, TickType_t period_ticks,
                           BaseType_t auto_reload, void *timer_id,
                           TimerCallbackFunction_t cb);
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t ticks_to_wait);
BaseType_t xTimerChangePeriod(TimerHandle_t timer, TickType_t period_ticks,
                              TickType_t ticks_to_wait);
BaseType_t xTimerStop(TimerHandle_t timer, TickType_t ticks_to_wait);
BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
