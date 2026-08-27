/*
 * Candis-S31 watch demo - test runner service (spec C.3/C.4).
 *
 * Serial executor on a dedicated task (stack 8192, prio 4). Requests
 * arrive on a fixed queue (depth 8, full => refuse); run-all expands into
 * an internal list walked between queue checks so CANCEL stays
 * responsive. Progress/ask/canvas state lives in last-value mailboxes
 * guarded by a critical section - the runner never touches LVGL; the run
 * view pulls them with a 100 ms lv_timer. Discrete events are posted to
 * the single subscriber through ui_async().
 *
 * Pre-run checks (C.4): NEEDS_SD => svc_storage_mounted(); audio-domain
 * tests => codec idle (svc_audio_is_recording/playing); screen off (F6)
 * rejects at submission and SKIPs at dispatch. Camera/USB-disk
 * arbitration interfaces (app_camera_stream_active / app_usb_host_busy)
 * land with WS2/WS3 and are then checked first thing inside the
 * respective test functions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc_test.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "services/svc_audio.h"
#include "services/svc_power.h"
#include "services/svc_storage.h"
#include "ui/ui_manager.h"

#include "test_registry.h"

#define SVC_TEST_STACK       8192
#define SVC_TEST_PRIO        4
#define SVC_TEST_QUEUE_DEPTH 8
#define SVC_TEST_KEEPALIVE_US (5 * 1000 * 1000)  /* F6: screen keep-alive */

#define ASK_BIT_YES (1u << 0)
#define ASK_BIT_NO  (1u << 1)

static const char *TAG = "svc_test";

typedef enum {
    REQ_RUN = 0,
    REQ_RUN_ALL,
    REQ_CANCEL,
} req_op_t;

typedef struct {
    uint8_t op;
    char id[32];
} req_t;

static StaticQueue_t s_req_queue_state;
static uint8_t s_req_queue_storage[SVC_TEST_QUEUE_DEPTH * sizeof(req_t)];
static QueueHandle_t s_req_queue;
static EventGroupHandle_t s_ask_events;
static TimerHandle_t s_timeout_timer;
static TaskHandle_t s_task;
static bool s_started;

/* Cancellation flags (written from UI/timer tasks, read by runner). */
static portMUX_TYPE s_flag_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_cancel_current;
static volatile bool s_timed_out;

/* Run-all expansion state (runner task only). */
static const test_case_t *s_auto_list[TEST_DOM_COUNT * 8]; /* >= 42 */
static int s_auto_len;
static int s_auto_idx;

/* Busy bookkeeping for svc_test_busy/domain_busy. */
static portMUX_TYPE s_busy_lock = portMUX_INITIALIZER_UNLOCKED;
static const test_case_t *s_current;        /* running case, runner-owned */
static uint8_t s_pending_by_domain[TEST_DOM_COUNT]; /* queued single runs */
static int s_runall_pending;                /* queued run-all requests */

/* Mailboxes (critical-section last-value, status-bar pattern). */
static portMUX_TYPE s_mb_lock = portMUX_INITIALIZER_UNLOCKED;
static svc_test_progress_t s_progress;
static svc_test_ask_t s_ask;
static svc_test_canvas_t s_canvas;
static int64_t s_last_keepalive_us;

/* Single event subscriber. */
static portMUX_TYPE s_sub_lock = portMUX_INITIALIZER_UNLOCKED;
static svc_test_cb_t s_sub_cb;
static void *s_sub_user;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void flags_set_cancel(bool timed_out)
{
    portENTER_CRITICAL(&s_flag_lock);
    s_cancel_current = true;
    s_timed_out = timed_out;
    portEXIT_CRITICAL(&s_flag_lock);
}

static void flags_clear(void)
{
    portENTER_CRITICAL(&s_flag_lock);
    s_cancel_current = false;
    s_timed_out = false;
    portEXIT_CRITICAL(&s_flag_lock);
}

static bool flags_cancelled(void)
{
    bool v;
    portENTER_CRITICAL(&s_flag_lock);
    v = s_cancel_current;
    portEXIT_CRITICAL(&s_flag_lock);
    return v;
}

static bool flags_timed_out(void)
{
    bool v;
    portENTER_CRITICAL(&s_flag_lock);
    v = s_timed_out;
    portEXIT_CRITICAL(&s_flag_lock);
    return v;
}

/* ------------------------------------------------------------------ */
/* Event delivery (discrete events -> ui_async -> LVGL thread)         */
/* ------------------------------------------------------------------ */

typedef struct {
    svc_test_event_t ev;
    char id[32];
} ev_post_t;

static void ev_deliver(void *arg)
{
    ev_post_t *post = (ev_post_t *)arg;
    svc_test_cb_t cb;
    void *user;
    portENTER_CRITICAL(&s_sub_lock);
    cb = s_sub_cb;
    user = s_sub_user;
    portEXIT_CRITICAL(&s_sub_lock);
    if (cb) {
        cb(post->ev, post->id[0] ? post->id : NULL, user);
    }
    free(post);
}

static void ev_post(svc_test_event_t ev, const char *id)
{
    ev_post_t *post = malloc(sizeof(*post));
    if (!post) {
        return;
    }
    post->ev = ev;
    if (id) {
        strlcpy(post->id, id, sizeof(post->id));
    } else {
        post->id[0] = '\0';
    }
    if (!ui_async(ev_deliver, post)) {
        free(post); /* queue full / UI not up: drop, poll timers cover it */
    }
}

/* ------------------------------------------------------------------ */
/* Mailboxes                                                           */
/* ------------------------------------------------------------------ */

static void progress_publish(const char *id, int pct, const char *stage)
{
    portENTER_CRITICAL(&s_mb_lock);
    if (id) {
        strlcpy(s_progress.running_id, id, sizeof(s_progress.running_id));
    }
    s_progress.pct = pct;
    if (stage) {
        strlcpy(s_progress.stage, stage, sizeof(s_progress.stage));
    }
    ++s_progress.seq;
    portEXIT_CRITICAL(&s_mb_lock);
}

void svc_test_progress_snapshot(svc_test_progress_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_mb_lock);
    *out = s_progress;
    portEXIT_CRITICAL(&s_mb_lock);
}

void svc_test_ask_snapshot(svc_test_ask_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_mb_lock);
    *out = s_ask;
    portEXIT_CRITICAL(&s_mb_lock);
}

void svc_test_canvas_snapshot(svc_test_canvas_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_mb_lock);
    *out = s_canvas;
    portEXIT_CRITICAL(&s_mb_lock);
}

void svc_test_ask_answer(bool yes)
{
    if (!s_ask_events) {
        return;
    }
    xEventGroupSetBits(s_ask_events, yes ? ASK_BIT_YES : ASK_BIT_NO);
}

/* ------------------------------------------------------------------ */
/* test_ctx_t implementation (runner task context)                     */
/* ------------------------------------------------------------------ */

static void ctx_progress(const test_ctx_t *ctx, int pct, const char *stage)
{
    (void)ctx;
    progress_publish(NULL, pct, stage);

    /* F6: keep the screen awake while a test reports progress (<=5 s
     * cadence, never inside the LVGL lock). */
    const int64_t now = esp_timer_get_time();
    if (now - s_last_keepalive_us > SVC_TEST_KEEPALIVE_US) {
        s_last_keepalive_us = now;
        svc_power_activity();
    }
}

static bool ctx_cancel_requested(const test_ctx_t *ctx)
{
    (void)ctx;
    return flags_cancelled();
}

static bool ctx_ask_operator(const test_ctx_t *ctx, const char *question,
                             int timeout_ms, bool *timed_out)
{
    (void)ctx;
    if (timed_out) {
        *timed_out = false;
    }
    if (!s_ask_events) {
        return false;
    }
    xEventGroupClearBits(s_ask_events, ASK_BIT_YES | ASK_BIT_NO);

    portENTER_CRITICAL(&s_mb_lock);
    strlcpy(s_ask.question, question ? question : "", sizeof(s_ask.question));
    s_ask.timeout_ms = timeout_ms;
    s_ask.published_us = esp_timer_get_time();
    s_ask.pending = true;
    ++s_ask.seq;
    portEXIT_CRITICAL(&s_mb_lock);

    const EventBits_t bits = xEventGroupWaitBits(
        s_ask_events, ASK_BIT_YES | ASK_BIT_NO, pdTRUE, pdFALSE,
        timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY);

    portENTER_CRITICAL(&s_mb_lock);
    s_ask.pending = false;
    ++s_ask.seq;
    portEXIT_CRITICAL(&s_mb_lock);

    if ((bits & ASK_BIT_YES) != 0) {
        return true;
    }
    if ((bits & ASK_BIT_NO) != 0) {
        return false;
    }
    if (timed_out) {
        *timed_out = true; /* hard timeout: the test decides SKIP vs FAIL */
    }
    return false;
}

static void ctx_request_canvas(const test_ctx_t *ctx,
                               void (*build)(lv_obj_t *parent, void *user),
                               void *user)
{
    (void)ctx;
    portENTER_CRITICAL(&s_mb_lock);
    s_canvas.build = build;
    s_canvas.user = user;
    ++s_canvas.seq;
    portEXIT_CRITICAL(&s_mb_lock);
}

/* ------------------------------------------------------------------ */
/* Pre-run checks and execution (runner task)                          */
/* ------------------------------------------------------------------ */

/* Returns true when the test may run; otherwise records SKIP. */
static bool precheck(const test_case_t *tc, test_result_t *res)
{
    if (svc_power_is_screen_off()) {
        res->st = TEST_ST_SKIP;
        strlcpy(res->evidence, "screen off", sizeof(res->evidence));
        return false;
    }
    if ((tc->flags & TEST_F_NEEDS_SD) && !svc_storage_mounted()) {
        res->st = TEST_ST_SKIP;
        strlcpy(res->evidence, "no SD card", sizeof(res->evidence));
        return false;
    }
    if (tc->domain == TEST_DOM_AUDIO &&
        (svc_audio_is_recording() || svc_audio_is_playing())) {
        res->st = TEST_ST_SKIP;
        strlcpy(res->evidence, "audio busy", sizeof(res->evidence));
        return false;
    }
    return true;
}

static void timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    /* Hard cap: cooperative flag + release a blocked ask as NO. */
    flags_set_cancel(true);
    if (s_ask_events) {
        xEventGroupSetBits(s_ask_events, ASK_BIT_NO);
    }
}

static void run_one(const test_case_t *tc)
{
    test_result_t res;
    memset(&res, 0, sizeof(res));
    res.st = TEST_ST_NOT_RUN;

    if (!precheck(tc, &res)) {
        test_result_set(tc->id, &res);
        ev_post(SVC_TEST_EV_RESULT, tc->id);
        return;
    }

    flags_clear();
    s_last_keepalive_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_busy_lock);
    s_current = tc;
    portEXIT_CRITICAL(&s_busy_lock);

    /* Reset transient mailboxes for the new run. */
    portENTER_CRITICAL(&s_mb_lock);
    strlcpy(s_progress.running_id, tc->id, sizeof(s_progress.running_id));
    s_progress.pct = 0;
    s_progress.stage[0] = '\0';
    ++s_progress.seq;
    s_ask.pending = false;
    ++s_ask.seq;
    s_canvas.build = NULL;
    s_canvas.user = NULL;
    ++s_canvas.seq;
    portEXIT_CRITICAL(&s_mb_lock);

    ev_post(SVC_TEST_EV_STARTED, tc->id);

    if (s_timeout_timer && tc->timeout_ms > 0) {
        xTimerChangePeriod(s_timeout_timer, pdMS_TO_TICKS(tc->timeout_ms),
                           0);
        xTimerStart(s_timeout_timer, 0);
    }

    const test_ctx_t ctx = {
        .progress = ctx_progress,
        .cancel_requested = ctx_cancel_requested,
        .ask_operator = ctx_ask_operator,
        .request_canvas = ctx_request_canvas,
        .user = NULL,
    };

    const int64_t start_us = esp_timer_get_time();
    tc->run(&ctx, &res);
    res.duration_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);

    if (s_timeout_timer) {
        xTimerStop(s_timeout_timer, 0);
    }

    /* Verdict reconciliation: a cancelled/timed-out test that returned
     * without a verdict is SKIP; a test that forgot its verdict entirely
     * is SKIP too (never silently PASS). */
    if (res.st == TEST_ST_NOT_RUN) {
        res.st = TEST_ST_SKIP;
        if (flags_timed_out()) {
            strlcpy(res.evidence, "timeout", sizeof(res.evidence));
        } else if (flags_cancelled()) {
            strlcpy(res.evidence, "aborted", sizeof(res.evidence));
        } else {
            strlcpy(res.evidence, "no result", sizeof(res.evidence));
        }
    }

    test_result_set(tc->id, &res);

    portENTER_CRITICAL(&s_busy_lock);
    s_current = NULL;
    portEXIT_CRITICAL(&s_busy_lock);

    /* Tear down transient UI state: progress idle, canvas released. */
    portENTER_CRITICAL(&s_mb_lock);
    s_progress.running_id[0] = '\0';
    ++s_progress.seq;
    s_canvas.build = NULL;
    s_canvas.user = NULL;
    ++s_canvas.seq;
    portEXIT_CRITICAL(&s_mb_lock);

    ev_post(SVC_TEST_EV_RESULT, tc->id);
}

/* Fixed run-all domain order (C.3): pure software checks first, network
 * last because WiFi scanning briefly starves the CPU. */
static const test_domain_t s_runall_order[TEST_DOM_COUNT] = {
    TEST_DOM_SYSTEM, TEST_DOM_MEMORY, TEST_DOM_ACCEL,
    TEST_DOM_DISPLAY_TOUCH, TEST_DOM_CAMERA, TEST_DOM_AUDIO,
    TEST_DOM_STORAGE, TEST_DOM_NETWORK, TEST_DOM_POWER,
};

static void runall_expand(void)
{
    s_auto_len = 0;
    s_auto_idx = 0;
    for (int d = 0; d < TEST_DOM_COUNT; ++d) {
        int first = -1, cnt = 0;
        test_domain_range(s_runall_order[d], &first, &cnt);
        for (int i = first; i >= 0 && i < first + cnt; ++i) {
            const test_case_t *tc = test_at(i);
            if (!tc ||
                (tc->flags & (TEST_F_INTERACTIVE | TEST_F_NO_RUNALL))) {
                continue;
            }
            if (s_auto_len < (int)(sizeof(s_auto_list) /
                                   sizeof(s_auto_list[0]))) {
                s_auto_list[s_auto_len++] = tc;
            }
        }
    }
    ESP_LOGI(TAG, "run-all queued %d auto tests", s_auto_len);
}

static void runner_task(void *arg)
{
    (void)arg;
    req_t req;
    for (;;) {
        if (s_auto_idx < s_auto_len) {
            /* Mid-batch: poll the queue so CANCEL stays responsive. */
            if (xQueueReceive(s_req_queue, &req, 0) == pdTRUE) {
                if (req.op == REQ_CANCEL) {
                    s_auto_len = s_auto_idx = 0;
                    flags_set_cancel(false);
                    continue;
                }
                if (req.op == REQ_RUN_ALL) {
                    runall_expand();
                    continue;
                }
                /* REQ_RUN arriving mid-batch runs inline before the next
                 * auto item (still strictly one test at a time). */
                const test_case_t *one = test_find(req.id);
                if (one) {
                    portENTER_CRITICAL(&s_busy_lock);
                    if (s_pending_by_domain[one->domain] > 0) {
                        --s_pending_by_domain[one->domain];
                    }
                    portEXIT_CRITICAL(&s_busy_lock);
                    run_one(one);
                }
                continue;
            }
            if (flags_cancelled()) {
                s_auto_len = s_auto_idx = 0;
                continue;
            }
            const test_case_t *tc = s_auto_list[s_auto_idx++];
            run_one(tc);
            if (s_auto_idx >= s_auto_len) {
                s_auto_len = s_auto_idx = 0;
                ev_post(SVC_TEST_EV_QUEUE_DRAINED, NULL);
            }
            continue;
        }

        if (xQueueReceive(s_req_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (req.op) {
        case REQ_RUN: {
            const test_case_t *tc = test_find(req.id);
            portENTER_CRITICAL(&s_busy_lock);
            if (tc && s_pending_by_domain[tc->domain] > 0) {
                --s_pending_by_domain[tc->domain];
            }
            portEXIT_CRITICAL(&s_busy_lock);
            if (tc) {
                run_one(tc);
            }
            break;
        }
        case REQ_RUN_ALL:
            portENTER_CRITICAL(&s_busy_lock);
            if (s_runall_pending > 0) {
                --s_runall_pending;
            }
            portEXIT_CRITICAL(&s_busy_lock);
            runall_expand();
            break;
        case REQ_CANCEL:
            flags_set_cancel(false);
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t svc_test_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    test_registry_init();
    test_register_all();

    s_req_queue = xQueueCreateStatic(SVC_TEST_QUEUE_DEPTH, sizeof(req_t),
                                     s_req_queue_storage,
                                     &s_req_queue_state);
    s_ask_events = xEventGroupCreate();
    s_timeout_timer = xTimerCreate("svc_test_tmo", pdMS_TO_TICKS(1000),
                                   pdFALSE, NULL, timeout_cb);
    if (!s_req_queue || !s_ask_events || !s_timeout_timer) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(runner_task, "svc_test", SVC_TEST_STACK, NULL,
                    SVC_TEST_PRIO, &s_task) != pdPASS) {
        return ESP_FAIL;
    }
    s_started = true;
    ESP_LOGI(TAG, "test framework started: %d tests registered",
             test_count());
    return ESP_OK;
}

static esp_err_t submit(req_op_t op, const char *id)
{
    if (!s_started || !s_req_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (svc_power_is_screen_off()) {
        return ESP_ERR_INVALID_STATE; /* F6: refuse while screen is off */
    }
    req_t req = { .op = (uint8_t)op };
    if (id) {
        strlcpy(req.id, id, sizeof(req.id));
    }
    if (xQueueSend(s_req_queue, &req, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM; /* caller reports via toast */
    }
    return ESP_OK;
}

esp_err_t svc_test_run(const char *id)
{
    const test_case_t *tc = test_find(id);
    if (!tc) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t err = submit(REQ_RUN, id);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_busy_lock);
        ++s_pending_by_domain[tc->domain];
        portEXIT_CRITICAL(&s_busy_lock);
    }
    return err;
}

esp_err_t svc_test_run_all_auto(void)
{
    const esp_err_t err = submit(REQ_RUN_ALL, NULL);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_busy_lock);
        ++s_runall_pending;
        portEXIT_CRITICAL(&s_busy_lock);
    }
    return err;
}

void svc_test_cancel(void)
{
    if (!s_started || !s_req_queue) {
        return;
    }
    xQueueReset(s_req_queue); /* drop every queued request */
    portENTER_CRITICAL(&s_busy_lock);
    memset(s_pending_by_domain, 0, sizeof(s_pending_by_domain));
    s_runall_pending = 0;
    portEXIT_CRITICAL(&s_busy_lock);

    flags_set_cancel(false);
    if (s_ask_events) {
        /* Release a blocked ask so an INTERACTIVE test exits promptly
         * when its run view (and canvas) is being destroyed. */
        xEventGroupSetBits(s_ask_events, ASK_BIT_NO);
    }
}

bool svc_test_busy(void)
{
    bool busy;
    portENTER_CRITICAL(&s_busy_lock);
    busy = (s_current != NULL) || (s_auto_idx < s_auto_len) ||
           (s_runall_pending > 0);
    if (!busy) {
        for (int d = 0; d < TEST_DOM_COUNT; ++d) {
            if (s_pending_by_domain[d] > 0) {
                busy = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_busy_lock);
    return busy;
}

bool svc_test_domain_busy(test_domain_t domain)
{
    if (domain < 0 || domain >= TEST_DOM_COUNT) {
        return false;
    }
    bool busy = false;
    portENTER_CRITICAL(&s_busy_lock);
    if (s_current && s_current->domain == domain) {
        busy = true;
    } else if (s_pending_by_domain[domain] > 0) {
        busy = true;
    } else if (s_runall_pending > 0 || s_auto_idx < s_auto_len) {
        /* A live batch touches every domain that has AUTO tests. */
        for (int i = s_auto_idx; i < s_auto_len; ++i) {
            if (s_auto_list[i]->domain == domain) {
                busy = true;
                break;
            }
        }
        if (!busy && s_runall_pending > 0 &&
            test_auto_count(domain) > 0) {
            busy = true;
        }
    }
    portEXIT_CRITICAL(&s_busy_lock);
    return busy;
}

void svc_test_subscribe(svc_test_cb_t cb, void *user)
{
    portENTER_CRITICAL(&s_sub_lock);
    s_sub_cb = cb;
    s_sub_user = user;
    portEXIT_CRITICAL(&s_sub_lock);
}
