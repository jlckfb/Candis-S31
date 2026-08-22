/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_cam_sensor_xclk.h"
#include "esp_check.h"
#include "esp_video_init.h"

#include "bsp/candis_s31.h"

static const char *TAG = "candis_camera";
static bool s_started;
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC
static esp_cam_sensor_xclk_handle_t s_xclk;
#endif

/* The OV5640 register tables in esp_cam_sensor (sensors/ov5640) are all
 * calculated for a 24 MHz XCLK input ("24M input" in every format option);
 * any other frequency shifts frame rate and exposure timing.
 * BSP_CAMERA_XCLK_CLOCK_MHZ must stay 24. */
_Static_assert(BSP_CAMERA_XCLK_CLOCK_MHZ == 24,
               "OV5640 sensor register tables assume a 24 MHz XCLK");

/* XCLK comes from the CAM controller, not from LEDC.
 *
 * esp_video_init_with_flags() routes the DVP controller's own camera clock to
 * dvp_pin.xclk_io whenever xclk_io >= 0 && xclk_freq > 0: init_dvp_clk_func()
 * in esp_video_init.c calls esp_cam_ctlr_dvp_output_clock(), and the S31 CAM
 * controller divides that clock down from PLL_F160M itself. Because the GPIO
 * output matrix keeps only the signal attached last, an additional LEDC
 * channel on the same pin is disconnected in practice - it just consumes a
 * LEDC timer and channel. Measured on ESP32-S31 hardware (esp_video /
 * esp_cam_ctlr DVP path with an OV3660): the controller clock alone drives
 * the pin, and the earlier double-drive setup only went unnoticed because
 * both sources ran at 24 MHz.
 *
 * CONFIG_BSP_CAMERA_XCLK_USE_LEDC is therefore off by default and exists only
 * for diagnostics; the helpers below compile to no-ops when it is disabled. */
static esp_err_t xclk_ledc_start(void)
{
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC
    const esp_cam_sensor_xclk_config_t xclk_config = {
        .ledc_cfg = {
            .timer = LEDC_TIMER_1,
            .clk_cfg = LEDC_AUTO_CLK,
            .channel = CONFIG_BSP_CAMERA_XCLK_LEDC_CH,
            .xclk_freq_hz = BSP_CAMERA_XCLK_CLOCK_MHZ * 1000000,
            .xclk_pin = BSP_CAMERA_XCLK,
        },
    };
    ESP_RETURN_ON_ERROR(esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_LEDC,
                        &s_xclk),
                        TAG, "XCLK LEDC allocation failed");
    const esp_err_t error = esp_cam_sensor_xclk_start(s_xclk, &xclk_config);
    if (error != ESP_OK) {
        esp_cam_sensor_xclk_free(s_xclk);
        s_xclk = NULL;
    }
    return error;
#else
    return ESP_OK;
#endif
}

static esp_err_t xclk_ledc_stop(void)
{
#if CONFIG_BSP_CAMERA_XCLK_USE_LEDC
    if (s_xclk == NULL) {
        return ESP_OK;
    }
    esp_err_t result = esp_cam_sensor_xclk_stop(s_xclk);
    const esp_err_t free_error = esp_cam_sensor_xclk_free(s_xclk);
    if (result == ESP_OK) {
        result = free_error;
    }
    s_xclk = NULL;
    return result;
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

    /* xclk_io + xclk_freq below are what make the CAM controller output the
     * 24 MHz sensor clock on BSP_CAMERA_XCLK; both must stay set. */
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
            .xclk_io = BSP_CAMERA_XCLK,
        },
        .xclk_freq = BSP_CAMERA_XCLK_CLOCK_MHZ * 1000000,
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
