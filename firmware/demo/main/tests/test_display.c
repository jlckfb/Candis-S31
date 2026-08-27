/*
 * Candis-S31 watch demo - DISPLAY_TOUCH (display) domain test suite.
 *
 * Five tests (spec D.1/E.3): quadrant colors, brightness ramp, motion
 * FPS, TE timing statistics and the panel-only sleep cycle. All hardware
 * access goes through the shared display_diag module; LVGL interaction
 * goes through the ctx triple (canvas build callbacks run on the LVGL
 * thread). No test here calls an LVGL API directly (spec C.7).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "demo_board.h"

#include "display_diag.h"
#include "test_registry.h"

/* ------------------------------------------------------------------ */
/* display.quadrant: four color blocks + operator confirm              */
/* ------------------------------------------------------------------ */

static void quadrant_build(lv_obj_t *parent, void *user)
{
    (void)user;
    static const uint32_t colors[4] = {
        0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF,
    };
    /* Full-bleed coverage of the run-view content area (460x364 with
     * 16/8/16/16 padding): positions are relative to the padding box, so
     * negative offsets reach the content edges. Blocks sit at the bottom
     * of the z-order so the operator-ask panel and result chrome stay
     * visible and clickable on top of them. */
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *block = lv_obj_create(parent);
        lv_obj_remove_style_all(block);
        lv_obj_set_size(block, 230, 182);
        lv_obj_set_pos(block, (i & 1) ? 214 : -16, (i & 2) ? 174 : -8);
        lv_obj_set_style_bg_color(block, lv_color_hex(colors[i]),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(block, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(block, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(block, 0);
    }
}

static void run_quadrant(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->progress(ctx, -1, "Showing color quadrants");
    ctx->request_canvas(ctx, quadrant_build, NULL);
    ctx->progress(ctx, -1, "Waiting for operator");

    bool timed_out = false;
    const bool yes = ctx->ask_operator(ctx, "Four color quadrants OK?",
                                       60000, &timed_out);
    if (ctx->cancel_requested(ctx)) {
        return; /* runner records SKIP "aborted" */
    }
    if (yes) {
        out->st = TEST_ST_PASS;
        strlcpy(out->evidence, "operator confirmed R/G/B/W quadrants",
                sizeof(out->evidence));
    } else if (timed_out) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "operator timeout", sizeof(out->evidence));
    } else {
        out->st = TEST_ST_FAIL;
        strlcpy(out->evidence, "operator rejected color quadrants",
                sizeof(out->evidence));
    }
}

/* ------------------------------------------------------------------ */
/* display.brightness_ramp: visible brightness sweep + operator        */
/* ------------------------------------------------------------------ */

static void run_brightness_ramp(const test_ctx_t *ctx, test_result_t *out)
{
    const int saved = demo_settings()->brightness;
    static const int ramp[] = { 100, 80, 60, 40, 20, 10, 20, 40, 60, 80, 100 };
    const int steps = (int)(sizeof(ramp) / sizeof(ramp[0]));

    bool aborted = false;
    for (int i = 0; i < steps; ++i) {
        if (ctx->cancel_requested(ctx)) {
            aborted = true;
            break;
        }
        bsp_display_brightness_set(ramp[i]);
        ctx->progress(ctx, (i + 1) * 100 / steps, "Ramping brightness");
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    /* Always restore the configured level (runtime only, NVS untouched). */
    bsp_display_brightness_set(saved);
    if (aborted) {
        return;
    }

    bool timed_out = false;
    const bool yes = ctx->ask_operator(
        ctx, "Did brightness ramp down and back up?", 30000, &timed_out);
    if (ctx->cancel_requested(ctx)) {
        return;
    }
    if (yes) {
        out->st = TEST_ST_PASS;
        strlcpy(out->evidence, "operator confirmed brightness ramp",
                sizeof(out->evidence));
    } else if (timed_out) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "operator timeout", sizeof(out->evidence));
    } else {
        out->st = TEST_ST_FAIL;
        strlcpy(out->evidence, "operator rejected brightness ramp",
                sizeof(out->evidence));
    }
}

/* ------------------------------------------------------------------ */
/* display.motion_fps: ball-regime frame rate through display_diag     */
/* ------------------------------------------------------------------ */

#define MOTION_MEASURE_MS 6000

static void motion_canvas_build(lv_obj_t *parent, void *user)
{
    (void)user;
    /* Ball-only: the partial-refresh regime the PASS threshold targets. */
    display_diag_motion_start(parent, false);
}

static void run_motion_fps(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->progress(ctx, 0, "Starting motion");
    ctx->request_canvas(ctx, motion_canvas_build, NULL);

    /* The canvas build only runs when a run view is open. In a
     * background run-all there is no viewer: host the motion on the
     * active screen instead (display_diag resolves NULL). */
    int waited_ms = 0;
    while (!display_diag_motion_running() && waited_ms < 1200 &&
            !ctx->cancel_requested(ctx)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }
    if (!display_diag_motion_running()) {
        if (display_diag_motion_start(NULL, false) != ESP_OK) {
            out->st = TEST_ST_SKIP;
            strlcpy(out->evidence, "motion start failed",
                    sizeof(out->evidence));
            return;
        }
    }

    int elapsed_ms = 0;
    bool aborted = false;
    while (elapsed_ms < MOTION_MEASURE_MS) {
        if (ctx->cancel_requested(ctx)) {
            aborted = true;
            break;
        }
        if (!display_diag_motion_running()) {
            break; /* host screen died underneath us */
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        elapsed_ms += 200;
        ctx->progress(ctx, elapsed_ms * 100 / MOTION_MEASURE_MS,
                      "Measuring FPS");
    }

    if (display_diag_motion_running()) {
        display_diag_motion_request_stop();
        display_diag_motion_wait_stopped(1500);
    }
    display_diag_motion_stats_t stats;
    display_diag_motion_snapshot(&stats);
    if (aborted) {
        return;
    }
    if (stats.seconds < 3 || elapsed_ms < 3000) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "motion interrupted", sizeof(out->evidence));
        return;
    }

    const float avg_fps = (float)stats.frames_total /
                          ((float)elapsed_ms / 1000.0f);
    snprintf(out->evidence, sizeof(out->evidence),
             "avg %.1f fps min %" PRIu32 " (ball %ds)", (double)avg_fps,
             stats.cycles_min, (int)(elapsed_ms / 1000));
    /* PASS gate from the spec: local animation average >= 50 fps. */
    out->st = avg_fps >= 50.0f ? TEST_ST_PASS : TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* display.te_stats: TE period/jitter through the port observer        */
/* ------------------------------------------------------------------ */

static void run_te_stats(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->progress(ctx, -1, "Sampling TE edges (2 s)");
    display_diag_te_stats_t stats;
    const esp_err_t error = display_diag_te_stats(2000, &stats);
    if (ctx->cancel_requested(ctx)) {
        return;
    }
    if (error == ESP_ERR_INVALID_STATE) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "TE observer busy", sizeof(out->evidence));
        return;
    }
    if (error != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence), "observer failed %s",
                 esp_err_to_name(error));
        return;
    }
    /* 2 s at 60 Hz yields ~120 periods; far fewer means the TE line is
     * not pulsing (panel asleep or sync broken). */
    if (stats.period_count < 30) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "only %" PRIu32 " TE periods in 2 s", stats.period_count);
        return;
    }

    const int64_t avg_us =
        (int64_t)(stats.period_total_us / stats.period_count);
    const int64_t jitter_us = stats.period_max_us - stats.period_min_us;
    /* PASS gates from the spec: period mean 16.67 ms +/- 5 %, jitter
     * (max-min) < 2 ms. */
    const bool period_ok = avg_us >= 15833 && avg_us <= 17500;
    const bool jitter_ok = jitter_us < 2000;
    snprintf(out->evidence, sizeof(out->evidence),
             "avg %" PRId64 " us jitter %" PRId64 " us n=%" PRIu32,
             avg_us, jitter_us, stats.period_count);
    out->st = (period_ok && jitter_ok) ? TEST_ST_PASS : TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* display.sleep_cycle: panel-only sleep x3, redraw proves recovery    */
/* ------------------------------------------------------------------ */

#define SLEEP_CYCLE_COUNT 3

static void run_sleep_cycle(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->progress(ctx, 0, "Attaching flush counter");
    if (display_diag_flush_counter_attach() != ESP_OK) {
        out->st = TEST_ST_SKIP;
        strlcpy(out->evidence, "flush counter busy", sizeof(out->evidence));
        return;
    }

    int confirmed = 0;
    bool failed = false;
    for (int cycle = 0; cycle < SLEEP_CYCLE_COUNT && !failed; ++cycle) {
        if (ctx->cancel_requested(ctx)) {
            break;
        }
        char stage[32];
        snprintf(stage, sizeof(stage), "Panel sleep %d/%d", cycle + 1,
                 SLEEP_CYCLE_COUNT);
        ctx->progress(ctx, cycle * 100 / SLEEP_CYCLE_COUNT, stage);

        const uint32_t before = display_diag_flush_counter_read();
        /* svc_power screen-off pattern: freeze rendering first, run the
         * hardware sleep/wake sequence outside the LVGL lock, then
         * unfreeze and force a full redraw. */
        display_diag_set_invalidation(false);
        esp_err_t error = bsp_display_enter_sleep_panel();
        if (error != ESP_OK) {
            display_diag_set_invalidation(true);
            snprintf(out->evidence, sizeof(out->evidence),
                     "enter sleep failed %s", esp_err_to_name(error));
            failed = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        error = bsp_display_exit_sleep_panel();
        display_diag_set_invalidation(true);
        display_diag_invalidate_all();
        if (error != ESP_OK) {
            snprintf(out->evidence, sizeof(out->evidence),
                     "exit sleep failed %s", esp_err_to_name(error));
            failed = true;
            break;
        }

        /* Recovery proof: the display pipeline keeps flushing after the
         * wake. */
        bool grew = false;
        for (int wait = 0; wait < 15 && !grew; ++wait) {
            vTaskDelay(pdMS_TO_TICKS(100));
            grew = display_diag_flush_counter_read() > before;
        }
        if (grew) {
            ++confirmed;
        } else {
            snprintf(out->evidence, sizeof(out->evidence),
                     "no flush after wake %d", cycle + 1);
            failed = true;
        }
    }

    display_diag_flush_counter_detach();
    /* Paranoia: never leave the renderer frozen. */
    display_diag_set_invalidation(true);
    display_diag_invalidate_all();

    if (failed) {
        out->st = TEST_ST_FAIL;
        return;
    }
    if (confirmed < SLEEP_CYCLE_COUNT) {
        return; /* cancelled: runner records SKIP */
    }
    out->st = TEST_ST_PASS;
    snprintf(out->evidence, sizeof(out->evidence),
             "%d panel sleep cycles, redrawn each wake", SLEEP_CYCLE_COUNT);
}

/* ------------------------------------------------------------------ */

void test_display_register(void)
{
    static const test_case_t cases[] = {
        { "display.quadrant", "Quadrant colors", TEST_DOM_DISPLAY_TOUCH,
          TEST_F_INTERACTIVE, 120000, run_quadrant },
        { "display.brightness_ramp", "Brightness ramp",
          TEST_DOM_DISPLAY_TOUCH, TEST_F_INTERACTIVE, 60000,
          run_brightness_ramp },
        { "display.motion_fps", "Motion FPS", TEST_DOM_DISPLAY_TOUCH,
          TEST_F_LONG, 30000, run_motion_fps },
        { "display.te_stats", "TE timing", TEST_DOM_DISPLAY_TOUCH,
          0, 15000, run_te_stats },
        { "display.sleep_cycle", "Panel sleep cycle",
          TEST_DOM_DISPLAY_TOUCH, 0, 60000, run_sleep_cycle },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        test_register(&cases[i]);
    }
}
