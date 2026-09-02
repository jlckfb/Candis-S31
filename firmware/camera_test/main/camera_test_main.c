/*
 * Candis-S31 camera driver and visual validation demo.
 *
 * This is deliberately a separate firmware image from the product demo. It
 * initializes the AMOLED preview automatically, runs bounded driver checks,
 * and leaves a live camera image on the screen for operator confirmation.
 * The final PASS result is never emitted until the operator taps PASS on the
 * screen; serial-only frame checks are reported as automatic evidence.
 *
 * The esp_cam_sensor option/table named RGB565_BE writes 0x4300=0x6F,
 * although OV5640/Linux semantics define 0x6F as RGB565 LE and 0x61 as BE.
 * The vendored Candis-S31 BSP intentionally applies 0x61 after that table;
 * this diagnostic verifies the resulting RGB565X byte stream without hiding
 * it behind esp_video byte-swapping.
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

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_ioctl.h"
#include "camera_visual.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "sdkconfig.h"
#if CONFIG_CAMERA_TEST_RECEIVER_PCLK_INVERT
#include "hal/cam_ll.h"
#endif

static const char *TAG = "camera_test";

#define CAMERA_TEST_WIDTH             800U
#define CAMERA_TEST_HEIGHT            600U
#define CAMERA_TEST_PIXEL_BYTES       2U
#define CAMERA_TEST_EXPECTED_BYTES    \
    (CAMERA_TEST_WIDTH * CAMERA_TEST_HEIGHT * CAMERA_TEST_PIXEL_BYTES)
#define CAMERA_TEST_MAX_BUFFERS       3U
#define CAMERA_TEST_DQ_TIMEOUT_MS     3000U
#define CAMERA_TEST_HASH_STEP         64U
#define CAMERA_TEST_SENSOR_ADDR       0x3CU
#define CAMERA_TEST_SENSOR_PID        0x5640U
#define CAMERA_TEST_TASK_STACK        8192
#define CAMERA_TEST_TASK_PRIORITY     4

#if CONFIG_CAMERA_TEST_USE_UYVY
#define CAMERA_TEST_V4L2_FORMAT       V4L2_PIX_FMT_UYVY
#define CAMERA_TEST_FORMAT_CTRL       0x32U
#define CAMERA_TEST_FORMAT_MUX        0x00U
#define CAMERA_TEST_FORMAT_NAME       "UYVY"
#define CAMERA_TEST_EXPECTED_AEC      0x03D8U
#else
#define CAMERA_TEST_V4L2_FORMAT       V4L2_PIX_FMT_RGB565X
#define CAMERA_TEST_FORMAT_CTRL       0x61U
#define CAMERA_TEST_FORMAT_MUX        0x01U
#define CAMERA_TEST_FORMAT_NAME       "RGB565X"
#define CAMERA_TEST_EXPECTED_AEC      0x03D8U
#endif

#ifndef CONFIG_CAMERA_TEST_CYCLES
#error "CAMERA_TEST_CYCLES is missing; keep main/Kconfig.projbuild enabled"
#endif
#ifndef CONFIG_CAMERA_TEST_FRAMES
#error "CAMERA_TEST_FRAMES is missing; keep main/Kconfig.projbuild enabled"
#endif
#ifndef CONFIG_CAMERA_TEST_BOOT_DELAY_MS
#error "CAMERA_TEST_BOOT_DELAY_MS is missing; keep main/Kconfig.projbuild enabled"
#endif

#if CONFIG_CAMERA_TEST_CYCLES < 1
#error "CAMERA_TEST_CYCLES must be positive"
#endif
#if CONFIG_CAMERA_TEST_FRAMES < 3
#error "CAMERA_TEST_FRAMES must be at least three"
#endif

typedef enum {
    CAMERA_STATUS_PASS = 0,
    CAMERA_STATUS_WARN,
    CAMERA_STATUS_FAIL,
} camera_status_t;

typedef struct {
    int file;
    uint8_t *buffers[CAMERA_TEST_MAX_BUFFERS];
    uint32_t lengths[CAMERA_TEST_MAX_BUFFERS];
    unsigned buffer_count;
    bool camera_start_attempted;
    bool streaming;
    struct v4l2_format format;
    uint32_t row_bytes;
    uint32_t expected_bytes;
} camera_pipe_t;

typedef struct {
    uint32_t hash;
    uint8_t min_value;
    uint8_t max_value;
    uint32_t samples;
    bool uniform;
} frame_observation_t;

typedef struct {
    bool read_ok;
    uint16_t pid;
    uint8_t format_ctrl;
    uint8_t format_mux;
    uint16_t vts;
    uint16_t aec_max;
} sensor_snapshot_t;

typedef struct {
    camera_status_t status;
    unsigned delivered_frames;
    unsigned error_frames;
    unsigned valid_frames;
    unsigned buffer_mask;
    bool capture_ok;
    bool size_ok;
    bool content_ok;
    bool motion_seen;
    bool sensor_ok;
    bool format_ok;
    bool stream_ok;
    bool stop_ok;
    bool default_format_ok;
    uint16_t sensor_pid;
    uint16_t sensor_vts;
    uint16_t sensor_aec_max;
    uint8_t sensor_format_ctrl;
    uint8_t sensor_format_mux;
    size_t heap_before;
    size_t heap_after;
    uint32_t first_hash;
    uint32_t last_hash;
    uint64_t period_total_us;
    uint64_t period_min_us;
    uint64_t period_max_us;
    uint32_t first_bytes;
    uint32_t center_bytes;
    uint32_t last_bytes;
    int64_t elapsed_us;
} cycle_result_t;

static const char *status_text(camera_status_t status)
{
    switch (status) {
    case CAMERA_STATUS_PASS:
        return "PASS";
    case CAMERA_STATUS_WARN:
        return "WARN";
    default:
        return "FAIL";
    }
}

static void pipe_init(camera_pipe_t *pipe)
{
    memset(pipe, 0, sizeof(*pipe));
    pipe->file = -1;
}

static void fourcc_text(uint32_t fourcc, char out[5])
{
    for (unsigned index = 0; index < 4; ++index) {
        const unsigned char value = (unsigned char)(fourcc >> (index * 8));
        out[index] = (value >= 0x20 && value <= 0x7e) ? (char)value : '.';
    }
    out[4] = '\0';
}

static esp_err_t sensor_read8(i2c_master_dev_handle_t device, uint16_t reg,
                              uint8_t *value)
{
    const uint8_t address[2] = {
        (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff),
    };
    return i2c_master_transmit_receive(device, address, sizeof(address),
                                       value, 1, 100);
}

static bool sensor_read_snapshot(sensor_snapshot_t *snapshot)
{
    memset(snapshot, 0xff, sizeof(*snapshot));
    snapshot->read_ok = false;

    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "CAMERA_TEST_SENSOR status=FAIL reason=no_i2c_bus");
        return false;
    }

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CAMERA_TEST_SENSOR_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t device = NULL;
    esp_err_t error = i2c_master_bus_add_device(bus, &config, &device);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA_TEST_SENSOR status=FAIL add_device=%s",
                 esp_err_to_name(error));
        return false;
    }

    uint8_t pid_high = 0xff;
    uint8_t pid_low = 0xff;
    bool mandatory_ok = true;
    if (sensor_read8(device, 0x300a, &pid_high) != ESP_OK) {
        mandatory_ok = false;
    }
    if (sensor_read8(device, 0x300b, &pid_low) != ESP_OK) {
        mandatory_ok = false;
    }
    if (sensor_read8(device, 0x4300, &snapshot->format_ctrl) != ESP_OK) {
        mandatory_ok = false;
    }
    if (sensor_read8(device, 0x501f, &snapshot->format_mux) != ESP_OK) {
        mandatory_ok = false;
    }

    uint8_t high = 0xff;
    uint8_t low = 0xff;
    if (sensor_read8(device, 0x380e, &high) == ESP_OK &&
            sensor_read8(device, 0x380f, &low) == ESP_OK) {
        snapshot->vts = (uint16_t)((high << 8) | low);
    }
    high = 0xff;
    low = 0xff;
    if (sensor_read8(device, 0x3a02, &high) == ESP_OK &&
            sensor_read8(device, 0x3a03, &low) == ESP_OK) {
        snapshot->aec_max = (uint16_t)((high << 8) | low);
    }

    snapshot->pid = (uint16_t)((pid_high << 8) | pid_low);
    snapshot->read_ok = mandatory_ok;
    const bool pid_ok = snapshot->pid == CAMERA_TEST_SENSOR_PID;
    const bool format_ok = snapshot->format_ctrl == CAMERA_TEST_FORMAT_CTRL;
    const bool mux_ok = snapshot->format_mux == CAMERA_TEST_FORMAT_MUX;
    const bool aec_ok = snapshot->aec_max == CAMERA_TEST_EXPECTED_AEC;
    const bool control_ok = mandatory_ok && pid_ok && format_ok && mux_ok;

    ESP_LOGI(TAG,
             "CAMERA_TEST_SENSOR pid=0x%04x format_ctrl=0x%02x mux=0x%02x "
             "vts=0x%04x aec_max=0x%04x expected=%s status=%s",
             snapshot->pid, snapshot->format_ctrl, snapshot->format_mux,
             snapshot->vts, snapshot->aec_max, CAMERA_TEST_FORMAT_NAME,
             control_ok ? "PASS" : "FAIL");
    if (!mux_ok) {
        ESP_LOGW(TAG, "CAMERA_TEST_SENSOR_NOTE format_mux_expected=0x%02x",
                 CAMERA_TEST_FORMAT_MUX);
    }
    if (!aec_ok) {
        ESP_LOGW(TAG,
                 "CAMERA_TEST_SENSOR_NOTE aec_max_expected=0x%04x "
                 "(value may move after auto exposure)",
                 CAMERA_TEST_EXPECTED_AEC);
    }

    i2c_master_bus_rm_device(device);
    return control_ok;
}

static frame_observation_t inspect_frame(const uint8_t *data, size_t length)
{
    frame_observation_t observation = {
        .hash = 2166136261u,
        .min_value = 0xff,
        .max_value = 0,
        .samples = 0,
        .uniform = true,
    };
    if (data == NULL || length == 0) {
        return observation;
    }

    const uint8_t first = data[0];
    for (size_t offset = 0; offset < length; offset += CAMERA_TEST_HASH_STEP) {
        const uint8_t value = data[offset];
        observation.hash ^= value;
        observation.hash *= 16777619u;
        if (value < observation.min_value) {
            observation.min_value = value;
        }
        if (value > observation.max_value) {
            observation.max_value = value;
        }
        if (value != first) {
            observation.uniform = false;
        }
        ++observation.samples;
    }

    /* Include the final byte so a truncated buffer cannot look identical to
     * a complete one merely because its regular sample positions match. */
    if (length > 1) {
        const uint8_t value = data[length - 1];
        observation.hash ^= value;
        observation.hash *= 16777619u;
        if (value < observation.min_value) {
            observation.min_value = value;
        }
        if (value > observation.max_value) {
            observation.max_value = value;
        }
        if (value != first) {
            observation.uniform = false;
        }
        ++observation.samples;
    }
    return observation;
}

static uint16_t read_rgb565_be(const uint8_t *data, size_t offset)
{
    return (uint16_t)(((uint16_t)data[offset] << 8) | data[offset + 1]);
}

static uint16_t read_rgb565_native(const uint8_t *data, size_t offset)
{
    return (uint16_t)(data[offset] | ((uint16_t)data[offset + 1] << 8));
}

/* Keep the OV5640 table's native PLL at the board's 20 MHz XCLK. The earlier
 * 10 MHz issue-692 override produced unstable frame periods on this module. */
static const uint16_t variant_issue692_pll[][2] = {
    {0x3039, 0x00}, {0x3034, 0x1a}, {0x3035, 0x21},
    {0x3036, 0x46}, {0x3037, 0x13}, {0x3108, 0x01},
    {0x3824, 0x02}, {0x460c, 0x20}, {0x3103, 0x03},
};

static void sensor_write_variant(const uint16_t (*block)[2], size_t count,
                                 const char *name, unsigned cycle)
{
    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "CAMERA_TEST_VARIANT cycle=%u block=%s status=WARN "
                 "reason=no_i2c_bus", cycle, name);
        return;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CAMERA_TEST_SENSOR_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t device = NULL;
    esp_err_t error = i2c_master_bus_add_device(bus, &config, &device);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "CAMERA_TEST_VARIANT cycle=%u block=%s status=WARN "
                 "add_device=%s", cycle, name, esp_err_to_name(error));
        return;
    }
    size_t written = 0;
    for (size_t index = 0; index < count; ++index) {
        error = i2c_master_transmit(device, (const uint8_t[]){
            (uint8_t)(block[index][0] >> 8), (uint8_t)block[index][0],
            (uint8_t)block[index][1],
        }, 3, 100);
        if (error == ESP_OK) {
            ++written;
        } else {
            ESP_LOGW(TAG, "CAMERA_TEST_VARIANT cycle=%u block=%s reg=0x%04x "
                     "error=%s", cycle, name, block[index][0],
                     esp_err_to_name(error));
        }
    }
    ESP_LOGI(TAG, "CAMERA_TEST_VARIANT cycle=%u block=%s regs=%u/%u",
             cycle, name, (unsigned)written, (unsigned)count);
    i2c_master_bus_rm_device(device);
}

static void camera_apply_cycle_variant(unsigned cycle)
{
    sensor_write_variant(variant_issue692_pll,
                         sizeof(variant_issue692_pll) /
                         sizeof(variant_issue692_pll[0]),
                         "issue692_pll_30mhz", cycle);
#if CONFIG_CAMERA_TEST_SENSOR_COLOR_BAR
    static const uint16_t color_bar[][2] = {{0x503d, 0x80}};
    sensor_write_variant(color_bar, 1, "sensor_color_bar", cycle);
#endif

#if CONFIG_CAMERA_TEST_SENSOR_PCLK_INVERT
    static const uint16_t sensor_pclk_invert[][2] = {{0x4740, 0x00}};
    sensor_write_variant(sensor_pclk_invert, 1, "sensor_pclk_invert", cycle);
#endif

#if CONFIG_CAMERA_TEST_UYVY_CTRL_3F
    static const uint16_t uyvy_ctrl_3f[][2] = {
        {0x501f, 0x00}, {0x4300, 0x3f},
    };
    sensor_write_variant(uyvy_ctrl_3f, 2, "uyvy_ctrl_3f", cycle);
#endif
}

static void sensor_dump_run_state(unsigned cycle)
{
    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CAMERA_TEST_SENSOR_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t device = NULL;
    if (i2c_master_bus_add_device(bus, &config, &device) != ESP_OK) {
        return;
    }
    static const uint16_t regs[] = {
        0x3500, 0x3501, 0x3502, 0x350a, 0x350b,
        0x3400, 0x3401, 0x3402, 0x3403, 0x3404, 0x3405, 0x3406,
        0x5000, 0x5001, 0x5180, 0x3a0f, 0x3a10, 0x3a11, 0x503d,
        0x3820, 0x3821, 0x4514, 0x4520, 0x3814, 0x3815,
        0x4300, 0x501f,
        0x3034, 0x3035, 0x3036, 0x3037, 0x3108, 0x3824, 0x460c, 0x3103,
    };
    uint8_t values[sizeof(regs) / sizeof(regs[0])];
    bool ok = true;
    for (size_t index = 0; index < sizeof(regs) / sizeof(regs[0]); ++index) {
        if (sensor_read8(device, regs[index], &values[index]) != ESP_OK) {
            ok = false;
            break;
        }
    }
    if (ok) {
        ESP_LOGI(TAG,
                 "CAMSTATS cycle=%u exp=0x%02x%02x%02x gain=0x%02x%02x "
                 "awb=0x%02x%02x/0x%02x%02x/0x%02x%02x awb_fmt=0x%02x "
                 "ctl=0x%02x/0x%02x/0x%02x target=0x%02x/0x%02x/0x%02x "
                 "pattern=0x%02x phase=%02x/%02x/%02x/%02x inc=%02x/%02x "
                 "out=%02x/%02x pll=%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x",
                 cycle, values[0], values[1], values[2], values[3],
                 values[4], values[5], values[6], values[7], values[8],
                 values[9], values[10], values[11], values[12],
                 values[13], values[14], values[15], values[16],
                 values[17], values[18], values[19], values[20],
                 values[21], values[22], values[23], values[24],
                 values[25], values[26], values[27], values[28],
                 values[29], values[30], values[31], values[32],
                 values[33], values[34]);
    } else {
        ESP_LOGW(TAG, "CAMSTATS cycle=%u status=WARN reason=i2c", cycle);
    }
    i2c_master_bus_rm_device(device);
}





static bool pipe_cleanup(camera_pipe_t *pipe)
{
    bool ok = true;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (pipe->streaming) {
        if (ioctl(pipe->file, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "CAMERA_TEST_CLEANUP streamoff_errno=%d", errno);
            ok = false;
        }
        pipe->streaming = false;
    }
    for (unsigned index = 0; index < pipe->buffer_count &&
            index < CAMERA_TEST_MAX_BUFFERS; ++index) {
        if (pipe->buffers[index] != NULL) {
            munmap(pipe->buffers[index], pipe->lengths[index]);
            pipe->buffers[index] = NULL;
        }
    }
    if (pipe->file >= 0) {
        close(pipe->file);
        pipe->file = -1;
    }
    if (pipe->camera_start_attempted) {
        const esp_err_t error = bsp_camera_stop();
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "CAMERA_TEST_CLEANUP camera_stop=%s",
                     esp_err_to_name(error));
            ok = false;
        }
    }
    return ok;
}

static void log_camera_rail_state(bsp_pmic_regulator_t regulator,
                                  const char *name)
{
    uint16_t millivolts = 0;
    bool enabled = false;
    const esp_err_t voltage_error =
        bsp_pmic_regulator_get_voltage(regulator, &millivolts);
    const esp_err_t enable_error =
        bsp_pmic_regulator_is_enabled(regulator, &enabled);
    ESP_LOGI(TAG,
             "CAMERA_TEST_RAIL name=%s enabled=%u set_mv=%u "
             "enable_read=%s voltage_read=%s",
             name, enabled ? 1U : 0U, (unsigned)millivolts,
             esp_err_to_name(enable_error), esp_err_to_name(voltage_error));
}

#if CONFIG_CAMERA_TEST_RECEIVER_PCLK_INVERT
static void camera_set_receiver_pclk_invert(void)
{
    lcd_cam_dev_t *const hw = CAM_LL_GET_HW(0);
    cam_ll_enable_invert_pclk(hw, true);
    ESP_LOGI(TAG, "CAMERA_TEST_RECEIVER_PCLK invert=1");
}
#endif
static esp_err_t pipe_prepare(camera_pipe_t *pipe, cycle_result_t *result,
                              unsigned cycle)
{
    esp_err_t error = bsp_camera_start(NULL);
    pipe->camera_start_attempted = true;
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA_TEST_PIPE stage=bsp_camera_start error=%s",
                 esp_err_to_name(error));
        return error;
    }
    if (cycle == 1U) {
        log_camera_rail_state(BSP_PMIC_DCDC2, "DVDD");
        log_camera_rail_state(BSP_PMIC_ALDO4, "AVDD");
        log_camera_rail_state(BSP_PMIC_BLDO1, "DOVDD");
    }


    pipe->file = open(BSP_CAMERA_DEVICE, O_RDONLY);
    if (pipe->file < 0) {
        ESP_LOGE(TAG, "CAMERA_TEST_PIPE stage=open device=%s errno=%d",
                 BSP_CAMERA_DEVICE, errno);
        return ESP_ERR_NOT_FOUND;
    }
    /* open() reloads the sensor table; restore the board ISP and output path. */
    error = bsp_camera_apply_workaround();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA_TEST_PIPE post-open workaround error=%s",
                 esp_err_to_name(error));
        return error;
    }

    const struct timeval timeout = {
        .tv_sec = CAMERA_TEST_DQ_TIMEOUT_MS / 1000,
        .tv_usec = (CAMERA_TEST_DQ_TIMEOUT_MS % 1000) * 1000,
    };
    if (ioctl(pipe->file, VIDIOC_S_DQBUF_TIMEOUT, &timeout) != 0) {
        ESP_LOGE(TAG, "CAMERA_TEST_PIPE stage=set_timeout errno=%d", errno);
        return ESP_FAIL;
    }

    struct v4l2_format default_format = {0};
    default_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    result->default_format_ok = ioctl(pipe->file, VIDIOC_G_FMT,
                                      &default_format) == 0;
    if (result->default_format_ok) {
        char fourcc[5];
        fourcc_text(default_format.fmt.pix.pixelformat, fourcc);
        ESP_LOGI(TAG, "CAMERA_TEST_DEFAULT_FMT fourcc=%s width=%u height=%u",
                 fourcc, (unsigned)default_format.fmt.pix.width,
                 (unsigned)default_format.fmt.pix.height);
    } else {
        ESP_LOGW(TAG, "CAMERA_TEST_DEFAULT_FMT status=WARN errno=%d", errno);
    }

    memset(&pipe->format, 0, sizeof(pipe->format));
    pipe->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    pipe->format.fmt.pix.width = CAMERA_TEST_WIDTH;
    pipe->format.fmt.pix.height = CAMERA_TEST_HEIGHT;
    pipe->format.fmt.pix.pixelformat = CAMERA_TEST_V4L2_FORMAT;
    if (ioctl(pipe->file, VIDIOC_S_FMT, &pipe->format) != 0) {
        ESP_LOGE(TAG, "CAMERA_TEST_PIPE stage=set_format errno=%d", errno);
        return ESP_FAIL;
    }
    error = bsp_camera_apply_workaround();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA_TEST_PIPE post-format workaround error=%s",
                 esp_err_to_name(error));
        return error;
    }

    memset(&pipe->format, 0, sizeof(pipe->format));
    pipe->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(pipe->file, VIDIOC_G_FMT, &pipe->format) != 0) {
        ESP_LOGE(TAG, "CAMERA_TEST_PIPE stage=get_format errno=%d", errno);
        return ESP_FAIL;
    }
    sensor_snapshot_t sensor;
    result->sensor_ok = sensor_read_snapshot(&sensor);
    result->sensor_pid = sensor.pid;
    result->sensor_format_ctrl = sensor.format_ctrl;
    result->sensor_format_mux = sensor.format_mux;
    result->sensor_vts = sensor.vts;
    result->sensor_aec_max = sensor.aec_max;

    char fourcc[5];
    fourcc_text(pipe->format.fmt.pix.pixelformat, fourcc);
    pipe->row_bytes = pipe->format.fmt.pix.bytesperline != 0
                      ? pipe->format.fmt.pix.bytesperline
                      : pipe->format.fmt.pix.width * CAMERA_TEST_PIXEL_BYTES;
    pipe->expected_bytes = pipe->row_bytes * pipe->format.fmt.pix.height;
    result->format_ok = pipe->format.fmt.pix.pixelformat == CAMERA_TEST_V4L2_FORMAT &&
                        pipe->format.fmt.pix.width == CAMERA_TEST_WIDTH &&
                        pipe->format.fmt.pix.height == CAMERA_TEST_HEIGHT &&
                        pipe->row_bytes >= CAMERA_TEST_WIDTH *
                        CAMERA_TEST_PIXEL_BYTES &&
                        (pipe->format.fmt.pix.sizeimage == 0 ||
                         pipe->format.fmt.pix.sizeimage >= pipe->expected_bytes);
    ESP_LOGI(TAG,
             "CAMERA_TEST_FORMAT fourcc=%s fourcc_hex=0x%08" PRIx32
             " width=%u height=%u bytesperline=%u sizeimage=%u "
             "assumed_row_bytes=%u status=%s",
             fourcc, pipe->format.fmt.pix.pixelformat,
             (unsigned)pipe->format.fmt.pix.width,
             (unsigned)pipe->format.fmt.pix.height,
             (unsigned)pipe->format.fmt.pix.bytesperline,
             (unsigned)pipe->format.fmt.pix.sizeimage,
             (unsigned)pipe->row_bytes, result->format_ok ? "PASS" : "FAIL");
    if (!result->format_ok) {
        return ESP_ERR_INVALID_SIZE;
    }
    camera_apply_cycle_variant(cycle);

    struct v4l2_requestbuffers request = {0};
    request.count = CAMERA_TEST_MAX_BUFFERS;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(pipe->file, VIDIOC_REQBUFS, &request) != 0) {
        ESP_LOGE(TAG, "CAMERA_TEST_PIPE stage=reqbufs errno=%d", errno);
        return ESP_FAIL;
    }
    pipe->buffer_count = request.count;
    if (pipe->buffer_count < 2 || pipe->buffer_count > CAMERA_TEST_MAX_BUFFERS) {
        ESP_LOGE(TAG, "CAMERA_TEST_BUFFERS count=%u status=FAIL",
                 pipe->buffer_count);
        return ESP_ERR_INVALID_SIZE;
    }

    for (unsigned index = 0; index < pipe->buffer_count; ++index) {
        struct v4l2_buffer query = {0};
        query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        query.memory = V4L2_MEMORY_MMAP;
        query.index = index;
        if (ioctl(pipe->file, VIDIOC_QUERYBUF, &query) != 0) {
            ESP_LOGE(TAG, "CAMERA_TEST_PIPE stage=querybuf index=%u errno=%d",
                     index, errno);
            return ESP_FAIL;
        }
        pipe->lengths[index] = query.length;
        if (query.length < pipe->expected_bytes) {
            ESP_LOGE(TAG,
                     "CAMERA_TEST_BUFFERS index=%u length=%u expected=%u "
                     "status=FAIL",
                     index, (unsigned)query.length,
                     (unsigned)pipe->expected_bytes);
            return ESP_ERR_INVALID_SIZE;
        }
        pipe->buffers[index] = mmap(NULL, query.length,
                                    PROT_READ | PROT_WRITE, MAP_SHARED,
                                    pipe->file, query.m.offset);
        if (pipe->buffers[index] == NULL ||
                pipe->buffers[index] == MAP_FAILED) {
            pipe->buffers[index] = NULL;
            ESP_LOGE(TAG, "CAMERA_TEST_PIPE stage=mmap index=%u errno=%d",
                     index, errno);
            return ESP_FAIL;
        }
        if (ioctl(pipe->file, VIDIOC_QBUF, &query) != 0) {
            ESP_LOGE(TAG, "CAMERA_TEST_PIPE stage=qbuf index=%u errno=%d",
                     index, errno);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "CAMERA_TEST_BUFFERS count=%u length0=%u status=PASS",
             pipe->buffer_count, (unsigned)pipe->lengths[0]);

    const int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(pipe->file, VIDIOC_STREAMON, &stream_type) != 0) {
        return ESP_FAIL;
    }
#if CONFIG_CAMERA_TEST_RECEIVER_PCLK_INVERT
    camera_set_receiver_pclk_invert();
#endif
    pipe->streaming = true;
    result->stream_ok = true;
    return ESP_OK;
}

static void capture_frames(camera_pipe_t *pipe, cycle_result_t *result,
                           unsigned cycle)
{
    int64_t previous_time_us = 0;
    result->capture_ok = true;
    result->size_ok = true;
    result->content_ok = false;
    result->period_min_us = UINT64_MAX;

    for (unsigned frame = 0; frame < CONFIG_CAMERA_TEST_FRAMES; ++frame) {
        struct v4l2_buffer done = {0};
        done.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        done.memory = V4L2_MEMORY_MMAP;
        const int64_t wait_start_us = esp_timer_get_time();
        if (ioctl(pipe->file, VIDIOC_DQBUF, &done) != 0) {
            result->capture_ok = false;
            ESP_LOGE(TAG,
                     "CAMERA_TEST_FRAME cycle=%u frame=%u status=FAIL "
                     "reason=dqbuf errno=%d",
                     cycle, frame + 1, errno);
            break;
        }
        const int64_t frame_time_us = esp_timer_get_time();
        ++result->delivered_frames;

        if (done.index >= pipe->buffer_count ||
                done.index >= CAMERA_TEST_MAX_BUFFERS) {
            result->capture_ok = false;
            ESP_LOGE(TAG,
                     "CAMERA_TEST_FRAME cycle=%u frame=%u status=FAIL "
                     "reason=bad_index index=%u",
                     cycle, frame + 1, done.index);
            break;
        }
        if ((done.flags & V4L2_BUF_FLAG_ERROR) != 0 ||
                (done.flags & V4L2_BUF_FLAG_DONE) == 0) {
            result->capture_ok = false;
            ++result->error_frames;
            result->size_ok = false;
            ESP_LOGE(TAG,
                     "CAMERA_TEST_FRAME cycle=%u frame=%u status=FAIL "
                     "reason=%s bytes=%u",
                     cycle, frame + 1,
                     (done.flags & V4L2_BUF_FLAG_ERROR) != 0 ?
                     "buffer_error" : "not_done",
                     (unsigned)done.bytesused);
            if (ioctl(pipe->file, VIDIOC_QBUF, &done) != 0) {
                result->capture_ok = false;
                ESP_LOGE(TAG,
                         "CAMERA_TEST_FRAME cycle=%u frame=%u "
                         "reason=qbuf_after_error errno=%d",
                         cycle, frame + 1, errno);
            }
            break;
        }

        const bool size_ok = done.bytesused == pipe->expected_bytes &&
                             done.bytesused <= pipe->lengths[done.index];
        if (!size_ok) {
            result->size_ok = false;
            result->capture_ok = false;
        }

        const size_t inspect_length = done.bytesused <= pipe->lengths[done.index]
                                      ? done.bytesused
                                      : pipe->lengths[done.index];
        frame_observation_t observation = inspect_frame(
            pipe->buffers[done.index], inspect_length);
        const bool content_ok = inspect_length >= 2 &&
                                !observation.uniform &&
                                observation.max_value != observation.min_value;
        if (size_ok) {
#if CONFIG_CAMERA_TEST_USE_UYVY
            (void)camera_visual_publish_uyvy(pipe->buffers[done.index],
                                             inspect_length, pipe->row_bytes);
#else
            (void)camera_visual_publish(pipe->buffers[done.index],
                                         inspect_length, pipe->row_bytes);
#endif
        }
        if (content_ok) {
            result->content_ok = true;
            ++result->valid_frames;
            if (result->valid_frames == 1) {
                result->first_hash = observation.hash;
            } else if (observation.hash != result->last_hash) {
                result->motion_seen = true;
            }
            result->last_hash = observation.hash;
            result->buffer_mask |= 1u << done.index;
            if (result->valid_frames == 1) {
            }
        }

        const size_t center_offset = ((CAMERA_TEST_HEIGHT / 2U) *
                                      pipe->row_bytes) +
                                     ((CAMERA_TEST_WIDTH / 2U) * 2U);
        const size_t last_offset = ((CAMERA_TEST_HEIGHT - 1U) * pipe->row_bytes) +
                                   ((CAMERA_TEST_WIDTH - 1U) * 2U);
        if (inspect_length > last_offset + 1U) {
            const uint16_t first_be = read_rgb565_be(pipe->buffers[done.index], 0);
            const uint16_t center_be = read_rgb565_be(pipe->buffers[done.index],
                                                      center_offset);
            const uint16_t last_be = read_rgb565_be(pipe->buffers[done.index],
                                                    last_offset);
            const uint16_t first_native = read_rgb565_native(
                pipe->buffers[done.index], 0);
            const uint16_t center_native = read_rgb565_native(
                pipe->buffers[done.index], center_offset);
            const uint16_t last_native = read_rgb565_native(
                pipe->buffers[done.index], last_offset);
            if (result->valid_frames == 1) {
                result->first_bytes = first_be;
                result->center_bytes = center_be;
                result->last_bytes = last_be;
            }
            ESP_LOGI(TAG,
                     "CAMERA_TEST_FRAME cycle=%u frame=%u index=%u bytes=%u "
                     "hash=0x%08" PRIx32 " wait_us=%" PRId64 " period_us=%" PRId64
                     " raw_first=%02x%02x rgb565x_first=0x%04x "
                     "native_first=0x%04x raw_center=%04x raw_last=%04x "
                     "range=%u..%u status=%s",
                     cycle, frame + 1, done.index, (unsigned)done.bytesused,
                     observation.hash, wait_start_us == frame_time_us ? 0 :
                     frame_time_us - wait_start_us,
                     previous_time_us == 0 ? 0 : frame_time_us - previous_time_us,
                     pipe->buffers[done.index][0], pipe->buffers[done.index][1],
                     first_be, first_native, center_be, last_be,
                     observation.min_value, observation.max_value,
                     size_ok && content_ok ? "PASS" : "WARN");
            (void)center_native;
            (void)last_native;
        } else {
            ESP_LOGI(TAG,
                     "CAMERA_TEST_FRAME cycle=%u frame=%u index=%u bytes=%u "
                     "hash=0x%08" PRIx32 " status=%s",
                     cycle, frame + 1, done.index, (unsigned)done.bytesused,
                     observation.hash, size_ok && content_ok ? "PASS" : "WARN");
        }


        if (previous_time_us != 0) {
            const uint64_t period = (uint64_t)(frame_time_us - previous_time_us);
            result->period_total_us += period;
            if (period < result->period_min_us) {
                result->period_min_us = period;
            }
            if (period > result->period_max_us) {
                result->period_max_us = period;
            }
        }
        previous_time_us = frame_time_us;

        if (ioctl(pipe->file, VIDIOC_QBUF, &done) != 0) {
            result->capture_ok = false;
            ESP_LOGE(TAG,
                     "CAMERA_TEST_FRAME cycle=%u frame=%u status=FAIL "
                     "reason=qbuf errno=%d",
                     cycle, frame + 1, errno);
            break;
        }
    }
    if (result->valid_frames > 0) {
        sensor_dump_run_state(cycle);
    }
    if (result->delivered_frames != CONFIG_CAMERA_TEST_FRAMES) {
        result->capture_ok = false;
    }
    if (result->period_min_us == UINT64_MAX) {
        result->period_min_us = 0;
    }
}

static camera_status_t evaluate_cycle(const cycle_result_t *result)
{
    const bool hard_fail = !result->sensor_ok || !result->format_ok ||
                           !result->stream_ok || !result->stop_ok ||
                           !result->capture_ok || result->error_frames != 0 ||
                           !result->size_ok || !result->content_ok ||
                           result->valid_frames != CONFIG_CAMERA_TEST_FRAMES;
    if (hard_fail) {
        return CAMERA_STATUS_FAIL;
    }
    /* A static scene is valid camera output but cannot prove frame motion
     * without asking the operator to move something in front of the lens. */
    return result->motion_seen ? CAMERA_STATUS_PASS : CAMERA_STATUS_WARN;
}

static cycle_result_t run_cycle(unsigned cycle)
{
    const int64_t cycle_start_us = esp_timer_get_time();
    cycle_result_t result = {
        .status = CAMERA_STATUS_FAIL,
        .size_ok = true,
        .stop_ok = true,
        .heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    };
    camera_pipe_t pipe;
    pipe_init(&pipe);

    ESP_LOGI(TAG, "CAMERA_TEST_CYCLE_BEGIN cycle=%u", cycle);
    const esp_err_t prepare_error = pipe_prepare(&pipe, &result, cycle);
    if (prepare_error == ESP_OK) {
        capture_frames(&pipe, &result, cycle);
    } else {
        ESP_LOGE(TAG, "CAMERA_TEST_CYCLE cycle=%u stage=prepare error=%s",
                 cycle, esp_err_to_name(prepare_error));
    }

    /* STREAMOFF, unmap, close, and BSP shutdown are intentionally exercised
     * on every cycle, including failed setup paths. */
    result.stop_ok = pipe_cleanup(&pipe);
    result.heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    result.elapsed_us = esp_timer_get_time() - cycle_start_us;

    result.status = evaluate_cycle(&result);
    const uint64_t period_avg = result.delivered_frames > 1
                                ? result.period_total_us /
                                (result.delivered_frames - 1U)
                                : 0;
    ESP_LOGI(TAG,
             "CAMERA_TEST_CYCLE_END cycle=%u status=%s delivered=%u valid=%u "
             "errors=%u buffers=0x%x capture_ok=%d size_ok=%d content_ok=%d "
             "motion=%d sensor_ok=%d format_ok=%d stream_ok=%d stop_ok=%d "
             "pid=0x%04x 4300=0x%02x 501f=0x%02x vts=0x%04x aec=0x%04x "
             "period_us=%" PRIu64 "/%" PRIu64 "-%" PRIu64
             " elapsed_us=%" PRId64 " heap=%zu->%zu",
             cycle, status_text(result.status), result.delivered_frames,
             result.valid_frames, result.error_frames, result.buffer_mask,
             result.capture_ok, result.size_ok, result.content_ok,
             result.motion_seen, result.sensor_ok, result.format_ok,
             result.stream_ok, result.stop_ok, result.sensor_pid,
             result.sensor_format_ctrl, result.sensor_format_mux,
             result.sensor_vts, result.sensor_aec_max, period_avg,
             result.period_min_us, result.period_max_us, result.elapsed_us,
             result.heap_before, result.heap_after);
    return result;
}
static bool preview_until_confirmation(camera_pipe_t *pipe)
{
    for (;;) {
        if (camera_visual_get_decision() != CAMERA_VISUAL_PENDING) {
            return true;
        }

        struct v4l2_buffer done = {0};
        done.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        done.memory = V4L2_MEMORY_MMAP;
        if (ioctl(pipe->file, VIDIOC_DQBUF, &done) != 0) {
            ESP_LOGE(TAG, "CAMERA_TEST_PREVIEW status=FAIL reason=dqbuf errno=%d",
                     errno);
            camera_visual_set_error("Live preview DQBUF failed");
            return false;
        }
        if (done.index >= pipe->buffer_count ||
                done.index >= CAMERA_TEST_MAX_BUFFERS) {
            ESP_LOGE(TAG, "CAMERA_TEST_PREVIEW status=FAIL reason=bad_index index=%u",
                     done.index);
            camera_visual_set_error("Live preview returned an invalid buffer");
            return false;
        }
        if ((done.flags & V4L2_BUF_FLAG_ERROR) != 0 ||
                (done.flags & V4L2_BUF_FLAG_DONE) == 0 ||
                done.bytesused != pipe->expected_bytes ||
                done.bytesused > pipe->lengths[done.index]) {
            ESP_LOGE(TAG,
                     "CAMERA_TEST_PREVIEW status=FAIL flags=0x%08" PRIx32
                     " bytes=%u expected=%u",
                     done.flags, (unsigned)done.bytesused,
                     (unsigned)pipe->expected_bytes);
            (void)ioctl(pipe->file, VIDIOC_QBUF, &done);
            camera_visual_set_error("Live preview frame is invalid");
            return false;
        }
#if CONFIG_CAMERA_TEST_DISPLAY_PATTERN
        (void)camera_visual_publish_test_pattern();
#elif CONFIG_CAMERA_TEST_USE_UYVY
        (void)camera_visual_publish_uyvy(pipe->buffers[done.index],
                                         done.bytesused, pipe->row_bytes);
#else
        (void)camera_visual_publish(pipe->buffers[done.index],
                                     done.bytesused, pipe->row_bytes);
#endif
        if (ioctl(pipe->file, VIDIOC_QBUF, &done) != 0) {
            ESP_LOGE(TAG, "CAMERA_TEST_PREVIEW status=FAIL reason=qbuf errno=%d",
                     errno);
            camera_visual_set_error("Live preview QBUF failed");
            return false;
        }
    }
}

static void camera_test_task(void *arg)
{
    (void)arg;
    unsigned pass_cycles = 0;
    unsigned warn_cycles = 0;
    unsigned fail_cycles = 0;
    bool auto_ok = true;

    for (unsigned cycle = 1; cycle <= CONFIG_CAMERA_TEST_CYCLES; ++cycle) {
        camera_visual_set_streaming(true);
        const cycle_result_t result = run_cycle(cycle);
        camera_visual_set_streaming(false);
        if (result.status == CAMERA_STATUS_PASS) {
            ++pass_cycles;
        } else if (result.status == CAMERA_STATUS_WARN) {
            ++warn_cycles;
        } else {
            ++fail_cycles;
            auto_ok = false;
        }
        if (cycle < CONFIG_CAMERA_TEST_CYCLES) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGI(TAG,
             "CAMERA_TEST_AUTO_SUMMARY status=%s cycles=%d pass=%u warn=%u fail=%u",
             auto_ok ? "PASS" : "FAIL", CONFIG_CAMERA_TEST_CYCLES,
             pass_cycles, warn_cycles, fail_cycles);
    camera_visual_set_auto_state(auto_ok, CONFIG_CAMERA_TEST_CYCLES,
                                 CONFIG_CAMERA_TEST_FRAMES);

    if (!auto_ok) {
        camera_visual_set_error("Automatic driver checks failed");
        ESP_LOGI(TAG,
                 "CAMERA_TEST_SUMMARY status=FAIL visual_confirmation=blocked "
                 "reason=automatic_checks");
        vTaskDelete(NULL);
        return;
    }

    camera_pipe_t preview_pipe;
    pipe_init(&preview_pipe);
    cycle_result_t preview_result = {
        .status = CAMERA_STATUS_FAIL,
        .size_ok = true,
        .stop_ok = true,
    };
    /* Cycle 4 uses the same PLL with the test pattern disabled, so the screen
     * shows the real scene after the D0-D7 inference captures complete. */
    const esp_err_t prepare_error = pipe_prepare(&preview_pipe,
                                                  &preview_result, 4);
    if (prepare_error != ESP_OK) {
        camera_visual_set_error("Final live preview could not start");
        (void)pipe_cleanup(&preview_pipe);
        ESP_LOGI(TAG,
                 "CAMERA_TEST_SUMMARY status=FAIL visual_confirmation=blocked "
                 "reason=preview_start error=%s",
                 esp_err_to_name(prepare_error));
        vTaskDelete(NULL);
        return;
    }

    camera_visual_set_streaming(true);
    const bool preview_ok = preview_until_confirmation(&preview_pipe);
    camera_visual_set_streaming(false);
    const bool stop_ok = pipe_cleanup(&preview_pipe);
    const camera_visual_decision_t decision = camera_visual_get_decision();
    const bool visual_pass = decision == CAMERA_VISUAL_PASS;
    const bool final_pass = preview_ok && stop_ok && visual_pass;
    const char *confirmation = decision == CAMERA_VISUAL_PASS ? "pass" :
                               decision == CAMERA_VISUAL_FAIL ? "fail" :
                               "missing";
    ESP_LOGI(TAG,
             "CAMERA_TEST_SUMMARY status=%s visual_confirmation=%s "
             "format=%s automatic=%s preview_ok=%d stop_ok=%d",
             final_pass ? "PASS" : "FAIL", confirmation,
             camera_visual_is_swapped() ? "RGB565_SWAPPED" : "RGB565",
             auto_ok ? "PASS" : "FAIL", preview_ok, stop_ok);
    vTaskDelete(NULL);
}

static void idle_forever(camera_status_t status)
{
    for (;;) {
        ESP_LOGI(TAG, "CAMERA_TEST_IDLE status=%s", status_text(status));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "CAMERA_TEST_BEGIN version=5 cycles=%d frames=%d "
             "dq_timeout_ms=%u device=%s",
             CONFIG_CAMERA_TEST_CYCLES, CONFIG_CAMERA_TEST_FRAMES,
             CAMERA_TEST_DQ_TIMEOUT_MS, BSP_CAMERA_DEVICE);
    ESP_LOGI(TAG,
             "CAMERA_TEST_CONTRACT sensor=OV5640 v4l2=%s display=RGB565_SWAPPED "
             "visual_confirmation=required",
             CAMERA_TEST_FORMAT_NAME);

    vTaskDelay(pdMS_TO_TICKS(CONFIG_CAMERA_TEST_BOOT_DELAY_MS));
    const esp_err_t board_error = bsp_board_init();
    if (board_error != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA_TEST_SUMMARY status=FAIL stage=board_init error=%s",
                 esp_err_to_name(board_error));
        idle_forever(CAMERA_STATUS_FAIL);
    }

    const esp_err_t visual_error = camera_visual_start();
    if (visual_error != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA_TEST_SUMMARY status=FAIL stage=display_init error=%s",
                 esp_err_to_name(visual_error));
        idle_forever(CAMERA_STATUS_FAIL);
    }
    ESP_LOGI(TAG, "CAMERA_TEST_BOARD_INIT status=PASS display=on");

    if (xTaskCreate(camera_test_task, "camera_test", CAMERA_TEST_TASK_STACK,
                    NULL, CAMERA_TEST_TASK_PRIORITY, NULL) != pdPASS) {
        camera_visual_set_error("Camera test task could not start");
        ESP_LOGE(TAG, "CAMERA_TEST_SUMMARY status=FAIL stage=task_create");
        idle_forever(CAMERA_STATUS_FAIL);
    }
}
