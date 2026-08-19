/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/jpeg_encode.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include "factory_modules.h"
#include "factory_report.h"

#if SOC_JPEG_CODEC_SUPPORTED
#define JPEG_TEST_WIDTH  800
#define JPEG_TEST_HEIGHT 600

static bool parse_accel_u32(const char *text, uint32_t minimum, uint32_t maximum,
                            uint32_t *value)
{
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static int command_jpeg_encode_test(int argc, char **argv)
{
    uint32_t count = 10;
    uint32_t quality = 80;
    if (argc > 3 ||
            (argc >= 2 && !parse_accel_u32(argv[1], 1, 100, &count)) ||
            (argc >= 3 && !parse_accel_u32(argv[2], 1, 100, &quality))) {
        printf("usage: jpeg_encode_test [COUNT 1-100] [QUALITY 1-100]\n");
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t input_size = JPEG_TEST_WIDTH * JPEG_TEST_HEIGHT * 2;
    const jpeg_encode_memory_alloc_cfg_t input_alloc = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    const jpeg_encode_memory_alloc_cfg_t output_alloc = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t input_allocated = 0;
    size_t output_allocated = 0;
    uint8_t *input = jpeg_alloc_encoder_mem(input_size, &input_alloc,
                                             &input_allocated);
    uint8_t *output = jpeg_alloc_encoder_mem(input_size, &output_alloc,
                                              &output_allocated);
    if (input == NULL || output == NULL) {
        heap_caps_free(input);
        heap_caps_free(output);
        factory_report_error(FACTORY_TEST_JPEG, ESP_ERR_NO_MEM,
                             "jpeg buffer alloc");
        return ESP_ERR_NO_MEM;
    }

    uint16_t *pixels = (uint16_t *)input;
    for (uint32_t y = 0; y < JPEG_TEST_HEIGHT; ++y) {
        for (uint32_t x = 0; x < JPEG_TEST_WIDTH; ++x) {
            const uint16_t red = (x * 31U) / JPEG_TEST_WIDTH;
            const uint16_t green = (y * 63U) / JPEG_TEST_HEIGHT;
            const uint16_t blue = ((x + y) * 31U) /
                                  (JPEG_TEST_WIDTH + JPEG_TEST_HEIGHT);
            pixels[y * JPEG_TEST_WIDTH + x] =
                (uint16_t)((red << 11) | (green << 5) | blue);
        }
    }

    jpeg_encoder_handle_t encoder = NULL;
    const jpeg_encode_engine_cfg_t engine_config = {
        .timeout_ms = 500,
    };
    esp_err_t result = jpeg_new_encoder_engine(&engine_config, &encoder);
    const jpeg_encode_cfg_t encode_config = {
        .width = JPEG_TEST_WIDTH,
        .height = JPEG_TEST_HEIGHT,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = quality,
        .pixel_reverse = false,
    };
    uint32_t output_size = 0;
    int64_t total_us = 0;
    for (uint32_t index = 0; result == ESP_OK && index < count; ++index) {
        const int64_t start_us = esp_timer_get_time();
        result = jpeg_encoder_process(encoder, &encode_config, input,
                                       input_allocated, output,
                                       output_allocated, &output_size);
        total_us += esp_timer_get_time() - start_us;
    }

    const bool markers_ok = result == ESP_OK && output_size >= 2 &&
                            output[0] == 0xff && output[1] == 0xd8 &&
                            output[output_size - 2] == 0xff &&
                            output[output_size - 1] == 0xd9;
    const bool passed = result == ESP_OK && markers_ok &&
                        output_size > 0 && output_size < input_size;
    if (encoder != NULL) {
        const esp_err_t delete_result = jpeg_del_encoder_engine(encoder);
        if (result == ESP_OK) {
            result = delete_result;
        }
    }
    heap_caps_free(input);
    heap_caps_free(output);

    const uint32_t average_us = count == 0 ? 0 :
        (uint32_t)(total_us / count);
    const uint32_t fps = average_us == 0 ? 0 : 1000000U / average_us;
    const uint32_t ratio_pct = output_size == 0 ? 0 :
        output_size * 100U / input_size;
    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "800x600 count=%" PRIu32 " avg_us=%" PRIu32
             " fps=%" PRIu32 " out=%" PRIu32 " ratio=%" PRIu32 "%%",
             count, average_us, fps, output_size, ratio_pct);
    factory_report_set(FACTORY_TEST_JPEG,
                       passed && result == ESP_OK ? FACTORY_STATUS_PASS :
                                                    FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_JPEG);
    return passed && result == ESP_OK ? ESP_OK :
           (result != ESP_OK ? result : ESP_FAIL);
}
#endif

esp_err_t factory_accel_register(void)
{
#if SOC_JPEG_CODEC_SUPPORTED
    const esp_console_cmd_t command = {
        .command = "jpeg_encode_test",
        .help = "Benchmark hardware JPEG encoding of a synthetic 800x600 RGB565 frame.",
        .func = command_jpeg_encode_test,
    };
    return esp_console_cmd_register(&command);
#else
    return ESP_OK;
#endif
}
