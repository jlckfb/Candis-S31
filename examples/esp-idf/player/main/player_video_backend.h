/* Candis-S31 TE-synchronized LVGL video backend. SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_video_render_backend.h"

esp_err_t player_video_backend_init(void);
esp_err_t player_video_backend_submit_rgb565(const uint8_t *data,
                                              uint16_t stride_width,
                                              uint16_t frame_height);
esp_err_t player_video_backend_submit_rgb888(const uint8_t *data,
                                              uint16_t stride_width,
                                              uint16_t frame_height);
esp_err_t player_video_backend_submit_yuv420(const uint8_t *data,
                                              uint16_t stride_width,
                                              uint16_t frame_height);

const esp_video_render_backend_ops_t *player_video_backend_get_ops(void);

/** Switch between direct full-frame panel writes and the LVGL menu layer. */
void player_video_backend_set_direct(bool enabled);

/** Configure aspect-fit output for the next decoded video track. */
void player_video_backend_set_source_size(uint16_t width, uint16_t height);
