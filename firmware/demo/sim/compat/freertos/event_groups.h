/*
 * Candis-S31 simulator - FreeRTOS event group shim (POSIX).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t EventBits_t;
typedef struct sim_event_group *EventGroupHandle_t;

EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                BaseType_t clear_on_exit,
                                BaseType_t wait_for_all,
                                TickType_t ticks_to_wait);
void vEventGroupDelete(EventGroupHandle_t group);

#ifdef __cplusplus
}
#endif
