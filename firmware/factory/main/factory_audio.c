/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

/* Follow the BSP instead of pinning a rate here: the default must stay a row of
 * the es8389 driver's coeff_div[] table, and the BSP owns that decision. */
#define AUDIO_SAMPLE_RATE         BSP_I2S_SAMPLE_RATE
#define AUDIO_FRAME_COUNT         512
#define ES8389_I2C_ADDRESS        0x10

static bool parse_audio_u32(const char *text, uint32_t minimum, uint32_t maximum,
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

static esp_err_t codec_write_input_route(uint8_t input_select)
{
    esp_err_t result = bsp_i2c_init();
    if (result != ESP_OK) {
        return result;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8389_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t device = NULL;
    result = i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &device);
    if (result == ESP_OK) {
        const uint8_t reg = 0x72;
        uint8_t current = 0;
        result = i2c_master_transmit_receive(device, &reg, sizeof(reg),
                                             &current, sizeof(current), 200);
        if (result == ESP_OK) {
            const uint8_t payload[] = {
                reg,
                (uint8_t)((current & 0x0F) | input_select),
            };
            result = i2c_master_transmit(device, payload, sizeof(payload), 200);
        }
        i2c_master_bus_rm_device(device);
    }
    return result;
}

esp_err_t factory_audio_set_input_route(const char *route)
{
    if (route == NULL || strcmp(route, "default") == 0) {
        return ESP_OK;
    }
    if (strcmp(route, "mic2_left") == 0) {
        return codec_write_input_route(0x60);
    }
    if (strcmp(route, "mic1_single_left") == 0) {
        return codec_write_input_route(0x50);
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_codec_dev_handle_t s_speaker;
static esp_codec_dev_handle_t s_microphone;

static esp_codec_dev_handle_t audio_device(bool speaker);

#define ES8389_I2C_ADDR 0x10 /* 7-bit; 0x20 is the 8-bit write address */

static esp_err_t es8389_raw_read(i2c_master_dev_handle_t dev, uint8_t reg,
                                 uint8_t *value)
{
    return i2c_master_transmit_receive(dev, &reg, 1, value, 1, 200);
}

static int command_codec_reg(int argc, char **argv)
{
    if (argc > 3) {
        printf("usage: codec_reg [REG_HEX [VALUE_HEX]] - no args dumps key registers\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (bsp_i2c_init() != ESP_OK) {
        printf("codec_reg: i2c init failed\n");
        return ESP_FAIL;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8389_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t error = i2c_master_bus_add_device(bsp_i2c_get_handle(), &dev_cfg, &dev);
    if (error != ESP_OK) {
        printf("codec_reg: add device failed: %s\n", esp_err_to_name(error));
        return error;
    }
    int result = ESP_OK;
    if (argc == 2 && (strcmp(argv[1], "open") == 0 || strcmp(argv[1], "close") == 0)) {
        i2c_master_bus_rm_device(dev);
        esp_codec_dev_handle_t mic = audio_device(false);
        if (mic == NULL) {
            printf("codec_reg: microphone codec init failed\n");
            return ESP_FAIL;
        }
        if (argv[1][0] == 'o') {
            esp_codec_dev_sample_info_t format = {
                .bits_per_sample = 16,
                .channel = 2,
                .sample_rate = AUDIO_SAMPLE_RATE,
            };
            result = esp_codec_dev_open(mic, &format);
        } else {
            result = esp_codec_dev_close(mic);
        }
        printf("codec_reg %s: %s\n", argv[1], esp_err_to_name(result));
        return result;
    }
    if (argc == 1) {
        static const uint8_t keys[] = {
            0x00, 0x01, 0x02, 0x03, 0x20, 0x23, 0x24, 0x25, 0x2A, 0x2F, 0x31,
            0x60, 0x61, 0x64, 0x6D, 0x6E, 0x72, 0x73, 0xF0, 0xF1, 0xF2, 0xF3,
        };
        for (size_t i = 0; i < sizeof(keys); ++i) {
            uint8_t value = 0;
            error = es8389_raw_read(dev, keys[i], &value);
            if (error != ESP_OK) {
                printf("reg 0x%02x read failed: %s\n", keys[i], esp_err_to_name(error));
                result = error;
                break;
            }
            printf("reg 0x%02x = 0x%02x\n", keys[i], value);
        }
    } else {
        const uint32_t reg = strtoul(argv[1], NULL, 0);
        if (reg > 0xFF) { result = ESP_ERR_INVALID_ARG; goto done; }
        if (argc == 3) {
            const uint32_t value = strtoul(argv[2], NULL, 0);
            if (value > 0xFF) { result = ESP_ERR_INVALID_ARG; goto done; }
            const uint8_t payload[2] = {(uint8_t)reg, (uint8_t)value};
            error = i2c_master_transmit(dev, payload, sizeof(payload), 200);
            printf("codec_reg: write 0x%02x <- 0x%02x %s\n", (unsigned)reg, (unsigned)value,
                   error == ESP_OK ? "OK" : "FAIL");
            result = error;
        }
        uint8_t value = 0;
        error = es8389_raw_read(dev, (uint8_t)reg, &value);
        if (error == ESP_OK) {
            printf("reg 0x%02x = 0x%02x\n", (unsigned)reg, value);
        } else {
            printf("codec_reg: read 0x%02x failed: %s\n", (unsigned)reg, esp_err_to_name(error));
            result = error;
        }
    }
done:
    i2c_master_bus_rm_device(dev);
    return result;
}

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
    uint32_t frequency_hz = 880;
    uint32_t volume = 20;
    uint32_t duration_ms = 2000;
    if (argc > 4 ||
            (argc >= 2 && !parse_audio_u32(argv[1], 100, 8000, &frequency_hz)) ||
            (argc >= 3 && !parse_audio_u32(argv[2], 0, 100, &volume)) ||
            (argc >= 4 && !parse_audio_u32(argv[3], 100, 10000, &duration_ms))) {
        printf("usage: speaker_test [FREQ_HZ 100-8000] [VOLUME 0-100] [DURATION_MS 100-10000]\n");
        return ESP_ERR_INVALID_ARG;
    }

    esp_codec_dev_handle_t speaker = audio_device(true);
    if (speaker == NULL) {
        factory_report_error(FACTORY_TEST_SPEAKER, ESP_FAIL, "speaker codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t format = audio_format();
    int result = esp_codec_dev_open(speaker, &format);
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_out_vol(speaker, volume);
    }
    static int16_t samples[AUDIO_FRAME_COUNT * 2];
    const uint32_t period_samples = AUDIO_SAMPLE_RATE / frequency_hz;
    const uint32_t high_samples = period_samples / 2;
    for (unsigned frame = 0; frame < AUDIO_FRAME_COUNT; ++frame) {
        const int16_t value = frame % period_samples < high_samples ? 2200 : -2200;
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    const uint32_t block_count = (duration_ms * AUDIO_SAMPLE_RATE + 999) / 1000 /
                                  AUDIO_FRAME_COUNT;
    for (unsigned block = 0; result == ESP_CODEC_DEV_OK && block < block_count; ++block) {
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
    uint32_t duration_ms = 640;
    uint32_t gain_db = 24;
    const char *route = "default";
    if (argc > 4 ||
            (argc >= 2 && !parse_audio_u32(argv[1], 100, 10000, &duration_ms)) ||
            (argc >= 3 && !parse_audio_u32(argv[2], 0, 48, &gain_db))) {
        printf("usage: microphone_test [DURATION_MS 100-10000] [GAIN_DB 0-48] "
               "[default|mic2_left|mic1_single_left]\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (argc >= 4) {
        route = argv[3];
        if (strcmp(route, "default") != 0 && strcmp(route, "mic2_left") != 0 &&
                strcmp(route, "mic1_single_left") != 0) {
            printf("usage: microphone_test [DURATION_MS 100-10000] "
                   "[GAIN_DB 0-48] [default|mic2_left|mic1_single_left]\n");
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_codec_dev_handle_t microphone = audio_device(false);
    if (microphone == NULL) {
        factory_report_error(FACTORY_TEST_MICROPHONE, ESP_FAIL, "microphone codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t format = audio_format();
    int result = esp_codec_dev_open(microphone, &format);
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_in_gain(microphone, (float)gain_db);
    }
    if (result == ESP_CODEC_DEV_OK && strcmp(route, "default") != 0) {
        /* ES8389 PGA1 input select 0b110 is MIC2P single-ended and 0b101 is
         * MIC1P single-ended. These are reversible diagnostics; the default
         * BSP route remains differential MIC1P-MIC1N. */
        result = factory_audio_set_input_route(route);
        printf("microphone_test: temporary route %s -> ADC ch0: %s\n",
               strcmp(route, "mic2_left") == 0 ? "MIC2P" : "MIC1P",
               esp_err_to_name(result));
    }
    static int16_t samples[AUDIO_FRAME_COUNT * 2];
    uint16_t peak[2] = {0};
    int16_t minimum[2] = {INT16_MAX, INT16_MAX};
    int16_t maximum[2] = {INT16_MIN, INT16_MIN};
    int64_t sum[2] = {0};
    uint64_t square_sum[2] = {0};
    uint32_t sample_count[2] = {0};
    uint32_t clip_count[2] = {0};
    const uint32_t block_count =
        (duration_ms * AUDIO_SAMPLE_RATE + 1000U * AUDIO_FRAME_COUNT - 1U) /
        (1000U * AUDIO_FRAME_COUNT);
    for (unsigned block = 0; result == ESP_CODEC_DEV_OK && block < block_count; ++block) {
        result = esp_codec_dev_read(microphone, samples, sizeof(samples));
        if (result != ESP_CODEC_DEV_OK) {
            break;
        }
        for (unsigned index = 0; index < sizeof(samples) / sizeof(samples[0]); ++index) {
            const unsigned channel = index % 2;
            const int16_t sample = samples[index];
            const int32_t absolute = sample < 0 ? -(int32_t)sample : sample;
            if (absolute > peak[channel]) {
                peak[channel] = (uint16_t)absolute;
            }
            if (sample < minimum[channel]) {
                minimum[channel] = sample;
            }
            if (sample > maximum[channel]) {
                maximum[channel] = sample;
            }
            sum[channel] += sample;
            square_sum[channel] += (uint64_t)((int64_t)sample * sample);
            if (sample <= INT16_MIN + 8 || sample >= INT16_MAX - 7) {
                clip_count[channel]++;
            }
            sample_count[channel]++;
        }
    }
    const int close_result = esp_codec_dev_close(microphone);
    if (result == ESP_CODEC_DEV_OK && close_result != ESP_CODEC_DEV_OK) {
        result = close_result;
    }

    int32_t dc[2] = {0};
    uint32_t ac_rms[2] = {0};
    uint32_t peak_to_peak[2] = {0};
    bool channel_live[2] = {false};
    bool channel_clipping[2] = {false};
    for (unsigned channel = 0; channel < 2; ++channel) {
        if (sample_count[channel] == 0) {
            minimum[channel] = 0;
            maximum[channel] = 0;
            continue;
        }
        dc[channel] = (int32_t)(sum[channel] / sample_count[channel]);
        const uint64_t mean_square =
            square_sum[channel] / sample_count[channel];
        const uint64_t dc_square = (uint64_t)((int64_t)dc[channel] *
                                               dc[channel]);
        const uint64_t ac_variance =
            mean_square > dc_square ? mean_square - dc_square : 0;
        ac_rms[channel] = (uint32_t)sqrtf((float)ac_variance);
        peak_to_peak[channel] =
            (uint32_t)((int32_t)maximum[channel] - minimum[channel]);
        channel_live[channel] = peak_to_peak[channel] >= 16 &&
                                ac_rms[channel] >= 4;
        channel_clipping[channel] =
            clip_count[channel] > sample_count[channel] / 100U;
    }

    factory_status_t status = FACTORY_STATUS_WARN;
    if (result != ESP_CODEC_DEV_OK || !channel_live[0] || !channel_live[1] ||
            channel_clipping[0] || channel_clipping[1]) {
        status = FACTORY_STATUS_FAIL;
    }
    if (result != ESP_CODEC_DEV_OK) {
        printf("microphone_test: capture failed: %s\n",
               esp_err_to_name(result));
    }
    printf("microphone_stats ch0_min=%d ch0_max=%d ch0_p2p=%" PRIu32
           " ch0_live=%s ch0_clipping=%s ch1_min=%d ch1_max=%d"
           " ch1_p2p=%" PRIu32 " ch1_live=%s ch1_clipping=%s\n",
           minimum[0], maximum[0], peak_to_peak[0],
           channel_live[0] ? "yes" : "no",
           channel_clipping[0] ? "yes" : "no",
           minimum[1], maximum[1], peak_to_peak[1],
           channel_live[1] ? "yes" : "no",
           channel_clipping[1] ? "yes" : "no");
    printf("microphone_test: ch0/ch1 capture-path statistics only; channel "
           "mapping requires quiet and directed near-talk runs\n");

    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "pk=%u/%u ac_rms=%" PRIu32 "/%" PRIu32
             " dc=%" PRId32 "/%" PRId32
             " clip=%" PRIu32 "/%" PRIu32
             " gain=%" PRIu32 " dur_ms=%" PRIu32,
             (unsigned)peak[0], (unsigned)peak[1], ac_rms[0], ac_rms[1],
             dc[0], dc[1], clip_count[0], clip_count[1], gain_db, duration_ms);
    factory_report_set(FACTORY_TEST_MICROPHONE, status, detail);
    factory_report_print_one(FACTORY_TEST_MICROPHONE);
    if (status != FACTORY_STATUS_FAIL) {
        printf("microphone_test: WARN until physical ch0/ch1 mapping and "
               "cross-channel behavior are verified\n");
        return ESP_OK;
    }
    return result != ESP_CODEC_DEV_OK ? result : ESP_FAIL;
}

/* mic_snoop: bit-level probe of the microphone data return path.
 * While a real esp_codec_dev_read() stream runs on a helper task, count
 * edges on the ASDOUT/DIN pad and on the I2S clock pads. This splits
 * "codec never drives ASDOUT / wire broken" from "ESP32 I2S RX not
 * clocking" from "data arrives but the read path drops it". */
typedef struct {
    esp_codec_dev_handle_t microphone;
    volatile bool stop;
    volatile int last_result;
    volatile uint32_t blocks;
    TaskHandle_t owner;
} mic_snoop_reader_t;

static void mic_snoop_reader_task(void *arg)
{
    mic_snoop_reader_t *reader = (mic_snoop_reader_t *)arg;
    static int16_t buffer[AUDIO_FRAME_COUNT * 2];
    while (!reader->stop) {
        const int result =
            esp_codec_dev_read(reader->microphone, buffer, sizeof(buffer));
        if (result != ESP_CODEC_DEV_OK) {
            reader->last_result = result;
            break;
        }
        reader->blocks++;
    }
    xTaskNotifyGive(reader->owner);
    /* The owner always deletes this task after observing completion or a
     * timeout. Suspending here keeps the TaskHandle valid and guarantees the
     * stack-owned reader context is no longer touched before owner cleanup. */
    vTaskSuspend(NULL);
}

static uint32_t mic_snoop_count_edges(gpio_num_t gpio, uint32_t window_ms)
{
    gpio_ll_input_enable(&GPIO, (uint32_t)gpio);
    uint32_t edges = 0;
    int last = gpio_ll_get_level(&GPIO, (uint32_t)gpio);
    const int64_t deadline = esp_timer_get_time() + (int64_t)window_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        const int level = gpio_ll_get_level(&GPIO, (uint32_t)gpio);
        if (level != last) {
            edges++;
            last = level;
        }
    }
    return edges;
}

static int command_mic_snoop(int argc, char **argv)
{
    if (argc > 1) {
        printf("usage: mic_snoop - probe ASDOUT/BCLK/WS/MCLK pad activity during capture\n");
        return ESP_ERR_INVALID_ARG;
    }
    esp_codec_dev_handle_t microphone = audio_device(false);
    if (microphone == NULL) {
        printf("mic_snoop: microphone codec init failed\n");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t format = audio_format();
    int result = esp_codec_dev_open(microphone, &format);
    if (result != ESP_CODEC_DEV_OK) {
        printf("mic_snoop: open failed: %s\n", esp_err_to_name(result));
        return result;
    }
    mic_snoop_reader_t reader = {
        .microphone = microphone,
        .stop = false,
        .last_result = ESP_CODEC_DEV_OK,
        .blocks = 0,
        .owner = xTaskGetCurrentTaskHandle(),
    };
    while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
    }
    TaskHandle_t reader_task = NULL;
    if (xTaskCreate(mic_snoop_reader_task, "mic_snoop", 4096, &reader, 5,
                    &reader_task) != pdPASS) {
        printf("mic_snoop: reader task create failed\n");
        esp_codec_dev_close(microphone);
        return ESP_ERR_NO_MEM;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_dump_io_configuration(stdout, BIT64(BSP_I2S_DIN));
    static const struct {
        gpio_num_t gpio;
        const char *name;
        uint32_t expected_hz;
    } probes[] = {
        { BSP_I2S_DIN, "ASDOUT/DIN", 0 },
        { BSP_I2S_BCLK, "BCLK", AUDIO_SAMPLE_RATE * 16 * 2 },
        { BSP_I2S_LRCLK, "WS", AUDIO_SAMPLE_RATE },
        { BSP_I2S_MCLK, "MCLK", AUDIO_SAMPLE_RATE * 256 },
    };
    for (size_t index = 0; index < sizeof(probes) / sizeof(probes[0]); ++index) {
        const uint32_t edges = mic_snoop_count_edges(probes[index].gpio, 20);
        printf("mic_snoop: GPIO%d (%s) edges=%" PRIu32 " in 20ms",
               (int)probes[index].gpio, probes[index].name, edges);
        if (probes[index].expected_hz > 0) {
            printf(" (expect ~%" PRIu32 " edges)",
                   probes[index].expected_hz * 2 / 50);
        }
        printf("\n");
    }
    reader.stop = true;
    const bool reader_finished =
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) > 0;
    vTaskDelete(reader_task);
    printf("mic_snoop: reader blocks=%" PRIu32 " last_result=%s\n",
           reader.blocks, esp_err_to_name(reader.last_result));
    esp_codec_dev_close(microphone);
    printf("mic_snoop: GPIO%d idle level after close = %d\n",
           (int)BSP_I2S_DIN, gpio_ll_get_level(&GPIO, (uint32_t)BSP_I2S_DIN));
    if (!reader_finished) {
        return ESP_ERR_TIMEOUT;
    }
    return reader.last_result;
}

/* Feed a known I2S tone through the ES8389 DAC digital path, then mix the
 * DAC channels into the ADC channels with register 0x31. This deliberately
 * bypasses both physical microphones and their analog input networks. */
typedef struct {
    esp_codec_dev_handle_t speaker;
    const int16_t *samples;
    size_t sample_bytes;
    volatile bool stop;
    volatile int last_result;
    volatile uint32_t blocks;
    TaskHandle_t owner;
} codec_loopback_writer_t;

static void codec_loopback_writer_task(void *arg)
{
    codec_loopback_writer_t *writer = (codec_loopback_writer_t *)arg;
    while (!writer->stop) {
        const int result =
            esp_codec_dev_write(writer->speaker, (void *)writer->samples,
                                writer->sample_bytes);
        if (result != ESP_CODEC_DEV_OK) {
            writer->last_result = result;
            break;
        }
        writer->blocks++;
    }
    xTaskNotifyGive(writer->owner);
    vTaskSuspend(NULL);
}

static int command_codec_loopback(int argc, char **argv)
{
    if (argc != 1) {
        printf("usage: codec_loopback - bypass microphones with ES8389 DAC-to-ADC mix\n");
        return ESP_ERR_INVALID_ARG;
    }

    int result = factory_audio_stop();
    if (result != ESP_OK) {
        printf("codec_loopback: audio reset failed: %s\n",
               esp_err_to_name(result));
        return result;
    }
    esp_codec_dev_handle_t speaker = audio_device(true);
    esp_codec_dev_handle_t microphone = audio_device(false);
    if (speaker == NULL || microphone == NULL) {
        printf("codec_loopback: codec initialization failed\n");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t format = audio_format();
    bool speaker_open = false;
    bool microphone_open = false;
    bool mix_enabled = false;
    result = esp_codec_dev_open(speaker, &format);
    if (result != ESP_CODEC_DEV_OK) {
        printf("codec_loopback: speaker stream open failed: %s\n",
               esp_err_to_name(result));
        goto cleanup;
    }
    speaker_open = true;
    result = bsp_power_domain_set(BSP_POWER_AUDIO_PA, false);
    if (result != ESP_OK) {
        printf("codec_loopback: PA disable failed: %s\n",
               esp_err_to_name(result));
        goto cleanup;
    }
    result = esp_codec_dev_open(microphone, &format);
    if (result != ESP_CODEC_DEV_OK) {
        printf("codec_loopback: microphone stream open failed: %s\n",
               esp_err_to_name(result));
        goto cleanup;
    }
    microphone_open = true;

    /* The output device opens at its default volume of zero. Restore the
     * codec's documented 0 dB digital volume before enabling the internal
     * DAC-to-ADC mix. */
    result = esp_codec_dev_write_reg(microphone, 0x46, 0xBF);
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_write_reg(microphone, 0x47, 0xBF);
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_write_reg(microphone, 0x31, 0xC0);
    }
    if (result != ESP_CODEC_DEV_OK) {
        printf("codec_loopback: codec register setup failed: %s\n",
               esp_err_to_name(result));
        goto cleanup;
    }
    mix_enabled = true;

    int mix_value = 0;
    result = esp_codec_dev_read_reg(microphone, 0x31, &mix_value);
    if (result != ESP_CODEC_DEV_OK || mix_value != 0xC0) {
        printf("codec_loopback: register 0x31 readback=0x%02x result=%s\n",
               mix_value, esp_err_to_name(result));
        result = ESP_FAIL;
        goto cleanup;
    }

    static int16_t tone[AUDIO_FRAME_COUNT * 2];
    static int16_t capture[AUDIO_FRAME_COUNT * 2];
    const uint32_t period_samples = AUDIO_SAMPLE_RATE / 1000U;
    for (size_t frame = 0; frame < AUDIO_FRAME_COUNT; ++frame) {
        const int16_t value = frame % period_samples < period_samples / 2U ?
                              12000 : -12000;
        tone[frame * 2] = value;
        tone[frame * 2 + 1] = value;
    }

    codec_loopback_writer_t writer = {
        .speaker = speaker,
        .samples = tone,
        .sample_bytes = sizeof(tone),
        .last_result = ESP_CODEC_DEV_OK,
        .owner = xTaskGetCurrentTaskHandle(),
    };
    while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
    }
    TaskHandle_t writer_task = NULL;
    if (xTaskCreate(codec_loopback_writer_task, "codec_loop_tx", 4096,
                    &writer, 5, &writer_task) != pdPASS) {
        printf("codec_loopback: writer task create failed\n");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    uint32_t nonzero_samples = 0;
    uint32_t sample_count = 0;
    uint16_t peak = 0;
    for (unsigned block = 0; block < 12; ++block) {
        result = esp_codec_dev_read(microphone, capture, sizeof(capture));
        if (result != ESP_CODEC_DEV_OK) {
            break;
        }
        /* Discard two pipeline-fill blocks before scoring the loopback. */
        if (block < 2) {
            continue;
        }
        for (size_t index = 0;
                index < sizeof(capture) / sizeof(capture[0]); ++index) {
            const int32_t sample = capture[index];
            const uint32_t absolute = sample < 0 ? (uint32_t)-sample :
                                                  (uint32_t)sample;
            nonzero_samples += sample != 0;
            sample_count++;
            if (absolute > peak) {
                peak = (uint16_t)absolute;
            }
        }
    }
    const uint32_t asdout_edges =
        mic_snoop_count_edges(BSP_I2S_DIN, 20);
    writer.stop = true;
    const bool writer_finished =
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) > 0;
    vTaskDelete(writer_task);
    if (!writer_finished && result == ESP_CODEC_DEV_OK) {
        result = ESP_ERR_TIMEOUT;
    }
    if (writer.last_result != ESP_CODEC_DEV_OK && result == ESP_CODEC_DEV_OK) {
        result = writer.last_result;
    }

    const bool passed = result == ESP_CODEC_DEV_OK && writer.blocks > 0 &&
                        sample_count > 0 && nonzero_samples > sample_count / 2U &&
                        peak > 1000 && asdout_edges > 0;
    printf("codec_loopback: tx_blocks=%" PRIu32
           " samples=%" PRIu32 " nonzero=%" PRIu32
           " peak=%u GPIO%d_edges=%" PRIu32 " result=%s\n",
           writer.blocks, sample_count, nonzero_samples, (unsigned)peak,
           (int)BSP_I2S_DIN, asdout_edges, esp_err_to_name(result));
    printf("codec_loopback: %s - microphones and acoustic ports were bypassed\n",
           passed ? "PASS" : "FAIL");
    if (!passed && result == ESP_CODEC_DEV_OK) {
        result = ESP_FAIL;
    }

cleanup:
    if (mix_enabled) {
        esp_codec_dev_write_reg(microphone, 0x31, 0x00);
    }
    if (microphone_open) {
        esp_codec_dev_close(microphone);
    }
    if (speaker_open) {
        esp_codec_dev_close(speaker);
    }
    bsp_power_domain_set(BSP_POWER_AUDIO_PA, false);
    return result;
}

/* Prove the SoC I2S RX/DMA path independently of the ES8389 and GPIO44.
 * The I2S driver has an explicit loopback path when dout == din, so route a
 * known byte sequence through GPIO8 (the normal codec DAC input) while the
 * codec rail is powered and its serial clocks are left unrouted. */
#define I2S_LOOPBACK_PATTERN_BYTES 100
#define I2S_LOOPBACK_CAPTURE_BYTES 8192

static bool i2s_loopback_find_pattern(const uint8_t *capture,
                                      size_t capture_size,
                                      const uint8_t *pattern,
                                      size_t pattern_size,
                                      size_t *offset)
{
    if (capture_size < pattern_size) {
        return false;
    }
    for (size_t index = 0; index <= capture_size - pattern_size; ++index) {
        if (memcmp(capture + index, pattern, pattern_size) == 0) {
            *offset = index;
            return true;
        }
    }
    return false;
}

static int command_i2s_loopback(int argc, char **argv)
{
    if (argc != 1) {
        printf("usage: i2s_loopback - internally loop GPIO8 TX back to I2S RX\n");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = factory_audio_stop();
    if (result != ESP_OK) {
        printf("i2s_loopback: audio stop failed: %s\n", esp_err_to_name(result));
        return result;
    }
    result = bsp_audio_deinit();
    if (result != ESP_OK) {
        printf("i2s_loopback: audio deinit failed: %s\n", esp_err_to_name(result));
        return result;
    }
    result = bsp_peripheral_power_set(BSP_PERIPHERAL_AUDIO, true);
    if (result != ESP_OK) {
        printf("i2s_loopback: codec rail enable failed: %s\n",
               esp_err_to_name(result));
        return result;
    }

    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    bool tx_enabled = false;
    bool rx_enabled = false;
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    result = i2s_new_channel(&channel_config, &tx, &rx);
    if (result != ESP_OK) {
        goto cleanup;
    }

    const i2s_std_config_t loopback_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = GPIO_NUM_NC,
            .ws = GPIO_NUM_NC,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DOUT,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    result = i2s_channel_init_std_mode(tx, &loopback_config);
    if (result != ESP_OK) {
        goto cleanup;
    }
    result = i2s_channel_init_std_mode(rx, &loopback_config);
    if (result != ESP_OK) {
        goto cleanup;
    }
    result = i2s_channel_enable(tx);
    if (result != ESP_OK) {
        goto cleanup;
    }
    tx_enabled = true;
    result = i2s_channel_enable(rx);
    if (result != ESP_OK) {
        goto cleanup;
    }
    rx_enabled = true;

    static uint8_t pattern[I2S_LOOPBACK_PATTERN_BYTES];
    static uint8_t capture[I2S_LOOPBACK_CAPTURE_BYTES];
    for (size_t index = 0; index < sizeof(pattern); ++index) {
        pattern[index] = (uint8_t)(index + 1);
    }
    memset(capture, 0, sizeof(capture));
    size_t bytes_written = 0;
    size_t bytes_read = 0;
    result = i2s_channel_write(tx, pattern, sizeof(pattern), &bytes_written,
                               1000);
    if (result != ESP_OK) {
        goto cleanup;
    }
    result = i2s_channel_read(rx, capture, sizeof(capture), &bytes_read, 1000);
    if (result != ESP_OK) {
        goto cleanup;
    }

    size_t pattern_offset = 0;
    const bool matched =
        i2s_loopback_find_pattern(capture, bytes_read, pattern,
                                  sizeof(pattern), &pattern_offset);
    size_t nonzero_bytes = 0;
    for (size_t index = 0; index < bytes_read; ++index) {
        nonzero_bytes += capture[index] != 0;
    }
    printf("i2s_loopback: wrote=%u read=%u nonzero=%u pattern=%s",
           (unsigned)bytes_written, (unsigned)bytes_read,
           (unsigned)nonzero_bytes, matched ? "FOUND" : "NOT_FOUND");
    if (matched) {
        printf(" offset=%u", (unsigned)pattern_offset);
    }
    printf("\n");
    if (!matched || bytes_written != sizeof(pattern)) {
        result = ESP_FAIL;
    }

cleanup:
    if (rx_enabled) {
        i2s_channel_disable(rx);
    }
    if (tx_enabled) {
        i2s_channel_disable(tx);
    }
    if (rx != NULL) {
        i2s_del_channel(rx);
    }
    if (tx != NULL) {
        i2s_del_channel(tx);
    }
    const esp_err_t power_result =
        bsp_peripheral_power_set(BSP_PERIPHERAL_AUDIO, false);
    if (result == ESP_OK && power_result != ESP_OK) {
        result = power_result;
    }
    printf("i2s_loopback: %s (%s)\n",
           result == ESP_OK ? "PASS" : "FAIL", esp_err_to_name(result));
    return result;
}

typedef struct {
    bool ack[4];
    int gpio_level;
} codec_link_probe_state_t;

static esp_err_t codec_link_probe_park_audio_pins(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(BSP_I2S_DOUT) | BIT64(BSP_I2S_BCLK) |
                        BIT64(BSP_I2S_LRCLK) | BIT64(BSP_I2S_MCLK) |
                        BIT64(BSP_I2S_DIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static esp_err_t codec_link_probe_scan(gpio_pull_mode_t pull_mode,
                                       codec_link_probe_state_t *state)
{
    esp_err_t result = gpio_set_pull_mode(BSP_I2S_DIN, pull_mode);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* At 3.0 V the ES8389 remains inside its recommended AVDD range while
     * VIH falls to 2.1 V. This gives the ESP32-S31's weak pull-up enough
     * margin against the fitted 100 kOhm AD1 pull-down (R68), without ever
     * driving the shared ASDOUT/AD1 net as a push-pull output. */
    result = bsp_pmic_regulator_set_voltage(BSP_PMIC_ALDO3, 3000);
    if (result != ESP_OK) {
        return result;
    }
    result = bsp_pmic_regulator_enable(BSP_PMIC_ALDO3, true);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    state->gpio_level = gpio_get_level(BSP_I2S_DIN);
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        result = ESP_ERR_INVALID_STATE;
        goto power_off;
    }
    for (uint8_t address = 0x10; address <= 0x13; ++address) {
        const esp_err_t probe = i2c_master_probe(bus, address, 20);
        state->ack[address - 0x10] = probe == ESP_OK;
        if (probe != ESP_OK && probe != ESP_ERR_NOT_FOUND) {
            result = probe;
            goto power_off;
        }
    }

power_off:
    {
        const esp_err_t power_result =
            bsp_pmic_regulator_enable(BSP_PMIC_ALDO3, false);
        if (result == ESP_OK && power_result != ESP_OK) {
            result = power_result;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return result;
}

static void codec_link_probe_print_state(const char *name,
                                         const codec_link_probe_state_t *state)
{
    printf("codec_link_probe: %-8s GPIO%d=%d ACK 0x10=%s 0x11=%s 0x12=%s 0x13=%s\n",
           name, (int)BSP_I2S_DIN, state->gpio_level,
           state->ack[0] ? "yes" : "no", state->ack[1] ? "yes" : "no",
           state->ack[2] ? "yes" : "no", state->ack[3] ? "yes" : "no");
}

/* U19 pin 11 is both ASDOUT and the AD1 address strap. With the codec off it
 * is an input, so changing only GPIO44's weak pull before power-up can change
 * the ES8389 seven-bit address from 00100_00 (0x10) to 00100_10 (0x12).
 * Observing that transition proves the complete U4 pad -> PCB trace -> R61 ->
 * U19.11 path without a scope, jumper, or any push-pull contention. */
static int command_codec_link_probe(int argc, char **argv)
{
    if (argc != 1) {
        printf("usage: codec_link_probe - verify GPIO44/R61/U19.11 via ES8389 AD1\n");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = factory_audio_stop();
    if (result == ESP_OK) {
        result = bsp_audio_deinit();
    }
    if (result == ESP_OK) {
        result = bsp_power_domain_set(BSP_POWER_AUDIO_PA, false);
    }
    if (result == ESP_OK) {
        result = codec_link_probe_park_audio_pins();
    }
    if (result == ESP_OK) {
        result = bsp_pmic_init();
    }
    if (result == ESP_OK) {
        result = bsp_pmic_regulator_enable(BSP_PMIC_ALDO3, false);
    }
    if (result == ESP_OK) {
        result = bsp_i2c_init();
    }
    if (result != ESP_OK) {
        printf("codec_link_probe: setup failed: %s\n", esp_err_to_name(result));
        goto cleanup;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    codec_link_probe_state_t pulldown = {0};
    codec_link_probe_state_t pullup = {0};
    result = codec_link_probe_scan(GPIO_PULLDOWN_ONLY, &pulldown);
    if (result == ESP_OK) {
        result = codec_link_probe_scan(GPIO_PULLUP_ONLY, &pullup);
    }
    codec_link_probe_print_state("pulldown", &pulldown);
    codec_link_probe_print_state("pullup", &pullup);
    if (result == ESP_OK) {
        const bool pulldown_ok = pulldown.ack[0] && !pulldown.ack[1] &&
                                 !pulldown.ack[2] && !pulldown.ack[3];
        const bool pullup_ok = !pullup.ack[0] && !pullup.ack[1] &&
                               pullup.ack[2] && !pullup.ack[3];
        if (pulldown_ok && pullup_ok) {
            printf("codec_link_probe: PASS - GPIO44 reaches ES8389 AD1 through R61\n");
        } else {
            printf("codec_link_probe: INCONCLUSIVE - address did not switch 0x10 -> 0x12\n");
            result = ESP_FAIL;
        }
    } else {
        printf("codec_link_probe: scan failed: %s\n", esp_err_to_name(result));
    }

cleanup:
    {
        const esp_err_t power_result =
            bsp_pmic_regulator_enable(BSP_PMIC_ALDO3, false);
        if (result == ESP_OK && power_result != ESP_OK) {
            result = power_result;
        }
        const esp_err_t voltage_result =
            bsp_pmic_regulator_set_voltage(BSP_PMIC_ALDO3, 3300);
        if (result == ESP_OK && voltage_result != ESP_OK) {
            result = voltage_result;
        }
        const esp_err_t pin_result = gpio_set_pull_mode(BSP_I2S_DIN,
                                                        GPIO_FLOATING);
        if (result == ESP_OK && pin_result != ESP_OK) {
            result = pin_result;
        }
        const esp_err_t park_result = codec_link_probe_park_audio_pins();
        if (result == ESP_OK && park_result != ESP_OK) {
            result = park_result;
        }
    }
    printf("codec_link_probe: cleanup=%s, ALDO3 off, setpoint restored to 3.3 V\n",
           esp_err_to_name(result));
    return result;
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
        s_speaker = NULL;
        if (error != ESP_OK && first_error == ESP_OK) {
            first_error = error;
        }
    }
    if (s_microphone != NULL) {
        const esp_err_t error = bsp_audio_codec_deinit(s_microphone);
        s_microphone = NULL;
        if (error != ESP_OK && first_error == ESP_OK) {
            first_error = error;
        }
    }
    return first_error;
}

esp_err_t factory_audio_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "speaker_test", .help = "Play a square-wave tone: speaker_test [FREQ_HZ] [VOLUME] [DURATION_MS].", .func = command_speaker_test},
        {.command = "microphone_test", .help = "Capture stereo audio and check both channels: microphone_test [DURATION_MS] [GAIN_DB].", .func = command_microphone_test},
        {.command = "codec_reg", .help = "ES8389 register access: codec_reg [REG [VALUE]] (hex ok); no args dumps key registers.", .func = command_codec_reg},
        {.command = "mic_snoop", .help = "Probe ASDOUT/I2S pad edge activity during a live capture: mic_snoop.", .func = command_mic_snoop},
        {.command = "codec_loopback", .help = "Bypass microphones through ES8389 DAC-to-ADC digital mix.", .func = command_codec_loopback},
        {.command = "i2s_loopback", .help = "Loop GPIO8 TX internally to I2S RX and verify a known pattern.", .func = command_i2s_loopback},
        {.command = "codec_link_probe", .help = "Verify the GPIO44/R61/U19.11 path by switching the ES8389 AD1 address strap.", .func = command_codec_link_probe},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
