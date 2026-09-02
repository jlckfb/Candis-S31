/*
 * Candis-S31 player audio render adapter.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "audio_service.h"

#include <inttypes.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "esp_audio_render.h"
#include "esp_codec_dev.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "player_audio";

#define OUTPUT_SAMPLE_RATE 48000
#define OUTPUT_CHANNELS    2
#define OUTPUT_BITS        16

static esp_codec_dev_handle_t s_speaker;
static esp_audio_render_handle_t s_render;
static esp_audio_render_stream_handle_t s_stream;
static esp_gmf_pool_handle_t s_pool;
static SemaphoreHandle_t s_lock;
static bool s_started;
static bool s_codec_open;
static int s_volume = 20;
static uint32_t s_output_sample_rate;

static int clamp_volume(int volume)
{
    if (volume < 0) {
        return 0;
    }
    return volume > 100 ? 100 : volume;
}

static int codec_writer(uint8_t *pcm, uint32_t length, void *ctx)
{
    esp_codec_dev_handle_t codec = (esp_codec_dev_handle_t)ctx;
    if (codec == NULL || pcm == NULL || length == 0) {
        return -1;
    }
    /* The board has one speaker. Fold stereo to mono and duplicate it into
     * both I2S slots so content panned to either channel remains audible. */
    int16_t *samples = (int16_t *)pcm;
    const uint32_t frames = length / (2U * sizeof(int16_t));
    for (uint32_t i = 0; i < frames; ++i) {
        const int32_t mixed = ((int32_t)samples[i * 2U] + samples[i * 2U + 1U]) / 2;
        samples[i * 2U] = (int16_t)mixed;
        samples[i * 2U + 1U] = (int16_t)mixed;
    }
    const int result = esp_codec_dev_write(codec, pcm, length);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec write failed: %d, len=%" PRIu32, result, length);
    }
    return result;
}

static esp_err_t register_element(esp_gmf_element_handle_t element, const char *name)
{
    if (element == NULL) {
        ESP_LOGE(TAG, "%s element creation failed", name);
        return ESP_FAIL;
    }
    if (esp_gmf_pool_register_element(s_pool, element, NULL) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "%s element registration failed", name);
        esp_gmf_obj_delete(element);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t create_audio_pool(void)
{
    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        return ESP_ERR_NO_MEM;
    }

    esp_gmf_element_handle_t element = NULL;
    esp_ae_ch_cvt_cfg_t channel_cfg = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    if (esp_gmf_ch_cvt_init(&channel_cfg, &element) != ESP_GMF_ERR_OK ||
            register_element(element, "channel converter") != ESP_OK) {
        return ESP_FAIL;
    }

    element = NULL;
    esp_ae_bit_cvt_cfg_t bit_cfg = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    if (esp_gmf_bit_cvt_init(&bit_cfg, &element) != ESP_GMF_ERR_OK ||
            register_element(element, "bit converter") != ESP_OK) {
        return ESP_FAIL;
    }

    element = NULL;
    esp_ae_rate_cvt_cfg_t rate_cfg = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    if (esp_gmf_rate_cvt_init(&rate_cfg, &element) != ESP_GMF_ERR_OK ||
            register_element(element, "rate converter") != ESP_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void cleanup(void)
{
    if (s_render != NULL) {
        esp_audio_render_destroy(s_render);
        s_render = NULL;
        s_stream = NULL;
    }
    if (s_pool != NULL) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }
    if (s_started) {
        bsp_audio_deinit();
    }
    s_speaker = NULL;
    s_started = false;
}

esp_err_t audio_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = bsp_audio_init(NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(error));
        return error;
    }
    s_started = true;
    s_speaker = bsp_audio_codec_speaker_init();
    if (s_speaker == NULL) {
        cleanup();
        return ESP_FAIL;
    }
    error = create_audio_pool();
    if (error != ESP_OK) {
        cleanup();
        return error;
    }

    esp_audio_render_cfg_t render_cfg = {
        .max_stream_num = 1,
        .out_writer = codec_writer,
        .out_ctx = s_speaker,
        .out_sample_info = {
            .sample_rate = OUTPUT_SAMPLE_RATE,
            .bits_per_sample = OUTPUT_BITS,
            .channel = OUTPUT_CHANNELS,
        },
        .pool = s_pool,
        .process_period = 20,
    };
    if (esp_audio_render_create(&render_cfg, &s_render) != ESP_AUDIO_RENDER_ERR_OK ||
            esp_audio_render_stream_get(s_render, ESP_AUDIO_RENDER_FIRST_STREAM,
                                        &s_stream) != ESP_AUDIO_RENDER_ERR_OK) {
        cleanup();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "audio render ready (%d Hz, %d-bit, %d-channel)",
             OUTPUT_SAMPLE_RATE, OUTPUT_BITS, OUTPUT_CHANNELS);
    return ESP_OK;
}

void *audio_render_stream(void)
{
    return s_stream;
}

esp_err_t audio_prepare_playback(int volume)
{
    if (!s_started || s_speaker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_volume = clamp_volume(volume);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_configure_playback(uint32_t sample_rate)
{
    if (!s_started || s_speaker == NULL || s_render == NULL || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t device_rate =
        (sample_rate == 8000U || sample_rate == 16000U ||
         sample_rate == 44100U || sample_rate == 48000U) ? sample_rate : 48000U;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_codec_open && s_output_sample_rate == device_rate) {
        const esp_err_t volume_error = esp_codec_dev_set_out_vol(s_speaker, s_volume);
        xSemaphoreGive(s_lock);
        return volume_error;
    }
    if (s_codec_open) {
        esp_codec_dev_close(s_speaker);
        s_codec_open = false;
    }
    esp_audio_render_sample_info_t render_format = {
        .sample_rate = device_rate,
        .channel = OUTPUT_CHANNELS,
        .bits_per_sample = OUTPUT_BITS,
    };
    esp_audio_render_err_t render_error =
        esp_audio_render_set_out_sample_info(s_render, &render_format);
    if (render_error != ESP_AUDIO_RENDER_ERR_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "audio render rate switch to %" PRIu32 " Hz failed: %d",
                 device_rate, render_error);
        return ESP_FAIL;
    }
    esp_err_t error = bsp_power_domain_set(BSP_POWER_AUDIO_PA, true);
    if (error == ESP_OK) {
        esp_codec_dev_sample_info_t codec_format = {
            .sample_rate = device_rate,
            .channel = OUTPUT_CHANNELS,
            .bits_per_sample = OUTPUT_BITS,
            .mclk_multiple = 256,
        };
        error = esp_codec_dev_open(s_speaker, &codec_format);
    }
    if (error == ESP_CODEC_DEV_OK) {
        s_codec_open = true;
        s_output_sample_rate = device_rate;
        error = esp_codec_dev_set_out_vol(s_speaker, s_volume);
        ESP_LOGI(TAG, "playback path configured at %" PRIu32 " Hz", device_rate);
    } else {
        bsp_power_domain_set(BSP_POWER_AUDIO_PA, false);
        ESP_LOGE(TAG, "codec open at %" PRIu32 " Hz failed: %s",
                 device_rate, esp_err_to_name(error));
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t audio_write_pcm(uint8_t *pcm, uint32_t length)
{
    if (!s_codec_open || pcm == NULL || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec_writer(pcm, length, s_speaker) == ESP_CODEC_DEV_OK ?
           ESP_OK : ESP_FAIL;
}

void audio_finish_playback(void)
{
    if (!s_started || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_codec_open) {
        esp_codec_dev_close(s_speaker);
        s_codec_open = false;
        s_output_sample_rate = 0;
    }
    bsp_power_domain_set(BSP_POWER_AUDIO_PA, false);
    xSemaphoreGive(s_lock);
}

esp_err_t audio_set_volume(int volume)
{
    if (!s_started || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_volume = clamp_volume(volume);
    esp_err_t error = ESP_OK;
    if (s_codec_open) {
        error = esp_codec_dev_set_out_vol(s_speaker, s_volume);
    }
    xSemaphoreGive(s_lock);
    return error;
}
