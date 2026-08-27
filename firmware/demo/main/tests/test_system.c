/*
 * Candis-S31 watch demo - SYSTEM domain test suite (spec D.1).
 *
 * Ports the factory RTC / shared-IRQ / buttons / RGB-LED / die-temperature
 * checks onto the test framework. All functions run on the svc_test task;
 * no LVGL calls (C.7). BSP reads (RTC/PMIC IRQ banks) are short I2C and
 * allowed from this context; the RTC alarm program/clear pair is in the
 * write-BSP whitelist (C.7).
 *
 * Shared-IRQ discipline (F8): the svc_input service owns the GPIO2
 * callback. The tests below never re-register it - the RTC alarm flag is
 * observed through svc_input_add_listener() (SVC_INPUT_RTC_ALARM) and the
 * PWR key through SVC_INPUT_PWR_SHORT, with svc_input_mute_primary()
 * keeping key presses from navigating away mid-test.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "led_hw.h"
#include "services/svc_input.h"
#include "test_registry.h"

#define BUTTONS_STEP_TIMEOUT_MS 30000
#define LED_OPERATOR_TIMEOUT_MS 30000
#define RTC_ALARM_POLL_MS       200

/* EVT1 schematic SW2: KEY_BOOT drives GPIO61, external 10k pull-up, idle
 * high (svc_input already configured the pad; the test only samples it). */
#define BOOT_BUTTON_GPIO GPIO_NUM_61

static void result_fail(test_result_t *out, const char *evidence)
{
    out->st = TEST_ST_FAIL;
    strlcpy(out->evidence, evidence, sizeof(out->evidence));
}

/* ------------------------------------------------------------------ */
/* sys.rtc_read (factory_rtc.c:22)                                     */
/* ------------------------------------------------------------------ */

static void run_rtc_read(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    bsp_rtc_time_t time;
    bsp_rtc_status_t status;
    const esp_err_t err = bsp_rtc_get_time(&time, &status);
    if (err != ESP_OK) {
        result_fail(out, "RX8130CE read failed");
        return;
    }
    snprintf(out->evidence, sizeof(out->evidence),
             "%04u-%02u-%02u %02u:%02u:%02u flags=0x%02x%s",
             time.year, time.month, time.day, time.hour, time.minute,
             time.second, status.raw,
             status.backup_voltage_low ? " backup-low" : "");
    out->st = status.time_valid ? TEST_ST_PASS : TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* sys.rtc_alarm (factory_rtc.c:164-238) - next-minute alarm through   */
/* the shared IRQ line, observed via the svc_input listener.           */
/* ------------------------------------------------------------------ */

#define RTC_ALARM_EVT_BIT (1u << 0)

static void rtc_alarm_listener(svc_input_event_t ev, void *user)
{
    if (ev == SVC_INPUT_RTC_ALARM) {
        xEventGroupSetBits((EventGroupHandle_t)user, RTC_ALARM_EVT_BIT);
    }
}

static void run_rtc_alarm(const test_ctx_t *ctx, test_result_t *out)
{
    bsp_rtc_time_t now;
    bsp_rtc_status_t status;
    esp_err_t err = bsp_rtc_get_time(&now, &status);
    if (err != ESP_OK) {
        result_fail(out, "RX8130CE read failed");
        return;
    }

    EventGroupHandle_t events = xEventGroupCreate();
    if (events == NULL) {
        result_fail(out, "no memory for alarm wait");
        return;
    }

    /* Drain stale PMIC/RTC flags so a fresh alarm can assert the line. */
    bsp_shared_irq_status_t serviced;
    err = bsp_shared_irq_service(&serviced);
    bool listener_attached = false;
    if (err == ESP_OK) {
        err = svc_input_add_listener(rtc_alarm_listener, events);
        listener_attached = err == ESP_OK;
    }

    /* The RX8130CE compares minute/hour/day fields only; the fastest
     * self-contained check targets the next minute boundary. */
    bsp_rtc_alarm_t alarm = {0};
    alarm.minute_en = true;
    alarm.minute = (uint8_t)((now.minute + 1) % 60);
    if (err == ESP_OK) {
        err = bsp_rtc_set_alarm(&alarm);
    }
    if (err == ESP_OK) {
        err = bsp_rtc_alarm_irq_enable(true);
    }

    bool fired = false;
    bool irq_notified = false;
    if (err == ESP_OK) {
        /* Re-read the clock: some seconds may have passed since "now". */
        if (bsp_rtc_get_time(&now, &status) != ESP_OK) {
            now.second = 0;
        }
        const unsigned wait_s = 60u - now.second + 8u;
        const int64_t start_us = esp_timer_get_time();
        const int64_t deadline = start_us + (int64_t)wait_s * 1000000;
        while (esp_timer_get_time() < deadline) {
            if (ctx->cancel_requested(ctx)) {
                break;
            }
            const int pct = (int)((esp_timer_get_time() - start_us) * 90 /
                                  ((int64_t)wait_s * 1000000));
            ctx->progress(ctx, pct, "Waiting for RTC alarm");
            const EventBits_t bits = xEventGroupWaitBits(
                events, RTC_ALARM_EVT_BIT, pdTRUE, pdFALSE,
                pdMS_TO_TICKS(RTC_ALARM_POLL_MS));
            if ((bits & RTC_ALARM_EVT_BIT) != 0) {
                fired = true;
                irq_notified = true;
                break;
            }
            /* Poll fallback: if the input service is stalled the latched
             * AF flag is still visible here (bsp_rtc_get_status does not
             * clear flags). */
            if (bsp_rtc_get_status(&status) == ESP_OK && status.alarm) {
                fired = true;
                break;
            }
        }
    }

    /* Full cleanup chain (F7): disable the alarm IRQ, disarm the compare,
     * clear AF, detach the listener - deep sleep reuses this hardware. */
    const bsp_rtc_alarm_t disarm = {0};
    bsp_rtc_alarm_irq_enable(false);
    bsp_rtc_set_alarm(&disarm);
    bsp_rtc_get_and_clear_alarm_flag(NULL);
    if (listener_attached) {
        svc_input_add_listener(NULL, NULL);
    }
    vEventGroupDelete(events);

    if (err != ESP_OK) {
        snprintf(out->evidence, sizeof(out->evidence),
                 "alarm setup failed: %s", esp_err_to_name(err));
        out->st = TEST_ST_FAIL;
        return;
    }
    if (!fired && ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records aborted */
    }
    snprintf(out->evidence, sizeof(out->evidence),
             "minute=%02u %s shared_irq=%s", alarm.minute,
             fired ? "fired" : "no flag within timeout",
             irq_notified ? "yes" : "no");
    out->st = fired ? TEST_ST_PASS : TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* sys.shared_irq (factory_rtc.c:139)                                  */
/* ------------------------------------------------------------------ */

static void run_shared_irq(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    bsp_shared_irq_status_t status = {0};
    const esp_err_t err = bsp_shared_irq_service(&status);
    snprintf(out->evidence, sizeof(out->evidence),
             "passes=%u released=%s pmic=%02x:%02x:%02x rtc=0x%02x",
             status.service_passes, status.line_released ? "yes" : "no",
             status.pmic[0], status.pmic[1], status.pmic[2], status.rtc);
    out->st = (err == ESP_OK && status.line_released) ? TEST_ST_PASS
                                                      : TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* sys.buttons (factory_input.c:33,70-122)                             */
/* BOOT: direct GPIO61 pulse polling; PWR: svc_input listener event.   */
/* ------------------------------------------------------------------ */

#define PWR_EVT_BIT (1u << 0)

static void buttons_listener(svc_input_event_t ev, void *user)
{
    if (ev == SVC_INPUT_PWR_SHORT) {
        xEventGroupSetBits((EventGroupHandle_t)user, PWR_EVT_BIT);
    }
}

/* Polarity-independent pulse wait (factory_input.c:33): a press is the
 * non-idle level held for at least 30 ms, followed by a release back to
 * idle before the deadline. */
static bool wait_button_pulse(const test_ctx_t *ctx, int gpio,
                              int64_t deadline_us)
{
    const int idle = gpio_get_level(gpio);
    while (esp_timer_get_time() < deadline_us) {
        if (ctx->cancel_requested(ctx)) {
            return false;
        }
        if (gpio_get_level(gpio) == idle) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const int64_t press_start = esp_timer_get_time();
        bool held_30ms = false;
        bool released = false;
        while (esp_timer_get_time() < deadline_us) {
            if (gpio_get_level(gpio) == idle) {
                released = true;
                break;
            }
            if (esp_timer_get_time() - press_start >= 30000) {
                held_30ms = true;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (released) {
            if (held_30ms) {
                return true;
            }
            continue; /* too short: keep waiting for a real press */
        }
        return false; /* still held at the deadline: not a pulse */
    }
    return false;
}

static void run_buttons(const test_ctx_t *ctx, test_result_t *out)
{
    EventGroupHandle_t events = xEventGroupCreate();
    if (events == NULL) {
        result_fail(out, "no memory for key wait");
        return;
    }
    esp_err_t err = svc_input_add_listener(buttons_listener, events);
    if (err != ESP_OK) {
        vEventGroupDelete(events);
        result_fail(out, "input listener slot busy");
        return;
    }
    /* Mute navigation while the keys are exercised: a BOOT release or a
     * PWR short press would otherwise pop the run view and abort. */
    svc_input_mute_primary(true);

    ctx->progress(ctx, 5, "Press and release BOOT");
    const int64_t boot_deadline = esp_timer_get_time() +
                                  (int64_t)BUTTONS_STEP_TIMEOUT_MS * 1000;
    const bool boot_seen = wait_button_pulse(ctx, BOOT_BUTTON_GPIO,
                                             boot_deadline);

    bool pwr_seen = false;
    if (!ctx->cancel_requested(ctx)) {
        ctx->progress(ctx, 55, "Short-press PWR key");
        const EventBits_t bits = xEventGroupWaitBits(
            events, PWR_EVT_BIT, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(BUTTONS_STEP_TIMEOUT_MS));
        pwr_seen = (bits & PWR_EVT_BIT) != 0;
    }

    svc_input_mute_primary(false);
    svc_input_add_listener(NULL, NULL);
    vEventGroupDelete(events);

    if (!(boot_seen && pwr_seen) && ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records aborted */
    }
    snprintf(out->evidence, sizeof(out->evidence), "boot=%s power=%s",
             boot_seen ? "yes" : "no", pwr_seen ? "yes" : "no");
    out->st = (boot_seen && pwr_seen) ? TEST_ST_PASS : TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* sys.led_rgb (factory_input.c:124-176)                               */
/* ------------------------------------------------------------------ */

static void run_led_rgb(const test_ctx_t *ctx, test_result_t *out)
{
    const esp_err_t err = led_hw_acquire();
    if (err != ESP_OK) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, err == ESP_ERR_INVALID_STATE
                        ? "led busy" : "led init failed",
                sizeof(out->evidence));
        return;
    }

    static const struct {
        uint32_t rgb;
        const char *stage;
    } steps[] = {
        { SET_IRGB(0, 64, 0, 0), "red" },
        { SET_IRGB(0, 0, 64, 0), "green" },
        { SET_IRGB(0, 0, 0, 64), "blue" },
    };
    for (int i = 0; i < 3; ++i) {
        if (ctx->cancel_requested(ctx)) {
            break;
        }
        led_hw_set_rgb(steps[i].rgb);
        ctx->progress(ctx, 5 + i * 25, steps[i].stage);
        const int64_t until = esp_timer_get_time() + 700000;
        while (esp_timer_get_time() < until) {
            if (ctx->cancel_requested(ctx)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    bool timed_out = false;
    bool confirmed = false;
    if (!ctx->cancel_requested(ctx)) {
        confirmed = ctx->ask_operator(
            ctx, "Did the LED cycle red, green, blue?",
            LED_OPERATOR_TIMEOUT_MS, &timed_out);
    }
    led_hw_release();

    if (timed_out) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "operator timeout", sizeof(out->evidence));
        return;
    }
    if (ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records aborted */
    }
    snprintf(out->evidence, sizeof(out->evidence),
             "operator %s R-G-B sequence", confirmed ? "confirmed" : "rejected");
    out->st = confirmed ? TEST_ST_PASS : TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* sys.temp (factory_low_power.c:176-257) - SoC die estimate only      */
/* ------------------------------------------------------------------ */

static void run_temp(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    temperature_sensor_handle_t sensor = NULL;
    temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&config, &sensor);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(sensor);
    }
    if (err != ESP_OK) {
        if (sensor != NULL) {
            temperature_sensor_uninstall(sensor);
        }
        result_fail(out, "temp sensor setup failed");
        return;
    }

    float minimum = 0.0f;
    float maximum = 0.0f;
    float total = 0.0f;
    int samples = 0;
    for (; samples < 5; ++samples) {
        float celsius = 0.0f;
        err = temperature_sensor_get_celsius(sensor, &celsius);
        if (err != ESP_OK) {
            break;
        }
        if (samples == 0 || celsius < minimum) {
            minimum = celsius;
        }
        if (samples == 0 || celsius > maximum) {
            maximum = celsius;
        }
        total += celsius;
        if (samples + 1 < 5) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    temperature_sensor_disable(sensor);
    temperature_sensor_uninstall(sensor);
    if (err != ESP_OK || samples == 0) {
        result_fail(out, "temp read failed");
        return;
    }
    snprintf(out->evidence, sizeof(out->evidence),
             "die min=%.1f max=%.1f avg=%.1f C (not ambient)",
             (double)minimum, (double)maximum, (double)(total / samples));
    out->st = TEST_ST_PASS;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

void test_system_register(void)
{
    static const test_case_t s_cases[] = {
        {
            .id = "sys.rtc_read", .name = "RTC read",
            .domain = TEST_DOM_SYSTEM, .flags = 0,
            .timeout_ms = 5000, .run = run_rtc_read,
        },
        {
            /* ~70 s worst case: next minute boundary plus margin (NO_RUNALL). */
            .id = "sys.rtc_alarm", .name = "RTC alarm 1min",
            .domain = TEST_DOM_SYSTEM,
            .flags = TEST_F_LONG | TEST_F_NO_RUNALL,
            .timeout_ms = 90000, .run = run_rtc_alarm,
        },
        {
            .id = "sys.shared_irq", .name = "Shared IRQ line",
            .domain = TEST_DOM_SYSTEM, .flags = 0,
            .timeout_ms = 10000, .run = run_shared_irq,
        },
        {
            .id = "sys.buttons", .name = "BOOT+PWR keys",
            .domain = TEST_DOM_SYSTEM, .flags = TEST_F_INTERACTIVE,
            .timeout_ms = 90000, .run = run_buttons,
        },
        {
            .id = "sys.led_rgb", .name = "RGB LED colors",
            .domain = TEST_DOM_SYSTEM, .flags = TEST_F_INTERACTIVE,
            .timeout_ms = 60000, .run = run_led_rgb,
        },
        {
            .id = "sys.temp", .name = "SoC temperature",
            .domain = TEST_DOM_SYSTEM, .flags = 0,
            .timeout_ms = 10000, .run = run_temp,
        },
    };
    for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); ++i) {
        test_register(&s_cases[i]);
    }
}
