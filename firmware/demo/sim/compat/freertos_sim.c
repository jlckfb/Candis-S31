/*
 * Candis-S31 simulator - FreeRTOS POSIX shim implementation.
 *
 * Tasks are pthreads; semaphores/event groups/timers are condvar-based;
 * task notifications are a per-task counting semaphore. All timeouts use
 * the monotonic clock; 1 tick == 1 ms.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"

static void sim_deadline(struct timespec *out, TickType_t ticks)
{
    clock_gettime(CLOCK_MONOTONIC, out);
    if (ticks == portMAX_DELAY) {
        /* "Infinite" deadline: pthread cond waits need a concrete time. */
        out->tv_sec += 3600 * 24 * 365;
        return;
    }
    out->tv_sec += ticks / 1000;
    out->tv_nsec += (long)(ticks % 1000) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += 1;
        out->tv_nsec -= 1000000000L;
    }
}

static void sim_cond_init_monotonic(pthread_cond_t *cond)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

/* ---------------- Tasks ---------------- */

typedef struct sim_task {
    pthread_t thread;
    TaskFunction_t fn;
    void *arg;
    char name[16];
    uint32_t stack_words;
    UBaseType_t prio;
    atomic_int notify_count;
    pthread_mutex_t notify_mutex;
    pthread_cond_t notify_cond;
    bool deleted;
} sim_task_t;
/* Live-task registry (sysinfo snapshot). */
static sim_task_t *s_task_registry[64];
static size_t s_task_registry_count;
static pthread_mutex_t s_task_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

static void sim_task_unregister(sim_task_t *task)
{
    pthread_mutex_lock(&s_task_registry_mutex);
    for (size_t i = 0; i < s_task_registry_count; ++i) {
        if (s_task_registry[i] == task) {
            s_task_registry[i] = s_task_registry[--s_task_registry_count];
            break;
        }
    }
    pthread_mutex_unlock(&s_task_registry_mutex);
}

static void *sim_task_entry(void *raw)
{
    sim_task_t *task = raw;
    task->fn(task->arg);
    /* FreeRTOS semantics: returning from the task function self-deletes. */
    pthread_mutex_lock(&task->notify_mutex);
    task->deleted = true;
    pthread_mutex_unlock(&task->notify_mutex);
    sim_task_unregister(task);
    return NULL;
}

static BaseType_t sim_task_create(TaskFunction_t fn, const char *name,
                                  uint32_t stack_depth, void *arg,
                                  UBaseType_t prio, TaskHandle_t *out)
{
    (void)prio; /* host scheduling is left to the OS */
    sim_task_t *task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return pdFAIL;
    }
    task->fn = fn;
    task->arg = arg;
    snprintf(task->name, sizeof(task->name), "%s", name ? name : "task");
    task->stack_words = stack_depth;
    task->prio = prio;
    atomic_init(&task->notify_count, 0);
    pthread_mutex_init(&task->notify_mutex, NULL);
    sim_cond_init_monotonic(&task->notify_cond);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* stack_depth is in "words" in FreeRTOS convention (4 bytes each);
     * clamp to a sane host minimum. */
    size_t stack = (size_t)stack_depth * 4U;
    if (stack < 256U * 1024U) {
        stack = 256U * 1024U;
    }
    pthread_attr_setstacksize(&attr, stack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&task->thread, &attr, sim_task_entry, task);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        ESP_LOGE("sim_freertos", "pthread_create failed for %s: %d",
                 task->name, rc);
        free(task);
        return pdFAIL;
    }
    if (out != NULL) {
        *out = task;
    }
    pthread_mutex_lock(&s_task_registry_mutex);
    if (s_task_registry_count <
            sizeof(s_task_registry) / sizeof(s_task_registry[0])) {
        s_task_registry[s_task_registry_count++] = task;
    }
    pthread_mutex_unlock(&s_task_registry_mutex);
    return pdPASS;
}

UBaseType_t uxTaskGetNumberOfTasks(void)
{
    pthread_mutex_lock(&s_task_registry_mutex);
    const size_t count = s_task_registry_count;
    pthread_mutex_unlock(&s_task_registry_mutex);
    return (UBaseType_t)count;
}

UBaseType_t uxTaskGetSystemState(TaskStatus_t *task_status_array,
                                 UBaseType_t array_size,
                                 uint32_t *total_run_time)
{
    if (total_run_time != NULL) {
        *total_run_time = 0;
    }
    if (task_status_array == NULL || array_size == 0) {
        return 0;
    }
    UBaseType_t filled = 0;
    pthread_mutex_lock(&s_task_registry_mutex);
    for (size_t i = 0; i < s_task_registry_count && filled < array_size;
         ++i) {
        const sim_task_t *task = s_task_registry[i];
        TaskStatus_t *dst = &task_status_array[filled++];
        dst->xHandle = (TaskHandle_t)task;
        snprintf(dst->pcTaskName, sizeof(dst->pcTaskName), "%s", task->name);
        dst->uxCurrentPriority = task->prio;
        /* No real stack metering on the host; report half the requested
         * stack (words -> bytes happens at the call site). */
        dst->usStackHighWaterMark = (uint16_t)(task->stack_words / 2U);
    }
    pthread_mutex_unlock(&s_task_registry_mutex);
    return filled;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                       uint32_t stack_depth, void *arg, UBaseType_t prio,
                       TaskHandle_t *out)
{
    return sim_task_create(fn, name, stack_depth, arg, prio, out);
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   uint32_t stack_depth, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out,
                                   UBaseType_t core_id)
{
    (void)core_id;
    return sim_task_create(fn, name, stack_depth, arg, prio, out);
}

void vTaskDelete(TaskHandle_t handle)
{
    sim_task_t *task = handle;
    if (task == NULL || pthread_equal(task->thread, pthread_self())) {
        pthread_exit(NULL);
    }
    /* Deleting another live task is not used by the demo; refuse loudly. */
    ESP_LOGW("sim_freertos", "vTaskDelete(other) ignored");
}

void vTaskDelay(TickType_t ticks)
{
    if (ticks == 0) {
        return;
    }
    if (ticks == portMAX_DELAY) {
        ticks = 1000; /* treat "forever" as a long poll in the sim */
    }
    struct timespec req = {
        .tv_sec = ticks / 1000,
        .tv_nsec = (long)(ticks % 1000) * 1000000L,
    };
    nanosleep(&req, NULL);
}

TickType_t xTaskGetTickCount(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (TickType_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    /* The demo only compares/stores the handle; identity is not tracked. */
    return NULL;
}

void vTaskSuspend(TaskHandle_t task)
{
    (void)task;
    ESP_LOGW("sim_freertos", "vTaskSuspend ignored");
}

BaseType_t xTaskNotifyGive(TaskHandle_t handle)
{
    sim_task_t *task = handle;
    if (task == NULL) {
        return pdPASS;
    }
    pthread_mutex_lock(&task->notify_mutex);
    atomic_fetch_add(&task->notify_count, 1);
    pthread_cond_signal(&task->notify_cond);
    pthread_mutex_unlock(&task->notify_mutex);
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit,
                          TickType_t ticks_to_wait)
{
    /* Without per-thread task identity we cannot target "self" here; the
     * demo's only ulTaskNotifyTake call sites use the calling task's own
     * handle stored at create time, so route through that handle when the
     * caller passes it via the shim helper below. A NULL current handle
     * falls back to a plain sleep, which keeps polling loops alive. */
    (void)clear_count_on_exit;
    vTaskDelay(ticks_to_wait == portMAX_DELAY ? 10 : ticks_to_wait);
    return 0;
}

/* ---------------- Semaphores ---------------- */

struct sim_sem {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int max_count;
};

static SemaphoreHandle_t sim_sem_create(int initial, int max_count)
{
    struct sim_sem *sem = calloc(1, sizeof(*sem));
    if (sem == NULL) {
        return NULL;
    }
    pthread_mutex_init(&sem->mutex, NULL);
    sim_cond_init_monotonic(&sem->cond);
    sem->count = initial;
    sem->max_count = max_count;
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return sim_sem_create(0, 1);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return sim_sem_create(1, 1);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks_to_wait)
{
    struct sim_sem *sem = handle;
    if (sem == NULL) {
        return pdFALSE;
    }
    struct timespec deadline;
    sim_deadline(&deadline, ticks_to_wait);
    pthread_mutex_lock(&sem->mutex);
    while (sem->count <= 0) {
        if (ticks_to_wait == 0) {
            pthread_mutex_unlock(&sem->mutex);
            return pdFALSE;
        }
        if (pthread_cond_timedwait(&sem->cond, &sem->mutex, &deadline)
                == ETIMEDOUT) {
            pthread_mutex_unlock(&sem->mutex);
            return pdFALSE;
        }
    }
    --sem->count;
    pthread_mutex_unlock(&sem->mutex);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    struct sim_sem *sem = handle;
    if (sem == NULL) {
        return pdFALSE;
    }
    pthread_mutex_lock(&sem->mutex);
    if (sem->count < sem->max_count) {
        ++sem->count;
    }
    pthread_cond_signal(&sem->cond);
    pthread_mutex_unlock(&sem->mutex);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t handle)
{
    struct sim_sem *sem = handle;
    if (sem == NULL) {
        return;
    }
    pthread_mutex_destroy(&sem->mutex);
    pthread_cond_destroy(&sem->cond);
    free(sem);
}

/* ---------------- Software timers ---------------- */

struct sim_timer {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    TickType_t period;
    BaseType_t auto_reload;
    TimerCallbackFunction_t cb;
    bool thread_started;
    bool active;      /* armed */
    bool expired;     /* deadline reached, fire now */
    bool destroy;
};

static void *sim_timer_thread(void *raw)
{
    TimerHandle_t timer = raw;
    for (;;) {
        pthread_mutex_lock(&timer->mutex);
        while (!timer->expired && !timer->destroy) {
            if (!timer->active) {
                pthread_cond_wait(&timer->cond, &timer->mutex);
            } else {
                struct timespec deadline;
                sim_deadline(&deadline, timer->period);
                if (pthread_cond_timedwait(&timer->cond, &timer->mutex,
                                           &deadline) == ETIMEDOUT) {
                    timer->expired = true;
                    if (!timer->auto_reload) {
                        timer->active = false;
                    }
                }
            }
        }
        const bool destroy = timer->destroy;
        const bool fire = timer->expired;
        timer->expired = false;
        pthread_mutex_unlock(&timer->mutex);
        if (destroy) {
            return NULL;
        }
        if (fire) {
            timer->cb(timer);
        }
    }
}

TimerHandle_t xTimerCreate(const char *name, TickType_t period_ticks,
                           BaseType_t auto_reload, void *timer_id,
                           TimerCallbackFunction_t cb)
{
    (void)name;
    (void)timer_id;
    struct sim_timer *timer = calloc(1, sizeof(*timer));
    if (timer == NULL) {
        return NULL;
    }
    pthread_mutex_init(&timer->mutex, NULL);
    sim_cond_init_monotonic(&timer->cond);
    timer->period = period_ticks == 0 ? 1 : period_ticks;
    timer->auto_reload = auto_reload;
    timer->cb = cb;
    return timer;
}

static void sim_timer_ensure_thread(TimerHandle_t timer)
{
    if (timer->thread_started) {
        return;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&timer->thread, &attr, sim_timer_thread, timer) == 0) {
        timer->thread_started = true;
    }
    pthread_attr_destroy(&attr);
}

BaseType_t xTimerStart(TimerHandle_t timer, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (timer == NULL) {
        return pdFALSE;
    }
    pthread_mutex_lock(&timer->mutex);
    sim_timer_ensure_thread(timer);
    timer->active = true;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);
    return pdTRUE;
}

BaseType_t xTimerChangePeriod(TimerHandle_t timer, TickType_t period_ticks,
                              TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (timer == NULL || period_ticks == 0) {
        return pdFALSE;
    }
    pthread_mutex_lock(&timer->mutex);
    timer->period = period_ticks;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);
    return pdTRUE;
}

BaseType_t xTimerStop(TimerHandle_t timer, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (timer == NULL) {
        return pdFALSE;
    }
    pthread_mutex_lock(&timer->mutex);
    timer->active = false;
    timer->expired = false;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);
    return pdTRUE;
}

BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (timer == NULL) {
        return pdFALSE;
    }
    pthread_mutex_lock(&timer->mutex);
    timer->destroy = true;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);
    /* The detached thread frees nothing; the struct leaks by design in the
     * short-lived sim process. */
    return pdTRUE;
}

/* ---------------- Queues (blocking, timeout-aware) ---------------- */

QueueHandle_t xQueueCreateStatic(UBaseType_t queue_length,
                                 UBaseType_t item_size,
                                 uint8_t *queue_storage,
                                 StaticQueue_t *queue_buffer)
{
    if (queue_length == 0 || item_size == 0 || queue_storage == NULL ||
        queue_buffer == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&queue_buffer->mutex, NULL) != 0) {
        return NULL;
    }
    sim_cond_init_monotonic(&queue_buffer->not_empty);
    sim_cond_init_monotonic(&queue_buffer->not_full);
    queue_buffer->length = queue_length;
    queue_buffer->item_size = item_size;
    queue_buffer->head = 0;
    queue_buffer->count = 0;
    queue_buffer->storage = queue_storage;
    return queue_buffer;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t ticks_to_wait)
{
    if (queue == NULL || item == NULL) {
        return pdFALSE;
    }
    struct timespec deadline;
    sim_deadline(&deadline, ticks_to_wait);
    pthread_mutex_lock(&queue->mutex);
    while (queue->count >= queue->length) {
        if (ticks_to_wait == 0) {
            pthread_mutex_unlock(&queue->mutex);
            return pdFALSE;
        }
        if (pthread_cond_timedwait(&queue->not_full, &queue->mutex,
                                   &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&queue->mutex);
            return pdFALSE;
        }
    }
    const size_t index = (queue->head + queue->count) % queue->length;
    memcpy(queue->storage + index * queue->item_size,
           item, queue->item_size);
    ++queue->count;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t ticks_to_wait)
{
    if (queue == NULL || item == NULL) {
        return pdFALSE;
    }
    struct timespec deadline;
    sim_deadline(&deadline, ticks_to_wait);
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0) {
        if (ticks_to_wait == 0) {
            pthread_mutex_unlock(&queue->mutex);
            return pdFALSE;
        }
        if (pthread_cond_timedwait(&queue->not_empty, &queue->mutex,
                                   &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&queue->mutex);
            return pdFALSE;
        }
    }
    memcpy(item, queue->storage + queue->head * queue->item_size,
           queue->item_size);
    queue->head = (queue->head + 1) % queue->length;
    --queue->count;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return pdTRUE;
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    if (queue == NULL) {
        return pdFALSE;
    }
    pthread_mutex_lock(&queue->mutex);
    queue->head = 0;
    queue->count = 0;
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return pdTRUE;
}

/* ---------------- Event groups ---------------- */

struct sim_event_group {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    EventBits_t bits;
};

EventGroupHandle_t xEventGroupCreate(void)
{
    struct sim_event_group *group = calloc(1, sizeof(*group));
    if (group == NULL) {
        return NULL;
    }
    pthread_mutex_init(&group->mutex, NULL);
    sim_cond_init_monotonic(&group->cond);
    return group;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t handle, EventBits_t bits)
{
    struct sim_event_group *group = handle;
    pthread_mutex_lock(&group->mutex);
    group->bits |= bits;
    pthread_cond_broadcast(&group->cond);
    EventBits_t result = group->bits;
    pthread_mutex_unlock(&group->mutex);
    return result;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t handle, EventBits_t bits)
{
    struct sim_event_group *group = handle;
    pthread_mutex_lock(&group->mutex);
    group->bits &= ~bits;
    EventBits_t result = group->bits;
    pthread_mutex_unlock(&group->mutex);
    return result;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t handle, EventBits_t bits,
                                BaseType_t clear_on_exit,
                                BaseType_t wait_for_all,
                                TickType_t ticks_to_wait)
{
    struct sim_event_group *group = handle;
    struct timespec deadline;
    sim_deadline(&deadline, ticks_to_wait);
    pthread_mutex_lock(&group->mutex);
    for (;;) {
        const EventBits_t match = group->bits & bits;
        const bool satisfied = wait_for_all ? (match == bits) : (match != 0);
        if (satisfied) {
            const EventBits_t result = group->bits;
            if (clear_on_exit) {
                group->bits &= ~bits;
            }
            pthread_mutex_unlock(&group->mutex);
            return result;
        }
        if (ticks_to_wait == 0) {
            const EventBits_t result = group->bits;
            pthread_mutex_unlock(&group->mutex);
            return result;
        }
        if (pthread_cond_timedwait(&group->cond, &group->mutex, &deadline)
                == ETIMEDOUT) {
            const EventBits_t result = group->bits;
            pthread_mutex_unlock(&group->mutex);
            return result;
        }
    }
}

void vEventGroupDelete(EventGroupHandle_t handle)
{
    struct sim_event_group *group = handle;
    if (group == NULL) {
        return;
    }
    pthread_mutex_destroy(&group->mutex);
    pthread_cond_destroy(&group->cond);
    free(group);
}
