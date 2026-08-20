/*
 * Candis-S31 watch demo - app launcher menu (icon grid).
 *
 * Three-column card grid below a slim header. Tiles give press feedback
 * via a state transition (scale down + accent border). The grid scrolls
 * with momentum; swiping the page down returns to the watchface.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_menu.h"

#include <stdlib.h>

#include "ui_manager.h"

/* Down-swipe distance (px) that dismisses the menu. */
#define MENU_SWIPE_MIN_DY 80

/* Tile geometry: 3 columns x 132 px + gaps fit the 460 px panel. */
#define MENU_TILE_W 132
#define MENU_TILE_H 108
#define MENU_HEADER_H 64

/* Press transition shared by all tiles. */
static const lv_style_prop_t s_tile_trans_props[] = {
    LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_BORDER_COLOR, LV_STYLE_BG_COLOR,
    LV_STYLE_PROP_INV,
};
static lv_style_transition_dsc_t s_tile_trans;
static bool s_tile_style_ready;

/* Swipe-down detection state. */
static int s_press_x;
static int s_press_y;
static bool s_press_valid;

static void menu_tile_styles_ensure(void)
{
    if (s_tile_style_ready) {
        return;
    }
    lv_style_transition_dsc_init(&s_tile_trans, s_tile_trans_props,
                                 lv_anim_path_ease_out, 140, 0, NULL);
    s_tile_style_ready = true;
}

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
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x26262E), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_transform_scale(btn, LV_SCALE_NONE, 0);
    lv_obj_set_style_transition(btn, &s_tile_trans, 0);

    /* Pressed look: shrink slightly and light the border with the accent. */
    lv_obj_set_style_transform_scale(btn, 240, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT),
                                  LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x232329), LV_STATE_PRESSED);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 6, 0);
    lv_obj_add_event_cb(btn, menu_app_cb, LV_EVENT_CLICKED, (void *)app);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, app->icon);
    lv_obj_set_style_text_font(icon, ui_font_mid(), 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ACCENT), 0);

    lv_obj_t *name = lv_label_create(btn);
    lv_label_set_text(name, app->title);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT), 0);
    return btn;
}

/* ------------------------------------------------------------------ */
/* Swipe-down to dismiss                                               */
/* ------------------------------------------------------------------ */

static void menu_pressed_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_x = p.x;
    s_press_y = p.y;
    s_press_valid = true;
}

static void menu_released_cb(lv_event_t *event)
{
    if (!s_press_valid) {
        return;
    }
    s_press_valid = false;

    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dy = p.y - s_press_y; /* positive = moved down */
    if (dy > MENU_SWIPE_MIN_DY && dy > abs(p.x - s_press_x)) {
        ui_nav_home();
    }
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

lv_obj_t *ui_menu_create(void)
{
    menu_tile_styles_ensure();

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Header: accent tick + title on the left, hint on the right. */
    lv_obj_t *tick = lv_obj_create(screen);
    lv_obj_set_size(tick, 5, 22);
    lv_obj_set_pos(tick, 20, UI_CONTENT_Y + 16);
    lv_obj_set_style_bg_color(tick, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tick, 3, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "应用");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(title, 34, UI_CONTENT_Y + 17);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "下滑返回表盘");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -20, UI_CONTENT_Y + 20);

    /* Scrollable tile grid; momentum is on by default. */
    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_set_size(grid, 460, 460 - UI_CONTENT_Y - MENU_HEADER_H);
    lv_obj_set_pos(grid, 0, UI_CONTENT_Y + MENU_HEADER_H);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_style_pad_left(grid, 16, 0);
    lv_obj_set_style_pad_right(grid, 16, 0);
    lv_obj_set_style_pad_top(grid, 6, 0);
    lv_obj_set_style_pad_bottom(grid, 16, 0);
    lv_obj_set_style_pad_row(grid, 14, 0);
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

    for (int i = 0; i < ui_app_count(); ++i) {
        const ui_app_t *app = ui_app_at(i);
        if (!app) {
            continue;
        }
        menu_tile_create(grid, app);
    }

    lv_obj_add_event_cb(screen, menu_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen, menu_released_cb, LV_EVENT_RELEASED, NULL);
    return screen;
}
