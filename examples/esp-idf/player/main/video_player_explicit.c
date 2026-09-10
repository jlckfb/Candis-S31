/*
 * Candis-S31 explicit offline media player.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "video_player.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "audio_service.h"
#include "bsp/display.h"
#include "esp_check.h"
#include "esp_aac_dec.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_extractor.h"
#include "esp_extractor_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_dec.h"
#include "esp_video_dec_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include "player_utils.h"
#include "player_video_backend.h"
#include "storage_service.h"

static const char *TAG = "explicit_player";

#define CONTROL_TASK_STACK   (14 * 1024)
#define CONTROL_TASK_PRIO    6
#define CONTROL_QUEUE_DEPTH  16
#define PLAYER_PATH_MAX      320
#define EXTRACTOR_POOL_SIZE  (512 * 1024)
#define FRAME_ALIGNMENT      128
#define VIDEO_LATE_DROP_MS   80U
#define AUDIO_QUEUE_DEPTH    16
#define AUDIO_TASK_STACK     8192
#define AUDIO_TASK_PRIO      7

#define ALIGN_UP_16(value) (((value) + 15U) & ~15U)

typedef enum {
    CMD_PLAY = 0,
    CMD_STOP,
    CMD_PAUSE,
    CMD_SEEK,
    CMD_BRIGHTNESS,
    CMD_VOLUME,
    CMD_LOOP,
} command_type_t;

typedef struct {
    command_type_t type;
    int value;
    video_play_request_t request;
    char path[PLAYER_PATH_MAX];
} command_t;

typedef struct {
    uint32_t pts;
    uint32_t size;
    uint8_t data[];
} audio_packet_t;

typedef struct {
    QueueHandle_t queue;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool initialized;
    bool active;
    bool paused;
    bool loop;
    int volume;
    int brightness;
} player_state_t;

typedef struct {
    esp_extractor_config_t extractor_config;
    esp_extractor_handle_t extractor;
    FILE *file;
    uint32_t file_size;
    esp_video_dec_handle_t video_decoder;
    esp_audio_dec_handle_t audio_decoder;
    uint8_t *video_output;
    uint8_t *audio_output;
    uint32_t video_output_size;
    uint32_t audio_output_size;
    esp_extractor_video_stream_info_t video_info;
    esp_extractor_audio_stream_info_t audio_info;
    uint8_t *video_spec_info;
    uint32_t video_spec_info_len;
    uint32_t duration_ms;
    volatile uint32_t audio_clock_ms;
    volatile uint32_t audio_anchor_pts_ms;
    volatile uint32_t audio_anchor_wall_ms;
    volatile uint32_t audio_submitted_end_ms;
    volatile bool audio_clock_started;
    uint32_t wall_start_ms;
    uint32_t last_video_decode_pts;
    int last_progress;
    bool has_audio;
    bool has_video;
    bool stop;
    bool paused;
    bool loop;
    bool have_video_decode_pts;
    volatile bool audio_error;
    QueueHandle_t audio_queue;
    SemaphoreHandle_t audio_done;
    TaskHandle_t audio_task;
    video_play_request_t request;
    char path[PLAYER_PATH_MAX];
} playback_t;

static player_state_t s;

static uint32_t current_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t get_audio_clock(const playback_t *playback)
{
    if (!playback->audio_clock_started) {
        return 0;
    }
    const uint32_t wall_clock = playback->audio_anchor_pts_ms +
        (current_ms() - playback->audio_anchor_wall_ms);
    return wall_clock < playback->audio_submitted_end_ms ?
           wall_clock : playback->audio_submitted_end_ms;
}

static void audio_worker(void *arg)
{
    playback_t *playback = arg;
    audio_packet_t *packet = NULL;
    for (;;) {
        if (xQueueReceive(playback->audio_queue, &packet, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (packet == NULL) {
            break;
        }
        while (playback->paused && !playback->stop) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!playback->stop &&
                audio_write_pcm(packet->data, packet->size) == ESP_OK) {
            const uint32_t bytes_per_second =
                playback->audio_info.sample_rate * playback->audio_info.channel * 2U;
            if (!playback->audio_clock_started) {
                playback->audio_anchor_pts_ms = packet->pts;
                playback->audio_anchor_wall_ms = current_ms();
                playback->audio_submitted_end_ms = packet->pts;
                playback->audio_clock_started = true;
            }
            const uint32_t packet_duration = bytes_per_second != 0 ?
                (packet->size * 1000U) / bytes_per_second : 0U;
            const uint32_t packet_start =
                packet->pts > playback->audio_submitted_end_ms ?
                packet->pts : playback->audio_submitted_end_ms;
            playback->audio_submitted_end_ms = packet_start + packet_duration;
            playback->audio_clock_ms = get_audio_clock(playback);
        } else if (!playback->stop) {
            playback->audio_error = true;
        }
        heap_caps_free(packet);
    }
    while (!playback->stop && playback->audio_clock_started &&
            get_audio_clock(playback) < playback->audio_submitted_end_ms) {
        playback->audio_clock_ms = get_audio_clock(playback);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    xSemaphoreGive(playback->audio_done);
    vTaskDelete(NULL);
}

static esp_err_t start_audio_worker(playback_t *playback)
{
    playback->audio_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(audio_packet_t *));
    playback->audio_done = xSemaphoreCreateBinary();
    if (playback->audio_queue == NULL || playback->audio_done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_worker, "player_audio_out", AUDIO_TASK_STACK,
                    playback, AUDIO_TASK_PRIO, &playback->audio_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void finish_audio_worker(playback_t *playback)
{
    if (playback->audio_queue == NULL) {
        return;
    }
    audio_packet_t *end = NULL;
    xQueueSend(playback->audio_queue, &end, portMAX_DELAY);
    if (playback->audio_done != NULL) {
        xSemaphoreTake(playback->audio_done, pdMS_TO_TICKS(5000));
    }
    audio_packet_t *packet = NULL;
    while (xQueueReceive(playback->audio_queue, &packet, 0) == pdPASS) {
        heap_caps_free(packet);
    }
    vQueueDelete(playback->audio_queue);
    playback->audio_queue = NULL;
    if (playback->audio_done != NULL) {
        vSemaphoreDelete(playback->audio_done);
        playback->audio_done = NULL;
    }
    playback->audio_task = NULL;
}

static esp_err_t queue_audio_data(playback_t *playback, const uint8_t *data,
                                  uint32_t size, uint32_t pts)
{
    audio_packet_t *packet = heap_caps_malloc(sizeof(*packet) + size,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (packet == NULL) {
        return ESP_ERR_NO_MEM;
    }
    packet->pts = pts;
    packet->size = size;
    memcpy(packet->data, data, size);
    if (xQueueSend(playback->audio_queue, &packet, portMAX_DELAY) != pdPASS) {
        heap_caps_free(packet);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t queue_audio_packet(playback_t *playback,
                                    const esp_extractor_frame_info_t *frame)
{
    return queue_audio_data(playback, frame->frame_buffer,
                            frame->frame_size, frame->pts);
}

static int file_read(void *buffer, uint32_t size, void *ctx)
{
    playback_t *playback = ctx;
    return (int)fread(buffer, 1, size, playback->file);
}

static int file_seek(uint32_t position, void *ctx)
{
    playback_t *playback = ctx;
    return fseek(playback->file, (long)position, SEEK_SET);
}

static uint32_t file_size(void *ctx)
{
    return ((playback_t *)ctx)->file_size;
}

static void set_state(bool active, bool paused)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.active = active;
    s.paused = paused;
    xSemaphoreGive(s.lock);
}

static void emit(playback_t *playback, player_event_t event, int value)
{
    if (playback->request.cb != NULL) {
        playback->request.cb(event, value, playback->request.user);
    }
}

static void update_progress(playback_t *playback, uint32_t pts)
{
    if (playback->duration_ms == 0) {
        return;
    }
    int permille = (int)(((uint64_t)pts * 1000U) / playback->duration_ms);
    if (permille > 1000) {
        permille = 1000;
    }
    if (permille != playback->last_progress) {
        playback->last_progress = permille;
        emit(playback, PLAYER_EV_PROGRESS, permille);
    }
}

static void close_media(playback_t *playback)
{
    finish_audio_worker(playback);
    audio_finish_playback();
    if (playback->video_decoder != NULL) {
        esp_video_dec_close(playback->video_decoder);
        playback->video_decoder = NULL;
    }
    if (playback->audio_decoder != NULL) {
        esp_audio_dec_close(playback->audio_decoder);
        playback->audio_decoder = NULL;
    }
    heap_caps_free(playback->video_output);
    playback->video_output = NULL;
    playback->video_output_size = 0;
    heap_caps_free(playback->audio_output);
    playback->audio_output = NULL;
    playback->audio_output_size = 0;
    if (playback->extractor != NULL) {
        esp_extractor_close(playback->extractor);
        playback->extractor = NULL;
    }
    if (playback->file != NULL) {
        fclose(playback->file);
        playback->file = NULL;
    }
}

static esp_err_t open_media(playback_t *playback)
{
    playback->audio_error = false;
    playback->audio_clock_ms = 0;
    playback->audio_clock_started = false;
    playback->audio_submitted_end_ms = 0;
    playback->file = fopen(playback->path, "rb");
    if (playback->file == NULL || fseek(playback->file, 0, SEEK_END) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const long size = ftell(playback->file);
    if (size <= 0 || size > UINT32_MAX || fseek(playback->file, 0, SEEK_SET) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    playback->file_size = (uint32_t)size;
    playback->extractor_config = (esp_extractor_config_t) {
        .type = ESP_EXTRACTOR_TYPE_NONE,
        .extract_mask = ESP_EXTRACT_MASK_AV,
        .in_read_cb = file_read,
        .in_seek_cb = file_seek,
        .in_size_cb = file_size,
        .in_ctx = playback,
        .out_pool_size = EXTRACTOR_POOL_SIZE,
        .out_align = FRAME_ALIGNMENT,
    };
    if (esp_extractor_open(&playback->extractor_config, &playback->extractor) !=
            ESP_EXTRACTOR_ERR_OK ||
            esp_extractor_parse_stream(playback->extractor) != ESP_EXTRACTOR_ERR_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t count = 0;
    esp_extractor_stream_info_t info = {0};
    if (esp_extractor_get_stream_num(playback->extractor,
                                     ESP_EXTRACTOR_STREAM_TYPE_AUDIO,
                                     &count) == ESP_EXTRACTOR_ERR_OK && count > 0 &&
            esp_extractor_get_stream_info(playback->extractor,
                                          ESP_EXTRACTOR_STREAM_TYPE_AUDIO,
                                          0, &info) == ESP_EXTRACTOR_ERR_OK) {
        playback->has_audio = true;
        playback->audio_info = info.audio_info;
        if (info.duration > playback->duration_ms) {
            playback->duration_ms = info.duration;
        }
        ESP_LOGI(TAG, "audio format=0x%08x %" PRIu32 " Hz, %u ch, %u bit",
                 info.audio_info.format, info.audio_info.sample_rate,
                 info.audio_info.channel, info.audio_info.bits_per_sample);
    }

    count = 0;
    memset(&info, 0, sizeof(info));
    if (esp_extractor_get_stream_num(playback->extractor,
                                     ESP_EXTRACTOR_STREAM_TYPE_VIDEO,
                                     &count) != ESP_EXTRACTOR_ERR_OK || count == 0 ||
            esp_extractor_get_stream_info(playback->extractor,
                                          ESP_EXTRACTOR_STREAM_TYPE_VIDEO,
                                          0, &info) != ESP_EXTRACTOR_ERR_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    playback->has_video = true;
    playback->video_info = info.video_info;
    playback->video_spec_info = info.spec_info;
    playback->video_spec_info_len = info.spec_info_len;
    if (info.duration > playback->duration_ms) {
        playback->duration_ms = info.duration;
    }
    ESP_LOGI(TAG, "video format=0x%08x %ux%u@%u",
             info.video_info.format, info.video_info.width,
             info.video_info.height, info.video_info.fps);
    if (info.video_info.format != ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG &&
            info.video_info.format != ESP_EXTRACTOR_VIDEO_FORMAT_H264) {
        ESP_LOGE(TAG, "unsupported video codec 0x%08x", info.video_info.format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    player_video_backend_set_source_size(info.video_info.width,
                                         info.video_info.height);
    esp_video_dec_cfg_t decoder_config = {
        .codec_type = info.video_info.format == ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG ?
                      ESP_VIDEO_CODEC_TYPE_MJPEG : ESP_VIDEO_CODEC_TYPE_H264,
        .codec_cc = 0,
        .out_fmt = info.video_info.format == ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG ?
                   ESP_VIDEO_CODEC_PIXEL_FMT_RGB888 : ESP_VIDEO_CODEC_PIXEL_FMT_YUV420P,
        .codec_spec_info = playback->video_spec_info,
        .codec_spec_info_size = playback->video_spec_info_len,
    };
    if (esp_video_dec_open(&decoder_config, &playback->video_decoder) != ESP_VC_ERR_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t aligned_width = ALIGN_UP_16(info.video_info.width);
    const uint32_t aligned_height = ALIGN_UP_16(info.video_info.height);
    playback->video_output_size = info.video_info.format == ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG ?
                                  aligned_width * aligned_height * 3U :
                                  aligned_width * aligned_height * 3U / 2U;
    playback->video_output = heap_caps_aligned_alloc(
        FRAME_ALIGNMENT, playback->video_output_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (playback->video_output == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (playback->has_audio) {
        if ((playback->audio_info.format != ESP_EXTRACTOR_AUDIO_FORMAT_PCM &&
             playback->audio_info.format != ESP_EXTRACTOR_AUDIO_FORMAT_AAC) ||
                playback->audio_info.bits_per_sample != 16 ||
                playback->audio_info.channel != 2) {
            ESP_LOGE(TAG, "unsupported audio codec/format");
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (playback->audio_info.format == ESP_EXTRACTOR_AUDIO_FORMAT_AAC) {
            esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
            aac.sample_rate = playback->audio_info.sample_rate;
            aac.channel = playback->audio_info.channel;
            aac.bits_per_sample = playback->audio_info.bits_per_sample;
            aac.no_adts_header = true;
            esp_audio_dec_cfg_t audio_decoder_config = {
                .type = ESP_AUDIO_TYPE_AAC,
                .cfg = &aac,
                .cfg_sz = sizeof(aac),
            };
            if (esp_audio_dec_open(&audio_decoder_config,
                                   &playback->audio_decoder) != ESP_AUDIO_ERR_OK) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            playback->audio_output_size = 8192;
            playback->audio_output = heap_caps_malloc(
                playback->audio_output_size,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (playback->audio_output == NULL) {
                return ESP_ERR_NO_MEM;
            }
        }
        ESP_RETURN_ON_ERROR(audio_prepare_playback(playback->request.volume), TAG,
                            "audio prepare failed");
        ESP_RETURN_ON_ERROR(audio_configure_playback(playback->audio_info.sample_rate), TAG,
                            "audio configure failed");
        ESP_RETURN_ON_ERROR(start_audio_worker(playback), TAG,
                            "audio worker failed");
    }
    return ESP_OK;
}

static void handle_command(playback_t *playback, const command_t *command)
{
    switch (command->type) {
    case CMD_STOP:
        playback->stop = true;
        break;
    case CMD_PAUSE:
        playback->paused = command->value != 0;
        set_state(true, playback->paused);
        break;
    case CMD_SEEK:
        if (playback->extractor != NULL && playback->duration_ms != 0) {
            const uint32_t target = (uint32_t)player_percent_to_ms(
                command->value, playback->duration_ms);
            const esp_extractor_err_t error =
                esp_extractor_seek(playback->extractor, target);
            if (error == ESP_EXTRACTOR_ERR_OK) {
                playback->audio_clock_ms = target;
                playback->audio_clock_started = false;
                playback->audio_submitted_end_ms = target;
                playback->have_video_decode_pts = false;
                playback->wall_start_ms =
                    (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - target;
                playback->last_progress = -1;
            }
        }
        break;
    case CMD_BRIGHTNESS:
        s.brightness = command->value;
        bsp_display_brightness_set(command->value);
        break;
    case CMD_VOLUME:
        s.volume = command->value;
        audio_set_volume(command->value);
        break;
    case CMD_LOOP:
        s.loop = command->value != 0;
        playback->loop = s.loop;
        break;
    case CMD_PLAY:
        break;
    }
}

static void poll_controls(playback_t *playback)
{
    command_t command;
    while (xQueueReceive(s.queue, &command, 0) == pdPASS) {
        handle_command(playback, &command);
    }
    while (playback->paused && !playback->stop) {
        if (xQueueReceive(s.queue, &command, pdMS_TO_TICKS(50)) == pdPASS) {
            handle_command(playback, &command);
        }
    }
}

static esp_err_t decode_audio_frame(playback_t *playback,
                                    const esp_extractor_frame_info_t *frame)
{
    esp_audio_dec_in_raw_t raw = {
        .buffer = frame->frame_buffer,
        .len = frame->frame_size,
    };
    while (raw.len > 0) {
        raw.consumed = 0;
        esp_audio_dec_out_frame_t output = {
            .buffer = playback->audio_output,
            .len = playback->audio_output_size,
        };
        esp_audio_err_t result =
            esp_audio_dec_process(playback->audio_decoder, &raw, &output);
        if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (output.needed_size == 0 || output.needed_size > 64U * 1024U) {
                return ESP_ERR_INVALID_SIZE;
            }
            uint8_t *expanded = heap_caps_realloc(
                playback->audio_output, output.needed_size,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (expanded == NULL) {
                return ESP_ERR_NO_MEM;
            }
            playback->audio_output = expanded;
            playback->audio_output_size = output.needed_size;
            continue;
        }
        if (result != ESP_AUDIO_ERR_OK || raw.consumed > raw.len ||
                (raw.consumed == 0 && output.decoded_size == 0)) {
            ESP_LOGE(TAG, "AAC decode failed: %d consumed=%" PRIu32,
                     result, raw.consumed);
            return ESP_FAIL;
        }
        if (output.decoded_size > 0) {
            ESP_RETURN_ON_ERROR(queue_audio_data(playback, output.buffer,
                                                 output.decoded_size, frame->pts),
                                TAG, "queue decoded AAC failed");
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
    return ESP_OK;
}

static esp_err_t decode_video_frame(playback_t *playback,
                                    esp_extractor_frame_info_t *frame)
{
    esp_video_dec_in_frame_t input = {
        .pts = frame->pts,
        .dts = frame->pts,
        .data = frame->frame_buffer,
        .size = frame->frame_size,
    };
    do {
        input.consumed = 0;
        esp_video_dec_out_frame_t output = {
            .data = playback->video_output,
            .size = playback->video_output_size,
        };
        const esp_vc_err_t result =
            esp_video_dec_process(playback->video_decoder, &input, &output);
        if (result != ESP_VC_ERR_OK || input.consumed > input.size) {
            ESP_LOGE(TAG, "video decode failed: %d", result);
            return ESP_FAIL;
        }
        if (output.decoded_size > 0) {
            esp_video_codec_frame_info_t decoded = {0};
            if (esp_video_dec_get_frame_info(playback->video_decoder,
                                             &decoded) != ESP_VC_ERR_OK) {
                return ESP_FAIL;
            }
            const esp_err_t submit_error =
                playback->video_info.format == ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG ?
                player_video_backend_submit_rgb888(output.data,
                                                   decoded.res.width,
                                                   decoded.res.height) :
                player_video_backend_submit_yuv420(output.data,
                                                   decoded.res.width,
                                                   decoded.res.height);
            if (submit_error != ESP_OK) {
                return submit_error;
            }
        }
        if (input.consumed == 0) {
            break;
        }
        input.data += input.consumed;
        input.size -= input.consumed;
    } while (input.size > 0);
    return ESP_OK;
}

static esp_err_t play_once(playback_t *playback)
{
    esp_err_t error = open_media(playback);
    if (error != ESP_OK) {
        return error;
    }
    playback->wall_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    playback->have_video_decode_pts = false;
    playback->last_progress = -1;
    emit(playback, PLAYER_EV_STARTED, 0);
    ESP_LOGI(TAG, "playback started: %s", playback->path);

    for (;;) {
        poll_controls(playback);
        if (playback->stop) {
            break;
        }
        esp_extractor_frame_info_t frame = {0};
        const esp_extractor_err_t result =
            esp_extractor_read_frame(playback->extractor, &frame);
        if (result == ESP_EXTRACTOR_ERR_WAITING_OUTPUT) {
            vTaskDelay(1);
            continue;
        }
        if (result == ESP_EXTRACTOR_ERR_EOS) {
            break;
        }
        if (result != ESP_EXTRACTOR_ERR_OK) {
            ESP_LOGE(TAG, "extract frame failed: %d", result);
            return ESP_FAIL;
        }

        if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_AUDIO &&
                playback->has_audio) {
            error = playback->audio_decoder != NULL ?
                    decode_audio_frame(playback, &frame) :
                    queue_audio_packet(playback, &frame);
        } else if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_VIDEO) {
            const bool decode_this_frame =
                playback->video_info.format == ESP_EXTRACTOR_VIDEO_FORMAT_H264 ||
                !playback->have_video_decode_pts ||
                frame.pts >= playback->last_video_decode_pts + 33U;
            uint32_t clock_ms = playback->has_audio ? get_audio_clock(playback) :
                (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) -
                playback->wall_start_ms;
            if (frame.pts > clock_ms + 2U) {
                vTaskDelay(pdMS_TO_TICKS(frame.pts - clock_ms));
                clock_ms = frame.pts;
            }
            if (decode_this_frame && frame.pts + VIDEO_LATE_DROP_MS >= clock_ms) {
                error = decode_video_frame(playback, &frame);
                if (error == ESP_OK) {
                    playback->last_video_decode_pts = frame.pts;
                    playback->have_video_decode_pts = true;
                }
            }
        }
        update_progress(playback, frame.pts);
        esp_extractor_release_frame(playback->extractor, &frame);
        if (playback->audio_error) {
            error = ESP_FAIL;
        }
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

static void run_session(const command_t *command)
{
    playback_t playback = {
        .request = command->request,
        .loop = command->request.loop,
        .last_progress = -1,
    };
    memcpy(playback.path, command->path, sizeof(playback.path));
    playback.request.video_path = playback.path;

    storage_lease_t lease = {0};
    esp_err_t error = storage_lease_acquire(&lease);
    if (error != ESP_OK) {
        set_state(false, false);
        emit(&playback, PLAYER_EV_ERROR, error);
        return;
    }

    do {
        error = play_once(&playback);
        close_media(&playback);
        if (playback.stop || error != ESP_OK || !playback.loop) {
            break;
        }
        playback.audio_clock_ms = 0;
        playback.last_progress = -1;
    } while (true);

    storage_lease_release(&lease);
    player_video_backend_set_direct(false);
    set_state(false, false);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "playback ended: %s reason=%s", playback.path,
                 playback.stop ? "stopped" : "eof");
        emit(&playback, PLAYER_EV_FINISHED,
             playback.stop ? playback.last_progress : 100);
    } else {
        ESP_LOGE(TAG, "playback failed: %s error=%s", playback.path, esp_err_to_name(error));
        emit(&playback, PLAYER_EV_ERROR, error);
    }
}

static void control_task(void *arg)
{
    (void)arg;
    command_t command;
    for (;;) {
        if (xQueueReceive(s.queue, &command, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (command.type == CMD_PLAY) {
            run_session(&command);
        } else if (command.type == CMD_BRIGHTNESS) {
            bsp_display_brightness_set(command.value);
        } else if (command.type == CMD_VOLUME) {
            audio_set_volume(command.value);
        } else if (command.type == CMD_LOOP) {
            s.loop = command.value != 0;
        }
    }
}

static esp_err_t post(const command_t *command)
{
    if (!s.initialized || s.queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s.queue, command, pdMS_TO_TICKS(100)) == pdPASS ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t video_player_init(void)
{
    if (s.initialized) {
        return ESP_OK;
    }
    s.lock = xSemaphoreCreateMutex();
    s.queue = xQueueCreate(CONTROL_QUEUE_DEPTH, sizeof(command_t));
    if (s.lock == NULL || s.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(audio_start(), TAG, "audio init failed");
    ESP_RETURN_ON_ERROR(media_lib_add_default_adapter(), TAG, "media adapter failed");
    if (esp_extractor_register_default() != 0 ||
            esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
            esp_video_dec_register_default() != ESP_VC_ERR_OK) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(player_video_backend_init(), TAG, "video backend failed");
    s.volume = 20;
    s.brightness = 60;
    s.initialized = true;
    if (xTaskCreate(control_task, "explicit_player", CONTROL_TASK_STACK, NULL,
                    CONTROL_TASK_PRIO, &s.task) != pdPASS) {
        s.initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "explicit extractor -> HW MJPEG -> framebuffer pipeline ready");
    return ESP_OK;
}

esp_err_t video_player_play(const video_play_request_t *request)
{
    if (request == NULL || request->video_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strnlen(request->video_path, PLAYER_PATH_MAX);
    if (length == 0 || length >= PLAYER_PATH_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.active) {
        xSemaphoreGive(s.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s.active = true;
    s.paused = false;
    xSemaphoreGive(s.lock);
    command_t command = {.type = CMD_PLAY, .request = *request};
    memcpy(command.path, request->video_path, length + 1U);
    command.request.video_path = command.path;
    const esp_err_t error = post(&command);
    if (error != ESP_OK) {
        set_state(false, false);
    }
    return error;
}

esp_err_t video_player_stop(void)
{
    const command_t command = {.type = CMD_STOP};
    return post(&command);
}

esp_err_t video_player_pause(bool pause)
{
    const command_t command = {.type = CMD_PAUSE, .value = pause};
    return post(&command);
}

esp_err_t video_player_seek(int percent)
{
    const command_t command = {
        .type = CMD_SEEK,
        .value = player_clamp_percent(percent),
    };
    return post(&command);
}

esp_err_t video_player_set_brightness(int brightness)
{
    const command_t command = {
        .type = CMD_BRIGHTNESS,
        .value = player_clamp_percent(brightness),
    };
    return post(&command);
}

esp_err_t video_player_set_volume(int volume)
{
    const command_t command = {
        .type = CMD_VOLUME,
        .value = player_clamp_percent(volume),
    };
    return post(&command);
}

esp_err_t video_player_set_loop(bool loop)
{
    const command_t command = {.type = CMD_LOOP, .value = loop};
    return post(&command);
}

bool video_player_is_active(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool active = s.active;
    xSemaphoreGive(s.lock);
    return active;
}

bool video_player_is_paused(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool paused = s.paused;
    xSemaphoreGive(s.lock);
    return paused;
}
