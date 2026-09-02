/*
 * Live camera preview and operator confirmation for camera_test.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    CAMERA_VISUAL_PENDING = 0,
    CAMERA_VISUAL_PASS,
    CAMERA_VISUAL_FAIL,
} camera_visual_decision_t;

typedef enum {
    CAMERA_BAYER_RGGB = 0,
    CAMERA_BAYER_BGGR,
    CAMERA_BAYER_GRBG,
    CAMERA_BAYER_GBRG,
    CAMERA_BAYER_COUNT,
} camera_visual_bayer_t;

/** Start the AMOLED/LVGL preview surface and its confirmation controls. */
esp_err_t camera_visual_start(void);

/** Copy one RGB565X frame crop into a display-owned buffer. */
bool camera_visual_publish(const uint8_t *frame, size_t length,
                           uint32_t stride);
/** Convert one UYVY frame crop into a display-owned RGB565 buffer. */
bool camera_visual_publish_uyvy(const uint8_t *frame, size_t length,
                                uint32_t stride);
/** Publish a deterministic four-quadrant RGB565 pattern for path isolation. */
bool camera_visual_publish_test_pattern(void);


/** Demosaic the high byte of a two-byte-per-pixel RAW10 DVP frame. */
bool camera_visual_publish_raw8_lane1(const uint8_t *frame, size_t length,
                                      uint32_t stride);

/** Publish four grayscale interpretations of the 960000-byte DMA stream. */
bool camera_visual_publish_pack_matrix(const uint8_t *frame, size_t length);

/** Publish the result of the automatic driver checks to the preview UI. */
void camera_visual_set_auto_state(bool passed, unsigned cycles,
                                  unsigned frames);

/** Tell the UI whether the final continuous preview is live. */
void camera_visual_set_streaming(bool streaming);

/** Return true when the image source uses RGB565_SWAPPED. */
bool camera_visual_is_swapped(void);

/** Return the Bayer phase currently selected by the on-screen button. */
camera_visual_bayer_t camera_visual_get_bayer(void);

/** Show a non-recoverable preview error without calling LVGL off-thread. */
void camera_visual_set_error(const char *message);

/** Read the operator's on-screen confirmation decision. */
camera_visual_decision_t camera_visual_get_decision(void);
