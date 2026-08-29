/*
 * Candis-S31 simulator - SD card storage service mock.
 *
 * Backs the firmware storage service with a real host directory so the
 * app's genuine dirent/stat code paths run unchanged. The mount point is
 * ./candis_sim_sdcard (relative to the process cwd) on native builds and
 * /sdcard (MEMFS) under Emscripten.
 *
 * All callbacks are emitted from a dedicated service-task context, never
 * from the caller's thread, mirroring the real firmware; the mock itself
 * touches no LVGL API.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "services/svc_storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __EMSCRIPTEN__
#else
#include <sys/statvfs.h>
#endif

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "svc_storage_sim"

#ifdef __EMSCRIPTEN__
#define SIM_SD_MOUNT_POINT "/sdcard"
#else
#define SIM_SD_MOUNT_POINT "./candis_sim_sdcard"
#endif

/* 16 kHz, 16-bit, stereo PCM: 64000 data bytes per second, ~5 s of audio. */
#define SIM_WAV_SAMPLE_RATE   16000u
#define SIM_WAV_CHANNELS      2u
#define SIM_WAV_BITS          16u
#define SIM_WAV_DATA_BYTES    (320u * 1024u)

static SemaphoreHandle_t s_lock;
static svc_sd_cb_t s_cb;
static void *s_cb_user;
static bool s_mounted = true;
static bool s_gate_closed;
static unsigned s_lease_count;

/* --- little-endian helpers (no packed structs on the file boundary) ------ */

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* --- sample content seeding ---------------------------------------------- */

static esp_err_t sim_mkdir_p(const char *path)
{
    char tmp[256];
    size_t len = strlen(path);

    if (len >= sizeof(tmp)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return ESP_FAIL;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool sim_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static esp_err_t sim_write_wav(const char *path)
{
    /* 44-byte canonical PCM header. */
    uint8_t hdr[44];
    const uint32_t data_bytes = SIM_WAV_DATA_BYTES;
    const uint32_t byte_rate =
        SIM_WAV_SAMPLE_RATE * SIM_WAV_CHANNELS * (SIM_WAV_BITS / 8u);
    const uint16_t block_align =
        (uint16_t)(SIM_WAV_CHANNELS * (SIM_WAV_BITS / 8u));

    memcpy(hdr + 0, "RIFF", 4);
    put_le32(hdr + 4, 36u + data_bytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    put_le32(hdr + 16, 16u);                    /* fmt chunk size */
    put_le16(hdr + 20, 1u);                     /* PCM */
    put_le16(hdr + 22, (uint16_t)SIM_WAV_CHANNELS);
    put_le32(hdr + 24, SIM_WAV_SAMPLE_RATE);
    put_le32(hdr + 28, byte_rate);
    put_le16(hdr + 32, block_align);
    put_le16(hdr + 34, (uint16_t)SIM_WAV_BITS);
    memcpy(hdr + 36, "data", 4);
    put_le32(hdr + 40, data_bytes);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return ESP_FAIL;
    }
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return ESP_FAIL;
    }
    /* Silence payload: write zero blocks without a giant allocation. */
    uint8_t zeros[4096] = {0};
    uint32_t left = data_bytes;
    while (left > 0) {
        size_t chunk = left > sizeof(zeros) ? sizeof(zeros) : left;
        if (fwrite(zeros, 1, chunk, f) != chunk) {
            fclose(f);
            return ESP_FAIL;
        }
        left -= (uint32_t)chunk;
    }
    if (fclose(f) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t sim_write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return ESP_FAIL;
    }
    size_t len = strlen(text);
    if (fwrite(text, 1, len, f) != len) {
        fclose(f);
        return ESP_FAIL;
    }
    if (fclose(f) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void sim_seed_content(void)
{
    static const struct {
        const char *rel;
        int is_wav;
    } files[] = {
        {"music_01.wav", 1},
        {"voice_memo.wav", 1},
        {"REC_demo.wav", 1},
        {"Music/track_a.wav", 1},
        {"Music/track_b.wav", 1},
        {"readme.txt", 0},
        {"notes.txt", 0},
    };
    char path[256];

    (void)sim_mkdir_p(SIM_SD_MOUNT_POINT "/Photos");
    (void)sim_mkdir_p(SIM_SD_MOUNT_POINT "/Music");

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        int n = snprintf(path, sizeof(path), "%s/%s",
                         SIM_SD_MOUNT_POINT, files[i].rel);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        if (sim_file_exists(path)) {
            continue; /* keep user edits across restarts */
        }
        esp_err_t err;
        if (files[i].is_wav) {
            err = sim_write_wav(path);
        } else {
            err = sim_write_text(path,
                                 "Candis-S31 simulator seeded file.\r\n"
                                 "Safe to delete; it is regenerated when missing.\r\n");
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "seed %s failed: %s", path, esp_err_to_name(err));
        }
    }
}

/* --- service-task callback delivery -------------------------------------- */

typedef struct {
    svc_sd_cb_t cb;
    void *user;
    svc_sd_event_t ev;
} sim_sd_event_job_t;

static void sim_sd_event_task(void *arg)
{
    sim_sd_event_job_t *job = arg;

    /* Yield once so callers always see an async, service-context callback. */
    vTaskDelay(pdMS_TO_TICKS(10));
    if (job->cb != NULL) {
        job->cb(job->ev, job->user);
    }
    free(job);
    vTaskDelete(NULL);
}

static void sim_post_event(svc_sd_event_t ev)
{
    sim_sd_event_job_t *job = malloc(sizeof(*job));

    if (job == NULL) {
        ESP_LOGE(TAG, "no mem for event job");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    job->cb = s_cb;
    job->user = s_cb_user;
    xSemaphoreGive(s_lock);
    job->ev = ev;
    if (xTaskCreate(sim_sd_event_task, "svc_sd_ev", 4096, job, 3, NULL) !=
        pdPASS) {
        free(job);
        ESP_LOGE(TAG, "event task create failed");
    }
}

/* --- public API ------------------------------------------------------------ */

esp_err_t svc_storage_start(svc_sd_cb_t cb, void *user)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cb = cb;
    s_cb_user = user;
    s_mounted = true;
    xSemaphoreGive(s_lock);

    if (sim_mkdir_p(SIM_SD_MOUNT_POINT) != ESP_OK) {
        ESP_LOGE(TAG, "mount point %s unusable", SIM_SD_MOUNT_POINT);
        sim_post_event(SVC_SD_EV_MOUNT_FAIL);
        return ESP_FAIL;
    }
    sim_seed_content();
    ESP_LOGI(TAG, "mock TF mounted at %s", SIM_SD_MOUNT_POINT);
    sim_post_event(SVC_SD_EV_MOUNTED);
    return ESP_OK;
}

bool svc_storage_mounted(void)
{
    bool mounted;

    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mounted = s_mounted;
    xSemaphoreGive(s_lock);
    return mounted;
}

esp_err_t svc_storage_lease_acquire(svc_storage_lease_t *lease)
{
    if (lease == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_mounted || s_gate_closed) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    lease->held = true;
    s_lease_count++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void svc_storage_lease_release(svc_storage_lease_t *lease)
{
    if (lease == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (lease->held) {
        lease->held = false;
        if (s_lease_count > 0) {
            s_lease_count--;
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t svc_storage_quiesce_and_unmount(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_gate_closed = true;
    unsigned outstanding = s_lease_count;
    xSemaphoreGive(s_lock);

    /* Wait up to three seconds for leases to drain, like the real service. */
    for (int waited_ms = 0; outstanding > 0 && waited_ms < 3000;
         waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        outstanding = s_lease_count;
        xSemaphoreGive(s_lock);
    }
    if (outstanding > 0) {
        ESP_LOGW(TAG, "lease drain timeout (%u held)", outstanding);
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_mounted = s_mounted;
    s_mounted = false;
    xSemaphoreGive(s_lock);
    if (was_mounted) {
        sim_post_event(SVC_SD_EV_UNMOUNTED);
    }
    return ESP_OK;
}

const char *svc_storage_mount_point(void)
{
    return SIM_SD_MOUNT_POINT;
}

esp_err_t svc_storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (total_bytes == NULL || free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!svc_storage_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
#ifdef __EMSCRIPTEN__
    /* MEMFS has no meaningful statvfs; report the nominal FAT size. */
    *total_bytes = 32ull * 1024ull * 1024ull * 1024ull;
    *free_bytes = (*total_bytes * 60ull) / 100ull;
#else
    struct statvfs vfs;
    if (statvfs(SIM_SD_MOUNT_POINT, &vfs) == 0) {
        *total_bytes = (uint64_t)vfs.f_frsize * vfs.f_blocks;
        *free_bytes = (uint64_t)vfs.f_frsize * vfs.f_bavail;
    } else {
        *total_bytes = 32ull * 1024ull * 1024ull * 1024ull;
        *free_bytes = (*total_bytes * 60ull) / 100ull;
    }
#endif
    return ESP_OK;
}

/* --- simulator extensions -------------------------------------------------- */

void sim_storage_set_mounted(bool mounted)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_mounted != mounted;
    bool notify = changed && !s_gate_closed;
    s_mounted = mounted;
    xSemaphoreGive(s_lock);

    if (notify) {
        ESP_LOGI(TAG, "mock TF %s", mounted ? "inserted" : "removed");
        sim_post_event(mounted ? SVC_SD_EV_MOUNTED : SVC_SD_EV_UNMOUNTED);
    }
}
