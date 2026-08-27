/*
 * Candis-S31 watch demo - POWER domain test suite (spec D.1).
 *
 * Read-only PMIC diagnostics plus the two interactive power paths.
 * C10 red line: the TG28 charge state machine is svc_power's safety-
 * reviewed code - this file NEVER writes a charge register (no
 * set_charge_current / set_input_current_limit / set_charge_voltage /
 * REG62/REG16/REG64 access of any kind); every PMIC touch below is a
 * documented read-only BSP getter.
 *
 * All functions run on the svc_test task; no LVGL calls (C.7). The
 * screen_off_wake and deep_sleep tests interact through svc_power's
 * public API only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/svc_power.h"
#include "sleep_record.h"
#include "test_registry.h"

#define SCREEN_WAKE_TIMEOUT_MS  30000
#define DEEP_SLEEP_MINUTES      1

static void result_fail(test_result_t *out, const char *evidence)
{
    out->st = TEST_ST_FAIL;
    strlcpy(out->evidence, evidence, sizeof(out->evidence));
}

/* ------------------------------------------------------------------ */
/* power.pmic_status (factory_power.c:58-97) + sanity range checks     */
/* ------------------------------------------------------------------ */

static void run_pmic_status(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    bsp_pmic_status_t status;
    uint8_t power_on_source = 0;
    uint16_t charge_current = 0;
    esp_err_t err = bsp_pmic_get_status(&status);
    if (err == ESP_OK) {
        err = bsp_pmic_get_power_on_source(&power_on_source);
    }
    if (err == ESP_OK) {
        err = bsp_pmic_get_charge_current(&charge_current);
    }
    if (err != ESP_OK) {
        result_fail(out, "TG28 status read failed");
        return;
    }
    const bool known_id = status.chip_id == 0x47 || status.chip_id == 0x4a;
    /* Range sanity: a fitted battery reports 3.0-4.45 V; "absent" must
     * come with a 0 mV readback so the two never disagree. */
    const bool vbat_sane = status.battery_present
            ? (status.battery_mv >= 3000 && status.battery_mv <= 4450)
            : (status.battery_mv == 0);
    snprintf(out->evidence, sizeof(out->evidence),
             "id=0x%02x vbat=%u soc=%u bat=%s vbus=%s src=0x%02x chg=%umA",
             status.chip_id, status.battery_mv, status.battery_percent,
             status.battery_present ? "yes" : "no",
             status.vbus_present ? "yes" : "no",
             power_on_source, charge_current);
    out->st = (known_id && vbat_sane) ? TEST_ST_PASS : TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* power.rails_dump (factory_power.c rail dump :569+, read-only path)  */
/* ------------------------------------------------------------------ */

/* DLDO1/DLDO2 are OTP-strapped load switches on this board, not
 * adjustable LDOs (factory_power.c rail_uses_otp_switch). */
static bool rail_uses_otp_switch(bsp_pmic_regulator_t regulator,
                                 bsp_pmic_switch_t *sw)
{
    if (regulator == BSP_PMIC_DLDO1) {
        *sw = BSP_PMIC_SWITCH_DC1SW;
        return true;
    }
    if (regulator == BSP_PMIC_DLDO2) {
        *sw = BSP_PMIC_SWITCH_DC4SW;
        return true;
    }
    return false;
}

static void run_rails_dump(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    int enabled_count = 0;
    int total = 0;
    int errors = 0;
    char first_anomaly[24] = {0};

    for (int index = 0; index < BSP_PMIC_REGULATOR_COUNT; ++index) {
        const bsp_pmic_regulator_t regulator = (bsp_pmic_regulator_t)index;
        bsp_pmic_switch_t unused_switch = BSP_PMIC_SWITCH_COUNT;
        if (rail_uses_otp_switch(regulator, &unused_switch)) {
            continue;
        }
        ++total;
        bool enabled = false;
        uint16_t millivolts = 0;
        const esp_err_t enable_error =
            bsp_pmic_regulator_is_enabled(regulator, &enabled);
        const esp_err_t voltage_error =
            bsp_pmic_regulator_get_voltage(regulator, &millivolts);
        if (enable_error != ESP_OK || voltage_error != ESP_OK) {
            ++errors;
            if (first_anomaly[0] == '\0') {
                snprintf(first_anomaly, sizeof(first_anomaly), "%s:read",
                         bsp_pmic_regulator_name(regulator));
            }
            continue;
        }
        if (enabled) {
            ++enabled_count;
            if (millivolts == 0 && first_anomaly[0] == '\0') {
                snprintf(first_anomaly, sizeof(first_anomaly), "%s:0mV",
                         bsp_pmic_regulator_name(regulator));
            }
        } else if (first_anomaly[0] == '\0') {
            /* The display (ALDO1/ALDO2) and codec (ALDO3) rails must be
             * up while the demo runs; anything else off is normal. */
            if (regulator == BSP_PMIC_ALDO1 || regulator == BSP_PMIC_ALDO2 ||
                regulator == BSP_PMIC_ALDO3) {
                snprintf(first_anomaly, sizeof(first_anomaly), "%s:off",
                         bsp_pmic_regulator_name(regulator));
            }
        }
    }

    /* OTP switches (DC1SW feeds the LED, DC4SW is a spare): read-only
     * state check, not counted as rails. */
    int switch_errors = 0;
    static const bsp_pmic_switch_t switches[] = {
        BSP_PMIC_SWITCH_DC1SW, BSP_PMIC_SWITCH_DC4SW,
    };
    for (size_t index = 0; index < sizeof(switches) / sizeof(switches[0]);
         ++index) {
        bool enabled = false;
        if (bsp_pmic_switch_is_enabled(switches[index], &enabled) != ESP_OK) {
            ++switch_errors;
        }
    }
    errors += switch_errors;

    if (errors > 0) {
        snprintf(out->evidence, sizeof(out->evidence),
                 "%d/%d rails on, %d read error(s) (%s)",
                 enabled_count, total, errors,
                 first_anomaly[0] ? first_anomaly : "?");
        out->st = TEST_ST_FAIL;
    } else if (first_anomaly[0] != '\0') {
        snprintf(out->evidence, sizeof(out->evidence),
                 "%d/%d rails on, anomaly %s",
                 enabled_count, total, first_anomaly);
        out->st = TEST_ST_WARN;
    } else {
        snprintf(out->evidence, sizeof(out->evidence),
                 "%d/%d rails on, 2 OTP switches ok",
                 enabled_count, total);
        out->st = TEST_ST_PASS;
    }
}

/* ------------------------------------------------------------------ */
/* power.charge_check (factory_power.c:409) - READ-ONLY (C10)          */
/* ------------------------------------------------------------------ */

static void run_charge_check(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    /* Read-only chain: service snapshot + BSP read-back getters. No
     * charge-register write exists anywhere in this file (C10). */
    svc_power_status_t svc;
    svc_power_get_status(&svc);

    bsp_pmic_status_t status;
    uint16_t input_limit = 0;
    uint16_t charge_voltage = 0;
    esp_err_t err = bsp_pmic_get_status(&status);
    if (err == ESP_OK) {
        err = bsp_pmic_get_input_current_limit(&input_limit);
    }
    if (err == ESP_OK) {
        err = bsp_pmic_get_charge_voltage(&charge_voltage);
    }
    if (err != ESP_OK) {
        result_fail(out, "TG28 charger read failed");
        return;
    }
    if (!status.vbus_present) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "no vbus; charger path not exercised",
                sizeof(out->evidence));
        return;
    }

    const bool limit_is_baseline =
        input_limit == BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA;
    const bool limit_is_verified = svc.source_verified &&
            (input_limit == 500 || input_limit == 900 || input_limit == 1000 ||
             input_limit == 1500 || input_limit == 2000);
    if (!limit_is_baseline && !limit_is_verified) {
        snprintf(out->evidence, sizeof(out->evidence),
                 "unverified input limit=%u mA (expected %u mA)",
                 input_limit, BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA);
        out->st = TEST_ST_FAIL;
        return;
    }
    if (status.charging || status.charge_done) {
        snprintf(out->evidence, sizeof(out->evidence),
                 "%s, limit=%u mA target=%u mV ceiling=%u mA",
                 status.charging ? "charging" : "charge done",
                 input_limit, charge_voltage, svc.charge_ceiling_ma);
        out->st = TEST_ST_PASS;
        return;
    }
    snprintf(out->evidence, sizeof(out->evidence),
             "vbus present but not charging (limit=%u mA target=%u mV)",
             input_limit, charge_voltage);
    out->st = TEST_ST_WARN;
}

/* ------------------------------------------------------------------ */
/* power.screen_off_wake: panel sleep through svc_power, operator      */
/* touches to wake (svc_power.h wake contract).                        */
/* ------------------------------------------------------------------ */

static void run_screen_off_wake(const test_ctx_t *ctx, test_result_t *out)
{
    bool timed_out = false;
    const bool ready = ctx->ask_operator(
        ctx, "Screen turns off; touch to wake it. Ready?",
        SCREEN_WAKE_TIMEOUT_MS, &timed_out);
    if (timed_out) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "operator timeout", sizeof(out->evidence));
        return;
    }
    if (!ready) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "operator declined", sizeof(out->evidence));
        return;
    }
    if (ctx->cancel_requested(ctx)) {
        return;
    }

    const esp_err_t err = svc_power_screen_off();
    if (err != ESP_OK) {
        result_fail(out, "screen off refused");
        return;
    }

    ctx->progress(ctx, 50, "Screen off - touch to wake");
    const int64_t start_us = esp_timer_get_time();
    const int64_t deadline = start_us +
                             (int64_t)SCREEN_WAKE_TIMEOUT_MS * 1000;
    bool woke = false;
    while (esp_timer_get_time() < deadline) {
        if (ctx->cancel_requested(ctx)) {
            break;
        }
        if (!svc_power_is_screen_off()) {
            woke = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!woke) {
        if (ctx->cancel_requested(ctx)) {
            /* Best effort: never leave the panel asleep behind our back. */
            svc_power_screen_on();
            return; /* NOT_RUN -> runner records aborted */
        }
        svc_power_screen_on();
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "no wake within timeout",
                sizeof(out->evidence));
        return;
    }
    snprintf(out->evidence, sizeof(out->evidence),
             "woke after %.1f s, panel restored",
             (double)((esp_timer_get_time() - start_us) / 100000) / 10.0);
    out->st = TEST_ST_PASS;
}

/* ------------------------------------------------------------------ */
/* power.deep_sleep: confirm -> NVS stash (C.6) -> svc_power deep      */
/* sleep 1 min; success never returns (reboot).                        */
/* ------------------------------------------------------------------ */

static void run_deep_sleep(const test_ctx_t *ctx, test_result_t *out)
{
    bool timed_out = false;
    const bool yes = ctx->ask_operator(
        ctx, "Deep sleep 1 min, then reboot. Continue?",
        SCREEN_WAKE_TIMEOUT_MS, &timed_out);
    if (timed_out) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "operator timeout", sizeof(out->evidence));
        return;
    }
    if (!yes) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "operator declined", sizeof(out->evidence));
        return;
    }
    if (ctx->cancel_requested(ctx)) {
        return;
    }

    /* Stash {requested minutes, RTC time} so the next boot can show what
     * happened (spec C.6; one NVS write per sleep, wear negligible). */
    const esp_err_t stash_err = demo_sleep_record_stash(DEEP_SLEEP_MINUTES);

    ctx->progress(ctx, 90, "Entering deep sleep");
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the run view paint the stage */

    const esp_err_t err = svc_power_deep_sleep(DEEP_SLEEP_MINUTES);
    /* Only reached when deep sleep was refused. */
    snprintf(out->evidence, sizeof(out->evidence),
             "deep sleep refused: %s%s", esp_err_to_name(err),
             stash_err != ESP_OK ? " (stash failed)" : "");
    out->st = TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

void test_power_register(void)
{
    static const test_case_t s_cases[] = {
        {
            .id = "power.pmic_status", .name = "PMIC status",
            .domain = TEST_DOM_POWER, .flags = 0,
            .timeout_ms = 5000, .run = run_pmic_status,
        },
        {
            .id = "power.rails_dump", .name = "Rails snapshot",
            .domain = TEST_DOM_POWER, .flags = 0,
            .timeout_ms = 10000, .run = run_rails_dump,
        },
        {
            .id = "power.charge_check", .name = "Charge grading",
            .domain = TEST_DOM_POWER, .flags = 0,
            .timeout_ms = 5000, .run = run_charge_check,
        },
        {
            .id = "power.screen_off_wake", .name = "Screen off & wake",
            .domain = TEST_DOM_POWER, .flags = TEST_F_INTERACTIVE,
            .timeout_ms = 80000, .run = run_screen_off_wake,
        },
        {
            .id = "power.deep_sleep", .name = "Deep sleep 1min",
            .domain = TEST_DOM_POWER,
            .flags = TEST_F_INTERACTIVE | TEST_F_NO_RUNALL,
            .timeout_ms = 60000, .run = run_deep_sleep,
        },
    };
    for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); ++i) {
        test_register(&s_cases[i]);
    }
}
