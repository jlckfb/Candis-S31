/*
 * Candis-S31 speaker player example.
 *
 * Plays a one-second program-generated 1 kHz sine tone (no asset files),
 * then tries to play /sdcard/example_record.wav from the TF card; when the
 * file is missing the example prints a skip line and keeps running. Only
 * PCM16 WAV (1-2 channels, 8/16/44.1/48 kHz) is accepted, matching the
 * demo's verified playback path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "example_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_player";

#define PLAY_SAMPLE_RATE  16000U
#define PLAY_CHANNELS     2U
#define PLAY_VOLUME       60
#define PLAY_IO_BYTES     4096U
#define WAV_HEADER_BYTES  44U
#define IN_WAV_PATH       BSP_SD_MOUNT_POINT "/example_record.wav"

/* Sample rates the ES8389 BCLK policy supports. */
static bool wav_sample_rate_supported(uint32_t sample_rate)
{
    return sample_rate == 8000 || sample_rate == 16000 ||
           sample_rate == 44100 || sample_rate == 48000;
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t data_size;
    long data_offset;
} wav_info_t;

/* Minimal RIFF walker: find fmt + data, accept PCM16 only. */
static esp_err_t wav_parse(FILE *file, wav_info_t *info)
{
    uint8_t header[WAV_HEADER_BYTES];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
            memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t offset = 12;
    bool have_format = false;
    bool have_data = false;
    uint16_t audio_format = 0;
    uint16_t bits = 0;
    memset(info, 0, sizeof(*info));
    while (offset + 8 <= 0x1000000U) {
        uint8_t chunk[8];
        if (fseek(file, (long)offset, SEEK_SET) != 0 ||
                fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            break;
        }
        const uint32_t size = read_le32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            audio_format = read_le16(fmt);
            info->channels = read_le16(fmt + 2);
            info->sample_rate = read_le32(fmt + 4);
            bits = read_le16(fmt + 14);
            have_format = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data_size = size;
            info->data_offset = (long)(offset + 8);
            have_data = true;
        }
        offset += 8 + size + (size & 1);
        if (have_format && have_data) {
            break;
        }
    }
    if (!have_format || !have_data || audio_format != 1U ||
            bits != 16U || info->channels == 0 || info->channels > 2 ||
            !wav_sample_rate_supported(info->sample_rate) ||
            info->data_size == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_codec_dev_handle_t speaker_init(void)
{
    ESP_ERROR_CHECK(bsp_audio_init(NULL));
    esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
    if (speaker == NULL) {
        ESP_LOGE(TAG, "speaker codec init failed");
    }
    return speaker;
}


static esp_err_t play_sine(esp_codec_dev_handle_t speaker)
{
    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = PLAY_CHANNELS,
        .sample_rate = PLAY_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    int result = bsp_audio_codec_open(speaker, &format);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "sine: speaker open failed: %d", result);
        return ESP_FAIL;
    }
    result = esp_codec_dev_set_out_vol(speaker, PLAY_VOLUME);
    static int16_t frames[256 * PLAY_CHANNELS];
    const int frames_total = PLAY_SAMPLE_RATE; /* one second */
    int written = 0;
    while (result == ESP_CODEC_DEV_OK && written < frames_total) {
        const int batch = frames_total - written > 256 ? 256 : frames_total - written;
        for (int index = 0; index < batch; ++index) {
            const double t = (double)(written + index) / (double)PLAY_SAMPLE_RATE;
            const int16_t sample =
                (int16_t)(12000.0 * sin(2.0 * M_PI * 1000.0 * t));
            frames[index * PLAY_CHANNELS] = sample;
            frames[index * PLAY_CHANNELS + 1] = sample;
        }
        result = esp_codec_dev_write(speaker, frames,
                                    (size_t)batch * PLAY_CHANNELS * sizeof(int16_t));
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "sine: write failed: %d", result);
            break;
        }
        written += batch;
    }
    const int close_result = esp_codec_dev_close(speaker);
    if (result != ESP_CODEC_DEV_OK || close_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "sine failed: io=%d close=%d", result, close_result);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sine: played 1 s of 1 kHz at %u Hz", (unsigned)PLAY_SAMPLE_RATE);
    return ESP_OK;
}

static esp_err_t play_wav(esp_codec_dev_handle_t speaker, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            ESP_LOGI(TAG, "skip: %s not found (record one with audio-recorder)", path);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "wav: cannot open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    wav_info_t info;
    if (wav_parse(file, &info) != ESP_OK) {
        ESP_LOGW(TAG, "skip: %s is not a PCM16 WAV this example accepts", path);
        fclose(file);
        return ESP_OK;
    }
    if (fseek(file, info.data_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "wav: cannot seek to data in %s", path);
        fclose(file);
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = info.channels,
        .sample_rate = info.sample_rate,
        .mclk_multiple = 256,
    };
    if (bsp_audio_codec_open(speaker, &format) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "wav: speaker open failed for %u Hz/%uch",
                 (unsigned)info.sample_rate, (unsigned)info.channels);
        fclose(file);
        return ESP_FAIL;
    }
    int result = esp_codec_dev_set_out_vol(speaker, PLAY_VOLUME);

    static uint8_t io[PLAY_IO_BYTES];
    uint32_t remaining = info.data_size;
    while (result == ESP_CODEC_DEV_OK && remaining > 0) {
        const size_t chunk = remaining < PLAY_IO_BYTES ? remaining : PLAY_IO_BYTES;
        const size_t read_bytes = fread(io, 1, chunk, file);
        if (read_bytes != chunk) {
            ESP_LOGE(TAG, "wav: incomplete data in %s (%u bytes remaining)",
                     path, (unsigned)remaining);
            result = ESP_CODEC_DEV_DRV_ERR;
            break;
        }
        result = esp_codec_dev_write(speaker, io, read_bytes);
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "wav: write failed: %d", result);
            break;
        }
        remaining -= (uint32_t)read_bytes;
    }
    const int close_result = esp_codec_dev_close(speaker);
    const int file_result = fclose(file);
    if (result != ESP_CODEC_DEV_OK || close_result != ESP_CODEC_DEV_OK ||
            file_result != 0 || remaining != 0) {
        ESP_LOGE(TAG, "wav failed: io=%d close=%d file=%d remaining=%u",
                 result, close_result, file_result, (unsigned)remaining);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wav: played %u/%u bytes of %s (%u Hz, %u ch)",
             (unsigned)(info.data_size - remaining), (unsigned)info.data_size,
             path, (unsigned)info.sample_rate, (unsigned)info.channels);
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    }));

    esp_codec_dev_handle_t speaker = speaker_init();
    if (speaker == NULL) {
        ESP_ERROR_CHECK(bsp_audio_deinit());
        return;
    }

    esp_err_t result = play_sine(speaker);

    if (result == ESP_OK) {
        /* The WAV lives on the TF card, so mount it before playback. */
        const esp_err_t mount = bsp_sdcard_mount();
        if (mount == ESP_OK) {
            result = play_wav(speaker, IN_WAV_PATH);
            const esp_err_t unmount_result = bsp_sdcard_unmount();
            if (result == ESP_OK) {
                result = unmount_result;
            }
        } else {
            ESP_LOGW(TAG, "skip: TF card mount failed: %s", esp_err_to_name(mount));
        }
    }

    const esp_err_t deinit_result = bsp_audio_deinit();
    if (result == ESP_OK) {
        result = deinit_result;
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "playback failed: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(TAG, "playback done");
    }
}
