/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include "esp_log.h"
#include "esp_board_manager_err.h"
#include "periph_i2s.h"

#if defined(__has_include)
#if __has_include("hal/i2s_periph.h")
#include "hal/i2s_periph.h"
#elif __has_include("soc/i2s_periph.h")
#include "soc/i2s_periph.h"
#else
#error "Missing I2S peripheral signal header: expected hal/i2s_periph.h or soc/i2s_periph.h"
#endif  /* __has_include("hal/i2s_periph.h") */
#else
#include "soc/i2s_periph.h"
#endif  /* defined(__has_include) */

static const char *TAG = "PERIPH_I2S";

typedef struct {
    i2s_chan_handle_t  chan_in;        /*!< I2S input channel handle */
    i2s_chan_handle_t  chan_out;       /*!< I2S output channel handle */
    uint8_t            in_en     : 1;  /*!< Input channel hardware enable flag */
    uint8_t            out_en    : 1;  /*!< Output channel hardware enable flag */
    uint8_t            in_owned  : 1;  /*!< RX handle returned to a consumer */
    uint8_t            out_owned : 1;  /*!< TX handle returned to a consumer */
} periph_i2s_chan_t;

static periph_i2s_chan_t i2s_chan_handles[SOC_I2S_NUM] = {0};

static void periph_i2s_release_channel(i2s_chan_handle_t *handle, bool enabled, const char *label)
{
    if (handle == NULL || *handle == NULL) {
        return;
    }
    if (enabled) {
        i2s_channel_disable(*handle);
    }
    ESP_LOGW(TAG, "Caution: Releasing %s (%p).", label, *handle);
    i2s_del_channel(*handle);
    *handle = NULL;
}

static void periph_i2s_reclaim_port(periph_i2s_chan_t *slot)
{
    /**
     * STD/TDM may keep TX enabled as a clock helper for RX.
     * Only tear down a direction when no consumer still owns it,
     * and keep TX alive while RX is still owned.
     */
    if (!slot->out_owned && slot->in_owned) {
        return;
    }

    if (slot->out_owned && !slot->in_owned) {
        periph_i2s_release_channel(&slot->chan_in, slot->in_en, "RX");
        slot->in_en = false;
        return;
    }

    if (!slot->out_owned && !slot->in_owned) {
        periph_i2s_release_channel(&slot->chan_in, slot->in_en, "RX");
        periph_i2s_release_channel(&slot->chan_out, slot->out_en, "TX");
        slot->in_en = false;
        slot->out_en = false;
    }
}

esp_err_t periph_i2s_get_data_out_signal(const char *name, int line, int *sig_idx)
{
    if (name == NULL || name[0] == '\0' || sig_idx == NULL || line < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    periph_i2s_config_t *i2s_cfg = NULL;
    esp_err_t ret = esp_board_periph_get_config(name, (void **)&i2s_cfg);
    if (ret != ESP_OK || i2s_cfg == NULL) {
        ESP_LOGE(TAG, "Get I2S peripheral config failed, name:%s err:%s", name, esp_err_to_name(ret));
        if (ret == ESP_BOARD_ERR_PERIPH_NOT_FOUND) {
            return ESP_ERR_NOT_FOUND;
        }
        if (ret == ESP_BOARD_ERR_PERIPH_NOT_SUPPORTED) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        return ESP_ERR_INVALID_ARG;
    }

    int port = i2s_cfg->port;
    if (port < 0 || port >= SOC_I2S_NUM) {
        ESP_LOGE(TAG, "I2S peripheral '%s' has invalid port: %d", name, port);
        return ESP_ERR_INVALID_ARG;
    }

    int signal = -1;
    const i2s_signal_conn_t *sig_conn = &i2s_periph_signal[port];

    if (line == 0) {
#if SOC_I2S_PDM_MAX_TX_LINES > 0
        signal = sig_conn->data_out_sigs[0];
#else
        signal = sig_conn->data_out_sig;
#endif  /* SOC_I2S_PDM_MAX_TX_LINES > 0 */
    } else {
#if SOC_I2S_PDM_MAX_TX_LINES > 1
        if (line >= SOC_I2S_PDM_MAX_TX_LINES) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        signal = sig_conn->data_out_sigs[line];
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif  /* SOC_I2S_PDM_MAX_TX_LINES > 1 */
    }

    /* Unsupported lines may be encoded as 0xFF (from uint8_t -1), and on some targets
     * (e.g. esp32h2) line>0 can resolve to 0 when only data_out_sig is initialized. */
    if (signal < 0 || signal == 0xFF || (line > 0 && signal == 0)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *sig_idx = signal;
    return ESP_OK;
}

int periph_i2s_init(void *cfg, int cfg_size, void **periph_handle)
{
    if (!cfg || !periph_handle || cfg_size < sizeof(periph_i2s_config_t)) {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }
    periph_i2s_config_t *config = (periph_i2s_config_t *)cfg;
    if (config->port >= SOC_I2S_NUM) {
        ESP_LOGE(TAG, "Port number is invalid, %d", config->port);
        return -1;
    }
    esp_err_t err = ESP_OK;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(config->port, config->role);
    chan_cfg.auto_clear = true;
    periph_i2s_chan_t *slot = &i2s_chan_handles[config->port];

    if (config->mode == I2S_COMM_MODE_PDM) {
        if (config->direction == I2S_DIR_RX && slot->chan_in == NULL) {
            err = i2s_new_channel(&chan_cfg, NULL, &slot->chan_in);
        } else if (config->direction == I2S_DIR_TX && slot->chan_out == NULL) {
            err = i2s_new_channel(&chan_cfg, &slot->chan_out, NULL);
        }
    } else if (slot->chan_out == NULL && slot->chan_in == NULL) {
        /**
         * NOTE
         * For STD/TDM duplex use cases the clock may be driven by TX, so keep
         * allocating the paired channel for compatibility with existing boards.
         * PDM RX boards such as ESP32-P4-EYE work with an RX-only channel and
         * should not expose an uninitialized paired TX handle to esp_codec_dev.
         **/
        err = i2s_new_channel(&chan_cfg, &slot->chan_out, &slot->chan_in);
    } else if (config->direction == I2S_DIR_RX && slot->chan_in == NULL) {
        err = i2s_new_channel(&chan_cfg, NULL, &slot->chan_in);
    } else if (config->direction == I2S_DIR_TX && slot->chan_out == NULL) {
        err = i2s_new_channel(&chan_cfg, &slot->chan_out, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S[%d] new channel failed: %d", config->port, err);
        return -1;
    }
    if (config->mode == I2S_COMM_MODE_STD) {
        if (config->direction == I2S_DIR_TX && !slot->out_en) {
            err = i2s_channel_init_std_mode(slot->chan_out, &config->i2s_cfg.std);
            err = i2s_channel_enable(slot->chan_out);
            slot->out_en = true;
        } else if (config->direction == I2S_DIR_RX && !slot->in_en) {
            /* TX enabled here is a clock helper only; do not mark out_owned. */
            if (slot->out_en == false) {
                err = i2s_channel_init_std_mode(slot->chan_out, &config->i2s_cfg.std);
                err = i2s_channel_enable(slot->chan_out);
                slot->out_en = true;
            }
            err = i2s_channel_init_std_mode(slot->chan_in, &config->i2s_cfg.std);
            err = i2s_channel_enable(slot->chan_in);
            slot->in_en = true;
        } else {
            ESP_LOGW(TAG, "I2S[%d] STD already enabled, tx:%p, rx:%p", config->port, slot->chan_out, slot->chan_in);
        }
        ESP_LOGI(TAG, "I2S[%d] STD, %s, ws: %d, bclk: %d, dout: %d, din: %d", config->port,
                 config->direction == I2S_DIR_TX ? " TX" : "RX", config->i2s_cfg.std.gpio_cfg.ws, config->i2s_cfg.std.gpio_cfg.bclk,
                 config->i2s_cfg.std.gpio_cfg.dout, config->i2s_cfg.std.gpio_cfg.din);
    }
#if CONFIG_SOC_I2S_SUPPORTS_TDM
    else if (config->mode == I2S_COMM_MODE_TDM) {
        if (config->direction == I2S_DIR_TX && !slot->out_en) {
            err = i2s_channel_init_tdm_mode(slot->chan_out, &config->i2s_cfg.tdm);
            err = i2s_channel_enable(slot->chan_out);
            slot->out_en = true;
        } else if (config->direction == I2S_DIR_RX && !slot->in_en) {
            /* TX enabled here is a clock helper only; do not mark out_owned. */
            if (slot->out_en == false) {
                err = i2s_channel_init_tdm_mode(slot->chan_out, &config->i2s_cfg.tdm);
                err = i2s_channel_enable(slot->chan_out);
                slot->out_en = true;
            }
            err = i2s_channel_init_tdm_mode(slot->chan_in, &config->i2s_cfg.tdm);
            err = i2s_channel_enable(slot->chan_in);
            slot->in_en = true;
        } else {
            ESP_LOGW(TAG, "I2S[%d] TDM already enabled, tx:%p, rx:%p", config->port, slot->chan_out, slot->chan_in);
        }
        ESP_LOGI(TAG, "I2S[%d] TDM, %s, ws: %d, bclk: %d, dout: %d, din: %d", config->port,
                 config->direction == I2S_DIR_TX ? " TX" : "RX", config->i2s_cfg.tdm.gpio_cfg.ws, config->i2s_cfg.tdm.gpio_cfg.bclk,
                 config->i2s_cfg.tdm.gpio_cfg.dout, config->i2s_cfg.tdm.gpio_cfg.din);
    }
#endif  // CONFIG_SOC_I2S_SUPPORTS_TDM
#if CONFIG_SOC_I2S_SUPPORTS_PDM
    else if (config->mode == I2S_COMM_MODE_PDM) {
        if (config->direction == I2S_DIR_TX && !slot->out_en) {
#if CONFIG_SOC_I2S_SUPPORTS_PDM_TX
            err = i2s_channel_init_pdm_tx_mode(slot->chan_out, &config->i2s_cfg.pdm_tx);
            err = i2s_channel_enable(slot->chan_out);
            slot->out_en = true;
            ESP_LOGI(TAG, "I2S[%d] PDM-TX, clk: %d, dout: %d", config->port, config->i2s_cfg.pdm_tx.gpio_cfg.clk, config->i2s_cfg.pdm_tx.gpio_cfg.dout);
#endif  // CONFIG_SOC_I2S_SUPPORTS_PDM_TX
        } else if (config->direction == I2S_DIR_RX && !slot->in_en) {
#if CONFIG_SOC_I2S_SUPPORTS_PDM_RX
            err = i2s_channel_init_pdm_rx_mode(slot->chan_in, &config->i2s_cfg.pdm_rx);
            err = i2s_channel_enable(slot->chan_in);
            slot->in_en = true;
            ESP_LOGI(TAG, "I2S[%d] PDM-RX, clk: %d, din: %d", config->port, config->i2s_cfg.pdm_rx.gpio_cfg.clk, config->i2s_cfg.pdm_rx.gpio_cfg.din);
#endif  // CONFIG_SOC_I2S_SUPPORTS_PDM_RX
        } else {
            ESP_LOGW(TAG, "I2S[%d] PDM already enabled, tx:%p, rx:%p", config->port, slot->chan_out, slot->chan_in);
        }
    }
#endif  // CONFIG_SOC_I2S_SUPPORTS_PDM
    else {
        ESP_LOGE(TAG, "I2S[%d] Invalid mode: %d", config->port, config->mode);
        return -1;
    }
    if (err != ESP_OK) {
        if (!slot->in_owned) {
            periph_i2s_release_channel(&slot->chan_in, slot->in_en, "RX");
            slot->in_en = false;
        }
        if (!slot->out_owned) {
            periph_i2s_release_channel(&slot->chan_out, slot->out_en, "TX");
            slot->out_en = false;
        }
        ESP_LOGE(TAG, "I2S[%d] initialize failed: %d", config->port, err);
        return -1;
    }
    if (config->direction == I2S_DIR_TX) {
        slot->out_owned = true;
        *periph_handle = slot->chan_out;
    } else if (config->direction == I2S_DIR_RX) {
        slot->in_owned = true;
        *periph_handle = slot->chan_in;
    } else {
        ESP_LOGE(TAG, "I2S[%d] Invalid direction: %d", config->port, config->direction);
        return -1;
    }
    ESP_LOGI(TAG, "I2S[%d] initialize success: %p", config->port, *periph_handle);
    return 0;
}

int periph_i2s_deinit(void *periph_handle)
{
    if (!periph_handle) {
        ESP_LOGE(TAG, "Invalid handle");
        return -1;
    }
    i2s_chan_handle_t handle = (i2s_chan_handle_t)periph_handle;
    for (size_t i = 0; i < SOC_I2S_NUM; i++) {
        periph_i2s_chan_t *slot = &i2s_chan_handles[i];

        if (slot->chan_out == handle) {
            if (!slot->out_owned) {
                ESP_LOGW(TAG, "I2S[%u] TX handle is not owned", (unsigned)i);
                return -1;
            }
            slot->out_owned = false;
            periph_i2s_reclaim_port(slot);
            return 0;
        }
        if (slot->chan_in == handle) {
            if (!slot->in_owned) {
                ESP_LOGW(TAG, "I2S[%u] RX handle is not owned", (unsigned)i);
                return -1;
            }
            slot->in_owned = false;
            periph_i2s_reclaim_port(slot);
            return 0;
        }
    }
    ESP_LOGE(TAG, "I2S handle not found: %p", periph_handle);
    return -1;
}
