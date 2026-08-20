/*
 * Candis-S31 watch demo - SD card storage service implementation.
 *
 * Card-detect (GPIO0, active low) is polled once per second. Insertion
 * auto-mounts through the BSP and emits MOUNTED; removal unmounts and
 * emits UNMOUNTED. A failed mount is reported as MOUNT_FAIL and retried
 * on a slower cadence so a broken card cannot hammer the SDMMC bus every
 * second. Capacity is refreshed from FATFS in the service task context
 * (UI callers get a non-blocking snapshot).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc_storage.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#define STORAGE_TASK_STACK_BYTES 4096
#define STORAGE_TASK_PRIORITY    3
#define STORAGE_POLL_MS          1000
/* Throttle retries after a failed mount (bad/no-init card). */
#define MOUNT_RETRY_TICKS        5
/* Capacity re-read cadence while mounted. */
#define INFO_REFRESH_TICKS       5

static const char *TAG = "svc_storage";

static svc_sd_cb_t s_callback;
static void *s_user;
static TaskHandle_t s_task;
static bool s_started;
static volatile bool s_mounted;

/* Capacity snapshot guarded by a spinlock: writers are the service task
 * only, readers may be any task (UI thread included). */
static portMUX_TYPE s_info_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_total_bytes;
static uint64_t s_free_bytes;
static volatile bool s_info_valid;

static void storage_emit(svc_sd_event_t ev)
{
    if (s_callback != NULL) {
        s_callback(ev, s_user);
    }
}

/* Read FAT capacity for the mount point. Runs in the service task only,
 * so SD I/O never blocks the UI thread. */
static void info_refresh(void)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    const esp_err_t err =
        esp_vfs_fat_info(svc_storage_mount_point(), &total, &free_bytes);
    portENTER_CRITICAL(&s_info_lock);
    if (err == ESP_OK) {
        s_total_bytes = total;
        s_free_bytes = free_bytes;
        s_info_valid = true;
    } else {
        s_info_valid = false;
    }
    portEXIT_CRITICAL(&s_info_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "capacity read failed: %s", esp_err_to_name(err));
    }
}

static void storage_task(void *arg)
{
    (void)arg;
    int retry_countdown = 0;
    int info_countdown = 0;
    bool fail_reported = false; /* one MOUNT_FAIL report per broken card */

    for (;;) {
        const bool inserted = bsp_sdcard_is_inserted();

        if (inserted && !s_mounted) {
            if (retry_countdown > 0) {
                --retry_countdown;
            } else {
                const esp_err_t err = bsp_sdcard_mount();
                if (err == ESP_OK) {
                    s_mounted = true;
                    retry_countdown = 0;
                    fail_reported = false;
                    info_countdown = 0;
                    info_refresh();
                    ESP_LOGI(TAG, "TF card mounted at %s",
                             svc_storage_mount_point());
                    storage_emit(SVC_SD_EV_MOUNTED);
                } else {
                    ESP_LOGW(TAG, "TF card mount failed: %s", esp_err_to_name(err));
                    retry_countdown = MOUNT_RETRY_TICKS;
                    if (!fail_reported) {
                        fail_reported = true;
                        storage_emit(SVC_SD_EV_MOUNT_FAIL);
                    }
                }
            }
        } else if (!inserted && s_mounted) {
            const esp_err_t err = bsp_sdcard_unmount();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "TF card unmount failed: %s", esp_err_to_name(err));
            }
            s_mounted = false;
            retry_countdown = 0;
            fail_reported = false;
            portENTER_CRITICAL(&s_info_lock);
            s_info_valid = false;
            portEXIT_CRITICAL(&s_info_lock);
            ESP_LOGI(TAG, "TF card removed");
            storage_emit(SVC_SD_EV_UNMOUNTED);
        } else if (!inserted) {
            /* Card removed while sitting in the mount-fail retry loop (never
             * mounted): re-arm the failure report for the next card. */
            fail_reported = false;
        } else if (inserted && s_mounted) {
            if (--info_countdown <= 0) {
                info_countdown = INFO_REFRESH_TICKS;
                info_refresh();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(STORAGE_POLL_MS));
    }
}

esp_err_t svc_storage_start(svc_sd_cb_t cb, void *user)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_callback = cb;
    s_user = user;
    if (xTaskCreate(storage_task, "svc_storage", STORAGE_TASK_STACK_BYTES,
                    NULL, STORAGE_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "storage service started (poll %d ms)", STORAGE_POLL_MS);
    return ESP_OK;
}

bool svc_storage_mounted(void)
{
    return s_mounted;
}

const char *svc_storage_mount_point(void)
{
    return BSP_SD_MOUNT_POINT;
}

esp_err_t svc_storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_info_lock);
    const bool valid = s_info_valid;
    if (valid) {
        if (total_bytes != NULL) {
            *total_bytes = s_total_bytes;
        }
        if (free_bytes != NULL) {
            *free_bytes = s_free_bytes;
        }
    }
    portEXIT_CRITICAL(&s_info_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}
