/*
 * Candis-S31 watch demo - camera app: live viewfinder + capture.
 *
 * Real viewfinder replacing the EVT1 placeholder (spec E.3-5):
 *  1. bsp_camera_start(NULL) -> open(/dev/video2) inside a dedicated
 *     fetch task (sensor probing stays off the LVGL thread);
 *  2. REQBUFS MMAP buffers (driver-allocated, PSRAM);
 *  3. fetch task at priority 4 with a 3 s DQBUF timeout;
 *  4. zero-copy display: lv_image + static lv_image_dsc_t,
 *     header.stride=1600 (800*2), w/h=460, data=frame base + central
 *     460x460 crop offset, LV_COLOR_FORMAT_RGB565_SWAPPED (verified to
 *     exist in this LVGL and enabled through
 *     CONFIG_LV_DRAW_SW_SUPPORT_RGB565_SWAPPED, so no PSRAM pre-swap
 *     fallback is needed);
 *  5. 10 fps source posted at <=15 fps through a 66 ms LVGL timer
 *     (drop-when-behind frame handoff);
 *  6. capture button saves the current frame as
 *     /sdcard/IMG_<rtc>.rgb565 (raw 800x600 RGB565X, 960000 bytes);
 *  7. screen DELETE -> stop request -> fetch task runs the full
 *     STREAMOFF -> munmap -> close -> bsp_camera_stop chain (the page
 *     does not block on the 3 s DQBUF; a reopen retries until the
 *     previous stream has drained);
 *  8. app_camera_stream_active() exported for C.5 arbitration;
 *  9. in-page "Frame test" button: stops the preview, then queues
 *     camera.frames via svc_test_run() and shows the verdict when it
 *     lands in the result library.
 *
 * All V4L2 ioctls run on the fetch task only; the LVGL side consumes
 * frame buffers through a pending/release slot pair guarded by a
 * critical section.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "esp_video_ioctl.h"

#include "demo_apps.h"
#include "services/svc_storage.h"
#include "tests/svc_test.h"
#include "ui/ui_manager.h"

#define CAM_BUF_COUNT        3
#define CAM_DQBUF_TIMEOUT_MS 3000   /* spec: 3 s DQBUF timeout */
#define CAM_POST_MS          66     /* <=15 fps on-screen posting */
#define CAM_TASK_STACK       6144
#define CAM_TASK_PRIO        4
#define CAM_TIMEOUT_GIVEUP   5      /* consecutive DQBUF timeouts */

/* Central 460x460 window inside the 800x600 RGB565X frame. */
#define CAM_FRAME_W          800
#define CAM_FRAME_H          600
#define CAM_VIEW_W           460
#define CAM_VIEW_H           460
#define CAM_STRIDE           (CAM_FRAME_W * 2)
#define CAM_CROP_X           ((CAM_FRAME_W - CAM_VIEW_W) / 2)   /* 170 px */
#define CAM_CROP_Y           ((CAM_FRAME_H - CAM_VIEW_H) / 2)   /* 70 px */
#define CAM_CROP_OFFSET      (CAM_CROP_Y * CAM_STRIDE + CAM_CROP_X * 2)

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
    volatile bool save_requested;
    volatile bool saving;
    bool img_src_set;
    bool test_pending;        /* "Frame test" waits for the stream to drain */
    uint32_t last_test_seq;
    /* frame handoff (fetch task -> LVGL post timer) */
    portMUX_TYPE mux;
    int pending_idx;          /* DQBUF'd, not yet displayed (-1 none) */
    uint32_t pending_seq;
    int display_idx;          /* referenced by the image widget (-1 none) */
    uint32_t seen_seq;
    int release_idx[CAM_BUF_COUNT]; /* displayed buffers to re-QBUF */
    int release_count;
    /* V4L2 handles (fetch task only) */
    int file;
    uint8_t *buffers[CAM_BUF_COUNT];
    uint32_t buffer_lengths[CAM_BUF_COUNT];
    struct v4l2_format format;
    esp_err_t last_error;
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
    s_cam.state = CAM_STATE_STREAMING;
    lv_label_set_text(s_cam.lbl_status, "");
    lv_obj_remove_flag(s_cam.btn_capture, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_cam.btn_test, LV_OBJ_FLAG_HIDDEN);
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

static void cam_ui_save_done(void *arg)
{
    (void)arg;
    if (s_cam.root == NULL) {
        return;
    }
    ui_toast(s_save_message);
}

/* ------------------------------------------------------------------ */
/* Frame save (fetch task context; holds the buffer while writing)     */
/* ------------------------------------------------------------------ */

static void cam_save_frame(uint32_t index, uint32_t bytesused)
{
    s_cam.save_requested = false;
    s_cam.saving = true;

    esp_err_t error = ESP_OK;
    svc_storage_lease_t lease = {0};
    char path[96];
    error = svc_storage_lease_acquire(&lease);
    if (error == ESP_OK) {
        bsp_rtc_time_t rtc;
        bsp_rtc_status_t rtc_status;
        if (bsp_rtc_get_time(&rtc, &rtc_status) == ESP_OK &&
                rtc_status.time_valid) {
            snprintf(path, sizeof(path),
                     "%s/IMG_%04u%02u%02u_%02u%02u%02u.rgb565",
                     svc_storage_mount_point(),
                     (unsigned)rtc.year, (unsigned)rtc.month,
                     (unsigned)rtc.day, (unsigned)rtc.hour,
                     (unsigned)rtc.minute, (unsigned)rtc.second);
        } else {
            snprintf(path, sizeof(path), "%s/IMG_%llu.rgb565",
                     svc_storage_mount_point(),
                     (unsigned long long)(esp_timer_get_time() / 1000000));
        }
        FILE *file = fopen(path, "wb");
        if (file == NULL) {
            error = ESP_FAIL;
        } else {
            const uint32_t expect =
                s_cam.format.fmt.pix.width * s_cam.format.fmt.pix.height * 2;
            const uint32_t length = bytesused < expect ? bytesused : expect;
            if (fwrite(s_cam.buffers[index], 1, length, file) != length) {
                error = ESP_FAIL;
            }
            if (fclose(file) != 0) {
                error = ESP_FAIL;
            }
        }
        svc_storage_lease_release(&lease);
    }

    if (error == ESP_OK) {
        snprintf(s_save_message, sizeof(s_save_message), "Saved %.70s",
                 path);
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
    if (error == ESP_OK &&
            ioctl(s_cam.file, VIDIOC_G_FMT, &s_cam.format) != 0) {
        /* No driver default: request the sensor format explicitly. */
        s_cam.format.fmt.pix.width = CAM_FRAME_W;
        s_cam.format.fmt.pix.height = CAM_FRAME_H;
        s_cam.format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565X;
        if (ioctl(s_cam.file, VIDIOC_S_FMT, &s_cam.format) != 0) {
            error = ESP_FAIL;
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
        if (s_cam.buffers[index] == MAP_FAILED) {
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
    ui_async(cam_ui_ready, NULL);

    int timeouts = 0;
    bool running = true;
    while (running) {
        if (s_cam.stop_requested) {
            break;
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

        if (s_cam.save_requested && !s_cam.saving) {
            cam_save_frame(done.index, done.bytesused);
        }

        /* Publish as the newest pending frame; an unconsumed previous
         * pending goes straight back to the driver (drop-when-behind). */
        int requeue = -1;
        portENTER_CRITICAL(&s_cam.mux);
        if (s_cam.pending_idx >= 0) {
            requeue = s_cam.pending_idx;
        }
        s_cam.pending_idx = (int)done.index;
        ++s_cam.pending_seq;
        portEXIT_CRITICAL(&s_cam.mux);
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
    if (s_cam.task_running) {
        return;
    }
    s_cam.stop_requested = false;
    s_cam.save_requested = false;
    s_cam.saving = false;
    s_cam.img_src_set = false;
    s_cam.file = -1;
    s_cam.last_error = ESP_OK;
    portENTER_CRITICAL(&s_cam.mux);
    s_cam.pending_idx = -1;
    s_cam.display_idx = -1;
    s_cam.pending_seq = 0;
    s_cam.seen_seq = 0;
    s_cam.release_count = 0;
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

/* 66 ms post timer: consume the newest pending frame (<=15 fps) and, in
 * the stopped state, watch for the queued frame-test result. */
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
        const int old_idx = s_cam.display_idx;
        s_cam.display_idx = new_idx;
        if (old_idx >= 0 && s_cam.release_count < CAM_BUF_COUNT) {
            s_cam.release_idx[s_cam.release_count++] = old_idx;
        }
    }
    portEXIT_CRITICAL(&s_cam.mux);

    if (new_idx >= 0 && s_cam.buffers[new_idx] != NULL) {
        s_img_dsc.data = s_cam.buffers[new_idx] + CAM_CROP_OFFSET;
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

static void cam_capture_cb(lv_event_t *event)
{
    (void)event;
    if (s_cam.state != CAM_STATE_STREAMING) {
        return;
    }
    if (s_cam.saving || s_cam.save_requested) {
        ui_toast("Saving photo...");
        return;
    }
    if (!svc_storage_mounted()) {
        ui_toast("No SD card");
        return;
    }
    s_cam.save_requested = true;
    ui_toast("Saving photo...");
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
    lv_obj_remove_flag(s_cam.img, LV_OBJ_FLAG_CLICKABLE);

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
