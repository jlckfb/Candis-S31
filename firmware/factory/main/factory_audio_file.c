/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

#define AUDIO_DEMO_SAMPLE_RATE       BSP_I2S_SAMPLE_RATE
#define AUDIO_DEMO_CHANNELS          2U
#define AUDIO_DEMO_BITS_PER_SAMPLE   16U
#define AUDIO_DEMO_FRAME_BYTES       4U
#define AUDIO_DEMO_IO_FRAMES         512U
#define AUDIO_DEMO_IO_BYTES          (AUDIO_DEMO_IO_FRAMES * AUDIO_DEMO_FRAME_BYTES)
#define AUDIO_DEMO_DEFAULT_SECONDS   6U
#define AUDIO_DEMO_MAX_SECONDS       30U
#define AUDIO_DEMO_DEFAULT_GAIN_DB   24U
#define AUDIO_DEMO_DEFAULT_VOLUME    20U
#define AUDIO_DEMO_DEFAULT_FILE      "candis_record.wav"
#define AUDIO_DEMO_MIC1_MANUAL_FILE  "mic1_ch0_manual.wav"
#define AUDIO_DEMO_MIC2_MANUAL_FILE  "mic2_ch1_manual.wav"
#define AUDIO_DEMO_WAV_HEADER_BYTES  44U
#define AUDIO_DEMO_MAX_FILENAME      64U
#define AUDIO_DEMO_MAX_CHUNKS        64U

typedef enum {
    WAV_PLAY_MIX = 0,
    WAV_PLAY_LEFT,
    WAV_PLAY_RIGHT,
} wav_play_mode_t;

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t block_align;
    uint32_t data_size;
    long data_offset;
} wav_info_t;

typedef struct {
    int16_t minimum[2];
    int16_t maximum[2];
    uint16_t peak[2];
    uint32_t sample_count[2];
} stereo_capture_stats_t;

static _Alignas(4) uint8_t s_audio_io[AUDIO_DEMO_IO_BYTES];
/* A mono input block expands to twice its byte count when duplicated into
 * stereo, so this buffer is deliberately twice the raw input capacity. */
static int16_t s_audio_stereo[
    AUDIO_DEMO_IO_BYTES / sizeof(int16_t) * 2U];

static bool parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
                      uint32_t *value)
{
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t fnv1a_update(uint32_t hash, const uint8_t *data, size_t size)
{
    for (size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool make_sd_path(const char *filename, char *path, size_t path_size)
{
    const size_t length = strlen(filename);
    if (length == 0 || length > AUDIO_DEMO_MAX_FILENAME || filename[0] == '.') {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const char c = filename[index];
        const bool valid = (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                           c == '-';
        if (!valid) {
            return false;
        }
    }
    const int written = snprintf(path, path_size, "%s/%s", BSP_SD_MOUNT_POINT,
                                 filename);
    return written > 0 && (size_t)written < path_size;
}

static bool wav_sample_rate_supported(uint32_t sample_rate)
{
    /* With the shared ES8389 policy use_mclk=false/no_dac_ref=true and
     * 16-bit stereo I2S, BCLK is 32*fs. These are the sample rates with an
     * exact ratio-32 row in esp_codec_dev's ES8389 coefficient table. */
    return sample_rate == 8000U || sample_rate == 16000U ||
           sample_rate == 44100U || sample_rate == 48000U;
}

static esp_err_t wav_parse(FILE *file, wav_info_t *info)
{
    uint8_t header[12];
    if (fseek(file, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    const long file_size = ftell(file);
    if (file_size < (long)sizeof(header) || fseek(file, 0, SEEK_SET) != 0 ||
            fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool have_format = false;
    bool have_data = false;
    uint16_t audio_format = 0;
    uint32_t byte_rate = 0;
    memset(info, 0, sizeof(*info));

    for (unsigned chunk_index = 0;
            chunk_index < AUDIO_DEMO_MAX_CHUNKS && ftell(file) >= 0 &&
            ftell(file) + 8 <= file_size; ++chunk_index) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), file) !=
                sizeof(chunk_header)) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t chunk_size = read_le32(chunk_header + 4);
        const long payload_offset = ftell(file);
        const uint64_t next_offset_u64 = (uint64_t)payload_offset + chunk_size +
                                         (chunk_size & 1U);
        if (payload_offset < 0 || next_offset_u64 > (uint64_t)file_size ||
                next_offset_u64 > (uint64_t)LONG_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t format[16];
            if (chunk_size < sizeof(format) ||
                    fread(format, 1, sizeof(format), file) != sizeof(format)) {
                return ESP_ERR_INVALID_SIZE;
            }
            audio_format = read_le16(format);
            info->channels = read_le16(format + 2);
            info->sample_rate = read_le32(format + 4);
            byte_rate = read_le32(format + 8);
            info->block_align = read_le16(format + 12);
            info->bits_per_sample = read_le16(format + 14);
            have_format = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            info->data_offset = payload_offset;
            info->data_size = chunk_size;
            have_data = true;
        }

        if (have_format && have_data) {
            break;
        }
        if (fseek(file, (long)next_offset_u64, SEEK_SET) != 0) {
            return ESP_FAIL;
        }
    }

    if (!have_format || !have_data || audio_format != 1U ||
            (info->channels != 1U && info->channels != 2U) ||
            info->bits_per_sample != 16U ||
            !wav_sample_rate_supported(info->sample_rate)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint16_t expected_align = info->channels * sizeof(int16_t);
    if (info->block_align != expected_align ||
            byte_rate != info->sample_rate * info->block_align ||
            info->data_size == 0 || info->data_size % info->block_align != 0 ||
            (uint64_t)info->data_offset + info->data_size > (uint64_t)file_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, info->data_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void wav_make_header(uint8_t header[AUDIO_DEMO_WAV_HEADER_BYTES],
                            uint32_t data_size)
{
    memset(header, 0, AUDIO_DEMO_WAV_HEADER_BYTES);
    memcpy(header, "RIFF", 4);
    write_le32(header + 4, data_size + 36U);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_le32(header + 16, 16U);
    write_le16(header + 20, 1U);
    write_le16(header + 22, AUDIO_DEMO_CHANNELS);
    write_le32(header + 24, AUDIO_DEMO_SAMPLE_RATE);
    write_le32(header + 28,
               AUDIO_DEMO_SAMPLE_RATE * AUDIO_DEMO_FRAME_BYTES);
    write_le16(header + 32, AUDIO_DEMO_FRAME_BYTES);
    write_le16(header + 34, AUDIO_DEMO_BITS_PER_SAMPLE);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_size);
}

static esp_err_t wav_write_file(const char *path, const uint8_t *pcm,
                                uint32_t pcm_size)
{
    uint8_t header[AUDIO_DEMO_WAV_HEADER_BYTES];
    wav_make_header(header, pcm_size);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        printf("audio_demo: open %s for write failed: %s\n", path,
               strerror(errno));
        return ESP_FAIL;
    }
    esp_err_t result = ESP_OK;
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
            fwrite(pcm, 1, pcm_size, file) != pcm_size || fflush(file) != 0 ||
            fsync(fileno(file)) != 0) {
        printf("audio_demo: write %s failed: %s\n", path, strerror(errno));
        result = ESP_FAIL;
    }
    if (fclose(file) != 0 && result == ESP_OK) {
        printf("audio_demo: close %s failed: %s\n", path, strerror(errno));
        result = ESP_FAIL;
    }
    if (result != ESP_OK) {
        unlink(path);
    }
    return result;
}

static esp_codec_dev_sample_info_t sample_format(uint32_t sample_rate)
{
    return (esp_codec_dev_sample_info_t) {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = sample_rate,
        .mclk_multiple = 256,
    };
}

static void capture_stats_init(stereo_capture_stats_t *stats)
{
    *stats = (stereo_capture_stats_t) {
        .minimum = {INT16_MAX, INT16_MAX},
        .maximum = {INT16_MIN, INT16_MIN},
    };
}

static void capture_stats_add(stereo_capture_stats_t *stats,
                              const int16_t *samples, size_t sample_count)
{
    for (size_t index = 0; index < sample_count; ++index) {
        const unsigned channel = index & 1U;
        const int16_t sample = samples[index];
        const int32_t absolute = sample < 0 ? -(int32_t)sample : sample;
        if (sample < stats->minimum[channel]) {
            stats->minimum[channel] = sample;
        }
        if (sample > stats->maximum[channel]) {
            stats->maximum[channel] = sample;
        }
        if (absolute > stats->peak[channel]) {
            stats->peak[channel] = (uint16_t)absolute;
        }
        stats->sample_count[channel]++;
    }
}

static void drain_console_input(void)
{
    char discard[32];
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval no_wait = {.tv_sec = 0, .tv_usec = 0};
        if (select(STDIN_FILENO + 1, &read_set, NULL, NULL, &no_wait) <= 0 ||
                read(STDIN_FILENO, discard, sizeof(discard)) <= 0) {
            break;
        }
    }
}

static char wait_for_manual_record_start(const char *command_name)
{
    drain_console_input();
    printf("%s: send y to start recording, or n to cancel\n", command_name);
    fflush(stdout);

    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval slice = {.tv_sec = 0, .tv_usec = 100000};
        if (select(STDIN_FILENO + 1, &read_set, NULL, NULL, &slice) <= 0) {
            continue;
        }
        char input[16];
        const ssize_t count = read(STDIN_FILENO, input, sizeof(input));
        for (ssize_t index = 0; index < count; ++index) {
            const char key = (char)(input[index] | 0x20);
            if (key == 'y' || key == 'n') {
                printf("%s: control=%c\n", command_name, key);
                fflush(stdout);
                return key;
            }
        }
    }
}

static bool manual_record_stop_requested(void)
{
    bool stop = false;
    char input[32];
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval no_wait = {.tv_sec = 0, .tv_usec = 0};
        if (select(STDIN_FILENO + 1, &read_set, NULL, NULL, &no_wait) <= 0) {
            break;
        }
        const ssize_t count = read(STDIN_FILENO, input, sizeof(input));
        if (count <= 0) {
            break;
        }
        for (ssize_t index = 0; index < count; ++index) {
            if ((input[index] | 0x20) == 'n') {
                stop = true;
            }
        }
    }
    return stop;
}

static esp_err_t capture_stereo(esp_codec_dev_handle_t microphone,
                                uint8_t *capture, uint32_t capture_size,
                                uint32_t gain_db,
                                const char *route,
                                stereo_capture_stats_t *stats)
{
    esp_codec_dev_sample_info_t format = sample_format(AUDIO_DEMO_SAMPLE_RATE);
    int result = bsp_audio_codec_open(microphone, &format);
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_in_gain(microphone, (float)gain_db);
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = factory_audio_set_input_route(route);
    }
    if (result != ESP_CODEC_DEV_OK) {
        printf("audio_demo: microphone open/gain failed: %s\n",
               esp_err_to_name(result));
        /* esp_codec_dev_open() can partially set its opened state before a
         * later format/configuration error. Close unconditionally after an
         * attempted open so a retry cannot inherit that partial state. */
        esp_codec_dev_close(microphone);
        return result;
    }

    /* Discard two 32 ms blocks after enabling the ADC and its clocks. */
    for (unsigned index = 0; index < 2 && result == ESP_CODEC_DEV_OK; ++index) {
        result = esp_codec_dev_read(microphone, s_audio_io,
                                    sizeof(s_audio_io));
    }

    capture_stats_init(stats);
    for (uint32_t offset = 0;
            result == ESP_CODEC_DEV_OK && offset < capture_size;) {
        const uint32_t remaining = capture_size - offset;
        const uint32_t bytes = remaining < sizeof(s_audio_io) ? remaining :
                               sizeof(s_audio_io);
        result = esp_codec_dev_read(microphone, s_audio_io, bytes);
        if (result != ESP_CODEC_DEV_OK) {
            break;
        }
        memcpy(capture + offset, s_audio_io, bytes);
        capture_stats_add(stats, (const int16_t *)s_audio_io,
                          bytes / sizeof(int16_t));
        offset += bytes;
    }

    const int close_result = esp_codec_dev_close(microphone);
    if (result == ESP_CODEC_DEV_OK && close_result != ESP_CODEC_DEV_OK) {
        result = close_result;
    }
    if (result != ESP_CODEC_DEV_OK) {
        printf("audio_demo: microphone capture failed: %s\n",
               esp_err_to_name(result));
    }
    return result;
}

static esp_err_t capture_stereo_manual(esp_codec_dev_handle_t microphone,
                                       uint8_t *capture,
                                       uint32_t capture_capacity,
                                       uint32_t gain_db,
                                       const char *command_name,
                                       const char *route,
                                       stereo_capture_stats_t *stats,
                                       uint32_t *captured_size)
{
    *captured_size = 0;
    esp_codec_dev_sample_info_t format = sample_format(AUDIO_DEMO_SAMPLE_RATE);
    int result = bsp_audio_codec_open(microphone, &format);
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_in_gain(microphone, (float)gain_db);
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = factory_audio_set_input_route(route);
    }
    if (result != ESP_CODEC_DEV_OK) {
        printf("%s: microphone open/gain failed: %s\n", command_name,
               esp_err_to_name(result));
        esp_codec_dev_close(microphone);
        return result;
    }

    for (unsigned index = 0; index < 2 && result == ESP_CODEC_DEV_OK; ++index) {
        result = esp_codec_dev_read(microphone, s_audio_io,
                                    sizeof(s_audio_io));
    }

    capture_stats_init(stats);
    while (result == ESP_CODEC_DEV_OK &&
            *captured_size < capture_capacity) {
        const uint32_t remaining = capture_capacity - *captured_size;
        const uint32_t bytes = remaining < sizeof(s_audio_io) ? remaining :
                               sizeof(s_audio_io);
        result = esp_codec_dev_read(microphone, s_audio_io, bytes);
        if (result != ESP_CODEC_DEV_OK) {
            break;
        }
        memcpy(capture + *captured_size, s_audio_io, bytes);
        capture_stats_add(stats, (const int16_t *)s_audio_io,
                          bytes / sizeof(int16_t));
        *captured_size += bytes;
        if (manual_record_stop_requested()) {
            printf("%s: n received, stopping capture\n", command_name);
            break;
        }
    }

    if (result == ESP_CODEC_DEV_OK && *captured_size == capture_capacity) {
        printf("%s: reached the %u s safety limit\n", command_name,
               AUDIO_DEMO_MAX_SECONDS);
    }
    const int close_result = esp_codec_dev_close(microphone);
    if (result == ESP_CODEC_DEV_OK && close_result != ESP_CODEC_DEV_OK) {
        result = close_result;
    }
    if (result != ESP_CODEC_DEV_OK) {
        printf("%s: microphone capture failed: %s\n", command_name,
               esp_err_to_name(result));
    }
    return result;
}

static const char *play_mode_name(wav_play_mode_t mode)
{
    switch (mode) {
    case WAV_PLAY_LEFT:
        return "ch0/left duplicated to both outputs";
    case WAV_PLAY_RIGHT:
        return "ch1/right duplicated to both outputs";
    case WAV_PLAY_MIX:
    default:
        return "ch0+ch1 mono mix duplicated to both outputs";
    }
}

static esp_err_t wav_play_file(esp_codec_dev_handle_t speaker,
                               const char *path, uint32_t volume,
                               wav_play_mode_t mode, uint32_t *checksum)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        printf("wav_play: open %s failed: %s\n", path, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }
    wav_info_t info;
    esp_err_t result = wav_parse(file, &info);
    if (result != ESP_OK) {
        printf("wav_play: unsupported or malformed WAV (%s): %s\n", path,
               esp_err_to_name(result));
        fclose(file);
        return result;
    }
    if (info.channels == 1U && mode != WAV_PLAY_MIX) {
        printf("wav_play: mono input ignores left/right selection\n");
        mode = WAV_PLAY_MIX;
    }
    const uint64_t frames = info.data_size / info.block_align;
    const uint32_t duration_ms =
        (uint32_t)((frames * 1000U + info.sample_rate - 1U) /
                   info.sample_rate);
    printf("wav_play: %s rate=%" PRIu32 "Hz channels=%u bits=%u "
           "frames=%" PRIu64 " duration_ms=%" PRIu32 " mode=%s\n",
           path, info.sample_rate, info.channels, info.bits_per_sample,
           frames, duration_ms, play_mode_name(mode));

    esp_codec_dev_sample_info_t format = sample_format(info.sample_rate);
    int codec_result = bsp_audio_codec_open(speaker, &format);
    const bool open_attempted = true;
    if (codec_result == ESP_CODEC_DEV_OK) {
        codec_result = esp_codec_dev_set_out_vol(speaker, volume);
    }
    if (codec_result != ESP_CODEC_DEV_OK) {
        printf("wav_play: speaker open/volume failed: %s\n",
               esp_err_to_name(codec_result));
        result = codec_result;
        goto cleanup;
    }

    uint32_t remaining = info.data_size;
    uint32_t hash = UINT32_C(2166136261);
    while (remaining > 0 && result == ESP_OK) {
        const size_t request = remaining < sizeof(s_audio_io) ? remaining :
                               sizeof(s_audio_io);
        const size_t bytes_read = fread(s_audio_io, 1, request, file);
        if (bytes_read != request) {
            printf("wav_play: short read at data byte %" PRIu32 ": %s\n",
                   info.data_size - remaining, strerror(errno));
            result = ESP_FAIL;
            break;
        }
        hash = fnv1a_update(hash, s_audio_io, bytes_read);

        const int16_t *input = (const int16_t *)s_audio_io;
        const void *output = s_audio_io;
        size_t output_bytes = bytes_read;
        if (info.channels == 1U) {
            const size_t samples = bytes_read / sizeof(int16_t);
            for (size_t index = 0; index < samples; ++index) {
                s_audio_stereo[index * 2U] = input[index];
                s_audio_stereo[index * 2U + 1U] = input[index];
            }
            output = s_audio_stereo;
            output_bytes = bytes_read * 2U;
        } else {
            const size_t frame_count = bytes_read / AUDIO_DEMO_FRAME_BYTES;
            for (size_t frame = 0; frame < frame_count; ++frame) {
                int16_t sample;
                if (mode == WAV_PLAY_LEFT || mode == WAV_PLAY_RIGHT) {
                    const unsigned selected = mode == WAV_PLAY_LEFT ? 0U : 1U;
                    sample = input[frame * 2U + selected];
                } else {
                    const int32_t sum = (int32_t)input[frame * 2U] +
                                        input[frame * 2U + 1U];
                    sample = (int16_t)(sum / 2);
                }
                s_audio_stereo[frame * 2U] = sample;
                s_audio_stereo[frame * 2U + 1U] = sample;
            }
            output = s_audio_stereo;
        }
        codec_result = esp_codec_dev_write(speaker, (void *)output,
                                           output_bytes);
        if (codec_result != ESP_CODEC_DEV_OK) {
            printf("wav_play: speaker write failed: %s\n",
                   esp_err_to_name(codec_result));
            result = codec_result;
            break;
        }
        remaining -= bytes_read;
    }
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        *checksum = hash;
    }

cleanup:
    if (open_attempted) {
        const int close_result = esp_codec_dev_close(speaker);
        if (result == ESP_OK && close_result != ESP_CODEC_DEV_OK) {
            result = close_result;
        }
    }
    if (fclose(file) != 0 && result == ESP_OK) {
        result = ESP_FAIL;
    }
    return result;
}

static esp_err_t prepare_audio_command(bool *mounted_here)
{
    *mounted_here = false;
    if (!bsp_sdcard_is_inserted()) {
        printf("audio_demo: TF card detect reports no card\n");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t result = factory_audio_stop();
    if (result == ESP_OK) {
        result = bsp_audio_deinit();
    }
    if (result != ESP_OK) {
        printf("audio_demo: previous audio cleanup failed: %s\n",
               esp_err_to_name(result));
        return result;
    }
    if (bsp_sdcard_get_handle() == NULL) {
        result = bsp_sdcard_mount();
        if (result != ESP_OK) {
            printf("audio_demo: TF mount failed: %s\n", esp_err_to_name(result));
            return result;
        }
        *mounted_here = true;
    }
    return ESP_OK;
}

static esp_err_t finish_audio_command(bool mounted_here,
                                      esp_codec_dev_handle_t microphone,
                                      esp_codec_dev_handle_t speaker,
                                      esp_err_t result)
{
    if (microphone != NULL) {
        const esp_err_t error = bsp_audio_codec_deinit(microphone);
        if (result == ESP_OK && error != ESP_OK) {
            result = error;
        }
    }
    if (speaker != NULL) {
        const esp_err_t error = bsp_audio_codec_deinit(speaker);
        if (result == ESP_OK && error != ESP_OK) {
            result = error;
        }
    }
    const esp_err_t audio_error = bsp_audio_deinit();
    if (result == ESP_OK && audio_error != ESP_OK) {
        result = audio_error;
    }
    if (mounted_here) {
        const esp_err_t sd_error = bsp_sdcard_unmount();
        if (result == ESP_OK && sd_error != ESP_OK) {
            result = sd_error;
        }
    }
    return result;
}

static int command_audio_record_playback(int argc, char **argv)
{
    uint32_t seconds = AUDIO_DEMO_DEFAULT_SECONDS;
    uint32_t gain_db = AUDIO_DEMO_DEFAULT_GAIN_DB;
    uint32_t volume = AUDIO_DEMO_DEFAULT_VOLUME;
    const char *filename = AUDIO_DEMO_DEFAULT_FILE;
    const char *route = "default";
    bool ch0_only = false;
    if (argc > 7 ||
            (argc >= 2 && !parse_u32(argv[1], 1, AUDIO_DEMO_MAX_SECONDS,
                                     &seconds)) ||
            (argc >= 3 && !parse_u32(argv[2], 0, 48, &gain_db)) ||
            (argc >= 4 && !parse_u32(argv[3], 0, 100, &volume))) {
        printf("usage: audio_record_playback [SECONDS 1-30] [GAIN_DB 0-48] "
               "[VOLUME 0-100] [FILE.wav] [default|mic2_left|mic1_single_left] "
               "[both|ch0]\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (argc >= 5) {
        filename = argv[4];
    }
    if (argc >= 6) {
        route = argv[5];
        if (strcmp(route, "default") != 0 && strcmp(route, "mic2_left") != 0 &&
                strcmp(route, "mic1_single_left") != 0) {
            printf("audio_record_playback: invalid input route\n");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (argc >= 7) {
        if (strcmp(argv[6], "ch0") == 0) {
            ch0_only = true;
        } else if (strcmp(argv[6], "both") != 0) {
            printf("audio_record_playback: playback selection must be both or ch0\n");
            return ESP_ERR_INVALID_ARG;
        }
    }
    char path[sizeof(BSP_SD_MOUNT_POINT) + AUDIO_DEMO_MAX_FILENAME + 2U];
    if (!make_sd_path(filename, path, sizeof(path))) {
        printf("audio_record_playback: FILE must be a 1-64 character ASCII "
               "root filename using letters, digits, '.', '_' or '-'\n");
        return ESP_ERR_INVALID_ARG;
    }

    bool mounted_here = false;
    esp_err_t result = prepare_audio_command(&mounted_here);
    esp_codec_dev_handle_t microphone = NULL;
    esp_codec_dev_handle_t speaker = NULL;
    uint8_t *capture = NULL;
    if (result != ESP_OK) {
        return result;
    }

    const uint32_t capture_size = seconds * AUDIO_DEMO_SAMPLE_RATE *
                                  AUDIO_DEMO_FRAME_BYTES;
    capture = heap_caps_malloc(capture_size,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (capture == NULL) {
        printf("audio_record_playback: need %" PRIu32
               " bytes of PSRAM for gap-free capture\n", capture_size);
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    microphone = bsp_audio_codec_microphone_init();
    speaker = bsp_audio_codec_speaker_init();
    if (microphone == NULL || speaker == NULL) {
        printf("audio_record_playback: codec initialization failed\n");
        result = ESP_FAIL;
        goto cleanup;
    }

    printf("audio_record_playback: %" PRIu32
           " s stereo recording starts in 3 s; speak or tap near both MICs\n",
           seconds);
    printf("audio_record_playback: input route=%s playback=%s\n", route,
           ch0_only ? "ch0 only" : "ch0 then ch1");
    for (unsigned countdown = 3; countdown > 0; --countdown) {
        printf("audio_record_playback: %u...\n", countdown);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("audio_record_playback: RECORDING\n");

    stereo_capture_stats_t stats;
    result = capture_stereo(microphone, capture, capture_size, gain_db, route, &stats);
    if (result != ESP_OK) {
        goto cleanup;
    }
    const uint32_t p2p[2] = {
        (uint32_t)((int32_t)stats.maximum[0] - stats.minimum[0]),
        (uint32_t)((int32_t)stats.maximum[1] - stats.minimum[1]),
    };
    const bool channel_live[2] = {p2p[0] >= 16U, p2p[1] >= 16U};
    printf("audio_record_playback: capture complete; logical WAV "
           "ch0/left peak=%u p2p=%" PRIu32 " live=%s, "
           "ch1/right peak=%u p2p=%" PRIu32 " live=%s\n",
           stats.peak[0], p2p[0], channel_live[0] ? "yes" : "no",
           stats.peak[1], p2p[1], channel_live[1] ? "yes" : "no");

    char microphone_detail[FACTORY_DETAIL_LENGTH];
    snprintf(microphone_detail, sizeof(microphone_detail),
             "wav ch0/ch1 peak=%u/%u p2p=%" PRIu32 "/%" PRIu32
             " gain=%" PRIu32 " dur_s=%" PRIu32,
             stats.peak[0], stats.peak[1], p2p[0], p2p[1], gain_db, seconds);
    factory_report_set(FACTORY_TEST_MICROPHONE,
                       channel_live[0] && channel_live[1] ?
                       FACTORY_STATUS_WARN : FACTORY_STATUS_FAIL,
                       microphone_detail);
    factory_report_print_one(FACTORY_TEST_MICROPHONE);

    const uint32_t capture_checksum =
        fnv1a_update(UINT32_C(2166136261), capture, capture_size);
    result = wav_write_file(path, capture, capture_size);
    heap_caps_free(capture);
    capture = NULL;
    if (result != ESP_OK) {
        factory_report_error(FACTORY_TEST_SDCARD, result,
                             "stereo WAV write failed");
        goto cleanup;
    }
    printf("audio_record_playback: wrote %s bytes=%" PRIu32
           " checksum=0x%08" PRIx32 "\n", path,
           capture_size + AUDIO_DEMO_WAV_HEADER_BYTES, capture_checksum);

    uint32_t left_checksum = 0;
    uint32_t right_checksum = 0;
    printf("audio_record_playback: playing logical ch0/left first\n");
    result = wav_play_file(speaker, path, volume, WAV_PLAY_LEFT,
                           &left_checksum);
    if (result == ESP_OK && !ch0_only) {
        vTaskDelay(pdMS_TO_TICKS(300));
        printf("audio_record_playback: playing logical ch1/right second\n");
        result = wav_play_file(speaker, path, volume, WAV_PLAY_RIGHT,
                               &right_checksum);
    }
    if (result != ESP_OK || left_checksum != capture_checksum ||
            (!ch0_only && right_checksum != capture_checksum)) {
        if (result == ESP_OK) {
            result = ESP_ERR_INVALID_CRC;
        }
        printf("audio_record_playback: TF readback checksum mismatch "
               "capture=0x%08" PRIx32 " left=0x%08" PRIx32
               " right=0x%08" PRIx32 "\n", capture_checksum,
               left_checksum, right_checksum);
        factory_report_error(FACTORY_TEST_SDCARD, result,
                             "WAV readback/playback failed");
        goto cleanup;
    }
    char sd_detail[FACTORY_DETAIL_LENGTH];
    snprintf(sd_detail, sizeof(sd_detail),
             "stereo WAV write/read passed bytes=%" PRIu32
             " checksum=%08" PRIx32,
             capture_size + AUDIO_DEMO_WAV_HEADER_BYTES, capture_checksum);
    factory_report_set(FACTORY_TEST_SDCARD, FACTORY_STATUS_PASS, sd_detail);
    factory_report_print_one(FACTORY_TEST_SDCARD);

    if (!channel_live[0] || (!ch0_only && !channel_live[1])) {
        printf("audio_record_playback: FAIL - selected microphone channel "
               "was effectively silent\n");
        result = ESP_FAIL;
        goto cleanup;
    }
    const char answer = factory_console_ask_operator(
                            "speaker",
                            ch0_only ? "Did you hear clean CH0 speech?" :
                            "Did you hear ch0 first and ch1 second clearly?",
                            OPERATOR_PROMPT_TIMEOUT_S);
    factory_report_operator_verdict(
        FACTORY_TEST_SPEAKER, answer,
        ch0_only ? "operator heard clean CH0 speech" :
                   "operator heard both recorded microphone channels",
        ch0_only ? "operator did not hear clean CH0 speech" :
                   "operator did not hear both recorded microphone channels",
        ch0_only ? "WAV played CH0 only; operator confirmation pending" :
                   "stereo WAV played ch0 then ch1; operator confirmation pending");
    if (answer == 'n') {
        result = ESP_FAIL;
    }

cleanup:
    heap_caps_free(capture);
    result = finish_audio_command(mounted_here, microphone, speaker, result);
    printf("audio_record_playback: %s (%s); file retained at %s\n",
           result == ESP_OK ? "PASS" : "FAIL", esp_err_to_name(result), path);
    return result;
}

static int command_manual_mic_channel(int argc, char **argv,
                                      const char *command_name,
                                      const char *microphone_name,
                                      unsigned selected_channel,
                                      wav_play_mode_t play_mode,
                                      const char *default_filename)
{
    uint32_t gain_db = AUDIO_DEMO_DEFAULT_GAIN_DB;
    uint32_t volume = 100U;
    const char *filename = default_filename;
    if (argc > 4 ||
            (argc >= 2 && !parse_u32(argv[1], 0, 48, &gain_db)) ||
            (argc >= 3 && !parse_u32(argv[2], 0, 100, &volume))) {
        printf("usage: %s [GAIN_DB 0-48] [VOLUME 0-100] [FILE.wav]\n",
               command_name);
        return ESP_ERR_INVALID_ARG;
    }
    if (argc >= 4) {
        filename = argv[3];
    }

    char path[sizeof(BSP_SD_MOUNT_POINT) + AUDIO_DEMO_MAX_FILENAME + 2U];
    if (!make_sd_path(filename, path, sizeof(path))) {
        printf("%s: FILE must be a 1-64 character ASCII root filename "
               "using letters, digits, '.', '_' or '-'\n", command_name);
        return ESP_ERR_INVALID_ARG;
    }

    bool mounted_here = false;
    bool file_written = false;
    esp_err_t result = prepare_audio_command(&mounted_here);
    esp_codec_dev_handle_t microphone = NULL;
    esp_codec_dev_handle_t speaker = NULL;
    uint8_t *capture = NULL;
    if (result != ESP_OK) {
        return result;
    }

    const uint32_t capture_capacity = AUDIO_DEMO_MAX_SECONDS *
                                      AUDIO_DEMO_SAMPLE_RATE *
                                      AUDIO_DEMO_FRAME_BYTES;
    capture = heap_caps_malloc(capture_capacity,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (capture == NULL) {
        printf("%s: need %" PRIu32
               " bytes of PSRAM for the manual capture buffer\n",
               command_name, capture_capacity);
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    microphone = bsp_audio_codec_microphone_init();
    speaker = bsp_audio_codec_speaker_init();
    if (microphone == NULL || speaker == NULL) {
        printf("%s: codec initialization failed\n", command_name);
        result = ESP_FAIL;
        goto cleanup;
    }

    printf("%s: %s differential input -> logical CH%u; "
           "gain=%" PRIu32 " dB volume=%" PRIu32 " max=%u s\n",
           command_name, microphone_name, selected_channel, gain_db, volume,
           AUDIO_DEMO_MAX_SECONDS);
    if (wait_for_manual_record_start(command_name) != 'y') {
        printf("%s: cancelled before recording\n", command_name);
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    drain_console_input();
    printf("%s: RECORDING - send n to stop and play CH%u\n", command_name,
           selected_channel);
    fflush(stdout);

    stereo_capture_stats_t stats;
    uint32_t capture_size = 0;
    result = capture_stereo_manual(microphone, capture, capture_capacity,
                                   gain_db, command_name, "default", &stats,
                                   &capture_size);
    if (result != ESP_OK) {
        goto cleanup;
    }
    if (capture_size < AUDIO_DEMO_FRAME_BYTES) {
        printf("%s: no complete audio frame captured\n", command_name);
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    const uint32_t p2p[2] = {
        (uint32_t)((int32_t)stats.maximum[0] - stats.minimum[0]),
        (uint32_t)((int32_t)stats.maximum[1] - stats.minimum[1]),
    };
    const unsigned ignored_channel = selected_channel ^ 1U;
    const bool selected_live = p2p[selected_channel] >= 16U;
    const uint32_t duration_ms =
        (uint32_t)(((uint64_t)capture_size * 1000U) /
                   (AUDIO_DEMO_SAMPLE_RATE * AUDIO_DEMO_FRAME_BYTES));
    printf("%s: capture complete; duration_ms=%" PRIu32
           " ch%u peak=%u p2p=%" PRIu32 " live=%s; "
           "ch%u ignored peak=%u p2p=%" PRIu32 "\n",
           command_name, duration_ms, selected_channel,
           stats.peak[selected_channel], p2p[selected_channel],
           selected_live ? "yes" : "no", ignored_channel,
           stats.peak[ignored_channel], p2p[ignored_channel]);

    char microphone_detail[FACTORY_DETAIL_LENGTH];
    snprintf(microphone_detail, sizeof(microphone_detail),
             "manual %s/CH%u peak=%u p2p=%" PRIu32
             " gain=%" PRIu32 " dur_ms=%" PRIu32,
             microphone_name, selected_channel, stats.peak[selected_channel],
             p2p[selected_channel], gain_db, duration_ms);
    factory_report_set(FACTORY_TEST_MICROPHONE,
                       selected_live ? FACTORY_STATUS_WARN :
                                       FACTORY_STATUS_FAIL,
                       microphone_detail);
    factory_report_print_one(FACTORY_TEST_MICROPHONE);

    const uint32_t capture_checksum =
        fnv1a_update(UINT32_C(2166136261), capture, capture_size);
    result = wav_write_file(path, capture, capture_size);
    heap_caps_free(capture);
    capture = NULL;
    if (result != ESP_OK) {
        factory_report_error(FACTORY_TEST_SDCARD, result,
                             "manual microphone WAV write failed");
        goto cleanup;
    }
    file_written = true;
    printf("%s: wrote %s bytes=%" PRIu32 " checksum=0x%08" PRIx32 "\n",
           command_name, path, capture_size + AUDIO_DEMO_WAV_HEADER_BYTES,
           capture_checksum);

    uint32_t playback_checksum = 0;
    printf("%s: playing %s logical CH%u only\n", command_name,
           microphone_name, selected_channel);
    result = wav_play_file(speaker, path, volume, play_mode,
                           &playback_checksum);
    if (result != ESP_OK || playback_checksum != capture_checksum) {
        if (result == ESP_OK) {
            result = ESP_ERR_INVALID_CRC;
        }
        printf("%s: TF readback checksum mismatch capture=0x%08" PRIx32
               " playback=0x%08" PRIx32 "\n", command_name,
               capture_checksum, playback_checksum);
        factory_report_error(FACTORY_TEST_SDCARD, result,
                             "manual microphone WAV readback/playback failed");
        goto cleanup;
    }

    char sd_detail[FACTORY_DETAIL_LENGTH];
    snprintf(sd_detail, sizeof(sd_detail),
             "manual %s/CH%u WAV write/read passed bytes=%" PRIu32
             " checksum=%08" PRIx32,
             microphone_name, selected_channel,
             capture_size + AUDIO_DEMO_WAV_HEADER_BYTES, capture_checksum);
    factory_report_set(FACTORY_TEST_SDCARD, FACTORY_STATUS_PASS, sd_detail);
    factory_report_print_one(FACTORY_TEST_SDCARD);

    char speaker_detail[FACTORY_DETAIL_LENGTH];
    snprintf(speaker_detail, sizeof(speaker_detail),
             "%s CH%u WAV played; external confirmation pending",
             microphone_name, selected_channel);
    factory_report_set(FACTORY_TEST_SPEAKER, FACTORY_STATUS_NOT_RUN,
                       speaker_detail);
    factory_report_print_one(FACTORY_TEST_SPEAKER);

    if (!selected_live) {
        printf("%s: FAIL - %s/CH%u was effectively silent\n", command_name,
               microphone_name, selected_channel);
        result = ESP_FAIL;
    }

cleanup:
    heap_caps_free(capture);
    result = finish_audio_command(mounted_here, microphone, speaker, result);
    printf("%s: %s (%s); %s\n", command_name,
           result == ESP_OK ? "PASS" : "FAIL", esp_err_to_name(result),
           file_written ? path : "no new WAV file written");
    return result;
}

static int command_mic1_ch0_manual(int argc, char **argv)
{
    return command_manual_mic_channel(
        argc, argv, "mic1_ch0_manual", "MIC1", 0, WAV_PLAY_LEFT,
        AUDIO_DEMO_MIC1_MANUAL_FILE);
}

static int command_mic2_ch1_manual(int argc, char **argv)
{
    return command_manual_mic_channel(
        argc, argv, "mic2_ch1_manual", "MIC2", 1, WAV_PLAY_RIGHT,
        AUDIO_DEMO_MIC2_MANUAL_FILE);
}

static bool parse_play_mode(const char *text, wav_play_mode_t *mode)
{
    if (strcmp(text, "mix") == 0 || strcmp(text, "stereo") == 0) {
        *mode = WAV_PLAY_MIX;
    } else if (strcmp(text, "left") == 0 || strcmp(text, "ch0") == 0) {
        *mode = WAV_PLAY_LEFT;
    } else if (strcmp(text, "right") == 0 || strcmp(text, "ch1") == 0) {
        *mode = WAV_PLAY_RIGHT;
    } else {
        return false;
    }
    return true;
}

static int command_wav_play(int argc, char **argv)
{
    uint32_t volume = AUDIO_DEMO_DEFAULT_VOLUME;
    wav_play_mode_t mode = WAV_PLAY_MIX;
    if (argc < 2 || argc > 4 ||
            (argc >= 3 && !parse_u32(argv[2], 0, 100, &volume)) ||
            (argc >= 4 && !parse_play_mode(argv[3], &mode))) {
        printf("usage: wav_play FILE.wav [VOLUME 0-100] "
               "[mix|ch0|ch1]\n");
        return ESP_ERR_INVALID_ARG;
    }
    char path[sizeof(BSP_SD_MOUNT_POINT) + AUDIO_DEMO_MAX_FILENAME + 2U];
    if (!make_sd_path(argv[1], path, sizeof(path))) {
        printf("wav_play: FILE must be a root filename using ASCII letters, "
               "digits, '.', '_' or '-'\n");
        return ESP_ERR_INVALID_ARG;
    }

    bool mounted_here = false;
    esp_err_t result = prepare_audio_command(&mounted_here);
    esp_codec_dev_handle_t speaker = NULL;
    if (result == ESP_OK) {
        speaker = bsp_audio_codec_speaker_init();
        if (speaker == NULL) {
            result = ESP_FAIL;
        }
    }
    uint32_t checksum = 0;
    if (result == ESP_OK) {
        result = wav_play_file(speaker, path, volume, mode, &checksum);
    }
    if (result == ESP_OK) {
        printf("wav_play: completed checksum=0x%08" PRIx32 "\n", checksum);
        const char answer = factory_console_ask_operator(
                                "speaker", "Did you hear clean WAV playback?",
                                OPERATOR_PROMPT_TIMEOUT_S);
        factory_report_operator_verdict(
            FACTORY_TEST_SPEAKER, answer,
            "operator confirmed clean WAV playback",
            "operator reported missing or distorted WAV playback",
            "WAV data sent; operator confirmation pending");
        if (answer == 'n') {
            result = ESP_FAIL;
        }
    }
    result = finish_audio_command(mounted_here, NULL, speaker, result);
    return result;
}

esp_err_t factory_audio_file_register(void)
{
    const esp_console_cmd_t commands[] = {
        {
            .command = "audio_record_playback",
            .help = "Record stereo MIC WAV to TF, then play both channels or CH0 only.",
            .func = command_audio_record_playback,
        },
        {
            .command = "wav_play",
            .help = "Play 16-bit PCM WAV from TF: wav_play FILE [VOL] [mix|ch0|ch1].",
            .func = command_wav_play,
        },
        {
            .command = "mic1_ch0_manual",
            .help = "Manual MIC1/CH0 test: y starts recording, n stops and plays CH0.",
            .func = command_mic1_ch0_manual,
        },
        {
            .command = "mic2_ch1_manual",
            .help = "Manual MIC2/CH1 test: y starts recording, n stops and plays CH1.",
            .func = command_mic2_ch1_manual,
        },
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]);
            ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
