/*
 * Candis-S31 player demo - MJPEG AVI player core.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "video_player.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_service.h"
#include "aviparser.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "video_player";

#define TASK_STACK_BYTES     (1024 * 8)
#define TASK_PRIORITY        4
#define MAX_JPEG_SIZE        (1024 * 512)
#define AUDIO_BUF_SAMPLES    2048
#define CMD_QUEUE_DEPTH      8

typedef enum {
    CMD_PLAY = 0,
    CMD_STOP,
    CMD_PAUSE,
    CMD_SEEK,
    CMD_SET_BRIGHTNESS,
    CMD_SET_VOLUME,
    CMD_SET_SPEED,
} player_cmd_type_t;

typedef struct {
    player_cmd_type_t type;
    int value;
} player_cmd_t;

typedef struct {
    video_play_request_t req;
    avi_handle_t avi;
    jpeg_dec_handle_t jpeg_dec;
    uint8_t *jpeg_buf;
    uint8_t *frame_buf; /**< decoded RGB565, 16-byte aligned */
    int frame_width;
    int frame_height;
    bool active;
    bool paused;
    bool stop_requested;
    int current_percent;
    int volume;
    int brightness;
    float speed;
    SemaphoreHandle_t lock;
    QueueHandle_t queue;
    TaskHandle_t task;
    esp_lcd_panel_handle_t panel;
} player_state_t;

static player_state_t s = {0};

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

static void emit(player_event_t ev, int value)
{
    if (s.req.cb != NULL) {
        s.req.cb(ev, value, s.req.user);
    }
}

static void draw_frame(void)
{
    if (s.frame_buf == NULL || s.panel == NULL) {
        return;
    }
    /* Center the frame on the 460x460 panel. */
    int x = (BSP_LCD_H_RES - s.frame_width) / 2;
    int y = (BSP_LCD_V_RES - s.frame_height) / 2;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    esp_lcd_panel_draw_bitmap(s.panel, x, y, x + s.frame_width,
                              y + s.frame_height, s.frame_buf);
}

static bool decode_jpeg_frame(uint8_t *jpeg_data, uint32_t jpeg_size)
{
    jpeg_dec_io_t io = {0};
    jpeg_dec_header_info_t info = {0};
    io.inbuf = jpeg_data;
    io.inbuf_len = (int)jpeg_size;

    jpeg_error_t err = jpeg_dec_parse_header(s.jpeg_dec, &io, &info);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg parse header failed: %d", err);
        return false;
    }

    s.frame_width = info.width;
    s.frame_height = info.height;
    int out_len = info.width * info.height * 2;
    if (s.frame_buf != NULL) {
        heap_caps_free(s.frame_buf);
    }
    s.frame_buf = heap_caps_aligned_alloc(16, out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s.frame_buf == NULL) {
        ESP_LOGE(TAG, "frame buffer alloc failed");
        return false;
    }
    io.outbuf = s.frame_buf;
    err = jpeg_dec_process(s.jpeg_dec, &io);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg decode failed: %d", err);
        return false;
    }
    return true;
}

static void player_cleanup(void)
{
    if (s.jpeg_dec != NULL) {
        jpeg_dec_close(s.jpeg_dec);
        s.jpeg_dec = NULL;
    }
    if (s.frame_buf != NULL) {
        heap_caps_free(s.frame_buf);
        s.frame_buf = NULL;
    }
    if (s.jpeg_buf != NULL) {
        free(s.jpeg_buf);
        s.jpeg_buf = NULL;
    }
    avi_close(&s.avi);
    audio_stream_stop();
    s.active = false;
    s.paused = false;
}

static void play_loop(void)
{
    if (!avi_seek_movi_start(&s.avi)) {
        ESP_LOGE(TAG, "seek movi failed");
        emit(PLAYER_EV_ERROR, 0);
        return;
    }

    if (s.avi.has_audio) {
        audio_stream_start((int)s.avi.audio_sample_rate, s.avi.audio_channels, s.volume);
    }

    int frame_count = 0;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t base_period_ms = s.avi.video_fps > 0 ? 1000 / s.avi.video_fps : 33;
    uint32_t frame_period_ms = (uint32_t)((float)base_period_ms / s.speed);
    if (frame_period_ms < 5) frame_period_ms = 5;

    avi_chunk_t chunk;
    while (s.active && !s.stop_requested) {
        if (s.paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            last_wake = xTaskGetTickCount();
            continue;
        }

        /* Process pending commands without blocking. */
        player_cmd_t cmd;
        while (xQueueReceive(s.queue, &cmd, 0) == pdPASS) {
            switch (cmd.type) {
            case CMD_STOP:
                s.stop_requested = true;
                break;
            case CMD_PAUSE:
                s.paused = cmd.value != 0;
                if (s.paused) {
                    audio_pause(true);
                } else {
                    audio_pause(false);
                    last_wake = xTaskGetTickCount();
                }
                break;
            case CMD_SET_BRIGHTNESS:
                bsp_display_brightness_set(cmd.value);
                break;
            case CMD_SET_VOLUME:
                audio_set_volume(cmd.value);
                break;
            case CMD_SET_SPEED: {
                float sp = (float)cmd.value / 100.0f;
                if (sp >= 0.5f && sp <= 2.0f) {
                    s.speed = sp;
                }
                break;
            }
            case CMD_SEEK: {
                avi_chunk_t chunk;
                if (avi_seek_video_near_percent(&s.avi, cmd.value, &chunk)) {
                    s.current_percent = cmd.value;
                    /* Next loop iteration will decode this chunk. */
                }
                break;
            }
            default:
                break;
            }
        }
        if (s.stop_requested) {
            break;
        }

        if (!avi_next_chunk(&s.avi, &chunk)) {
            break;
        }

        if (chunk.type == AVI_CHUNK_VIDEO) {
            if (chunk.size > MAX_JPEG_SIZE) {
                ESP_LOGW(TAG, "video chunk too large: %lu", chunk.size);
                continue;
            }
            if (!avi_read_chunk_data(&s.avi, &chunk, s.jpeg_buf, MAX_JPEG_SIZE)) {
                ESP_LOGE(TAG, "read video chunk failed");
                break;
            }
            if (!decode_jpeg_frame(s.jpeg_buf, chunk.size)) {
                continue;
            }
            draw_frame();
            frame_count++;
            long pos = ftell(s.avi.file);
            if (pos > 0 && s.avi.movi_size > 0) {
                s.current_percent = (int)(((pos - s.avi.movi_start) * 100) / s.avi.movi_size);
                if (s.current_percent > 100) s.current_percent = 100;
            }
            if (frame_count % 15 == 0) {
                emit(PLAYER_EV_PROGRESS, s.current_percent);
            }
            /* Pace to nominal frame rate. */
            xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(frame_period_ms));
        } else if (chunk.type == AVI_CHUNK_AUDIO && s.avi.has_audio) {
            if (chunk.size > MAX_JPEG_SIZE) {
                continue;
            }
            if (!avi_read_chunk_data(&s.avi, &chunk, s.jpeg_buf, MAX_JPEG_SIZE)) {
                break;
            }
            audio_stream_write((const int16_t *)s.jpeg_buf, chunk.size / sizeof(int16_t));
        }
    }

    if (s.stop_requested) {
        emit(PLAYER_EV_FINISHED, s.current_percent);
    } else {
        emit(PLAYER_EV_FINISHED, 100);
    }
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        player_cmd_t cmd;
        if (xQueueReceive(s.queue, &cmd, pdMS_TO_TICKS(100)) != pdPASS) {
            continue;
        }
        if (cmd.type != CMD_PLAY) {
            continue;
        }

        xSemaphoreTake(s.lock, portMAX_DELAY);
        s.stop_requested = false;
        s.paused = false;
        s.active = true;
        s.current_percent = 0;
        xSemaphoreGive(s.lock);

        emit(PLAYER_EV_STARTED, 0);
        play_loop();
        player_cleanup();

        if (s.req.loop && !s.stop_requested) {
            /* Re-enqueue play command for loop. */
            player_cmd_t replay = {.type = CMD_PLAY};
            xQueueSend(s.queue, &replay, 0);
        }
    }
}

esp_err_t video_player_init(void)
{
    if (s.lock != NULL) {
        return ESP_OK;
    }
    s.lock = xSemaphoreCreateMutex();
    s.queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(player_cmd_t));
    if (s.lock == NULL || s.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Obtain panel handle from BSP display. */
    s.panel = bsp_display_get_panel_handle();
    if (s.panel == NULL) {
        ESP_LOGE(TAG, "panel handle not available");
        return ESP_FAIL;
    }
    esp_lcd_panel_disp_on_off(s.panel, true);

    s.jpeg_buf = malloc(MAX_JPEG_SIZE);
    if (s.jpeg_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    jpeg_error_t jerr = jpeg_dec_open(&cfg, &s.jpeg_dec);
    if (jerr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed: %d", jerr);
        return ESP_FAIL;
    }

    if (xTaskCreate(player_task, "video_player", TASK_STACK_BYTES, NULL,
                    TASK_PRIORITY, &s.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t video_player_play(const video_play_request_t *req)
{
    if (req == NULL || req->video_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.active) {
        s.stop_requested = true;
    }
    s.req = *req;
    s.volume = clamp_int(req->volume, 0, 100);
    s.speed = req->speed > 0.2f && req->speed < 5.0f ? req->speed : 1.0f;
    player_cmd_t cmd = {.type = CMD_PLAY};
    esp_err_t err = xQueueSend(s.queue, &cmd, pdMS_TO_TICKS(200)) == pdPASS ?
                    ESP_OK : ESP_ERR_TIMEOUT;
    xSemaphoreGive(s.lock);
    return err;
}

esp_err_t video_player_stop(void)
{
    player_cmd_t cmd = {.type = CMD_STOP};
    return xQueueSend(s.queue, &cmd, pdMS_TO_TICKS(200)) == pdPASS ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t video_player_pause(bool pause)
{
    player_cmd_t cmd = {.type = CMD_PAUSE, .value = pause ? 1 : 0};
    return xQueueSend(s.queue, &cmd, pdMS_TO_TICKS(200)) == pdPASS ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t video_player_seek(int percent)
{
    player_cmd_t cmd = {.type = CMD_SEEK, .value = clamp_int(percent, 0, 100)};
    return xQueueSend(s.queue, &cmd, pdMS_TO_TICKS(200)) == pdPASS ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t video_player_set_brightness(int brightness)
{
    player_cmd_t cmd = {.type = CMD_SET_BRIGHTNESS, .value = clamp_int(brightness, 0, 100)};
    return xQueueSend(s.queue, &cmd, pdMS_TO_TICKS(200)) == pdPASS ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t video_player_set_volume(int volume)
{
    player_cmd_t cmd = {.type = CMD_SET_VOLUME, .value = clamp_int(volume, 0, 100)};
    return xQueueSend(s.queue, &cmd, pdMS_TO_TICKS(200)) == pdPASS ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t video_player_set_speed(float speed)
{
    int v = (int)(speed * 100.0f);
    player_cmd_t cmd = {.type = CMD_SET_SPEED, .value = clamp_int(v, 50, 200)};
    return xQueueSend(s.queue, &cmd, pdMS_TO_TICKS(200)) == pdPASS ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

bool video_player_is_active(void)
{
    if (s.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool active = s.active;
    xSemaphoreGive(s.lock);
    return active;
}

bool video_player_is_paused(void)
{
    if (s.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool paused = s.paused;
    xSemaphoreGive(s.lock);
    return paused;
}
