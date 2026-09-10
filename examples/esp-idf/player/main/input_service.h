/*
 * Candis-S31 player demo - physical input service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_EV_BOOT_SHORT = 0,
    INPUT_EV_BOOT_LONG,
    INPUT_EV_PWR_SHORT,
} input_event_t;

typedef void (*input_cb_t)(input_event_t ev, void *user);

esp_err_t input_start(input_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
