/*
 * Candis-S31 camera preview surface.
 *
 * Camera DMA buffers are copied into display-owned PSRAM buffers so the LVGL
 * image source stays valid after the V4L2 buffer is re-queued.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_visual.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_rom_crc.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "lvgl.h"
#include "sdkconfig.h"

#define VISUAL_SERIAL_DUMP_BAUD 460800U
static const char *TAG = "camera_visual";

#define VISUAL_WIDTH             460U
#define VISUAL_HEIGHT            460U
#define VISUAL_PIXEL_BYTES       2U
#define VISUAL_STRIDE            (VISUAL_WIDTH * VISUAL_PIXEL_BYTES)
#define VISUAL_BUFFER_BYTES      (VISUAL_STRIDE * VISUAL_HEIGHT)
#define VISUAL_BUFFER_ALIGN 64U
#define VISUAL_BUFFER_ALLOC_BYTES \
    ((VISUAL_BUFFER_BYTES + VISUAL_BUFFER_ALIGN - 1U) & \
     ~(VISUAL_BUFFER_ALIGN - 1U))
#define VISUAL_CROP_X            ((800U - VISUAL_WIDTH) / 2U)
#define VISUAL_CROP_Y            ((600U - VISUAL_HEIGHT) / 2U)
#define VISUAL_SOURCE_WIDTH      800U
#define VISUAL_SOURCE_HEIGHT     600U
#define VISUAL_BUFFER_COUNT      4U
#define VISUAL_TIMER_MS          1U
#define VISUAL_MESSAGE_HEIGHT    80

typedef struct {
    lv_obj_t *image;
    lv_obj_t *message;
    lv_timer_t *timer;
    lv_image_dsc_t image_dsc;
    uint8_t *buffers[VISUAL_BUFFER_COUNT];
    uint8_t free_mask;
    uint8_t retire_mask;
    bool flushed_in_cycle;
    int ready_index;
    int active_index;
    camera_visual_stats_t progress;
    bool refresh_pending;
    bool image_set;
    char error[80];
    portMUX_TYPE mux;
    bool started;
} visual_state_t;

static visual_state_t s_visual = {
    .ready_index = -1,
    .active_index = -1,
    .mux = portMUX_INITIALIZER_UNLOCKED,
};

static void visual_refresh_done(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_FLUSH_START) {
        s_visual.flushed_in_cycle = true;
    } else if (s_visual.flushed_in_cycle) {
        s_visual.flushed_in_cycle = false;
        portENTER_CRITICAL(&s_visual.mux);
        if (s_visual.refresh_pending) {
            ++s_visual.progress.refreshed;
            s_visual.refresh_pending = false;
        }
        s_visual.free_mask |= s_visual.retire_mask;
        s_visual.retire_mask = 0;
        portEXIT_CRITICAL(&s_visual.mux);
    }
}

static void visual_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    int new_index = -1;
    bool image_set;
    char error[sizeof(s_visual.error)];

    portENTER_CRITICAL(&s_visual.mux);
    if (s_visual.ready_index >= 0) {
        new_index = s_visual.ready_index;
        s_visual.ready_index = -1;
        const int old_index = s_visual.active_index;
        s_visual.active_index = new_index;
        ++s_visual.progress.presented;
        if (s_visual.refresh_pending) {
            ++s_visual.progress.superseded;
        }
        s_visual.refresh_pending = true;
        if (old_index >= 0) {
            s_visual.retire_mask |= (uint8_t)(1U << old_index);
        }
    }
    image_set = s_visual.image_set;
    memcpy(error, s_visual.error, sizeof(error));
    portEXIT_CRITICAL(&s_visual.mux);

    if (new_index >= 0 && s_visual.image != NULL) {
        s_visual.image_dsc.data = s_visual.buffers[new_index];
        if (!image_set) {
            s_visual.image_set = true;
            lv_image_set_src(s_visual.image, &s_visual.image_dsc);
        } else {
            lv_obj_invalidate(s_visual.image);
        }
    }
    if (s_visual.message == NULL) {
        return;
    }
    if (error[0] != '\0') {
        lv_obj_remove_flag(s_visual.message, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_visual.message, error);
    } else if (s_visual.image_set) {
        lv_obj_add_flag(s_visual.message, LV_OBJ_FLAG_HIDDEN);
    }
}

static void visual_free_buffers(void)
{
    for (unsigned index = 0; index < VISUAL_BUFFER_COUNT; ++index) {
        if (s_visual.buffers[index] != NULL) {
            heap_caps_free(s_visual.buffers[index]);
            s_visual.buffers[index] = NULL;
        }
    }
}

esp_err_t camera_visual_start(void)
{
    if (s_visual.started) {
        return ESP_OK;
    }

    for (unsigned index = 0; index < VISUAL_BUFFER_COUNT; ++index) {
        s_visual.buffers[index] = heap_caps_aligned_alloc(
            VISUAL_BUFFER_ALIGN, VISUAL_BUFFER_ALLOC_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (s_visual.buffers[index] == NULL) {
            ESP_LOGE(TAG, "display buffer allocation failed index=%u bytes=%u",
                     index, (unsigned)VISUAL_BUFFER_ALLOC_BYTES);
            visual_free_buffers();
            return ESP_ERR_NO_MEM;
        }
        memset(s_visual.buffers[index], 0, VISUAL_BUFFER_ALLOC_BYTES);
    }

    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        visual_free_buffers();
        return ESP_FAIL;
    }
    if (bsp_display_brightness_set(100) != ESP_OK) {
        ESP_LOGW(TAG, "display brightness set failed; continuing");
    }

    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock failed while creating the preview page");
        visual_free_buffers();
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, VISUAL_WIDTH, VISUAL_HEIGHT);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    memset(&s_visual.image_dsc, 0, sizeof(s_visual.image_dsc));
    s_visual.image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_visual.image_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    s_visual.image_dsc.header.w = VISUAL_WIDTH;
    s_visual.image_dsc.header.h = VISUAL_HEIGHT;
    s_visual.image_dsc.header.stride = VISUAL_STRIDE;
    s_visual.image_dsc.data_size = VISUAL_BUFFER_BYTES;
    s_visual.image = lv_image_create(root);
    lv_obj_set_pos(s_visual.image, 0, 0);

    s_visual.message = lv_label_create(root);
    lv_obj_set_size(s_visual.message, VISUAL_WIDTH - 40,
                    VISUAL_MESSAGE_HEIGHT);
    lv_obj_set_pos(s_visual.message, 20, 190);
    lv_obj_set_style_bg_color(s_visual.message, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_visual.message, LV_OPA_70, 0);
    lv_obj_set_style_text_color(s_visual.message, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_visual.message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(s_visual.message, 8, 0);
    lv_label_set_text(s_visual.message, "Starting camera");

    s_visual.free_mask = (uint8_t)((1U << VISUAL_BUFFER_COUNT) - 1U);
    s_visual.ready_index = -1;
    s_visual.active_index = -1;
    s_visual.retire_mask = 0;
    s_visual.flushed_in_cycle = false;
    lv_display_add_event_cb(lv_display_get_default(), visual_refresh_done,
                            LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), visual_refresh_done,
                            LV_EVENT_REFR_READY, NULL);
    memset(&s_visual.progress, 0, sizeof(s_visual.progress));
    s_visual.refresh_pending = false;
    s_visual.image_set = false;
    s_visual.error[0] = '\0';
    s_visual.timer = lv_timer_create(visual_ui_timer_cb, VISUAL_TIMER_MS, NULL);
    s_visual.started = true;

    lv_obj_invalidate(root);
    bsp_display_unlock();
    return ESP_OK;
}

static int visual_claim_buffer(void)
{
    int target = -1;
    portENTER_CRITICAL(&s_visual.mux);
    if (s_visual.started && s_visual.free_mask != 0) {
        for (unsigned index = 0; index < VISUAL_BUFFER_COUNT; ++index) {
            if ((s_visual.free_mask & (1U << index)) != 0) {
                target = (int)index;
                s_visual.free_mask &= (uint8_t)~(1U << index);
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_visual.mux);
    return target;
}

static void visual_submit_buffer(int target)
{
    portENTER_CRITICAL(&s_visual.mux);
    if (s_visual.ready_index >= 0) {
        s_visual.free_mask |= (uint8_t)(1U << s_visual.ready_index);
        ++s_visual.progress.superseded;
    }
    s_visual.ready_index = target;
    portEXIT_CRITICAL(&s_visual.mux);
}

camera_visual_stats_t camera_visual_get_stats(void)
{
    portENTER_CRITICAL(&s_visual.mux);
    const camera_visual_stats_t stats = s_visual.progress;
    portEXIT_CRITICAL(&s_visual.mux);
    return stats;
}

#if CONFIG_CAMERA_TEST_SERIAL_FRAME_DUMP
static bool visual_serial_write_all(const uint8_t *data, size_t length)
{
    while (length != 0) {
        const size_t chunk = length < 2048U ? length : 2048U;
        const int written = uart_write_bytes(UART_NUM_0, data, chunk);
        if (written <= 0) {
            return false;
        }
        data += written;
        length -= written;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

/* Export one source frame plus the exact bytes submitted to LVGL. The console
 * temporarily switches to 460800 baud; tools/capture_camera_frame.py receives
 * it and writes PPM images for comparison without the panel. */
static void visual_serial_dump(const uint8_t *frame, size_t frame_length,
                               const uint8_t *rgb565, uint32_t stride)
{
    static bool dumped;
    static int64_t eligible_since_us;
    const int64_t now_us = esp_timer_get_time();
    if (eligible_since_us == 0) {
        eligible_since_us = now_us;
    }
    /* The sensor needs a few frames to converge AEC/AWB after STREAMON. */
    const bool ready = !dumped &&
                       now_us - eligible_since_us >= INT64_C(3000000);
    if (!ready) {
        return;
    }
    dumped = true;
    const size_t raw_bytes = (size_t)stride * VISUAL_SOURCE_HEIGHT;
    if (frame_length < raw_bytes) {
        ESP_LOGE(TAG, "serial frame dump source is truncated");
        return;
    }
    const uint32_t raw_crc = esp_rom_crc32_le(0, frame, raw_bytes);
    const uint32_t rgb_crc = esp_rom_crc32_le(0, rgb565, VISUAL_BUFFER_BYTES);
    uint32_t previous_baud;
    if (uart_get_baudrate(UART_NUM_0, &previous_baud) != ESP_OK) {
        return;
    }
    /* The default ROM console need not install an IDF UART driver. Keep one
     * for this export's lifetime; binary writes must bypass stdio. */
    if (!uart_is_driver_installed(UART_NUM_0) &&
            uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "frame export UART driver installation failed");
        return;
    }
    ESP_LOGI(TAG, "CAMERA_FRAME_DUMP preparing raw=%u rgb=%u baud=%u",
             (unsigned)raw_bytes, (unsigned)VISUAL_BUFFER_BYTES,
             VISUAL_SERIAL_DUMP_BAUD);
    const esp_log_level_t previous_level = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    fflush(stdout);
    uart_wait_tx_idle_polling(UART_NUM_0);
    if (uart_set_baudrate(UART_NUM_0, VISUAL_SERIAL_DUMP_BAUD) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        char header[256];
        const int header_length = snprintf(header, sizeof(header),
               "CAMERA_FRAME_DUMP_BEGIN version=2 baud=%u raw=%u rgb=%u "
               "stride=%" PRIu32 " width=%u height=%u crop_x=%u crop_y=%u "
               "order=%s rgb_order=%s raw_crc32=%08" PRIx32
               " rgb_crc32=%08" PRIx32 "\n",
               VISUAL_SERIAL_DUMP_BAUD, (unsigned)raw_bytes,
               (unsigned)VISUAL_BUFFER_BYTES, stride,
               VISUAL_SOURCE_WIDTH, VISUAL_SOURCE_HEIGHT,
               VISUAL_CROP_X, VISUAL_CROP_Y, "RGB565X", "big",
               raw_crc, rgb_crc);
        const bool ok = header_length > 0 &&
                        (size_t)header_length < sizeof(header) &&
                        visual_serial_write_all((const uint8_t *)header, header_length) &&
                        visual_serial_write_all(frame, raw_bytes) &&
                        visual_serial_write_all(rgb565, VISUAL_BUFFER_BYTES);
        const char *trailer = ok ? "\nCAMERA_FRAME_DUMP_END status=PASS\n" :
                                   "\nCAMERA_FRAME_DUMP_END status=FAIL\n";
        visual_serial_write_all((const uint8_t *)trailer, strlen(trailer));
        uart_wait_tx_idle_polling(UART_NUM_0);
        vTaskDelay(pdMS_TO_TICKS(100));
        uart_set_baudrate(UART_NUM_0, previous_baud);
    }
    esp_log_level_set("*", previous_level);
}
#endif

bool camera_visual_publish(const uint8_t *frame, size_t length, uint32_t stride)
{
    if (frame == NULL || stride < VISUAL_SOURCE_WIDTH * VISUAL_PIXEL_BYTES ||
            length < ((size_t)(VISUAL_CROP_Y + VISUAL_HEIGHT - 1U) * stride) +
                     ((size_t)(VISUAL_CROP_X + VISUAL_WIDTH) *
                      VISUAL_PIXEL_BYTES)) {
        return false;
    }

    const int target = visual_claim_buffer();
    if (target < 0) {
        return false;
    }

    uint8_t *destination = s_visual.buffers[target];
    for (unsigned row = 0; row < VISUAL_HEIGHT; ++row) {
        const uint8_t *source = frame +
            ((size_t)(VISUAL_CROP_Y + row) * stride) +
            (size_t)VISUAL_CROP_X * VISUAL_PIXEL_BYTES;
        uint8_t *output = destination + (size_t)row * VISUAL_STRIDE;
        memcpy(output, source, VISUAL_STRIDE);
    }
#if CONFIG_CAMERA_TEST_SERIAL_FRAME_DUMP
    visual_serial_dump(frame, length, destination, stride);
#endif
    visual_submit_buffer(target);
    return true;
}

void camera_visual_set_error(const char *message)
{
    portENTER_CRITICAL(&s_visual.mux);
    if (message == NULL) {
        s_visual.error[0] = '\0';
    } else {
        snprintf(s_visual.error, sizeof(s_visual.error), "%s", message);
    }
    portEXIT_CRITICAL(&s_visual.mux);
}
