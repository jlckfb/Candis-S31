/*
 * Candis-S31 watch demo - AMOLED and touch verification playground.
 *
 * The modes intentionally update bounded regions so the page also exercises
 * the verified TE-paced partial-refresh path instead of promising full-screen
 * 60 fps.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>

#include "demo_apps.h"
#include "ui/ui_manager.h"

typedef enum {
    DISPLAY_MODE_COLOR = 0,
    DISPLAY_MODE_TOUCH,
    DISPLAY_MODE_MOTION,
} display_mode_t;

static struct {
    lv_obj_t *root;
    lv_obj_t *stage;
    lv_obj_t *motion_dot;
    lv_obj_t *fullscreen;
    lv_obj_t *fullscreen_hint;
    lv_obj_t *fullscreen_dot;
    lv_timer_t *hint_timer;
    int color_index;
} s_display;

static const struct {
    uint32_t color;
    uint32_t text_color;
    const char *name;
} s_full_colors[] = {
    { 0xFF0000, 0xFFFFFF, "Red" },
    { 0x00FF00, 0x000000, "Green" },
    { 0x0000FF, 0xFFFFFF, "Blue" },
    { 0xFFFFFF, 0x000000, "White" },
    { 0x000000, 0xFFFFFF, "Black" },
};

static void fullscreen_close(void)
{
    if (s_display.hint_timer) {
        lv_timer_delete(s_display.hint_timer);
        s_display.hint_timer = NULL;
    }
    if (s_display.fullscreen) {
        lv_obj_delete(s_display.fullscreen);
    }
    s_display.fullscreen = NULL;
    s_display.fullscreen_hint = NULL;
    s_display.fullscreen_dot = NULL;
}

static void fullscreen_exit_cb(lv_event_t *event)
{
    (void)event;
    fullscreen_close();
}

static void fullscreen_hint_hide(lv_timer_t *timer)
{
    (void)timer;
    if (s_display.fullscreen_hint) {
        lv_obj_add_flag(s_display.fullscreen_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display.hint_timer) {
        lv_timer_delete(s_display.hint_timer);
        s_display.hint_timer = NULL;
    }
}

static void fullscreen_hint_restart(void)
{
    if (!s_display.fullscreen_hint) {
        return;
    }
    lv_obj_remove_flag(s_display.fullscreen_hint, LV_OBJ_FLAG_HIDDEN);
    if (s_display.hint_timer) {
        lv_timer_delete(s_display.hint_timer);
    }
    s_display.hint_timer = lv_timer_create(fullscreen_hint_hide, 900, NULL);
    lv_timer_set_repeat_count(s_display.hint_timer, 1);
}

static void fullscreen_color_refresh(void)
{
    const int count = (int)(sizeof(s_full_colors) / sizeof(s_full_colors[0]));
    const int index = s_display.color_index % count;
    lv_obj_set_style_bg_color(s_display.fullscreen,
                              lv_color_hex(s_full_colors[index].color), 0);
    lv_label_set_text_fmt(s_display.fullscreen_hint, "%s - tap to switch, hold to exit",
                          s_full_colors[index].name);
    lv_obj_set_style_text_color(s_display.fullscreen_hint,
                                lv_color_hex(s_full_colors[index].text_color), 0);
    fullscreen_hint_restart();
}

static void fullscreen_color_click_cb(lv_event_t *event)
{
    (void)event;
    s_display.color_index++;
    fullscreen_color_refresh();
}

static void fullscreen_touch_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev || !s_display.fullscreen || !s_display.fullscreen_dot ||
            !s_display.fullscreen_hint) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const int x = LV_CLAMP(0, (int)point.x - 14, 432);
    const int y = LV_CLAMP(0, (int)point.y - 14, 432);
    lv_obj_set_pos(s_display.fullscreen_dot, x, y);
    lv_label_set_text_fmt(s_display.fullscreen_hint,
                          "x:%d y:%d - check edges/corners, hold to exit",
                          (int)point.x, (int)point.y);
}

static void fullscreen_open(display_mode_t mode)
{
    fullscreen_close();
    lv_obj_t *panel = lv_obj_create(s_display.root);
    s_display.fullscreen = panel;
    lv_obj_set_size(panel, 460, 460);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, fullscreen_exit_cb, LV_EVENT_LONG_PRESSED,
                        NULL);

    s_display.fullscreen_hint = lv_label_create(panel);
    lv_obj_set_style_text_font(s_display.fullscreen_hint, ui_font_body(), 0);
    lv_obj_set_style_bg_color(s_display.fullscreen_hint,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_display.fullscreen_hint, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_display.fullscreen_hint, 12, 0);
    lv_obj_set_style_pad_hor(s_display.fullscreen_hint, 12, 0);
    lv_obj_set_style_pad_ver(s_display.fullscreen_hint, 8, 0);
    lv_obj_remove_flag(s_display.fullscreen_hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_display.fullscreen_hint, LV_ALIGN_TOP_MID, 0, 18);

    if (mode == DISPLAY_MODE_COLOR) {
        s_display.color_index = 0;
        lv_obj_add_event_cb(panel, fullscreen_color_click_cb,
                            LV_EVENT_CLICKED, NULL);
        fullscreen_color_refresh();
        return;
    }

    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101014), 0);
    lv_label_set_text(s_display.fullscreen_hint,
                      "Swipe edges/corners to check coords, hold to exit");
    lv_obj_set_style_text_color(s_display.fullscreen_hint,
                                lv_color_hex(UI_COLOR_TEXT), 0);
    s_display.fullscreen_dot = lv_obj_create(panel);
    lv_obj_set_size(s_display.fullscreen_dot, 28, 28);
    lv_obj_set_pos(s_display.fullscreen_dot, 216, 216);
    lv_obj_set_style_radius(s_display.fullscreen_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_display.fullscreen_dot,
                              lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_display.fullscreen_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_display.fullscreen_dot,
                                  lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_display.fullscreen_dot, 2, 0);
    lv_obj_remove_flag(s_display.fullscreen_dot,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, fullscreen_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(panel, fullscreen_touch_cb, LV_EVENT_PRESSING, NULL);
}

static void stage_clear(void)
{
    if (s_display.motion_dot) {
        lv_anim_delete(s_display.motion_dot, NULL);
    }
    lv_obj_clean(s_display.stage);
    s_display.motion_dot = NULL;
}

static void show_motion_mode(void)
{
    stage_clear();
    lv_obj_set_style_bg_color(s_display.stage, lv_color_hex(0x101014), 0);
    lv_obj_set_style_bg_opa(s_display.stage, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_display.stage);
    lv_label_set_text(title, "Partial-refresh motion, ~400 x 96 px dirty area");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *track = lv_obj_create(s_display.stage);
    lv_obj_set_size(track, 390, 96);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x17171C), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(track, lv_color_hex(0x2C2C34), 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_radius(track, 48, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_display.motion_dot = lv_obj_create(track);
    lv_obj_set_size(s_display.motion_dot, 72, 72);
    lv_obj_set_pos(s_display.motion_dot, 4, 4);
    lv_obj_set_style_radius(s_display.motion_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_display.motion_dot,
                              lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_display.motion_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_display.motion_dot, 0, 0);
    lv_obj_remove_flag(s_display.motion_dot,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_display.motion_dot);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&anim, 4, 310);
    lv_anim_set_duration(&anim, 900);
    lv_anim_set_playback_duration(&anim, 900);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);

    lv_obj_t *hint = lv_label_create(s_display.stage);
    lv_label_set_text(hint, "Watch for tearing, dropped frames, touch response");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -48);

    lv_obj_t *gradient = lv_obj_create(s_display.stage);
    lv_obj_set_size(gradient, 390, 28);
    lv_obj_align(gradient, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(gradient, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_color(gradient, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_grad_dir(gradient, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_color(gradient, lv_color_hex(0x3A3A42), 0);
    lv_obj_set_style_border_width(gradient, 1, 0);
    lv_obj_set_style_radius(gradient, 10, 0);
    lv_obj_remove_flag(gradient,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

static void mode_click_cb(lv_event_t *event)
{
    display_mode_t mode = (display_mode_t)(intptr_t)lv_event_get_user_data(event);
    if (mode == DISPLAY_MODE_COLOR) {
        fullscreen_open(DISPLAY_MODE_COLOR);
    } else if (mode == DISPLAY_MODE_TOUCH) {
        fullscreen_open(DISPLAY_MODE_TOUCH);
    } else {
        show_motion_mode();
    }
}

static lv_obj_t *mode_button(lv_obj_t *parent, const char *text,
                             display_mode_t mode)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 128, UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_ACCENT),
                              LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x2C2C34), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_add_event_cb(button, mode_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)mode);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_title(), 0);
    lv_obj_center(label);
    return button;
}

static void root_delete_cb(lv_event_t *event)
{
    (void)event;
    if (s_display.hint_timer) {
        lv_timer_delete(s_display.hint_timer);
        s_display.hint_timer = NULL;
    }
    if (s_display.motion_dot) {
        lv_anim_delete(s_display.motion_dot, NULL);
    }
    s_display = (typeof(s_display)){ 0 };
}

lv_obj_t *app_display_test_create(void)
{
    s_display = (typeof(s_display)){ 0 };

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("Screen test", &content);
    s_display.root = root;
    lv_obj_add_event_cb(root, root_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *modes = lv_obj_create(content);
    lv_obj_set_size(modes, 428, UI_TOUCH_MIN);
    lv_obj_set_pos(modes, 0, 0);
    lv_obj_set_style_bg_opa(modes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(modes, 0, 0);
    lv_obj_set_style_pad_all(modes, 0, 0);
    lv_obj_set_flex_flow(modes, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(modes, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(modes, LV_OBJ_FLAG_SCROLLABLE);
    mode_button(modes, "Colors", DISPLAY_MODE_COLOR);
    mode_button(modes, "Touch", DISPLAY_MODE_TOUCH);
    mode_button(modes, "Motion", DISPLAY_MODE_MOTION);

    s_display.stage = lv_obj_create(content);
    lv_obj_set_size(s_display.stage, 428, 272);
    lv_obj_set_pos(s_display.stage, 0, 68);
    lv_obj_set_style_bg_opa(s_display.stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_display.stage, 0, 0);
    lv_obj_set_style_radius(s_display.stage, 0, 0);
    lv_obj_set_style_pad_all(s_display.stage, 0, 0);
    lv_obj_remove_flag(s_display.stage, LV_OBJ_FLAG_SCROLLABLE);

    show_motion_mode();
    return root;
}
