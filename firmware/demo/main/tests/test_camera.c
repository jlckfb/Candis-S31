/*
 * Candis-S31 watch demo - CAMERA domain test suite.
 *
 * camera.frames: five-frame capture through the esp_video V4L2 pipeline
 * (/dev/video2), sequence copied from factory_camera.c
 * (command_camera_test): bsp_camera_start -> open -> S_DQBUF_TIMEOUT 3 s
 * -> G/S_FMT -> REQBUFS -> QUERYBUF/mmap/QBUF -> STREAMON -> DQBUF x5
 * with sampled CRC -> STREAMOFF -> munmap -> close -> bsp_camera_stop.
 * Verdict: frame size exact + content non-blank + frame-to-frame CRC
 * change (identical frames are suspect, not conclusive: WARN).
 *
 * Runs on the svc_test task; no LVGL interaction. Resource arbitration
 * (spec C.5): SKIPs while the camera app holds the stream.
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

#include "test_registry.h"

#define CAMERA_CAPTURE_FRAMES   5
#define CAMERA_BUFFER_COUNT     2
#define CAMERA_DQBUF_TIMEOUT_MS 3000

static uint32_t camera_frame_crc(const uint8_t *pixels, uint32_t length,
                                 bool *uniform)
{
    /* Sample one byte every 4 KiB: enough to catch a blank or frozen
     * pipe without adding a full-frame pass to a 10 fps capture. */
    uint32_t crc = 5381;
    const uint8_t first = pixels[0];
    *uniform = true;
    for (uint32_t offset = 0; offset < length; offset += 4096) {
        crc = crc * 33 + pixels[offset];
        if (pixels[offset] != first) {
            *uniform = false;
        }
    }
    return crc;
}

static uint32_t camera_expected_frame_bytes(const struct v4l2_format *format)
{
    /* Enforce an exact byte count only for formats with a known fixed
     * pixel size; anything else is checked by buffer bound and content. */
    if (format->fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565X) {
        return format->fmt.pix.width * format->fmt.pix.height * 2;
    }
    return 0;
}

static void run_camera_frames(const test_ctx_t *ctx, test_result_t *out)
{
    /* C.5 arbitration: the feature app owns the stream while previewing. */
    if (app_camera_stream_active()) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "camera busy (app)", sizeof(out->evidence));
        return;
    }

    ctx->progress(ctx, 0, "Starting camera");

    esp_err_t error = bsp_camera_start(NULL);
    int file = -1;
    if (error == ESP_OK) {
        file = open(BSP_CAMERA_DEVICE, O_RDONLY);
        if (file < 0) {
            error = ESP_ERR_NOT_FOUND;
        }
    }
    /* esp_video_open() lazily reloads the sensor table; restore the board
     * override before configuring or streaming buffers. */
    if (error == ESP_OK) {
        error = bsp_camera_apply_workaround();
    }
    if (error == ESP_OK) {
        /* A dead or unpowered sensor must not wedge the runner: bound
         * the per-frame DQBUF wait. */
        const struct timeval dqbuf_timeout = {
            .tv_sec = CAMERA_DQBUF_TIMEOUT_MS / 1000,
            .tv_usec = (CAMERA_DQBUF_TIMEOUT_MS % 1000) * 1000,
        };
        if (ioctl(file, VIDIOC_S_DQBUF_TIMEOUT, &dqbuf_timeout) != 0) {
            error = ESP_FAIL;
        }
    }

    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format format = {0};
    format.type = type;
    uint8_t *buffers[CAMERA_BUFFER_COUNT] = {NULL};
    uint32_t buffer_lengths[CAMERA_BUFFER_COUNT] = {0};
    bool streaming = false;
    unsigned frames = 0;
    uint32_t frame_bytes = 0;
    bool size_ok = true;
    bool content_ok = true;
    bool change_seen = false;
    bool have_previous = false;
    uint32_t previous_crc = 0;
    bool aborted = false;

    if (error == ESP_OK && ioctl(file, VIDIOC_G_FMT, &format) != 0) {
        /* No driver default: request the sensor format explicitly. */
        memset(&format, 0, sizeof(format));
        format.type = type;
        format.fmt.pix.width = 800;
        format.fmt.pix.height = 600;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565X;
        if (ioctl(file, VIDIOC_S_FMT, &format) != 0) {
            error = ESP_FAIL;
        }
    }

    if (error == ESP_OK) {
        struct v4l2_requestbuffers request = {0};
        request.count = CAMERA_BUFFER_COUNT;
        request.type = type;
        request.memory = V4L2_MEMORY_MMAP;
        if (ioctl(file, VIDIOC_REQBUFS, &request) != 0) {
            error = ESP_FAIL;
        }
    }
    for (int index = 0; error == ESP_OK && index < CAMERA_BUFFER_COUNT;
            ++index) {
        struct v4l2_buffer query = {0};
        query.type = type;
        query.memory = V4L2_MEMORY_MMAP;
        query.index = index;
        if (ioctl(file, VIDIOC_QUERYBUF, &query) != 0) {
            error = ESP_FAIL;
            break;
        }
        buffers[index] = mmap(NULL, query.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, file, query.m.offset);
        if (buffers[index] == NULL || buffers[index] == MAP_FAILED) {
            buffers[index] = NULL;
            error = ESP_FAIL;
            break;
        }
        buffer_lengths[index] = query.length;
        if (ioctl(file, VIDIOC_QBUF, &query) != 0) {
            error = ESP_FAIL;
            break;
        }
    }
    if (error == ESP_OK) {
        if (ioctl(file, VIDIOC_STREAMON, &type) != 0) {
            error = ESP_FAIL;
        } else {
            streaming = true;
        }
    }

    while (error == ESP_OK && frames < CAMERA_CAPTURE_FRAMES) {
        if (ctx->cancel_requested(ctx)) {
            aborted = true;
            break;
        }
        char stage[24];
        snprintf(stage, sizeof(stage), "Frame %u/%d", frames + 1,
                 CAMERA_CAPTURE_FRAMES);
        ctx->progress(ctx, (int)frames * 100 / CAMERA_CAPTURE_FRAMES, stage);

        struct v4l2_buffer done = {0};
        done.type = type;
        done.memory = V4L2_MEMORY_MMAP;
        /* DQBUF waits at most CAMERA_DQBUF_TIMEOUT_MS per frame. */
        if (ioctl(file, VIDIOC_DQBUF, &done) != 0) {
            error = ESP_ERR_TIMEOUT;
            break;
        }
        if ((done.flags & V4L2_BUF_FLAG_DONE) != 0 &&
                done.index < CAMERA_BUFFER_COUNT) {
            const uint32_t expected = camera_expected_frame_bytes(&format);
            if ((expected != 0 && done.bytesused != expected) ||
                    done.bytesused > buffer_lengths[done.index]) {
                size_ok = false;
            } else {
                bool uniform = true;
                const uint32_t crc = camera_frame_crc(buffers[done.index],
                                                      done.bytesused,
                                                      &uniform);
                if (uniform) {
                    content_ok = false;
                }
                if (have_previous && crc != previous_crc) {
                    change_seen = true;
                }
                previous_crc = crc;
                have_previous = true;
                frame_bytes = done.bytesused;
            }
            ++frames;
        }
        if (ioctl(file, VIDIOC_QBUF, &done) != 0) {
            error = ESP_FAIL;
            break;
        }
    }

    if (streaming) {
        ioctl(file, VIDIOC_STREAMOFF, &type);
    }
    for (int index = 0; index < CAMERA_BUFFER_COUNT; ++index) {
        if (buffers[index] != NULL) {
            munmap(buffers[index], buffer_lengths[index]);
        }
    }
    if (file >= 0) {
        close(file);
    }
    const esp_err_t stop_error = bsp_camera_stop();
    if (error == ESP_OK && stop_error != ESP_OK) {
        error = stop_error;
    }

    if (aborted) {
        return; /* runner records SKIP "aborted" */
    }
    if (error != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence), "capture failed %s",
                 esp_err_to_name(error));
        return;
    }

    snprintf(out->evidence, sizeof(out->evidence),
             "%ux%u frames=%u bytes=%" PRIu32 "%s%s",
             (unsigned)format.fmt.pix.width, (unsigned)format.fmt.pix.height,
             frames, frame_bytes,
             size_ok ? "" : " bad_size", content_ok ? "" : " blank");
    const bool pass = frames >= 3 && size_ok && content_ok && change_seen;
    /* A stream that delivers valid but identical frames is suspect, not
     * conclusive: flag it instead of failing outright (factory rule). */
    out->st = pass ? TEST_ST_PASS :
              frames >= 3 && size_ok && content_ok ? TEST_ST_WARN :
              TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */

void test_camera_register(void)
{
    static const test_case_t cases[] = {
        { "camera.frames", "Frame capture x5", TEST_DOM_CAMERA,
          0, 30000, run_camera_frames },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        test_register(&cases[i]);
    }
}
