/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * Auto-generated device handle definition file
 * DO NOT MODIFY THIS FILE MANUALLY
 *
 * See LICENSE file for details.
 */

#include <stddef.h>
#include "esp_board_device.h"
#include "dev_audio_codec.h"
#include "dev_camera.h"
#include "dev_custom.h"
#include "dev_display_lcd.h"
#include "dev_fs_fat.h"
#include "dev_lcd_touch.h"
#include "dev_led_strip.h"
#include "dev_power_ctrl.h"

// Device handle array
esp_board_device_handle_t g_esp_board_device_handles[] = {
    {
        .next = &g_esp_board_device_handles[1],
        .name = "pmic",
        .chip = "tg28_sw",
        .type = "custom",
        .device_handle = NULL,
        .init = dev_custom_init,
        .deinit = dev_custom_deinit
    },
    {
        .next = &g_esp_board_device_handles[2],
        .name = "rtc",
        .chip = "rx8130ce",
        .type = "custom",
        .device_handle = NULL,
        .init = dev_custom_init,
        .deinit = dev_custom_deinit
    },
    {
        .next = &g_esp_board_device_handles[3],
        .name = "board_power_ctrl",
        .chip = NULL,
        .type = "power_ctrl",
        .device_handle = NULL,
        .init = dev_power_ctrl_init,
        .deinit = dev_power_ctrl_deinit
    },
    {
        .next = &g_esp_board_device_handles[4],
        .name = "led_strip",
        .chip = "ws2812",
        .type = "led_strip",
        .device_handle = NULL,
        .init = dev_led_strip_init,
        .deinit = dev_led_strip_deinit
    },
    {
        .next = &g_esp_board_device_handles[5],
        .name = "audio_dac",
        .chip = "es8389",
        .type = "audio_codec",
        .device_handle = NULL,
        .init = dev_audio_codec_init,
        .deinit = dev_audio_codec_deinit
    },
    {
        .next = &g_esp_board_device_handles[6],
        .name = "audio_adc",
        .chip = "es8389",
        .type = "audio_codec",
        .device_handle = NULL,
        .init = dev_audio_codec_init,
        .deinit = dev_audio_codec_deinit
    },
    {
        .next = &g_esp_board_device_handles[7],
        .name = "fs_sdcard",
        .chip = NULL,
        .type = "fs_fat",
        .device_handle = NULL,
        .init = dev_fs_fat_init,
        .deinit = dev_fs_fat_deinit
    },
    {
        .next = &g_esp_board_device_handles[8],
        .name = "display_lcd",
        .chip = "co5300",
        .type = "display_lcd",
        .device_handle = NULL,
        .init = dev_display_lcd_init,
        .deinit = dev_display_lcd_deinit
    },
    {
        .next = &g_esp_board_device_handles[9],
        .name = "lcd_touch",
        .chip = "cst820",
        .type = "lcd_touch",
        .device_handle = NULL,
        .init = dev_lcd_touch_init,
        .deinit = dev_lcd_touch_deinit
    },
    {
        .next = NULL,
        .name = "camera",
        .chip = NULL,
        .type = "camera",
        .device_handle = NULL,
        .init = dev_camera_init,
        .deinit = dev_camera_deinit
    },
};
