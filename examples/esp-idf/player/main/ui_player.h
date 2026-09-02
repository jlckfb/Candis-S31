/*
 * Candis-S31 player demo - UI interface.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize player UI and file browser. */
esp_err_t ui_player_init(void);

/** Add controls after the video backend has attached its frame object. */
esp_err_t ui_player_complete_init(void);

/** Refresh the file list (call after TF card mount/unmount). */
void ui_player_refresh_files(void);

/** Show/hide playback overlay. */
void ui_player_show_overlay(bool show);

/** Update progress text/position. */
void ui_player_set_progress(int percent);

#ifdef __cplusplus
}
#endif
