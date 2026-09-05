/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>

#include "esp_check.h"
#include "driver/ledc.h"
#include "soc/ledc_struct.h"
#include "esp_video_init.h"
#include "driver/i2c_master.h"
#include "linux/videodev2.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/candis_s31.h"
#include "ov5640_af_firmware.h"

static const char *TAG = "candis_camera";
static bool s_started;
static bool s_autofocus_firmware_loaded;

/* EVT1's measured-good source clock is an exact 20 MHz, 50 percent
 * duty-cycle signal generated from XTAL-backed LEDC. The post-format sensor
 * profile derives 95 MHz DVP byte clock / 47.5 Mpixel/s from that input.
 * Keeping XCLK outside esp_video also gives the BSP sole ownership of the
 * shared pin. */
static esp_err_t xclk_ledc_start(void)
{
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC
    /* Exact 20 MHz from the 40 MHz XTAL, matching the validated EVT1 path. */
    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 20000000,
        .clk_cfg = LEDC_USE_XTAL_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG,
                        "XCLK LEDC timer config failed");
    const ledc_channel_config_t channel_config = {
        .gpio_num = BSP_CAMERA_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = CONFIG_BSP_CAMERA_XCLK_LEDC_CH,
        .timer_sel = LEDC_TIMER_1,
        .duty = 1, /* 1 of 2 counts: exact 50% */
        .hpoint = 0,
    };
    return ledc_channel_config(&channel_config);
#else
    return ESP_OK;
#endif
}

static esp_err_t xclk_ledc_stop(void)
{
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC
    /* Park the pin low; the next xclk_ledc_start() reconfigures it. */
    return ledc_stop(LEDC_LOW_SPEED_MODE, CONFIG_BSP_CAMERA_XCLK_LEDC_CH, 0);
#else
    return ESP_OK;
#endif
}

#define OV5640_SCCB_ADDRESS         0x3CU
#define OV5640_AF_CMD_MAIN          0x3022U
#define OV5640_AF_CMD_ACK           0x3023U
#define OV5640_AF_CMD_PARA0         0x3024U
#define OV5640_AF_FW_STATUS         0x3029U
#define OV5640_AF_CMD_TRIGGER       0x03U
#define OV5640_AF_CMD_GET_RESULT    0x07U
#define OV5640_AF_CMD_RELAUNCH_ZONE 0x12U
#define OV5640_AF_STATUS_IDLE       0x70U
#define OV5640_AF_STATUS_FOCUSED    0x10U
#define OV5640_PREVIEW_HTS          0x0768U
#define OV5640_PREVIEW_VTS          0x0343U
#define OV5640_PREVIEW_AEC_MAX      0x033FU

static esp_err_t camera_sccb_open(i2c_master_dev_handle_t *device)
{
    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OV5640_SCCB_ADDRESS,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &config, device);
}

static esp_err_t camera_sensor_write8(i2c_master_dev_handle_t device,
                                      uint16_t reg, uint8_t value)
{
    const uint8_t payload[] = {
        (uint8_t)(reg >> 8), (uint8_t)reg, value,
    };
    return i2c_master_transmit(device, payload, sizeof(payload), 100);
}

static esp_err_t camera_sensor_read8(i2c_master_dev_handle_t device,
                                     uint16_t reg, uint8_t *value)
{
    const uint8_t address[] = {
        (uint8_t)(reg >> 8), (uint8_t)reg,
    };
    return i2c_master_transmit_receive(device, address, sizeof(address),
                                       value, 1, 100);
}

static esp_err_t camera_sensor_wait8(i2c_master_dev_handle_t device,
                                     uint16_t reg, uint8_t expected,
                                     uint32_t timeout_ms, uint8_t *last)
{
    const int64_t deadline =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t value = 0xff;
    do {
        const esp_err_t error = camera_sensor_read8(device, reg, &value);
        if (error != ESP_OK) {
            return error;
        }
        if (last != NULL) {
            *last = value;
        }
        if (value == expected) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (esp_timer_get_time() <= deadline);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t camera_autofocus_command(i2c_master_dev_handle_t device,
                                          uint8_t command,
                                          uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(
        camera_sensor_write8(device, OV5640_AF_CMD_ACK, 0x01), TAG,
        "autofocus ACK arm failed");
    ESP_RETURN_ON_ERROR(
        camera_sensor_write8(device, OV5640_AF_CMD_MAIN, command), TAG,
        "autofocus command 0x%02x failed", command);
    return camera_sensor_wait8(device, OV5640_AF_CMD_ACK, 0x00,
                               timeout_ms, NULL);
}

static esp_err_t camera_autofocus_load(i2c_master_dev_handle_t device,
                                       uint32_t timeout_ms)
{
    if (s_autofocus_firmware_loaded) {
        return ESP_OK;
    }

    const int64_t started_us = esp_timer_get_time();
    esp_err_t error = camera_sensor_write8(device, 0x3000, 0x20);
    for (size_t i = 0;
            error == ESP_OK && i < sizeof(s_ov5640_af_firmware); ++i) {
        error = camera_sensor_write8(
            device, (uint16_t)(0x8000U + i), s_ov5640_af_firmware[i]);
    }

    uint8_t first = 0xff;
    uint8_t last = 0xff;
    if (error == ESP_OK) {
        error = camera_sensor_read8(device, 0x8000, &first);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(
            device,
            (uint16_t)(0x8000U + sizeof(s_ov5640_af_firmware) - 1U),
            &last);
    }
    if (error == ESP_OK &&
            (first != s_ov5640_af_firmware[0] ||
             last != s_ov5640_af_firmware[
                         sizeof(s_ov5640_af_firmware) - 1U])) {
        error = ESP_ERR_INVALID_RESPONSE;
    }

    for (uint16_t reg = OV5640_AF_CMD_MAIN;
            error == ESP_OK && reg <= OV5640_AF_CMD_PARA0 + 4U; ++reg) {
        error = camera_sensor_write8(device, reg, 0x00);
    }
    if (error == ESP_OK) {
        error = camera_sensor_write8(
            device, OV5640_AF_FW_STATUS, 0x7f);
    }
    if (error == ESP_OK) {
        error = camera_sensor_write8(device, 0x3000, 0x00);
    }
    uint8_t firmware_status = 0xff;
    if (error == ESP_OK) {
        error = camera_sensor_wait8(
            device, OV5640_AF_FW_STATUS, OV5640_AF_STATUS_IDLE,
            timeout_ms, &firmware_status);
    }
    if (error == ESP_OK) {
        s_autofocus_firmware_loaded = true;
    }
    ESP_LOGI(TAG,
             "autofocus firmware status=%s bytes=%u verify=%02x/%02x "
             "fw_status=0x%02x elapsed_ms=%" PRId64,
             error == ESP_OK ? "PASS" : "FAIL",
             (unsigned)sizeof(s_ov5640_af_firmware), first, last,
             firmware_status, (esp_timer_get_time() - started_us) / 1000);
    return error;
}

/* Board-level profile for the OV5640 autofocus module on the EVT1 FPC.
 * It is applied before and after the first video-device open:
 * esp_video_open() lazily runs dvp_video_init(), whose format-table write
 * resets these registers after the pre-open pass. Three facts established on
 * 2026-08-31 by serial frame analysis (see AGENT-AI.md 0P):
 *
 * 1. The esp_cam_sensor option/table is named "RGB565_BE" but writes
 *    FORMAT_CTRL0 0x4300 = 0x6F. That upstream name is inverted relative
 *    to OV5640/Linux semantics: 0x6F is RGB565 LE, while 0x61 is RGB565
 *    BE. Do not change the verified 0x61 override back to 0x6F based only
 *    on the esp_cam_sensor symbol name.
 *
 *    With that table's 0x6F value the module streams raw Bayer - its ISP
 *    demosaic and color engine never run (RGB output is a mosaic; YUV422
 *    U/V sit at 128 +/- 3). espressif/esp32-camera's proven sequence
 *    0x4300 = 0x61 makes the same module stream processed color. Serial
 *    frame dumps prove that its first captured byte carries RGB565 bits
 *    [15:8] and the second carries [7:0], i.e. V4L2 RGB565X byte order.
 * 2. The esp32-camera AWB/CMX values restore correct color. For live view,
 *    the upstream esp_cam_sensor automatic CIP/gamma/SDE profile replaces
 *    forced manual edge enhancement and non-vendor thresholds that can erase
 *    fine texture before the host receives a frame.
 * 3. The old 20 MHz table timing produced about 11.9 fps and allowed
 *    44-72 ms exposures. The calculated 30 fps profile below keeps HTS
 *    unchanged, reduces VTS, raises the sensor PLL within its datasheet
 *    limit, and caps banded exposure at one frame.
 *
 * The capture buffer therefore matches both V4L2_PIX_FMT_RGB565X and
 * the LV_COLOR_FORMAT_RGB565_SWAPPED image source used by the app.
 *
 * The board supplies DVDD externally from TG28 DCDC2. OV5640 register
 * 0x3031 resets with its internal core regulator enabled; leaving both
 * regulators active back-drives CAM_DVDD_1V5_SW to about 1.63 V even when
 * DCDC2 is programmed lower. Set SC PWC bit 3 so the external rail is the
 * only core supply, as required by the datasheet's external-DVDD sequence. */
static esp_err_t camera_sensor_workaround(uint32_t v4l2_pixel_format)
{
    static const uint16_t isp_block[][2] = {
        {0x5000, 0xa7}, {0x5001, 0xa3}, {0x5003, 0x08},
        {0x5180, 0xff}, {0x5181, 0xf2}, {0x5182, 0x00},
        {0x5183, 0x14}, {0x5184, 0x25}, {0x5185, 0x24},
        {0x5186, 0x09}, {0x5187, 0x09}, {0x5188, 0x09},
        {0x5189, 0x75}, {0x518a, 0x54}, {0x518b, 0xe0},
        {0x518c, 0xb2}, {0x518d, 0x42}, {0x518e, 0x3d},
        {0x518f, 0x56}, {0x5190, 0x46}, {0x5191, 0xf8},
        {0x5192, 0x04}, {0x5193, 0x70}, {0x5194, 0xf0},
        {0x5195, 0xf0}, {0x5196, 0x03}, {0x5197, 0x01},
        {0x5198, 0x04}, {0x5199, 0x12}, {0x519a, 0x04},
        {0x519b, 0x00}, {0x519c, 0x06}, {0x519d, 0x82},
        {0x519e, 0x38},
        {0x5381, 0x1e}, {0x5382, 0x5b}, {0x5383, 0x08},
        {0x5384, 0x0a}, {0x5385, 0x7e}, {0x5386, 0x88},
        {0x5387, 0x7c}, {0x5388, 0x6c}, {0x5389, 0x10},
        {0x538a, 0x01}, {0x538b, 0x98},
        /*
         * Restore esp_cam_sensor's 800x600 preview tuning. In particular,
         * keep edge enhancement and raw denoise in the table's automatic
         * modes (0x5308=0x25); esp32-camera's manual edge override destroyed
         * fine texture on this live-view path.
         */
        {0x5300, 0x08}, {0x5301, 0x30}, {0x5302, 0x10},
        {0x5303, 0x00}, {0x5304, 0x08}, {0x5305, 0x30},
        {0x5306, 0x08}, {0x5307, 0x16}, {0x5308, 0x25},
        {0x5309, 0x08}, {0x530a, 0x30}, {0x530b, 0x04},
        {0x530c, 0x06},
        {0x5480, 0x01}, {0x5481, 0x08}, {0x5482, 0x14},
        {0x5483, 0x28}, {0x5484, 0x51}, {0x5485, 0x65},
        {0x5486, 0x71}, {0x5487, 0x7d}, {0x5488, 0x87},
        {0x5489, 0x91}, {0x548a, 0x9a}, {0x548b, 0xaa},
        {0x548c, 0xb8}, {0x548d, 0xcd}, {0x548e, 0xdd},
        {0x548f, 0xea}, {0x5490, 0x1d},
        {0x5580, 0x02}, {0x5583, 0x40}, {0x5584, 0x10},
        {0x5589, 0x10}, {0x558a, 0x00}, {0x558b, 0xf8},
        {0x501d, 0x40},
    };
    /*
     * Motion-preview timing. The 8-bit DVP sends two byte clocks per RGB565
     * pixel. The earlier measured 29.9 fps profile used 0x3034=0x1a,
     * 0x3035=0x21, 0x3036=0xb0 and 0x3037=0x13. It proved the board path,
     * but its 117.3 MHz PCLK exceeded the OV5640's 96 MHz maximum and drove
     * PLL1 to about 1.17 GHz.
     *
     *   PLL1         = 20 MHz / 2 * 76         = 760 MHz
     *   DVP PCLK     = PLL1 / 2 / 2 / 2        = 95 MHz
     *   RGB565 rate  = 95 MHz / 2 bytes         = 47.5 MHz
     *   fps          = 47.5 MHz / (1896 * 835) = 30.003
     *
     * B50=251 lines with max=3 limits 50 Hz exposure to 30.06 ms;
     * B60=209 with max=3 limits 60 Hz exposure to 25.03 ms. AEC=VTS-4
     * preserves readout margin and replaces the measured 44-72 ms range.
     */
    static const uint8_t timing_regs[][2] = {
        {0x38, 0x0c}, {0x38, 0x0d},
        {0x38, 0x0e}, {0x38, 0x0f},
        {0x3a, 0x02}, {0x3a, 0x03},
        {0x3a, 0x14}, {0x3a, 0x15},
        {0x3a, 0x08}, {0x3a, 0x09},
        {0x3a, 0x0a}, {0x3a, 0x0b},
        {0x3a, 0x0d}, {0x3a, 0x0e},
    };
    static const uint8_t timing_vals[] = {
        0x07, 0x68, 0x03, 0x43, 0x03, 0x3f, 0x03, 0x3f,
        0x00, 0xfb, 0x00, 0xd1, 0x03, 0x03,
    };
    _Static_assert(sizeof(timing_vals) ==
                   sizeof(timing_regs) / sizeof(timing_regs[0]),
                   "OV5640 timing register/value count mismatch");
    static const uint8_t pll_bit_divider = 0x18;
    static const uint8_t pll_system_divider = 0x21;
    static const uint8_t pll_multiplier = 0x4c;
    static const uint8_t pll_pre_root_divider = 0x12;
    static const uint8_t system_root_dividers = 0x01;
    i2c_master_dev_handle_t sccb = NULL;
    esp_err_t error = camera_sccb_open(&sccb);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor workaround: sccb device add failed: %s",
                 esp_err_to_name(error));
        return error;
    }
    uint8_t pid_high = 0xff;
    uint8_t pid_low = 0xff;
    error = i2c_master_transmit_receive(sccb,
                                        (const uint8_t[]){0x30, 0x0a},
                                        2, &pid_high, 1, 100);
    if (error == ESP_OK) {
        error = i2c_master_transmit_receive(sccb,
                                            (const uint8_t[]){0x30, 0x0b},
                                            2, &pid_low, 1, 100);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor workaround: PID read failed: %s",
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    if (((uint16_t)pid_high << 8 | pid_low) != 0x5640) {
        ESP_LOGI(TAG, "sensor workaround skipped for PID=0x%02x%02x",
                 pid_high, pid_low);
        i2c_master_bus_rm_device(sccb);
        return ESP_OK;
    }
    /* VIDIOC_S_FMT can reset the sensor MCU and always invalidates a
     * previously downloaded autofocus image. */
    s_autofocus_firmware_loaded = false;
    uint8_t output_format;
    uint8_t output_mux;
    switch (v4l2_pixel_format) {
    case V4L2_PIX_FMT_RGB565X:
        /* The working RGB565 sequence emits big-endian bytes (RGB565X). */
        output_format = 0x61;
        output_mux = 0x01;
        break;
    case V4L2_PIX_FMT_UYVY:
        output_format = 0x32;
        output_mux = 0x00;
        break;
    default:
        ESP_LOGE(TAG, "sensor workaround: unsupported V4L2 format 0x%08lx",
                 (unsigned long)v4l2_pixel_format);
        i2c_master_bus_rm_device(sccb);
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t power_control = 0xff;
    error = i2c_master_transmit_receive(sccb,
                                        (const uint8_t[]){0x30, 0x31},
                                        2, &power_control, 1, 100);
    if (error == ESP_OK) {
        error = i2c_master_transmit(sccb, (const uint8_t[]){
#if CONFIG_BSP_CAMERA_USE_INTERNAL_DVDD
            0x30, 0x31, (uint8_t)(power_control & (uint8_t)~0x08U),
#else
            0x30, 0x31, (uint8_t)(power_control | 0x08U),
#endif
        }, 3, 100);
    }
    uint8_t power_control_check = 0xff;
    if (error == ESP_OK) {
        error = i2c_master_transmit_receive(sccb,
                                            (const uint8_t[]){0x30, 0x31},
                                            2, &power_control_check, 1, 100);
    }
#if CONFIG_BSP_CAMERA_USE_INTERNAL_DVDD
    const char *expected_mode = "internal";
    const bool dvdd_mode_ok = (power_control_check & 0x08U) == 0;
#else
    const char *expected_mode = "external";
    const bool dvdd_mode_ok = (power_control_check & 0x08U) != 0;
#endif
    if (error != ESP_OK || !dvdd_mode_ok) {
        ESP_LOGW(TAG,
                 "sensor workaround: DVDD mode setup failed "
                 "0x3031=0x%02x expected_%s error=%s",
                 power_control_check, expected_mode,
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error != ESP_OK ? error : ESP_FAIL;
    }
    for (size_t i = 0; i < sizeof(isp_block) / sizeof(isp_block[0]); ++i) {
        error = i2c_master_transmit(sccb, (const uint8_t[]){
            (uint8_t)(isp_block[i][0] >> 8), (uint8_t)isp_block[i][0],
            (uint8_t)isp_block[i][1],
        }, 3, 100);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "sensor workaround: ISP write %u failed: %s",
                     (unsigned)i, esp_err_to_name(error));
            i2c_master_bus_rm_device(sccb);
            return error;
        }
    }
    /* The module's working RGB565 sequence emits RGB565X byte order. */
    error = i2c_master_transmit(sccb,
        (const uint8_t[]){0x50, 0x1f, output_mux}, 3, 100);
    if (error == ESP_OK) {
        error = i2c_master_transmit(sccb,
            (const uint8_t[]){0x43, 0x00, output_format}, 3, 100);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor workaround: output format write failed: %s",
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    for (size_t i = 0;
            i < sizeof(timing_regs) / sizeof(timing_regs[0]); ++i) {
        error = i2c_master_transmit(sccb, (const uint8_t[]){
            timing_regs[i][0], timing_regs[i][1], timing_vals[i],
        }, 3, 100);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "sensor workaround: timing write %u failed: %s",
                     (unsigned)i, esp_err_to_name(error));
            i2c_master_bus_rm_device(sccb);
            return error;
        }
    }
    error = i2c_master_transmit(sccb, (const uint8_t[]){
        0x30, 0x34, pll_bit_divider,
    }, 3, 100);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor PLL bit divider write failed: %s",
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    error = i2c_master_transmit(sccb, (const uint8_t[]){
        0x30, 0x35, pll_system_divider,
    }, 3, 100);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor PLL system divider write failed: %s",
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    error = i2c_master_transmit(sccb, (const uint8_t[]){
        0x30, 0x36, pll_multiplier,
    }, 3, 100);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor PLL multiplier write failed: %s",
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    error = i2c_master_transmit(sccb, (const uint8_t[]){
        0x30, 0x37, pll_pre_root_divider,
    }, 3, 100);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor PLL root divider write failed: %s",
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    error = i2c_master_transmit(sccb, (const uint8_t[]){
        0x31, 0x08, system_root_dividers,
    }, 3, 100);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor system root divider write failed: %s",
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    uint8_t check = 0;
    uint8_t mux_check = 0;
    uint8_t pll_bit_divider_check = 0;
    uint8_t pll_system_divider_check = 0;
    uint8_t pll_multiplier_check = 0;
    uint8_t pll_pre_root_divider_check = 0;
    uint8_t system_root_dividers_check = 0;
    uint8_t hts_high = 0;
    uint8_t hts_low = 0;
    uint8_t vts_high = 0;
    uint8_t vts_low = 0;
    uint8_t aec_high = 0;
    uint8_t aec_low = 0;
    uint8_t cip_control = 0;
    error = camera_sensor_read8(sccb, 0x4300, &check);
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x501f, &mux_check);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(
            sccb, 0x3034, &pll_bit_divider_check);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(
            sccb, 0x3035, &pll_system_divider_check);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x3036, &pll_multiplier_check);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(
            sccb, 0x3037, &pll_pre_root_divider_check);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(
            sccb, 0x3108, &system_root_dividers_check);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x380c, &hts_high);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x380d, &hts_low);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x380e, &vts_high);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x380f, &vts_low);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x3a02, &aec_high);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x3a03, &aec_low);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x5308, &cip_control);
    }
    const uint16_t hts_check = (uint16_t)((hts_high << 8) | hts_low);
    const uint16_t vts_check = (uint16_t)((vts_high << 8) | vts_low);
    const uint16_t aec_check = (uint16_t)((aec_high << 8) | aec_low);
    if (error != ESP_OK || check != output_format ||
            mux_check != output_mux ||
            pll_bit_divider_check != pll_bit_divider ||
            pll_system_divider_check != pll_system_divider ||
            pll_multiplier_check != pll_multiplier ||
            pll_pre_root_divider_check != pll_pre_root_divider ||
            system_root_dividers_check != system_root_dividers ||
            hts_check != OV5640_PREVIEW_HTS ||
            vts_check != OV5640_PREVIEW_VTS ||
            aec_check != OV5640_PREVIEW_AEC_MAX ||
            cip_control != 0x25) {
        ESP_LOGW(TAG,
                 "sensor workaround: readback failed output=0x%02x/0x%02x "
                 "pll=0x%02x/0x%02x/0x%02x/0x%02x/0x%02x "
                 "hts=0x%04x vts=0x%04x aec=0x%04x cip=0x%02x error=%s",
                 check, mux_check, pll_bit_divider_check,
                 pll_system_divider_check, pll_multiplier_check,
                 pll_pre_root_divider_check, system_root_dividers_check,
                 hts_check, vts_check, aec_check, cip_control,
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error != ESP_OK ? error : ESP_FAIL;
    }
    ESP_LOGI(TAG,
             "sensor workaround applied: 0x3031=0x%02x output=0x%02x/0x%02x "
             "pll=0x%02x/0x%02x/0x%02x/0x%02x/0x%02x "
             "hts=0x%04x vts=0x%04x aec=0x%04x cip=0x%02x "
             "isp_regs=%u dvdd=%s",
             power_control_check, check, mux_check, pll_bit_divider_check,
             pll_system_divider_check, pll_multiplier_check,
             pll_pre_root_divider_check, system_root_dividers_check,
             hts_check, vts_check, aec_check, cip_control,
             (unsigned)(sizeof(isp_block) / sizeof(isp_block[0])),
             expected_mode);
    i2c_master_bus_rm_device(sccb);
    return ESP_OK;
}

esp_err_t bsp_camera_apply_workaround(uint32_t v4l2_pixel_format)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    return camera_sensor_workaround(v4l2_pixel_format);
}

esp_err_t bsp_camera_autofocus_once(uint32_t timeout_ms)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t device = NULL;
    esp_err_t error = camera_sccb_open(&device);
    if (error != ESP_OK) {
        return error;
    }

    const int64_t started_us = esp_timer_get_time();
    uint8_t firmware_status = 0xff;
    uint8_t zones[5] = {0xff, 0xff, 0xff, 0xff, 0xff};
    error = camera_autofocus_load(device, timeout_ms);
    if (error == ESP_OK) {
        error = camera_autofocus_command(
            device, OV5640_AF_CMD_RELAUNCH_ZONE, timeout_ms);
    }
    if (error == ESP_OK) {
        error = camera_autofocus_command(
            device, OV5640_AF_CMD_TRIGGER, timeout_ms);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(
            device, OV5640_AF_FW_STATUS, &firmware_status);
    }
    if (error == ESP_OK && firmware_status != OV5640_AF_STATUS_FOCUSED) {
        error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error == ESP_OK) {
        error = camera_autofocus_command(
            device, OV5640_AF_CMD_GET_RESULT, timeout_ms);
    }
    bool zone_focused = false;
    for (size_t i = 0; error == ESP_OK && i < 5; ++i) {
        error = camera_sensor_read8(
            device, (uint16_t)(OV5640_AF_CMD_PARA0 + i), &zones[i]);
        zone_focused = zone_focused || (error == ESP_OK && zones[i] == 0);
    }
    if (error == ESP_OK && !zone_focused) {
        error = ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG,
             "autofocus status=%s fw_status=0x%02x "
             "zones=%02x/%02x/%02x/%02x/%02x elapsed_ms=%" PRId64,
             error == ESP_OK ? "PASS" : "FAIL", firmware_status,
             zones[0], zones[1], zones[2], zones[3], zones[4],
             (esp_timer_get_time() - started_us) / 1000);
    const esp_err_t remove_error = i2c_master_bus_rm_device(device);
    return error != ESP_OK ? error : remove_error;
}

esp_err_t bsp_camera_start(const bsp_camera_cfg_t *cfg)
{
    (void)cfg;
    if (s_started) {
        return ESP_OK;
    }
    s_autofocus_firmware_loaded = false;
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "main I2C init failed");
    ESP_RETURN_ON_ERROR(bsp_peripheral_power_set(BSP_PERIPHERAL_CAMERA, true),
                        TAG, "camera power-up failed");

    esp_err_t error = xclk_ledc_start();
    if (error != ESP_OK) {
        bsp_peripheral_power_set(BSP_PERIPHERAL_CAMERA, false);
        return error;
    }

    /* With board-owned LEDC XCLK, keep the CAM controller away from the
     * shared pin. xclk_io=GPIO_NUM_NC prevents pin routing and xclk_freq=0
     * skips esp_cam_ctlr_dvp_output_clock(); DVP input capture is unchanged. */
    const esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = bsp_i2c_get_handle(),
            .freq = 100000,
        },
        .reset_pin = BSP_CAMERA_RST,
        .pwdn_pin = BSP_CAMERA_PWDN,
        .dvp_pin = {
            .data_width = 8,
            /* The fitted FD6540/OV5640-compatible module does not use the
             * reference camera's D0-D7 order. Three OV5640 built-in patterns
             * identify D0<->D1 and D3<->D4; keep the schematic net names but
             * route them into the controller's logical bit positions here. */
#if CONFIG_BSP_CAMERA_SENSOR_DATA_REMAP
            .data_io = {
                BSP_CAMERA_D1, BSP_CAMERA_D0, BSP_CAMERA_D2, BSP_CAMERA_D4,
                BSP_CAMERA_D3, BSP_CAMERA_D5, BSP_CAMERA_D6, BSP_CAMERA_D7,
            },
#else
            .data_io = {
                BSP_CAMERA_D0, BSP_CAMERA_D1, BSP_CAMERA_D2, BSP_CAMERA_D3,
                BSP_CAMERA_D4, BSP_CAMERA_D5, BSP_CAMERA_D6, BSP_CAMERA_D7,
            },
#endif
            .vsync_io = BSP_CAMERA_VSYNC,
            .de_io = BSP_CAMERA_HSYNC,
            .pclk_io = BSP_CAMERA_PCLK,
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC
            .xclk_io = GPIO_NUM_NC,
#else
            .xclk_io = BSP_CAMERA_XCLK,
#endif
        },
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC
        .xclk_freq = 0,
#else
        .xclk_freq = BSP_CAMERA_XCLK_CLOCK_MHZ * 1000000,
#endif
    };
    /*
     * The AF module's VCM supply is the raw AF_VCC rail; no separate host-side
     * motor device exists. The OV5640's embedded MCU drives that VCM, so
     * cam_motor remains unset and bsp_camera_autofocus_once() controls focus
     * through the sensor SCCB interface after streaming starts.
     */
    const esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };
    /* Only the DVP device is initialized; the plain esp_video_init() would
     * initialize every video device enabled in sdkconfig (ISP, JPEG, ...). */
    error = esp_video_init_with_flags(&video_config, ESP_VIDEO_INIT_FLAGS_DVP);
    if (error == ESP_OK) {
        const esp_err_t workaround_error =
            camera_sensor_workaround(V4L2_PIX_FMT_RGB565X);
        if (workaround_error != ESP_OK) {
            ESP_LOGW(TAG, "pre-open sensor workaround failed: %s",
                     esp_err_to_name(workaround_error));
        }
        s_started = true;
        return ESP_OK;
    }

    xclk_ledc_stop();
    bsp_peripheral_power_set(BSP_PERIPHERAL_CAMERA, false);
    return error;
}

esp_err_t bsp_camera_stop(void)
{
    if (!s_started) {
        return bsp_peripheral_power_set(BSP_PERIPHERAL_CAMERA, false);
    }
    /* Tear down only what bsp_camera_start() initialized. The controller's own
     * XCLK output is released by esp_video_deinit_with_flags(). */
    esp_err_t result = esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_DVP);
    const esp_err_t xclk_error = xclk_ledc_stop();
    if (result == ESP_OK) {
        result = xclk_error;
    }
    s_started = false;
    s_autofocus_firmware_loaded = false;
    const esp_err_t power_error =
        bsp_peripheral_power_set(BSP_PERIPHERAL_CAMERA, false);
    return result != ESP_OK ? result : power_error;
}
