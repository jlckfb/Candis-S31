#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef void *TaskHandle_t;

#define pdTRUE       1
#define pdFALSE      0
#define pdPASS       1
#define pdFAIL       0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef struct sim_queue {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    size_t length;
    size_t item_size;
    size_t head;
    size_t count;
    uint8_t *storage;
} StaticQueue_t;

typedef StaticQueue_t *QueueHandle_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER

#define portENTER_CRITICAL(mux) pthread_mutex_lock((mux))
#define portEXIT_CRITICAL(mux) pthread_mutex_unlock((mux))

#ifdef __cplusplus
}
#endif
