/*
 * Candis-S31 player demo - video player API.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_EV_STARTED = 0,
    PLAYER_EV_FINISHED,
    PLAYER_EV_ERROR,
    PLAYER_EV_PROGRESS, /**< value = playback percentage, 0..100 */
} player_event_t;

typedef void (*player_event_cb_t)(player_event_t ev, int value, void *user);

typedef struct {
    const char *video_path; /**< AVI (MJPG) file on TF card */
    int volume;             /**< 0..100 */
    bool loop;              /**< loop playback */
    player_event_cb_t cb;
    void *user;
} video_play_request_t;

/** Initialize video player (display, JPEG decoder). */
esp_err_t video_player_init(void);

/** Play a video. Non-blocking; playback runs on a dedicated task. */
esp_err_t video_player_play(const video_play_request_t *req);

/** Stop playback. */
esp_err_t video_player_stop(void);

/** Pause/resume. */
esp_err_t video_player_pause(bool pause);

/** Seek to approximate position (0..100 percent). */
esp_err_t video_player_seek(int percent);

/** Set brightness 0..100. */
esp_err_t video_player_set_brightness(int brightness);

/** Set volume 0..100. */
esp_err_t video_player_set_volume(int volume);
esp_err_t video_player_set_loop(bool loop);

/** True while playing or paused. */
bool video_player_is_active(void);

/** True while paused. */
bool video_player_is_paused(void);

#ifdef __cplusplus
}
#endif
