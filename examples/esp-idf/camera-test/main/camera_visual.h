/*
 * Live camera preview surface for the camera-test example.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t presented;  /* Frames handed to LVGL for the next refresh. */
    uint32_t refreshed;  /* Refreshes that completed and released a buffer. */
    uint32_t superseded; /* Frames replaced before they reached the panel. */
} camera_visual_stats_t;

/** Cumulative LVGL handoff counters, not panel scanout measurements. */
camera_visual_stats_t camera_visual_get_stats(void);

/** Bring up the AMOLED and the full-screen preview surface. */
esp_err_t camera_visual_start(void);

/** Copy one RGB565X source frame's centre crop into a display buffer.
 *  Returns false when no buffer is free or the frame is too short. */
bool camera_visual_publish(const uint8_t *frame, size_t length,
                           uint32_t stride);

/** Show a non-recoverable preview error without calling LVGL off-thread. */
void camera_visual_set_error(const char *message);
