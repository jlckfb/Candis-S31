/*
 * Candis-S31 watch demo - last deep-sleep record (redesign spec C.6).
 *
 * The power.deep_sleep test stashes {requested minutes, RTC calendar time}
 * into the NVS "demo" namespace, key "lastsleep", right before entering
 * deep sleep (one write per sleep; wear is negligible). After the wake
 * reboot, app_power reads the blob and shows it next to the decoded
 * esp_reset_reason() (factory wake_info semantics,
 * factory_low_power.c:357). svc_power itself is untouched.
 *
 * Header-only so both tests/test_power.c and apps/app_power.c /
 * apps/app_sysinfo.c share one blob layout (the CMake GLOB
 * only picks up tests/test_*.c, so helpers cannot live in a separate .c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs.h"

#define DEMO_SLEEP_RECORD_NAMESPACE "demo"
#define DEMO_SLEEP_RECORD_KEY       "lastsleep"
#define DEMO_SLEEP_RECORD_MAGIC     UINT32_C(0x534c5031) /* "SLP1" */

typedef struct {
    uint32_t magic;             /* DEMO_SLEEP_RECORD_MAGIC */
    uint32_t requested_min;     /* RTC-alarm minutes requested (0 = key only) */
    bsp_rtc_time_t rtc_at_request; /* calendar time when sleep was armed */
} demo_sleep_record_t;

/** Persist the record; best-effort - a failure must not block the sleep. */
static inline esp_err_t demo_sleep_record_stash(int minutes)
{
    demo_sleep_record_t rec = {
        .magic = DEMO_SLEEP_RECORD_MAGIC,
        .requested_min = (uint32_t)minutes,
    };
    bsp_rtc_status_t status;
    if (bsp_rtc_get_time(&rec.rtc_at_request, &status) != ESP_OK) {
        rec.rtc_at_request = (bsp_rtc_time_t){0};
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEMO_SLEEP_RECORD_NAMESPACE, NVS_READWRITE,
                             &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, DEMO_SLEEP_RECORD_KEY, &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/** Load the record; false when none was stashed (first boot / erased). */
static inline bool demo_sleep_record_load(demo_sleep_record_t *out)
{
    nvs_handle_t handle;
    if (nvs_open(DEMO_SLEEP_RECORD_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t length = sizeof(*out);
    const esp_err_t err = nvs_get_blob(handle, DEMO_SLEEP_RECORD_KEY, out,
                                       &length);
    nvs_close(handle);
    return err == ESP_OK && length == sizeof(*out) &&
           out->magic == DEMO_SLEEP_RECORD_MAGIC;
}

/** Short reset-reason name (factory_low_power.c reset_reason_name). */
static inline const char *demo_reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:  return "task_watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    default:                return "other";
    }
}
