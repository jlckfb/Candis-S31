/*
 * Candis-S31 player shared pure helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool player_media_file_supported(const char *name);
int player_clamp_percent(int percent);
uint64_t player_percent_to_ms(int percent, uint64_t duration_ms);
int player_ms_to_percent(uint64_t position_ms, uint64_t duration_ms);

#ifdef __cplusplus
}
#endif
