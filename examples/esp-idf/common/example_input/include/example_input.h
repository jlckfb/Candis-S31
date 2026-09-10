/*
 * Candis-S31 standalone examples - physical input helper.
 *
 * Extracted from the watch demo's svc_input.c: BOOT key polling on GPIO61
 * (external 10k pull-up, idle high) plus PWR short-press and RTC alarm
 * events from the shared IRQ line (GPIO2).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EXAMPLE_INPUT_BOOT_SHORT = 0,  /*< BOOT released before 800 ms */
    EXAMPLE_INPUT_BOOT_LONG,       /*< BOOT held >= 800 ms; fires at the threshold, release is swallowed */
    EXAMPLE_INPUT_PWR_SHORT,       /*< PWR short press (TG28 INT_STATUS1 bit3); long press powers off in hardware */
    EXAMPLE_INPUT_RTC_ALARM,       /*< RX8130CE alarm flag drained from the shared IRQ line */
} example_input_event_t;

typedef void (*example_input_cb_t)(example_input_event_t ev, void *user);

/*
 * Start the input task: BOOT is polled every 5 ms with 30 ms debounce,
 * PWR/RTC events are serviced in task context on the shared IRQ line.
 * Events are delivered to cb from the input task context.
 */
esp_err_t example_input_start(example_input_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
