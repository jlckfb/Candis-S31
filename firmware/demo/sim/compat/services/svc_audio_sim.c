/*
 * Candis-S31 simulator - audio service mock (no codec, no I2S).
 *
 * Reproduces the firmware svc_audio contract for the host/web LVGL
 * preview: recording and playback are mutually exclusive, callbacks run
 * on the dedicated "svc_audio" sim task context (UI work must go through
 * ui_async(), exactly like on real firmware), and recording requires the
 * mock SD card to be present (sim_audio_set_sd_present).
 *
 * The mock itself never touches LVGL; timing is driven by the FreeRTOS
 * task shim tick (1 tick = 1 ms).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "services/svc_audio.h"

#define TAG "svc_audio_sim"

#define RECORD_LEVEL_PERIOD_MS 200
#define PLAY_PROGRESS_PERIOD_MS 1000
#define PLAY_MOCK_DURATION_S 30
#define TASK_LOOP_PERIOD_MS 10
#define TONE_MAX_DURATION_MS 60000
#define STOP_ACK_TIMEOUT_MS 5000
#define RECORD_MOCK_PATH "/sdcard/REC_20260829_120000.wav"

/* Mutually-exclusive owner of the mock codec (firmware spec C.5). */
typedef enum {
    SIM_AUDIO_IDLE = 0,
    SIM_AUDIO_RECORDING,
    SIM_AUDIO_PLAYING,
    SIM_AUDIO_TONE,
} sim_audio_state_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static sim_audio_state_t s_state;
static bool s_started;
static bool s_sd_present = true;

static svc_audio_cb_t s_cb;
static void *s_user;
static int s_gain_db;
static int s_volume;

static bool s_stop_requested;
static bool s_paused;
static TickType_t s_tone_deadline;
static SemaphoreHandle_t s_stop_ack;

/* True while any caller-blocking diagnostic owns the codec path. */
static bool s_diag_busy;

static bool sim_codec_busy_locked(void)
{
    return s_state != SIM_AUDIO_IDLE || s_diag_busy;
}

static void sim_emit(svc_audio_event_t type, int value,
                     const char *path, esp_err_t err)
{
    svc_audio_cb_t cb;
    void *user;
    svc_audio_event_msg_t ev;

    portENTER_CRITICAL(&s_lock);
    cb = s_cb;
    user = s_user;
    portEXIT_CRITICAL(&s_lock);
    if (cb == NULL) {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.value = value;
    ev.err = err;
    if (path != NULL) {
        strncpy(ev.path, path, sizeof(ev.path) - 1);
    }
    cb(&ev, user);
}

/* Level meter: pseudo-random 20..80, mean raised by the PGA gain. */
static int sim_level_value(void)
{
    int mean = 32 + s_gain_db; /* gain 0..36 -> mean 32..68 */
    int jitter = (int)(esp_random() % 31) - 15;
    int value = mean + jitter;
    if (value < 20) {
        value = 20;
    }
    if (value > 80) {
        value = 80;
    }
    return value;
}

static void sim_audio_task(void *arg)
{
    TickType_t phase_tick = xTaskGetTickCount();
    int elapsed_s = 0;

    (void)arg;
    for (;;) {
        sim_audio_state_t state;
        bool stop;
        bool paused;
        bool tone_expired = false;
        TickType_t now = xTaskGetTickCount();

        portENTER_CRITICAL(&s_lock);
        state = s_state;
        stop = s_stop_requested;
        paused = s_paused;
        if (state == SIM_AUDIO_TONE && now >= s_tone_deadline) {
            s_state = SIM_AUDIO_IDLE;
            tone_expired = true;
        }
        portEXIT_CRITICAL(&s_lock);

        if (tone_expired) {
            ESP_LOGI(TAG, "tone finished");
            phase_tick = now;
        }

        switch (state) {
        case SIM_AUDIO_RECORDING:
            if (stop) {
                portENTER_CRITICAL(&s_lock);
                s_state = SIM_AUDIO_IDLE;
                s_stop_requested = false;
                portEXIT_CRITICAL(&s_lock);
                ESP_LOGI(TAG, "record stop -> %s", RECORD_MOCK_PATH);
                sim_emit(SVC_AUDIO_EV_RECORD_DONE, 0, RECORD_MOCK_PATH, ESP_OK);
                if (s_stop_ack != NULL) {
                    xSemaphoreGive(s_stop_ack);
                }
            } else if (now - phase_tick >= RECORD_LEVEL_PERIOD_MS) {
                phase_tick = now;
                sim_emit(SVC_AUDIO_EV_LEVEL, sim_level_value(), NULL, ESP_OK);
            }
            break;

        case SIM_AUDIO_PLAYING:
            if (stop) {
                portENTER_CRITICAL(&s_lock);
                s_state = SIM_AUDIO_IDLE;
                s_stop_requested = false;
                portEXIT_CRITICAL(&s_lock);
                ESP_LOGI(TAG, "play stop at %d s", elapsed_s);
                sim_emit(SVC_AUDIO_EV_PLAY_DONE, elapsed_s, NULL, ESP_OK);
                if (s_stop_ack != NULL) {
                    xSemaphoreGive(s_stop_ack);
                }
                elapsed_s = 0;
            } else if (!paused && now - phase_tick >= PLAY_PROGRESS_PERIOD_MS) {
                phase_tick = now;
                elapsed_s++;
                if (elapsed_s >= PLAY_MOCK_DURATION_S) {
                    portENTER_CRITICAL(&s_lock);
                    s_state = SIM_AUDIO_IDLE;
                    portEXIT_CRITICAL(&s_lock);
                    ESP_LOGI(TAG, "play done (%d s)", elapsed_s);
                    sim_emit(SVC_AUDIO_EV_PLAY_DONE, elapsed_s, NULL, ESP_OK);
                    elapsed_s = 0;
                } else {
                    sim_emit(SVC_AUDIO_EV_PLAY_PROGRESS, elapsed_s, NULL, ESP_OK);
                }
            }
            break;

        default:
            phase_tick = now;
            elapsed_s = 0;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(TASK_LOOP_PERIOD_MS));
    }
}

static esp_err_t sim_request_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (s_stop_ack == NULL) {
        s_stop_ack = xSemaphoreCreateBinary();
        if (s_stop_ack == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_stop_requested = true;
    portEXIT_CRITICAL(&s_lock);
    if (xSemaphoreTake(s_stop_ack, pdMS_TO_TICKS(STOP_ACK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "stop ack timeout");
        ret = ESP_ERR_TIMEOUT;
    }
    return ret;
}

esp_err_t svc_audio_start(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_started) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_started = true;
    portEXIT_CRITICAL(&s_lock);

    if (xTaskCreate(sim_audio_task, "svc_audio", 4096, NULL, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_started = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "audio service mock started");
    return ESP_OK;
}

esp_err_t svc_audio_record_start(svc_audio_route_t route, int gain_db,
                                 svc_audio_cb_t cb, void *user)
{
    esp_err_t ret = ESP_OK;

    portENTER_CRITICAL(&s_lock);
    if (!s_sd_present) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (sim_codec_busy_locked()) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_state = SIM_AUDIO_RECORDING;
        s_cb = cb;
        s_user = user;
        s_gain_db = gain_db < 0 ? 0 : (gain_db > 36 ? 36 : gain_db);
        s_stop_requested = false;
    }
    portEXIT_CRITICAL(&s_lock);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "record_start rejected: %s",
                 s_sd_present ? "codec busy" : "no SD card");
        return ret;
    }
    ESP_LOGI(TAG, "record start route=%d gain=%d dB", (int)route, s_gain_db);
    return ESP_OK;
}

esp_err_t svc_audio_record_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    bool rec = (s_state == SIM_AUDIO_RECORDING);
    portEXIT_CRITICAL(&s_lock);
    if (!rec) {
        return ESP_ERR_INVALID_STATE;
    }
    return sim_request_stop();
}

esp_err_t svc_audio_record_stop_async(void)
{
    portENTER_CRITICAL(&s_lock);
    bool rec = (s_state == SIM_AUDIO_RECORDING);
    if (rec) {
        s_stop_requested = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return rec ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool svc_audio_is_recording(void)
{
    portENTER_CRITICAL(&s_lock);
    bool rec = (s_state == SIM_AUDIO_RECORDING) || s_diag_busy;
    portEXIT_CRITICAL(&s_lock);
    return rec;
}

esp_err_t svc_audio_record_set_gain(int gain_db)
{
    portENTER_CRITICAL(&s_lock);
    if (s_state != SIM_AUDIO_RECORDING) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_gain_db = gain_db < 0 ? 0 : (gain_db > 36 ? 36 : gain_db);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t svc_audio_play(const char *path, int volume,
                         svc_audio_cb_t cb, void *user)
{
    (void)path;

    portENTER_CRITICAL(&s_lock);
    if (sim_codec_busy_locked()) {
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "play rejected: codec busy");
        return ESP_ERR_INVALID_STATE;
    }
    s_state = SIM_AUDIO_PLAYING;
    s_cb = cb;
    s_user = user;
    s_volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    s_paused = false;
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "play start path=%s volume=%d",
             path != NULL ? path : "(null)", s_volume);
    return ESP_OK;
}

esp_err_t svc_audio_play_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    bool playing = (s_state == SIM_AUDIO_PLAYING);
    portEXIT_CRITICAL(&s_lock);
    if (!playing) {
        return ESP_ERR_INVALID_STATE;
    }
    return sim_request_stop();
}

esp_err_t svc_audio_play_stop_async(void)
{
    portENTER_CRITICAL(&s_lock);
    bool playing = (s_state == SIM_AUDIO_PLAYING);
    if (playing) {
        s_stop_requested = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return playing ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool svc_audio_is_playing(void)
{
    portENTER_CRITICAL(&s_lock);
    bool playing = (s_state == SIM_AUDIO_PLAYING) || (s_state == SIM_AUDIO_TONE);
    portEXIT_CRITICAL(&s_lock);
    return playing;
}

esp_err_t svc_audio_play_pause(bool pause)
{
    portENTER_CRITICAL(&s_lock);
    if (s_state != SIM_AUDIO_PLAYING) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_paused = pause;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t svc_audio_set_volume(int volume)
{
    portENTER_CRITICAL(&s_lock);
    s_volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t svc_audio_tone_start(uint32_t freq_hz, int volume_pct,
                               uint32_t duration_ms)
{
    (void)freq_hz;

    portENTER_CRITICAL(&s_lock);
    if (sim_codec_busy_locked()) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = SIM_AUDIO_TONE;
    s_volume = volume_pct < 0 ? 0 : (volume_pct > 100 ? 100 : volume_pct);
    if (duration_ms == 0 || duration_ms > TONE_MAX_DURATION_MS) {
        duration_ms = TONE_MAX_DURATION_MS;
    }
    s_tone_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "tone start %u Hz %u ms", (unsigned)freq_hz,
             (unsigned)duration_ms);
    return ESP_OK;
}

esp_err_t svc_audio_tone_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    bool tone = (s_state == SIM_AUDIO_TONE);
    if (tone) {
        s_state = SIM_AUDIO_IDLE;
    }
    portEXIT_CRITICAL(&s_lock);
    return tone ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* Diagnostics block the caller (as on firmware) and only claim the busy
 * flag for the duration; no events are emitted for them. */

esp_err_t svc_audio_capture_analyze(uint32_t duration_ms,
                                    svc_audio_route_t route, int gain_db,
                                    svc_audio_capture_stats_t *out_stats)
{
    (void)route;
    (void)gain_db;

    if (out_stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (sim_codec_busy_locked()) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_diag_busy = true;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "capture_analyze %u ms", (unsigned)duration_ms);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->ch[0].peak = 14505;
    out_stats->ch[0].minimum = -14505;
    out_stats->ch[0].maximum = 14505;
    out_stats->ch[0].live = true;
    out_stats->ch[1].peak = 1945;
    out_stats->ch[1].minimum = -1945;
    out_stats->ch[1].maximum = 1945;
    out_stats->ch[1].live = true;
    out_stats->sample_rate = 16000;
    out_stats->captured_ms = duration_ms;

    portENTER_CRITICAL(&s_lock);
    s_diag_busy = false;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t svc_audio_codec_loopback(svc_audio_loopback_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (sim_codec_busy_locked()) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_diag_busy = true;
    portEXIT_CRITICAL(&s_lock);

    vTaskDelay(pdMS_TO_TICKS(120));

    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->tx_blocks = 100;
    out_stats->sample_count = 51200;
    out_stats->nonzero_samples = 40000; /* >50% */
    out_stats->peak = 20000;
    out_stats->asdout_edges = 320;

    portENTER_CRITICAL(&s_lock);
    s_diag_busy = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "codec loopback PASS (peak=%u)", (unsigned)out_stats->peak);
    return ESP_OK;
}

esp_err_t svc_audio_i2s_loopback(svc_audio_i2s_loopback_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (sim_codec_busy_locked()) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_diag_busy = true;
    portEXIT_CRITICAL(&s_lock);

    vTaskDelay(pdMS_TO_TICKS(50));

    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->bytes_written = 4096;
    out_stats->bytes_read = 4096;
    out_stats->nonzero_bytes = 4096;
    out_stats->pattern_found = true;
    out_stats->pattern_offset = 17;

    portENTER_CRITICAL(&s_lock);
    s_diag_busy = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "i2s loopback PASS (offset=%u)",
             (unsigned)out_stats->pattern_offset);
    return ESP_OK;
}

void sim_audio_set_sd_present(bool present)
{
    portENTER_CRITICAL(&s_lock);
    s_sd_present = present;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "mock SD card %s", present ? "present" : "absent");
}
