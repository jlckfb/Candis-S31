/*
 * Candis-S31 camera preview demo.
 *
 * Continuous OV5640 preview on the 2.0-inch 460x460 AMOLED. The image is
 * cropped to the panel's square aspect ratio and published directly, with no
 * overlay or operator interaction. Initialisation and stream failures are
 * shown on the panel and reported on the serial console.
 *
 * The board profile (byte order, timing, DVDD source and colour trim) lives in
 * components/candis_s31/src/bsp_camera.c; this example only selects the V4L2
 * format and drives the preview.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <fcntl.h>

#include "bsp/esp-bsp.h"
#include "camera_visual.h"
#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "sdkconfig.h"

#if !defined(CONFIG_CAMERA_TEST_FORCE_50HZ)
#define CONFIG_CAMERA_TEST_FORCE_50HZ 0
#endif
#if !defined(CONFIG_CAMERA_TEST_SENSOR_STATS)
#define CONFIG_CAMERA_TEST_SENSOR_STATS 0
#endif

static const char *TAG = "camera_test";

#define CAMERA_WIDTH             800U
#define CAMERA_HEIGHT            600U
#define CAMERA_PIXEL_BYTES       2U
#define CAMERA_MAX_BUFFERS       3U
#define CAMERA_DQ_TIMEOUT_MS     3000U
#define CAMERA_TASK_STACK        8192
#define CAMERA_TASK_PRIORITY     4
#define CAMERA_RATE_WINDOW_US    2000000
/* A single short or errored transfer is skipped; only a run of them means
 * the pipeline is broken. */
#define CAMERA_SKIPPED_FRAME_LIMIT 15U
#define CAMERA_SENSOR_ADDRESS    0x3CU
/* IDF's non-JPEG DVP path reports the configured size even for a short DMA.
 * A cache-line sentinel detects an untouched frame tail without editing the
 * driver or mistaking old PSRAM contents for a complete image. */
#define CAMERA_GUARD_WORDS       16U
#define CAMERA_GUARD_BYTES       (CAMERA_GUARD_WORDS * sizeof(uint32_t))

typedef struct {
    int file;
    uint8_t *buffers[CAMERA_MAX_BUFFERS];
    uint32_t lengths[CAMERA_MAX_BUFFERS];
    uint32_t tail_guard[CAMERA_MAX_BUFFERS][CAMERA_GUARD_WORDS];
    uint32_t guard_generation;
    unsigned buffer_count;
    uint32_t stride;
    uint32_t expected_bytes;
    bool camera_started;
    bool streaming;
} camera_pipe_t;

static void pipe_init(camera_pipe_t *pipe)
{
    memset(pipe, 0, sizeof(*pipe));
    pipe->file = -1;
}

static int camera_qbuf(camera_pipe_t *pipe, const struct v4l2_buffer *buffer)
{
    if (buffer->index >= pipe->buffer_count ||
            pipe->expected_bytes < CAMERA_GUARD_BYTES ||
            (pipe->expected_bytes % CAMERA_GUARD_BYTES) != 0) {
        errno = EINVAL;
        return -1;
    }
    uint32_t *guard = pipe->tail_guard[buffer->index];
    const uint32_t seed = ++pipe->guard_generation * UINT32_C(0x9e3779b9);
    for (unsigned word = 0; word < CAMERA_GUARD_WORDS; ++word) {
        guard[word] = seed ^ (UINT32_C(0xa58c137f) + word * UINT32_C(0x10204081));
    }
    uint8_t *tail = pipe->buffers[buffer->index] + pipe->expected_bytes -
                    CAMERA_GUARD_BYTES;
    memcpy(tail, guard, CAMERA_GUARD_BYTES);
    if (esp_cache_msync(tail, CAMERA_GUARD_BYTES,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) {
        errno = EIO;
        return -1;
    }
    return ioctl(pipe->file, VIDIOC_QBUF, buffer);
}

static bool camera_tail_written(const camera_pipe_t *pipe, uint32_t index)
{
    const uint8_t *tail = pipe->buffers[index] + pipe->expected_bytes -
                          CAMERA_GUARD_BYTES;
    return memcmp(tail, pipe->tail_guard[index], CAMERA_GUARD_BYTES) != 0;
}

static void pipe_cleanup(camera_pipe_t *pipe)
{
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (pipe->streaming) {
        if (ioctl(pipe->file, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "STREAMOFF failed errno=%d", errno);
        }
        pipe->streaming = false;
    }
    for (unsigned index = 0; index < pipe->buffer_count &&
            index < CAMERA_MAX_BUFFERS; ++index) {
        if (pipe->buffers[index] != NULL) {
            munmap(pipe->buffers[index], pipe->lengths[index]);
            pipe->buffers[index] = NULL;
        }
    }
    if (pipe->file >= 0) {
        close(pipe->file);
        pipe->file = -1;
    }
    if (pipe->camera_started) {
        const esp_err_t error = bsp_camera_stop();
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "camera power-down failed: %s", esp_err_to_name(error));
        }
        pipe->camera_started = false;
    }
}

#if CONFIG_CAMERA_TEST_FORCE_50HZ
static esp_err_t camera_set_manual_50hz(void)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CAMERA_SENSOR_ADDRESS,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t device = NULL;
    esp_err_t error = i2c_master_bus_add_device(bsp_i2c_get_handle(), &config,
                                                &device);
    if (error != ESP_OK) {
        return error;
    }
    /* OV5640 DVP Application Notes 2.13, 4.7.8: manual 50 Hz. Keep reserved
     * and the board's clock-derived band steps; set only the selection bits. */
    static const struct {
        uint16_t reg;
        uint8_t mask;
    } settings[] = {
        {0x3c00, 0x04}, {0x3c01, 0x80}, {0x3a00, 0x20},
    };
    for (unsigned i = 0; error == ESP_OK &&
            i < sizeof(settings) / sizeof(settings[0]); ++i) {
        const uint8_t address[2] = {
            (uint8_t)(settings[i].reg >> 8), (uint8_t)settings[i].reg,
        };
        uint8_t before = 0;
        uint8_t after = 0;
        error = i2c_master_transmit_receive(device, address, sizeof(address),
                                            &before, 1, 100);
        const uint8_t wanted = before | settings[i].mask;
        if (error == ESP_OK) {
            const uint8_t payload[3] = {
                address[0], address[1], wanted,
            };
            error = i2c_master_transmit(device, payload, sizeof(payload), 100);
        }
        if (error == ESP_OK) {
            error = i2c_master_transmit_receive(device, address,
                                                sizeof(address), &after, 1, 100);
            if (error == ESP_OK && after != wanted) {
                error = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    const esp_err_t cleanup = i2c_master_bus_rm_device(device);
    return error != ESP_OK ? error : cleanup;
}
#endif

static esp_err_t pipe_open(camera_pipe_t *pipe)
{
    esp_err_t error = bsp_camera_start(NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "bsp_camera_start failed: %s", esp_err_to_name(error));
        return error;
    }
    pipe->camera_started = true;

    pipe->file = open(BSP_CAMERA_DEVICE, O_RDONLY);
    if (pipe->file < 0) {
        ESP_LOGE(TAG, "open(%s) failed errno=%d", BSP_CAMERA_DEVICE, errno);
        return ESP_ERR_NOT_FOUND;
    }

    const struct timeval timeout = {
        .tv_sec = CAMERA_DQ_TIMEOUT_MS / 1000,
        .tv_usec = (CAMERA_DQ_TIMEOUT_MS % 1000) * 1000,
    };
    if (ioctl(pipe->file, VIDIOC_S_DQBUF_TIMEOUT, &timeout) != 0) {
        ESP_LOGE(TAG, "DQBUF timeout setup failed errno=%d", errno);
        return ESP_FAIL;
    }

    struct v4l2_format format = {0};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = CAMERA_WIDTH;
    format.fmt.pix.height = CAMERA_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565X;
    if (ioctl(pipe->file, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed errno=%d", errno);
        return ESP_FAIL;
    }
    /* esp_video_open()/S_FMT reruns the sensor format table, so the board
     * profile has to be reapplied before the buffers are queued. */
    error = bsp_camera_apply_workaround(V4L2_PIX_FMT_RGB565X);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "board camera profile failed: %s", esp_err_to_name(error));
        return error;
    }

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(pipe->file, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed errno=%d", errno);
        return ESP_FAIL;
    }
    pipe->stride = format.fmt.pix.bytesperline != 0
                   ? format.fmt.pix.bytesperline
                   : format.fmt.pix.width * CAMERA_PIXEL_BYTES;
    pipe->expected_bytes = pipe->stride * format.fmt.pix.height;
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565X ||
            format.fmt.pix.width != CAMERA_WIDTH ||
            format.fmt.pix.height != CAMERA_HEIGHT ||
            pipe->stride < CAMERA_WIDTH * CAMERA_PIXEL_BYTES ||
            (format.fmt.pix.sizeimage != 0 &&
             format.fmt.pix.sizeimage < pipe->expected_bytes)) {
        ESP_LOGE(TAG, "unexpected V4L2 format %ux%u fourcc=0x%08" PRIx32
                 " stride=%" PRIu32,
                 (unsigned)format.fmt.pix.width,
                 (unsigned)format.fmt.pix.height,
                 (uint32_t)format.fmt.pix.pixelformat, pipe->stride);
        return ESP_ERR_INVALID_SIZE;
    }

    struct v4l2_requestbuffers request = {0};
    request.count = CAMERA_MAX_BUFFERS;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(pipe->file, VIDIOC_REQBUFS, &request) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed errno=%d", errno);
        return ESP_FAIL;
    }
    pipe->buffer_count = request.count;
    if (pipe->buffer_count < 2 || pipe->buffer_count > CAMERA_MAX_BUFFERS) {
        ESP_LOGE(TAG, "unexpected buffer count %u", pipe->buffer_count);
        return ESP_ERR_INVALID_SIZE;
    }

    for (unsigned index = 0; index < pipe->buffer_count; ++index) {
        struct v4l2_buffer query = {0};
        query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        query.memory = V4L2_MEMORY_MMAP;
        query.index = index;
        if (ioctl(pipe->file, VIDIOC_QUERYBUF, &query) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF index=%u failed errno=%d",
                     index, errno);
            return ESP_FAIL;
        }
        pipe->lengths[index] = query.length;
        if (query.length < pipe->expected_bytes) {
            ESP_LOGE(TAG, "buffer %u is %u bytes, need %" PRIu32, index,
                     (unsigned)query.length, pipe->expected_bytes);
            return ESP_ERR_INVALID_SIZE;
        }
        pipe->buffers[index] = mmap(NULL, query.length,
                                    PROT_READ | PROT_WRITE, MAP_SHARED,
                                    pipe->file, query.m.offset);
        if (pipe->buffers[index] == MAP_FAILED) {
            pipe->buffers[index] = NULL;
            ESP_LOGE(TAG, "mmap index=%u failed errno=%d", index, errno);
            return ESP_FAIL;
        }
        if (camera_qbuf(pipe, &query) != 0) {
            ESP_LOGE(TAG, "QBUF index=%u failed errno=%d", index, errno);
            return ESP_FAIL;
        }
    }

#if CONFIG_CAMERA_TEST_FORCE_50HZ
    error = camera_set_manual_50hz();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "manual 50 Hz setup failed: %s", esp_err_to_name(error));
        return error;
    }
#endif

    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(pipe->file, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed errno=%d", errno);
        return ESP_FAIL;
    }
    pipe->streaming = true;
    return ESP_OK;
}


#if CONFIG_CAMERA_TEST_SENSOR_STATS
#define CAMERA_STATS_I2C(h, r, v) do { \
        const uint8_t a[2] = {(uint8_t)((r) >> 8), (uint8_t)(r)}; \
        ok = ok && i2c_master_transmit_receive((h), a, sizeof(a), (v), 1, 100) == ESP_OK; \
    } while (0)

/* One line per second with the sensor's exposure, AGC gain, AWB gains and the
 * frame's mean RGB. Use it when tuning the board sensor profile for a
 * different module or illuminant; a normal preview is silent without it. */
static void camera_log_sensor_stats(const uint8_t *frame, uint32_t stride)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CAMERA_SENSOR_ADDRESS,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t device = NULL;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &device) != ESP_OK) {
        return;
    }
    static unsigned tick;
    ++tick;
    bool ok = true;
    uint8_t exp[3] = {0}, agc = 0, awb[6] = {0};
    uint8_t limit[3] = {0};
    CAMERA_STATS_I2C(device, 0x3500, &exp[0]);
    CAMERA_STATS_I2C(device, 0x3501, &exp[1]);
    CAMERA_STATS_I2C(device, 0x3502, &exp[2]);
    CAMERA_STATS_I2C(device, 0x350b, &agc);
    CAMERA_STATS_I2C(device, 0x519f, &awb[0]);
    CAMERA_STATS_I2C(device, 0x51a0, &awb[1]);
    CAMERA_STATS_I2C(device, 0x51a1, &awb[2]);
    CAMERA_STATS_I2C(device, 0x51a2, &awb[3]);
    CAMERA_STATS_I2C(device, 0x51a3, &awb[4]);
    CAMERA_STATS_I2C(device, 0x51a4, &awb[5]);
    CAMERA_STATS_I2C(device, 0x5193, &limit[0]);
    CAMERA_STATS_I2C(device, 0x5194, &limit[1]);
    CAMERA_STATS_I2C(device, 0x5195, &limit[2]);
    uint64_t sum[3] = {0, 0, 0};
    uint32_t samples = 0;
    for (uint32_t y = 0; y < CAMERA_HEIGHT; y += 5U) {
        const uint8_t *row = frame + (size_t)y * stride;
        for (uint32_t x = 0; x < CAMERA_WIDTH; x += 5U) {
            const uint16_t pixel = (uint16_t)((row[x * 2U] << 8) | row[x * 2U + 1U]);
            sum[0] += (pixel >> 11) & 0x1fU;
            sum[1] += (pixel >> 5) & 0x3fU;
            sum[2] += pixel & 0x1fU;
            ++samples;
        }
    }
    const uint32_t r5 = samples ? (uint32_t)(sum[0] / samples) * 255U / 31U : 0;
    const uint32_t g6 = samples ? (uint32_t)(sum[1] / samples) * 255U / 63U : 0;
    const uint32_t b5 = samples ? (uint32_t)(sum[2] / samples) * 255U / 31U : 0;
    ESP_LOGI(TAG, "CAMERA_SENSOR_STATS t=%u exp=0x%02x%02x%02x agc=0x%02x "
             "awb=0x%02x%02x/0x%02x%02x/0x%02x%02x limit=0x%02x/0x%02x/0x%02x "
             "rgb=%u/%u/%u status=%s",
             tick, exp[0], exp[1], exp[2], agc,
             awb[0], awb[1], awb[2], awb[3], awb[4], awb[5],
             limit[0], limit[1], limit[2], r5, g6, b5,
             ok ? "PASS" : "WARN");
    i2c_master_bus_rm_device(device);
}
#endif


/* Returns false on a fatal I/O error. *valid reports frame integrity: a
 * frame that fails the checks is skipped (never displayed) so a single short
 * or errored transfer right after STREAMON cannot end the preview. */
static bool camera_take_frame(camera_pipe_t *pipe, bool *valid,
                              const uint8_t **frame, uint32_t *stride)
{
    struct v4l2_buffer done = {0};
    done.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    done.memory = V4L2_MEMORY_MMAP;
    if (ioctl(pipe->file, VIDIOC_DQBUF, &done) != 0) {
        camera_visual_set_error("Camera stopped responding");
        ESP_LOGE(TAG, "VIDIOC_DQBUF failed errno=%d", errno);
        return false;
    }
    const bool index_ok = done.index < pipe->buffer_count &&
                          done.index < CAMERA_MAX_BUFFERS;
    *valid = index_ok &&
             (done.flags & V4L2_BUF_FLAG_ERROR) == 0 &&
             (done.flags & V4L2_BUF_FLAG_DONE) != 0 &&
             done.bytesused == pipe->expected_bytes &&
             done.bytesused <= pipe->lengths[done.index] &&
             camera_tail_written(pipe, done.index);
    if (*valid) {
        *frame = pipe->buffers[done.index];
        *stride = pipe->stride;
        *valid = camera_visual_publish(pipe->buffers[done.index],
                                       done.bytesused, pipe->stride);
    } else if (index_ok) {
        const uint32_t *tail = (const uint32_t *)(
            pipe->buffers[done.index] + pipe->expected_bytes -
            CAMERA_GUARD_BYTES);
        ESP_LOGW(TAG, "skipped frame index=%u flags=0x%08" PRIx32
                 " bytes=%u expected=%" PRIu32 " tail=0x%08" PRIx32
                 " guard=0x%08" PRIx32, done.index, done.flags,
                 (unsigned)done.bytesused, pipe->expected_bytes, tail[0],
                 pipe->tail_guard[done.index][0]);
    }
    if (camera_qbuf(pipe, &done) != 0) {
        camera_visual_set_error("Camera buffer requeue failed");
        ESP_LOGE(TAG, "QBUF failed errno=%d", errno);
        return false;
    }
    return true;
}

static void preview_loop(camera_pipe_t *pipe)
{
    int64_t first_frame_us = esp_timer_get_time();
    int64_t reported_start_us = first_frame_us;
    uint32_t captured = 0;
    unsigned skipped = 0;
    bool ready = false;
    bool rate_reported = false;

    for (;;) {
        bool valid = false;
        const uint8_t *frame = NULL;
        uint32_t stride = 0;
        if (!camera_take_frame(pipe, &valid, &frame, &stride)) {
            return;
        }
        if (!valid) {
            if (++skipped >= CAMERA_SKIPPED_FRAME_LIMIT) {
                camera_visual_set_error("Camera frames are not valid");
                ESP_LOGE(TAG, "too many invalid frames; stopping preview");
                return;
            }
            continue;
        }
        skipped = 0;
        const int64_t now_us = esp_timer_get_time();
        ++captured;
        if (!ready) {
            ready = true;
            ESP_LOGI(TAG, "CAMERA_PREVIEW_READY format=RGB565X source=%ux%u "
                     "crop=460x460 first_frame_ms=%" PRId64,
                     CAMERA_WIDTH, CAMERA_HEIGHT, (now_us - first_frame_us) / 1000);
        }
#if CONFIG_CAMERA_TEST_SENSOR_STATS
        static int64_t stats_next_us;
        if (frame != NULL && now_us >= stats_next_us) {
            stats_next_us = now_us + 1000000;
            camera_log_sensor_stats(frame, stride);
        }
#endif
        if (!rate_reported && now_us - reported_start_us >= CAMERA_RATE_WINDOW_US) {
            rate_reported = true;
            const camera_visual_stats_t stats = camera_visual_get_stats();
            const uint32_t fps_x10 = (uint32_t)(
                ((uint64_t)captured * 10000000ULL +
                 (uint64_t)(now_us - reported_start_us) / 2U) /
                (uint64_t)(now_us - reported_start_us));
            ESP_LOGI(TAG, "CAMERA_PREVIEW_RATE capture=%" PRIu32
                     ".%01" PRIu32 "fps presented=%" PRIu32 " superseded=%" PRIu32,
                     fps_x10 / 10U, fps_x10 % 10U, stats.presented,
                     stats.superseded);
        }
    }
}

static void camera_task(void *arg)
{
    (void)arg;
    camera_pipe_t pipe;
    pipe_init(&pipe);
    const esp_err_t error = pipe_open(&pipe);
    if (error == ESP_OK) {
        preview_loop(&pipe);
    } else {
        char message[80];
        snprintf(message, sizeof(message), "Camera error: %s",
                 esp_err_to_name(error));
        camera_visual_set_error(message);
    }
    pipe_cleanup(&pipe);
    ESP_LOGE(TAG, "CAMERA_PREVIEW_STOPPED error=%s", esp_err_to_name(error));
    vTaskDelete(NULL);
}

void app_main(void)
{
    const esp_err_t board_error = bsp_board_init();
    if (board_error != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(board_error));
        return;
    }
    const esp_err_t visual_error = camera_visual_start();
    if (visual_error != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(visual_error));
        return;
    }
    if (xTaskCreate(camera_task, "camera_test", CAMERA_TASK_STACK, NULL,
                    CAMERA_TASK_PRIORITY, NULL) != pdPASS) {
        camera_visual_set_error("Camera task could not start");
        ESP_LOGE(TAG, "camera task creation failed");
    }
}
