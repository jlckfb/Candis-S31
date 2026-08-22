/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>

#include "esp_check.h"
#include "esp_codec_dev_defaults.h"

#include "bsp/candis_s31.h"

static const char *TAG = "candis_audio";
static i2s_chan_handle_t s_tx_channel;
static i2s_chan_handle_t s_rx_channel;
static const audio_codec_data_if_t *s_data_if;

typedef struct {
    esp_codec_dev_handle_t device;
    const audio_codec_if_t *codec;
    const audio_codec_ctrl_if_t *control;
    const audio_codec_gpio_if_t *gpio;
} codec_instance_t;

/* Speaker and microphone are separate logical devices over one ES8389.
 * esp_codec_dev >= 1.6.0 identifies that shared physical codec by I2C
 * port/address and reference-counts its hardware state. Both instances must
 * nevertheless use the same clock/reference policy: the first instance owns
 * the physical initialization while the second reuses it. */
static codec_instance_t s_speaker;
static codec_instance_t s_microphone;

static const i2s_std_gpio_config_t s_i2s_gpio = {
    .mclk = BSP_I2S_MCLK,
    .bclk = BSP_I2S_BCLK,
    .ws = BSP_I2S_LRCLK,
    .dout = BSP_I2S_DOUT,
    .din = BSP_I2S_DIN,
    .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
    },
};

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    if (s_tx_channel != NULL && s_rx_channel != NULL && s_data_if != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_peripheral_power_set(BSP_PERIPHERAL_AUDIO, true),
                        TAG, "audio power-up failed");

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    esp_err_t error = i2s_new_channel(&channel_config, &s_tx_channel,
                                      &s_rx_channel);
    if (error != ESP_OK) {
        goto fail;
    }

    /* Interface mode note: the Korvo-1 documentation requires the ES8389 to
     * run in TDM mode ("DAC 与 ADC 信号均须以 TDM 格式发送至 SoC",
     * esp-dev-kits-zh_CN), but this BSP (like the official esp32_s31_korvo_1
     * BSP) uses I2S STD/Philips mode and the es8389 driver stays in its
     * hard-coded default I2S data format. Kept as-is until the Sunking
     * datasheet confirms the TDM frame layout.
     *
     * TDM pre-study (esp_codec_dev ~1.5, ESP-IDF 6.1): the driver exposes no
     * TDM option - es8389_codec_cfg_t has no format/slot field, and
     * es8389_set_fs() unconditionally calls es8389_config_fmt(ES_I2S_NORMAL)
     * (regs 0x20/0x40 DAIFMT bits, reg 0x0C bits[7:5]=0); the only codec
     * formats are I2S/LJ/RJ/DSP-A/DSP-B (es_common.h es_i2s_fmt_t).
     * audio_codec_new_i2s_data() merely wraps the already-initialized I2S
     * channels, so the peripheral mode is fixed by i2s_channel_init_*_mode().
     * Switching to TDM therefore needs both sides together:
     * X: here, replace the two i2s_channel_init_std_mode() calls below with
     *    i2s_channel_init_tdm_mode() + an i2s_tdm_config_t (slot_mask,
     *    total_slot, ws_width, ...) for the frame the vendor datasheet
     *    specifies, keeping MCLK = 256*fs and master role;
     * Y: on the codec side, either patch the es8389 driver to program the
     *    DSP/TDM bits in regs 0x0C/0x20/0x40 via a new config field, or write
     *    those registers after open through audio_codec_if_t::set_reg() -
     *    note that set_fs() re-forces ES_I2S_NORMAL on every stream start. */
    const i2s_std_config_t default_config = {
        /* BSP_I2S_SAMPLE_RATE must stay a rate the es8389 coefficient table
         * knows (see the macro comment in bsp/candis_s31.h). */
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = s_i2s_gpio,
    };
    const i2s_std_config_t *config = i2s_config != NULL ?
                                     i2s_config : &default_config;
    error = i2s_channel_init_std_mode(s_tx_channel, config);
    if (error != ESP_OK) {
        goto fail;
    }
    error = i2s_channel_init_std_mode(s_rx_channel, config);
    if (error != ESP_OK) {
        goto fail;
    }
    error = i2s_channel_enable(s_tx_channel);
    if (error != ESP_OK) {
        goto fail;
    }
    error = i2s_channel_enable(s_rx_channel);
    if (error != ESP_OK) {
        goto fail;
    }

    audio_codec_i2s_cfg_t codec_i2s_config = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_rx_channel,
        .tx_handle = s_tx_channel,
    };
    s_data_if = audio_codec_new_i2s_data(&codec_i2s_config);
    if (s_data_if == NULL) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    return ESP_OK;

fail:
    bsp_audio_deinit();
    return error;
}

esp_err_t bsp_audio_deinit(void)
{
    esp_err_t result = ESP_OK;
    if (bsp_audio_codec_deinit(s_speaker.device) != ESP_OK) {
        result = ESP_FAIL;
    }
    if (bsp_audio_codec_deinit(s_microphone.device) != ESP_OK) {
        result = ESP_FAIL;
    }
    if (s_data_if != NULL) {
        if (audio_codec_delete_data_if(s_data_if) != ESP_CODEC_DEV_OK) {
            result = ESP_FAIL;
        }
        s_data_if = NULL;
    }
    if (s_tx_channel != NULL) {
        esp_err_t error = i2s_channel_disable(s_tx_channel);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            result = error;
        }
        error = i2s_del_channel(s_tx_channel);
        if (error != ESP_OK) {
            result = error;
        }
        s_tx_channel = NULL;
    }
    if (s_rx_channel != NULL) {
        esp_err_t error = i2s_channel_disable(s_rx_channel);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            result = error;
        }
        error = i2s_del_channel(s_rx_channel);
        if (error != ESP_OK) {
            result = error;
        }
        s_rx_channel = NULL;
    }
    if (bsp_power_domain_set(BSP_POWER_AUDIO_PA, false) != ESP_OK &&
            result == ESP_OK) {
        result = ESP_FAIL;
    }
    if (bsp_peripheral_power_set(BSP_PERIPHERAL_AUDIO, false) != ESP_OK &&
            result == ESP_OK) {
        result = ESP_FAIL;
    }
    return result;
}

const audio_codec_data_if_t *bsp_audio_get_codec_itf(void)
{
    return s_data_if;
}

static const audio_codec_ctrl_if_t *new_codec_control(void)
{
    if (bsp_i2c_init() != ESP_OK) {
        return NULL;
    }
    /* ES8389_CODEC_DEFAULT_ADDR is 0x20, the 8-bit write address in the
     * esp_codec_dev convention. On the wire the codec answers at 7-bit 0x10
     * (AD0 and AD1 both strapped low: R63/R68 100kOhm pull-downs fitted,
     * R62/R65 DNP); audio_codec_new_i2c_ctrl shifts the 8-bit form. */
    audio_codec_i2c_cfg_t config = {
        .port = BSP_I2C_NUM,
        .addr = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    return audio_codec_new_i2c_ctrl(&config);
}

static esp_err_t delete_codec_instance(codec_instance_t *instance)
{
    esp_err_t result = ESP_OK;
    if (instance->device != NULL) {
        esp_codec_dev_delete(instance->device);
        instance->device = NULL;
    }
    if (instance->codec != NULL) {
        if (audio_codec_delete_codec_if(instance->codec) != ESP_CODEC_DEV_OK) {
            result = ESP_FAIL;
        }
        instance->codec = NULL;
    }
    if (instance->control != NULL) {
        if (audio_codec_delete_ctrl_if(instance->control) != ESP_CODEC_DEV_OK) {
            result = ESP_FAIL;
        }
        instance->control = NULL;
    }
    if (instance->gpio != NULL) {
        if (audio_codec_delete_gpio_if(instance->gpio) != ESP_CODEC_DEV_OK) {
            result = ESP_FAIL;
        }
        instance->gpio = NULL;
    }
    return result;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (s_speaker.device != NULL) {
        return s_speaker.device;
    }
    if (bsp_audio_init(NULL) != ESP_OK) {
        return NULL;
    }
    s_speaker.control = new_codec_control();
    s_speaker.gpio = audio_codec_new_gpio();
    if (s_speaker.control == NULL || s_speaker.gpio == NULL) {
        delete_codec_instance(&s_speaker);
        return NULL;
    }
    /* Hardware gain values verified against the board schematic rev 0.5,
     * audio sheet (page 8): the codec U19 AVDD/DVDD/PVDD pins and the
     * microphone coupling networks (R69/R70 = MIC1, R78/R79 = MIC2) all sit
     * directly on AUDIO_3V3_SW, which is TG28 ALDO3 (set to
     * 3.3 V by the audio power-up in bsp_power.c). R60 is only the AGND-to-GND
     * single-point link. The class-D PA (NS4150B, U22) VCC comes from
     * TG28_VSYS - the TG28 battery/system rail, about 3.0-4.5 V depending
     * on charge state (the TG28 datasheet specifies a 2.6 V VOFF threshold).
     * pa_voltage therefore uses 4.2 V (the nominal Li-ion charge target)
     * rather than the 5.0 V copied from esp32_s31_korvo_1. Both values only
     * feed the esp_codec_dev hardware-gain dB math
     * (esp_codec_dev_col_calc_hw_gain: 20*log10(dac/pa)), not the analog
     * path itself. */
    const esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 4.2,
        .codec_dac_voltage = 3.3,
    };
    /* Use the same BCLK/no-DAC-reference policy as the microphone instance.
     * esp_codec_dev 1.6.x shares the physical ES8389 between both logical
     * devices, so these fields must not depend on creation order. At
     * 16 kHz/16-bit stereo, BCLK is 512 kHz and the x2 coefficient row is an
     * exact match. Post-reflow DAC-to-ADC loopback proved the digital DAC
     * path, and the operator separately confirmed every acoustic speaker
     * regression tone with this common policy. */
    es8389_codec_cfg_t codec_config = {
        .ctrl_if = s_speaker.control,
        .gpio_if = s_speaker.gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_AUDIO_PA_EN,
        .pa_reverted = BSP_AUDIO_PA_EN_ACTIVE_LEVEL == 0,
        .master_mode = false,
        .use_mclk = false,
        .no_dac_ref = true,
        .hw_gain = gain,
    };
    s_speaker.codec = es8389_codec_new(&codec_config);
    if (s_speaker.codec == NULL) {
        delete_codec_instance(&s_speaker);
        return NULL;
    }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_speaker.codec,
        .data_if = s_data_if,
    };
    s_speaker.device = esp_codec_dev_new(&device_config);
    if (s_speaker.device == NULL) {
        delete_codec_instance(&s_speaker);
    }
    return s_speaker.device;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (s_microphone.device != NULL) {
        return s_microphone.device;
    }
    if (bsp_audio_init(NULL) != ESP_OK) {
        return NULL;
    }
    s_microphone.control = new_codec_control();
    if (s_microphone.control == NULL) {
        return NULL;
    }
    es8389_codec_cfg_t codec_config = {
        .ctrl_if = s_microphone.control,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        /* Keep this identical to the speaker instance. On esp_codec_dev
         * 1.5.11 the two conflicting policies reset the same physical chip
         * and left 0x02/0x23/0xF0 in speaker state after mic -> speaker ->
         * mic, producing an exact-zero left ADC channel. Version 1.6.2 adds
         * physical-codec reuse; matching configs make that reuse independent
         * of which logical device is created first. */
        .use_mclk = false,
        .no_dac_ref = true,
    };
    s_microphone.codec = es8389_codec_new(&codec_config);
    if (s_microphone.codec == NULL) {
        delete_codec_instance(&s_microphone);
        return NULL;
    }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_microphone.codec,
        .data_if = s_data_if,
    };
    s_microphone.device = esp_codec_dev_new(&device_config);
    if (s_microphone.device == NULL) {
        delete_codec_instance(&s_microphone);
    }
    return s_microphone.device;
}

esp_err_t bsp_audio_codec_deinit(esp_codec_dev_handle_t device)
{
    if (device == NULL) {
        return ESP_OK;
    }
    if (device == s_speaker.device) {
        return delete_codec_instance(&s_speaker);
    }
    if (device == s_microphone.device) {
        return delete_codec_instance(&s_microphone);
    }
    return ESP_ERR_NOT_FOUND;
}
