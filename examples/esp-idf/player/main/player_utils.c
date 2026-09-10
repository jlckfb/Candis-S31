/*
 * Candis-S31 player shared pure helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "player_utils.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static bool extension_is(const char *extension, const char *expected)
{
    while (*extension != '\0' && *expected != '\0') {
        if (tolower((unsigned char)*extension) != tolower((unsigned char)*expected)) {
            return false;
        }
        ++extension;
        ++expected;
    }
    return *extension == '\0' && *expected == '\0';
}

bool player_media_file_supported(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    return dot != NULL && extension_is(dot, ".avi");
}

int player_clamp_percent(int percent)
{
    if (percent < 0) {
        return 0;
    }
    return percent > 100 ? 100 : percent;
}

uint64_t player_percent_to_ms(int percent, uint64_t duration_ms)
{
    const uint64_t value = (uint64_t)player_clamp_percent(percent);
    return (duration_ms / 100U) * value + ((duration_ms % 100U) * value) / 100U;
}

int player_ms_to_percent(uint64_t position_ms, uint64_t duration_ms)
{
    if (duration_ms == 0) {
        return 0;
    }
    if (position_ms >= duration_ms) {
        return 100;
    }
    return (int)((position_ms / duration_ms) * 100U +
                 ((position_ms % duration_ms) * 100U) / duration_ms);
}
