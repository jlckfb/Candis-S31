/*
 * Candis-S31 watch demo - UI manager core: navigation stack, status bar,
 * theme, toast, msgbox, async helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_manager.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"

#include "services/svc_power.h"
#include "ui_menu.h"
#include "ui_watchface.h"

#define UI_MAX_APPS 24
#define UI_NAV_DEPTH 8
#define UI_TOAST_MS 2000
#define UI_ANIM_MS 150

static const ui_app_t *s_apps[UI_MAX_APPS];
static int s_app_count;

static lv_obj_t *s_stack[UI_NAV_DEPTH];
static const ui_app_t *s_stack_app[UI_NAV_DEPTH]; /* NULL = shell screen */
static int s_top;

static lv_obj_t *s_status_bar;
static lv_obj_t *s_lbl_time;
static lv_obj_t *s_lbl_batt;
static lv_obj_t *s_lbl_wifi;
static lv_obj_t *s_lbl_ble;
static lv_obj_t *s_lbl_sd;
static lv_obj_t *s_lbl_usb;

/* Shared pressed-state transition (scale + colors). */
static const lv_style_prop_t s_press_trans_props[] = {
    LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_BORDER_COLOR, LV_STYLE_BG_COLOR,
    LV_STYLE_PROP_INV,
};
static lv_style_transition_dsc_t s_press_trans;
static bool s_styles_ready;

/* ------------------------------------------------------------------ */
/* Locking / async                                                     */
/* ------------------------------------------------------------------ */

bool ui_lock(void)
{
    return bsp_display_lock(1000);
}

void ui_unlock(void)
{
    bsp_display_unlock();
}

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} ui_async_call_t;

static void ui_async_trampoline(void *ptr)
{
    ui_async_call_t *call = ptr;
    call->fn(call->arg);
    lv_free(call);
}

void ui_async(void (*fn)(void *), void *arg)
{
    ui_async_call_t *call = lv_malloc(sizeof(*call));
    if (!call) {
        return;
    }
    call->fn = fn;
    call->arg = arg;
    if (ui_lock()) {
        if (lv_async_call(ui_async_trampoline, call) != LV_RESULT_OK) {
            lv_free(call);
        }
        ui_unlock();
    } else {
        lv_free(call);
    }
}

/* ------------------------------------------------------------------ */
/* Theme helpers                                                       */
/* ------------------------------------------------------------------ */

const lv_font_t *ui_font_text(void)
{
    return LV_FONT_DEFAULT;
}

const lv_font_t *ui_font_mid(void)
{
    return &lv_font_montserrat_32;
}

const lv_font_t *ui_font_big(void)
{
    return &lv_font_montserrat_48;
}

static void ui_styles_ensure(void)
{
    if (s_styles_ready) {
        return;
    }
    lv_style_transition_dsc_init(&s_press_trans, s_press_trans_props,
                                 lv_anim_path_ease_out, 140, 0, NULL);
    s_styles_ready = true;
}

static void style_screen(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(obj, ui_font_text(), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/* Thin accent scrollbar used by scaffolded content areas. */
static void style_dark_scrollbar(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(obj, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(obj, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(obj, 3, LV_PART_SCROLLBAR);
}

/* ------------------------------------------------------------------ */
/* Status bar                                                          */
/* ------------------------------------------------------------------ */

static lv_obj_t *status_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    return label;
}

static void status_bar_build(void)
{
    s_status_bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_status_bar, 460, UI_STATUS_BAR_HEIGHT);
    lv_obj_set_pos(s_status_bar, 0, 0);
    lv_obj_set_style_bg_color(s_status_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    lv_obj_set_style_radius(s_status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_status_bar, 0, 0);
    lv_obj_remove_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(s_status_bar, LV_OBJ_FLAG_CLICKABLE);

    s_lbl_time = status_label(s_status_bar, "--:--");
    lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_LEFT_MID, 12, 0);

    s_lbl_wifi = status_label(s_status_bar, LV_SYMBOL_WIFI);
    s_lbl_ble = status_label(s_status_bar, LV_SYMBOL_BLUETOOTH);
    s_lbl_usb = status_label(s_status_bar, LV_SYMBOL_USB);
    s_lbl_sd = status_label(s_status_bar, LV_SYMBOL_SD_CARD);
    s_lbl_batt = status_label(s_status_bar, "--%");

    lv_obj_align(s_lbl_batt, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_align(s_lbl_sd, LV_ALIGN_RIGHT_MID, -62, 0);
    lv_obj_align(s_lbl_usb, LV_ALIGN_RIGHT_MID, -100, 0);
    lv_obj_align(s_lbl_ble, LV_ALIGN_RIGHT_MID, -138, 0);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_RIGHT_MID, -176, 0);
}

/* Charging indicator: blink the battery label (small dirty area). */
static void charge_blink_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void charge_blink_start(void)
{
    if (!s_lbl_batt) {
        return;
    }
    lv_anim_delete(s_lbl_batt, charge_blink_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lbl_batt);
    lv_anim_set_exec_cb(&a, charge_blink_cb);
    lv_anim_set_values(&a, 255, 70);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_reverse_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void charge_blink_stop(void)
{
    if (!s_lbl_batt) {
        return;
    }
    lv_anim_delete(s_lbl_batt, charge_blink_cb);
    lv_obj_set_style_opa(s_lbl_batt, LV_OPA_COVER, 0);
}

typedef struct {
    int a;
    int b;
    int c;
} ui_status_msg_t;

static void status_apply(void *ptr)
{
    ui_status_msg_t *msg = ptr;
    int what = msg->a >> 16;
    int v0 = msg->a & 0xFFFF, v1 = msg->b, v2 = msg->c;
    switch (what) {
    case 0: { /* time */
        if (v0 == 0xFFFF) {
            lv_label_set_text(s_lbl_time, "--:--");
        } else {
            lv_label_set_text_fmt(s_lbl_time, "%02d:%02d", v0, v1);
        }
        break;
    }
    case 1: /* battery: v0=percent v1=charging v2=present */
        if (!v2) {
            lv_label_set_text(s_lbl_batt, "--");
        } else {
            lv_label_set_text_fmt(s_lbl_batt, "%s%d%%",
                                  v1 ? LV_SYMBOL_CHARGE " " : "", v0);
        }
        lv_obj_set_style_text_color(s_lbl_batt,
                                    lv_color_hex(v1 ? UI_COLOR_OK : UI_COLOR_TEXT), 0);
        if (v1) {
            charge_blink_start();
        } else {
            charge_blink_stop();
        }
        break;
    case 2: /* wifi */
        lv_obj_set_style_text_color(s_lbl_wifi,
                                    lv_color_hex(v0 == 2 ? UI_COLOR_ACCENT :
                                                 v0 == 1 ? UI_COLOR_WARN : UI_COLOR_TEXT_DIM), 0);
        break;
    case 3: /* ble */
        lv_obj_set_style_text_color(s_lbl_ble,
                                    lv_color_hex(v0 ? UI_COLOR_ACCENT : UI_COLOR_TEXT_DIM), 0);
        break;
    case 4: /* sd */
        lv_obj_set_style_text_color(s_lbl_sd,
                                    lv_color_hex(v0 ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM), 0);
        break;
    case 5: /* usb */
        lv_obj_set_style_text_color(s_lbl_usb,
                                    lv_color_hex(v0 ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM), 0);
        break;
    default:
        break;
    }
    lv_free(msg);
}

static void status_post(int what, int v0, int v1, int v2)
{
    ui_status_msg_t *msg = lv_malloc(sizeof(*msg));
    if (!msg) {
        return;
    }
    msg->a = (what << 16) | (v0 & 0xFFFF);
    msg->b = v1;
    msg->c = v2;
    ui_async(status_apply, msg);
}

void ui_status_set_time(int hour, int minute)
{
    status_post(0, hour < 0 ? 0xFFFF : hour, minute, 0);
}

void ui_status_set_battery(int percent, bool charging, bool present)
{
    status_post(1, percent, charging, present);
}

void ui_status_set_wifi(int state)
{
    status_post(2, state, 0, 0);
}

void ui_status_set_ble(bool active)
{
    status_post(3, active, 0, 0);
}

void ui_status_set_sd(bool mounted)
{
    status_post(4, mounted, 0, 0);
}

void ui_status_set_usb(int role)
{
    status_post(5, role, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

static void nav_apply_status_visibility(void)
{
    const ui_app_t *app = s_stack_app[s_top];
    if (!s_status_bar) {
        return;
    }
    if (app && app->hide_status_bar) {
        lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void nav_push(lv_obj_t *screen, const ui_app_t *app)
{
    if (!screen) {
        return;
    }
    if (s_top >= UI_NAV_DEPTH - 1) {
        /* A freshly created, never-loaded screen is safe to delete here;
         * silently dropping it would leak the whole object tree. */
        lv_obj_delete(screen);
        return;
    }
    ++s_top;
    s_stack[s_top] = screen;
    s_stack_app[s_top] = app;
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_IN, UI_ANIM_MS, 0, false);
    nav_apply_status_visibility();
}

void ui_nav_back(void)
{
    if (s_top <= 0) {
        return;
    }
    --s_top;
    /* auto_del deletes the screen being replaced (the one we just popped). */
    lv_scr_load_anim(s_stack[s_top], LV_SCR_LOAD_ANIM_FADE_OUT, UI_ANIM_MS, 0, true);
    nav_apply_status_visibility();
}

/* Screens popped without an LVGL transition (ui_nav_home) must not be
 * deleted immediately: during a load animation the display still references
 * the outgoing screen, so a direct delete leaves act_scr/scr_to_load
 * dangling. They are reaped by a one-shot timer that fires after any
 * in-flight transition (UI_ANIM_MS) has ended. */
#define UI_GRAVEYARD_DELAY_MS (UI_ANIM_MS * 2 + 50)

static lv_obj_t *s_graveyard[UI_NAV_DEPTH];
static int s_graveyard_count;
static lv_timer_t *s_graveyard_timer;

static void graveyard_reap(lv_timer_t *timer)
{
    (void)timer;
    for (int i = 0; i < s_graveyard_count; ++i) {
        lv_obj_delete(s_graveyard[i]);
    }
    s_graveyard_count = 0;
    s_graveyard_timer = NULL;
}

static void graveyard_add(lv_obj_t *screen)
{
    if (!screen || s_graveyard_count >= UI_NAV_DEPTH) {
        return;
    }
    s_graveyard[s_graveyard_count++] = screen;
    if (s_graveyard_timer) {
        lv_timer_delete(s_graveyard_timer);
    }
    s_graveyard_timer = lv_timer_create(graveyard_reap, UI_GRAVEYARD_DELAY_MS,
                                        NULL);
    lv_timer_set_repeat_count(s_graveyard_timer, 1);
}

void ui_nav_home(void)
{
    if (s_top <= 0) {
        return;
    }
    /* Intermediate screens go to the deferred graveyard; the active top
     * screen is handed to LVGL via auto_del (deleted after the fade). */
    for (int i = 1; i < s_top; ++i) {
        graveyard_add(s_stack[i]);
        s_stack[i] = NULL;
    }
    s_top = 0;
    lv_scr_load_anim(s_stack[0], LV_SCR_LOAD_ANIM_FADE_IN, UI_ANIM_MS, 0, true);
    nav_apply_status_visibility();
}

void ui_nav_open_menu(void)
{
    nav_push(ui_menu_create(), NULL);
}

void ui_nav_open(const char *app_id)
{
    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i]->id, app_id) == 0) {
            nav_push(s_apps[i]->create(), s_apps[i]);
            return;
        }
    }
}

bool ui_nav_at_home(void)
{
    return s_top == 0;
}

void ui_app_register(const ui_app_t *app)
{
    if (app && s_app_count < UI_MAX_APPS) {
        s_apps[s_app_count++] = app;
    }
}

int ui_app_count(void)
{
    return s_app_count;
}

const ui_app_t *ui_app_at(int index)
{
    return (index >= 0 && index < s_app_count) ? s_apps[index] : NULL;
}

/* ------------------------------------------------------------------ */
/* App scaffold                                                        */
/* ------------------------------------------------------------------ */

static void scaffold_back_cb(lv_event_t *event)
{
    (void)event;
    ui_nav_back();
}

lv_obj_t *ui_app_scaffold(const char *title, lv_obj_t **content_out)
{
    ui_styles_ensure();

    lv_obj_t *root = lv_obj_create(NULL);
    style_screen(root);

    /* Watch-style title row: rounded back key, accent tick, title. */
    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, 44, 40);
    lv_obj_set_pos(back, 10, UI_CONTENT_Y + 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x26262E), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_transform_scale(back, LV_SCALE_NONE, 0);
    lv_obj_set_style_transition(back, &s_press_trans, 0);
    lv_obj_set_style_transform_scale(back, 238, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(back, lv_color_hex(UI_COLOR_ACCENT),
                                  LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, scaffold_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *tick = lv_obj_create(root);
    lv_obj_set_size(tick, 4, 22);
    lv_obj_set_pos(tick, 62, UI_CONTENT_Y + 13);
    lv_obj_set_style_bg_color(tick, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tick, 2, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title_lbl = lv_label_create(root);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, ui_font_text(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(title_lbl, 74, UI_CONTENT_Y + 14);

    lv_obj_t *content = lv_obj_create(root);
    lv_obj_set_size(content, 460, 460 - UI_CONTENT_Y - 48);
    lv_obj_set_pos(content, 0, UI_CONTENT_Y + 48);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    style_dark_scrollbar(content);

    if (content_out) {
        *content_out = content;
    }
    return root;
}

/* ------------------------------------------------------------------ */
/* Toast: fades in while floating up, then sinks and fades out.        */
/* ------------------------------------------------------------------ */

static void anim_set_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

static void anim_set_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void toast_delete_on_ready(lv_anim_t *a)
{
    lv_obj_delete(a->var);
}

static void toast_expire_cb(lv_timer_t *timer)
{
    lv_obj_t *toast = lv_timer_get_user_data(timer);
    lv_anim_delete(toast, NULL);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, toast);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, toast_delete_on_ready);

    lv_anim_set_exec_cb(&a, anim_set_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(toast), lv_obj_get_y(toast) + 30);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_set_values(&a, lv_obj_get_style_opa(toast, 0), 0);
    lv_anim_start(&a);
}

void ui_toast(const char *text)
{
    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(toast, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast, 18, 0);
    lv_obj_set_style_border_color(toast, lv_color_hex(0x2C2C34), 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_set_style_pad_left(toast, 18, 0);
    lv_obj_set_style_pad_right(toast, 18, 0);
    lv_obj_set_style_pad_top(toast, 10, 0);
    lv_obj_set_style_pad_bottom(toast, 10, 0);

    lv_obj_t *label = lv_label_create(toast);
    lv_label_set_text(label, text); /* lv_label copies the text */
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(label);

    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -60);
    int32_t final_y = lv_obj_get_y(toast);

    /* Entrance: fade in while floating up. */
    lv_obj_set_y(toast, final_y + 26);
    lv_obj_set_style_opa(toast, LV_OPA_TRANSP, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, toast);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);

    lv_anim_set_exec_cb(&a, anim_set_y_cb);
    lv_anim_set_values(&a, final_y + 26, final_y);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_start(&a);

    lv_timer_t *timer = lv_timer_create(toast_expire_cb, UI_TOAST_MS, toast);
    lv_timer_set_repeat_count(timer, 1);
}

/* ------------------------------------------------------------------ */
/* Msgbox                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    void (*cb)(bool ok, void *user);
    void *user;
    lv_obj_t *modal;
} ui_msgbox_ctx_t;

static void msgbox_click_cb(lv_event_t *event)
{
    ui_msgbox_ctx_t *ctx = lv_event_get_user_data(event);
    bool ok = lv_obj_get_user_data(lv_event_get_target_obj(event)) != NULL;
    lv_obj_t *modal = ctx->modal;
    void (*cb)(bool, void *) = ctx->cb;
    void *user = ctx->user;
    lv_free(ctx);
    lv_obj_delete(modal);
    if (cb) {
        cb(ok, user);
    }
}

static lv_obj_t *msgbox_button_create(lv_obj_t *parent, const char *text,
                                      uint32_t bg_color)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 136, 46);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_transform_scale(btn, LV_SCALE_NONE, 0);
    lv_obj_set_style_transition(btn, &s_press_trans, 0);
    lv_obj_set_style_transform_scale(btn, 244, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color == UI_COLOR_ACCENT
                                                    ? 0x3A7FD0 : 0x2C2C34),
                              LV_STATE_PRESSED);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(label);
    return btn;
}

void ui_msgbox(const char *title, const char *text,
               void (*cb)(bool ok, void *user), void *user)
{
    ui_styles_ensure();

    /* Dim overlay below the status bar so the bar stays visible. */
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, 460, 460 - UI_STATUS_BAR_HEIGHT);
    lv_obj_set_pos(modal, 0, UI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(modal);
    lv_obj_set_size(box, 330, 210);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 20, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x2E2E36), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_shadow_color(box, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(box, 28, 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(box);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, ui_font_text(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *text_lbl = lv_label_create(box);
    lv_label_set_text(text_lbl, text);
    lv_obj_set_width(text_lbl, 284);
    lv_obj_set_style_text_color(text_lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(text_lbl, LV_ALIGN_CENTER, 0, -16);

    ui_msgbox_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (!ctx) {
        lv_obj_delete(modal);
        return;
    }
    ctx->cb = cb;
    ctx->user = user;
    ctx->modal = modal;

    lv_obj_t *cancel = msgbox_button_create(box, "取消", 0x232329);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 18, -14);
    lv_obj_set_user_data(cancel, NULL);
    lv_obj_add_event_cb(cancel, msgbox_click_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *ok = msgbox_button_create(box, "确定", UI_COLOR_ACCENT);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, -18, -14);
    lv_obj_set_user_data(ok, (void *)1); /* non-NULL = OK */
    lv_obj_add_event_cb(ok, msgbox_click_cb, LV_EVENT_CLICKED, ctx);
}

/* ------------------------------------------------------------------ */
/* Activity ping                                                       */
/* ------------------------------------------------------------------ */

static void indev_activity_cb(lv_event_t *event)
{
    (void)event;
    svc_power_activity();
}

void ui_activity_ping(void)
{
    svc_power_activity();
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void ui_manager_init(void)
{
    if (!ui_lock()) {
        return;
    }
    ui_styles_ensure();
    status_bar_build();

    s_top = 0;
    s_stack[0] = ui_watchface_create();
    s_stack_app[0] = NULL;
    lv_scr_load_anim(s_stack[0], LV_SCR_LOAD_ANIM_FADE_IN, UI_ANIM_MS, 0, false);

    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev) {
        lv_indev_add_event_cb(indev, indev_activity_cb, LV_EVENT_PRESSED, NULL);
    }
    ui_unlock();
}
