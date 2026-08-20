/*
 * Candis-S31 watch demo - modal on-screen keyboard.
 *
 * Wraps lv_keyboard + lv_textarea for text entry (WiFi password etc.).
 * The callback receives the entered UTF-8 text (valid only during the
 * callback; copy if needed) or NULL on cancel. Runs on LVGL context.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_keyboard_open(const char *title, const char *initial,
                      bool password_mode,
                      void (*cb)(const char *text, void *user), void *user);

#ifdef __cplusplus
}
#endif
