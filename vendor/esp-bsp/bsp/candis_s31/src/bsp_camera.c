/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "driver/ledc.h"
#include "soc/ledc_struct.h"
#include "esp_video_init.h"
#include "driver/i2c_master.h"

#include "bsp/candis_s31.h"

static const char *TAG = "candis_camera";
static bool s_started;
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC

#endif

/* The selected OV5640 table is authored for a nominal 24 MHz XCLK, so keep
 * BSP_CAMERA_XCLK_CLOCK_MHZ as that table identifier. The S31 DVP clock
 * router cannot generate an exact 24 MHz integer divider; the board-validated
 * LEDC path below uses the 40 MHz XTAL divided by two (20 MHz, measured about
 * 20.1 MHz by PCNT). This intentional timing deviation is reported by the
 * headless camera test; it must not be mistaken for a measured 24 MHz clock. */
_Static_assert(BSP_CAMERA_XCLK_CLOCK_MHZ == 24,
               "OV5640 table identifier must remain 24 MHz");

/* XCLK comes from LEDC, not from the CAM controller.
 *
 * esp_video_init_with_flags() would route the DVP controller's own camera
 * clock to dvp_pin.xclk_io whenever xclk_io >= 0 && xclk_freq > 0:
 * init_dvp_clk_func() in esp_video_init.c calls
 * esp_cam_ctlr_dvp_output_clock(). That driver path deliberately supports
 * only integer dividers ("camera sensors require precision without frequency
 * and duty cycle jitter, so the fractional divisor can't be used" -
 * esp_cam_ctlr_dvp_cam.c), and the S31 CAM controller clock sources
 * (PLL_F160M 160 MHz, XTAL 40 MHz, APLL) contain no integer multiple of the
 * 24 MHz the OV5640 register tables require (160 % 24 = 16, 40 % 24 = 16).
 * esp_cam_ctlr_dvp_output_clock() therefore hard-fails with
 * "calculated frequency divider is not integer" and
 * esp_video_init_with_flags() aborts before any sensor detection.
 * APLL could reach 96 MHz = 4 x 24 MHz, but esp_video hardcodes
 * CAM_CLK_SRC_DEFAULT (PLL_F160M), so the controller path cannot be used
 * without patching official components, which this project forbids.
 *
 * The working configuration drives XCLK from LEDC instead:
 * CONFIG_BSP_CAMERA_XCLK_USE_LEDC=y starts a fractional LEDC channel on
 * BSP_CAMERA_XCLK before esp_video runs, and dvp_config below passes
 * xclk_io=GPIO_NUM_NC / xclk_freq=0 so esp_cam_ctlr_dvp_init() skips the pin
 * and init_dvp_clk_func() skips the failing output_clock() call. The DVP
 * video device still creates its controller with pin_dont_init=true, so the
 * capture path is unaffected. */
static esp_err_t xclk_ledc_start(void)
{
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC
    /* Use the exact XTAL/2 setting that was measured on the S31 board. The
     * PLL_DIV path's nominal 24 MHz request measured about 18.75 MHz, while
     * its compensation setting was not stable across the tested clock paths.
     * The camera diagnostic reports the resulting frame period so this
     * deliberate 20 MHz operating point remains visible in evidence. */
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

/* Board-level workaround for the FD6540 (OV5640-compatible) module on
 * the EVT1 FPC. It is applied before and after the first video-device open:
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
 * 2. The esp32-camera ISP block (AWB/CMX/CIP/gamma/SDE below, verbatim
 *    from its sensor_default_regs) then yields correctly white-balanced
 *    color; the esp_cam_sensor block values do not.
 * 3. The BE table's trailing vendor "test" block raises the AEC exposure
 *    ceiling (0x3a02/03, 0x3a14/15) past one frame (VTS 0x040a); restore
 *    the table's own ceiling 0x03d8 to keep dim-light frame rate stable.
 *
 * The capture buffer therefore matches both V4L2_PIX_FMT_RGB565X and
 * the LV_COLOR_FORMAT_RGB565_SWAPPED image source used by the app. */
static esp_err_t camera_sensor_workaround(void)
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
        {0x5300, 0x10}, {0x5301, 0x10}, {0x5302, 0x18},
        {0x5303, 0x19}, {0x5304, 0x10}, {0x5305, 0x10},
        {0x5306, 0x08}, {0x5307, 0x16}, {0x5308, 0x40},
        {0x5309, 0x10}, {0x530a, 0x10}, {0x530b, 0x04},
        {0x530c, 0x06},
        {0x5480, 0x01}, {0x5481, 0x00}, {0x5482, 0x1e},
        {0x5483, 0x3b}, {0x5484, 0x58}, {0x5485, 0x66},
        {0x5486, 0x71}, {0x5487, 0x7d}, {0x5488, 0x83},
        {0x5489, 0x8f}, {0x548a, 0x98}, {0x548b, 0xa6},
        {0x548c, 0xb8}, {0x548d, 0xca}, {0x548e, 0xd7},
        {0x548f, 0xe3}, {0x5490, 0x1d},
        {0x5580, 0x06}, {0x5583, 0x40}, {0x5584, 0x10},
        {0x5586, 0x20}, {0x5587, 0x00}, {0x5588, 0x00},
        {0x5589, 0x10}, {0x558a, 0x00}, {0x558b, 0xf8},
        {0x501d, 0x40},
    };
    /* AEC/AGC ceiling values of the format table's main body. */
    static const uint8_t aec_regs[][2] = {
        {0x3a, 0x02}, {0x3a, 0x03},   /* max exposure = 0x03d8 lines */
        {0x3a, 0x14}, {0x3a, 0x15},
        {0x3a, 0x08}, {0x3a, 0x09},   /* max AGC = 0x0127 */
        {0x3a, 0x0a}, {0x3a, 0x0b},   /* min AGC = 0x00f6 */
        {0x3a, 0x0d}, {0x3a, 0x0e},   /* stability band 0x04/0x03 */
    };
    static const uint8_t aec_vals[] = {
        0x03, 0xd8, 0x03, 0xd8, 0x01, 0x27, 0x00, 0xf6, 0x04, 0x03,
    };
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "sensor workaround: main I2C bus is unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_dev_handle_t sccb = NULL;
    /* OV5640 SCCB 7-bit address. */
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x3C,
        .scl_speed_hz = 100000,
    };
    esp_err_t error = i2c_master_bus_add_device(bus, &dev_cfg, &sccb);
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
        (const uint8_t[]){0x50, 0x1f, 0x01}, 3, 100);
    if (error == ESP_OK) {
        error = i2c_master_transmit(sccb,
            (const uint8_t[]){0x43, 0x00, 0x61}, 3, 100);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor workaround: output format write failed: %s",
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    for (size_t i = 0; i < sizeof(aec_regs) / sizeof(aec_regs[0]); ++i) {
        error = i2c_master_transmit(sccb, (const uint8_t[]){
            aec_regs[i][0], aec_regs[i][1], aec_vals[i],
        }, 3, 100);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "sensor workaround: AEC write %u failed: %s",
                     (unsigned)i, esp_err_to_name(error));
            i2c_master_bus_rm_device(sccb);
            return error;
        }
    }
    uint8_t check = 0;
    uint8_t mux_check = 0;
    error = i2c_master_transmit_receive(sccb, (const uint8_t[]){0x43, 0x00},
                                         2, &check, 1, 100);
    if (error == ESP_OK) {
        error = i2c_master_transmit_receive(sccb,
                                            (const uint8_t[]){0x50, 0x1f},
                                            2, &mux_check, 1, 100);
    }
    if (error != ESP_OK || check != 0x61 || mux_check != 0x01) {
        ESP_LOGW(TAG,
                 "sensor workaround: readback failed 0x4300=0x%02x "
                 "0x501f=0x%02x error=%s",
                 check, mux_check, esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error != ESP_OK ? error : ESP_FAIL;
    }
    ESP_LOGI(TAG, "sensor workaround applied: 0x4300=0x%02x, "
             "%u ISP regs, AEC ceilings restored",
             check, (unsigned)(sizeof(isp_block) / sizeof(isp_block[0])));
    i2c_master_bus_rm_device(sccb);
    return ESP_OK;
}

esp_err_t bsp_camera_apply_workaround(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    return camera_sensor_workaround();
}

esp_err_t bsp_camera_start(const bsp_camera_cfg_t *cfg)
{
    (void)cfg;
    if (s_started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "main I2C init failed");
    ESP_RETURN_ON_ERROR(bsp_peripheral_power_set(BSP_PERIPHERAL_CAMERA, true),
                        TAG, "camera power-up failed");

    esp_err_t error = xclk_ledc_start();
    if (error != ESP_OK) {
        bsp_peripheral_power_set(BSP_PERIPHERAL_CAMERA, false);
        return error;
    }

    /* With LEDC XCLK (the only working option on S31, see the header comment)
     * the CAM controller must stay away from BSP_CAMERA_XCLK:
     * xclk_io=GPIO_NUM_NC keeps esp_cam_ctlr_dvp_init() from routing the
     * controller clock signal onto the pin, and xclk_freq=0 keeps
     * init_dvp_clk_func() from calling esp_cam_ctlr_dvp_output_clock(). */
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
            .data_io = {
                BSP_CAMERA_D0, BSP_CAMERA_D1, BSP_CAMERA_D2, BSP_CAMERA_D3,
                BSP_CAMERA_D4, BSP_CAMERA_D5, BSP_CAMERA_D6, BSP_CAMERA_D7,
            },
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
    /* This board has no autofocus hardware. The schematic carries no VCM
     * driver (no DW9714 or equivalent anywhere in the design), and the camera
     * FPC's AF_VCC pin is tied to the 2.8 V camera rail through a 0 ohm
     * resistor with no I2C control path. The OV5640 therefore runs fixed
     * focus: esp_video_init_config_t.cam_motor stays unset and only the DVP
     * video device is initialized below. */
    const esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };
    /* Only the DVP device is initialized; the plain esp_video_init() would
     * initialize every video device enabled in sdkconfig (ISP, JPEG, ...). */
    error = esp_video_init_with_flags(&video_config, ESP_VIDEO_INIT_FLAGS_DVP);
    if (error == ESP_OK) {
        const esp_err_t workaround_error = camera_sensor_workaround();
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
    const esp_err_t power_error =
        bsp_peripheral_power_set(BSP_PERIPHERAL_CAMERA, false);
    return result != ESP_OK ? result : power_error;
}
