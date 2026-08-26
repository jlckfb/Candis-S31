/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "driver/ledc.h"
#include "soc/ledc_struct.h"
#include "esp_video_init.h"

#include "bsp/candis_s31.h"

static const char *TAG = "candis_camera";
static bool s_started;
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC

#endif

/* The OV5640 register tables in esp_cam_sensor (sensors/ov5640) are all
 * calculated for a 24 MHz XCLK input ("24M input" in every format option);
 * any other frequency shifts frame rate and exposure timing.
 * must stay 24: the OV5640 register tables are tuned for a 24 MHz input. */
_Static_assert(BSP_CAMERA_XCLK_CLOCK_MHZ == 24,
               "OV5640 sensor register tables assume a 24 MHz XCLK");

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
    /* Direct LEDC configuration instead of the esp_cam_sensor helper.
     * Measured on ESP32-S31 (PCNT): the PLL_DIV source delivers 62.5 MHz
     * while the driver computes the divider against an assumed 80 MHz, so a
     * plain 24 MHz request lands at 18.74 MHz. Requesting 30.72 MHz makes
     * the driver pick div = 80e6/(30.72e6*2) = 1.302083 -> clk_div 333,
     * which the real source turns into 62.5e6/(333/256*2) = 24.02 MHz
     * (+0.1%) at an exact 50% duty. */
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
