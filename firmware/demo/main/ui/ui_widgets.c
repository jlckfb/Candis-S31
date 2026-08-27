/*
 * Candis-S31 watch demo - shared widgets (redesign spec B.4).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_widgets.h"

#include "ui_manager.h"
#include "ui_theme.h"

/* ------------------------------------------------------------------ */
/* Status chip                                                         */
/* ------------------------------------------------------------------ */

lv_obj_t *uiw_chip_create(lv_obj_t *parent)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_height(chip, UI_CHIP_H);
    lv_obj_set_style_radius(chip, UI_CHIP_H / 2, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_shadow_width(chip, 0, 0);
    lv_obj_set_style_pad_hor(chip, 10, 0);
    lv_obj_set_style_pad_ver(chip, 0, 0);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(chip);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_center(label);

    uiw_chip_set(chip, UIW_CHIP_NOT_RUN);
    return chip;
}

void uiw_chip_set(lv_obj_t *chip, uiw_chip_state_t state)
{
    if (!chip) {
        return;
    }
    lv_obj_t *label = lv_obj_get_child(chip, 0);

    uint32_t bg;
    uint32_t fg;
    const char *text;
    switch (state) {
    case UIW_CHIP_PASS:
        bg = UI_COL_PASS_DIM; fg = UI_COL_PASS; text = "PASS";
        break;
    case UIW_CHIP_FAIL:
        bg = UI_COL_FAIL_DIM; fg = UI_COL_FAIL; text = "FAIL";
        break;
    case UIW_CHIP_WARN:
        bg = UI_COL_WARN_DIM; fg = UI_COL_WARN; text = "WARN";
        break;
    case UIW_CHIP_SKIP:
        bg = UI_COL_IDLE_DIM; fg = UI_COL_TEXT_DIM; text = "SKIP";
        break;
    case UIW_CHIP_RUN:
        bg = UI_COL_ACCENT_DIM; fg = UI_COL_ACCENT; text = "RUN";
        break;
    case UIW_CHIP_NOT_RUN:
    default:
        bg = UI_COL_IDLE_DIM; fg = UI_COL_IDLE; text = "NOT RUN";
        break;
    }

    lv_obj_set_style_bg_color(chip, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    if (label) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    }
}

/* ------------------------------------------------------------------ */
/* List row                                                            */
/* ------------------------------------------------------------------ */

lv_obj_t *uiw_row_create(lv_obj_t *parent, const char *icon,
                         const char *title, uiw_row_t *out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, UI_ROW_W, UI_ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_ROW_RADIUS, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COL_HAIRLINE), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Press feedback: color swap only, never transform_scale. */
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_SURFACE_2),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COL_ACCENT),
                                  LV_STATE_PRESSED);

    /* Leading FA icon (accent), centered in a 48 px slot. */
    lv_obj_t *icon_lbl = lv_label_create(row);
    lv_label_set_text(icon_lbl, icon ? icon : "");
    lv_obj_set_style_text_font(icon_lbl, ui_font_body(), 0);
    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_pos(icon_lbl, 14, 0);
    lv_obj_set_height(icon_lbl, UI_ROW_H);
    lv_obj_set_style_text_align(icon_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_top(icon_lbl, (UI_ROW_H - 24) / 2, 0);

    lv_obj_t *title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, title ? title : "");
    lv_obj_set_style_text_font(title_lbl, ui_font_body_lg(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_width(title_lbl, UI_ROW_W - 56 - 194);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title_lbl, 56, 8);

    lv_obj_t *sub_lbl = lv_label_create(row);
    lv_label_set_text(sub_lbl, "");
    lv_obj_set_style_text_font(sub_lbl, ui_font_text(), 0);
    lv_obj_set_style_text_color(sub_lbl, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_set_width(sub_lbl, UI_ROW_W - 56 - 194);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(sub_lbl, 56, 36);

    /* Right-aligned evidence/status text slot (B.3: <=180 px). */
    lv_obj_t *status_lbl = lv_label_create(row);
    lv_label_set_text(status_lbl, "");
    lv_obj_set_style_text_font(status_lbl, ui_font_text(), 0);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_set_width(status_lbl, 180);
    lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(status_lbl, LV_ALIGN_RIGHT_MID, -14, 0);

    if (out) {
        out->row = row;
        out->status_lbl = status_lbl;
        out->sub_lbl = sub_lbl;
    }
    return row;
}

/* ------------------------------------------------------------------ */
/* Section header                                                      */
/* ------------------------------------------------------------------ */

lv_obj_t *uiw_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_set_size(section, UI_SECTION_W, UI_SECTION_H);
    lv_obj_set_style_bg_opa(section, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(section, 0, 0);
    lv_obj_set_style_radius(section, 0, 0);
    lv_obj_set_style_pad_all(section, 0, 0);
    lv_obj_set_style_shadow_width(section, 0, 0);
    lv_obj_remove_flag(section,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(section);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, ui_font_text(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 4, -6);

    return section;
}

/* ------------------------------------------------------------------ */
/* Progress bar                                                        */
/* ------------------------------------------------------------------ */

lv_obj_t *uiw_progress_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, UI_ROW_W, UI_PROGRESS_H);
    lv_obj_set_style_radius(bar, UI_PROGRESS_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_SURFACE_2),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, UI_PROGRESS_RADIUS, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

/* ------------------------------------------------------------------ */
/* Operator-confirm panel                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    lv_obj_t *lbl_question;
    lv_obj_t *arc;
    uiw_ask_cb_t cb;
    void *user;
} uiw_ask_ctx_t;

static void ask_button_cb(lv_event_t *event)
{
    lv_obj_t *btn = lv_event_get_target_obj(event);
    const bool yes = lv_obj_get_user_data(btn) != NULL;
    lv_obj_t *panel = lv_obj_get_parent(btn);
    uiw_ask_ctx_t *ctx = lv_obj_get_user_data(panel);
    if (!ctx) {
        return;
    }
    uiw_ask_cb_t cb = ctx->cb;
    void *user = ctx->user;
    uiw_ask_panel_show(panel, false);
    if (cb) {
        cb(yes, user);
    }
}

static void ask_panel_delete_cb(lv_event_t *event)
{
    lv_obj_t *panel = lv_event_get_target_obj(event);
    uiw_ask_ctx_t *ctx = lv_obj_get_user_data(panel);
    if (ctx) {
        lv_free(ctx);
        lv_obj_set_user_data(panel, NULL);
    }
}

static lv_obj_t *ask_button_create(lv_obj_t *panel, const char *text,
                                   bool primary, bool yes,
                                   uiw_ask_ctx_t *ctx)
{
    (void)ctx;
    lv_obj_t *btn = lv_button_create(panel);
    lv_obj_set_size(btn, 200, UI_ASK_BTN_H);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    /* B.4 button language: primary = ACCENT_DIM fill + ACCENT 1 px edge,
     * secondary = SURFACE + HAIRLINE. Press = color swap only. */
    lv_obj_set_style_bg_color(btn,
                              lv_color_hex(primary ? UI_COL_ACCENT_DIM
                                                   : UI_COL_SURFACE),
                              0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn,
                                  lv_color_hex(primary ? UI_COL_ACCENT
                                                       : UI_COL_HAIRLINE),
                                  0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_SURFACE_2),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COL_ACCENT),
                                  LV_STATE_PRESSED);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_body(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(label);

    lv_obj_set_user_data(btn, yes ? (void *)1 : NULL);
    lv_obj_add_event_cb(btn, ask_button_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

lv_obj_t *uiw_ask_panel(lv_obj_t *parent, const char *question,
                        uiw_ask_cb_t cb, void *user)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, UI_ROW_W, 160);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    uiw_ask_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (!ctx) {
        lv_obj_delete(panel);
        return NULL;
    }
    ctx->cb = cb;
    ctx->user = user;

    ctx->lbl_question = lv_label_create(panel);
    lv_label_set_text(ctx->lbl_question, question ? question : "");
    lv_obj_set_style_text_font(ctx->lbl_question, ui_font_body_lg(), 0);
    lv_obj_set_style_text_color(ctx->lbl_question,
                                lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_width(ctx->lbl_question, 340);
    lv_label_set_long_mode(ctx->lbl_question, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ctx->lbl_question, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ctx->lbl_question, LV_ALIGN_TOP_MID, 0, 12);

    /* Countdown ring (hidden until uiw_ask_panel_set_countdown >= 0). */
    ctx->arc = lv_arc_create(panel);
    lv_obj_set_size(ctx->arc, 40, 40);
    lv_obj_align(ctx->arc, LV_ALIGN_TOP_RIGHT, 6, 8);
    lv_arc_set_rotation(ctx->arc, 270);
    lv_arc_set_bg_angles(ctx->arc, 0, 360);
    lv_arc_set_range(ctx->arc, 0, 100);
    lv_obj_remove_flag(ctx->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(ctx->arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->arc, 0, 0);
    lv_obj_set_style_arc_width(ctx->arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ctx->arc, lv_color_hex(UI_COL_TRACK),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ctx->arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ctx->arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ctx->arc, lv_color_hex(UI_COL_WARN),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ctx->arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ctx->arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ctx->arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(ctx->arc, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ctx->arc, 0, LV_PART_KNOB);
    lv_obj_add_flag(ctx->arc, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *btn_yes = ask_button_create(panel, "YES", true, true, ctx);
    lv_obj_align(btn_yes, LV_ALIGN_BOTTOM_LEFT, 6, -8);
    lv_obj_t *btn_no = ask_button_create(panel, "NO", false, false, ctx);
    lv_obj_align(btn_no, LV_ALIGN_BOTTOM_RIGHT, -6, -8);

    lv_obj_set_user_data(panel, ctx);
    lv_obj_add_event_cb(panel, ask_panel_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    return panel;
}

void uiw_ask_panel_set_question(lv_obj_t *panel, const char *question)
{
    uiw_ask_ctx_t *ctx = panel ? lv_obj_get_user_data(panel) : NULL;
    if (ctx && ctx->lbl_question) {
        lv_label_set_text(ctx->lbl_question, question ? question : "");
    }
}

void uiw_ask_panel_show(lv_obj_t *panel, bool show)
{
    if (!panel) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void uiw_ask_panel_set_countdown(lv_obj_t *panel, int remain_pct)
{
    uiw_ask_ctx_t *ctx = panel ? lv_obj_get_user_data(panel) : NULL;
    if (!ctx || !ctx->arc) {
        return;
    }
    if (remain_pct < 0) {
        lv_obj_add_flag(ctx->arc, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (remain_pct > 100) {
        remain_pct = 100;
    }
    lv_obj_remove_flag(ctx->arc, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(ctx->arc, remain_pct);
}
