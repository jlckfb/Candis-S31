/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/jpeg_encode.h"
#include "driver/cordic.h"
#include "driver/ppa.h"
#include "driver/bitscrambler_loopback.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include "factory_modules.h"
#include "factory_report.h"

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

#if SOC_JPEG_CODEC_SUPPORTED
#define JPEG_TEST_WIDTH  800
#define JPEG_TEST_HEIGHT 600

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

#if SOC_PPA_SUPPORTED
#define PPA_TEST_WIDTH   800
#define PPA_TEST_HEIGHT  600
#define PPA_BUFFER_ALIGN 64

static uint16_t ppa_test_pixel(uint32_t x, uint32_t y)
{
    const uint16_t red = (x * 31U) / PPA_TEST_WIDTH;
    const uint16_t green = (y * 63U) / PPA_TEST_HEIGHT;
    const uint16_t blue = ((x + y) * 31U) /
                          (PPA_TEST_WIDTH + PPA_TEST_HEIGHT);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static int command_ppa_srm_test(int argc, char **argv)
{
    uint32_t count = 10;
    if (argc > 2 ||
            (argc == 2 && !parse_accel_u32(argv[1], 1, 100, &count))) {
        printf("usage: ppa_srm_test [COUNT 1-100]\n");
        return ESP_ERR_INVALID_ARG;
    }

    const size_t input_size = PPA_TEST_WIDTH * PPA_TEST_HEIGHT *
                              sizeof(uint16_t);
    const size_t output_size = (input_size + PPA_BUFFER_ALIGN - 1) /
                               PPA_BUFFER_ALIGN * PPA_BUFFER_ALIGN;
    uint16_t *input = heap_caps_aligned_calloc(4, input_size,
                                               sizeof(uint8_t),
                                               MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_DMA);
    uint16_t *output = heap_caps_aligned_calloc(4, output_size,
                                                sizeof(uint8_t),
                                                MALLOC_CAP_SPIRAM |
                                                MALLOC_CAP_DMA);
    if (input == NULL || output == NULL) {
        heap_caps_free(input);
        heap_caps_free(output);
        factory_report_error(FACTORY_TEST_PPA, ESP_ERR_NO_MEM,
                             "ppa buffer alloc");
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t y = 0; y < PPA_TEST_HEIGHT; ++y) {
        for (uint32_t x = 0; x < PPA_TEST_WIDTH; ++x) {
            input[y * PPA_TEST_WIDTH + x] = ppa_test_pixel(x, y);
        }
    }

    ppa_client_handle_t client = NULL;
    const ppa_client_config_t client_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t result = ppa_register_client(&client_config, &client);
    const ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = input,
            .pic_w = PPA_TEST_WIDTH,
            .pic_h = PPA_TEST_HEIGHT,
            .block_w = PPA_TEST_WIDTH,
            .block_h = PPA_TEST_HEIGHT,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = output,
            .buffer_size = output_size,
            .pic_w = PPA_TEST_HEIGHT,
            .pic_h = PPA_TEST_WIDTH,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    const int64_t start_us = esp_timer_get_time();
    for (uint32_t index = 0; result == ESP_OK && index < count; ++index) {
        result = ppa_do_scale_rotate_mirror(client, &operation);
    }
    const int64_t total_us = esp_timer_get_time() - start_us;

    uint32_t mismatch_count = 0;
    for (uint32_t y = 0; result == ESP_OK && y < PPA_TEST_WIDTH; ++y) {
        for (uint32_t x = 0; x < PPA_TEST_HEIGHT; ++x) {
            const uint16_t expected = ppa_test_pixel(
                PPA_TEST_WIDTH - 1 - y, x);
            if (output[y * PPA_TEST_HEIGHT + x] != expected) {
                ++mismatch_count;
            }
        }
    }

    if (client != NULL) {
        const esp_err_t unregister_result = ppa_unregister_client(client);
        if (result == ESP_OK) {
            result = unregister_result;
        }
    }
    heap_caps_free(input);
    heap_caps_free(output);

    const uint64_t pixels = (uint64_t)count * PPA_TEST_WIDTH *
                            PPA_TEST_HEIGHT;
    const uint32_t average_us = count == 0 ? 0 :
        (uint32_t)(total_us / count);
    const uint32_t megapixels_per_second = total_us == 0 ? 0 :
        (uint32_t)(pixels * 1000000ULL / total_us / 1000000ULL);
    const bool passed = result == ESP_OK && mismatch_count == 0;

    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "800x600 rot90 count=%" PRIu32 " avg_us=%" PRIu32
             " mpps=%" PRIu32 " mismatch=%" PRIu32,
             count, average_us, megapixels_per_second, mismatch_count);
    factory_report_set(FACTORY_TEST_PPA,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_PPA);
    return passed ? ESP_OK : (result != ESP_OK ? result : ESP_FAIL);
}
#endif

#if SOC_CORDIC_SUPPORTED
static int command_cordic_test(int argc, char **argv)
{
    uint32_t count = 256;
    if (argc > 2 ||
            (argc == 2 && !parse_accel_u32(argv[1], 32, 4096, &count))) {
        printf("usage: cordic_test [COUNT 32-4096]\n");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t *input = malloc(count * sizeof(*input));
    uint32_t *cosine = malloc(count * sizeof(*cosine));
    uint32_t *sine = malloc(count * sizeof(*sine));
    if (input == NULL || cosine == NULL || sine == NULL) {
        free(input);
        free(cosine);
        free(sine);
        factory_report_error(FACTORY_TEST_CORDIC, ESP_ERR_NO_MEM,
                             "cordic buffer alloc");
        return ESP_ERR_NO_MEM;
    }

    const cordic_iq_format_t format = ESP_CORDIC_FORMAT_Q15;
    for (uint32_t index = 0; index < count; ++index) {
        const float value = -1.0f + (2.0f * index) / (count - 1);
        input[index] = cordic_convert_float_to_fixed(value, format);
    }

    cordic_engine_handle_t engine = NULL;
    const cordic_engine_config_t engine_config = {
        .clock_source = CORDIC_CLK_SRC_DEFAULT,
    };
    esp_err_t result = cordic_new_engine(&engine_config, &engine);
    const cordic_calculate_config_t calculate_config = {
        .function = ESP_CORDIC_FUNC_COS,
        .iq_format = format,
        .iteration_count = 4,
        .scale_exp = 0,
    };
    cordic_input_buffer_desc_t input_buffer = {
        .p_data_arg1 = input,
        .p_data_arg2 = NULL,
    };
    cordic_output_buffer_desc_t output_buffer = {
        .p_data_res1 = cosine,
        .p_data_res2 = sine,
    };

    const int64_t hardware_start = esp_timer_get_time();
    if (result == ESP_OK) {
        result = cordic_calculate_polling(engine, &calculate_config,
                                          &input_buffer, &output_buffer, count);
    }
    const int64_t hardware_us = esp_timer_get_time() - hardware_start;

    float max_cosine_error = 0.0f;
    float max_sine_error = 0.0f;
    volatile float software_sink = 0.0f;
    const int64_t software_start = esp_timer_get_time();
    for (uint32_t index = 0; index < count; ++index) {
        const float angle = cordic_convert_fixed_to_float(input[index], format) *
                            (float)M_PI;
        software_sink += cosf(angle) + sinf(angle);
    }
    const int64_t software_us = esp_timer_get_time() - software_start;

    for (uint32_t index = 0; result == ESP_OK && index < count; ++index) {
        const float angle = cordic_convert_fixed_to_float(input[index], format) *
                            (float)M_PI;
        const float hardware_cosine = cordic_convert_fixed_to_float(
            cosine[index] & UINT16_MAX, format);
        const float hardware_sine = cordic_convert_fixed_to_float(
            sine[index] & UINT16_MAX, format);
        const float cosine_error = fabsf(cosf(angle) - hardware_cosine);
        const float sine_error = fabsf(sinf(angle) - hardware_sine);
        if (cosine_error > max_cosine_error) {
            max_cosine_error = cosine_error;
        }
        if (sine_error > max_sine_error) {
            max_sine_error = sine_error;
        }
    }

    if (engine != NULL) {
        const esp_err_t delete_result = cordic_delete_engine(engine);
        if (result == ESP_OK) {
            result = delete_result;
        }
    }
    free(input);
    free(cosine);
    free(sine);

    const uint32_t max_error_milli = (uint32_t)(
        (max_cosine_error > max_sine_error ? max_cosine_error : max_sine_error) *
        1000.0f);
    const uint32_t hardware_ns_per_point = count == 0 ? 0 :
        (uint32_t)(hardware_us * 1000 / count);
    const uint32_t software_ns_per_point = count == 0 ? 0 :
        (uint32_t)(software_us * 1000 / count);
    const uint32_t speedup_x100 = hardware_us == 0 ? 0 :
        (uint32_t)(software_us * 100 / hardware_us);
    const bool passed = result == ESP_OK && max_error_milli <= 10;

    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "count=%" PRIu32 " hw_ns=%" PRIu32 " sw_ns=%" PRIu32
             " speed=%" PRIu32 ".%02" PRIu32 " err_milli=%" PRIu32,
             count, hardware_ns_per_point, software_ns_per_point,
             speedup_x100 / 100, speedup_x100 % 100, max_error_milli);
    factory_report_set(FACTORY_TEST_CORDIC,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_CORDIC);
    printf("cordic: software_sink=%f\n", (double)software_sink);
    return passed ? ESP_OK : (result != ESP_OK ? result : ESP_FAIL);
}
#endif

#if SOC_BITSCRAMBLER_SUPPORTED
#define BITSCRAMBLER_TEST_SIZE 4096

BITSCRAMBLER_PROGRAM(factory_bitscrambler_program, "factory_bitscrambler");

static uint8_t bitscrambler_input_byte(uint32_t index)
{
    const uint32_t block = index / 8;
    const uint32_t offset = index % 8;
    return (uint8_t)(0x01U << offset) ^ (uint8_t)block;
}

static uint8_t bitscrambler_expected_byte(const uint8_t *input, uint32_t index)
{
    const uint32_t block = index / 8;
    const uint32_t output_bit = index % 8;
    uint8_t result = 0;
    for (uint32_t input_byte = 0; input_byte < 8; ++input_byte) {
        if ((input[block * 8 + input_byte] & (1U << output_bit)) != 0) {
            result |= 1U << input_byte;
        }
    }
    return result;
}

static int command_bitscrambler_test(int argc, char **argv)
{
    uint32_t count = 10;
    if (argc > 2 ||
            (argc == 2 && !parse_accel_u32(argv[1], 1, 100, &count))) {
        printf("usage: bitscrambler_test [COUNT 1-100]\n");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *input = heap_caps_calloc(BITSCRAMBLER_TEST_SIZE,
                                      sizeof(uint8_t), MALLOC_CAP_DMA);
    uint8_t *output = heap_caps_calloc(BITSCRAMBLER_TEST_SIZE,
                                       sizeof(uint8_t), MALLOC_CAP_DMA);
    if (input == NULL || output == NULL) {
        heap_caps_free(input);
        heap_caps_free(output);
        factory_report_error(FACTORY_TEST_BITSCRAMBLER, ESP_ERR_NO_MEM,
                             "bitscrambler buffer alloc");
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t index = 0; index < BITSCRAMBLER_TEST_SIZE; ++index) {
        input[index] = bitscrambler_input_byte(index);
    }

    bitscrambler_handle_t bitscrambler = NULL;
    esp_err_t result = bitscrambler_loopback_create(
        &bitscrambler, SOC_BITSCRAMBLER_ATTACH_GPSPI2,
        BITSCRAMBLER_TEST_SIZE);
    if (result == ESP_OK) {
        result = bitscrambler_load_program(bitscrambler,
                                           factory_bitscrambler_program);
    }

    size_t output_size = 0;
    const int64_t start_us = esp_timer_get_time();
    for (uint32_t index = 0; result == ESP_OK && index < count; ++index) {
        output_size = 0;
        result = bitscrambler_loopback_run(
            bitscrambler, input, BITSCRAMBLER_TEST_SIZE,
            output, BITSCRAMBLER_TEST_SIZE, &output_size);
    }
    const int64_t total_us = esp_timer_get_time() - start_us;

    uint32_t mismatch_count = 0;
    if (result == ESP_OK && output_size == BITSCRAMBLER_TEST_SIZE) {
        for (uint32_t index = 0; index < BITSCRAMBLER_TEST_SIZE; ++index) {
            if (output[index] != bitscrambler_expected_byte(input, index)) {
                ++mismatch_count;
            }
        }
    } else if (result == ESP_OK) {
        result = ESP_ERR_INVALID_SIZE;
    }

    if (bitscrambler != NULL) {
        bitscrambler_free(bitscrambler);
    }
    heap_caps_free(input);
    heap_caps_free(output);

    const uint32_t average_us = count == 0 ? 0 :
        (uint32_t)(total_us / count);
    const uint32_t kib_per_second = total_us == 0 ? 0 :
        (uint32_t)((uint64_t)count * BITSCRAMBLER_TEST_SIZE * 1000000ULL /
                   total_us / 1024ULL);
    const bool passed = result == ESP_OK && mismatch_count == 0;

    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "4096B count=%" PRIu32 " avg_us=%" PRIu32
             " KiBps=%" PRIu32 " mismatch=%" PRIu32,
             count, average_us, kib_per_second, mismatch_count);
    factory_report_set(FACTORY_TEST_BITSCRAMBLER,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_BITSCRAMBLER);
    return passed ? ESP_OK : (result != ESP_OK ? result : ESP_FAIL);
}
#endif

esp_err_t factory_accel_register(void)
{
    esp_err_t error = ESP_OK;
#if SOC_JPEG_CODEC_SUPPORTED
    const esp_console_cmd_t jpeg_command = {
        .command = "jpeg_encode_test",
        .help = "Benchmark hardware JPEG encoding of a synthetic 800x600 RGB565 frame.",
        .func = command_jpeg_encode_test,
    };
    error = esp_console_cmd_register(&jpeg_command);
    if (error != ESP_OK) {
        return error;
    }
#endif
#if SOC_CORDIC_SUPPORTED
    const esp_console_cmd_t cordic_command = {
        .command = "cordic_test",
        .help = "Compare and benchmark the hardware CORDIC sine/cosine path.",
        .func = command_cordic_test,
    };
    error = esp_console_cmd_register(&cordic_command);
    if (error != ESP_OK) {
        return error;
    }
#endif
#if SOC_PPA_SUPPORTED
    const esp_console_cmd_t ppa_command = {
        .command = "ppa_srm_test",
        .help = "Benchmark a hardware PPA 90-degree RGB565 rotation.",
        .func = command_ppa_srm_test,
    };
    error = esp_console_cmd_register(&ppa_command);
    if (error != ESP_OK) {
        return error;
    }
#endif
#if SOC_BITSCRAMBLER_SUPPORTED
    const esp_console_cmd_t bitscrambler_command = {
        .command = "bitscrambler_test",
        .help = "Benchmark and verify a BitScrambler 64-bit transpose program.",
        .func = command_bitscrambler_test,
    };
    error = esp_console_cmd_register(&bitscrambler_command);
#endif
    return error;
}
