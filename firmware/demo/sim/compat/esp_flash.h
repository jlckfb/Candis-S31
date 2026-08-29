/*
 * Candis-S31 simulator - esp_flash shim (fixed 16 MB W25Q128).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_flash_get_size(void *chip, uint32_t *out_size_bytes);

#ifdef __cplusplus
}
#endif
