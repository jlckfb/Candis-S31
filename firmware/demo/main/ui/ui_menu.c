/*
 * Candis-S31 watch demo - app launcher menu (three-column card grid).
 *
 * Launcher cards sit in a three-column flex-wrap grid (redesign spec
 * B.6.2): 132x112 px cards, 12 px gaps, 16 px side margins
 * (132*3 + 12*2 + 16*2 = 452 <= 460). Group section headers are
 * full-width 428x36 flex items that force a row wrap. Cards give press
 * feedback through plain color state swaps only - no scale transforms
 * (they stalled the draw dispatcher and were removed 2026-08-21; do not
 * reintroduce them). The grid scrolls with momentum; leaving the menu is
 * done through navigation (BOOT key / ui_nav_request HOME), not a swipe
 * gesture.
 *
 * Grouping and order follow spec A.2 (TESTS / HARDWARE / CONNECTIVITY /
 * SYSTEM / GAMES); the display order is driven by the id table below,
 * not by the registration order, so demo_apps.c stays untouched. Apps
 * registered but missing from the table are appended under MORE.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_menu.h"

#include <stdint.h>
#include <string.h>

#include "ui_manager.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#define MENU_HEADER_H 64

/* Spec A.2 grouping. Ids missing here fall into the MORE catch-all. */
static const char *const s_group_tests[] = { "tests" };
static const char *const s_group_hardware[] = {
    "display", "camera", "recorder", "player", "led",
};
static const char *const s_group_connectivity[] = {
    "wifi", "ble", "usb", "files",
};
static const char *const s_group_system[] = {
    "power", "settings", "sysinfo",
};
static const char *const s_group_games[] = {
    "game2048", "snake", "breakout",
};

typedef struct {
    const char *title;
    const char *const *ids;
    int count;
} menu_group_t;

static const menu_group_t s_groups[] = {
    { "TESTS", s_group_tests, 1 },
    { "HARDWARE", s_group_hardware, 5 },
    { "CONNECTIVITY", s_group_connectivity, 4 },
    { "SYSTEM", s_group_system, 3 },
    { "GAMES", s_group_games, 3 },
};

#define MENU_GROUP_COUNT (sizeof(s_groups) / sizeof(s_groups[0]))

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
    lv_obj_set_size(btn, UI_TILE_W, UI_TILE_H);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, UI_TILE_RADIUS, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COL_HAIRLINE), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Pressed look: plain color swap ONLY. transform_scale press styles
     * created layer-transform draw tasks that made lv_draw_dispatch()
     * spin with zero progress until IDLE starved; removed 2026-08-21.
     * Do not reintroduce scale transforms here. */
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COL_ACCENT),
                                  LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_SURFACE_2),
                              LV_STATE_PRESSED);

    lv_obj_add_event_cb(btn, menu_app_cb, LV_EVENT_CLICKED, (void *)app);

    const bool unavailable = (app->availability != UI_APP_AVAILABLE);

    /* Icon in the top-left corner; dimmed when the app is unavailable. */
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, app->icon);
    lv_obj_set_style_text_font(icon, ui_font_body(), 0);
    lv_obj_set_style_text_color(icon,
                                lv_color_hex(unavailable
                                                 ? UI_COL_TEXT_DIM
                                                 : UI_COL_ACCENT),
                                0);
    lv_obj_set_pos(icon, 16, 14);

    /* Availability badge pinned top-right, width-bounded so the reason
     * text never overflows the 132 px card. */
    if (unavailable) {
        lv_obj_t *badge = lv_label_create(btn);
        lv_label_set_text(badge, app->availability_text
                                    ? app->availability_text : "\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8");
        lv_obj_set_style_text_color(badge, lv_color_hex(UI_COL_WARN), 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(UI_COL_WARN_DIM), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, 8, 0);
        lv_obj_set_style_pad_hor(badge, 6, 0);
        lv_obj_set_style_pad_ver(badge, 2, 0);
        lv_obj_set_width(badge, 100);
        lv_label_set_long_mode(badge, LV_LABEL_LONG_DOT);
        lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -8, 8);
    }

    /* Card title in a bounded band at the bottom row: 20 px project font
     * (B.2 body-lg), single line with ellipsis. */
    lv_obj_t *name = lv_label_create(btn);
    lv_label_set_text(name, app->title);
    lv_obj_set_style_text_font(name, ui_font_body_lg(), 0);
    lv_obj_set_style_text_color(name,
                                lv_color_hex(unavailable ? UI_COL_TEXT_DIM
                                                         : UI_COL_TEXT),
                                0);
    lv_obj_set_width(name, UI_TILE_W - 24);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 12, -10);

    return btn;
}

/* Find the registry index of an app id, or -1. */
static int menu_find_app(const char *id)
{
    const int count = ui_app_count();
    for (int i = 0; i < count && i < 32; ++i) {
        const ui_app_t *app = ui_app_at(i);
        if (app && app->id && strcmp(app->id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

lv_obj_t *ui_menu_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Header: accent tick + title on the left, hint on the right. */
    lv_obj_t *tick = lv_obj_create(screen);
    lv_obj_set_size(tick, 5, 28);
    lv_obj_set_pos(tick, UI_SCREEN_PAD, UI_CONTENT_Y + 18);
    lv_obj_set_style_bg_color(tick, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tick, 3, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Apps");
    lv_obj_set_style_text_font(title, ui_font_title(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_pos(title, 32, UI_CONTENT_Y + 24);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text_fmt(hint, "%d apps", ui_app_count());
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -UI_SCREEN_PAD,
                 UI_CONTENT_Y + 25);

    /* Scrollable three-column tile grid; momentum is on by default.
     * ROW_WRAP fits three 132 px cards per row (12 px column gap, 16 px
     * side margins); full-width section headers force row breaks. */
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
    lv_obj_set_style_pad_row(grid, UI_GAP, 0);
    lv_obj_set_style_pad_column(grid, UI_GAP, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(grid, lv_color_hex(UI_COL_ACCENT),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(grid, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(grid, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(grid, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(grid, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(grid, 3, LV_PART_SCROLLBAR);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Render groups in the A.2 order; track consumed registry slots. */
    uint32_t rendered_mask = 0;
    for (size_t g = 0; g < MENU_GROUP_COUNT; ++g) {
        const menu_group_t *group = &s_groups[g];
        bool header_done = false;
        for (int k = 0; k < group->count; ++k) {
            const int idx = menu_find_app(group->ids[k]);
            if (idx < 0) {
                continue; /* id not registered on this build */
            }
            if (!header_done) {
                uiw_section(grid, group->title);
                header_done = true;
            }
            menu_tile_create(grid, ui_app_at(idx));
            rendered_mask |= UINT32_C(1) << idx;
        }
    }

    /* Catch-all: registered apps missing from the group table must not
     * silently vanish from the launcher. */
    const int count = ui_app_count();
    bool more_header_done = false;
    for (int i = 0; i < count && i < 32; ++i) {
        if ((rendered_mask & (UINT32_C(1) << i)) != 0) {
            continue;
        }
        const ui_app_t *app = ui_app_at(i);
        if (!app) {
            continue;
        }
        if (!more_header_done) {
            uiw_section(grid, "MORE");
            more_header_done = true;
        }
        menu_tile_create(grid, app);
    }

    return screen;
}
