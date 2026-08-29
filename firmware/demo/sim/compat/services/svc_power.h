/*
 * Candis-S31 simulator - power service shim.
 *
 * API mirrors firmware/demo/main/services/svc_power.h verbatim (status
 * struct, events, control functions) plus the sim_power_set_preview_state()
 * URL-state injection used by sim_ui.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Charge-controller phase (see the firmware header for semantics). */
typedef enum {
    SVC_POWER_CHARGE_IDLE = 0,
    SVC_POWER_CHARGE_TRICKLE,
    SVC_POWER_CHARGE_RAMP,
    SVC_POWER_CHARGE_HOLD,
    SVC_POWER_CHARGE_FAULT,
} svc_power_charge_phase_t;

typedef struct {
    int battery_mv;
    int percent;
    bool present;
    bool vbus;
    bool charging;
    bool charge_done;
    bool fuel_gauge_valid;
    bool fuel_gauge_reference_model;
    int charge_target_ma;
    svc_power_charge_phase_t charge_phase;
    bool source_verified;
    int charge_ceiling_ma;
} svc_power_status_t;

typedef enum {
    SVC_POWER_EV_UPDATE = 0,
    SVC_POWER_EV_CHARGE_START,
    SVC_POWER_EV_CHARGE_DONE,
    SVC_POWER_EV_VBUS_OFF,
    SVC_POWER_EV_SCREEN_OFF,
    SVC_POWER_EV_SCREEN_ON,
} svc_power_event_t;

typedef void (*svc_power_cb_t)(const svc_power_status_t *st,
                               svc_power_event_t ev, void *user);

esp_err_t svc_power_start(svc_power_cb_t cb, void *user);
esp_err_t svc_power_set_external_source_verified(bool verified);
void svc_power_get_status(svc_power_status_t *out);
void svc_power_activity(void);
void svc_power_set_screen_timeout(int seconds);
esp_err_t svc_power_screen_off(void);
esp_err_t svc_power_screen_on(void);
bool svc_power_is_screen_off(void);
esp_err_t svc_power_deep_sleep(int wake_after_min);
esp_err_t svc_power_shutdown(void);

/* Sim extensions: URL ?state= injection (called before ui_manager_init). */
void sim_power_set_preview_state(const char *state);

#ifdef __cplusplus
}
#endif
