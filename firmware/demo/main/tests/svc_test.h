/*
 * Candis-S31 watch demo - test runner service (redesign spec C.3).
 *
 * A dedicated FreeRTOS task ("svc_test", stack 8192, prio 4) executes
 * registered tests strictly one at a time. UI/services submit requests
 * through a fixed queue of depth 8 (run / run-all / cancel); a full queue
 * refuses the request and the caller reports via toast.
 *
 * Runner -> UI uses two channels:
 *  - discrete events (test finished, queue drained) are posted through
 *    ui_async() to the single subscriber, on the LVGL thread;
 *  - high-frequency progress goes through a last-value mailbox
 *    (same pattern as the status bar) and is pulled by the run view with
 *    a 100 ms lv_timer. Progress reporting NEVER touches LVGL.
 *
 * Single subscriber: the test-center page subscribes; leaving the test
 * center (screen DELETE) unsubscribes. Queued AUTO tests keep running in
 * the background (results land in the library); an INTERACTIVE test is
 * cancelled immediately when its page exits because its canvas is about
 * to be destroyed.
 *
 * Cooperative cancellation: cancel_requested() flag, checked by test
 * functions between steps; a hard per-test timeout (test_case_t
 * .timeout_ms) arms a one-shot timer that forces the flag (and releases
 * a blocked ask_operator), recording SKIP "timeout".
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "test_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SVC_TEST_EV_STARTED = 0,    /* a test started; id carries which */
    SVC_TEST_EV_RESULT,         /* a test finished; result is in the library */
    SVC_TEST_EV_QUEUE_DRAINED,  /* run-all batch finished */
} svc_test_event_t;

/* Delivered on the LVGL thread. Single subscriber; subscribing NULL
 * unsubscribes. id is only valid during the callback (may be NULL). */
typedef void (*svc_test_cb_t)(svc_test_event_t ev, const char *id,
                              void *user);

/* Last-value progress mailbox snapshot (thread-safe). */
typedef struct {
    uint32_t seq;            /* bumps on every published update */
    char running_id[32];     /* test id being run; "" when idle */
    int pct;                 /* 0..100, -1 = indeterminate */
    char stage[48];          /* short stage text */
} svc_test_progress_t;

/* Operator-ask mailbox snapshot (thread-safe). While pending is true the
 * run view shows the ask panel; answer via svc_test_ask_answer(). */
typedef struct {
    uint32_t seq;            /* bumps on every new question */
    bool pending;
    char question[64];
    int timeout_ms;
    int64_t published_us;    /* esp_timer timestamp; UI derives countdown */
} svc_test_ask_t;

/* Interactive-canvas mailbox snapshot (thread-safe). When seq changes the
 * run view clears its content area and calls build(content, user) on the
 * LVGL thread. A NULL build with a bumped seq means "tear down". */
typedef struct {
    uint32_t seq;
    void (*build)(lv_obj_t *parent, void *user); /* NULL = none/tear down */
    void *user;
} svc_test_canvas_t;

/**
 * Start the test framework: registry init, test_register_all(), runner
 * task + queues. Degrades gracefully (returns the error) so demo_main
 * treats it like any other service.
 */
esp_err_t svc_test_start(void);

/** Queue a single run of test id. ESP_ERR_NO_MEM when the request queue
 *  is full, ESP_ERR_NOT_FOUND for an unknown id, ESP_ERR_INVALID_STATE
 *  when the screen is off (F6). */
esp_err_t svc_test_run(const char *id);

/** Queue every !INTERACTIVE && !NO_RUNALL test in the fixed domain order
 *  system -> memory -> accel -> display_touch -> camera -> audio ->
 *  storage -> network -> power (C.3). */
esp_err_t svc_test_run_all_auto(void);

/** Cancel the running test (cooperative) and drop every queued request. */
void      svc_test_cancel(void);

/** True while a test is running or requests remain queued. */
bool      svc_test_busy(void);

/** True while the running/queued work touches the given domain (used by
 *  feature apps for C.5 arbitration, e.g. app_usb before mounting). */
bool      svc_test_domain_busy(test_domain_t domain);

/** Single-slot event subscription; callbacks run on the LVGL thread. */
void      svc_test_subscribe(svc_test_cb_t cb, void *user);

/** Thread-safe snapshot of the progress mailbox. */
void      svc_test_progress_snapshot(svc_test_progress_t *out);

/** Thread-safe snapshot of the ask mailbox. */
void      svc_test_ask_snapshot(svc_test_ask_t *out);

/** Answer the pending operator question (LVGL thread). No-op when none. */
void      svc_test_ask_answer(bool yes);

/** Thread-safe snapshot of the canvas mailbox. */
void      svc_test_canvas_snapshot(svc_test_canvas_t *out);

#ifdef __cplusplus
}
#endif
