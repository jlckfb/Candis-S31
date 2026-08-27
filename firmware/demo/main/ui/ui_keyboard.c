/*
 * Candis-S31 watch demo - modal on-screen keyboard.
 *
 * Full-screen modal (below the status bar, which stays visible) hosting a
 * one-line text area plus lv_keyboard. Confirm via the top-row "OK"
 * button or the keyboard's own OK key; cancel via "Cancel", the X button or
 * the keyboard's own close key. Password mode masks input and offers an
 * eye toggle to peek at the plain text.
 *
 * Must be called on the LVGL thread. Only one instance may be open;
 * overlapping opens are ignored.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_keyboard.h"

#include <stddef.h>
#include <string.h>

#include "lvgl.h"

#include "ui_manager.h"

#define KB_MAX_LEN 127

typedef struct {
    void (*cb)(const char *text, void *user);
    void *user;
    lv_obj_t *modal;
    lv_obj_t *ta;
    lv_obj_t *peek_icon;
    bool finished;
} kb_ctx_t;

static kb_ctx_t *s_ctx;

/* No transform_scale press transitions: they create layer-transform draw
 * tasks that stalled lv_draw_dispatch() with zero progress and starved
 * IDLE0 (see ui_menu.c, 2026-08-21). Pressed feedback is a plain color
 * swap, matching the menu tiles. */

/* ------------------------------------------------------------------ */
/* Teardown / callbacks                                                */
/* ------------------------------------------------------------------ */

static void kb_modal_delete_cb(lv_event_t *event)
{
    kb_ctx_t *ctx = lv_event_get_user_data(event);
    if (s_ctx == ctx) {
        s_ctx = NULL;
    }
    lv_free(ctx);
}

/* Deferred modal deletion: lv_keyboard's built-in handler keeps touching
 * its textarea after firing READY/CANCEL, so the modal must outlive the
 * current event dispatch. lv_async_call runs on the next LVGL tick. */
static void kb_delete_deferred(void *arg)
{
    lv_obj_delete(arg);
}

/* Runs the user callback, then tears the modal down on the next tick. */
static void kb_finish(kb_ctx_t *ctx, const char *text)
{
    if (!ctx || ctx->finished) {
        return;
    }
    ctx->finished = true;
    void (*cb)(const char *, void *) = ctx->cb;
    void *user = ctx->user;
    lv_obj_t *modal = ctx->modal;
    if (cb) {
        cb(text, user);
    }
    lv_async_call(kb_delete_deferred, modal);
}

static void kb_confirm(kb_ctx_t *ctx)
{
    char buf[KB_MAX_LEN + 1];
    const char *txt = lv_textarea_get_text(ctx->ta);
    size_t len = strlen(txt);
    if (len > KB_MAX_LEN) {
        len = KB_MAX_LEN;
    }
    memcpy(buf, txt, len);
    buf[len] = '\0';
    kb_finish(ctx, buf);
}

static void kb_confirm_cb(lv_event_t *event)
{
    (void)event;
    if (s_ctx) {
        kb_confirm(s_ctx);
    }
}

static void kb_cancel_cb(lv_event_t *event)
{
    (void)event;
    if (s_ctx) {
        kb_finish(s_ctx, NULL);
    }
}

/* lv_keyboard (a buttonmatrix) fires READY on its OK key. */
static void kb_widget_ready_cb(lv_event_t *event)
{
    (void)event;
    if (s_ctx) {
        kb_confirm(s_ctx);
    }
}

/* ... and CANCEL on its close key. */
static void kb_widget_cancel_cb(lv_event_t *event)
{
    (void)event;
    if (s_ctx) {
        kb_finish(s_ctx, NULL);
    }
}

static void kb_peek_cb(lv_event_t *event)
{
    (void)event;
    kb_ctx_t *ctx = s_ctx;
    if (!ctx || ctx->finished) {
        return;
    }
    bool masked = lv_textarea_get_password_mode(ctx->ta);
    lv_textarea_set_password_mode(ctx->ta, !masked);
    if (ctx->peek_icon) {
        lv_label_set_text(ctx->peek_icon,
                          masked ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    }
}

/* ------------------------------------------------------------------ */
/* Builders                                                            */
/* ------------------------------------------------------------------ */

static lv_obj_t *kb_button_create(lv_obj_t *parent, const char *text,
                                  uint32_t bg_color)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 140, UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color == UI_COL_ACCENT
                                                    ? 0x3A7FD0 : 0x2C2C34),
                              LV_STATE_PRESSED);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_body(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(label);
    return btn;
}

static void kb_style_textarea(lv_obj_t *ta)
{
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x30303A), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_COL_ACCENT),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_left(ta, 12, 0);
    lv_obj_set_style_pad_right(ta, 12, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_style_text_font(ta, ui_font_body(), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_COL_TEXT_DIM),
                                LV_PART_TEXTAREA_PLACEHOLDER);
}

static void kb_style_keyboard(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 6, 0);
    lv_obj_set_style_pad_row(kb, 5, 0);
    lv_obj_set_style_pad_column(kb, 5, 0);

    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COL_SURFACE),
                              LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_COL_TEXT),
                                LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x2E2E36),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x24324A),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void ui_keyboard_open(const char *title, const char *initial,
                      bool password_mode,
                      void (*cb)(const char *text, void *user), void *user)
{
    if (s_ctx) {
        return; /* one modal at a time */
    }
    kb_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (!ctx) {
        if (cb) {
            cb(NULL, user);
        }
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->cb = cb;
    ctx->user = user;
    s_ctx = ctx;

    /* Modal panel below the status bar so the bar stays visible. */
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    ctx->modal = modal;
    lv_obj_set_size(modal, 460, 460 - UI_STATUS_BAR_HEIGHT);
    lv_obj_set_pos(modal, 0, UI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, kb_modal_delete_cb, LV_EVENT_DELETE, ctx);

    lv_obj_t *title_lbl = lv_label_create(modal);
    lv_label_set_text(title_lbl, (title && title[0]) ? title : "Input");
    lv_obj_set_style_text_font(title_lbl, ui_font_title(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_pos(title_lbl, 18, 12);

    lv_obj_t *close_btn = lv_button_create(modal);
    lv_obj_set_size(close_btn, UI_TOUCH_MIN, UI_TOUCH_MIN);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, kb_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_center(close_lbl);

    /* Text area; the peek button steals width in password mode. */
    lv_obj_t *ta = lv_textarea_create(modal);
    ctx->ta = ta;
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, KB_MAX_LEN);
    lv_textarea_set_placeholder_text(ta, password_mode ? "Enter password" : "Enter text");
    lv_obj_set_size(ta, password_mode ? 364 : 428, UI_TOUCH_MIN);
    lv_obj_set_pos(ta, 16, 64);
    kb_style_textarea(ta);
    if (initial && initial[0]) {
        lv_textarea_set_text(ta, initial);
        lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    }

    if (password_mode) {
        lv_textarea_set_password_mode(ta, true);
        lv_obj_t *peek = lv_button_create(modal);
        lv_obj_set_size(peek, UI_TOUCH_MIN, UI_TOUCH_MIN);
        lv_obj_set_pos(peek, 460 - 16 - UI_TOUCH_MIN, 64);
        lv_obj_set_style_bg_color(peek, lv_color_hex(UI_COL_SURFACE), 0);
        lv_obj_set_style_bg_opa(peek, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(peek, 12, 0);
        lv_obj_set_style_border_width(peek, 0, 0);
        lv_obj_set_style_shadow_width(peek, 0, 0);
        lv_obj_add_event_cb(peek, kb_peek_cb, LV_EVENT_CLICKED, NULL);
        ctx->peek_icon = lv_label_create(peek);
        lv_label_set_text(ctx->peek_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(ctx->peek_icon,
                                    lv_color_hex(UI_COL_TEXT_DIM), 0);
        lv_obj_center(ctx->peek_icon);
    }

    lv_obj_t *cancel_btn = kb_button_create(modal, "Cancel", 0x232329);
    lv_obj_set_pos(cancel_btn, 16, 128);
    lv_obj_add_event_cb(cancel_btn, kb_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok_btn = kb_button_create(modal, "OK", UI_COL_ACCENT);
    lv_obj_align(ok_btn, LV_ALIGN_TOP_RIGHT, -16, 128);
    lv_obj_add_event_cb(ok_btn, kb_confirm_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *kb = lv_keyboard_create(modal);
    lv_keyboard_set_textarea(kb, ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(kb, 460, 238);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    kb_style_keyboard(kb);
    lv_obj_add_event_cb(kb, kb_widget_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_widget_cancel_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}
