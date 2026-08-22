/*
 * Candis-S31 watch demo - app launcher menu (two-column card grid).
 *
 * Launcher cards sit in a two-column flex-wrap grid per DESIGN.md:
 * 208x112 px cards, 12 px gaps, 16 px side margins
 * (208*2 + 12 + 16*2 = 460). Cards give press feedback through plain
 * color state swaps only - no scale transforms (see menu_tile_create:
 * they stalled the draw dispatcher and were removed 2026-08-21). The grid
 * scrolls with momentum; leaving the menu is done through navigation
 * (BOOT key / ui_nav_request HOME), not a swipe gesture.
 *
 * The card list is driven by the app registry (ui_app_count /
 * ui_app_at), so newly registered apps appear automatically in
 * registration order; Test Center stays first because it registers
 * first.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_menu.h"

#include <stdlib.h>

#include "ui_manager.h"

/* Tile geometry: two columns of large cards (DESIGN.md: two-column
 * launcher card ~208x112 px, 12 px gaps, 16 px side margins). */
#define MENU_TILE_W 208
#define MENU_TILE_H 112
#define MENU_HEADER_H 64
#define MENU_CARD_RADIUS 18

static void menu_app_cb(lv_event_t *event)
{
    const ui_app_t *app = lv_event_get_user_data(event);
    if (app) {
        ui_nav_open(app->id);
    }
}

static lv_obj_t *menu_tile_create(lv_obj_t *grid, const ui_app_t *app)
{
    lv_obj_t *btn = lv_button_create(grid);
    lv_obj_set_size(btn, MENU_TILE_W, MENU_TILE_H);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, MENU_CARD_RADIUS, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2C2C34), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Pressed look: plain color swap ONLY. transform_scale press styles
     * created layer-transform draw tasks that made lv_draw_dispatch()
     * spin with zero progress until IDLE starved; removed 2026-08-21.
     * Do not reintroduce scale transforms here. */
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT),
                                  LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x232329), LV_STATE_PRESSED);

    lv_obj_add_event_cb(btn, menu_app_cb, LV_EVENT_CLICKED, (void *)app);

    const bool unavailable = (app->availability != UI_APP_AVAILABLE);

    /* Icon in the top-left corner; dimmed when the app is unavailable. */
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, app->icon);
    lv_obj_set_style_text_font(icon, ui_font_body(), 0);
    lv_obj_set_style_text_color(icon,
                                lv_color_hex(unavailable
                                                 ? UI_COLOR_TEXT_DIM
                                                 : UI_COLOR_ACCENT),
                                0);
    lv_obj_set_pos(icon, 16, 14);

    /* Availability badge pinned top-right; the 124 px title band below
     * keeps both labels clear of each other. */
    if (unavailable) {
        lv_obj_t *badge = lv_label_create(btn);
        lv_label_set_text(badge, app->availability_text
                                    ? app->availability_text : "\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8");
        lv_obj_set_style_text_color(badge, lv_color_hex(UI_COLOR_WARN), 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x332814), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, 8, 0);
        lv_obj_set_style_pad_hor(badge, 6, 0);
        lv_obj_set_style_pad_ver(badge, 2, 0);
        lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -8, 8);
    }

    /* Card title in a fixed 176 px band at the bottom row: 20 px CJK
     * (ui_font_body_lg per DESIGN.md), single line with ellipsis. */
    lv_obj_t *name = lv_label_create(btn);
    lv_label_set_text(name, app->title);
    lv_obj_set_style_text_font(name, ui_font_body_lg(), 0);
    lv_obj_set_style_text_color(name,
                                lv_color_hex(unavailable ? UI_COLOR_TEXT_DIM
                                                         : UI_COLOR_TEXT),
                                0);
    lv_obj_set_width(name, 176);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 16, -14);

    return btn;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

lv_obj_t *ui_menu_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Header: accent tick + title on the left, hint on the right. */
    lv_obj_t *tick = lv_obj_create(screen);
    lv_obj_set_size(tick, 5, 28);
    lv_obj_set_pos(tick, UI_SCREEN_PAD, UI_CONTENT_Y + 18);
    lv_obj_set_style_bg_color(tick, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tick, 3, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "All apps");
    lv_obj_set_style_text_font(title, ui_font_title(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(title, 32, UI_CONTENT_Y + 24);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text_fmt(hint, "%d apps", ui_app_count());
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -UI_SCREEN_PAD,
                 UI_CONTENT_Y + 25);

    /* Scrollable two-column tile grid; momentum is on by default.
     * ROW_WRAP puts two 208 px cards per row (12 px column gap, 16 px
     * side margins) and wraps the rest; row gap is 12 px. */
    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_set_size(grid, 460, 460 - UI_CONTENT_Y - MENU_HEADER_H);
    lv_obj_set_pos(grid, 0, UI_CONTENT_Y + MENU_HEADER_H);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_style_pad_left(grid, UI_SCREEN_PAD, 0);
    lv_obj_set_style_pad_right(grid, UI_SCREEN_PAD, 0);
    lv_obj_set_style_pad_top(grid, 2, 0);
    lv_obj_set_style_pad_bottom(grid, 2, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(grid, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(grid, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(grid, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(grid, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(grid, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(grid, 3, LV_PART_SCROLLBAR);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);

    for (int i = 0; i < ui_app_count(); ++i) {
        const ui_app_t *app = ui_app_at(i);
        if (!app) {
            continue;
        }
        menu_tile_create(grid, app);
    }

    return screen;
}
