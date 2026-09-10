/*
 * Candis-S31 TE-synchronized LVGL video backend.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "player_video_backend.h"

#include <inttypes.h>
#include <string.h>

#include "bsp/display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

static const char *TAG = "player_video_backend";

#define PRESENT_PERIOD_US   33333
#define PRESENT_TOLERANCE_US 3000
#define FRAME_ALIGNMENT     128
#define PRESENT_BUFFER_COUNT 3
#define PRESENT_TIMER_MS    5

typedef enum {
    BUFFER_FREE = 0,
    BUFFER_WRITING,
    BUFFER_READY,
    BUFFER_DISPLAYED,
} buffer_state_t;

typedef struct {
    lv_display_t *display;
    lv_obj_t *image;
    lv_image_dsc_t descriptors[PRESENT_BUFFER_COUNT];
    esp_video_render_fb_t framebuffer;
    uint8_t *present_buffers[PRESENT_BUFFER_COUNT];
    uint8_t *canvas_buffer;
    buffer_state_t buffer_states[PRESENT_BUFFER_COUNT];
    uint32_t buffer_sequences[PRESENT_BUFFER_COUNT];
    esp_video_render_pos_t buffer_positions[PRESENT_BUFFER_COUNT];
    bool buffer_position_valid[PRESENT_BUFFER_COUNT];
    portMUX_TYPE buffer_mux;
    lv_timer_t *present_timer;
    esp_video_render_pos_t last_position;
    int64_t next_present_us;
    uint32_t next_sequence;
    int8_t current_index;
    bool direct_enabled;
    uint16_t content_width;
    uint16_t content_height;
    uint16_t content_x;
    uint16_t content_y;
    uint16_t source_width;
    uint16_t source_height;
} player_backend_t;

static player_backend_t *s_backend;

static esp_video_render_err_t backend_deinit(esp_video_render_backend_handle_t handle);

static void backend_present_timer(lv_timer_t *timer)
{
    player_backend_t *backend = lv_timer_get_user_data(timer);
    if (backend == NULL || backend->image == NULL || !backend->direct_enabled) {
        return;
    }

    int selected = -1;
    uint32_t newest_sequence = 0;

    taskENTER_CRITICAL(&backend->buffer_mux);
    for (int i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        if (backend->buffer_states[i] == BUFFER_READY &&
                (selected < 0 || backend->buffer_sequences[i] > newest_sequence)) {
            selected = i;
            newest_sequence = backend->buffer_sequences[i];
        }
    }
    if (selected >= 0) {
        backend->buffer_states[selected] = BUFFER_DISPLAYED;
        backend->current_index = (int8_t)selected;
    }
    taskEXIT_CRITICAL(&backend->buffer_mux);

    if (selected < 0) {
        return;
    }
    memcpy(backend->canvas_buffer, backend->present_buffers[selected],
           backend->framebuffer.size);
    lv_obj_invalidate(backend->image);

    taskENTER_CRITICAL(&backend->buffer_mux);
    backend->buffer_states[selected] = BUFFER_FREE;
    backend->current_index = -1;
    taskEXIT_CRITICAL(&backend->buffer_mux);
}

static esp_video_render_err_t backend_ensure_fb(player_backend_t *backend)
{
    if (backend->framebuffer.data != NULL) {
        return ESP_VIDEO_RENDER_ERR_OK;
    }
    backend->framebuffer.data = heap_caps_aligned_calloc(
        FRAME_ALIGNMENT, 1, backend->framebuffer.size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (backend->framebuffer.data == NULL) {
        return ESP_VIDEO_RENDER_ERR_NO_MEM;
    }
    backend->canvas_buffer = heap_caps_aligned_calloc(
        FRAME_ALIGNMENT, 1, backend->framebuffer.size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (backend->canvas_buffer == NULL) {
        return ESP_VIDEO_RENDER_ERR_NO_MEM;
    }
    return ESP_VIDEO_RENDER_ERR_OK;
}

static void backend_free_buffers(player_backend_t *backend)
{
    heap_caps_free(backend->framebuffer.data);
    backend->framebuffer.data = NULL;
    heap_caps_free(backend->canvas_buffer);
    backend->canvas_buffer = NULL;
    for (size_t i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        heap_caps_free(backend->present_buffers[i]);
        backend->present_buffers[i] = NULL;
        backend->descriptors[i].data = NULL;
    }
}

static esp_video_render_err_t backend_ensure_present_buffers(player_backend_t *backend)
{
    for (size_t i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        if (backend->present_buffers[i] == NULL) {
            backend->present_buffers[i] = heap_caps_aligned_calloc(
                FRAME_ALIGNMENT, 1, backend->framebuffer.size,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (backend->present_buffers[i] == NULL) {
                return ESP_VIDEO_RENDER_ERR_NO_MEM;
            }
        }
    }
    return ESP_VIDEO_RENDER_ERR_OK;
}

static esp_video_render_err_t backend_init(void *config, int config_size,
                                           esp_video_render_backend_handle_t *output)
{
    if (config == NULL || output == NULL ||
            config_size < (int)sizeof(esp_video_render_lvgl_cfg_t)) {
        return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    }
    const esp_video_render_lvgl_cfg_t *cfg = config;
    if (cfg->lv_disp == NULL || cfg->width == 0 || cfg->height == 0) {
        return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    }
    player_backend_t *backend = heap_caps_calloc(1, sizeof(*backend), MALLOC_CAP_INTERNAL);
    if (backend == NULL) {
        return ESP_VIDEO_RENDER_ERR_NO_MEM;
    }
    portMUX_INITIALIZE(&backend->buffer_mux);
    backend->current_index = -1;
    backend->display = cfg->lv_disp;
    backend->framebuffer.info.format = cfg->out_format;
    backend->framebuffer.info.width = cfg->width;
    backend->framebuffer.info.height = cfg->height;
    backend->content_width = cfg->width;
    backend->content_height = cfg->height;
    backend->framebuffer.size = (uint32_t)cfg->width * cfg->height * 2U;
    for (size_t i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        backend->descriptors[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        backend->descriptors[i].header.cf =
            cfg->out_format == ESP_VIDEO_RENDER_FORMAT_RGB565_BE ?
            LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
        backend->descriptors[i].header.w = cfg->width;
        backend->descriptors[i].header.h = cfg->height;
        backend->descriptors[i].header.stride = (uint16_t)(cfg->width * 2U);
        backend->descriptors[i].data_size = backend->framebuffer.size;
    }

    if (backend_ensure_fb(backend) != ESP_VIDEO_RENDER_ERR_OK ||
            backend_ensure_present_buffers(backend) != ESP_VIDEO_RENDER_ERR_OK) {
        backend_free_buffers(backend);
        heap_caps_free(backend);
        return ESP_VIDEO_RENDER_ERR_NO_MEM;
    }

    if (!lvgl_port_lock(0)) {
        backend_free_buffers(backend);
        heap_caps_free(backend);
        return ESP_VIDEO_RENDER_ERR_TIMEOUT;
    }
    backend->image = lv_image_create(lv_display_get_screen_active(backend->display));
    if (backend->image != NULL) {
        lv_obj_set_size(backend->image, cfg->width, cfg->height);
        lv_obj_set_pos(backend->image, 0, 0);
        lv_obj_move_to_index(backend->image, 0);
        backend->descriptors[0].data = backend->canvas_buffer;
        lv_image_set_src(backend->image, &backend->descriptors[0]);
        backend->present_timer = lv_timer_create(backend_present_timer,
                                                 PRESENT_TIMER_MS, backend);
    }
    lvgl_port_unlock();
    if (backend->image == NULL || backend->present_timer == NULL) {
        if (lvgl_port_lock(0)) {
            if (backend->image != NULL) {
                lv_obj_delete(backend->image);
            }
            lvgl_port_unlock();
        }
        backend_free_buffers(backend);
        heap_caps_free(backend);
        return ESP_VIDEO_RENDER_ERR_NO_MEM;
    }
    *output = backend;
    s_backend = backend;
    ESP_LOGI(TAG, "backend ready: %ux%u stride=%u, triple-buffered at 30 fps",
             cfg->width, cfg->height, backend->descriptors[0].header.stride);
    return ESP_VIDEO_RENDER_ERR_OK;
}

static bool backend_with_gram(esp_video_render_backend_handle_t handle)
{
    (void)handle;
    return true;
}

static esp_video_render_err_t backend_get_display_info(
    esp_video_render_backend_handle_t handle, esp_video_render_disp_info_t *info)
{
    if (handle == NULL || info == NULL) {
        return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    }
    player_backend_t *backend = handle;
    info->format = backend->framebuffer.info.format;
    taskENTER_CRITICAL(&backend->buffer_mux);
    info->width = backend->content_width;
    info->height = backend->content_height;
    taskEXIT_CRITICAL(&backend->buffer_mux);
    return ESP_VIDEO_RENDER_ERR_OK;
}

static esp_video_render_err_t backend_get_fb(esp_video_render_backend_handle_t handle,
                                             esp_video_render_fb_t *framebuffer)
{
    if (handle == NULL || framebuffer == NULL) {
        return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    }
    player_backend_t *backend = handle;
    const esp_video_render_err_t error = backend_ensure_fb(backend);
    if (error != ESP_VIDEO_RENDER_ERR_OK) {
        return error;
    }
    *framebuffer = backend->framebuffer;
    return ESP_VIDEO_RENDER_ERR_OK;
}

static esp_video_render_err_t backend_lock_fb(esp_video_render_backend_handle_t handle,
                                              esp_video_render_fb_t *framebuffer,
                                              bool lock)
{
    (void)handle;
    (void)framebuffer;
    (void)lock;
    /* The renderer composes into a private ingress buffer which LVGL never
     * reads, so it does not need to hold the LVGL mutex. */
    return ESP_VIDEO_RENDER_ERR_OK;
}

static bool should_queue_frame(player_backend_t *backend, int64_t now_us)
{
    if (backend->next_present_us == 0 ||
            now_us + PRESENT_TOLERANCE_US >= backend->next_present_us) {
        backend->next_present_us = now_us + PRESENT_PERIOD_US;
        return true;
    }
    return false;
}

static esp_video_render_err_t backend_write_fb(
    esp_video_render_backend_handle_t handle, esp_video_render_fb_t *framebuffer,
    const esp_video_render_rect_t *dirty_rect, const esp_video_render_pos_t *position)
{
    if (handle == NULL || framebuffer == NULL || framebuffer->data == NULL) {
        return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    }
    player_backend_t *backend = handle;
    uint16_t content_width;
    uint16_t content_height;
    uint16_t content_x;
    uint16_t content_y;
    uint16_t visible_source_width;
    uint16_t visible_source_height;
    taskENTER_CRITICAL(&backend->buffer_mux);
    content_width = backend->content_width;
    content_height = backend->content_height;
    content_x = backend->content_x;
    content_y = backend->content_y;
    visible_source_width = backend->source_width;
    visible_source_height = backend->source_height;
    taskEXIT_CRITICAL(&backend->buffer_mux);
    const uint32_t source_stride = (uint32_t)framebuffer->info.width * 2U;
    const uint32_t target_stride = (uint32_t)backend->framebuffer.info.width * 2U;
    const uint32_t required_source_size = source_stride * framebuffer->info.height;
    if (visible_source_width == 0 || visible_source_height == 0 ||
            framebuffer->info.width < visible_source_width ||
            framebuffer->info.height < visible_source_height ||
            framebuffer->info.format != backend->framebuffer.info.format ||
            framebuffer->size < required_source_size) {
        ESP_LOGE(TAG, "invalid frame: fmt=0x%x %ux%u size=%" PRIu32
                 ", expected fmt=0x%x width>=%u height>=%u source-size>=%" PRIu32,
                 framebuffer->info.format, framebuffer->info.width,
                 framebuffer->info.height, framebuffer->size,
                 backend->framebuffer.info.format,
                 backend->framebuffer.info.width, backend->framebuffer.info.height,
                 backend->framebuffer.size);
        return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    }
    (void)dirty_rect;
    const int64_t now_us = esp_timer_get_time();
    if (!should_queue_frame(backend, now_us)) {
        return ESP_VIDEO_RENDER_ERR_OK;
    }

    int write_index = -1;
    taskENTER_CRITICAL(&backend->buffer_mux);
    for (int i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        if (backend->buffer_states[i] == BUFFER_FREE) {
            backend->buffer_states[i] = BUFFER_WRITING;
            write_index = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&backend->buffer_mux);
    if (write_index < 0) {
        return ESP_VIDEO_RENDER_ERR_OK;
    }

    uint8_t *target = backend->present_buffers[write_index];
    const uint8_t *source = framebuffer->data;
    memset(target, 0, backend->framebuffer.size);
    const uint32_t target_offset =
        ((uint32_t)content_y * backend->framebuffer.info.width + content_x) * 2U;
    if (visible_source_width == content_width &&
            visible_source_height == content_height) {
        for (uint16_t row = 0; row < content_height; ++row) {
            memcpy(target + target_offset + (uint32_t)row * target_stride,
                   source + (uint32_t)row * source_stride,
                   (uint32_t)content_width * 2U);
        }
    } else {
        for (uint16_t dst_y = 0; dst_y < content_height; ++dst_y) {
            const uint32_t src_y =
                ((uint32_t)dst_y * visible_source_height) / content_height;
            uint8_t *dst_row = target + target_offset + (uint32_t)dst_y * target_stride;
            const uint8_t *src_row = source + src_y * source_stride;
            for (uint16_t dst_x = 0; dst_x < content_width; ++dst_x) {
                const uint32_t src_x =
                    ((uint32_t)dst_x * visible_source_width) / content_width;
                dst_row[dst_x * 2U] = src_row[src_x * 2U];
                dst_row[dst_x * 2U + 1U] = src_row[src_x * 2U + 1U];
            }
        }
    }

    taskENTER_CRITICAL(&backend->buffer_mux);
    for (int i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        if (i != write_index && backend->buffer_states[i] == BUFFER_READY) {
            backend->buffer_states[i] = BUFFER_FREE;
        }
    }
    backend->buffer_sequences[write_index] = ++backend->next_sequence;
    backend->buffer_position_valid[write_index] = position != NULL;
    if (position != NULL) {
        backend->buffer_positions[write_index] = *position;
    }
    backend->buffer_states[write_index] = BUFFER_READY;
    taskEXIT_CRITICAL(&backend->buffer_mux);
    return ESP_VIDEO_RENDER_ERR_OK;
}

static esp_video_render_err_t backend_deinit(esp_video_render_backend_handle_t handle)
{
    if (handle == NULL) {
        return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    }
    player_backend_t *backend = handle;
    if (lvgl_port_lock(0)) {
        if (backend->present_timer != NULL) {
            lv_timer_delete(backend->present_timer);
            backend->present_timer = NULL;
        }
        if (backend->image != NULL) {
            lv_obj_delete(backend->image);
        }
        lvgl_port_unlock();
    }
    backend_free_buffers(backend);
    if (s_backend == backend) {
        s_backend = NULL;
    }
    heap_caps_free(backend);
    return ESP_VIDEO_RENDER_ERR_OK;
}

void player_video_backend_set_direct(bool enabled)
{
    player_backend_t *backend = s_backend;
    if (backend == NULL || backend->direct_enabled == enabled) {
        return;
    }
    backend->direct_enabled = enabled;
}

void player_video_backend_set_source_size(uint16_t width, uint16_t height)
{
    player_backend_t *backend = s_backend;
    if (backend == NULL || width == 0 || height == 0) {
        return;
    }
    const uint32_t panel_width = backend->framebuffer.info.width;
    const uint32_t panel_height = backend->framebuffer.info.height;
    uint32_t fit_width = panel_width;
    uint32_t fit_height = ((uint64_t)height * panel_width) / width;
    if (fit_height > panel_height) {
        fit_height = panel_height;
        fit_width = ((uint64_t)width * panel_height) / height;
    }
    fit_width &= ~1U;
    fit_height &= ~1U;
    if (fit_width < 2U) {
        fit_width = 2U;
    }
    if (fit_height < 2U) {
        fit_height = 2U;
    }
    taskENTER_CRITICAL(&backend->buffer_mux);
    backend->content_width = (uint16_t)fit_width;
    backend->content_height = (uint16_t)fit_height;
    backend->content_x = (uint16_t)((panel_width - fit_width) / 2U);
    backend->content_y = (uint16_t)((panel_height - fit_height) / 2U);
    backend->source_width = width;
    backend->source_height = height;
    taskEXIT_CRITICAL(&backend->buffer_mux);
    ESP_LOGI(TAG, "aspect fit %ux%u -> %ux%u at (%u,%u)",
             width, height, (unsigned)fit_width, (unsigned)fit_height,
             (unsigned)((panel_width - fit_width) / 2U),
             (unsigned)((panel_height - fit_height) / 2U));
}

esp_err_t player_video_backend_init(void)
{
    if (s_backend != NULL) {
        return ESP_OK;
    }
    esp_video_render_lvgl_cfg_t config = {
        .lv_disp = lv_display_get_default(),
        .out_format = ESP_VIDEO_RENDER_FORMAT_RGB565_BE,
        .width = BSP_LCD_H_RES,
        .height = BSP_LCD_V_RES,
    };
    esp_video_render_backend_handle_t handle = NULL;
    const esp_video_render_err_t error = backend_init(&config, sizeof(config), &handle);
    return error == ESP_VIDEO_RENDER_ERR_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t player_video_backend_submit_rgb888(const uint8_t *data,
                                              uint16_t stride_width,
                                              uint16_t frame_height)
{
    player_backend_t *backend = s_backend;
    if (backend == NULL || data == NULL || stride_width == 0 || frame_height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t now_us = esp_timer_get_time();
    if (!should_queue_frame(backend, now_us)) {
        return ESP_OK;
    }

    uint16_t content_width;
    uint16_t content_height;
    uint16_t content_x;
    uint16_t content_y;
    uint16_t source_width;
    uint16_t source_height;
    int write_index = -1;
    taskENTER_CRITICAL(&backend->buffer_mux);
    content_width = backend->content_width;
    content_height = backend->content_height;
    content_x = backend->content_x;
    content_y = backend->content_y;
    source_width = backend->source_width;
    source_height = backend->source_height;
    for (int i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        if (backend->buffer_states[i] == BUFFER_FREE) {
            backend->buffer_states[i] = BUFFER_WRITING;
            write_index = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&backend->buffer_mux);
    if (write_index < 0 || source_width == 0 || source_height == 0 ||
            stride_width < source_width || frame_height < source_height) {
        if (write_index >= 0) {
            taskENTER_CRITICAL(&backend->buffer_mux);
            backend->buffer_states[write_index] = BUFFER_FREE;
            taskEXIT_CRITICAL(&backend->buffer_mux);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *target = backend->present_buffers[write_index];
    memset(target, 0, backend->framebuffer.size);
    const uint32_t target_stride = (uint32_t)backend->framebuffer.info.width * 2U;
    const uint32_t target_offset =
        ((uint32_t)content_y * backend->framebuffer.info.width + content_x) * 2U;
    const uint32_t source_stride = (uint32_t)stride_width * 3U;
    for (uint16_t dst_y = 0; dst_y < content_height; ++dst_y) {
        const uint32_t src_y = ((uint32_t)dst_y * source_height) / content_height;
        const uint8_t *src_row = data + src_y * source_stride;
        uint8_t *dst_row = target + target_offset + (uint32_t)dst_y * target_stride;
        for (uint16_t dst_x = 0; dst_x < content_width; ++dst_x) {
            const uint32_t src_x = ((uint32_t)dst_x * source_width) / content_width;
            const uint8_t *rgb = src_row + src_x * 3U;
            const uint16_t pixel = (uint16_t)(((uint16_t)(rgb[0] & 0xF8U) << 8) |
                                              ((uint16_t)(rgb[1] & 0xFCU) << 3) |
                                              ((uint16_t)rgb[2] >> 3));
            dst_row[dst_x * 2U] = (uint8_t)(pixel >> 8);
            dst_row[dst_x * 2U + 1U] = (uint8_t)pixel;
        }
    }

    taskENTER_CRITICAL(&backend->buffer_mux);
    for (int i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        if (i != write_index && backend->buffer_states[i] == BUFFER_READY) {
            backend->buffer_states[i] = BUFFER_FREE;
        }
    }
    backend->buffer_sequences[write_index] = ++backend->next_sequence;
    backend->buffer_states[write_index] = BUFFER_READY;
    taskEXIT_CRITICAL(&backend->buffer_mux);
    return ESP_OK;
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    return value > 255 ? 255 : (uint8_t)value;
}

esp_err_t player_video_backend_submit_yuv420(const uint8_t *data,
                                              uint16_t stride_width,
                                              uint16_t frame_height)
{
    player_backend_t *backend = s_backend;
    if (backend == NULL || data == NULL || stride_width == 0 || frame_height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!should_queue_frame(backend, esp_timer_get_time())) {
        return ESP_OK;
    }
    uint16_t content_width;
    uint16_t content_height;
    uint16_t content_x;
    uint16_t content_y;
    uint16_t source_width;
    uint16_t source_height;
    int write_index = -1;
    taskENTER_CRITICAL(&backend->buffer_mux);
    content_width = backend->content_width;
    content_height = backend->content_height;
    content_x = backend->content_x;
    content_y = backend->content_y;
    source_width = backend->source_width;
    source_height = backend->source_height;
    for (int i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        if (backend->buffer_states[i] == BUFFER_FREE) {
            backend->buffer_states[i] = BUFFER_WRITING;
            write_index = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&backend->buffer_mux);
    if (write_index < 0 || stride_width < source_width || frame_height < source_height) {
        if (write_index >= 0) {
            taskENTER_CRITICAL(&backend->buffer_mux);
            backend->buffer_states[write_index] = BUFFER_FREE;
            taskEXIT_CRITICAL(&backend->buffer_mux);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t y_size = (uint32_t)stride_width * frame_height;
    const uint32_t uv_stride = stride_width / 2U;
    const uint32_t uv_height = frame_height / 2U;
    const uint8_t *plane_y = data;
    const uint8_t *plane_u = data + y_size;
    const uint8_t *plane_v = plane_u + uv_stride * uv_height;
    uint8_t *target = backend->present_buffers[write_index];
    memset(target, 0, backend->framebuffer.size);
    const uint32_t target_stride = (uint32_t)backend->framebuffer.info.width * 2U;
    const uint32_t target_offset =
        ((uint32_t)content_y * backend->framebuffer.info.width + content_x) * 2U;
    for (uint16_t dst_y = 0; dst_y < content_height; ++dst_y) {
        const uint32_t src_y = ((uint32_t)dst_y * source_height) / content_height;
        uint8_t *dst_row = target + target_offset + (uint32_t)dst_y * target_stride;
        for (uint16_t dst_x = 0; dst_x < content_width; ++dst_x) {
            const uint32_t src_x = ((uint32_t)dst_x * source_width) / content_width;
            const int y = (int)plane_y[src_y * stride_width + src_x] - 16;
            const int u = (int)plane_u[(src_y / 2U) * uv_stride + src_x / 2U] - 128;
            const int v = (int)plane_v[(src_y / 2U) * uv_stride + src_x / 2U] - 128;
            const int c = y < 0 ? 0 : y;
            const uint8_t r = clamp_u8((298 * c + 409 * v + 128) >> 8);
            const uint8_t g = clamp_u8((298 * c - 100 * u - 208 * v + 128) >> 8);
            const uint8_t b = clamp_u8((298 * c + 516 * u + 128) >> 8);
            const uint16_t pixel = (uint16_t)(((uint16_t)(r & 0xF8U) << 8) |
                                              ((uint16_t)(g & 0xFCU) << 3) |
                                              ((uint16_t)b >> 3));
            dst_row[dst_x * 2U] = (uint8_t)(pixel >> 8);
            dst_row[dst_x * 2U + 1U] = (uint8_t)pixel;
        }
    }
    taskENTER_CRITICAL(&backend->buffer_mux);
    for (int i = 0; i < PRESENT_BUFFER_COUNT; ++i) {
        if (i != write_index && backend->buffer_states[i] == BUFFER_READY) {
            backend->buffer_states[i] = BUFFER_FREE;
        }
    }
    backend->buffer_sequences[write_index] = ++backend->next_sequence;
    backend->buffer_states[write_index] = BUFFER_READY;
    taskEXIT_CRITICAL(&backend->buffer_mux);
    return ESP_OK;
}
