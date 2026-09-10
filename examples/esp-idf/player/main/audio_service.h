/*
 * Candis-S31 player audio render adapter.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_start(void);
esp_err_t audio_prepare_playback(int volume);
esp_err_t audio_configure_playback(uint32_t sample_rate);
esp_err_t audio_write_pcm(uint8_t *pcm, uint32_t length);
void audio_finish_playback(void);
esp_err_t audio_set_volume(int volume);

#ifdef __cplusplus
}
#endif
