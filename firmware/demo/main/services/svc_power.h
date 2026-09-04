/*
 * Candis-S31 watch demo - power service.
 *
 * Polls the TG28 every 2 s for battery/charge state, tracks UI idle time and
 * drives screen-off / deep-sleep / shutdown.
 *
 * Screen-off sequence (panel-only; the touch controller is never reset):
 *   off: stop UI refresh (lv_display_enable_invalidation false) + disable
 *        indev -> bsp_display_enter_sleep_panel(). The CST820 is left
 *        running and auto-enters standby, which keeps INT wake alive.
 *   on:  bsp_display_exit_sleep_panel() -> re-enable invalidation + indev
 *        + full refresh. The panel sleep/wake hardware sequences run
 *        OUTSIDE the LVGL lock (backlight/SLPIN/SLPOUT + settle delay
 *        are pure hardware); the lock brackets only the LVGL
 *        bookkeeping, so taskLVGL is never blocked across the >=120 ms
 *        transitions. Zero touch-controller resets: a reset landing on
 *        a touched panel poisons the baseline and latches phantom touches
 *        (2026-08-21 post-wake CPU storm root cause).
 * Wake sources while off: touch (50 ms poll, two samples - the CST820 INT
 * level on GPIO3 plus a direct report-register poll over I2C, because the
 * INT pulse width in the screen-off power tier is unverified on EVT),
 * BOOT key, PWR key. The radios are parked while the screen is off
 * (svc_net_suspend_rf) and restored on wake.
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

/** Charge-controller phase. The service ramps the TG28 REG62 charge
 *  current-limit target 50 -> 100 -> 200 -> 300 -> 400 -> 500 mA under
 *  evidence gates; the value is a configured ceiling, not a measured
 *  current (the TG28 power path may back it off under VINDPM at any
 *  time). FAULT latches until VBUS is removed and re-attached. */
typedef enum {
    SVC_POWER_CHARGE_IDLE = 0, /**< no VBUS; REG62 parked at 50 mA */
    SVC_POWER_CHARGE_TRICKLE,  /**< VBUS applied, observing at 50 mA */
    SVC_POWER_CHARGE_RAMP,     /**< stepping the target upward */
    SVC_POWER_CHARGE_HOLD,     /**< at ceiling or charge-done, monitoring */
    SVC_POWER_CHARGE_FAULT,    /**< collapsed to 50 mA, latched until replug */
} svc_power_charge_phase_t;

typedef struct {
    int battery_mv;
    /**< TG28 SOC percent, or -1 when there is no battery or no valid model
     * this boot. A voltage-derived percent must never be substituted. */
    int percent;
    bool present;
    bool vbus;
    bool charging;
    bool charge_done;
    /** True while the TG28 SOC is backed by the verified factory ROM model
     * or a successfully programmed runtime override; see percent. */
    bool fuel_gauge_valid;
    /** True while the active model is the TG28 factory ROM model. Its SOC is
     * reference accuracy, never per-battery calibrated. False = custom model
     * or no valid model. */
    bool fuel_gauge_reference_model;

    /** Verified REG62 target written by the charge controller (mA); 0
     *  while no VBUS session is active. Target/register, not measurement. */
    int charge_target_ma;
    svc_power_charge_phase_t charge_phase;
    /** Per-session user confirmation that an external (non-PC) source is
     *  verified. False at every boot and every VBUS replug: firmware
     *  cannot classify C1 sources, so the safe default is the 200 mA
     *  PC-safe charge ceiling. */
    bool source_verified;
    /** Active charge ceiling (200 safe / 500 verified), a configured
     *  limit - not a measured current. */
    int charge_ceiling_ma;
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


/**
 * Per-session charge-ceiling confirmation. verified=true unlocks the
 * 300-500 mA REG62 charge steps for THIS VBUS session only; the caller
 * must have actually confirmed an external power source (user action) -
 * firmware cannot tell a PC USB port from a charger on C1, and REG16 is
 * a fixed relaxed 2000 mA board default, so the 200 mA default ceiling
 * is risk reduction, not a hard total-input guarantee. verified=false
 * returns to the 200 mA ceiling: any target above 200 is driven down
 * (verified write) on the power task promptly. Never persists across
 * unplug or reboot. Safe from any task (queued internally).
 */
esp_err_t svc_power_set_external_source_verified(bool verified);
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
