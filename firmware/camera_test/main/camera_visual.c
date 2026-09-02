/*
 * Candis-S31 camera_test live preview and visual confirmation.
 *
 * Camera DMA buffers are copied into two display-owned PSRAM buffers. This
 * keeps the LVGL image source valid while the V4L2 buffer is re-queued or the
 * camera pipeline is torn down between automatic test cycles.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_visual.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "lvgl.h"

static const char *TAG = "camera_visual";

#define VISUAL_WIDTH             460U
#define VISUAL_HEIGHT            460U
#define VISUAL_PIXEL_BYTES       2U
#define VISUAL_STRIDE            (VISUAL_WIDTH * VISUAL_PIXEL_BYTES)
#define VISUAL_BUFFER_BYTES      (VISUAL_STRIDE * VISUAL_HEIGHT)
#define VISUAL_CROP_X            ((800U - VISUAL_WIDTH) / 2U)
#define VISUAL_CROP_Y            ((600U - VISUAL_HEIGHT) / 2U)
#define VISUAL_SOURCE_WIDTH      800U
#define VISUAL_SOURCE_HEIGHT     600U
#define VISUAL_SOURCE_BYTES      (VISUAL_SOURCE_WIDTH * VISUAL_SOURCE_HEIGHT)
#define VISUAL_RAW_WIDTH         320U
#define VISUAL_RAW_HEIGHT        240U
#define VISUAL_RAW_ACTIVE_HEIGHT 344U
#define VISUAL_RAW_OFFSET_Y      ((VISUAL_HEIGHT - VISUAL_RAW_ACTIVE_HEIGHT) / 2U)
#define VISUAL_MATRIX_HALF       (VISUAL_WIDTH / 2U)
#define VISUAL_BUFFER_COUNT      3U
#define VISUAL_TIMER_MS          33U
#define VISUAL_BAR_HEIGHT        42
#define VISUAL_BUTTON_HEIGHT     52
#define VISUAL_BUTTON_Y          402

static const char *const s_bayer_names[CAMERA_BAYER_COUNT] = {
    "RGGB", "BGGR", "GRBG", "GBRG",
};


typedef struct {
    lv_obj_t *image;
    lv_obj_t *title;
    lv_obj_t *stats;
    lv_obj_t *hint;
    lv_obj_t *pass_button;
    lv_obj_t *fail_button;
    lv_obj_t *format_button;
    lv_timer_t *timer;
    lv_obj_t *phase_label;
    lv_image_dsc_t image_dsc;
    uint8_t *buffers[VISUAL_BUFFER_COUNT];
    uint8_t free_mask;
    int ready_index;
    int active_index;
    uint32_t frame_count;
    uint32_t dropped_count;
    unsigned auto_cycles;
    unsigned auto_frames;
    bool auto_passed;
    bool auto_finished;
    bool streaming;
    bool image_set;
    bool swapped;
    camera_visual_bayer_t bayer;
    camera_visual_decision_t decision;
    char error[80];
    portMUX_TYPE mux;
    bool started;
} visual_state_t;

static visual_state_t s_visual = {
    .ready_index = -1,
    .active_index = -1,
    .decision = CAMERA_VISUAL_PENDING,
    .mux = portMUX_INITIALIZER_UNLOCKED,
};

static void visual_pass_cb(lv_event_t *event)
{
    (void)event;
    bool allowed = false;
    portENTER_CRITICAL(&s_visual.mux);
    allowed = s_visual.started && s_visual.auto_finished &&
              s_visual.auto_passed && s_visual.streaming &&
              s_visual.decision == CAMERA_VISUAL_PENDING;
    if (allowed) {
        s_visual.decision = CAMERA_VISUAL_PASS;
    }
    portEXIT_CRITICAL(&s_visual.mux);

    if (allowed) {
        ESP_LOGI(TAG, "CAMERA_TEST_VISUAL_CONFIRM status=PASS source=touch");
    } else {
        ESP_LOGW(TAG, "CAMERA_TEST_VISUAL_CONFIRM ignored reason=auto_not_ready");
    }
}

static void visual_fail_cb(lv_event_t *event)
{
    (void)event;
    bool allowed = false;
    portENTER_CRITICAL(&s_visual.mux);
    allowed = s_visual.started && s_visual.streaming &&
              s_visual.decision == CAMERA_VISUAL_PENDING;
    if (allowed) {
        s_visual.decision = CAMERA_VISUAL_FAIL;
    }
    portEXIT_CRITICAL(&s_visual.mux);

    if (allowed) {
        ESP_LOGW(TAG, "CAMERA_TEST_VISUAL_CONFIRM status=FAIL source=touch");
    }
}
static void visual_format_cb(lv_event_t *event)
{
    (void)event;
    camera_visual_bayer_t bayer;
    portENTER_CRITICAL(&s_visual.mux);
    s_visual.bayer =
        (camera_visual_bayer_t)((s_visual.bayer + 1) % CAMERA_BAYER_COUNT);
    bayer = s_visual.bayer;
    portEXIT_CRITICAL(&s_visual.mux);
    if (s_visual.phase_label != NULL) {
        lv_label_set_text(s_visual.phase_label, s_bayer_names[bayer]);
    }
    ESP_LOGI(TAG, "CAMERA_TEST_BAYER_TOGGLE phase=%s",
             s_bayer_names[bayer]);
}

static lv_obj_t *visual_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t callback, int x, uint32_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 210, VISUAL_BUTTON_HEIGHT);
    lv_obj_set_pos(button, x, VISUAL_BUTTON_Y);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}
static lv_obj_t *visual_format_button(lv_obj_t *parent)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 106, 30);
    lv_obj_set_pos(button, 348, 6);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x3b4657), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_add_event_cb(button, visual_format_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, "FORMAT");
    s_visual.phase_label = label;
    lv_obj_center(label);
    return button;
}

static void visual_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    int new_index = -1;
    uint32_t frame_count;
    uint32_t dropped_count;
    unsigned auto_cycles;
    unsigned auto_frames;
    bool auto_finished;
    bool auto_passed;
    bool streaming;
    camera_visual_bayer_t bayer;
    camera_visual_decision_t decision;
    char error[sizeof(s_visual.error)];

    portENTER_CRITICAL(&s_visual.mux);
    if (s_visual.ready_index >= 0) {
        new_index = s_visual.ready_index;
        s_visual.ready_index = -1;
        const int old_index = s_visual.active_index;
        s_visual.active_index = new_index;
        if (old_index >= 0) {
            s_visual.free_mask |= (uint8_t)(1U << old_index);
        }
    }
    frame_count = s_visual.frame_count;
    dropped_count = s_visual.dropped_count;
    auto_cycles = s_visual.auto_cycles;
    auto_frames = s_visual.auto_frames;
    auto_finished = s_visual.auto_finished;
    auto_passed = s_visual.auto_passed;
    streaming = s_visual.streaming;
    bayer = s_visual.bayer;
    decision = s_visual.decision;
    memcpy(error, s_visual.error, sizeof(error));
    portEXIT_CRITICAL(&s_visual.mux);

    if (new_index >= 0 && s_visual.image != NULL) {
        s_visual.image_dsc.data = s_visual.buffers[new_index];
        if (!s_visual.image_set) {
            s_visual.image_set = true;
            lv_image_set_src(s_visual.image, &s_visual.image_dsc);
        } else {
            lv_obj_invalidate(s_visual.image);
        }
    }

    if (s_visual.title == NULL || s_visual.stats == NULL ||
            s_visual.hint == NULL) {
        return;
    }

    if (decision == CAMERA_VISUAL_PASS) {
        lv_label_set_text(s_visual.title, "VISUAL PASS - OPERATOR CONFIRMED");
        lv_label_set_text(s_visual.hint, "Camera stream stopped; result is recorded.");
    } else if (decision == CAMERA_VISUAL_FAIL) {
        lv_label_set_text(s_visual.title, "VISUAL FAIL - OPERATOR REPORTED");
        lv_label_set_text(s_visual.hint, "Result is recorded as FAIL; inspect the log.");
    } else if (error[0] != '\0') {
        lv_label_set_text(s_visual.title, "CAMERA STREAM ERROR");
        lv_label_set_text(s_visual.hint, error);
    } else if (auto_finished && auto_passed && streaming) {
        lv_label_set_text(s_visual.title, "CHECK LIVE CAMERA IMAGE");
        lv_label_set_text(s_visual.hint,
                          "Check geometry, color, and motion; then tap PASS or FAIL.");
    } else if (auto_finished) {
        lv_label_set_text(s_visual.title, "AUTOMATIC CAMERA CHECK FAILED");
        lv_label_set_text(s_visual.hint, "Do not tap PASS; inspect the serial diagnostics.");
    } else if (streaming) {
        lv_label_set_text(s_visual.title, "CAMERA LIVE - CHECKS RUNNING");
        lv_label_set_text(s_visual.hint,
                          "Live frame is converted for the display.");
    } else {
        lv_label_set_text(s_visual.title, "STARTING CAMERA");
        lv_label_set_text(s_visual.hint, "Please wait for the live image.");
    }

    lv_label_set_text_fmt(s_visual.stats,
                          "auto=%s LIVE %s c=%u f=%u shown=%" PRIu32 " d=%u",
                          auto_finished ? (auto_passed ? "PASS" : "FAIL") :
                          "RUN", s_bayer_names[bayer],
                          auto_cycles, auto_frames, frame_count,
                          (unsigned)dropped_count);

    const bool enable_pass = auto_finished && auto_passed && streaming &&
                             decision == CAMERA_VISUAL_PENDING;
    const bool enable_fail = streaming && decision == CAMERA_VISUAL_PENDING;
    if (s_visual.pass_button != NULL) {
        if (enable_pass) {
            lv_obj_remove_state(s_visual.pass_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_visual.pass_button, LV_STATE_DISABLED);
        }
    }
    if (s_visual.fail_button != NULL) {
        if (enable_fail) {
            lv_obj_remove_state(s_visual.fail_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_visual.fail_button, LV_STATE_DISABLED);
        }
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
        s_visual.buffers[index] = heap_caps_malloc(
            VISUAL_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_visual.buffers[index] == NULL) {
            ESP_LOGE(TAG, "display buffer allocation failed index=%u bytes=%u",
                     index, (unsigned)VISUAL_BUFFER_BYTES);
            visual_free_buffers();
            return ESP_ERR_NO_MEM;
        }
        memset(s_visual.buffers[index], 0, VISUAL_BUFFER_BYTES);
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
        ESP_LOGE(TAG, "LVGL lock failed while creating camera page");
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
    s_visual.swapped = false;
    s_visual.bayer = CAMERA_BAYER_RGGB;
    s_visual.image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_visual.image_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    s_visual.image_dsc.header.w = VISUAL_WIDTH;
    s_visual.image_dsc.header.h = VISUAL_HEIGHT;
    s_visual.image_dsc.header.stride = VISUAL_STRIDE;
    s_visual.image_dsc.data_size = VISUAL_BUFFER_BYTES;
    s_visual.image = lv_image_create(root);
    lv_obj_set_pos(s_visual.image, 0, 0);

    s_visual.title = lv_label_create(root);
    lv_obj_set_size(s_visual.title, 340, VISUAL_BAR_HEIGHT);
    lv_obj_set_pos(s_visual.title, 0, 0);
    lv_obj_set_style_bg_color(s_visual.title, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_visual.title, LV_OPA_70, 0);
    lv_obj_set_style_text_color(s_visual.title, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_visual.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(s_visual.title, 10, 0);
    lv_label_set_text(s_visual.title, "STARTING CAMERA");
    s_visual.format_button = visual_format_button(root);

    s_visual.stats = lv_label_create(root);
    lv_obj_set_size(s_visual.stats, VISUAL_WIDTH, 24);
    lv_obj_set_pos(s_visual.stats, 0, VISUAL_BAR_HEIGHT);
    lv_obj_set_style_bg_color(s_visual.stats, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_visual.stats, LV_OPA_60, 0);
    lv_obj_set_style_text_color(s_visual.stats, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_visual.stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_visual.stats,
                      "auto=RUN LIVE c=0 f=0 shown=0 d=0");

    s_visual.hint = lv_label_create(root);
    lv_obj_set_size(s_visual.hint, VISUAL_WIDTH - 20, 38);
    lv_obj_set_pos(s_visual.hint, 10, 355);
    lv_obj_set_style_bg_color(s_visual.hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_visual.hint, LV_OPA_70, 0);
    lv_obj_set_style_text_color(s_visual.hint, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_visual.hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(s_visual.hint, 8, 0);
    lv_label_set_text(s_visual.hint, "Please wait for the live image.");

    s_visual.pass_button = visual_button(root, "PASS (looks correct)",
                                         visual_pass_cb, 8, 0x176b45);
    s_visual.fail_button = visual_button(root, "FAIL (looks wrong)",
                                         visual_fail_cb, 242, 0x8d3030);
    lv_obj_add_state(s_visual.pass_button, LV_STATE_DISABLED);
    lv_obj_add_state(s_visual.fail_button, LV_STATE_DISABLED);

    s_visual.free_mask = (uint8_t)((1U << VISUAL_BUFFER_COUNT) - 1U);
    s_visual.ready_index = -1;
    s_visual.active_index = -1;
    s_visual.frame_count = 0;
    s_visual.dropped_count = 0;
    s_visual.auto_cycles = 0;
    s_visual.auto_frames = 0;
    s_visual.auto_passed = false;
    s_visual.auto_finished = false;
    s_visual.streaming = false;
    s_visual.image_set = false;
    s_visual.decision = CAMERA_VISUAL_PENDING;
    s_visual.error[0] = '\0';
    s_visual.timer = lv_timer_create(visual_ui_timer_cb, VISUAL_TIMER_MS, NULL);
    s_visual.started = true;

    lv_obj_invalidate(root);
    bsp_display_unlock();
    ESP_LOGI(TAG, "camera visual confirmation screen ready");
    return ESP_OK;
}

static int visual_claim_buffer(camera_visual_bayer_t *bayer)
{
    int target = -1;
    portENTER_CRITICAL(&s_visual.mux);
    if (bayer != NULL) {
        *bayer = s_visual.bayer;
    }
    if (s_visual.started && s_visual.free_mask != 0) {
        for (unsigned index = 0; index < VISUAL_BUFFER_COUNT; ++index) {
            if ((s_visual.free_mask & (1U << index)) != 0) {
                target = (int)index;
                s_visual.free_mask &= (uint8_t)~(1U << index);
                break;
            }
        }
    } else if (s_visual.started) {
        ++s_visual.dropped_count;
    }
    portEXIT_CRITICAL(&s_visual.mux);
    return target;
}

static void visual_submit_buffer(int target)
{
    portENTER_CRITICAL(&s_visual.mux);
    if (s_visual.ready_index >= 0) {
        s_visual.free_mask |= (uint8_t)(1U << s_visual.ready_index);
    }
    s_visual.ready_index = target;
    ++s_visual.frame_count;
    portEXIT_CRITICAL(&s_visual.mux);
}

static inline void visual_raw_cell(const uint8_t *frame, uint32_t stride,
                                   unsigned x, unsigned y,
                                   camera_visual_bayer_t bayer,
                                   uint8_t *red, uint8_t *green,
                                   uint8_t *blue)
{
    const uint8_t p00 = frame[(size_t)y * stride + x * 2U];
    const uint8_t p01 = frame[(size_t)y * stride + (x + 1U) * 2U];
    const uint8_t p10 = frame[(size_t)(y + 1U) * stride + x * 2U];
    const uint8_t p11 = frame[(size_t)(y + 1U) * stride + (x + 1U) * 2U];
    if (bayer == CAMERA_BAYER_RGGB) {
        *red = p00;
        *green = (uint8_t)(((unsigned)p01 + p10) / 2U);
        *blue = p11;
    } else if (bayer == CAMERA_BAYER_BGGR) {
        *red = p11;
        *green = (uint8_t)(((unsigned)p01 + p10) / 2U);
        *blue = p00;
    } else if (bayer == CAMERA_BAYER_GRBG) {
        *red = p01;
        *green = (uint8_t)(((unsigned)p00 + p11) / 2U);
        *blue = p10;
    } else {
        *red = p10;
        *green = (uint8_t)(((unsigned)p00 + p11) / 2U);
        *blue = p01;
    }
}

static uint8_t visual_scale_sample(uint8_t value, uint32_t gain_q10)
{
    const uint32_t scaled = ((uint32_t)value * gain_q10 + 512U) >> 10;
    return (uint8_t)(scaled > UINT8_MAX ? UINT8_MAX : scaled);
}

bool camera_visual_publish(const uint8_t *frame, size_t length, uint32_t stride)
{
    if (frame == NULL || stride < VISUAL_WIDTH * VISUAL_PIXEL_BYTES ||
            length < ((size_t)(VISUAL_CROP_Y + VISUAL_HEIGHT - 1U) * stride) +
                     ((size_t)(VISUAL_CROP_X + VISUAL_WIDTH) *
                      VISUAL_PIXEL_BYTES)) {
        return false;
    }

    const int target = visual_claim_buffer(NULL);
    if (target < 0) {
        return false;
    }

    uint8_t *destination = s_visual.buffers[target];
    bool swapped;
    portENTER_CRITICAL(&s_visual.mux);
    swapped = s_visual.swapped;
    portEXIT_CRITICAL(&s_visual.mux);
    for (unsigned row = 0; row < VISUAL_HEIGHT; ++row) {
        const uint8_t *source = frame +
            ((size_t)(VISUAL_CROP_Y + row) * stride) +
            (size_t)VISUAL_CROP_X * VISUAL_PIXEL_BYTES;
        uint8_t *output = destination + (size_t)row * VISUAL_STRIDE;
        if (!swapped) {
            memcpy(output, source, VISUAL_STRIDE);
            continue;
        }
        for (unsigned column = 0; column < VISUAL_WIDTH; ++column) {
            output[column * 2U] = source[column * 2U + 1U];
            output[column * 2U + 1U] = source[column * 2U];
        }
    }
    visual_submit_buffer(target);
    return true;
}
static uint8_t visual_clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static uint16_t visual_yuv_to_rgb565(uint8_t y, uint8_t u, uint8_t v)
{
    const int c = (int)y - 16;
    const int d = (int)u - 128;
    const int e = (int)v - 128;
    const uint8_t red = visual_clamp_u8((298 * c + 409 * e + 128) >> 8);
    const uint8_t green =
        visual_clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    const uint8_t blue = visual_clamp_u8((298 * c + 516 * d + 128) >> 8);
    return (uint16_t)(((uint16_t)(red & 0xf8U) << 8) |
                      ((uint16_t)(green & 0xfcU) << 3) |
                      (blue >> 3));
}

bool camera_visual_publish_uyvy(const uint8_t *frame, size_t length,
                                uint32_t stride)
{
    const size_t required =
        (size_t)(VISUAL_CROP_Y + VISUAL_HEIGHT - 1U) * stride +
        (size_t)(VISUAL_CROP_X + VISUAL_WIDTH) * 2U;
    if (frame == NULL || (VISUAL_CROP_X & 1U) != 0 ||
            stride < (VISUAL_CROP_X + VISUAL_WIDTH) * 2U ||
            length < required) {
        return false;
    }

    const int target = visual_claim_buffer(NULL);
    if (target < 0) {
        return false;
    }

    uint8_t *destination = s_visual.buffers[target];
    for (unsigned row = 0; row < VISUAL_HEIGHT; ++row) {
        const uint8_t *source = frame +
            (size_t)(VISUAL_CROP_Y + row) * stride +
            (size_t)VISUAL_CROP_X * 2U;
        uint8_t *output = destination + (size_t)row * VISUAL_STRIDE;
        for (unsigned column = 0; column < VISUAL_WIDTH; column += 2U) {
            const uint8_t u = source[column * 2U];
            const uint8_t y0 = source[column * 2U + 1U];
            const uint8_t v = source[column * 2U + 2U];
            const uint8_t y1 = source[column * 2U + 3U];
            const uint16_t pixel0 = visual_yuv_to_rgb565(y0, u, v);
            const uint16_t pixel1 = visual_yuv_to_rgb565(y1, u, v);
            /* LV_COLOR_FORMAT_RGB565_SWAPPED stores each RGB565 word MSB first. */
            output[column * 2U] = (uint8_t)(pixel0 >> 8);
            output[column * 2U + 1U] = (uint8_t)pixel0;
            output[column * 2U + 2U] = (uint8_t)(pixel1 >> 8);
            output[column * 2U + 3U] = (uint8_t)pixel1;
        }
    }
    visual_submit_buffer(target);
    return true;
}

bool camera_visual_publish_test_pattern(void)
{
    const int target = visual_claim_buffer(NULL);
    if (target < 0) {
        return false;
    }
    static const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    uint8_t *destination = s_visual.buffers[target];
    for (unsigned y = 0; y < VISUAL_HEIGHT; ++y) {
        const unsigned row_half = y >= VISUAL_HEIGHT / 2U ? 1U : 0U;
        for (unsigned x = 0; x < VISUAL_WIDTH; ++x) {
            const unsigned column_half = x >= VISUAL_WIDTH / 2U ? 1U : 0U;
            const uint16_t pixel = colors[row_half * 2U + column_half];
            const size_t offset = (size_t)y * VISUAL_STRIDE + x * 2U;
            destination[offset] = (uint8_t)(pixel >> 8);
            destination[offset + 1U] = (uint8_t)pixel;
        }
    }
    visual_submit_buffer(target);
    return true;
}


bool camera_visual_publish_raw8_lane1(const uint8_t *frame, size_t length,
                                      uint32_t stride)
{
    const size_t required = (size_t)stride * VISUAL_RAW_HEIGHT;
    if (frame == NULL || stride < VISUAL_RAW_WIDTH * 2U ||
            length < required) {
        return false;
    }

    camera_visual_bayer_t bayer;
    const int target = visual_claim_buffer(&bayer);
    if (target < 0) {
        return false;
    }

    uint64_t sums[3] = {0, 0, 0};
    for (unsigned row = 0; row < VISUAL_RAW_HEIGHT - 1U; row += 2) {
        for (unsigned col = 0; col < VISUAL_RAW_WIDTH - 1U; col += 2) {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            visual_raw_cell(frame, stride, col, row, bayer,
                            &red, &green, &blue);
            sums[0] += red;
            sums[1] += green;
            sums[2] += blue;
        }
    }
    const uint64_t target_sum = (sums[0] + sums[1] + sums[2]) / 3U;
    uint32_t gains[3];
    for (unsigned channel = 0; channel < 3; ++channel) {
        gains[channel] = sums[channel] == 0 ? 1024U :
            (uint32_t)((target_sum * 1024U + sums[channel] / 2U) /
                       sums[channel]);
        if (gains[channel] < 512U) {
            gains[channel] = 512U;
        } else if (gains[channel] > 4096U) {
            gains[channel] = 4096U;
        }
    }

    uint8_t *destination = s_visual.buffers[target];
    memset(destination, 0, VISUAL_BUFFER_BYTES);
    for (unsigned row = 0; row < VISUAL_RAW_ACTIVE_HEIGHT; row += 2) {
        const unsigned source_row =
            ((row * (VISUAL_RAW_HEIGHT - 2U)) /
             VISUAL_RAW_ACTIVE_HEIGHT) & ~1U;
        const unsigned output_row = VISUAL_RAW_OFFSET_Y + row;
        for (unsigned col = 0; col < VISUAL_WIDTH; col += 2) {
            const unsigned source_col =
                ((col * (VISUAL_RAW_WIDTH - 2U)) / VISUAL_WIDTH) & ~1U;
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            visual_raw_cell(frame, stride, source_col, source_row, bayer,
                            &red, &green, &blue);
            red = visual_scale_sample(red, gains[0]);
            green = visual_scale_sample(green, gains[1]);
            blue = visual_scale_sample(blue, gains[2]);
            const uint16_t pixel =
                (uint16_t)(((uint16_t)(red >> 3) << 11) |
                           ((uint16_t)(green >> 2) << 5) |
                           (blue >> 3));
            for (unsigned dy = 0; dy < 2; ++dy) {
                const size_t offset =
                    (size_t)(output_row + dy) * VISUAL_STRIDE + col * 2U;
                destination[offset] = (uint8_t)pixel;
                destination[offset + 1U] = (uint8_t)(pixel >> 8);
                destination[offset + 2U] = (uint8_t)pixel;
                destination[offset + 3U] = (uint8_t)(pixel >> 8);
            }
        }
    }
    visual_submit_buffer(target);
    return true;
}

static uint8_t visual_pack_sample(const uint8_t *frame, unsigned mode,
                                  unsigned x, unsigned y)
{
    const size_t index = (size_t)y * VISUAL_SOURCE_WIDTH + x;
    if (mode == 0U) {
        return frame[index];
    }
    if (mode == 1U) {
        return frame[VISUAL_SOURCE_BYTES + index];
    }
    return frame[index * 2U + (mode == 3U ? 1U : 0U)];
}

bool camera_visual_publish_pack_matrix(const uint8_t *frame, size_t length)
{
    if (frame == NULL || length < VISUAL_SOURCE_BYTES * 2U) {
        return false;
    }
    const int target = visual_claim_buffer(NULL);
    if (target < 0) {
        return false;
    }

    uint8_t *destination = s_visual.buffers[target];
    for (unsigned y = 0; y < VISUAL_HEIGHT; ++y) {
        const unsigned quadrant_y = y / VISUAL_MATRIX_HALF;
        const unsigned source_y =
            VISUAL_CROP_Y + (y % VISUAL_MATRIX_HALF) * 2U;
        for (unsigned x = 0; x < VISUAL_WIDTH; ++x) {
            const unsigned quadrant_x = x / VISUAL_MATRIX_HALF;
            const unsigned source_x =
                VISUAL_CROP_X + (x % VISUAL_MATRIX_HALF) * 2U;
            const unsigned mode = quadrant_y * 2U + quadrant_x;
            uint8_t value = visual_pack_sample(frame, mode,
                                               source_x, source_y);
            if (x == VISUAL_MATRIX_HALF - 1U ||
                    y == VISUAL_MATRIX_HALF - 1U) {
                value = UINT8_MAX;
            }
            const uint16_t pixel =
                (uint16_t)(((uint16_t)(value >> 3) << 11) |
                           ((uint16_t)(value >> 2) << 5) |
                           (value >> 3));
            const size_t offset = (size_t)y * VISUAL_STRIDE + x * 2U;
            destination[offset] = (uint8_t)(pixel >> 8);
            destination[offset + 1U] = (uint8_t)pixel;
        }
    }
    visual_submit_buffer(target);
    return true;
}

void camera_visual_set_auto_state(bool passed, unsigned cycles,
                                  unsigned frames)
{
    portENTER_CRITICAL(&s_visual.mux);
    s_visual.auto_passed = passed;
    s_visual.auto_finished = true;
    s_visual.auto_cycles = cycles;
    s_visual.auto_frames = frames;
    portEXIT_CRITICAL(&s_visual.mux);
}

void camera_visual_set_streaming(bool streaming)
{
    portENTER_CRITICAL(&s_visual.mux);
    s_visual.streaming = streaming;
    portEXIT_CRITICAL(&s_visual.mux);
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

bool camera_visual_is_swapped(void)
{
    bool swapped;
    portENTER_CRITICAL(&s_visual.mux);
    swapped = s_visual.swapped;
    portEXIT_CRITICAL(&s_visual.mux);
    return swapped;
}

camera_visual_bayer_t camera_visual_get_bayer(void)
{
    camera_visual_bayer_t bayer;
    portENTER_CRITICAL(&s_visual.mux);
    bayer = s_visual.bayer;
    portEXIT_CRITICAL(&s_visual.mux);
    return bayer;
}

camera_visual_decision_t camera_visual_get_decision(void)
{
    camera_visual_decision_t decision;
    portENTER_CRITICAL(&s_visual.mux);
    decision = s_visual.decision;
    portEXIT_CRITICAL(&s_visual.mux);
    return decision;
}
