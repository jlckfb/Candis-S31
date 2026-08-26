/*
 * Candis-S31 player demo - audio service.
 *
 * Simplified from firmware/demo/main/services/svc_audio.c.
 * Supports WAV file playback and streaming PCM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "audio_service.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "player_audio";

#define AUDIO_TASK_STACK 8192
#define AUDIO_TASK_PRIO  5
#define AUDIO_QUEUE_DEPTH 8
#define WAV_HDR_BYTES 44
#define IO_BYTES      2048

static esp_codec_dev_handle_t s_speaker_dev = NULL;
static esp_codec_dev_handle_t s_mic_dev = NULL;
static bool s_started = false;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static QueueHandle_t s_queue;

static bool s_playing = false;
static bool s_paused = false;
static int s_volume = 20;
static FILE *s_file = NULL;
static int s_file_bytes_left = 0;
static int s_stream_sample_rate = 0;
static int s_stream_channels = 0;

typedef enum {
    CMD_PLAY_WAV = 0,
    CMD_STOP,
    CMD_PAUSE,
    CMD_SET_VOLUME,
    CMD_STREAM_START,
    CMD_STREAM_DATA,
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t type;
    int value;       /**< volume/pause/sample_rate/channels */
    int value2;
    void *ptr;
    size_t len;
} audio_cmd_t;

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static bool wav_header_parse(const uint8_t *hdr, uint16_t *channels,
                             uint32_t *sample_rate, uint16_t *bits,
                             uint32_t *data_offset, uint32_t *data_size)
{
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return false;
    }
    uint16_t fmt_channels = hdr[22] | (hdr[23] << 8);
    uint32_t fmt_rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    uint16_t fmt_bits = hdr[34] | (hdr[35] << 8);
    if (fmt_bits != 16) {
        return false;
    }
    /* Walk chunks to find 'data'. */
    uint32_t offset = 12;
    while (offset + 8 < WAV_HDR_BYTES) {
        uint32_t id = hdr[offset] | (hdr[offset + 1] << 8) |
                      (hdr[offset + 2] << 16) | (hdr[offset + 3] << 24);
        uint32_t sz = hdr[offset + 4] | (hdr[offset + 5] << 8) |
                      (hdr[offset + 6] << 16) | (hdr[offset + 7] << 24);
        if (id == 0x61746164) { /* 'data' */
            *channels = fmt_channels;
            *sample_rate = fmt_rate;
            *bits = fmt_bits;
            *data_offset = offset + 8;
            *data_size = sz;
            return true;
        }
        offset += 8 + sz;
        if (offset & 1) {
            offset++;
        }
    }
    return false;
}

static esp_err_t codec_open_playback(int sample_rate, int channels)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = channels,
        .bits_per_sample = 16,
        .mclk_multiple = 256,
    };
    esp_err_t err = esp_codec_dev_open(s_speaker_dev, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_codec_dev_set_out_vol(s_speaker_dev, s_volume);
    bsp_power_domain_set(BSP_POWER_AUDIO_PA, true);
    return ESP_OK;
}

static void codec_close_playback(void)
{
    esp_codec_dev_close(s_speaker_dev);
    bsp_power_domain_set(BSP_POWER_AUDIO_PA, false);
}

static void play_run(void)
{
    uint8_t buf[IO_BYTES] __attribute__((aligned(4)));
    while (s_playing && s_file != NULL && s_file_bytes_left > 0 && !s_paused) {
        size_t want = (s_file_bytes_left < (int)sizeof(buf)) ?
                      (size_t)s_file_bytes_left : sizeof(buf);
        size_t got = fread(buf, 1, want, s_file);
        if (got == 0) {
            break;
        }
        esp_codec_dev_write(s_speaker_dev, buf, got);
        s_file_bytes_left -= (int)got;
    }
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    s_playing = false;
    codec_close_playback();
    ESP_LOGI(TAG, "playback finished");
}


static void audio_task(void *arg)
{
    (void)arg;
    audio_cmd_t cmd;
    for (;;) {
        if (!xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(20))) {
            continue;
        }
        switch (cmd.type) {
        case CMD_PLAY_WAV: {
            if (s_playing) {
                break;
            }
            char *path = (char *)cmd.ptr;
            FILE *f = fopen(path, "rb");
            if (f == NULL) {
                ESP_LOGE(TAG, "open %s failed", path);
                break;
            }
            uint8_t hdr[WAV_HDR_BYTES];
            if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
                fclose(f);
                break;
            }
            uint16_t ch = 0;
            uint32_t rate = 0;
            uint16_t bits = 0;
            uint32_t off = 0;
            uint32_t size = 0;
            if (!wav_header_parse(hdr, &ch, &rate, &bits, &off, &size)) {
                ESP_LOGE(TAG, "bad WAV header");
                fclose(f);
                break;
            }
            if (ch != 1 && ch != 2) {
                ESP_LOGE(TAG, "unsupported channels %u", ch);
                fclose(f);
                break;
            }
            fseek(f, off, SEEK_SET);
            if (codec_open_playback((int)rate, (int)ch) != ESP_OK) {
                fclose(f);
                break;
            }
            s_file = f;
            s_file_bytes_left = (int)size;
            s_playing = true;
            s_paused = false;
            play_run();
            break;
        }
        case CMD_STOP:
            s_playing = false;
            if (s_file != NULL) {
                fclose(s_file);
                s_file = NULL;
            }
            codec_close_playback();
            break;
        case CMD_PAUSE:
            s_paused = cmd.value != 0;
            break;
        case CMD_SET_VOLUME:
            s_volume = clamp_int(cmd.value, 0, 100);
            if (s_speaker_dev != NULL) {
                esp_codec_dev_set_out_vol(s_speaker_dev, s_volume);
            }
            break;
        case CMD_STREAM_START:
            if (s_playing) {
                codec_close_playback();
            }
            s_stream_sample_rate = cmd.value;
            s_stream_channels = cmd.value2;
            if (codec_open_playback(s_stream_sample_rate, s_stream_channels) != ESP_OK) {
                s_playing = false;
            } else {
                s_playing = true;
                s_paused = false;
            }
            break;
        case CMD_STREAM_DATA: {
            if (!s_playing || s_paused || cmd.ptr == NULL || cmd.len == 0) {
                break;
            }
            esp_codec_dev_write(s_speaker_dev, cmd.ptr, cmd.len);
            free(cmd.ptr);
            break;
        }
        default:
            break;
        }
    }
}

static esp_err_t send_cmd(const audio_cmd_t *cmd, TickType_t wait)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_queue, cmd, wait) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    esp_err_t err = bsp_audio_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init: %s", esp_err_to_name(err));
        return err;
    }
    s_speaker_dev = bsp_audio_codec_speaker_init();
    s_mic_dev = bsp_audio_codec_microphone_init();
    if (s_speaker_dev == NULL || s_mic_dev == NULL) {
        bsp_audio_deinit();
        return ESP_FAIL;
    }
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(audio_cmd_t));
    if (s_lock == NULL || s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_task, "player_audio", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t audio_play_wav(const char *path, int volume)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_volume = clamp_int(volume, 0, 100);
    audio_cmd_t cmd = {
        .type = CMD_PLAY_WAV,
        .ptr = (void *)path,
    };
    esp_err_t err = send_cmd(&cmd, pdMS_TO_TICKS(100));
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t audio_stop(void)
{
    audio_cmd_t cmd = {.type = CMD_STOP};
    return send_cmd(&cmd, pdMS_TO_TICKS(100));
}

esp_err_t audio_pause(bool pause)
{
    audio_cmd_t cmd = {.type = CMD_PAUSE, .value = pause ? 1 : 0};
    return send_cmd(&cmd, pdMS_TO_TICKS(100));
}

esp_err_t audio_set_volume(int volume)
{
    audio_cmd_t cmd = {.type = CMD_SET_VOLUME, .value = clamp_int(volume, 0, 100)};
    return send_cmd(&cmd, pdMS_TO_TICKS(100));
}

bool audio_is_playing(void)
{
    return s_playing;
}

esp_err_t audio_stream_start(int sample_rate, int channels, int volume)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_volume = clamp_int(volume, 0, 100);
    audio_cmd_t cmd = {
        .type = CMD_STREAM_START,
        .value = sample_rate,
        .value2 = channels,
    };
    esp_err_t err = send_cmd(&cmd, pdMS_TO_TICKS(100));
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t audio_stream_write(const int16_t *samples, size_t count)
{
    if (samples == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t bytes = count * sizeof(int16_t);
    void *copy = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, samples, bytes);
    audio_cmd_t cmd = {
        .type = CMD_STREAM_DATA,
        .ptr = copy,
        .len = bytes,
    };
    esp_err_t err = send_cmd(&cmd, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        free(copy);
    }
    return err;
}

esp_err_t audio_stream_stop(void)
{
    return audio_stop();
}
