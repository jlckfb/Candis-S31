/*
 * Candis-S31 watch demo - ACCEL domain test suite (spec D.1).
 *
 * Ports the five factory hardware-accelerator checks (factory_accel.c):
 * JPEG encode :43, CORDIC sin/cos :318, PPA rotate-90 :199 (pattern
 * :190), BitScrambler 64-bit transpose :461, ASRC 16k->48k :542. Each
 * test keeps its `#if SOC_*_SUPPORTED` guard (factory_accel.c:715-769);
 * buffers are allocated from PSRAM (aligned) and freed on every exit
 * path. All functions run on the svc_test task; no LVGL calls (C.7).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_encode.h"
#include "driver/cordic.h"
#include "driver/ppa.h"
#include "driver/bitscrambler_loopback.h"
#include "esp_asrc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include "test_registry.h"

/* Fixed run parameters (factory defaults; the console arguments collapse
 * to these constants for the UI one-shot runs). */
#define ACCEL_RUN_COUNT   10
#define JPEG_QUALITY      80
#define JPEG_TEST_WIDTH   800
#define JPEG_TEST_HEIGHT  600
#define CORDIC_RUN_COUNT  256
#define PPA_TEST_WIDTH    800
#define PPA_TEST_HEIGHT   600
#define PPA_BUFFER_ALIGN  64
#define BITSCRAMBLER_TEST_SIZE 4096

static void result_fail(test_result_t *out, const char *evidence)
{
    out->st = TEST_ST_FAIL;
    strlcpy(out->evidence, evidence, sizeof(out->evidence));
}

/* ------------------------------------------------------------------ */
/* accel.jpeg (factory_accel.c:43)                                     */
/* ------------------------------------------------------------------ */

#if SOC_JPEG_CODEC_SUPPORTED
static void run_jpeg(const test_ctx_t *ctx, test_result_t *out)
{
    const uint32_t width = JPEG_TEST_WIDTH;
    const uint32_t height = JPEG_TEST_HEIGHT;
    const uint32_t bytes_per_pixel = 2;
    const uint32_t input_size = width * height * bytes_per_pixel;
    const size_t input_allocated = (input_size + 63U) / 64U * 64U;
    const size_t output_allocated = input_allocated;
    uint8_t *input = heap_caps_aligned_calloc(64, 1, input_allocated,
                                              MALLOC_CAP_SPIRAM);
    uint8_t *output = heap_caps_aligned_calloc(64, 1, output_allocated,
                                               MALLOC_CAP_SPIRAM);
    if (input == NULL || output == NULL) {
        heap_caps_free(input);
        heap_caps_free(output);
        result_fail(out, "jpeg buffer alloc failed");
        return;
    }

    /* Synthetic gradient frame, RGB565 big-endian like the factory. */
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint16_t red = (x * 31U) / width;
            const uint16_t green = (y * 63U) / height;
            const uint16_t blue = ((x + y) * 31U) / (width + height);
            const uint16_t pixel = (uint16_t)((red << 11) | (green << 5) |
                                              blue);
            const uint32_t offset = (y * width + x) * bytes_per_pixel;
            input[offset] = (uint8_t)(pixel >> 8);
            input[offset + 1] = (uint8_t)pixel;
        }
    }

    jpeg_encoder_handle_t encoder = NULL;
    const jpeg_encode_engine_cfg_t engine_config = {
        .timeout_ms = 500,
    };
    esp_err_t result = jpeg_new_encoder_engine(&engine_config, &encoder);
    const jpeg_encode_cfg_t encode_config = {
        .width = width,
        .height = height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = JPEG_QUALITY,
        .pixel_reverse = false,
    };
    uint32_t output_size = 0;
    int64_t total_us = 0;
    for (uint32_t index = 0;
         result == ESP_OK && index < ACCEL_RUN_COUNT &&
         !ctx->cancel_requested(ctx);
         ++index) {
        const int64_t start_us = esp_timer_get_time();
        result = jpeg_encoder_process(encoder, &encode_config, input,
                                      input_allocated, output,
                                      output_allocated, &output_size);
        total_us += esp_timer_get_time() - start_us;
        ctx->progress(ctx, 5 + (int)(index * 90 / ACCEL_RUN_COUNT),
                      "encoding");
    }

    const bool markers_ok = result == ESP_OK && output_size >= 2 &&
                            output[0] == 0xff && output[1] == 0xd8 &&
                            output[output_size - 2] == 0xff &&
                            output[output_size - 1] == 0xd9;
    const bool passed = result == ESP_OK && markers_ok &&
                        output_size > 0 && output_size < input_size;
    if (encoder != NULL) {
        jpeg_del_encoder_engine(encoder);
    }
    heap_caps_free(input);
    heap_caps_free(output);

    if (ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records aborted */
    }
    const uint32_t average_us =
        (uint32_t)(total_us / ACCEL_RUN_COUNT);
    const uint32_t fps = average_us == 0 ? 0 : 1000000U / average_us;
    const uint32_t ratio_pct = output_size == 0 ? 0 :
                               output_size * 100U / input_size;
    snprintf(out->evidence, sizeof(out->evidence),
             "%ux%u rgb565 avg_us=%" PRIu32 " fps=%" PRIu32
             " out=%" PRIu32 " ratio=%" PRIu32 "%%",
             (unsigned)width, (unsigned)height, average_us, fps,
             output_size, ratio_pct);
    out->st = passed ? TEST_ST_PASS : TEST_ST_FAIL;
}
#endif /* SOC_JPEG_CODEC_SUPPORTED */

/* ------------------------------------------------------------------ */
/* accel.cordic (factory_accel.c:318)                                  */
/* ------------------------------------------------------------------ */

#if SOC_CORDIC_SUPPORTED
static void run_cordic(const test_ctx_t *ctx, test_result_t *out)
{
    const uint32_t count = CORDIC_RUN_COUNT;
    uint32_t *input = malloc(count * sizeof(*input));
    uint32_t *cosine = malloc(count * sizeof(*cosine));
    uint32_t *sine = malloc(count * sizeof(*sine));
    if (input == NULL || cosine == NULL || sine == NULL) {
        free(input);
        free(cosine);
        free(sine);
        result_fail(out, "cordic buffer alloc failed");
        return;
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
                                          &input_buffer, &output_buffer,
                                          count);
    }
    const int64_t hardware_us = esp_timer_get_time() - hardware_start;

    float max_cosine_error = 0.0f;
    float max_sine_error = 0.0f;
    volatile float software_sink = 0.0f;
    const int64_t software_start = esp_timer_get_time();
    for (uint32_t index = 0; index < count; ++index) {
        const float angle =
            cordic_convert_fixed_to_float(input[index], format) *
            (float)M_PI;
        software_sink += cosf(angle) + sinf(angle);
    }
    const int64_t software_us = esp_timer_get_time() - software_start;

    for (uint32_t index = 0; result == ESP_OK && index < count; ++index) {
        const float angle =
            cordic_convert_fixed_to_float(input[index], format) *
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
    (void)software_sink;

    if (engine != NULL) {
        cordic_delete_engine(engine);
    }
    free(input);
    free(cosine);
    free(sine);

    const uint32_t max_error_milli = (uint32_t)(
        (max_cosine_error > max_sine_error ? max_cosine_error
                                           : max_sine_error) * 1000.0f);
    const uint32_t hardware_ns_per_point =
        (uint32_t)(hardware_us * 1000 / count);
    const uint32_t speedup_x100 = hardware_us == 0 ? 0 :
        (uint32_t)(software_us * 100 / hardware_us);
    snprintf(out->evidence, sizeof(out->evidence),
             "hw_ns=%" PRIu32 " speed=%" PRIu32 ".%02" PRIu32
             " err_milli=%" PRIu32,
             hardware_ns_per_point, speedup_x100 / 100, speedup_x100 % 100,
             max_error_milli);
    out->st = (result == ESP_OK && max_error_milli <= 10) ? TEST_ST_PASS
                                                          : TEST_ST_FAIL;
}
#endif /* SOC_CORDIC_SUPPORTED */

/* ------------------------------------------------------------------ */
/* accel.ppa (factory_accel.c:190-317) - 90-degree RGB565 rotation     */
/* ------------------------------------------------------------------ */

#if SOC_PPA_SUPPORTED
static uint16_t ppa_test_pixel(uint32_t x, uint32_t y)
{
    const uint16_t red = (x * 31U) / PPA_TEST_WIDTH;
    const uint16_t green = (y * 63U) / PPA_TEST_HEIGHT;
    const uint16_t blue = ((x + y) * 31U) /
                          (PPA_TEST_WIDTH + PPA_TEST_HEIGHT);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static void run_ppa(const test_ctx_t *ctx, test_result_t *out)
{
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
        result_fail(out, "ppa buffer alloc failed");
        return;
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
    for (uint32_t index = 0;
         result == ESP_OK && index < ACCEL_RUN_COUNT &&
         !ctx->cancel_requested(ctx);
         ++index) {
        result = ppa_do_scale_rotate_mirror(client, &operation);
        ctx->progress(ctx, 5 + (int)(index * 80 / ACCEL_RUN_COUNT),
                      "rotating");
    }
    const int64_t total_us = esp_timer_get_time() - start_us;

    uint32_t mismatch_count = 0;
    for (uint32_t y = 0; result == ESP_OK && y < PPA_TEST_WIDTH; ++y) {
        for (uint32_t x = 0; x < PPA_TEST_HEIGHT; ++x) {
            const uint16_t expected =
                ppa_test_pixel(PPA_TEST_WIDTH - 1 - y, x);
            if (output[y * PPA_TEST_HEIGHT + x] != expected) {
                ++mismatch_count;
            }
        }
    }

    if (client != NULL) {
        ppa_unregister_client(client);
    }
    heap_caps_free(input);
    heap_caps_free(output);

    if (ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records aborted */
    }
    const uint64_t pixels = (uint64_t)ACCEL_RUN_COUNT * PPA_TEST_WIDTH *
                            PPA_TEST_HEIGHT;
    const uint32_t average_us = (uint32_t)(total_us / ACCEL_RUN_COUNT);
    const uint32_t mpps = total_us == 0 ? 0 :
        (uint32_t)(pixels * 1000000ULL / total_us / 1000000ULL);
    snprintf(out->evidence, sizeof(out->evidence),
             "800x600 rot90 avg_us=%" PRIu32 " mpps=%" PRIu32
             " mismatch=%" PRIu32,
             average_us, mpps, mismatch_count);
    out->st = (result == ESP_OK && mismatch_count == 0) ? TEST_ST_PASS
                                                        : TEST_ST_FAIL;
}
#endif /* SOC_PPA_SUPPORTED */

/* ------------------------------------------------------------------ */
/* accel.bitscrambler (factory_accel.c:461) + factory_bitscrambler.bsasm */
/* ------------------------------------------------------------------ */

#if SOC_BITSCRAMBLER_SUPPORTED
BITSCRAMBLER_PROGRAM(factory_bitscrambler_program, "factory_bitscrambler");

static uint8_t bitscrambler_input_byte(uint32_t index)
{
    const uint32_t block = index / 8;
    const uint32_t offset = index % 8;
    return (uint8_t)(0x01U << offset) ^ (uint8_t)block;
}

static uint8_t bitscrambler_expected_byte(const uint8_t *input,
                                          uint32_t index)
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

static void run_bitscrambler(const test_ctx_t *ctx, test_result_t *out)
{
    uint8_t *input = heap_caps_calloc(BITSCRAMBLER_TEST_SIZE,
                                      sizeof(uint8_t), MALLOC_CAP_DMA);
    uint8_t *output = heap_caps_calloc(BITSCRAMBLER_TEST_SIZE,
                                       sizeof(uint8_t), MALLOC_CAP_DMA);
    if (input == NULL || output == NULL) {
        heap_caps_free(input);
        heap_caps_free(output);
        result_fail(out, "bitscrambler buffer alloc failed");
        return;
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
    for (uint32_t index = 0;
         result == ESP_OK && index < ACCEL_RUN_COUNT &&
         !ctx->cancel_requested(ctx);
         ++index) {
        output_size = 0;
        result = bitscrambler_loopback_run(
            bitscrambler, input, BITSCRAMBLER_TEST_SIZE,
            output, BITSCRAMBLER_TEST_SIZE, &output_size);
        ctx->progress(ctx, 5 + (int)(index * 80 / ACCEL_RUN_COUNT),
                      "transposing");
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

    if (ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records aborted */
    }
    const uint32_t average_us = (uint32_t)(total_us / ACCEL_RUN_COUNT);
    const uint32_t kib_per_second = total_us == 0 ? 0 :
        (uint32_t)((uint64_t)ACCEL_RUN_COUNT * BITSCRAMBLER_TEST_SIZE *
                   1000000ULL / total_us / 1024ULL);
    snprintf(out->evidence, sizeof(out->evidence),
             "4096B avg_us=%" PRIu32 " KiBps=%" PRIu32
             " mismatch=%" PRIu32,
             average_us, kib_per_second, mismatch_count);
    out->st = (result == ESP_OK && mismatch_count == 0) ? TEST_ST_PASS
                                                        : TEST_ST_FAIL;
}
#endif /* SOC_BITSCRAMBLER_SUPPORTED */

/* ------------------------------------------------------------------ */
/* accel.asrc (factory_accel.c:542) - 16 kHz mono to 48 kHz stereo     */
/* ------------------------------------------------------------------ */

#if SOC_ASRC_SUPPORTED
static void run_asrc(const test_ctx_t *ctx, test_result_t *out)
{
    esp_asrc_handle_t asrc = NULL;
    float weight[2] = {1.0f, 1.0f};
    const esp_asrc_cfg_t config = {
        .src_info = {
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
        .dest_info = {
            .sample_rate = 48000,
            .channel = 2,
            .bits_per_sample = 16,
        },
        .weight = weight,
        .weight_len = 2,
        .perf_type = ESP_ASRC_PERF_TYPE_HW_ONLY,
        .complexity = 1,
        .timeout_ms = 1000,
    };
    esp_asrc_err_t result = esp_asrc_open((esp_asrc_cfg_t *)&config, &asrc);
    if (result != ESP_ASRC_ERR_OK || asrc == NULL) {
        result_fail(out, "asrc open failed");
        return;
    }

    uint16_t in_sample_bytes = 0;
    uint16_t out_sample_bytes = 0;
    uint32_t expected_out_samples = 0;
    result = esp_asrc_get_bytes_per_sample(asrc, &in_sample_bytes,
                                           &out_sample_bytes);
    if (result == ESP_ASRC_ERR_OK) {
        result = esp_asrc_get_out_sample_num(asrc, 480,
                                             &expected_out_samples);
    }

    esp_asrc_buffer_alignment_t alignment = {0};
    if (result == ESP_ASRC_ERR_OK) {
        result = esp_asrc_get_buffer_alignment(&alignment);
    }

    uint32_t allocated_in_size = 0;
    uint32_t allocated_out_size = 0;
    uint8_t *input = NULL;
    uint8_t *output = NULL;
    if (result == ESP_ASRC_ERR_OK) {
        input = esp_asrc_align_alloc(480 * in_sample_bytes,
                                     alignment.inbuf_addr_align,
                                     alignment.inbuf_size_align,
                                     &allocated_in_size);
        output = esp_asrc_align_alloc(expected_out_samples * out_sample_bytes,
                                      alignment.outbuf_addr_align,
                                      alignment.outbuf_size_align,
                                      &allocated_out_size);
        if (input == NULL || output == NULL) {
            result = ESP_ASRC_ERR_MEM_LACK;
        }
    }

    if (result == ESP_ASRC_ERR_OK) {
        int16_t *samples = (int16_t *)input;
        for (uint32_t index = 0; index < 480; ++index) {
            samples[index] = 0x4000;
        }
    }

    uint32_t mismatch_count = 0;
    uint32_t output_samples = 0;
    const int64_t start_us = esp_timer_get_time();
    for (uint32_t index = 0;
         result == ESP_ASRC_ERR_OK && index < ACCEL_RUN_COUNT &&
         !ctx->cancel_requested(ctx);
         ++index) {
        output_samples = allocated_out_size / out_sample_bytes;
        result = esp_asrc_process(asrc, input, 480, output,
                                  &output_samples);
        if (result == ESP_ASRC_ERR_OK) {
            const int16_t *stereo = (const int16_t *)output;
            for (uint32_t sample = 0; sample < output_samples; ++sample) {
                if (stereo[sample * 2] != stereo[sample * 2 + 1]) {
                    ++mismatch_count;
                }
            }
        }
        ctx->progress(ctx, 5 + (int)(index * 80 / ACCEL_RUN_COUNT),
                      "converting");
    }
    const int64_t total_us = esp_timer_get_time() - start_us;

    free(input);
    free(output);
    esp_asrc_close(asrc);

    if (ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records aborted */
    }
    const uint32_t average_us = (uint32_t)(total_us / ACCEL_RUN_COUNT);
    const bool passed = result == ESP_ASRC_ERR_OK &&
                        mismatch_count == 0 && output_samples > 0;
    snprintf(out->evidence, sizeof(out->evidence),
             "16k1->48k2 avg_us=%" PRIu32 " out=%" PRIu32
             " mismatch=%" PRIu32,
             average_us, output_samples, mismatch_count);
    out->st = passed ? TEST_ST_PASS : TEST_ST_FAIL;
}
#endif /* SOC_ASRC_SUPPORTED */

/* ------------------------------------------------------------------ */
/* Registration (guards mirror factory_accel.c:715-769)                */
/* ------------------------------------------------------------------ */

void test_accel_register(void)
{
#if SOC_JPEG_CODEC_SUPPORTED
    static const test_case_t jpeg_case = {
        .id = "accel.jpeg", .name = "HW JPEG encode",
        .domain = TEST_DOM_ACCEL, .flags = 0,
        .timeout_ms = 30000, .run = run_jpeg,
    };
    test_register(&jpeg_case);
#endif
#if SOC_CORDIC_SUPPORTED
    static const test_case_t cordic_case = {
        .id = "accel.cordic", .name = "CORDIC sin/cos",
        .domain = TEST_DOM_ACCEL, .flags = 0,
        .timeout_ms = 15000, .run = run_cordic,
    };
    test_register(&cordic_case);
#endif
#if SOC_PPA_SUPPORTED
    static const test_case_t ppa_case = {
        .id = "accel.ppa", .name = "PPA rotate 90",
        .domain = TEST_DOM_ACCEL, .flags = 0,
        .timeout_ms = 30000, .run = run_ppa,
    };
    test_register(&ppa_case);
#endif
#if SOC_BITSCRAMBLER_SUPPORTED
    static const test_case_t bitscrambler_case = {
        .id = "accel.bitscrambler", .name = "BitScrambler xpose",
        .domain = TEST_DOM_ACCEL, .flags = 0,
        .timeout_ms = 30000, .run = run_bitscrambler,
    };
    test_register(&bitscrambler_case);
#endif
#if SOC_ASRC_SUPPORTED
    static const test_case_t asrc_case = {
        .id = "accel.asrc", .name = "ASRC 16k to 48k",
        .domain = TEST_DOM_ACCEL, .flags = 0,
        .timeout_ms = 30000, .run = run_asrc,
    };
    test_register(&asrc_case);
#endif
}
