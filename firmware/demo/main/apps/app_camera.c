/*
 * Candis-S31 watch demo - camera app: live viewfinder + capture.
 *
 * Real viewfinder path for the EVT1 camera diagnostic:
 *  1. bsp_camera_start(NULL) -> open(/dev/video2) inside a dedicated fetch
 *     task (sensor probing stays off the LVGL thread);
 *  2. REQBUFS MMAP buffers (driver-allocated, PSRAM);
 *  3. fetch task at priority 4 with a 3 s DQBUF timeout;
 *  4. zero-copy display: lv_image + static lv_image_dsc_t, central 460x460
 *     crop, and LV_COLOR_FORMAT_RGB565_SWAPPED. The negotiated V4L2
 *     bytesperline is used when present; zero means tightly packed.
 *  5. 30.003 fps sensor target polled at the 60 Hz AMOLED cadence through a
 *     16 ms LVGL timer (drop-when-behind frame handoff);
 *  6. after STREAMON, run the OV5640 embedded single-shot AF protocol;
 *     tapping the preview requests a center-zone refocus;
 *  7. capture button attempts S31 hardware JPEG encoding to
 *     /sdcard/photos/IMG_nnnn.jpg; long-pressing keeps a packed RGB565 debug
 *     frame. The capture path is compile-tested; real-scene color and JPEG
 *     file validation remain hardware test items;
 *  8. screen DELETE -> stop request -> fetch task runs the full STREAMOFF ->
 *     munmap -> close -> bsp_camera_stop chain (the page does not block on
 *     the 3 s DQBUF; a reopen retries until the previous stream has drained);
 *  9. app_camera_stream_active() exported for C.5 arbitration;
 * 10. in-page "Frame test" button stops the preview, queues camera.frames,
 *     and shows the verdict when it lands in the result library.
 *
 * All V4L2 ioctls run on the fetch task only; the LVGL side consumes
 * frame buffers through pending/retire/release queues guarded by a critical
 * section. A retired V4L2 buffer is never re-queued before LV_EVENT_REFR_READY.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "esp_video_ioctl.h"

#include "demo_apps.h"
#include "services/svc_power.h"
#include "services/svc_storage.h"
#include "tests/svc_test.h"
#include "ui/ui_manager.h"

static const char *TAG = "app_camera";

#define CAM_BUF_COUNT 4 /* one LVGL frame plus DVP queueing headroom */
#define CAM_DQBUF_TIMEOUT_MS 3000   /* spec: 3 s DQBUF timeout */
#define CAM_POST_MS 16 /* poll at display cadence; present each 30 fps DVP frame */
#define CAM_TASK_STACK       8192   /* JPEG encoder path needs the depth
                                     * proven by the svc_test runner */
#define CAM_TASK_PRIO        4
#define CAM_TIMEOUT_GIVEUP   5      /* consecutive DQBUF timeouts */
#define CAM_JPEG_QUALITY     80     /* factory jpeg_encode_test default */
#define CAM_AF_TIMEOUT_MS    5000U /* measured full sequence is about 3.7 s */

/* Pending capture kind (s_cam.save_request). Single byte on purpose. */
#define CAM_SAVE_NONE        0
#define CAM_SAVE_JPEG        1
#define CAM_SAVE_RAW         2

/* Central 460x460 window inside the 800x600 RGB565 frame. */
#define CAM_FRAME_W          800
#define CAM_FRAME_H          600
#define CAM_VIEW_W           460
#define CAM_VIEW_H           460
#define CAM_STRIDE           (CAM_FRAME_W * 2)
#define CAM_CROP_X           ((CAM_FRAME_W - CAM_VIEW_W) / 2)   /* 170 px */
#define CAM_CROP_Y           ((CAM_FRAME_H - CAM_VIEW_H) / 2)   /* 70 px */

typedef enum {
    CAM_STATE_IDLE = 0,   /* page open, no stream (stopped or not started) */
    CAM_STATE_STARTING,   /* fetch task bringing the pipeline up */
    CAM_STATE_STREAMING,
    CAM_STATE_STOPPING,   /* stop requested, fetch task draining */
    CAM_STATE_ERROR,
} cam_state_t;

static struct {
    lv_obj_t *root;
    lv_obj_t *img;
    lv_obj_t *lbl_status;
    lv_obj_t *lbl_test;
    lv_obj_t *btn_capture;
    lv_obj_t *btn_test;
    lv_obj_t *btn_start;
    lv_timer_t *post_timer;
    TaskHandle_t task;
    cam_state_t state;
    volatile bool task_running;
    volatile bool stream_active;
    volatile bool stop_requested;
    volatile uint8_t save_request;  /* 0 none, 1 jpeg, 2 raw debug; single
                                     * byte so the dual-core handshake needs
                                     * no ordering between separate flags */
    volatile bool saving;
    volatile bool focus_requested;
    volatile bool focus_running;
    bool img_src_set;
    bool suppress_click;      /* long-press already acted; skip the click */
    bool test_pending;        /* "Frame test" waits for the stream to drain */
    uint32_t last_test_seq;
    /* frame handoff (fetch task -> LVGL post timer) */
    portMUX_TYPE mux;
    int pending_idx;          /* DQBUF'd, not yet displayed (-1 none) */
    uint32_t pending_seq;
    int display_idx;          /* referenced by the image widget (-1 none) */
    uint32_t seen_seq;
    int retire_idx[CAM_BUF_COUNT]; /* wait for LV_EVENT_REFR_READY */
    int retire_count;
    int release_idx[CAM_BUF_COUNT]; /* safe for the fetch task to re-QBUF */
    int release_count;
    uint32_t captured_frames;  /* completed DVP frames in stats window */
    uint32_t presented_frames; /* frames published to the LVGL image */
    uint32_t refreshed_frames; /* LVGL cycles that flushed camera pixels */
    bool flushed_in_cycle;
    bool refresh_hook_attached;
    uint32_t dropped_frames;   /* superseded before LVGL consumed them */
    int64_t stats_start_us;
    int64_t keepawake_deadline_us; /* LVGL thread only */
    /* V4L2 handles (fetch task only) */
    int file;
    uint8_t *buffers[CAM_BUF_COUNT];
    uint32_t buffer_lengths[CAM_BUF_COUNT];
    struct v4l2_format format;
    uint32_t stride_bytes;       /* negotiated bytes per line; 0 means tight */

    esp_err_t last_error;
    esp_err_t focus_error;
} s_cam;

static lv_image_dsc_t s_img_dsc;
static char s_save_message[80]; /* fetch task -> LVGL toast (single slot) */

bool app_camera_stream_active(void)
{
    return s_cam.stream_active;
}

/* ------------------------------------------------------------------ */
/* UI updates posted from the fetch task (LVGL thread)                 */
/* ------------------------------------------------------------------ */

static void cam_ui_ready(void *arg)
{
    (void)arg;
    if (s_cam.root == NULL || s_cam.state != CAM_STATE_STARTING) {
        return;
    }
    portENTER_CRITICAL(&s_cam.mux);
    const esp_err_t focus_error = s_cam.focus_error;
    portEXIT_CRITICAL(&s_cam.mux);
    s_cam.state = CAM_STATE_STREAMING;
    lv_label_set_text(s_cam.lbl_status, "");
    lv_obj_remove_flag(s_cam.btn_capture, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_cam.btn_test, LV_OBJ_FLAG_HIDDEN);
    ui_toast(focus_error == ESP_OK ?
             "Tap preview to refocus" : "Autofocus failed; tap to retry");
}

static void cam_ui_error(void *arg)
{
    (void)arg;
    if (s_cam.root == NULL) {
        return;
    }
    s_cam.state = CAM_STATE_ERROR;
    lv_label_set_text_fmt(s_cam.lbl_status, "Camera start failed: %s",
                          esp_err_to_name(s_cam.last_error));
    lv_obj_add_flag(s_cam.btn_capture, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cam.btn_test, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_cam.btn_start, LV_OBJ_FLAG_HIDDEN);
}

static void cam_ui_focus_done(void *arg)
{
    (void)arg;
    if (s_cam.root == NULL || s_cam.state != CAM_STATE_STREAMING) {
        return;
    }
    portENTER_CRITICAL(&s_cam.mux);
    const esp_err_t focus_error = s_cam.focus_error;
    portEXIT_CRITICAL(&s_cam.mux);
    lv_label_set_text(s_cam.lbl_status, "");
    ui_toast(focus_error == ESP_OK ? "Focus locked" : "Focus failed");
}

static void cam_ui_save_done(void *arg)
{
    (void)arg;
    if (s_cam.root == NULL) {
        return;
    }
    ui_toast(s_save_message);
}

/* ------------------------------------------------------------------ */
/* Photo storage (fetch task context; holds the buffer while writing)  */
/* ------------------------------------------------------------------ */

/* Sequential photo naming under <mount>/photos: take the first free
 * IMG_0001..IMG_9999 slot after the highest existing index. Scanning on
 * every capture (instead of caching the result at boot) also survives
 * card swaps between captures. Must run with the storage lease held. */
static esp_err_t cam_photo_path_next(const char *extension, char *path,
                                     size_t path_size, char *name,
                                     size_t name_size)
{
    char dir[80];
    snprintf(dir, sizeof(dir), "%s/photos", svc_storage_mount_point());
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: errno=%d", dir, errno);
        return ESP_FAIL;
    }

    uint32_t max_index = 0;
    DIR *stream = opendir(dir);
    if (stream == NULL) {
        ESP_LOGE(TAG, "opendir %s failed: errno=%d", dir, errno);
        return ESP_FAIL;
    }
    const struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        unsigned long value = 0;
        if (sscanf(entry->d_name, "IMG_%lu", &value) == 1 &&
                value >= 1 && value <= 9999 && value > max_index) {
            max_index = (uint32_t)value;
        }
    }
    closedir(stream);

    uint32_t next = max_index >= 9999 ? 1 : max_index + 1;
    for (uint32_t guard = 0; guard < 10000; ++guard) {
        snprintf(name, name_size, "IMG_%04" PRIu32 ".%s", next, extension);
        snprintf(path, path_size, "%s/%s", dir, name);
        struct stat info;
        if (stat(path, &info) != 0) {
            return ESP_OK;
        }
        next = next >= 9999 ? 1 : next + 1;
    }
    ESP_LOGE(TAG, "no free IMG_nnnn slot under %s", dir);
    return ESP_FAIL;
}

static esp_err_t cam_file_write(const char *path, const uint8_t *data,
                                uint32_t length)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "fopen %s failed: errno=%d", path, errno);
        return ESP_FAIL;
    }
    esp_err_t error = ESP_OK;
    if (fwrite(data, 1, length, file) != length || fflush(file) != 0 ||
            fsync(fileno(file)) != 0) {
        ESP_LOGE(TAG, "write %s failed: errno=%d", path, errno);
        error = ESP_FAIL;
    }
    if (fclose(file) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }
    if (error != ESP_OK) {
        unlink(path);
    }
    return error;
}

/* Encode one captured frame through the S31 hardware JPEG encoder (the
 * same API set as the proven factory jpeg_encode_test). The camera delivers
 * RGB565 big-endian as V4L2_PIX_FMT_RGB565X; byte swapping is disabled in
 * esp_video for that format. The app performs the explicit BE-to-LE copy
 * required by JPEG_ENCODE_IN_FORMAT_RGB565 while LVGL keeps consuming the
 * original RGB565_SWAPPED buffer. The red/blue order itself still needs the
 * on-board check listed in the README. On success the caller owns
 * *jpeg_data (PSRAM) and must heap_caps_free() it. */

static esp_err_t cam_encode_jpeg(uint32_t index, uint32_t bytesused,
                                 uint8_t **jpeg_data, uint32_t *jpeg_size)
{
    const struct v4l2_pix_format *pix = &s_cam.format.fmt.pix;
    const uint32_t width = pix->width;
    const uint32_t height = pix->height;
    const uint32_t row_bytes = width * sizeof(uint16_t);
    /* V4L2 permits bytesperline == 0 when the format is tightly packed. */
    const uint32_t stride = pix->bytesperline != 0 ?
                            pix->bytesperline : row_bytes;
    const size_t frame_bytes = (size_t)width * height * sizeof(uint16_t);
    const size_t required_bytes = height > 0 ?
                                  (size_t)(height - 1) * stride + row_bytes : 0;
    if (width == 0 || height == 0 || stride < row_bytes ||
            bytesused < frame_bytes || index >= CAM_BUF_COUNT ||
            s_cam.buffers[index] == NULL ||
            s_cam.buffer_lengths[index] < required_bytes) {
        /* Reject incomplete or padded layouts that do not fit the MMAP
         * buffer; a zero bytesperline is valid and means tight packing. */
        ESP_LOGE(TAG, "frame layout mismatch: reported_stride=%u stride=%u "
                 "bytesused=%u required=%u buffer=%u",
                 (unsigned)pix->bytesperline, (unsigned)stride,
                 (unsigned)bytesused, (unsigned)required_bytes,
                 index < CAM_BUF_COUNT ? (unsigned)s_cam.buffer_lengths[index] : 0U);
        return ESP_ERR_INVALID_ARG;
    }
    const size_t buffer_bytes = (frame_bytes + 63U) / 64U * 64U;

    uint8_t *input = heap_caps_aligned_calloc(64, 1, buffer_bytes,
                                              MALLOC_CAP_SPIRAM);
    uint8_t *output = heap_caps_aligned_calloc(64, 1, buffer_bytes,
                                               MALLOC_CAP_SPIRAM);
    if (input == NULL || output == NULL) {
        heap_caps_free(input);
        heap_caps_free(output);
        return ESP_ERR_NO_MEM;
    }

    /* Copy row by row so any V4L2 padding is skipped and the encoder receives
     * native little-endian RGB565 values. */
    const uint8_t *src = s_cam.buffers[index];
    uint16_t *dst = (uint16_t *)input;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = src + (size_t)y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *pixel = row + (size_t)x * sizeof(uint16_t);
            dst[(size_t)y * width + x] =
                ((uint16_t)pixel[0] << 8) | pixel[1];
        }
    }

    jpeg_encoder_handle_t encoder = NULL;
    const jpeg_encode_engine_cfg_t engine_config = {
        .timeout_ms = 500,
    };
    esp_err_t error = jpeg_new_encoder_engine(&engine_config, &encoder);
    if (error == ESP_OK) {
        const jpeg_encode_cfg_t encode_config = {
            .width = width,
            .height = height,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
            .image_quality = CAM_JPEG_QUALITY,
            .pixel_reverse = false,
        };
        const int64_t start_us = esp_timer_get_time();
        error = jpeg_encoder_process(encoder, &encode_config, input,
                                     buffer_bytes, output, buffer_bytes,
                                     jpeg_size);
        const uint32_t elapsed_ms =
            (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "jpeg %ux%u -> %" PRIu32 " bytes in %" PRIu32
                     " ms", (unsigned)width, (unsigned)height, *jpeg_size,
                     elapsed_ms);
        }
        /* Verify the bitstream envelope before it lands on the card so
         * a corrupt file can never be reported as a success. */
        if (error == ESP_OK &&
                (*jpeg_size < 4 || *jpeg_size > buffer_bytes ||
                 output[0] != 0xff || output[1] != 0xd8 ||
                 output[*jpeg_size - 2] != 0xff ||
                 output[*jpeg_size - 1] != 0xd9)) {
            ESP_LOGE(TAG, "jpeg bitstream markers invalid");
            error = ESP_FAIL;
        }
        const esp_err_t delete_result = jpeg_del_encoder_engine(encoder);
        if (error == ESP_OK && delete_result != ESP_OK) {
            error = delete_result;
        }
    }
    heap_caps_free(input);
    if (error == ESP_OK) {
        *jpeg_data = output;
        return ESP_OK;
    }
    heap_caps_free(output);
    return error;
}

static void cam_save_frame(uint8_t request, uint32_t index,
                           uint32_t bytesused)
{
    const bool raw_mode = request == CAM_SAVE_RAW;
    /* s_cam.saving was already claimed under s_cam.mux by camera_task. */

    esp_err_t error = ESP_OK;
    svc_storage_lease_t lease = {0};
    char path[128];
    char name[24];
    uint32_t saved_bytes = 0;

    error = svc_storage_lease_acquire(&lease);
    if (error == ESP_OK) {
        error = cam_photo_path_next(raw_mode ? "rgb565" : "jpg",
                                    path, sizeof(path), name,
                                    sizeof(name));
        if (error == ESP_OK && raw_mode) {
            /* Debug path (long-press): raw 800x600 RGB565 BE frame. Repack
             * padded V4L2 rows so the saved file is always tightly packed. */
            const uint32_t width = s_cam.format.fmt.pix.width;
            const uint32_t height = s_cam.format.fmt.pix.height;
            const uint32_t row_bytes = width * sizeof(uint16_t);
            const uint32_t stride = s_cam.stride_bytes != 0 ?
                                    s_cam.stride_bytes : row_bytes;
            const size_t expect = (size_t)width * height * sizeof(uint16_t);
            const size_t required = height > 0 ?
                                    (size_t)(height - 1) * stride + row_bytes : 0;
            if (index >= CAM_BUF_COUNT || s_cam.buffers[index] == NULL ||
                    stride < row_bytes || bytesused < expect ||
                    s_cam.buffer_lengths[index] < required) {
                error = ESP_ERR_INVALID_ARG;
            } else if (stride == row_bytes) {
                error = cam_file_write(path, s_cam.buffers[index],
                                       (uint32_t)expect);
            } else {
                uint8_t *packed = heap_caps_malloc(expect, MALLOC_CAP_SPIRAM);
                if (packed == NULL) {
                    error = ESP_ERR_NO_MEM;
                } else {
                    for (uint32_t y = 0; y < height; ++y) {
                        memcpy(packed + (size_t)y * row_bytes,
                               s_cam.buffers[index] + (size_t)y * stride,
                               row_bytes);
                    }
                    error = cam_file_write(path, packed, (uint32_t)expect);
                    heap_caps_free(packed);
                }
            }
            if (error == ESP_OK) {
                saved_bytes = (uint32_t)expect;
            }
        } else if (error == ESP_OK) {
            uint8_t *jpeg_data = NULL;
            uint32_t jpeg_size = 0;
            error = cam_encode_jpeg(index, bytesused, &jpeg_data,
                                    &jpeg_size);
            if (error == ESP_OK) {
                error = cam_file_write(path, jpeg_data, jpeg_size);
                saved_bytes = jpeg_size;
                heap_caps_free(jpeg_data);
            }
        }
        svc_storage_lease_release(&lease);
    }

    if (error == ESP_OK) {
        const uint32_t whole_kb = saved_bytes / 1024;
        const uint32_t frac_kb = (saved_bytes % 1024) * 10U / 1024U;
        snprintf(s_save_message, sizeof(s_save_message),
                 "Saved %s (%" PRIu32 ".%" PRIu32 " kB)", name, whole_kb,
                 frac_kb);
    } else {
        snprintf(s_save_message, sizeof(s_save_message),
                 "Save failed: %.60s", esp_err_to_name(error));
    }
    s_cam.saving = false;
    ui_async(cam_ui_save_done, NULL);
}

/* ------------------------------------------------------------------ */
/* Fetch task: owns every V4L2 ioctl                                   */
/* ------------------------------------------------------------------ */

static void cam_qbuf_index(int index)
{
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    ioctl(s_cam.file, VIDIOC_QBUF, &buf);
}

static esp_err_t cam_open_pipeline(void)
{
    esp_err_t error = bsp_camera_start(NULL);
    if (error == ESP_OK) {
        s_cam.file = open(BSP_CAMERA_DEVICE, O_RDONLY);
        if (s_cam.file < 0) {
            error = ESP_ERR_NOT_FOUND;
        }
    }
    if (error == ESP_OK) {
        const struct timeval dqbuf_timeout = {
            .tv_sec = CAM_DQBUF_TIMEOUT_MS / 1000,
            .tv_usec = (CAM_DQBUF_TIMEOUT_MS % 1000) * 1000,
        };
        if (ioctl(s_cam.file, VIDIOC_S_DQBUF_TIMEOUT, &dqbuf_timeout) != 0) {
            error = ESP_FAIL;
        }
    }

    memset(&s_cam.format, 0, sizeof(s_cam.format));
    s_cam.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (error == ESP_OK) {
        /* Best-effort probe of the driver default; the explicit S_FMT
         * below is what actually selects the pixel format. */
        ioctl(s_cam.file, VIDIOC_G_FMT, &s_cam.format);
    }

    if (error == ESP_OK) {
        /* Always request the EVT1 sensor format explicitly instead of
         * trusting a driver default: a YUV422 stream decoded as RGB565
         * renders as scrambled colors. Mirrors factory_camera.c. */
        memset(&s_cam.format, 0, sizeof(s_cam.format));
        s_cam.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        s_cam.format.fmt.pix.width = CAM_FRAME_W;
        s_cam.format.fmt.pix.height = CAM_FRAME_H;
        s_cam.format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565X;
        if (ioctl(s_cam.file, VIDIOC_S_FMT, &s_cam.format) != 0) {
            error = ESP_FAIL;
        }
    }

    if (error == ESP_OK) {
        /* S_FMT reloads the upstream table; now force the requested board
         * byte order and ISP block before allocating capture buffers. */
        error = bsp_camera_apply_workaround(V4L2_PIX_FMT_RGB565X);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "post-format sensor workaround failed: %s",
                     esp_err_to_name(error));
        }
    }

    if (error == ESP_OK) {
        /* Read back the negotiated format: a mismatch must surface as
         * an error, never as silently wrong colors. */
        memset(&s_cam.format, 0, sizeof(s_cam.format));
        s_cam.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s_cam.file, VIDIOC_G_FMT, &s_cam.format) != 0) {
            error = ESP_FAIL;
        }
    }

    if (error == ESP_OK) {
        const struct v4l2_pix_format *pix = &s_cam.format.fmt.pix;
        const uint32_t fourcc = pix->pixelformat;
        ESP_LOGI(TAG, "V4L2 fmt %c%c%c%c %ux%u bytesperline=%u",
                 (char)(fourcc & 0xFF), (char)((fourcc >> 8) & 0xFF),
                 (char)((fourcc >> 16) & 0xFF), (char)((fourcc >> 24) & 0xFF),
                 (unsigned)pix->width, (unsigned)pix->height,
                 (unsigned)pix->bytesperline);
        if (pix->pixelformat != V4L2_PIX_FMT_RGB565X ||
                pix->width != CAM_FRAME_W || pix->height != CAM_FRAME_H ||
                (pix->bytesperline != 0 && pix->bytesperline < CAM_STRIDE)) {
            ESP_LOGE(TAG, "format mismatch: stride=%u effective=%u expected=%u",
                     (unsigned)pix->bytesperline,
                     (unsigned)(pix->bytesperline != 0 ? pix->bytesperline : CAM_STRIDE),
                     (unsigned)CAM_STRIDE);
            error = ESP_FAIL;
        } else {
            s_cam.stride_bytes = pix->bytesperline != 0 ?
                                 pix->bytesperline : CAM_STRIDE;
        }

    }
    if (error == ESP_OK) {
        struct v4l2_requestbuffers request = {0};
        request.count = CAM_BUF_COUNT;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_cam.file, VIDIOC_REQBUFS, &request) != 0) {
            error = ESP_FAIL;
        }
    }
    for (int index = 0; error == ESP_OK && index < CAM_BUF_COUNT; ++index) {
        struct v4l2_buffer query = {0};
        query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        query.memory = V4L2_MEMORY_MMAP;
        query.index = index;
        if (ioctl(s_cam.file, VIDIOC_QUERYBUF, &query) != 0) {
            error = ESP_FAIL;
            break;
        }
        s_cam.buffers[index] = mmap(NULL, query.length,
                                    PROT_READ | PROT_WRITE, MAP_SHARED,
                                    s_cam.file, query.m.offset);
        if (s_cam.buffers[index] == NULL ||
                s_cam.buffers[index] == MAP_FAILED) {
            s_cam.buffers[index] = NULL;
            error = ESP_FAIL;
            break;
        }
        s_cam.buffer_lengths[index] = query.length;
        if (ioctl(s_cam.file, VIDIOC_QBUF, &query) != 0) {
            error = ESP_FAIL;
            break;
        }
    }
    if (error == ESP_OK) {
        const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s_cam.file, VIDIOC_STREAMON, &type) != 0) {
            error = ESP_FAIL;
        }
    }
    return error;
}

static void cam_close_pipeline(void)
{
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (s_cam.file >= 0) {
        ioctl(s_cam.file, VIDIOC_STREAMOFF, &type);
    }
    for (int index = 0; index < CAM_BUF_COUNT; ++index) {
        if (s_cam.buffers[index] != NULL) {
            munmap(s_cam.buffers[index], s_cam.buffer_lengths[index]);
            s_cam.buffers[index] = NULL;
        }
    }
    if (s_cam.file >= 0) {
        close(s_cam.file);
        s_cam.file = -1;
    }
    bsp_camera_stop();
}

static void cam_display_refresh_cb(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
    case LV_EVENT_FLUSH_START:
        s_cam.flushed_in_cycle = true;
        break;
    case LV_EVENT_REFR_READY:
        if (s_cam.flushed_in_cycle) {
            s_cam.flushed_in_cycle = false;
            portENTER_CRITICAL(&s_cam.mux);
            ++s_cam.refreshed_frames;
            while (s_cam.retire_count > 0 &&
                    s_cam.release_count < CAM_BUF_COUNT) {
                s_cam.release_idx[s_cam.release_count++] =
                    s_cam.retire_idx[--s_cam.retire_count];
            }
            portEXIT_CRITICAL(&s_cam.mux);
        }
        break;
    default:
        break;
    }
}

static void camera_task(void *arg)
{
    (void)arg;
    const esp_err_t error = cam_open_pipeline();
    if (error != ESP_OK) {
        s_cam.last_error = error;
        cam_close_pipeline();
        ui_async(cam_ui_error, NULL);
        s_cam.task_running = false;
        s_cam.stream_active = false;
        vTaskDelete(NULL);
        return;
    }
    s_cam.stream_active = true;
    const esp_err_t initial_focus_error =
        bsp_camera_autofocus_once(CAM_AF_TIMEOUT_MS);
    portENTER_CRITICAL(&s_cam.mux);
    s_cam.focus_error = initial_focus_error;
    portEXIT_CRITICAL(&s_cam.mux);
    if (initial_focus_error != ESP_OK) {
        ESP_LOGW(TAG, "initial autofocus failed: %s",
                 esp_err_to_name(initial_focus_error));
    }
    ui_async(cam_ui_ready, NULL);
    s_cam.stats_start_us = esp_timer_get_time();

    int timeouts = 0;
    bool running = true;
    while (running) {
        if (s_cam.stop_requested) {
            break;
        }
        bool run_focus = false;
        portENTER_CRITICAL(&s_cam.mux);
        if (s_cam.focus_requested) {
            s_cam.focus_requested = false;
            s_cam.focus_running = true;
            run_focus = true;
        }
        portEXIT_CRITICAL(&s_cam.mux);
        if (run_focus) {
            const esp_err_t focus_error =
                bsp_camera_autofocus_once(CAM_AF_TIMEOUT_MS);
            portENTER_CRITICAL(&s_cam.mux);
            s_cam.focus_error = focus_error;
            s_cam.focus_running = false;
            portEXIT_CRITICAL(&s_cam.mux);
            if (focus_error != ESP_OK) {
                ESP_LOGW(TAG, "tap autofocus failed: %s",
                         esp_err_to_name(focus_error));
            }
            ui_async(cam_ui_focus_done, NULL);
        }
        /* Re-queue buffers the LVGL side has finished displaying. */
        portENTER_CRITICAL(&s_cam.mux);
        const int release_count = s_cam.release_count;
        portEXIT_CRITICAL(&s_cam.mux);
        for (int i = 0; i < release_count; ++i) {
            portENTER_CRITICAL(&s_cam.mux);
            const int index = s_cam.release_idx[--s_cam.release_count];
            portEXIT_CRITICAL(&s_cam.mux);
            cam_qbuf_index(index);
        }

        struct v4l2_buffer done = {0};
        done.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        done.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_cam.file, VIDIOC_DQBUF, &done) != 0) {
            /* 3 s timeout: recheck the stop flag; a dead sensor gives up
             * after CAM_TIMEOUT_GIVEUP consecutive timeouts. */
            if (++timeouts >= CAM_TIMEOUT_GIVEUP) {
                s_cam.last_error = ESP_ERR_TIMEOUT;
                ui_async(cam_ui_error, NULL);
                break;
            }
            continue;
        }
        timeouts = 0;
        if ((done.flags & V4L2_BUF_FLAG_DONE) == 0 ||
                done.index >= CAM_BUF_COUNT) {
            cam_qbuf_index(done.index < CAM_BUF_COUNT ? done.index : 0);
            continue;
        }

        portENTER_CRITICAL(&s_cam.mux);
        const uint8_t request = s_cam.save_request;
        s_cam.save_request = CAM_SAVE_NONE;
        if (request != CAM_SAVE_NONE) {
            /* Claim busy in the same critical section: the UI can never
             * observe saving=false with a request in flight. */
            s_cam.saving = true;
        }
        portEXIT_CRITICAL(&s_cam.mux);
        if (request != CAM_SAVE_NONE) {
            cam_save_frame(request, done.index, done.bytesused);
        }

        /* Publish as the newest pending frame; an unconsumed previous
         * pending goes straight back to the driver (drop-when-behind). */
        const int64_t stats_now_us = esp_timer_get_time();
        uint32_t stats_captured = 0;
        uint32_t stats_presented = 0;
        uint32_t stats_dropped = 0;
        uint32_t stats_refreshed = 0;
        int64_t stats_elapsed_us = 0;
        bool report_stats = false;
        int requeue = -1;
        portENTER_CRITICAL(&s_cam.mux);
        ++s_cam.captured_frames;
        if (s_cam.pending_idx >= 0) {
            requeue = s_cam.pending_idx;
            ++s_cam.dropped_frames;
        }
        s_cam.pending_idx = (int)done.index;
        ++s_cam.pending_seq;
        stats_elapsed_us = stats_now_us - s_cam.stats_start_us;
        if (stats_elapsed_us >= 2000000) {
            stats_captured = s_cam.captured_frames;
            stats_presented = s_cam.presented_frames;
            stats_dropped = s_cam.dropped_frames;
            stats_refreshed = s_cam.refreshed_frames;
            s_cam.captured_frames = 0;
            s_cam.presented_frames = 0;
            s_cam.dropped_frames = 0;
            s_cam.refreshed_frames = 0;
            s_cam.stats_start_us = stats_now_us;
            report_stats = true;
        }
        portEXIT_CRITICAL(&s_cam.mux);
        if (report_stats) {
            const uint32_t capture_fps_x10 = (uint32_t)(
                ((uint64_t)stats_captured * 10000000ULL +
                 (uint64_t)stats_elapsed_us / 2U) /
                (uint64_t)stats_elapsed_us);
            const uint32_t present_fps_x10 = (uint32_t)(
                ((uint64_t)stats_presented * 10000000ULL +
                 (uint64_t)stats_elapsed_us / 2U) /
                (uint64_t)stats_elapsed_us);
            const uint32_t refresh_fps_x10 = (uint32_t)(
                ((uint64_t)stats_refreshed * 10000000ULL +
                 (uint64_t)stats_elapsed_us / 2U) /
                (uint64_t)stats_elapsed_us);
            ESP_LOGI(TAG,
                     "preview capture=%" PRIu32 ".%01" PRIu32
                     " fps present=%" PRIu32 ".%01" PRIu32
                     " fps refresh=%" PRIu32 ".%01" PRIu32
                     " fps dropped=%" PRIu32,
                     capture_fps_x10 / 10U, capture_fps_x10 % 10U,
                     present_fps_x10 / 10U, present_fps_x10 % 10U,
                     refresh_fps_x10 / 10U, refresh_fps_x10 % 10U,
                     stats_dropped);
        }
        if (requeue >= 0) {
            cam_qbuf_index(requeue);
        }
    }

    /* Teardown chain (spec point 7): STREAMOFF -> munmap -> close ->
     * bsp_camera_stop, always on this task. */
    cam_close_pipeline();
    portENTER_CRITICAL(&s_cam.mux);
    s_cam.pending_idx = -1;
    s_cam.display_idx = -1;
    s_cam.retire_count = 0;
    s_cam.release_count = 0;
    portEXIT_CRITICAL(&s_cam.mux);
    s_cam.stream_active = false;
    s_cam.task_running = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Stream start/stop helpers (LVGL thread)                             */
/* ------------------------------------------------------------------ */

static void cam_start_stream(void)
{
    s_cam.save_request = CAM_SAVE_NONE;
    s_cam.saving = false;
    s_cam.stop_requested = false;
    s_cam.suppress_click = false;
    s_cam.img_src_set = false;
    s_cam.file = -1;
    s_cam.last_error = ESP_OK;
    s_cam.stride_bytes = 0;

    portENTER_CRITICAL(&s_cam.mux);
    s_cam.pending_idx = -1;
    s_cam.display_idx = -1;
    s_cam.pending_seq = 0;
    s_cam.seen_seq = 0;
    s_cam.release_count = 0;
    s_cam.retire_count = 0;
    s_cam.captured_frames = 0;
    s_cam.presented_frames = 0;
    s_cam.dropped_frames = 0;
    s_cam.refreshed_frames = 0;
    s_cam.flushed_in_cycle = false;
    s_cam.stats_start_us = 0;
    s_cam.keepawake_deadline_us = 0;
    s_cam.focus_requested = false;
    s_cam.focus_running = false;
    s_cam.focus_error = ESP_OK;
    portEXIT_CRITICAL(&s_cam.mux);
    memset(s_cam.buffers, 0, sizeof(s_cam.buffers));

    s_cam.state = CAM_STATE_STARTING;
    lv_label_set_text(s_cam.lbl_status, "Starting camera...");
    lv_obj_add_flag(s_cam.btn_capture, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cam.btn_test, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cam.btn_start, LV_OBJ_FLAG_HIDDEN);

    s_cam.task_running = true;
    if (xTaskCreate(camera_task, "app_camera", CAM_TASK_STACK, NULL,
                    CAM_TASK_PRIO, &s_cam.task) != pdPASS) {
        s_cam.task_running = false;
        s_cam.state = CAM_STATE_ERROR;
        lv_label_set_text(s_cam.lbl_status, "Camera task create failed");
        lv_obj_remove_flag(s_cam.btn_start, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cam_request_stop(void)
{
    if (!s_cam.task_running) {
        return;
    }
    s_cam.stop_requested = true;
    if (s_cam.state == CAM_STATE_STREAMING ||
            s_cam.state == CAM_STATE_STARTING) {
        s_cam.state = CAM_STATE_STOPPING;
    }
}

/* ------------------------------------------------------------------ */
/* LVGL-side timers                                                    */
/* ------------------------------------------------------------------ */

/* 16 ms post timer: consume every new frame at the AMOLED refresh cadence
 * and, in the stopped state, watch for the queued frame-test result. */
static void cam_post_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_cam.root == NULL) {
        return;
    }

    /* "Frame test" second phase: the stream has drained, queue the test. */
    if (s_cam.test_pending && !s_cam.task_running) {
        s_cam.test_pending = false;
        const esp_err_t error = svc_test_run("camera.frames");
        if (error == ESP_OK) {
            lv_label_set_text(s_cam.lbl_status,
                              "Preview stopped, frame test queued");
        } else {
            lv_label_set_text(s_cam.lbl_status,
                              "Frame test queue failed");
        }
        lv_obj_remove_flag(s_cam.btn_start, LV_OBJ_FLAG_HIDDEN);
    }

    /* Frame-test verdict watcher (stopped state only). */
    if (s_cam.state == CAM_STATE_IDLE || s_cam.state == CAM_STATE_STOPPING) {
        test_result_t result;
        test_result_get("camera.frames", &result);
        if (result.run_seq != s_cam.last_test_seq) {
            s_cam.last_test_seq = result.run_seq;
            static const char *const names[] = {
                "NOT RUN", "PASS", "FAIL", "SKIP", "WARN",
            };
            lv_label_set_text_fmt(s_cam.lbl_test, "frames test: %s %s",
                                  names[result.st], result.evidence);
        }
    }

    if (s_cam.state != CAM_STATE_STREAMING) {
        return;
    }

    /* Consume the newest pending frame. */
    int new_idx = -1;
    portENTER_CRITICAL(&s_cam.mux);
    if (s_cam.pending_seq != s_cam.seen_seq && s_cam.pending_idx >= 0) {
        new_idx = s_cam.pending_idx;
        s_cam.pending_idx = -1;
        s_cam.seen_seq = s_cam.pending_seq;
        ++s_cam.presented_frames;
        const int old_idx = s_cam.display_idx;
        s_cam.display_idx = new_idx;
        if (old_idx >= 0) {
            assert(s_cam.retire_count < CAM_BUF_COUNT);
            s_cam.retire_idx[s_cam.retire_count++] = old_idx;
        }
    }
    portEXIT_CRITICAL(&s_cam.mux);

    if (new_idx >= 0 && s_cam.buffers[new_idx] != NULL) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= s_cam.keepawake_deadline_us) {
            svc_power_activity();
            s_cam.keepawake_deadline_us = now_us + 2000000;
        }
        const uint32_t stride = s_cam.stride_bytes != 0 ?
                                s_cam.stride_bytes : CAM_STRIDE;
        s_img_dsc.header.stride = stride;
        s_img_dsc.data_size = CAM_VIEW_H * stride;
        s_img_dsc.data = s_cam.buffers[new_idx] +
                         (size_t)CAM_CROP_Y * stride + CAM_CROP_X * 2;
        if (!s_cam.img_src_set) {
            s_cam.img_src_set = true;
            lv_image_set_src(s_cam.img, &s_img_dsc);
        } else {
            lv_obj_invalidate(s_cam.img);
        }
    }
}

/* Retry while a previous stream is still draining (page reopened quickly
 * after exit): wait for task_running to clear, then start fresh. */
static void cam_retry_start_cb(lv_timer_t *timer)
{
    if (s_cam.root == NULL) {
        lv_timer_delete(timer);
        return;
    }
    if (!s_cam.task_running) {
        lv_timer_delete(timer);
        cam_start_stream();
    }
}

/* ------------------------------------------------------------------ */
/* Button callbacks                                                    */
/* ------------------------------------------------------------------ */

static void cam_back_cb(lv_event_t *event)
{
    (void)event;
    ui_nav_back();
}

static void cam_focus_cb(lv_event_t *event)
{
    (void)event;
    if (s_cam.state != CAM_STATE_STREAMING) {
        return;
    }

    bool queued = false;
    portENTER_CRITICAL(&s_cam.mux);
    if (!s_cam.focus_requested && !s_cam.focus_running) {
        s_cam.focus_requested = true;
        queued = true;
    }
    portEXIT_CRITICAL(&s_cam.mux);
    if (queued) {
        lv_label_set_text(s_cam.lbl_status, "Focusing...");
    }
}

static void cam_capture_cb(lv_event_t *event)
{
    (void)event;
    if (s_cam.suppress_click) {
        /* The long-press handler already acted on this press. */
        s_cam.suppress_click = false;
        return;
    }
    if (s_cam.state != CAM_STATE_STREAMING) {
        return;
    }
    portENTER_CRITICAL(&s_cam.mux);
    const bool busy = s_cam.saving || s_cam.save_request != 0;
    portEXIT_CRITICAL(&s_cam.mux);
    if (busy) {
        ui_toast("Saving photo...");
        return;
    }
    if (!svc_storage_mounted()) {
        ui_toast("No SD card");
        return;
    }
    portENTER_CRITICAL(&s_cam.mux);
    s_cam.save_request = CAM_SAVE_JPEG;
    portEXIT_CRITICAL(&s_cam.mux);
    ui_toast("Saving photo...");
}

/* Long-press keeps the raw RGB565 store reachable as a debug aid (the
 * default capture is JPEG; raw frames need special tools on a PC). */
static void cam_capture_long_cb(lv_event_t *event)
{
    (void)event;
    if (s_cam.state != CAM_STATE_STREAMING) {
        return;
    }
    portENTER_CRITICAL(&s_cam.mux);
    const bool busy = s_cam.saving || s_cam.save_request != 0;
    portEXIT_CRITICAL(&s_cam.mux);
    if (busy) {
        return;
    }
    if (!svc_storage_mounted()) {
        ui_toast("No SD card");
        return;
    }
    s_cam.suppress_click = true;
    portENTER_CRITICAL(&s_cam.mux);
    s_cam.save_request = CAM_SAVE_RAW;
    portEXIT_CRITICAL(&s_cam.mux);
    ui_toast("Saving RAW frame...");
}

/* A fresh press must never be swallowed by a stale long-press flag. */
static void cam_capture_press_cb(lv_event_t *event)
{
    (void)event;
    s_cam.suppress_click = false;
}

static void cam_test_cb(lv_event_t *event)
{
    (void)event;
    if (s_cam.state != CAM_STATE_STREAMING) {
        return;
    }
    /* The frames test owns the camera while it runs: stop the preview
     * first, queue the test once the stream has drained (post timer). */
    lv_obj_add_flag(s_cam.btn_capture, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cam.btn_test, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_cam.lbl_status, "Stopping preview...");
    s_cam.test_pending = true;
    cam_request_stop();
    s_cam.state = CAM_STATE_IDLE;
}


static void cam_start_cb(lv_event_t *event)
{
    (void)event;
    if (s_cam.task_running) {
        ui_toast("Camera busy");
        return;
    }
    lv_label_set_text(s_cam.lbl_test, "");
    cam_start_stream();
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void cam_root_delete_cb(lv_event_t *event)
{
    (void)event;
    /* Detach the image widget from camera memory without touching LVGL
     * state further: the static dsc simply stops being repointed. The
     * object tree is being deleted anyway. */
    s_img_dsc.data = NULL;
    if (s_cam.refresh_hook_attached) {
        lv_display_remove_event_cb_with_user_data(
            lv_display_get_default(), cam_display_refresh_cb, NULL);
        s_cam.refresh_hook_attached = false;
    }

    if (s_cam.post_timer != NULL) {
        lv_timer_delete(s_cam.post_timer);
        s_cam.post_timer = NULL;
    }
    /* Spec point 7 teardown: the fetch task notices stop_requested
     * within its 3 s DQBUF window and runs STREAMOFF -> munmap ->
     * close -> bsp_camera_stop itself. The page deliberately does not
     * block the UI on that; a fast reopen retries (cam_retry_start_cb). */
    cam_request_stop();
    s_cam.root = NULL;
    s_cam.img = NULL;
    s_cam.lbl_status = NULL;
    s_cam.lbl_test = NULL;
    s_cam.btn_capture = NULL;
    s_cam.btn_test = NULL;
    s_cam.btn_start = NULL;
    s_cam.state = CAM_STATE_IDLE;
    s_cam.img_src_set = false;
}

static lv_obj_t *cam_overlay_button(lv_obj_t *parent, const char *text,
                                    lv_event_cb_t cb, int x, int y, int w)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, w, UI_TOUCH_MIN);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COL_ACCENT_DIM), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_body(), 0);
    lv_obj_center(label);
    return button;
}

lv_obj_t *app_camera_create(void)
{
    portMUX_INITIALIZE(&s_cam.mux);

    lv_obj_t *root = lv_obj_create(NULL);
    s_cam.root = root;
    lv_obj_set_size(root, 460, 460);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, cam_root_delete_cb, LV_EVENT_DELETE, NULL);
    lv_display_t *display = lv_display_get_default();
    if (display != NULL) {
        lv_display_add_event_cb(display, cam_display_refresh_cb,
                                LV_EVENT_FLUSH_START, NULL);
        lv_display_add_event_cb(display, cam_display_refresh_cb,
                                LV_EVENT_REFR_READY, NULL);
        s_cam.refresh_hook_attached = true;
    }

    /* Zero-copy viewfinder surface (src set on the first frame). */
    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
    s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    s_img_dsc.header.w = CAM_VIEW_W;
    s_img_dsc.header.h = CAM_VIEW_H;
    s_img_dsc.header.stride = CAM_STRIDE;
    s_img_dsc.data_size = CAM_VIEW_H * CAM_STRIDE;
    s_cam.img = lv_image_create(root);
    lv_obj_set_pos(s_cam.img, 0, 0);
    lv_obj_add_flag(s_cam.img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cam.img, cam_focus_cb, LV_EVENT_CLICKED, NULL);

    s_cam.lbl_status = lv_label_create(root);
    lv_obj_set_style_text_font(s_cam.lbl_status, ui_font_body_lg(), 0);
    lv_obj_set_style_text_color(s_cam.lbl_status,
                                lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_style_bg_color(s_cam.lbl_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cam.lbl_status, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_cam.lbl_status, 12, 0);
    lv_obj_set_style_pad_hor(s_cam.lbl_status, 12, 0);
    lv_obj_set_style_pad_ver(s_cam.lbl_status, 8, 0);
    lv_obj_align(s_cam.lbl_status, LV_ALIGN_CENTER, 0, -40);

    /* Back key (scaffold style, overlays the preview). */
    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, UI_TOUCH_MIN, UI_TOUCH_MIN);
    lv_obj_set_pos(back, 8, 36);
    lv_obj_set_style_bg_color(back, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(UI_COL_HAIRLINE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, cam_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, ui_font_mid(), 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(back_lbl);

    /* Bottom controls: capture + frame test (hidden until streaming),
     * plus a restart button shown when the preview is stopped. */
    s_cam.btn_capture = cam_overlay_button(root, "Capture",
                                           cam_capture_cb, 46, 388, 170);
    lv_obj_add_event_cb(s_cam.btn_capture, cam_capture_press_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_cam.btn_capture, cam_capture_long_cb,
                        LV_EVENT_LONG_PRESSED, NULL);
    s_cam.btn_test = cam_overlay_button(root, "Frame test",
                                        cam_test_cb, 244, 388, 170);
    s_cam.btn_start = cam_overlay_button(root, "Start preview",
                                         cam_start_cb, 130, 388, 200);
    lv_obj_add_flag(s_cam.btn_capture, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cam.btn_test, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cam.btn_start, LV_OBJ_FLAG_HIDDEN);

    s_cam.lbl_test = lv_label_create(root);
    lv_obj_set_style_text_font(s_cam.lbl_test, ui_font_text(), 0);
    lv_obj_set_style_text_color(s_cam.lbl_test,
                                lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_set_style_bg_color(s_cam.lbl_test, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cam.lbl_test, LV_OPA_50, 0);
    lv_obj_set_width(s_cam.lbl_test, 420);
    lv_label_set_long_mode(s_cam.lbl_test, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_cam.lbl_test, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cam.lbl_test, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_label_set_text(s_cam.lbl_test, "");

    s_cam.post_timer = lv_timer_create(cam_post_timer_cb, CAM_POST_MS, NULL);

    /* C.5 arbitration: a camera-domain test owns the hardware. */
    if (svc_test_domain_busy(TEST_DOM_CAMERA)) {
        s_cam.state = CAM_STATE_ERROR;
        lv_label_set_text(s_cam.lbl_status, "Camera busy: test running");
        return root;
    }

    if (s_cam.task_running) {
        /* Previous stream still draining (fast reopen): retry shortly. */
        s_cam.state = CAM_STATE_STARTING;
        lv_label_set_text(s_cam.lbl_status, "Camera restarting...");
        lv_timer_create(cam_retry_start_cb, 500, NULL);
    } else {
        cam_start_stream();
    }
    return root;
}
