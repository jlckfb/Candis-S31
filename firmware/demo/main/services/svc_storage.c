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

#include <inttypes.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define STORAGE_TASK_STACK_BYTES 4096
#define STORAGE_TASK_PRIORITY    3
#define STORAGE_POLL_MS          1000
/* Throttle retries after a failed mount (bad/no-init card). */
#define MOUNT_RETRY_TICKS        5
/* Capacity re-read cadence while mounted. */
#define INFO_REFRESH_TICKS       5
#define UNMOUNT_DRAIN_MS         3000
#define STORAGE_IO_IDLE_BIT      BIT0

static const char *TAG = "svc_storage";

static svc_sd_cb_t s_callback;
static void *s_user;
static TaskHandle_t s_task;
static bool s_started;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_unmount_mutex;
static EventGroupHandle_t s_io_events;
static bool s_mounted;
static bool s_removing;
/* Final safe-state gate. Unlike a physical-card removal, insertion must not
 * cancel this state or allow a remount before rails are removed. */
static bool s_quiescing;
static uint32_t s_active_ops;

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
    svc_storage_lease_t lease = {0};
    if (svc_storage_lease_acquire(&lease) != ESP_OK) {
        return;
    }
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
    svc_storage_lease_release(&lease);
}

static void storage_set_mounted(bool mounted)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_mounted = mounted;
    s_removing = false;
    if (mounted) {
        s_quiescing = false;
    }
    xSemaphoreGive(s_state_mutex);
}

static bool storage_state(bool *removing, bool *quiescing,
                          uint32_t *active_ops)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool mounted = s_mounted;
    if (removing != NULL) {
        *removing = s_removing;
    }
    if (quiescing != NULL) {
        *quiescing = s_quiescing;
    }
    if (active_ops != NULL) {
        *active_ops = s_active_ops;
    }
    xSemaphoreGive(s_state_mutex);
    return mounted;
}

/* Close the lease gate before waiting. Once removing is true, active_ops can
 * only decrease, so observing the idle event proves no live FILE/DIR remains. */
static bool storage_drain_io(bool final_quiesce)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (final_quiesce) {
        s_quiescing = true;
    }
    s_removing = true;
    const uint32_t active = s_active_ops;
    xSemaphoreGive(s_state_mutex);

    if (active == 0) {
        return true;
    }
    ESP_LOGW(TAG, "TF removal waiting for %" PRIu32 " active I/O lease(s)",
             active);
    const EventBits_t bits = xEventGroupWaitBits(
        s_io_events, STORAGE_IO_IDLE_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(UNMOUNT_DRAIN_MS));
    if ((bits & STORAGE_IO_IDLE_BIT) != 0) {
        ESP_LOGI(TAG, "TF I/O leases drained before unmount");
        return true;
    }

    uint32_t remaining = 0;
    storage_state(NULL, NULL, &remaining);
    ESP_LOGE(TAG, "TF unmount deferred after %d ms; %" PRIu32
             " I/O lease(s) still active", UNMOUNT_DRAIN_MS, remaining);
    return false;
}

static void storage_cancel_removal(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_quiescing) {
        s_removing = false;
    }
    xSemaphoreGive(s_state_mutex);
}

static void storage_unmount_complete(void)
{
    storage_set_mounted(false);
    portENTER_CRITICAL(&s_info_lock);
    s_info_valid = false;
    portEXIT_CRITICAL(&s_info_lock);
}

static void storage_task(void *arg)
{
    (void)arg;
    int retry_countdown = 0;
    int unmount_retry_countdown = 0;
    int info_countdown = 0;
    bool fail_reported = false; /* one MOUNT_FAIL report per broken card */

    for (;;) {
        const bool inserted = bsp_sdcard_is_inserted();
        bool removing = false;
        bool quiescing = false;
        const bool mounted = storage_state(&removing, &quiescing, NULL);

        if (quiescing) {
            /* A final power transition owns the mount from this point. Do
             * not remount, refresh capacity, or cancel its closed gate. */
            vTaskDelay(pdMS_TO_TICKS(STORAGE_POLL_MS));
            continue;
        }

        if (inserted && !mounted) {
            if (retry_countdown > 0) {
                --retry_countdown;
            } else {
                const esp_err_t err = bsp_sdcard_mount();
                if (err == ESP_OK) {
                    storage_set_mounted(true);
                    retry_countdown = 0;
                    unmount_retry_countdown = 0;
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
        } else if (!inserted && mounted) {
            if (unmount_retry_countdown > 0) {
                --unmount_retry_countdown;
            } else {
                if (xSemaphoreTake(s_unmount_mutex,
                                   pdMS_TO_TICKS(UNMOUNT_DRAIN_MS)) != pdTRUE) {
                    ESP_LOGW(TAG, "TF unmount serializer busy; retrying");
                    unmount_retry_countdown = MOUNT_RETRY_TICKS;
                    vTaskDelay(pdMS_TO_TICKS(STORAGE_POLL_MS));
                    continue;
                }
                if (!storage_drain_io(false)) {
                    if (bsp_sdcard_is_inserted()) {
                        ESP_LOGI(TAG, "TF card detected again while draining; "
                                 "removal cancelled");
                        storage_cancel_removal();
                        unmount_retry_countdown = 0;
                    } else {
                        unmount_retry_countdown = MOUNT_RETRY_TICKS;
                    }
                    xSemaphoreGive(s_unmount_mutex);
                    vTaskDelay(pdMS_TO_TICKS(STORAGE_POLL_MS));
                    continue;
                }
                if (bsp_sdcard_is_inserted()) {
                    ESP_LOGI(TAG, "TF card detected again before unmount; "
                             "removal cancelled");
                    storage_cancel_removal();
                    unmount_retry_countdown = 0;
                    xSemaphoreGive(s_unmount_mutex);
                    vTaskDelay(pdMS_TO_TICKS(STORAGE_POLL_MS));
                    continue;
                }
                const esp_err_t err = bsp_sdcard_unmount();
                if (err != ESP_OK) {
                    /* Keep the mounted flag truthful. The BSP retains its
                     * card handle on failure, so reporting UNMOUNTED here
                     * would let UI code race a still-live VFS mount. */
                    ESP_LOGW(TAG, "TF card unmount failed; retrying: %s",
                             esp_err_to_name(err));
                    unmount_retry_countdown = MOUNT_RETRY_TICKS;
                } else {
                    storage_unmount_complete();
                    retry_countdown = 0;
                    unmount_retry_countdown = 0;
                    fail_reported = false;
                    ESP_LOGI(TAG, "TF card removed");
                    storage_emit(SVC_SD_EV_UNMOUNTED);
                }
                xSemaphoreGive(s_unmount_mutex);
            }
        } else if (!inserted) {
            /* Card removed while sitting in the mount-fail retry loop (never
             * mounted): re-arm the failure report for the next card. */
            fail_reported = false;
        } else if (inserted && mounted) {
            if (removing) {
                ESP_LOGI(TAG, "TF card detected again; removal cancelled");
                storage_cancel_removal();
            }
            unmount_retry_countdown = 0;
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
    s_state_mutex = xSemaphoreCreateMutex();
    s_unmount_mutex = xSemaphoreCreateMutex();
    s_io_events = xEventGroupCreate();
    if (s_state_mutex == NULL || s_unmount_mutex == NULL ||
            s_io_events == NULL) {
        if (s_io_events != NULL) {
            vEventGroupDelete(s_io_events);
            s_io_events = NULL;
        }
        if (s_state_mutex != NULL) {
            vSemaphoreDelete(s_state_mutex);
            s_state_mutex = NULL;
        }
        if (s_unmount_mutex != NULL) {
            vSemaphoreDelete(s_unmount_mutex);
            s_unmount_mutex = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_io_events, STORAGE_IO_IDLE_BIT);
    if (xTaskCreate(storage_task, "svc_storage", STORAGE_TASK_STACK_BYTES,
                    NULL, STORAGE_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        vEventGroupDelete(s_io_events);
        s_io_events = NULL;
        vSemaphoreDelete(s_unmount_mutex);
        s_unmount_mutex = NULL;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "storage service started (poll %d ms)", STORAGE_POLL_MS);
    return ESP_OK;
}

bool svc_storage_mounted(void)
{
    if (s_state_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool mounted = s_mounted && !s_removing && !s_quiescing;
    xSemaphoreGive(s_state_mutex);
    return mounted;
}

esp_err_t svc_storage_lease_acquire(svc_storage_lease_t *lease)
{
    if (lease == NULL || lease->held) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_mounted || s_removing || s_quiescing) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_active_ops++ == 0) {
        xEventGroupClearBits(s_io_events, STORAGE_IO_IDLE_BIT);
    }
    lease->held = true;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

void svc_storage_lease_release(svc_storage_lease_t *lease)
{
    if (lease == NULL || !lease->held || s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    lease->held = false;
    if (s_active_ops == 0) {
        ESP_LOGE(TAG, "unbalanced TF I/O lease release");
    } else if (--s_active_ops == 0) {
        xEventGroupSetBits(s_io_events, STORAGE_IO_IDLE_BIT);
    }
    xSemaphoreGive(s_state_mutex);
}

esp_err_t svc_storage_quiesce_and_unmount(void)
{
    if (s_state_mutex == NULL || s_unmount_mutex == NULL ||
            s_io_events == NULL) {
        /* The storage service is the sole owner of the BSP mount. If it was
         * never started, there is no service-managed filesystem to drain. */
        return ESP_OK;
    }

    /* Close the gate before waiting for the serializer. This prevents a new
     * lease or background remount from extending the shutdown indefinitely. */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_quiescing = true;
    s_removing = true;
    xSemaphoreGive(s_state_mutex);

    if (xSemaphoreTake(s_unmount_mutex,
                       pdMS_TO_TICKS(UNMOUNT_DRAIN_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "TF final unmount serializer timed out");
        return ESP_ERR_TIMEOUT;
    }

    bool mounted = storage_state(NULL, NULL, NULL);
    if (!mounted) {
        xSemaphoreGive(s_unmount_mutex);
        return ESP_OK;
    }

    if (!storage_drain_io(true)) {
        xSemaphoreGive(s_unmount_mutex);
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t err = bsp_sdcard_unmount();
    if (err != ESP_OK) {
        /* Keep both facts visible: the BSP still owns a live mount, but no
         * future service client may start I/O during a power transition. */
        ESP_LOGE(TAG, "TF final unmount failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_unmount_mutex);
        return err;
    }

    storage_unmount_complete();
    ESP_LOGI(TAG, "TF filesystem quiesced and unmounted");
    storage_emit(SVC_SD_EV_UNMOUNTED);
    xSemaphoreGive(s_unmount_mutex);
    return ESP_OK;
}

const char *svc_storage_mount_point(void)
{
    return BSP_SD_MOUNT_POINT;
}

esp_err_t svc_storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    svc_storage_lease_t lease = {0};
    const esp_err_t lease_result = svc_storage_lease_acquire(&lease);
    if (lease_result != ESP_OK) {
        return lease_result;
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
    svc_storage_lease_release(&lease);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}
