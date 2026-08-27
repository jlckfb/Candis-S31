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

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SVC_INPUT_BOOT_SHORT = 0,  /**< back / menu */
    SVC_INPUT_BOOT_LONG,       /**< screen off toggle */
    SVC_INPUT_PWR_SHORT,       /**< home */
    SVC_INPUT_RTC_ALARM,       /**< RTC alarm flag drained from the shared line */
} svc_input_event_t;

typedef void (*svc_input_cb_t)(svc_input_event_t ev, void *user);

/** Start the input task (stack 3072, prio 4). Single subscriber. */
esp_err_t svc_input_start(svc_input_cb_t cb, void *user);
/** Attach an extra single-slot listener that receives the same events as
 *  the primary callback (test-framework observers such as the buttons and
 *  rtc_alarm tests); the shared-IRQ registration itself is never touched
 *  (spec F8). Pass NULL to detach. ESP_ERR_INVALID_STATE when the slot is
 *  already taken. Callbacks run on the service task context. */
esp_err_t svc_input_add_listener(svc_input_cb_t cb, void *user);

/** While muted, the primary callback (UI navigation) receives nothing and
 *  only the listener sees events. The buttons test uses this so BOOT/PWR
 *  presses do not navigate away mid-test; the test's cleanup path always
 *  restores it. */
void svc_input_mute_primary(bool muted);

#ifdef __cplusplus
}
#endif
