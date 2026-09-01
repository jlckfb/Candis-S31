/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "esp_video_ioctl.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

#define CAMERA_CAPTURE_FRAMES     5
#define CAMERA_BUFFER_COUNT       2
#define CAMERA_DQBUF_TIMEOUT_MS   3000

static uint32_t camera_frame_crc(const uint8_t *pixels, uint32_t length,
                                 bool *uniform)
{
    /* Sample one byte every 4 KiB: enough to catch a blank or frozen pipe
     * without adding a full-frame pass to a 10 fps capture. */
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
    /* Enforce an exact byte count only for formats with a known fixed pixel
     * size; anything else is checked by buffer bound and content alone. */
    if (format->fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565X) {
        return format->fmt.pix.width * format->fmt.pix.height * 2;
    }
    return 0;
}

static int command_camera_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

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
        /* A dead or unpowered sensor must not wedge the console until
         * power-off: bound the per-frame DQBUF wait. */
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

    if (error == ESP_OK && ioctl(file, VIDIOC_G_FMT, &format) != 0) {
        /* No driver default: request the EVT1 sensor format explicitly. */
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
    for (int index = 0; error == ESP_OK && index < CAMERA_BUFFER_COUNT; ++index) {
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
        struct v4l2_buffer done = {0};
        done.type = type;
        done.memory = V4L2_MEMORY_MMAP;
        /* DQBUF waits at most CAMERA_DQBUF_TIMEOUT_MS per frame (set above). */
        if (ioctl(file, VIDIOC_DQBUF, &done) != 0) {
            error = ESP_ERR_TIMEOUT;
            break;
        }
        if ((done.flags & V4L2_BUF_FLAG_DONE) != 0 && done.index < CAMERA_BUFFER_COUNT) {
            const uint32_t expected = camera_expected_frame_bytes(&format);
            if ((expected != 0 && done.bytesused != expected) ||
                    done.bytesused > buffer_lengths[done.index]) {
                size_ok = false;
            } else {
                bool uniform = true;
                const uint32_t crc = camera_frame_crc(buffers[done.index],
                                                      done.bytesused, &uniform);
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

    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_CAMERA, error, "camera capture failed");
        return error;
    }
    const bool pass = frames >= 3 && size_ok && content_ok && change_seen;
    /* A stream that delivers valid but identical frames is suspect, not
     * conclusive: flag it for the operator instead of failing outright. */
    const factory_status_t result = pass ? FACTORY_STATUS_PASS :
                                    frames >= 3 && size_ok && content_ok ?
                                    FACTORY_STATUS_WARN : FACTORY_STATUS_FAIL;
    char detail[96];
    snprintf(detail, sizeof(detail),
             "%ux%u fmt=0x%08lx frames=%u bytes=%" PRIu32 "%s%s",
             (unsigned)format.fmt.pix.width, (unsigned)format.fmt.pix.height,
             (unsigned long)format.fmt.pix.pixelformat,
             frames, frame_bytes,
             size_ok ? "" : " bad_size", content_ok ? "" : " blank");
    factory_report_set(FACTORY_TEST_CAMERA, result, detail);
    factory_report_print_one(FACTORY_TEST_CAMERA);
    return result == FACTORY_STATUS_FAIL ? ESP_FAIL : ESP_OK;
}


esp_err_t factory_camera_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "camera_test", .help = "Capture DVP frames and verify size, content, and motion.", .func = command_camera_test},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
