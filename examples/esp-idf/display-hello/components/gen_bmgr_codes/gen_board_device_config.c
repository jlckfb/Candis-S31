/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * Auto-generated device configuration file
 * DO NOT MODIFY THIS FILE MANUALLY
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include "esp_board_device.h"
#include "dev_audio_codec.h"
#include "dev_camera.h"
#include "dev_custom.h"
#include "dev_display_lcd.h"
#include "dev_fs_fat.h"
#include "dev_lcd_touch.h"
#include "dev_led_strip.h"
#include "dev_power_ctrl.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "gen_board_device_custom.h"

// Device configuration structures
const static dev_custom_pmic_config_t esp_bmgr_pmic_cfg = {
    .name = "pmic",
    .type = "custom",
    .chip = "tg28_sw",
    .i2c_address = 52,
    .frequency_hz = 400000,
    .peripheral_count = 2,
    .peripheral_names[0] = "i2c_low_power",
    .peripheral_names[1] = "gpio_pmic_rtc_interrupt",
};

const static dev_custom_rtc_config_t esp_bmgr_rtc_cfg = {
    .name = "rtc",
    .type = "custom",
    .chip = "rx8130ce",
    .i2c_address = 50,
    .frequency_hz = 400000,
    .peripheral_count = 2,
    .peripheral_names[0] = "i2c_low_power",
    .peripheral_names[1] = "gpio_pmic_rtc_interrupt",
};

static const char * esp_bmgr_board_power_ctrl_periph_names[] = {
    "gpio_display_vbat", "gpio_display_vci", "gpio_sd_power", "gpio_pa_control"
};

static dev_custom_board_power_ctrl_custom_config_t esp_bmgr_board_power_ctrl_custom_cfg = {
    .name = "board_power_ctrl_custom",
    .type = "custom",
    .chip = "unknown",
    .camera_reset_gpio = 39,
    .camera_pwdn_gpio = 40,
};

const static dev_power_ctrl_config_t esp_bmgr_board_power_ctrl_cfg = {
    .name = "board_power_ctrl",
    .sub_type = "custom",
    .sub_cfg = {
        .custom = {
            .periph_names = esp_bmgr_board_power_ctrl_periph_names,
            .periph_count = 4,
            .user_cfg = &esp_bmgr_board_power_ctrl_custom_cfg,
            .user_cfg_size = sizeof(esp_bmgr_board_power_ctrl_custom_cfg),
        },
    },
};

const static dev_led_strip_config_t esp_bmgr_led_strip_cfg = {
    .name = "led_strip",
    .chip = "ws2812",
    .sub_type = "rmt",
    .strip_config = {
        .strip_gpio_num = 4,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    },
    .sub_cfg = {
        .rmt = {
            .rmt_config = {
                .clk_src = RMT_CLK_SRC_DEFAULT,
                .resolution_hz = 10000000,
                .mem_block_symbols = 0,
                .flags = {
                    .with_dma = false,
                },
            },
        },
    },
};

const static dev_audio_codec_config_t esp_bmgr_audio_dac_cfg = {
    .name = "audio_dac",
    .chip = "es8389",
    .type = "audio_codec",
    .data_if_type = 0,
    .adc_enabled = false,
    .adc_max_channel = 0,
    .adc_channel_mask = 0x3,
    .adc_channel_labels = "",
    .adc_init_gain = 0,
    .dac_enabled = true,
    .dac_max_channel = 2,
    .dac_channel_mask = 0x3,
    .dac_init_gain = 0,
    .pa_cfg = {
        .name = "gpio_pa_control",
        .port = 42,
        .active_level = 1,
        .gain = 6.0,
    },
    .i2c_cfg = {
        .name = "i2c_main",
        .port = 0,
        .address = 32,
        .frequency = 400000,
    },
    .i2s_cfg = {
        .name = "i2s_audio_out",
        .port = 0,
        .clk_src = 0,
        .tx_aux_out_io = -1,
        .tx_aux_out_line = 0,
        .tx_aux_out_invert = false,
    },
    .adc_cfg = {
        .periph_name = NULL,
        .sample_rate_hz = 0,
        .max_store_buf_size = 0,
        .conv_frame_size = 0,
        .conv_mode = 0,
        .format = 0,
        .pattern_num = 0,
        .cfg_mode = 0,
        .cfg = {
            .single_unit = {
                .unit_id = 0,
                .atten = 0,
                .bit_width = 0,
                .channel_id = {},
            },
        },
    },
    .metadata = NULL,
    .metadata_size = 0,
    .mclk_enabled = true,
    .aec_enabled = false,
    .eq_enabled = false,
    .alc_enabled = false,
};

const static dev_audio_codec_config_t esp_bmgr_audio_adc_cfg = {
    .name = "audio_adc",
    .chip = "es8389",
    .type = "audio_codec",
    .data_if_type = 0,
    .adc_enabled = true,
    .adc_max_channel = 2,
    .adc_channel_mask = 0x3,
    .adc_channel_labels = "FR,FL",
    .adc_init_gain = 0,
    .dac_enabled = false,
    .dac_max_channel = 0,
    .dac_channel_mask = 0x0,
    .dac_init_gain = 0,
    .pa_cfg = {
        .name = NULL,
        .port = -1,
        .active_level = 0,
        .gain = 0.0,
    },
    .i2c_cfg = {
        .name = "i2c_main",
        .port = 0,
        .address = 32,
        .frequency = 400000,
    },
    .i2s_cfg = {
        .name = "i2s_audio_in",
        .port = 0,
        .clk_src = 0,
        .tx_aux_out_io = -1,
        .tx_aux_out_line = 0,
        .tx_aux_out_invert = false,
    },
    .adc_cfg = {
        .periph_name = NULL,
        .sample_rate_hz = 0,
        .max_store_buf_size = 0,
        .conv_frame_size = 0,
        .conv_mode = 0,
        .format = 0,
        .pattern_num = 0,
        .cfg_mode = 0,
        .cfg = {
            .single_unit = {
                .unit_id = 0,
                .atten = 0,
                .bit_width = 0,
                .channel_id = {},
            },
        },
    },
    .metadata = NULL,
    .metadata_size = 0,
    .mclk_enabled = true,
    .aec_enabled = false,
    .eq_enabled = false,
    .alc_enabled = false,
};

const static dev_fs_fat_config_t esp_bmgr_fs_sdcard_cfg = {
    .name = "fs_sdcard",
    .mount_point = "/sdcard",
    .frequency = SDMMC_FREQ_HIGHSPEED,
    .vfs_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16384,
    },
    .sub_type = "sdmmc",
    .sub_cfg = {
        .sdmmc = {
            .slot = SDMMC_HOST_SLOT_0,
            .bus_width = 4,
            .slot_flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
            .pins = {
                .clk = 24,
                .cmd = 25,
                .d0 = 20,
                .d1 = 21,
                .d2 = 22,
                .d3 = 23,
                .d4 = -1,
                .d5 = -1,
                .d6 = -1,
                .d7 = -1,
                .cd = 0,
                .wp = -1,
            },
            .ldo_chan_id = -1,
        },
    },
};

const static dev_display_lcd_config_t esp_bmgr_display_lcd_cfg = {
    .name = "display_lcd",
    .chip = "co5300",
    .sub_type = "spi",
    .lcd_width = 460,
    .lcd_height = 460,
    .swap_xy = false,
    .mirror_x = true,
    .mirror_y = true,
    .need_reset = true,
    .invert_color = false,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
    .sub_cfg = {
        .spi = {
            .spi_name = "spi_display",
            .panel_config = {
                .reset_gpio_num = 15,
                .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
                .bits_per_pixel = 16,
                .flags = {
                    .reset_active_high = false,
                },
                .vendor_config = "",
            },
            .io_spi_config = {
                .cs_gpio_num = 10,
                .dc_gpio_num = -1,
                .spi_mode = 0,
                .pclk_hz = 48000000,
                .trans_queue_depth = 10,
                .lcd_cmd_bits = 32,
                .lcd_param_bits = 8,
                .cs_ena_pretrans = 0,
                .cs_ena_posttrans = 0,
                .flags = {
                    .dc_high_on_cmd = false,
                    .dc_low_on_data = false,
                    .dc_low_on_param = false,
                    .octal_mode = false,
                    .quad_mode = true,
                    .sio_mode = false,
                    .lsb_first = false,
                    .cs_high_active = false,
                    .psram_dma_direct = false,
                },
            },
        },
    },
};

const static dev_lcd_touch_config_t esp_bmgr_lcd_touch_cfg = {
    .name = "lcd_touch",
    .chip = "cst820",
    .type = "lcd_touch",
    .sub_type = "i2c",
    .touch_config = {
        .x_max = 459,
        .y_max = 459,
        .rst_gpio_num = 17,
        .int_gpio_num = 3,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .process_coordinates = NULL,
        .interrupt_callback = NULL,
        .user_data = NULL,
        .driver_data = NULL,
    },
    .sub_cfg = {
        .i2c = {
            .i2c_name = "i2c_main",
            .i2c_addr_count = 1,
            .i2c_addr = {0x2a, 0x00, 0x00, 0x00},
            .io_i2c_config = {
                .dev_addr = 0,
                .control_phase_bytes = 1,
                .dc_bit_offset = 0,
                .lcd_cmd_bits = 8,
                .lcd_param_bits = 0,
                .scl_speed_hz = 400000,
                .flags = {
                    .dc_low_on_data = false,
                    .disable_control_phase = true,
                },
                .transaction_timeout_ms = 0,
            },
        },
    },
};

const static dev_camera_config_t esp_bmgr_camera_cfg = {
    .name = "camera",
    .type = "camera",
    .sub_type = "dvp",
    .sub_cfg = {
        .dvp = {
            .i2c_name = "i2c_main",
            .i2c_freq = 100000,
            .reset_io = 39,
            .pwdn_io = 40,
            .dvp_io = {
                .data_width = CAM_CTLR_DATA_WIDTH_8,
                .data_io = {46, 47, 48, 49, 50, 51, 52, 53, -1, -1, -1, -1, -1, -1, -1, -1},
                .vsync_io = 56,
                .de_io = 57,
                .pclk_io = 54,
                .xclk_io = 55,
            },
            .xclk_freq = 24000000,
        },
    },
};

// Device descriptor array
static const char* esp_bmgr_board_power_ctrl_deps[] = {
    "pmic",
};

const esp_board_device_desc_t g_esp_board_devices[] = {
    {
        .next = &g_esp_board_devices[1],
        .name = "pmic",
        .chip = "tg28_sw",
        .type = "custom",
        .sub_type = NULL,
        .cfg = &esp_bmgr_pmic_cfg,
        .cfg_size = sizeof(esp_bmgr_pmic_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[2],
        .name = "rtc",
        .chip = "rx8130ce",
        .type = "custom",
        .sub_type = NULL,
        .cfg = &esp_bmgr_rtc_cfg,
        .cfg_size = sizeof(esp_bmgr_rtc_cfg),
        .init_skip = false,
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[3],
        .name = "board_power_ctrl",
        .chip = NULL,
        .type = "power_ctrl",
        .sub_type = "custom",
        .cfg = &esp_bmgr_board_power_ctrl_cfg,
        .cfg_size = sizeof(esp_bmgr_board_power_ctrl_cfg),
        .init_skip = false,
        .depends_on = esp_bmgr_board_power_ctrl_deps,
        .depends_on_num = 1,
    },
    {
        .next = &g_esp_board_devices[4],
        .name = "led_strip",
        .chip = "ws2812",
        .type = "led_strip",
        .sub_type = "rmt",
        .cfg = &esp_bmgr_led_strip_cfg,
        .cfg_size = sizeof(esp_bmgr_led_strip_cfg),
        .init_skip = false,
        .power_ctrl_device = "board_power_ctrl",
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[5],
        .name = "audio_dac",
        .chip = "es8389",
        .type = "audio_codec",
        .sub_type = NULL,
        .cfg = &esp_bmgr_audio_dac_cfg,
        .cfg_size = sizeof(esp_bmgr_audio_dac_cfg),
        .init_skip = false,
        .power_ctrl_device = "board_power_ctrl",
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[6],
        .name = "audio_adc",
        .chip = "es8389",
        .type = "audio_codec",
        .sub_type = NULL,
        .cfg = &esp_bmgr_audio_adc_cfg,
        .cfg_size = sizeof(esp_bmgr_audio_adc_cfg),
        .init_skip = false,
        .power_ctrl_device = "board_power_ctrl",
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[7],
        .name = "fs_sdcard",
        .chip = NULL,
        .type = "fs_fat",
        .sub_type = "sdmmc",
        .cfg = &esp_bmgr_fs_sdcard_cfg,
        .cfg_size = sizeof(esp_bmgr_fs_sdcard_cfg),
        .init_skip = false,
        .power_ctrl_device = "board_power_ctrl",
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[8],
        .name = "display_lcd",
        .chip = "co5300",
        .type = "display_lcd",
        .sub_type = "spi",
        .cfg = &esp_bmgr_display_lcd_cfg,
        .cfg_size = sizeof(esp_bmgr_display_lcd_cfg),
        .init_skip = false,
        .power_ctrl_device = "board_power_ctrl",
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = &g_esp_board_devices[9],
        .name = "lcd_touch",
        .chip = "cst820",
        .type = "lcd_touch",
        .sub_type = "i2c",
        .cfg = &esp_bmgr_lcd_touch_cfg,
        .cfg_size = sizeof(esp_bmgr_lcd_touch_cfg),
        .init_skip = false,
        .power_ctrl_device = "board_power_ctrl",
        .depends_on = NULL,
        .depends_on_num = 0,
    },
    {
        .next = NULL,
        .name = "camera",
        .chip = NULL,
        .type = "camera",
        .sub_type = "dvp",
        .cfg = &esp_bmgr_camera_cfg,
        .cfg_size = sizeof(esp_bmgr_camera_cfg),
        .init_skip = true,
        .power_ctrl_device = "board_power_ctrl",
        .depends_on = NULL,
        .depends_on_num = 0,
    },
};
