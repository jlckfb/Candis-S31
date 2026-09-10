/*
 * Candis-S31 microphone recorder example.
 *
 * Records five seconds from the ES8389 analog microphones to
 * /sdcard/example_record.wav (16 kHz, stereo, PCM16) and prints the byte
 * count and peak level. The validated open order is speaker first,
 * speaker PA domain off during capture, microphone second, gains last.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "example_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_recorder";

#define REC_SAMPLE_RATE   16000U
#define REC_CHANNELS      2U
#define REC_SECONDS       5U
#define REC_FRAME_BYTES   (REC_CHANNELS * sizeof(int16_t))
#define REC_IO_BYTES      2048U              /* factory block size */
#define REC_DISCARD_BLOCKS 2U                /* pipeline garbage */
#define REC_TOTAL_BYTES   (REC_SECONDS * REC_SAMPLE_RATE * REC_FRAME_BYTES)
#define REC_GAIN_DB       12.0f
#define WAV_HEADER_BYTES  44U
#define OUT_WAV_PATH      BSP_SD_MOUNT_POINT "/example_record.wav"

static const uint8_t s_wav_header_template[WAV_HEADER_BYTES] = {
    'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
    'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0,
    'd', 'a', 't', 'a', 0, 0, 0, 0,
};

static void wav_patch_header(uint8_t *header, uint32_t data_size,
                             uint32_t sample_rate, uint16_t channels)
{
    const uint32_t frame_bytes = channels * sizeof(int16_t);
    const uint32_t riff_size = data_size + 36U;
    header[4] = (uint8_t)riff_size;
    header[5] = (uint8_t)(riff_size >> 8);
    header[6] = (uint8_t)(riff_size >> 16);
    header[7] = (uint8_t)(riff_size >> 24);
    header[24] = (uint8_t)sample_rate;
    header[25] = (uint8_t)(sample_rate >> 8);
    header[26] = (uint8_t)(sample_rate >> 16);
    header[27] = (uint8_t)(sample_rate >> 24);
    const uint32_t byte_rate = sample_rate * frame_bytes;
    header[28] = (uint8_t)byte_rate;
    header[29] = (uint8_t)(byte_rate >> 8);
    header[30] = (uint8_t)(byte_rate >> 16);
    header[31] = (uint8_t)(byte_rate >> 24);
    header[40] = (uint8_t)data_size;
    header[41] = (uint8_t)(data_size >> 8);
    header[42] = (uint8_t)(data_size >> 16);
    header[43] = (uint8_t)(data_size >> 24);
}


static esp_err_t record_wav(void)
{
    esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
    esp_codec_dev_handle_t microphone = bsp_audio_codec_microphone_init();
    if (speaker == NULL || microphone == NULL) {
        ESP_LOGE(TAG, "codec init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = REC_CHANNELS,
        .sample_rate = REC_SAMPLE_RATE,
        .mclk_multiple = 256,
    };

    /* Preserve the validated speaker -> PA off -> mic -> gains order. */
    int result = bsp_audio_codec_open(speaker, &format);
    if (result == ESP_CODEC_DEV_OK) {
        result = bsp_power_domain_set(BSP_POWER_AUDIO_PA, false);
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = bsp_audio_codec_open(microphone, &format);
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_out_vol(speaker, 0);
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_in_gain(microphone, REC_GAIN_DB);
    }
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "record open chain failed: %d", result);
        return ESP_FAIL;
    }

    int16_t *pcm = heap_caps_malloc(REC_TOTAL_BYTES, MALLOC_CAP_SPIRAM);
    if (pcm == NULL) {
        ESP_LOGE(TAG, "no memory for %u s capture", REC_SECONDS);
        return ESP_ERR_NO_MEM;
    }

    /* Discard the first pipeline-fill blocks, then capture five seconds. */
    static uint8_t io[REC_IO_BYTES];
    for (unsigned index = 0; result == ESP_CODEC_DEV_OK &&
            index < REC_DISCARD_BLOCKS; ++index) {
        result = esp_codec_dev_read(microphone, io, REC_IO_BYTES);
    }
    uint32_t filled = 0;
    while (result == ESP_CODEC_DEV_OK && filled < REC_TOTAL_BYTES) {
        const uint32_t remaining = REC_TOTAL_BYTES - filled;
        const uint32_t chunk = remaining < REC_IO_BYTES ? remaining : REC_IO_BYTES;
        result = esp_codec_dev_read(microphone, io, chunk);
        if (result == ESP_CODEC_DEV_OK) {
            memcpy((uint8_t *)pcm + filled, io, chunk);
            filled += chunk;
        }
    }
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "capture failed: %d", result);
        free(pcm);
        return ESP_FAIL;
    }

    int32_t peak = 0;
    for (size_t index = 0; index < filled / sizeof(int16_t); ++index) {
        const int32_t absolute = pcm[index] < 0 ? -(int32_t)pcm[index] : pcm[index];
        if (absolute > peak) {
            peak = absolute;
        }
    }
    ESP_LOGI(TAG, "captured %u bytes (%.1f s), peak level %ld/%d (%.1f%%FS)",
             (unsigned)filled, (double)filled / (REC_SAMPLE_RATE * REC_FRAME_BYTES),
             (long)peak, INT16_MAX, 100.0 * peak / INT16_MAX);

    uint8_t header[WAV_HEADER_BYTES];
    memcpy(header, s_wav_header_template, WAV_HEADER_BYTES);
    wav_patch_header(header, filled, REC_SAMPLE_RATE, REC_CHANNELS);

    FILE *file = fopen(OUT_WAV_PATH, "wb");
    esp_err_t error = ESP_FAIL;
    if (file == NULL) {
        ESP_LOGE(TAG, "cannot write " OUT_WAV_PATH);
    } else {
        const size_t header_written = fwrite(header, 1, WAV_HEADER_BYTES, file);
        const size_t data_written = fwrite(pcm, 1, filled, file);
        const int close_result = fclose(file);
        if (header_written == WAV_HEADER_BYTES && data_written == filled &&
                close_result == 0) {
            error = ESP_OK;
        } else {
            ESP_LOGE(TAG, "WAV write failed (header=%u data=%u close=%d)",
                     (unsigned)header_written, (unsigned)data_written, close_result);
        }
    }

    free(pcm);
    return error;
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = true,
        .start_display = false,
        .brightness_percent = 0,
    }));

    if (!bsp_sdcard_is_inserted()) {
        ESP_LOGE(TAG, "no TF card: recording needs " OUT_WAV_PATH);
        return;
    }
    const esp_err_t mount_result = bsp_sdcard_mount();
    if (mount_result != ESP_OK) {
        ESP_LOGE(TAG, "TF card mount failed: %s", esp_err_to_name(mount_result));
        return;
    }

    esp_err_t result = record_wav();
    /* BSP deinit closes both devices before releasing their shared I2S
     * channels and powers the PA down; do not turn the PA back on here. */
    const esp_err_t audio_result = bsp_audio_deinit();
    /* Flush FAT and the directory entry before reporting a durable recording. */
    const esp_err_t unmount_result = bsp_sdcard_unmount();
    if (result == ESP_OK) {
        result = audio_result != ESP_OK ? audio_result : unmount_result;
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "recording failed: %s", esp_err_to_name(result));
        return;
    }
    ESP_LOGI(TAG, "saved " OUT_WAV_PATH);
    ESP_LOGI(TAG, "recording done");
}
