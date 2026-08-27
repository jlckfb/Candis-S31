/*
 * Candis-S31 watch demo - declarative test registry (redesign spec C.2).
 *
 * Contract header for the test framework: status semantics aligned with
 * the factory firmware (NOT_RUN/PASS/FAIL/SKIP/WARN), nine domains,
 * declarative test_case_t registration and a thread-safe RAM result
 * library (mutex-guarded; session-scoped, exported to TF via
 * test_report.c, never written to NVS - spec C.6).
 *
 * LVGL threading red line (C.7): test run functions execute on the
 * svc_test task and MUST NOT call LVGL APIs directly; all UI interaction
 * goes through the test_ctx_t triple (progress / ask_operator /
 * request_canvas).
 *
 * Resource arbitration contract (C.5), shared across workstreams:
 *   codec audio channel : recorder/player apps vs tone/mic tests
 *       query svc_audio_is_recording()/svc_audio_is_playing();
 *       a busy codec makes the test SKIP "audio busy".
 *   camera stream       : app_camera preview vs camera tests
 *       query app_camera_stream_active() (added by WS2); the later
 *       consumer is refused.
 *   USB host stack      : app_usb vs usb enum/MSC tests
 *       query app_usb_host_busy() (added by WS3); the later consumer is
 *       refused.
 *   WS2812B             : app_led vs led tests
 *       led_hw_acquire()/led_hw_release() (tests/led_hw, added by WS4);
 *       the later consumer is refused.
 *   TF filesystem       : apps and tests share svc_storage_lease_acquire();
 *       a failed lease makes the test SKIP.
 *   WiFi/BLE            : serialized inside svc_net; handle
 *       ESP_ERR_INVALID_STATE from its API.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TEST_ST_NOT_RUN = 0, TEST_ST_PASS, TEST_ST_FAIL,
    TEST_ST_SKIP, TEST_ST_WARN
} test_status_t;                      /* aligned with the factory 5 tiers */

typedef enum {
    TEST_DOM_DISPLAY_TOUCH = 0, TEST_DOM_CAMERA, TEST_DOM_AUDIO,
    TEST_DOM_STORAGE, TEST_DOM_NETWORK, TEST_DOM_SYSTEM,
    TEST_DOM_MEMORY, TEST_DOM_ACCEL, TEST_DOM_POWER,
    TEST_DOM_COUNT                     /* 9 domains */
} test_domain_t;

#define TEST_F_INTERACTIVE   (1u<<0)  /* operator must see/hear/touch */
#define TEST_F_NEEDS_SD      (1u<<1)  /* needs a mounted TF card (else SKIP) */
#define TEST_F_NEEDS_USB_DISK (1u<<2) /* needs a USB disk (else SKIP/wait) */
#define TEST_F_NO_RUNALL     (1u<<3)  /* excluded from run-all (reboot/long) */
#define TEST_F_LONG          (1u<<4)  /* >10 s, run view forces progress bar */

typedef struct {
    test_status_t st;
    char     evidence[96];            /* short proof, e.g. "5 frames CRC ok" */
    uint32_t duration_ms;
    uint32_t run_seq;                 /* +1 per completion; UI dedups on it */
} test_result_t;

typedef struct test_ctx {
    /* Progress report (test-task context; internally a last-value
     * mailbox, never the LVGL lock). */
    void (*progress)(const struct test_ctx *ctx, int pct, const char *stage);
    /* Cancellation query: long loops must check this between steps. */
    bool (*cancel_requested)(const struct test_ctx *ctx);
    /* Operator confirm: the run view shows YES/NO and blocks (with a
     * timeout); on timeout returns false with *timed_out=true - the test
     * decides whether to record SKIP or FAIL. */
    bool (*ask_operator)(const struct test_ctx *ctx, const char *question,
                         int timeout_ms, bool *timed_out);
    /* Interactive canvas host: borrow the run-view content area for a
     * full-screen interaction (quadrants/touch/preview); build(parent,
     * user) runs on the LVGL thread and is torn down automatically when
     * the test ends or is cancelled. */
    void (*request_canvas)(const struct test_ctx *ctx,
                           void (*build)(lv_obj_t *parent, void *user),
                           void *user);
    void *user;
} test_ctx_t;

typedef void (*test_run_fn_t)(const test_ctx_t *ctx, test_result_t *out);

typedef struct {
    const char   *id;         /* unique key, e.g. "display.quadrant" */
    const char   *name;       /* short display name, English-first */
    test_domain_t domain;
    uint32_t      flags;
    int           timeout_ms; /* hard cap; on expiry the runner cancels and
                               * records SKIP "timeout" */
    test_run_fn_t run;
} test_case_t;

/* Result library: RAM-persistent for the session (trade-off in C.6).
 * Thread-safe (internal mutex). */
void               test_registry_init(void);   /* called by svc_test_start */
esp_err_t          test_register(const test_case_t *tc);
int                test_count(void);
const test_case_t *test_at(int index);         /* domain-sorted order */
int                test_domain_range(test_domain_t d, int *first, int *cnt);
const test_case_t *test_find(const char *id);
void               test_result_get(const char *id, test_result_t *out); /* copy */
void               test_results_reset(void);   /* everything NOT_RUN */
const char        *test_domain_name(test_domain_t d);   /* badge wall name */

/* --- Framework-internal extensions beyond spec C.2 -------------------- */

/* Runner-side result store (svc_test.c only); bumps run_seq. */
void               test_result_set(const char *id, const test_result_t *res);

/* Domain aggregate: FAIL > WARN > NOT_RUN > SKIP > PASS (C.1 priority;
 * SKIP sits between NOT_RUN and PASS so a fully skipped domain does not
 * read as a failure). */
test_status_t      test_domain_aggregate(test_domain_t d);

/* Count of tests eligible for run-all (!INTERACTIVE && !NO_RUNALL),
 * globally or within one domain (-1 for none). */
int                test_auto_count(test_domain_t d); /* d < 0 => global */

/* Registers every domain suite (tests/test_all.c); called once by
 * svc_test_start() after test_registry_init(). */
void               test_register_all(void);

#ifdef __cplusplus
}
#endif
