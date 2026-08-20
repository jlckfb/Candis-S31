/*
 * Candis-S31 watch demo - physical input service.
 *
 * BOOT key: GPIO61, external 10k pull-up, idle high, press to GND. Polled
 * every 5 ms with 30 ms debounce (factory_input.c pattern). Long press at
 * >= 800 ms hold.
 *
 * PWR key: no GPIO; delivered through the TG28 shared IRQ line (GPIO2,
 * level-triggered, wire-OR with RX8130CE). The service registers the BSP
 * shared-IRQ callback, services the line with bsp_shared_irq_service() and
 * reports the PWR short-press bit (IRQ bank1 bit3). Long-press powers the
 * board off in TG28 hardware and is NOT reported to the UI.
 *
 * Callbacks run on the service task context; UI work must go through
 * ui_async().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SVC_INPUT_BOOT_SHORT = 0,  /**< back / menu */
    SVC_INPUT_BOOT_LONG,       /**< screen off toggle */
    SVC_INPUT_PWR_SHORT,       /**< home */
} svc_input_event_t;

typedef void (*svc_input_cb_t)(svc_input_event_t ev, void *user);

/** Start the input task (stack 3072, prio 4). Single subscriber. */
esp_err_t svc_input_start(svc_input_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
