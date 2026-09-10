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
#include "linux/videodev2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_BSP_CAMERA_CORE_120MHZ
#include "soc/hp_sys_clkrst_struct.h"
#include "esp_private/periph_ctrl.h"
#endif

#include "bsp/candis_s31.h"

static const char *TAG = "candis_camera";
static bool s_started;
#ifndef CONFIG_BSP_CAMERA_SKIP_ISP_PROFILE
#define CONFIG_BSP_CAMERA_SKIP_ISP_PROFILE 0
#endif

/* Human-readable DVDD source for the startup summary line. */
#if CONFIG_BSP_CAMERA_USE_INTERNAL_DVDD
#define BSP_CAMERA_DVDD_MODE_NAME "internal"
#else
#define BSP_CAMERA_DVDD_MODE_NAME "external"
#endif

#if CONFIG_BSP_CAMERA_CORE_120MHZ
static uint32_t s_core_clock_divider;
static bool s_core_clock_saved;

static esp_err_t camera_core_clock_enable(void)
{
    bool supported = false;
    /* IDF esp32s31 clk_gate_ll.h selects source 1: BBPLL 120 MHz / 2.
     * Change only its integer divider; never reinterpret an unknown source. */
    PERIPH_RCC_ATOMIC() {
        if (HP_SYS_CLKRST.lcdcam_lcdcam_ctrl0.reg_lcdcam_clk_src_sel == 1 &&
                HP_SYS_CLKRST.lcdcam_lcdcam_ctrl0.reg_lcdcam_clk_div_numerator == 0) {
            s_core_clock_divider = HP_SYS_CLKRST.lcdcam_lcdcam_ctrl0.reg_lcdcam_clk_div_num;
            s_core_clock_saved = true;
            HP_SYS_CLKRST.lcdcam_lcdcam_ctrl0.reg_lcdcam_clk_div_num = 0;
            supported = true;
        }
    }
    ESP_LOGD(TAG, "LCDCAM core clock: %s source=BBPLL120 divider=%u->1",
             supported ? "120MHz" : "unsupported", (unsigned)s_core_clock_divider + 1U);
    return supported ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static void camera_core_clock_restore(void)
{
    if (s_core_clock_saved) {
        PERIPH_RCC_ATOMIC() {
            HP_SYS_CLKRST.lcdcam_lcdcam_ctrl0.reg_lcdcam_clk_div_num = s_core_clock_divider;
        }
        s_core_clock_saved = false;
    }
}
#endif

/* EVT1's measured-good source clock is an exact 20 MHz, 50 percent
 * duty-cycle signal generated from XTAL-backed LEDC. The post-format sensor
 * profile derives a 80 MHz DVP byte clock from that input.
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
#if CONFIG_BSP_CAMERA_TIMING_60MHZ
#define OV5640_PREVIEW_HTS          2000U
#define OV5640_PREVIEW_PCLK_HZ      60000000U
#define OV5640_PREVIEW_PLL_MULT     60U
#elif CONFIG_BSP_CAMERA_TIMING_MATCHED80
#define OV5640_PREVIEW_HTS          2666U
#define OV5640_PREVIEW_PCLK_HZ      80000000U
#define OV5640_PREVIEW_PLL_MULT     80U
#elif CONFIG_BSP_CAMERA_TIMING_15FPS
#define OV5640_PREVIEW_HTS          2060U
#define OV5640_PREVIEW_PCLK_HZ      80000000U
#define OV5640_PREVIEW_PLL_MULT     80U
#else
#define OV5640_PREVIEW_HTS          2060U
#define OV5640_PREVIEW_PCLK_HZ      80000000U
#define OV5640_PREVIEW_PLL_MULT     80U
#endif
#if CONFIG_BSP_CAMERA_TIMING_15FPS
#define OV5640_PREVIEW_VTS          2460U
#define OV5640_PREVIEW_AEC_MAX      2456U
#else
#define OV5640_PREVIEW_VTS          0x03D8U
#define OV5640_PREVIEW_AEC_MAX      0x03D4U
#endif
#define OV5640_PREVIEW_BAND50       (OV5640_PREVIEW_PCLK_HZ / (OV5640_PREVIEW_HTS * 100U))
#define OV5640_PREVIEW_BAND60       (OV5640_PREVIEW_PCLK_HZ / (OV5640_PREVIEW_HTS * 120U))
#if !CONFIG_BSP_CAMERA_TIMING_15FPS
_Static_assert(OV5640_PREVIEW_PCLK_HZ >= 30U * OV5640_PREVIEW_HTS * OV5640_PREVIEW_VTS,
               "OV5640 preview timing must target at least 30 fps");
#endif

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

/* Board-level profile for the OV5640 module on the EVT1 FPC.
 * It is applied before and after the first video-device open:
 * esp_video_open() lazily runs dvp_video_init(), whose format-table write
 * resets these registers after the pre-open pass. Three facts established by
 * analyzing frames captured from this board over the serial log:
 *
 * 1. The esp_cam_sensor option/table is named "RGB565_BE" but writes
 *    FORMAT_CTRL0 0x4300 = 0x6F. That upstream name is inverted relative
 *    to OV5640/Linux semantics: 0x6F is RGB565 LE, while 0x61 is RGB565
 *    BE. Do not change the verified 0x61 override back to 0x6F based only
 *    on the esp_cam_sensor symbol name.
 *
 *    These are RGB565 byte-order encodings, not a Bayer/ISP selector.
 *    FORMAT_MUX 0x501F selects the processing output; the profile uses
 *    0x01 for RGB. Verified source frames carry RGB565 bits [15:8]
 *    first and [7:0] second, matching V4L2 RGB565X byte order.
 * 2. Keep the board's measured AWB/CMX baseline and module trim. For live view,
 *    the upstream esp_cam_sensor automatic CIP/gamma/SDE profile replaces
 *    forced manual edge enhancement and non-vendor thresholds that can erase
 *    fine texture before the host receives a frame.
 * 3. Use the SVGA blanking and clock dividers of this board.
 *    A frame must include the binned source readout, not only the 600
 *    output rows.
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
        {0x5000, 0xa7}, {0x5001, 0xa3}, {0x5003, 0x08},
        {0x5180, 0xff}, {0x5181, 0xf2}, {0x5182, 0x00},
        {0x5180, 0xff}, {0x5181, 0xf2}, {0x5182, 0x00},
        {0x5183, 0x14}, {0x5184, 0x25}, {0x5185, 0x24},
        {0x5183, 0x14}, {0x5184, 0x25}, {0x5185, 0x24},
        {0x5186, 0x09}, {0x5187, 0x09}, {0x5188, 0x09},
        {0x5186, 0x09}, {0x5187, 0x09}, {0x5188, 0x09},
        {0x5189, 0x75}, {0x518a, 0x54}, {0x518b, 0xe0},
        {0x5189, 0x75}, {0x518a, 0x54}, {0x518b, 0xe0},
        {0x518c, 0xb2}, {0x518d, 0x42}, {0x518e, 0x3d},
        {0x518c, 0xb2}, {0x518d, 0x42}, {0x518e, 0x3d},
        {0x518f, 0x56}, {0x5190, 0x46}, {0x5191, 0xf8},
        {0x518f, 0x56}, {0x5190, 0x46}, {0x5191, 0xf8},
        {0x5192, 0x04}, {0x5193, 0x70}, {0x5194, 0xf0},
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
     * Both profiles keep the 20 MHz XCLK, 10-bit PLL mode, manual PCLK
     * divider and VTS=984 for the complete 972-line binned readout.
     * The normal profile is PCLK80/HTS2060; the optional PCLK60/HTS2000
     * profile targets 30.488 fps with additional receiver clock margin.
     * Exposure band steps follow the selected line time, not the table name.
     */
    static const uint8_t timing_regs[][2] = {
        {0x38, 0x0c}, {0x38, 0x0d},
        {0x38, 0x0e}, {0x38, 0x0f},
        {0x3a, 0x02}, {0x3a, 0x03},
        {0x3a, 0x14}, {0x3a, 0x15},
        {0x3a, 0x08}, {0x3a, 0x09},
        {0x3a, 0x0a}, {0x3a, 0x0b},
        {0x3a, 0x0d}, {0x3a, 0x0e},
        {0x30, 0x39}, {0x38, 0x24}, {0x46, 0x0c},
    };
    static const uint8_t timing_vals[] = {
        OV5640_PREVIEW_HTS >> 8, OV5640_PREVIEW_HTS & 0xff,
        OV5640_PREVIEW_VTS >> 8, OV5640_PREVIEW_VTS & 0xff,
        OV5640_PREVIEW_AEC_MAX >> 8, OV5640_PREVIEW_AEC_MAX & 0xff,
        OV5640_PREVIEW_AEC_MAX >> 8, OV5640_PREVIEW_AEC_MAX & 0xff,
        OV5640_PREVIEW_BAND50 >> 8, OV5640_PREVIEW_BAND50 & 0xff,
        OV5640_PREVIEW_BAND60 >> 8, OV5640_PREVIEW_BAND60 & 0xff,
        OV5640_PREVIEW_AEC_MAX / OV5640_PREVIEW_BAND60,
        OV5640_PREVIEW_AEC_MAX / OV5640_PREVIEW_BAND50,
        0x00, 0x02, 0x22,
    };
    _Static_assert(sizeof(timing_vals) ==
                   sizeof(timing_regs) / sizeof(timing_regs[0]),
                   "OV5640 timing register/value count mismatch");
    static const uint8_t pll_bit_divider = 0x1a;
    static const uint8_t pll_system_divider = 0x11;
    static const uint8_t pll_multiplier = OV5640_PREVIEW_PLL_MULT;
#if CONFIG_BSP_CAMERA_STANDARD_CLOCK_ROOTS
    /* OV5640 v2.33 Figure 2-3 / Table 2-3 recommends 0x3108=0x01.
     * Halve PLL root output and halve all three downstream divisors as a
     * pair: SCLK, SCLK2x and PCLK retain their existing frequencies. */
    static const uint8_t pll_pre_root_divider = 0x12;
    static const uint8_t system_root_dividers = 0x01;
#else
    static const uint8_t pll_pre_root_divider = 0x02;
    static const uint8_t system_root_dividers = 0x16;
#endif
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
    /* The reset PLL2 defaults produce 200 MHz from a 24 MHz XVCLK, but
     * only 166.7 MHz from this board's 20 MHz source. OV5640 v2.33
     * Figure 2-3 specifies 190-230 MHz for PLLADCLK. Keep the known
     * divider path and raise only the PLL2 multiplier from 25 to 30. */
    uint8_t pll2[4] = {0xff, 0xff, 0xff, 0xff};
    for (unsigned i = 0; error == ESP_OK && i < sizeof(pll2); ++i) {
        error = camera_sensor_read8(sccb, 0x303a + i, &pll2[i]);
    }
    ESP_LOGD(TAG, "sensor ADC PLL2 before=%02x/%02x/%02x/%02x",
             pll2[0], pll2[1], pll2[2], pll2[3]);
    if (error == ESP_OK && ((pll2[0] & 0x80U) != 0 ||
            (pll2[2] & 0x0fU) != 1 || (pll2[3] & 0x37U) != 0x30)) {
        error = ESP_ERR_NOT_SUPPORTED;
    }
    const uint8_t adc_multiplier = (pll2[1] & 0xe0U) | 30U;
    if (error == ESP_OK) {
        error = camera_sensor_write8(sccb, 0x303b, adc_multiplier);
    }
    uint8_t adc_multiplier_check = 0xff;
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x303b, &adc_multiplier_check);
        if (error == ESP_OK && adc_multiplier_check != adc_multiplier) {
            error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "sensor ADC clock setup failed: %s", esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    ESP_LOGD(TAG, "sensor ADC clock=200MHz PLL2 multiplier=%u->%u",
             pll2[1] & 0x1fU, adc_multiplier_check & 0x1fU);
#if !CONFIG_BSP_CAMERA_SKIP_ISP_PROFILE
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
#if CONFIG_BSP_CAMERA_CMX_COLOR_TRIM
    /* Empirical module trim: scale the magnitudes in the red/blue input
     * columns without writing manual AWB gains or disabling automatic AWB.
     * Integer coefficients, clipping and subsequent ISP processing mean
     * this is not an exact end-to-end RGB gain or an absolute calibration.
     * Signs stay in 0x538A/0x538B and are not touched here. */
    static const uint8_t cmx_base[9] = {
        0x1e, 0x5b, 0x08, 0x0a, 0x7e, 0x88, 0x7c, 0x6c, 0x10,
    };
    static const uint16_t cmx_percent[9] = {
        CONFIG_BSP_CAMERA_CMX_RED_PERCENT, 100, CONFIG_BSP_CAMERA_CMX_BLUE_PERCENT,
        CONFIG_BSP_CAMERA_CMX_RED_PERCENT, 100, CONFIG_BSP_CAMERA_CMX_BLUE_PERCENT,
        CONFIG_BSP_CAMERA_CMX_RED_PERCENT, 100, CONFIG_BSP_CAMERA_CMX_BLUE_PERCENT,
    };
    for (size_t i = 0; error == ESP_OK && i < 9U; ++i) {
        uint32_t value = (uint32_t)cmx_base[i] * cmx_percent[i] / 100U;
        error = i2c_master_transmit(sccb, (const uint8_t[]){
            (uint8_t)((0x5381U + i) >> 8), (uint8_t)(0x5381U + i),
            (uint8_t)(value > 0xffU ? 0xffU : value),
        }, 3, 100);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "sensor workaround: colour-matrix trim failed: %s",
                 esp_err_to_name(error));
        i2c_master_bus_rm_device(sccb);
        return error;
    }
#endif
#endif
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
    /* OV5640 v2.33 documents only bit 1 here as the system clock selector.
     * Retain the official table's other bits instead of forcing legacy 0x13. */
    uint8_t clock_select_before = 0;
    uint8_t clock_select_after = 0;
    error = camera_sensor_read8(sccb, 0x3103, &clock_select_before);
    if (error == ESP_OK) {
        error = camera_sensor_write8(sccb, 0x3103, clock_select_before | 0x02U);
    }
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x3103, &clock_select_after);
        if (error == ESP_OK && clock_select_after != (clock_select_before | 0x02U)) {
            error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_LOGD(TAG, "sensor clock select reg=3103 before=%02x after=%02x error=%s",
             clock_select_before, clock_select_after, esp_err_to_name(error));
    if (error != ESP_OK) {
        i2c_master_bus_rm_device(sccb);
        return error;
    }
    for (size_t i = 0; i < sizeof(timing_vals); ++i) {
        const uint16_t reg = (uint16_t)timing_regs[i][0] << 8 | timing_regs[i][1];
        uint8_t actual = 0xff;
        error = camera_sensor_read8(sccb, reg, &actual);
        if (error != ESP_OK || actual != timing_vals[i]) {
            ESP_LOGE(TAG, "sensor timing readback failed reg=%04x expected=%02x actual=%02x error=%s",
                     reg, timing_vals[i], actual, esp_err_to_name(error));
            i2c_master_bus_rm_device(sccb);
            return error != ESP_OK ? error : ESP_ERR_INVALID_RESPONSE;
        }
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
#if !CONFIG_BSP_CAMERA_SKIP_ISP_PROFILE && CONFIG_BSP_CAMERA_CMX_COLOR_TRIM
    if (error == ESP_OK) {
        uint8_t cmx_read = 0;
        for (size_t i = 0; error == ESP_OK && i < 9U; ++i) {
            uint32_t expected = (uint32_t)cmx_base[i] * cmx_percent[i] / 100U;
            error = camera_sensor_read8(sccb, (uint16_t)(0x5381U + i),
                                        &cmx_read);
            if (error == ESP_OK &&
                    cmx_read != (uint8_t)(expected > 0xffU ? 0xffU : expected)) {
                ESP_LOGW(TAG, "sensor workaround: CMX 0x%04x readback %02x "
                         "expected %02x", (unsigned)(0x5381U + i), cmx_read,
                         (unsigned)expected);
                error = ESP_FAIL;
            }
        }
    }
#endif
    if (error == ESP_OK) {
        error = camera_sensor_read8(sccb, 0x4300, &check);
    }
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
            (!CONFIG_BSP_CAMERA_SKIP_ISP_PROFILE && cip_control != 0x25)) {
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
    ESP_LOGD(TAG, "camera profile: 800x600 pclk=%uMHz hts=%u vts=%u "
             "%.3ffps rgb565x 0x%02x/0x%02x dvdd=%s",
             OV5640_PREVIEW_PCLK_HZ / 1000000U, OV5640_PREVIEW_HTS,
             OV5640_PREVIEW_VTS,
             (double)OV5640_PREVIEW_PCLK_HZ /
             (double)(OV5640_PREVIEW_HTS * OV5640_PREVIEW_VTS),
             check, mux_check, expected_mode);
    ESP_LOGD(TAG,
             "sensor workaround applied: 0x3031=0x%02x output=0x%02x/0x%02x "
             "pll=0x%02x/0x%02x/0x%02x/0x%02x/0x%02x "
             "hts=0x%04x vts=0x%04x aec=0x%04x cip=0x%02x "
             "isp_profile=%s isp_regs=%u",
             power_control_check, check, mux_check, pll_bit_divider_check,
             pll_system_divider_check, pll_multiplier_check,
             pll_pre_root_divider_check, system_root_dividers_check,
             hts_check, vts_check, aec_check, cip_control,
             CONFIG_BSP_CAMERA_SKIP_ISP_PROFILE ? "official" : "candis",
             (unsigned)(CONFIG_BSP_CAMERA_SKIP_ISP_PROFILE ? 0 :
                        sizeof(isp_block) / sizeof(isp_block[0])));
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
            /* Keep nonstandard-adapter rewiring opt-in. Applying a legacy
             * permutation to a standard module corrupts every pixel byte. */
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
    const esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };
    /* Only the DVP device is initialized; the plain esp_video_init() would
     * initialize every video device enabled in sdkconfig (ISP, JPEG, ...). */
    error = esp_video_init_with_flags(&video_config, ESP_VIDEO_INIT_FLAGS_DVP);
#if CONFIG_BSP_CAMERA_CORE_120MHZ
    if (error == ESP_OK) {
        error = camera_core_clock_enable();
        if (error != ESP_OK) {
            esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_DVP);
        }
    }
#endif
    if (error == ESP_OK) {
        const esp_err_t workaround_error =
            camera_sensor_workaround(V4L2_PIX_FMT_RGB565X);
        if (workaround_error != ESP_OK) {
            ESP_LOGW(TAG, "pre-open sensor workaround failed: %s",
                     esp_err_to_name(workaround_error));
        }
        ESP_LOGD(TAG, "camera initialized: 800x600 RGB565X pclk=%uMHz hts=%u vts=%u "
                 "%.1ffps dvdd=%s; post-open profile required",
                 OV5640_PREVIEW_PCLK_HZ / 1000000U, OV5640_PREVIEW_HTS,
                 OV5640_PREVIEW_VTS,
                 (double)OV5640_PREVIEW_PCLK_HZ /
                 (double)(OV5640_PREVIEW_HTS * OV5640_PREVIEW_VTS),
                 BSP_CAMERA_DVDD_MODE_NAME);
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
#if CONFIG_BSP_CAMERA_CORE_120MHZ
    if (result == ESP_OK) {
        camera_core_clock_restore();
    }
#endif
    const esp_err_t xclk_error = xclk_ledc_stop();
    if (result == ESP_OK) {
        result = xclk_error;
    }
    s_started = false;
    const esp_err_t power_error =
        bsp_peripheral_power_set(BSP_PERIPHERAL_CAMERA, false);
    return result != ESP_OK ? result : power_error;
}
