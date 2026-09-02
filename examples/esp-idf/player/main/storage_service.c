/*
 * Candis-S31 player demo - SD card storage service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage_service.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "player_storage";

#define TASK_STACK_BYTES 4096
#define TASK_PRIORITY    3
#define POLL_MS          300
#define MOUNT_RETRY_MS   5000
#define DEMO_PATH        BSP_SD_MOUNT_POINT "/Candis_demo_460x460_30fps.avi"
#define DEMO_TMP         BSP_SD_MOUNT_POINT "/.Candis_demo.tmp"
#define MP4_PATH         BSP_SD_MOUNT_POINT "/Candis_reference_460x460_30fps.mp4"
#define MP4_TMP          BSP_SD_MOUNT_POINT "/.Candis_reference_mp4.tmp"

extern const uint8_t s_demo_start[]
    asm("_binary_Candis_demo_460x460_30fps_avi_start");
extern const uint8_t s_demo_end[]
    asm("_binary_Candis_demo_460x460_30fps_avi_end");
extern const uint8_t s_mp4_start[]
    asm("_binary_Candis_reference_460x460_30fps_mp4_start");
extern const uint8_t s_mp4_end[]
    asm("_binary_Candis_reference_460x460_30fps_mp4_end");

static storage_cb_t s_callback;
static void *s_user;
static TaskHandle_t s_task;
static bool s_started;
static SemaphoreHandle_t s_state_mutex;
static bool s_mounted;
static bool s_removing;
static uint32_t s_active_ops;

static void emit(storage_event_t ev)
{
    if (s_callback != NULL) {
        s_callback(ev, s_user);
    }
}

static bool state_locked(bool *removing, uint32_t *active, bool usable_only)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool mounted = s_mounted && (!usable_only || !s_removing);
    if (removing != NULL) {
        *removing = s_removing;
    }
    if (active != NULL) {
        *active = s_active_ops;
    }
    xSemaphoreGive(s_state_mutex);
    return mounted;
}

static esp_err_t mount_card(void)
{
    sdmmc_host_t host;
    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    bsp_sdcard_cfg_t config = {
        .host = &host,
    };
    esp_err_t error = bsp_sdcard_sdmmc_mount(&config);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "TF mounted in 4-bit high-speed mode (%d kHz)", host.max_freq_khz);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "40 MHz mount failed (%s), retrying BSP default 20 MHz",
             esp_err_to_name(error));
    return bsp_sdcard_mount();
}

static void task(void *arg)
{
    (void)arg;
    int mount_retry_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        bool card_present = bsp_sdcard_is_inserted();
        bool removing = false;
        uint32_t active = 0;
        bool mounted = state_locked(&removing, &active, false);

        if (mounted && removing && active == 0) {
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
        } else if (card_present && !mounted) {
            if (mount_retry_ms > 0) {
                mount_retry_ms -= POLL_MS;
                continue;
            }
            const esp_err_t err = mount_card();
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
            active = s_active_ops;
            xSemaphoreGive(s_state_mutex);
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
    if (s_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
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
    s_active_ops++;
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
    } else {
        s_active_ops--;
    }
    xSemaphoreGive(s_state_mutex);
}

static esp_err_t install_embedded_file(const char *path, const char *temporary,
                                       const uint8_t *start, const uint8_t *end)
{
    const size_t reference_size = (size_t)(end - start);
    struct stat status;
    if (stat(path, &status) == 0 &&
            (size_t)status.st_size == reference_size) {
        return ESP_OK;
    }
    esp_err_t error = ESP_OK;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    size_t written = 0;
    while (written < reference_size) {
        const size_t remaining = reference_size - written;
        const size_t block = remaining > 32768U ? 32768U : remaining;
        const size_t result = fwrite(start + written, 1, block, file);
        if (result != block) {
            error = ESP_FAIL;
            break;
        }
        written += result;
    }
    if (error == ESP_OK && (fflush(file) != 0 || fsync(fileno(file)) != 0)) {
        error = ESP_FAIL;
    }
    if (fclose(file) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }
    if (error == ESP_OK && rename(temporary, path) != 0) {
        error = ESP_FAIL;
    }
    if (error != ESP_OK) {
        unlink(temporary);
    } else {
        ESP_LOGI(TAG, "installed %s (%u bytes)", path, (unsigned)reference_size);
    }
    return error;
}

esp_err_t storage_install_reference_media(void)
{
    storage_lease_t lease = {0};
    esp_err_t error = storage_lease_acquire(&lease);
    if (error != ESP_OK) {
        return error;
    }
    unlink(BSP_SD_MOUNT_POINT "/Candis_reference_460x460_30fps.avi");
    error = install_embedded_file(DEMO_PATH, DEMO_TMP,
                                  s_demo_start, s_demo_end);
    if (error == ESP_OK) {
        error = install_embedded_file(MP4_PATH, MP4_TMP,
                                      s_mp4_start, s_mp4_end);
    }
    storage_lease_release(&lease);
    return error;
}
