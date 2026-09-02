/*
 * Candis-S31 watch demo - test center: overview + domain detail pages.
 *
 * Overview (spec B.6.3): summary card (totals + progress), a 3x3 domain
 * badge wall (icon + name + aggregate chip, C.1 priority FAIL > WARN >
 * NOT_RUN > SKIP > PASS) and a fixed bottom action row (RUN ALL / Export
 * / Reset). Tapping a domain card pushes a domain detail page (spec
 * B.6.4): a "Run domain" row plus one uiw_row per test (status chip +
 * evidence), each row opening the run view (app_test_run_open).
 *
 * Refresh model: the overview holds the single svc_test subscription
 * (LVGL-thread callbacks) and repaints on events; a 1 s lv_timer is the
 * backstop (also greys Export without a card). The pushed detail page
 * polls with its own 500 ms timer. All repaints are aggregate-value
 * guarded so an idle center costs zero LVGL writes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo_apps.h"
#include "esp_log.h"
#include "services/svc_storage.h"
#include "tests/svc_test.h"
#include "tests/test_registry.h"
#include "tests/test_report.h"
#include "ui/ui_manager.h"
#include "ui/ui_perf.h"
#include "ui/ui_widgets.h"

#define OVERVIEW_REFRESH_MS 1000
#define DETAIL_REFRESH_MS   500

/* ------------------------------------------------------------------ */
/* Overview page                                                       */
/* ------------------------------------------------------------------ */

static const char *const s_domain_icons[TEST_DOM_COUNT] = {
    [TEST_DOM_DISPLAY_TOUCH] = LV_SYMBOL_EYE_OPEN,
    [TEST_DOM_CAMERA]        = LV_SYMBOL_IMAGE,
    [TEST_DOM_AUDIO]         = LV_SYMBOL_AUDIO,
    [TEST_DOM_STORAGE]       = LV_SYMBOL_SD_CARD,
    [TEST_DOM_NETWORK]       = LV_SYMBOL_WIFI,
    [TEST_DOM_SYSTEM]        = LV_SYMBOL_SETTINGS,
    [TEST_DOM_MEMORY]        = LV_SYMBOL_SAVE,
    [TEST_DOM_ACCEL]         = LV_SYMBOL_BARS,
    [TEST_DOM_POWER]         = LV_SYMBOL_POWER,
};

/* Last-rendered aggregate snapshot: no LVGL write while unchanged. */
typedef struct {
    int total;
    int pass;
    int fail;
    int warn;
    int skip;
    int not_run;
    int auto_total;
    bool busy;
    bool sd;
    test_status_t dom[TEST_DOM_COUNT];
    bool dom_busy[TEST_DOM_COUNT];
} overview_model_t;

static struct {
    lv_obj_t *screen;
    lv_obj_t *summary_total;
    lv_obj_t *summary_stats;
    lv_obj_t *summary_bar;
    lv_obj_t *summary_pct;
    lv_obj_t *chip[TEST_DOM_COUNT];
    lv_obj_t *btn_run_all;
    lv_obj_t *lbl_run_all;
    lv_obj_t *btn_export;
    lv_timer_t *timer;
    overview_model_t last;
    bool valid; /* last holds a rendered snapshot */
} s_ov;

static uiw_chip_state_t chip_of_status(test_status_t st)
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

static overview_model_t overview_collect(void)
{
    overview_model_t m;
    memset(&m, 0, sizeof(m));
    m.auto_total = test_auto_count((test_domain_t)-1);
    m.busy = svc_test_busy();
    m.sd = svc_storage_mounted();

    const int n = test_count();
    for (int i = 0; i < n; ++i) {
        const test_case_t *tc = test_at(i);
        if (!tc) {
            continue;
        }
        test_result_t res;
        test_result_get(tc->id, &res);
        ++m.total;
        switch (res.st) {
        case TEST_ST_PASS: ++m.pass; break;
        case TEST_ST_FAIL: ++m.fail; break;
        case TEST_ST_WARN: ++m.warn; break;
        case TEST_ST_SKIP: ++m.skip; break;
        default:           ++m.not_run; break;
        }
    }
    for (int d = 0; d < TEST_DOM_COUNT; ++d) {
        m.dom[d] = test_domain_aggregate((test_domain_t)d);
        m.dom_busy[d] = svc_test_domain_busy((test_domain_t)d);
    }
    return m;
}

static void overview_render(const overview_model_t *m)
{
    const int ran = m->total - m->not_run;
    lv_label_set_text_fmt(s_ov.summary_total, "TOTAL %d/%d", ran, m->total);
    lv_label_set_text_fmt(s_ov.summary_stats,
                          "PASS %d  FAIL %d  WARN %d  SKIP %d",
                          m->pass, m->fail, m->warn, m->skip);
    const int pct = m->total > 0 ? (ran * 100) / m->total : 0;
    lv_bar_set_value(s_ov.summary_bar, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_ov.summary_pct, "%d%%", pct);

    for (int d = 0; d < TEST_DOM_COUNT; ++d) {
        uiw_chip_set(s_ov.chip[d], m->dom_busy[d]
                                       ? UIW_CHIP_RUN
                                       : chip_of_status(m->dom[d]));
    }

    lv_label_set_text_fmt(s_ov.lbl_run_all, LV_SYMBOL_PLAY " RUN ALL (%d)",
                          m->auto_total);
    if (m->busy) {
        lv_obj_add_state(s_ov.btn_run_all, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_ov.btn_run_all, LV_STATE_DISABLED);
    }
    if (m->sd) {
        lv_obj_remove_state(s_ov.btn_export, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_ov.btn_export, LV_STATE_DISABLED);
    }
}

static void overview_refresh(void)
{
    if (!s_ov.screen) {
        return;
    }
    const overview_model_t m = overview_collect();
    if (s_ov.valid && memcmp(&m, &s_ov.last, sizeof(m)) == 0) {
        return; /* unchanged: zero LVGL writes */
    }
    s_ov.last = m;
    s_ov.valid = true;
    overview_render(&m);
}

static void overview_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    overview_refresh();
}

/* svc_test event subscriber, runs on the LVGL thread. */
static void overview_test_event(svc_test_event_t ev, const char *id,
                                void *user)
{
    (void)ev;
    (void)id;
    (void)user;
    overview_refresh();
}

static void toast_owned(void *arg)
{
    ui_toast((const char *)arg);
    free(arg);
}

static void run_all_cb(lv_event_t *event)
{
    (void)event;
    const esp_err_t err = svc_test_run_all_auto();
    if (err == ESP_OK) {
        ui_toast("Run-all queued");
    } else if (err == ESP_ERR_NO_MEM) {
        ui_toast("Runner queue full");
    } else {
        ui_toast("Runner unavailable");
    }
    overview_refresh();
}

static void export_cb(lv_event_t *event)
{
    (void)event;
    char *msg = malloc(128);
    if (!msg) {
        return;
    }
    char path[96];
    const esp_err_t err = test_report_export_to_sd(path, sizeof(path));
    if (err == ESP_OK) {
        snprintf(msg, 128, "Report: %s", strrchr(path, '/')
                                            ? strrchr(path, '/') + 1 : path);
    } else if (err == ESP_ERR_INVALID_STATE) {
        snprintf(msg, 128, "No SD card");
    } else {
        snprintf(msg, 128, "Export failed");
    }
    /* svc_test / storage callbacks aside, we are on the LVGL thread here,
     * but routing through ui_async keeps the toast contract uniform. */
    if (!ui_async(toast_owned, msg)) {
        free(msg);
    }
}

static void reset_confirm_cb(bool ok, void *user)
{
    (void)user;
    if (ok) {
        test_results_reset();
        overview_refresh();
    }
}

static void reset_cb(lv_event_t *event)
{
    (void)event;
    ui_msgbox("Reset results", "Clear every test result?", reset_confirm_cb,
              NULL);
}

static void domain_card_cb(lv_event_t *event);

static lv_obj_t *summary_card_create(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, UI_ROW_W, 72);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_TILE_RADIUS, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_ov.summary_total = lv_label_create(card);
    lv_label_set_text(s_ov.summary_total, "TOTAL 0/0");
    lv_obj_set_style_text_font(s_ov.summary_total, ui_font_body_lg(), 0);
    lv_obj_set_style_text_color(s_ov.summary_total,
                                lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_pos(s_ov.summary_total, 14, 8);

    s_ov.summary_stats = lv_label_create(card);
    lv_label_set_text(s_ov.summary_stats, "PASS 0  FAIL 0  WARN 0  SKIP 0");
    lv_obj_set_style_text_font(s_ov.summary_stats, ui_font_text(), 0);
    lv_obj_set_style_text_color(s_ov.summary_stats,
                                lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_align(s_ov.summary_stats, LV_ALIGN_TOP_RIGHT, -14, 14);

    s_ov.summary_bar = lv_bar_create(card);
    lv_obj_set_size(s_ov.summary_bar, 300, UI_PROGRESS_H);
    lv_obj_set_pos(s_ov.summary_bar, 14, 46);
    lv_obj_set_style_radius(s_ov.summary_bar, UI_PROGRESS_RADIUS,
                            LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ov.summary_bar,
                              lv_color_hex(UI_COL_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ov.summary_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ov.summary_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ov.summary_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ov.summary_bar, UI_PROGRESS_RADIUS,
                            LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ov.summary_bar, lv_color_hex(UI_COL_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_ov.summary_bar, LV_OPA_COVER,
                            LV_PART_INDICATOR);
    lv_bar_set_range(s_ov.summary_bar, 0, 100);
    lv_obj_remove_flag(s_ov.summary_bar, LV_OBJ_FLAG_CLICKABLE);

    s_ov.summary_pct = lv_label_create(card);
    lv_label_set_text(s_ov.summary_pct, "0%");
    lv_obj_set_style_text_font(s_ov.summary_pct, ui_font_text(), 0);
    lv_obj_set_style_text_color(s_ov.summary_pct,
                                lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_set_pos(s_ov.summary_pct, 324, 40);

    return card;
}

static lv_obj_t *domain_card_create(lv_obj_t *parent, test_domain_t d)
{
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, UI_TILE_W, UI_DOMAIN_TILE_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_TILE_RADIUS, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COL_HAIRLINE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* Press feedback: color swap only, never transform_scale. */
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_SURFACE_2),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COL_ACCENT),
                                  LV_STATE_PRESSED);

    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, s_domain_icons[d]);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_pos(icon, 12, 8);

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, test_domain_name(d));
    lv_obj_set_style_text_font(name, ui_font_text(), 0);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 12, -32);

    s_ov.chip[d] = uiw_chip_create(card);
    lv_obj_align(s_ov.chip[d], LV_ALIGN_BOTTOM_LEFT, 12, -6);

    lv_obj_add_event_cb(card, domain_card_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)d);
    return card;
}

static void action_button_style(lv_obj_t *btn, uint32_t bg, uint32_t border)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_SURFACE_2),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COL_ACCENT),
                                  LV_STATE_PRESSED);
}

static lv_obj_t *action_row_create(lv_obj_t *root)
{
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_set_size(row, 460, UI_ACTION_ROW_H);
    lv_obj_set_pos(row, 0, 460 - UI_ACTION_ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_ov.btn_run_all = lv_button_create(row);
    lv_obj_set_size(s_ov.btn_run_all, 200, 52);
    lv_obj_set_pos(s_ov.btn_run_all, UI_SCREEN_PAD, 4);
    action_button_style(s_ov.btn_run_all, UI_COL_ACCENT_DIM, UI_COL_ACCENT);
    lv_obj_add_event_cb(s_ov.btn_run_all, run_all_cb, LV_EVENT_CLICKED,
                        NULL);
    s_ov.lbl_run_all = lv_label_create(s_ov.btn_run_all);
    lv_label_set_text(s_ov.lbl_run_all, LV_SYMBOL_PLAY " RUN ALL (0)");
    lv_obj_set_style_text_font(s_ov.lbl_run_all, ui_font_body(), 0);
    lv_obj_set_style_text_color(s_ov.lbl_run_all,
                                lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(s_ov.lbl_run_all);

    s_ov.btn_export = lv_button_create(row);
    lv_obj_set_size(s_ov.btn_export, 100, 52);
    lv_obj_set_pos(s_ov.btn_export, 228, 4);
    action_button_style(s_ov.btn_export, UI_COL_SURFACE, UI_COL_HAIRLINE);
    lv_obj_add_event_cb(s_ov.btn_export, export_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_export = lv_label_create(s_ov.btn_export);
    lv_label_set_text(lbl_export, "Export");
    lv_obj_set_style_text_font(lbl_export, ui_font_body(), 0);
    lv_obj_set_style_text_color(lbl_export, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(lbl_export);

    lv_obj_t *btn_reset = lv_button_create(row);
    lv_obj_set_size(btn_reset, 100, 52);
    lv_obj_set_pos(btn_reset, 340, 4);
    action_button_style(btn_reset, UI_COL_FAIL_DIM, UI_COL_FAIL);
    lv_obj_add_event_cb(btn_reset, reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset);
    lv_label_set_text(lbl_reset, "Reset");
    lv_obj_set_style_text_font(lbl_reset, ui_font_body(), 0);
    lv_obj_set_style_text_color(lbl_reset, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(lbl_reset);

    return row;
}

static void overview_delete_cb(lv_event_t *event)
{
    (void)event;
    svc_test_subscribe(NULL, NULL);
    if (s_ov.timer) {
        lv_timer_delete(s_ov.timer);
        s_ov.timer = NULL;
    }
    memset(&s_ov, 0, sizeof(s_ov));
}

/* ------------------------------------------------------------------ */
/* Domain detail page (pushed shell screen, not a registered app)      */
/* ------------------------------------------------------------------ */

typedef struct {
    test_domain_t domain;
    lv_obj_t *screen;
    lv_timer_t *timer;
    int count;                    /* tests in this domain */
    lv_obj_t *chip[24];           /* per-row status chips */
    lv_obj_t *sub[24];            /* per-row evidence labels */
    lv_obj_t *run_row_sub;
    char ids[24][32];             /* row -> test id */
    uint32_t last_seq[24];        /* per-row result run_seq dedup */
    char last_running[32];        /* run-state dedup */
    uint32_t last_auto_seq;
} detail_state_t;

static void detail_delete_cb(lv_event_t *event)
{
    detail_state_t *st = lv_event_get_user_data(event);
    if (!st) {
        return;
    }
    if (st->timer) {
        lv_timer_delete(st->timer);
    }
    free(st);
}

static void run_view_open_cb(lv_event_t *event)
{
    const char *id = lv_event_get_user_data(event);
    if (id) {
        app_test_run_open(id);
    }
}

static void detail_run_domain_cb(lv_event_t *event)
{
    detail_state_t *st = lv_event_get_user_data(event);
    if (!st) {
        return;
    }
    int queued = 0;
    int first = -1, cnt = 0;
    test_domain_range(st->domain, &first, &cnt);
    for (int i = first; i >= 0 && i < first + cnt; ++i) {
        const test_case_t *tc = test_at(i);
        if (!tc ||
            (tc->flags & (TEST_F_INTERACTIVE | TEST_F_NO_RUNALL))) {
            continue;
        }
        if (svc_test_run(tc->id) == ESP_OK) {
            ++queued;
        }
    }
    if (queued > 0) {
        ui_toast("Domain run queued");
    } else {
        ui_toast("Nothing to run");
    }
}

static void detail_timer_cb(lv_timer_t *timer)
{
    detail_state_t *st = lv_timer_get_user_data(timer);
    if (!st || !st->screen) {
        return;
    }

    /* RUN chip follows the runner mailbox (deduped on running_id). */
    svc_test_progress_t prog;
    svc_test_progress_snapshot(&prog);
    if (strncmp(prog.running_id, st->last_running,
                sizeof(st->last_running)) != 0) {
        strlcpy(st->last_running, prog.running_id,
                sizeof(st->last_running));
        /* Force every row to repaint its chip on state change. */
        memset(st->last_seq, 0, sizeof(st->last_seq));
    }

    for (int i = 0; i < st->count; ++i) {
        test_result_t res;
        test_result_get(st->ids[i], &res);
        const bool running = strncmp(prog.running_id, st->ids[i],
                                     sizeof(prog.running_id)) == 0;
        if (res.run_seq == st->last_seq[i] && !running) {
            continue;
        }
        st->last_seq[i] = res.run_seq;
        if (running) {
            uiw_chip_set(st->chip[i], UIW_CHIP_RUN);
        } else {
            uiw_chip_set(st->chip[i], chip_of_status(res.st));
        }
        if (res.evidence[0]) {
            lv_label_set_text(st->sub[i], res.evidence);
        }
    }
}

static lv_obj_t *domain_detail_create(test_domain_t d)
{
    detail_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    st->domain = d;

    lv_obj_t *content = NULL;
    ui_perf_create_begin("domain_detail");
    lv_obj_t *root = ui_app_scaffold(test_domain_name(d), &content);
    ui_perf_create_end();
    st->screen = root;

    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, UI_GAP, 0);

    /* Domain run row (auto tests of this domain). */
    const int auto_n = test_auto_count(d);
    uiw_row_t run_row;
    uiw_row_create(content, LV_SYMBOL_PLAY, "Run domain", &run_row);
    lv_label_set_text_fmt(run_row.sub_lbl, "%d auto tests", auto_n);
    lv_obj_add_event_cb(run_row.row, detail_run_domain_cb,
                        LV_EVENT_CLICKED, st);
    if (auto_n == 0) {
        lv_obj_add_state(run_row.row, LV_STATE_DISABLED);
    }

    int first = -1, cnt = 0;
    test_domain_range(d, &first, &cnt);
    if (cnt > 24) {
        cnt = 24;
    }
    st->count = cnt;
    for (int i = 0; i < cnt; ++i) {
        const test_case_t *tc = test_at(first + i);
        if (!tc) {
            continue;
        }
        strlcpy(st->ids[i], tc->id, sizeof(st->ids[i]));

        uiw_row_t row;
        uiw_row_create(content, s_domain_icons[d], tc->name, &row);
        /* Flags hint until evidence exists (C.4 precheck semantics). */
        const char *hint = "";
        if ((tc->flags & TEST_F_NEEDS_SD) != 0) {
            hint = "needs SD card";
        } else if ((tc->flags & TEST_F_NEEDS_USB_DISK) != 0) {
            hint = "needs USB disk";
        } else if ((tc->flags & TEST_F_INTERACTIVE) != 0) {
            hint = "interactive";
        } else if ((tc->flags & TEST_F_LONG) != 0) {
            hint = "long test";
        }
        lv_label_set_text(row.sub_lbl, hint);

        st->chip[i] = uiw_chip_create(row.row);
        lv_obj_align(st->chip[i], LV_ALIGN_RIGHT_MID, -14, 0);
        st->sub[i] = row.sub_lbl;

        /* The title/sub band leaves the chip zone free (B.3 right slot). */
        lv_obj_add_event_cb(row.row, run_view_open_cb, LV_EVENT_CLICKED,
                            (void *)st->ids[i]);
    }

    st->timer = lv_timer_create(detail_timer_cb, DETAIL_REFRESH_MS, st);
    lv_obj_add_event_cb(root, detail_delete_cb, LV_EVENT_DELETE, st);
    return root;
}

static void domain_card_cb(lv_event_t *event)
{
    const test_domain_t d = (test_domain_t)(intptr_t)
        lv_event_get_user_data(event);
    lv_obj_t *screen = domain_detail_create(d);
    if (screen) {
        ui_nav_push_screen(screen);
    }
}

/* ------------------------------------------------------------------ */
/* App entry                                                           */
/* ------------------------------------------------------------------ */

lv_obj_t *app_test_center_create(void)
{
    memset(&s_ov, 0, sizeof(s_ov));

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("Test center", &content);
    s_ov.screen = root;

    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, UI_GAP, 0);
    /* Keep the last badge row clear of the fixed action row. */
    lv_obj_set_style_pad_bottom(content, UI_ACTION_ROW_H + 8, 0);

    summary_card_create(content);

    /* 3x3 badge wall: flex wrap container, 12 px gaps. */
    lv_obj_t *wall = lv_obj_create(content);
    lv_obj_set_width(wall, UI_ROW_W);
    lv_obj_set_height(wall, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wall, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wall, 0, 0);
    lv_obj_set_style_radius(wall, 0, 0);
    lv_obj_set_style_pad_all(wall, 0, 0);
    lv_obj_set_style_pad_row(wall, UI_GAP, 0);
    lv_obj_set_style_pad_column(wall, UI_GAP, 0);
    lv_obj_set_flex_flow(wall, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(wall, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(wall, LV_OBJ_FLAG_SCROLLABLE);
    for (int d = 0; d < TEST_DOM_COUNT; ++d) {
        domain_card_create(wall, (test_domain_t)d);
    }

    action_row_create(root);

    svc_test_subscribe(overview_test_event, NULL);
    s_ov.timer = lv_timer_create(overview_timer_cb, OVERVIEW_REFRESH_MS,
                                 NULL);
    lv_obj_add_event_cb(root, overview_delete_cb, LV_EVENT_DELETE, NULL);

    overview_refresh();
    return root;
}
