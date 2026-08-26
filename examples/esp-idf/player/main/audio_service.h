/*
 * Candis-S31 player demo - audio service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the audio service. */
esp_err_t audio_start(void);

/** Play a WAV file (PCM 16-bit, 1/2 ch, 8/16/44.1/48 kHz). */
esp_err_t audio_play_wav(const char *path, int volume);

/** Stop playback. */
esp_err_t audio_stop(void);

/** Pause/resume. */
esp_err_t audio_pause(bool pause);

/** Set volume 0..100. */
esp_err_t audio_set_volume(int volume);

/** True while playback is active. */
bool audio_is_playing(void);

/** Push PCM samples for streaming playback.
 *  fmt: 16-bit signed, stereo, at sample_rate Hz (see implementation).
 *  Returns ESP_OK if accepted, ESP_ERR_INVALID_STATE if not streaming. */
esp_err_t audio_stream_start(int sample_rate, int channels, int volume);
esp_err_t audio_stream_write(const int16_t *samples, size_t count);
esp_err_t audio_stream_stop(void);

#ifdef __cplusplus
}
#endif
