/*
 * Candis-S31 watch demo - shared widgets (redesign spec B.4).
 *
 * Small themed building blocks used by the test center and any future
 * page: status chip, full-width list row, section header, progress bar
 * and the operator-confirm panel. All functions are LVGL-thread only.
 *
 * Press feedback is a plain color swap (bg -> SURFACE_2, border ->
 * ACCENT). Scale transforms are forbidden project-wide: they stalled the
 * draw dispatcher on 2026-08-21.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status badge states (B.4): h=24, r=12, uppercase Montserrat 16. */
typedef enum {
    UIW_CHIP_NOT_RUN = 0,  /* IDLE_DIM fill, IDLE text */
    UIW_CHIP_PASS,         /* PASS_DIM fill, PASS text */
    UIW_CHIP_FAIL,         /* FAIL_DIM fill, FAIL text */
    UIW_CHIP_WARN,         /* WARN_DIM fill, WARN text */
    UIW_CHIP_SKIP,         /* IDLE_DIM fill, TEXT_DIM text */
    UIW_CHIP_RUN,          /* ACCENT_DIM fill, ACCENT text (static) */
} uiw_chip_state_t;

/** Create a status chip (initially NOT RUN). */
lv_obj_t *uiw_chip_create(lv_obj_t *parent);

/** Restyle + relabel an existing chip. */
void uiw_chip_set(lv_obj_t *chip, uiw_chip_state_t state);

/* Full-width list row (B.3): 428x64, r14, SURFACE + hairline, leading
 * FA icon (accent), candis-20 title, sc16 subtitle and a right-aligned
 * sc16 status/evidence label (<=180 px). Clickable with color-swap press
 * feedback; children bubble events to the row. */
typedef struct {
    lv_obj_t *row;        /* the row container (attach CLICKED here) */
    lv_obj_t *status_lbl; /* right-aligned evidence/status text (sc16) */
    lv_obj_t *sub_lbl;    /* subtitle under the title (sc16, dim) */
} uiw_row_t;

lv_obj_t *uiw_row_create(lv_obj_t *parent, const char *icon,
                         const char *title, uiw_row_t *out);

/** Section header: full-width 428x36 transparent flex item, sc16 accent
 *  uppercase text. In a ROW_WRAP flex it forces a line break. */
lv_obj_t *uiw_section(lv_obj_t *parent, const char *text);

/** Test progress bar: 428x8, r4, SURFACE_2 track + ACCENT indicator,
 *  range 0..100. Update with lv_bar_set_value only (local invalidate). */
lv_obj_t *uiw_progress_create(lv_obj_t *parent);

/* Operator-confirm panel (B.6.4 run view): question text (candis 20,
 * centered), optional countdown ring (hidden until set_countdown) and
 * YES (primary) / NO (secondary) buttons, UI_ASK_BTN_H tall. Answering
 * invokes cb(yes, user) on the LVGL thread and auto-hides the panel. */
typedef void (*uiw_ask_cb_t)(bool yes, void *user);

lv_obj_t *uiw_ask_panel(lv_obj_t *parent, const char *question,
                        uiw_ask_cb_t cb, void *user);

/** Change the question text of an existing panel. */
void uiw_ask_panel_set_question(lv_obj_t *panel, const char *question);

/** Show/hide the panel (starts hidden). */
void uiw_ask_panel_show(lv_obj_t *panel, bool show);

/** Countdown ring: remaining 0..100 shows/updates the ring, -1 hides it. */
void uiw_ask_panel_set_countdown(lv_obj_t *panel, int remain_pct);

#ifdef __cplusplus
}
#endif
