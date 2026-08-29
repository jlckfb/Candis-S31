#pragma once

#include <stdint.h>
#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

QueueHandle_t xQueueCreateStatic(UBaseType_t queue_length,
                                 UBaseType_t item_size,
                                 uint8_t *queue_storage,
                                 StaticQueue_t *queue_buffer);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t ticks_to_wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t ticks_to_wait);
BaseType_t xQueueReset(QueueHandle_t queue);

#ifdef __cplusplus
}
#endif
