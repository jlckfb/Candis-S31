/*
 * Candis-S31 watch demo - power service.
 *
 * Polls the TG28 every 2 s for battery/charge state, tracks UI idle time and
 * drives screen-off / deep-sleep / shutdown.
 *
 * Screen-off sequence (review-locked, mirrors examples/esp-idf/low-power):
 *   off: stop UI refresh (lv_display_enable_invalidation false) ->
 *        bsp_display_enter_sleep() -> esp_lcd_touch_cst820_wakeup() ->
 *        esp_lcd_touch_cst820_enter_monitor_mode()
 *   on:  exit_monitor_mode -> bsp_display_exit_sleep() -> re-enable
 *        invalidation + full refresh.
 * Wake sources while off: touch monitor interrupt, BOOT key, PWR key (all
 * detected by the service's own poll/IRQ path).
 *
 * Deep sleep copies the verified recipe from low_power_main.c: RTC alarm
 * (RX8130CE, no 32.768 kHz xtal on this board) + EXT1 ANY_LOW on GPIO2 with
 * rtc_gpio pull-up; the shared IRQ line must be serviced to line_released
 * before arming. Deep sleep reboots on wake.
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

typedef struct {
    int battery_mv;
    int percent;       /**< TG28 fuel gauge estimate; -1 when no battery */
    bool present;
    bool vbus;
    bool charging;
    bool charge_done;
} svc_power_status_t;

typedef enum {
    SVC_POWER_EV_UPDATE = 0,   /**< periodic status refresh */
    SVC_POWER_EV_CHARGE_START, /**< vbus/charging rising edge */
    SVC_POWER_EV_CHARGE_DONE,  /**< charge_done rising edge */
    SVC_POWER_EV_VBUS_OFF,     /**< vbus falling edge */
    SVC_POWER_EV_SCREEN_OFF,   /**< display entered sleep */
    SVC_POWER_EV_SCREEN_ON,    /**< display woke */
} svc_power_event_t;

typedef void (*svc_power_cb_t)(const svc_power_status_t *st,
                               svc_power_event_t ev, void *user);

/** Start the power task (stack 4096, prio 4). Single subscriber. */
esp_err_t svc_power_start(svc_power_cb_t cb, void *user);

/** Latest snapshot (copy). Valid before start too (zero-filled). */
void svc_power_get_status(svc_power_status_t *out);

/** Reset the idle timer; called by the UI on any user activity. */
void svc_power_activity(void);

/** Change the screen-off timeout (seconds, 0 = never). */
void svc_power_set_screen_timeout(int seconds);

/** Immediate screen off/on. Safe from any task (queued internally). */
esp_err_t svc_power_screen_off(void);
esp_err_t svc_power_screen_on(void);
bool svc_power_is_screen_off(void);

/**
 * Enter deep sleep; wakes on RTC alarm after wake_after_min minutes (0 =
 * alarm disabled, wake on PWR/IRQ line only). Does not return on success;
 * the boot after wake is a fresh reset.
 */
esp_err_t svc_power_deep_sleep(int wake_after_min);

/**
 * Power the board off via TG28 soft-PWROFF. The VBUS presence check runs
 * FIRST and refuses with ESP_ERR_INVALID_STATE while USB power is attached
 * (soft-PWROFF with VBUS reboots instead of cutting power); only then does
 * the BSP safe-shutdown chain run, followed by the PWROFF command.
 */
esp_err_t svc_power_shutdown(void);

#ifdef __cplusplus
}
#endif
