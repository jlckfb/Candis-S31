/*
 * Candis-S31 player demo - SD card storage service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage_service.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "player_storage";

#define TASK_STACK_BYTES 4096
#define TASK_PRIORITY    3
#define POLL_MS          1000
#define MOUNT_RETRY_MS   5000
#define IO_IDLE_BIT      BIT0

static storage_cb_t s_callback;
static void *s_user;
static TaskHandle_t s_task;
static bool s_started;
static SemaphoreHandle_t s_state_mutex;
static EventGroupHandle_t s_io_events;
static bool s_mounted;
static bool s_removing;
static uint32_t s_active_ops;

static void emit(storage_event_t ev)
{
    if (s_callback != NULL) {
        s_callback(ev, s_user);
    }
}

static bool state_locked(bool *removing, uint32_t *active)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool mounted = s_mounted && !s_removing;
    if (removing != NULL) {
        *removing = s_removing;
    }
    if (active != NULL) {
        *active = s_active_ops;
    }
    xSemaphoreGive(s_state_mutex);
    return mounted;
}

static void task(void *arg)
{
    (void)arg;
    int mount_retry_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        bool card_present = bsp_sdcard_is_inserted();
        bool mounted = state_locked(NULL, NULL);

        if (card_present && !mounted) {
            if (mount_retry_ms > 0) {
                mount_retry_ms -= POLL_MS;
                continue;
            }
            const esp_err_t err = bsp_sdcard_mount();
            if (err == ESP_OK) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_mounted = true;
                s_removing = false;
                xSemaphoreGive(s_state_mutex);
                ESP_LOGI(TAG, "TF card mounted at %s", BSP_SD_MOUNT_POINT);
                emit(STORAGE_EV_MOUNTED);
            } else {
                ESP_LOGW(TAG, "TF mount failed: %s", esp_err_to_name(err));
                emit(STORAGE_EV_MOUNT_FAIL);
                mount_retry_ms = MOUNT_RETRY_MS;
            }
        } else if (!card_present && mounted) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_removing = true;
            const uint32_t active = s_active_ops;
            xSemaphoreGive(s_state_mutex);
            if (active == 0) {
                const esp_err_t err = bsp_sdcard_unmount();
                if (err == ESP_OK) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_mounted = false;
                    s_removing = false;
                    xSemaphoreGive(s_state_mutex);
                    ESP_LOGI(TAG, "TF card unmounted");
                    emit(STORAGE_EV_UNMOUNTED);
                } else {
                    ESP_LOGW(TAG, "TF unmount failed: %s", esp_err_to_name(err));
                }
            }
        }
    }
}

esp_err_t storage_start(storage_cb_t cb, void *user)
{
    if (s_started) {
        return ESP_OK;
    }
    s_callback = cb;
    s_user = user;
    s_state_mutex = xSemaphoreCreateMutex();
    s_io_events = xEventGroupCreate();
    if (s_state_mutex == NULL || s_io_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_io_events, IO_IDLE_BIT);
    if (xTaskCreate(task, "player_storage", TASK_STACK_BYTES, NULL,
                    TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

bool storage_mounted(void)
{
    if (s_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool mounted = s_mounted && !s_removing;
    xSemaphoreGive(s_state_mutex);
    return mounted;
}

const char *storage_mount_point(void)
{
    return BSP_SD_MOUNT_POINT;
}

esp_err_t storage_lease_acquire(storage_lease_t *lease)
{
    if (lease == NULL || lease->held) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_mounted || s_removing) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_active_ops++ == 0) {
        xEventGroupClearBits(s_io_events, IO_IDLE_BIT);
    }
    lease->held = true;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

void storage_lease_release(storage_lease_t *lease)
{
    if (lease == NULL || !lease->held) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    lease->held = false;
    if (s_active_ops == 0) {
        ESP_LOGE(TAG, "unbalanced lease release");
    } else if (--s_active_ops == 0) {
        xEventGroupSetBits(s_io_events, IO_IDLE_BIT);
    }
    xSemaphoreGive(s_state_mutex);
}
