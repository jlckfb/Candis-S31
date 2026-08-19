/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

/* Follow the BSP instead of pinning a rate here: the default must stay a row of
 * the es8389 driver's coeff_div[] table, and the BSP owns that decision. */
#define AUDIO_SAMPLE_RATE         BSP_I2S_SAMPLE_RATE
#define AUDIO_FRAME_COUNT         512

static esp_codec_dev_handle_t s_speaker;
static esp_codec_dev_handle_t s_microphone;

static esp_codec_dev_handle_t audio_device(bool speaker)
{
    esp_codec_dev_handle_t *handle = speaker ? &s_speaker : &s_microphone;
    if (*handle == NULL) {
        *handle = speaker ? bsp_audio_codec_speaker_init() :
                            bsp_audio_codec_microphone_init();
    }
    return *handle;
}

static esp_codec_dev_sample_info_t audio_format(void)
{
    return (esp_codec_dev_sample_info_t) {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
}

static int command_speaker_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_codec_dev_handle_t speaker = audio_device(true);
    if (speaker == NULL) {
        factory_report_error(FACTORY_TEST_SPEAKER, ESP_FAIL, "speaker codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t format = audio_format();
    int result = esp_codec_dev_open(speaker, &format);
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_out_vol(speaker, 30);
    }
    static int16_t samples[AUDIO_FRAME_COUNT * 2];
    for (unsigned frame = 0; frame < AUDIO_FRAME_COUNT; ++frame) {
        const int16_t value = (frame % (AUDIO_SAMPLE_RATE / 880)) <
                              (AUDIO_SAMPLE_RATE / 1760) ? 2200 : -2200;
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    for (unsigned block = 0; result == ESP_CODEC_DEV_OK && block < 65; ++block) {
        result = esp_codec_dev_write(speaker, samples, sizeof(samples));
    }
    esp_codec_dev_close(speaker);
    if (result != ESP_CODEC_DEV_OK) {
        factory_report_error(FACTORY_TEST_SPEAKER, result, "speaker write failed");
        return result;
    }
    const char answer = factory_console_ask_operator(
                            "speaker", "Did you hear the tone?",
                            OPERATOR_PROMPT_TIMEOUT_S);
    factory_report_operator_verdict(FACTORY_TEST_SPEAKER, answer,
                            "operator confirmed the tone",
                            "operator did not hear the tone",
                            "tone sent; confirm sound then use mark");
    return ESP_OK;
}

static int command_microphone_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_codec_dev_handle_t microphone = audio_device(false);
    if (microphone == NULL) {
        factory_report_error(FACTORY_TEST_MICROPHONE, ESP_FAIL, "microphone codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t format = audio_format();
    int result = esp_codec_dev_open(microphone, &format);
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_in_gain(microphone, 24.0f);
    }
    static int16_t samples[AUDIO_FRAME_COUNT * 2];
    uint16_t peak = 0;
    for (unsigned block = 0; result == ESP_CODEC_DEV_OK && block < 20; ++block) {
        result = esp_codec_dev_read(microphone, samples, sizeof(samples));
        for (unsigned index = 0; index < sizeof(samples) / sizeof(samples[0]); ++index) {
            const int32_t value = samples[index] < 0 ? -(int32_t)samples[index] : samples[index];
            if (value > peak) {
                peak = (uint16_t)value;
            }
        }
    }
    esp_codec_dev_close(microphone);
    const bool passed = result == ESP_CODEC_DEV_OK && peak > 64;
    char detail[96];
    snprintf(detail, sizeof(detail), "peak=%u", peak);
    factory_report_set(FACTORY_TEST_MICROPHONE,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_MICROPHONE);
    return passed ? ESP_OK : (result != ESP_CODEC_DEV_OK ? result : ESP_FAIL);
}


bool factory_audio_busy(void)
{
    return s_speaker != NULL || s_microphone != NULL;
}

esp_err_t factory_audio_stop(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_speaker != NULL) {
        const esp_err_t error = bsp_audio_codec_deinit(s_speaker);
        if (error == ESP_OK) {
            s_speaker = NULL;
        } else if (first_error == ESP_OK) {
            first_error = error;
        }
    }
    if (s_microphone != NULL) {
        const esp_err_t error = bsp_audio_codec_deinit(s_microphone);
        if (error == ESP_OK) {
            s_microphone = NULL;
        } else if (first_error == ESP_OK) {
            first_error = error;
        }
    }
    return first_error;
}

esp_err_t factory_audio_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "speaker_test", .help = "Play a short low-level square-wave tone.", .func = command_speaker_test},
        {.command = "microphone_test", .help = "Capture audio and check for a non-zero signal.", .func = command_microphone_test},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
