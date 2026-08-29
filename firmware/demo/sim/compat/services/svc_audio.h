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
/* Busy in the arbitration sense (spec C.5): true while recording, saving
 * the WAV, or running a capture_analyze/loopback diagnostic - any state
 * where the mic path is owned. */
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
/* Busy in the arbitration sense (spec C.5): true while playing a file or
 * a diagnostic tone. */
bool svc_audio_is_playing(void);

/** Pause/resume the running playback. */
esp_err_t svc_audio_play_pause(bool pause);

/** Live volume change during playback (0..100). */
esp_err_t svc_audio_set_volume(int volume);

/* ------------------------------------------------------------------ */
/* Diagnostics (test center; every call runs on the audio task so the */
/* single-owner codec discipline and the iron open order still hold). */
/* ------------------------------------------------------------------ */

/* Per-channel capture statistics, mirroring the factory microphone_test
 * judgement fields (peak / DC-removed RMS / DC / clip rate). */
typedef struct {
    uint16_t peak;         /**< largest |sample| */
    int16_t  minimum;
    int16_t  maximum;
    int32_t  dc;           /**< mean value (DC offset) */
    uint32_t ac_rms;       /**< RMS with DC removed */
    uint32_t clip_count;   /**< samples within 8 LSB of the rail */
    uint32_t sample_count;
    bool     live;         /**< p2p >= 16 && ac_rms >= 4 (factory rule) */
    bool     clipping;     /**< clip_count > 1% of sample_count */
} svc_audio_channel_stats_t;

typedef struct {
    svc_audio_channel_stats_t ch[2]; /**< logical ch0 = left, ch1 = right */
    uint32_t sample_rate;
    uint32_t captured_ms;
} svc_audio_capture_stats_t;

/* Codec DAC->ADC internal-mix loopback result (factory codec_loopback). */
typedef struct {
    uint32_t tx_blocks;      /**< speaker blocks written */
    uint32_t sample_count;   /**< scored RX samples (2 pipeline blocks dropped) */
    uint32_t nonzero_samples;
    uint16_t peak;           /**< largest |sample| in the scored window */
    uint32_t asdout_edges;   /**< BSP_I2S_DIN edges during a 20 ms window */
} svc_audio_loopback_stats_t;

/* SoC I2S internal loopback result (factory i2s_loopback). */
typedef struct {
    uint32_t bytes_written;
    uint32_t bytes_read;
    uint32_t nonzero_bytes;
    bool     pattern_found;
    uint32_t pattern_offset; /**< valid when pattern_found */
} svc_audio_i2s_loopback_stats_t;

/**
 * Play a square-wave tone through the resident speaker handle (factory
 * speaker_test synthesis: 512-frame blocks, +-2200). freq_hz 100..8000,
 * volume_pct 0..100 (clamped), duration_ms 100..10000 or 0 to run until
 * svc_audio_tone_stop() (60 s safety cap). The call returns once the
 * tone has STARTED; it stops by itself after duration_ms.
 * ESP_ERR_INVALID_STATE when the codec is busy (record/play/tone/diag).
 */
esp_err_t svc_audio_tone_start(uint32_t freq_hz, int volume_pct,
                               uint32_t duration_ms);
esp_err_t svc_audio_tone_stop(void);

/**
 * One-shot capture + analysis, blocking the caller for the whole
 * duration: records duration_ms (100..10000) into a PSRAM buffer without
 * touching the SD card, then computes per-channel statistics (factory
 * microphone_test algorithm). route selects the raw hardware input
 * route (LEFT/RIGHT/STEREO only; no software channel processing or
 * denoise). gain_db is snapped to the PGA grid (0..36, 3 dB steps).
 * Returns ESP_OK when the capture completed; the live/clipping verdict
 * inside out_stats is left to the caller. ESP_ERR_INVALID_STATE when
 * the codec is busy.
 */
esp_err_t svc_audio_capture_analyze(uint32_t duration_ms,
                                    svc_audio_route_t route, int gain_db,
                                    svc_audio_capture_stats_t *out_stats);

/**
 * ES8389 DAC-to-ADC internal-mix loopback (factory codec_loopback):
 * bypasses the microphones entirely. Returns ESP_OK only when the
 * factory pass criteria hold (writer ran, >50% nonzero samples,
 * peak > 1000, ASDOUT edges seen); out_stats is filled whenever the
 * sequence ran, so evidence survives a FAIL. ESP_ERR_INVALID_STATE
 * when the codec is busy.
 */
esp_err_t svc_audio_codec_loopback(svc_audio_loopback_stats_t *out_stats);

/**
 * SoC I2S internal loopback (factory i2s_loopback): loops GPIO8 TX back
 * to RX with a 100-byte pattern. This temporarily tears the resident
 * codec stack down and re-creates it afterwards (the codec rail stays
 * powered, matching the demo's resident model). Returns ESP_OK only
 * when the pattern is found; ESP_FAIL also when the resident stack
 * could not be restored (audio service then stays degraded until
 * reboot). ESP_ERR_INVALID_STATE when the codec is busy.
 */
esp_err_t svc_audio_i2s_loopback(svc_audio_i2s_loopback_stats_t *out_stats);

/* ------------------------------------------------------------------ */
/* Simulator extensions (not present in the firmware build).          */
/* ------------------------------------------------------------------ */

/** Toggle the mock SD card presence for recording (default: present).
 * svc_audio_record_start() fails with ESP_ERR_INVALID_STATE while the
 * card is absent, mirroring the "no mounted card" firmware path. */
void sim_audio_set_sd_present(bool present);

#ifdef __cplusplus
}
#endif
