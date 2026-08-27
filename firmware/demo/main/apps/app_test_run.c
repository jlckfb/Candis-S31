/*
 * Candis-S31 watch demo - single-test run view (spec C.4 / B.6.4).
 *
 * Opened by the domain detail page: pushes a shell screen and starts the
 * test with svc_test_run(). The view is a pure observer of the runner
 * mailboxes: a 100 ms lv_timer pulls progress (bar + stage text), the
 * operator-ask mailbox (question + YES/NO + countdown ring, answers via
 * svc_test_ask_answer) and the canvas mailbox (build() runs right here on
 * the LVGL thread, handing the whole content area to the test). Finished
 * results freeze as a large chip + evidence line.
 *
 * pct == -1 means indeterminate: the bar is replaced by a small 32 px
 * shuttle animating back and forth (B.5 motion budget; the animation is
 * deleted on screen DELETE).
 *
 * Cleanup chain (F13): screen DELETE deletes the timer and animations;
 * an INTERACTIVE test still running is cancelled through svc_test_cancel
 * (its canvas dies with the screen), AUTO tests keep running in the
 * background and land in the result library (C.3).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "tests/svc_test.h"
#include "tests/test_registry.h"
#include "ui/ui_manager.h"
#include "ui/ui_theme.h"
#include "ui/ui_widgets.h"

#define RUN_POLL_MS 100

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_obj_t *spinner;
    lv_obj_t *lbl_stage;
    lv_obj_t *bar;
    lv_obj_t *shuttle;      /* indeterminate 32 px bar */
    lv_obj_t *ask_panel;
    lv_obj_t *chip_result;
    lv_obj_t *lbl_evidence;
    lv_timer_t *timer;

    char test_id[32];
    bool interactive;
    bool finished;          /* verdict frozen on screen */
    bool canvas_built;      /* content handed to the test */
    uint32_t last_prog_seq;
    uint32_t last_ask_seq;
    uint32_t last_canvas_seq;
    uint32_t last_result_seq;
    bool ask_visible;
} run_view_t;

/* Single live run view (nav depth >= 2 screens of test center max). */
static run_view_t *s_view;

/* ------------------------------------------------------------------ */
/* Indeterminate shuttle animation (32 px wide, B.5 budget)            */
/* ------------------------------------------------------------------ */

static void shuttle_anim_cb(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void shuttle_start(run_view_t *v)
{
    if (!v->shuttle || lv_obj_has_flag(v->shuttle, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_anim_delete(v->shuttle, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, v->shuttle);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_values(&a, 0, UI_ROW_W - 32);
    lv_anim_set_exec_cb(&a, shuttle_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_playback_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void shuttle_stop(run_view_t *v)
{
    if (v->shuttle) {
        lv_anim_delete(v->shuttle, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static uiw_chip_state_t chip_of(test_status_t st)
{
    switch (st) {
    case TEST_ST_PASS: return UIW_CHIP_PASS;
    case TEST_ST_FAIL: return UIW_CHIP_FAIL;
    case TEST_ST_WARN: return UIW_CHIP_WARN;
    case TEST_ST_SKIP: return UIW_CHIP_SKIP;
    case TEST_ST_NOT_RUN:
    default:           return UIW_CHIP_NOT_RUN;
    }
}

static void show_running_ui(run_view_t *v, bool running)
{
    if (running) {
        lv_obj_remove_flag(v->spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v->bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->chip_result, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v->spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->shuttle, LV_OBJ_FLAG_HIDDEN);
        shuttle_stop(v);
        lv_obj_remove_flag(v->chip_result, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_progress(run_view_t *v, const svc_test_progress_t *p)
{
    lv_label_set_text(v->lbl_stage, p->stage);

    if (p->pct < 0) {
        /* Indeterminate: hide the bar, run the small shuttle. */
        if (!lv_obj_has_flag(v->bar, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(v->bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (lv_obj_has_flag(v->shuttle, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(v->shuttle, LV_OBJ_FLAG_HIDDEN);
        }
        shuttle_start(v);
    } else {
        if (!lv_obj_has_flag(v->shuttle, LV_OBJ_FLAG_HIDDEN)) {
            shuttle_stop(v);
            lv_obj_add_flag(v->shuttle, LV_OBJ_FLAG_HIDDEN);
        }
        if (lv_obj_has_flag(v->bar, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(v->bar, LV_OBJ_FLAG_HIDDEN);
        }
        lv_bar_set_value(v->bar, p->pct, LV_ANIM_OFF);
    }
}

static void render_result(run_view_t *v, const test_result_t *res)
{
    v->finished = true;
    show_running_ui(v, false);
    uiw_chip_set(v->chip_result, chip_of(res->st));
    if (res->evidence[0]) {
        lv_label_set_text_fmt(v->lbl_evidence, "%s  (%lu ms)",
                              res->evidence,
                              (unsigned long)res->duration_ms);
    } else {
        lv_label_set_text_fmt(v->lbl_evidence, "(%lu ms)",
                              (unsigned long)res->duration_ms);
    }
}

static void render_ask(run_view_t *v, const svc_test_ask_t *ask)
{
    if (ask->pending && !v->ask_visible) {
        v->ask_visible = true;
        uiw_ask_panel_set_question(v->ask_panel, ask->question);
        uiw_ask_panel_show(v->ask_panel, true);
    } else if (!ask->pending && v->ask_visible) {
        v->ask_visible = false;
        uiw_ask_panel_show(v->ask_panel, false);
        uiw_ask_panel_set_countdown(v->ask_panel, -1);
    }
    if (ask->pending && ask->timeout_ms > 0) {
        const int64_t elapsed_ms =
            (esp_timer_get_time() - ask->published_us) / 1000;
        int remain = 100 - (int)((elapsed_ms * 100) / ask->timeout_ms);
        if (remain < 0) {
            remain = 0;
        }
        uiw_ask_panel_set_countdown(v->ask_panel, remain);
    }
}

static void render_canvas(run_view_t *v, const svc_test_canvas_t *canvas)
{
    if (canvas->build && !v->canvas_built) {
        v->canvas_built = true;
        /* Interactive tests own the content area; the running chrome
         * (spinner/bar/stage/evidence) steps aside. */
        lv_obj_add_flag(v->spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->shuttle, LV_OBJ_FLAG_HIDDEN);
        shuttle_stop(v);
        canvas->build(v->content, canvas->user);
    }
}

/* ------------------------------------------------------------------ */
/* Poll timer (100 ms): pulls every runner mailbox                     */
/* ------------------------------------------------------------------ */

static void run_poll_cb(lv_timer_t *timer)
{
    run_view_t *v = lv_timer_get_user_data(timer);
    if (!v || !v->screen) {
        return;
    }

    svc_test_progress_t prog;
    svc_test_progress_snapshot(&prog);
    svc_test_ask_t ask;
    svc_test_ask_snapshot(&ask);
    svc_test_canvas_t canvas;
    svc_test_canvas_snapshot(&canvas);

    if (canvas.seq != v->last_canvas_seq) {
        v->last_canvas_seq = canvas.seq;
        render_canvas(v, &canvas);
    }

    if (ask.seq != v->last_ask_seq || v->ask_visible) {
        v->last_ask_seq = ask.seq;
        render_ask(v, &ask);
    }

    if (!v->finished) {
        if (prog.seq != v->last_prog_seq) {
            v->last_prog_seq = prog.seq;
            if (strncmp(prog.running_id, v->test_id,
                        sizeof(prog.running_id)) == 0) {
                render_progress(v, &prog);
            }
        }
        /* Result freeze: dedup on run_seq. */
        test_result_t res;
        test_result_get(v->test_id, &res);
        if (res.run_seq != v->last_result_seq) {
            v->last_result_seq = res.run_seq;
            if (res.run_seq != 0 &&
                strncmp(prog.running_id, v->test_id,
                        sizeof(prog.running_id)) != 0) {
                render_result(v, &res);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void ask_answered_cb(bool yes, void *user)
{
    (void)user;
    svc_test_ask_answer(yes);
}

static void run_delete_cb(lv_event_t *event)
{
    run_view_t *v = lv_event_get_user_data(event);
    if (!v) {
        return;
    }
    shuttle_stop(v);
    if (v->timer) {
        lv_timer_delete(v->timer);
    }

    /* C.3: INTERACTIVE tests die with their canvas; AUTO tests keep
     * running in the background and land in the result library. */
    if (v->interactive && !v->finished) {
        svc_test_progress_t prog;
        svc_test_progress_snapshot(&prog);
        if (strncmp(prog.running_id, v->test_id,
                    sizeof(prog.running_id)) == 0) {
            svc_test_cancel();
        }
    }
    if (s_view == v) {
        s_view = NULL;
    }
    free(v);
}

void app_test_run_open(const char *test_id)
{
    const test_case_t *tc = test_find(test_id);
    if (!tc) {
        ui_toast("Unknown test");
        return;
    }

    run_view_t *v = calloc(1, sizeof(*v));
    if (!v) {
        return;
    }
    strlcpy(v->test_id, tc->id, sizeof(v->test_id));
    v->interactive = (tc->flags & TEST_F_INTERACTIVE) != 0;

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold(tc->name, &content);
    v->screen = root;
    v->content = content;
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    /* Spinner (small, 32 px) + stage text + progress bar (B.6.4). */
    v->spinner = lv_spinner_create(content);
    lv_obj_set_size(v->spinner, 32, 32);
    lv_obj_align(v->spinner, LV_ALIGN_TOP_MID, 0, 24);

    v->lbl_stage = lv_label_create(content);
    lv_label_set_text(v->lbl_stage, "Starting...");
    lv_obj_set_style_text_font(v->lbl_stage, ui_font_body_lg(), 0);
    lv_obj_set_style_text_color(v->lbl_stage, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_width(v->lbl_stage, UI_ROW_W);
    lv_obj_set_style_text_align(v->lbl_stage, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v->lbl_stage, LV_ALIGN_TOP_MID, 0, 76);

    v->bar = uiw_progress_create(content);
    lv_obj_align(v->bar, LV_ALIGN_TOP_MID, 0, 128);

    v->shuttle = lv_obj_create(content);
    lv_obj_set_size(v->shuttle, 32, UI_PROGRESS_H);
    lv_obj_align(v->shuttle, LV_ALIGN_TOP_LEFT, 0, 128);
    lv_obj_set_style_bg_color(v->shuttle, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(v->shuttle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(v->shuttle, UI_PROGRESS_RADIUS, 0);
    lv_obj_set_style_border_width(v->shuttle, 0, 0);
    lv_obj_add_flag(v->shuttle, LV_OBJ_FLAG_HIDDEN);

    /* Operator-confirm panel (hidden until the ask mailbox fires). */
    v->ask_panel = uiw_ask_panel(content, "", ask_answered_cb, NULL);
    if (v->ask_panel) {
        lv_obj_align(v->ask_panel, LV_ALIGN_TOP_MID, 0, 150);
    }

    /* Result freeze: big chip + evidence line. */
    v->chip_result = uiw_chip_create(content);
    lv_obj_align(v->chip_result, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_add_flag(v->chip_result, LV_OBJ_FLAG_HIDDEN);

    v->lbl_evidence = lv_label_create(content);
    lv_label_set_text(v->lbl_evidence, "");
    lv_obj_set_style_text_font(v->lbl_evidence, ui_font_text(), 0);
    lv_obj_set_style_text_color(v->lbl_evidence,
                                lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_set_width(v->lbl_evidence, UI_ROW_W);
    lv_obj_set_style_text_align(v->lbl_evidence, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v->lbl_evidence, LV_ALIGN_TOP_MID, 0, 200);

    /* Previous result (if any) shows immediately until the run starts. */
    test_result_t res;
    test_result_get(v->test_id, &res);
    v->last_result_seq = res.run_seq;

    v->timer = lv_timer_create(run_poll_cb, RUN_POLL_MS, v);
    lv_obj_add_event_cb(root, run_delete_cb, LV_EVENT_DELETE, v);

    s_view = v;
    ui_nav_push_screen(root);

    /* Start the test unless it is already running (e.g. re-opened while
     * a background AUTO run of the same id is in flight). */
    svc_test_progress_t prog;
    svc_test_progress_snapshot(&prog);
    if (strncmp(prog.running_id, v->test_id, sizeof(prog.running_id)) != 0) {
        const esp_err_t err = svc_test_run(tc->id);
        if (err == ESP_ERR_NO_MEM) {
            ui_toast("Runner queue full");
        } else if (err != ESP_OK) {
            ui_toast("Runner unavailable");
        }
    }
}
