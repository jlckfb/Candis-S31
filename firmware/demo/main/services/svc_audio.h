/*
 * Candis-S31 watch demo - audio service (ES8389 via esp_codec_dev 1.6.2).
 *
 * Single audio task owns the codec; recorder and player are mutually
 * exclusive (ESP_ERR_INVALID_STATE when busy). Discipline inherited from
 * factory (DO NOT violate):
 *  - fmt: 16 bit / stereo / 16000 Hz, mclk_multiple 256; use_mclk=false,
 *    no_dac_ref=true on both logical devices (BSP-fixed).
 *  - Open order: speaker first, then mic; then set_out_vol, set_in_gain,
 *    then write the input route. The route is ES8389 reg 0x72 high nibble
 *    (0x50 = MIC1P single-ended, 0x60 = MIC2P single-ended) and MUST be
 *    re-applied after every set_in_gain (the driver rewrites 0x72 whole).
 *  - esp_codec_dev_open failure still requires esp_codec_dev_close.
 *  - First two RX blocks after open are pipeline garbage: discard.
 *  - Recording turns the PA power domain off (anti-feedback, lower noise).
 *  - No sample-rate switch / re-open while recording.
 *
 * Single-mic routes zero the opposite channel in software so "left only"
 * really is left only in the saved WAV. DENOISE records both channels and
 * applies the built-in basic noise reduction (high-pass + noise gate,
 * honest "基础降噪"; esp-sr S31 NS is a follow-up, not this iteration).
 *
 * Recording captures to PSRAM (64 KB/s at 16 kHz stereo), max 60 s, then
 * writes a 44-byte-header WAV to the SD card (requires mounted card).
 *
 * Callbacks run on the audio task context; UI work must go through
 * ui_async().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SVC_AUDIO_ROUTE_LEFT = 0,  /**< MIC1 only (MIC1P single-ended) */
    SVC_AUDIO_ROUTE_RIGHT,     /**< MIC2 only (MIC2P single-ended) */
    SVC_AUDIO_ROUTE_STEREO,    /**< both mics, default differential */
    SVC_AUDIO_ROUTE_DENOISE,   /**< both mics + basic noise reduction */
} svc_audio_route_t;

typedef enum {
    SVC_AUDIO_EV_LEVEL = 0,  /**< record level meter, value 0..100 */
    SVC_AUDIO_EV_RECORD_DONE,
    SVC_AUDIO_EV_PLAY_DONE,
    SVC_AUDIO_EV_PLAY_PROGRESS, /**< value = elapsed seconds */
    SVC_AUDIO_EV_ERROR,
} svc_audio_event_t;

typedef struct {
    svc_audio_event_t type;
    int value;             /**< level 0..100 / progress seconds */
    char path[96];         /**< RECORD_DONE: saved file path */
    esp_err_t err;         /**< ERROR: cause */
} svc_audio_event_msg_t;

typedef void (*svc_audio_cb_t)(const svc_audio_event_msg_t *ev, void *user);

/** Start the audio service: brings the codec/I2S up once (persistent
 * handles, see svc_audio.c) and then starts the audio task. */
esp_err_t svc_audio_start(void);

/**
 * Start recording. gain_db is clamped to the ES8389 PGA range 0..36 in
 * 3 dB steps. Emits LEVEL events ~5/s while recording and RECORD_DONE with
 * the file path after svc_audio_record_stop() (or auto-stop at the length
 * cap). Returns ESP_ERR_INVALID_STATE when no SD card is mounted or the
 * codec is busy.
 */
esp_err_t svc_audio_record_start(svc_audio_route_t route, int gain_db,
                                 svc_audio_cb_t cb, void *user);
esp_err_t svc_audio_record_stop(void);
/**
 * Fire-and-forget stop for page teardown: enqueues the stop without
 * waiting for the audio task (a synchronous stop can block up to
 * AUDIO_ACK_TIMEOUT_MS when the task is stuck on slow SD IO). The caller
 * must already have invalidated its audio event token; the completion
 * event may arrive after the page is gone.
 */
esp_err_t svc_audio_record_stop_async(void);
bool svc_audio_is_recording(void);

/** Adjust gain of the running recording (re-applies the route register). */
esp_err_t svc_audio_record_set_gain(int gain_db);

/**
 * Play a WAV file (PCM 16-bit, 1/2 ch, 8/16/44.1/48 kHz). volume 0..100.
 * Emits PLAY_PROGRESS 1/s and PLAY_DONE at EOF or after svc_audio_play_stop.
 */
esp_err_t svc_audio_play(const char *path, int volume,
                         svc_audio_cb_t cb, void *user);
esp_err_t svc_audio_play_stop(void);
/** Fire-and-forget stop for page teardown (see svc_audio_record_stop_async). */
esp_err_t svc_audio_play_stop_async(void);
bool svc_audio_is_playing(void);

/** Pause/resume the running playback. */
esp_err_t svc_audio_play_pause(bool pause);

/** Live volume change during playback (0..100). */
esp_err_t svc_audio_set_volume(int volume);

#ifdef __cplusplus
}
#endif
