/*
 * Candis-S31 watch demo - UI manager core: navigation stack, status bar,
 * theme, toast, msgbox, async helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_manager.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"

#include "services/svc_power.h"
#include "fonts/candis_ui_fonts.h"
#include "ui_menu.h"
#include "ui_perf.h"
#include "ui_watchface.h"

#define UI_MAX_APPS 24
#define UI_NAV_DEPTH 8
#define UI_TOAST_MS 2000
#define UI_ANIM_MS 0
#define UI_ASYNC_QUEUE_DEPTH 32
#define UI_ASYNC_DRAIN_BUDGET 16
#define UI_ASYNC_POLL_MS 5
#define UI_NAV_QUEUE_DEPTH 16

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

static StaticQueue_t s_async_queue_state;
static uint8_t s_async_queue_storage[
    UI_ASYNC_QUEUE_DEPTH * sizeof(ui_async_call_t)];
static QueueHandle_t s_async_queue;
static lv_timer_t *s_async_timer;
static lv_timer_t *s_clock_timer;

static StaticQueue_t s_nav_queue_state;
static uint8_t s_nav_queue_storage[
    UI_NAV_QUEUE_DEPTH * sizeof(ui_nav_request_t)];
static QueueHandle_t s_nav_queue;
static portMUX_TYPE s_nav_fallback_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_nav_fallback;

static void status_flush_pending(void);

static void nav_request_apply(ui_nav_request_t request)
{
    switch (request) {
    case UI_NAV_REQUEST_BACK_OR_MENU:
        /* BOOT: back anywhere below the root; menu entry on the watchface. */
        if (ui_nav_at_home()) {
            ui_nav_open_menu();
        } else {
            ui_nav_back();
        }
        break;
    case UI_NAV_REQUEST_HOME:
        ui_nav_home();
        break;
    default:
        break;
    }
}

static void nav_flush_pending(void)
{
    ui_nav_request_t request;
    while (xQueueReceive(s_nav_queue, &request, 0) == pdTRUE) {
        nav_request_apply(request);
    }

    uint32_t fallback;
    portENTER_CRITICAL(&s_nav_fallback_lock);
    fallback = s_nav_fallback;
    s_nav_fallback = 0;
    portEXIT_CRITICAL(&s_nav_fallback_lock);

    if ((fallback & (UINT32_C(1) << UI_NAV_REQUEST_BACK_OR_MENU)) != 0) {
        nav_request_apply(UI_NAV_REQUEST_BACK_OR_MENU);
    }
    /* HOME is deliberately applied last: under pathological key pressure,
     * the safe deterministic outcome is the watchface, not a deeper page. */
    if ((fallback & (UINT32_C(1) << UI_NAV_REQUEST_HOME)) != 0) {
        nav_request_apply(UI_NAV_REQUEST_HOME);
    }
}

static void ui_async_drain(lv_timer_t *timer)
{
    (void)timer;
    nav_flush_pending();
    ui_async_call_t call;
    for (int count = 0; count < UI_ASYNC_DRAIN_BUDGET; ++count) {
        if (xQueueReceive(s_async_queue, &call, 0) != pdTRUE) {
            break;
        }
        call.fn(call.arg);
    }
    /* Status indicators use a fixed last-value mailbox. Poll it from the
     * LVGL timer so queue pressure can delay, but never permanently lose,
     * an SD/USB/wireless state transition. */
    status_flush_pending();
}

bool ui_async(void (*fn)(void *), void *arg)
{
    if (fn == NULL || s_async_queue == NULL || s_async_timer == NULL) {
        return false;
    }
    const ui_async_call_t call = {
        .fn = fn,
        .arg = arg,
    };
    return xQueueSend(s_async_queue, &call, 0) == pdTRUE;
}

bool ui_nav_request(ui_nav_request_t request)
{
    if (s_nav_queue == NULL || s_async_timer == NULL ||
        request < UI_NAV_REQUEST_BACK_OR_MENU ||
        request > UI_NAV_REQUEST_HOME) {
        return false;
    }
    if (xQueueSend(s_nav_queue, &request, 0) == pdTRUE) {
        return true;
    }

    portENTER_CRITICAL(&s_nav_fallback_lock);
    s_nav_fallback |= UINT32_C(1) << request;
    portEXIT_CRITICAL(&s_nav_fallback_lock);
    return true;
}

/* ------------------------------------------------------------------ */
/* Theme helpers                                                       */
/* ------------------------------------------------------------------ */

const lv_font_t *ui_font_text(void)
{
    /* Compact 460x460 density hierarchy (DESIGN.md typography):
     * text/subtitles/rows = 16 (CJK default font), body/buttons = 24
     * Montserrat, large body = project CJK 20, titles = project CJK 24,
     * mid/big Montserrat 32/48 for symbols and watchface digits. */
    /* Must stay the full CJK default (Source Han Sans SC 16):
     * screen-base text and the msgbox body carry arbitrary user strings,
     * e.g. Chinese filenames - a latin-only font renders tofu. Symbols
     * (FontAwesome) are set to Montserrat explicitly where needed. */
    return LV_FONT_DEFAULT;
}

const lv_font_t *ui_font_body(void)
{
    return &lv_font_montserrat_24;
}

const lv_font_t *ui_font_body_lg(void)
{
    /* Project Source Han Sans SC subset, 20 px; falls back to the bundled
     * CJK 16 for glyphs outside the subset. */
    return &candis_ui_20;
}

const lv_font_t *ui_font_title(void)
{
    /* Project Source Han Sans SC subset, 24 px; same CJK 16 fallback. */
    return &candis_ui_24;
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
    s_styles_ready = true;
}

static void style_screen(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_style_text_font(obj, ui_font_text(), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/* Thin accent scrollbar used by scaffolded content areas. */
static void style_dark_scrollbar(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COL_ACCENT),
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
    /* Symbols need a font with the FontAwesome glyph range; the compact
     * default (Montserrat16) covers it, the tiny default may not. */
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT_DIM), 0);
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
    lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_LEFT_MID, 12, 0);

    s_lbl_wifi = status_label(s_status_bar, LV_SYMBOL_WIFI);
    s_lbl_ble = status_label(s_status_bar, LV_SYMBOL_BLUETOOTH);
    s_lbl_usb = status_label(s_status_bar, LV_SYMBOL_USB);
    s_lbl_sd = status_label(s_status_bar, LV_SYMBOL_SD_CARD);
    s_lbl_batt = status_label(s_status_bar, "--%");

    /* The battery slot must fit the worst case "CHARGE 100%" (~58 px);
     * the icons keep their 38 px pitch and sit 6 px further left so the
     * charging label no longer overlaps the SD glyph at 100%. */
    lv_obj_align(s_lbl_batt, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_align(s_lbl_sd, LV_ALIGN_RIGHT_MID, -68, 0);
    lv_obj_align(s_lbl_usb, LV_ALIGN_RIGHT_MID, -106, 0);
    lv_obj_align(s_lbl_ble, LV_ALIGN_RIGHT_MID, -144, 0);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_RIGHT_MID, -182, 0);
}

/* Last applied status-bar time: identical repeats are dropped before
 * touching LVGL (same full-frame-redraw rationale as the battery state
 * below). Pre-seeded with the normalized unknown post (0xFFFF, -1) so a
 * boot-time RTC failure does not redraw the "--:--" placeholder. */
static int s_time_last_hour = 0xFFFF;
static int s_time_last_minute = -1;

/* Last applied battery state. The PMIC polls every 2 s, so identical
 * repeats are dropped before touching LVGL: under FULL render mode any
 * label write merges into a full-frame redraw, and the old blinking
 * charge animation was both a 0.7 s infinite-invalidate loop and got
 * rebuilt on every poll (losing its phase). Charging is shown as a
 * static charge glyph + green text instead, which stays readable
 * without motion (DESIGN.md: charging must be understandable without
 * blinking). */
static int s_batt_last_percent = INT_MIN;
static bool s_batt_last_charging;
static bool s_batt_last_present;

typedef struct {
    int a;
    int b;
    int c;
} ui_status_msg_t;

#define UI_STATUS_KIND_COUNT 6

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static ui_status_msg_t s_status_pending[UI_STATUS_KIND_COUNT];
static uint32_t s_status_dirty;

static void status_apply_value(const ui_status_msg_t *msg)
{
    int what = msg->a >> 16;
    int v0 = msg->a & 0xFFFF, v1 = msg->b, v2 = msg->c;
    switch (what) {
    case 0: { /* time */
        if (v0 == s_time_last_hour && v1 == s_time_last_minute) {
            break; /* unchanged since last apply: no LVGL write, no redraw */
        }
        s_time_last_hour = v0;
        s_time_last_minute = v1;
        if (v0 == 0xFFFF) {
            lv_label_set_text(s_lbl_time, "--:--");
        } else {
            lv_label_set_text_fmt(s_lbl_time, "%02d:%02d", v0, v1);
        }
        break;
    }
    case 1: /* battery: v0=percent(-1 = absent or no valid model) v1=charging v2=present */
        if (v0 == s_batt_last_percent && v1 == s_batt_last_charging &&
            v2 == s_batt_last_present) {
            break; /* unchanged since last apply: no LVGL write, no redraw */
        }
        s_batt_last_percent = v0;
        s_batt_last_charging = v1;
        s_batt_last_present = v2;
        if (!v2 || v0 < 0) {
            lv_label_set_text(s_lbl_batt, "--");
        } else {
            lv_label_set_text_fmt(s_lbl_batt, "%s%d%%",
                                  v1 ? LV_SYMBOL_CHARGE " " : "", v0);
        }
        lv_obj_set_style_text_color(s_lbl_batt,
                                    lv_color_hex(v1 ? UI_COL_PASS : UI_COL_TEXT), 0);
        lv_obj_set_style_opa(s_lbl_batt, LV_OPA_COVER, 0);
        break;
    case 2: /* wifi */
        lv_obj_set_style_text_color(s_lbl_wifi,
                                    lv_color_hex(v0 == 2 ? UI_COL_ACCENT :
                                                 v0 == 1 ? UI_COL_WARN : UI_COL_TEXT_DIM), 0);
        break;
    case 3: /* ble */
        lv_obj_set_style_text_color(s_lbl_ble,
                                    lv_color_hex(v0 ? UI_COL_ACCENT : UI_COL_TEXT_DIM), 0);
        break;
    case 4: /* sd */
        lv_obj_set_style_text_color(s_lbl_sd,
                                    lv_color_hex(v0 ? UI_COL_TEXT : UI_COL_TEXT_DIM), 0);
        break;
    case 5: /* usb */
        lv_obj_set_style_text_color(s_lbl_usb,
                                    lv_color_hex(v0 ? UI_COL_TEXT : UI_COL_TEXT_DIM), 0);
        break;
    default:
        break;
    }
}

static void status_flush_pending(void)
{
    ui_status_msg_t snapshot[UI_STATUS_KIND_COUNT];
    uint32_t dirty = 0;

    portENTER_CRITICAL(&s_status_lock);
    dirty = s_status_dirty;
    s_status_dirty = 0;
    for (int what = 0; what < UI_STATUS_KIND_COUNT; ++what) {
        if ((dirty & (UINT32_C(1) << what)) != 0) {
            snapshot[what] = s_status_pending[what];
        }
    }
    portEXIT_CRITICAL(&s_status_lock);

    for (int what = 0; what < UI_STATUS_KIND_COUNT; ++what) {
        if ((dirty & (UINT32_C(1) << what)) != 0) {
            status_apply_value(&snapshot[what]);
        }
    }
}

static void status_post(int what, int v0, int v1, int v2)
{
    if (what < 0 || what >= UI_STATUS_KIND_COUNT) {
        return;
    }
    const ui_status_msg_t msg = {
        .a = (what << 16) | (v0 & 0xFFFF),
        .b = v1,
        .c = v2,
    };
    portENTER_CRITICAL(&s_status_lock);
    s_status_pending[what] = msg;
    s_status_dirty |= UINT32_C(1) << what;
    portEXIT_CRITICAL(&s_status_lock);
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
/* Clock: single 1 s source for the status bar and the watchface       */
/* ------------------------------------------------------------------ */

/* Runs on the LVGL thread. bsp_rtc_get_time() is a short I2C read;
 * demo_board and app_settings already issue it from LVGL context. */
static void ui_clock_tick(lv_timer_t *timer)
{
    (void)timer;

    /* Screen off: invalidation is disabled and nothing is visible, so
     * skip the I2C read instead of polling once per second. */
    if (svc_power_is_screen_off()) {
        return;
    }

    bsp_rtc_time_t t;
    bsp_rtc_status_t status;
    const bool ok = (bsp_rtc_get_time(&t, &status) == ESP_OK) &&
                    status.time_valid;

    /* Status-bar time; value dedup lives in status_apply_value. A failed
     * or invalid read reports unknown so a broken RTC shows "--:--",
     * never stale digits. */
    if (ok) {
        ui_status_set_time(t.hour, t.minute);
    } else {
        ui_status_set_time(-1, -1);
    }

    /* Watchface widgets; the tick itself gates on the active screen, and
     * a transient RTC failure keeps its last good rendering. */
    ui_watchface_tick(ok ? &t : NULL);
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
    /* Full-screen fades repeatedly invalidate the entire 460x460 panel. The
     * CO5300 QSPI transfer is longer than one scan period, so transitions must
     * be instantaneous; local widget animations remain tear-resistant under
     * the TE-paced partial pipeline. */
    ui_perf_load_begin(app ? app->id : NULL);
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, UI_ANIM_MS, 0, false);
    nav_apply_status_visibility();
}

void ui_nav_back(void)
{
    if (s_top <= 0) {
        return;
    }
    /* The persistent launcher menu (stack layer 1) survives its own pop;
     * every app screen above it is deleted by auto_del. */
    const bool popped_persistent = (s_top == 1);
    --s_top;
    /* auto_del deletes the screen being replaced (the one we just popped). */
    ui_perf_load_begin("back");
    lv_scr_load_anim(s_stack[s_top], LV_SCR_LOAD_ANIM_NONE, UI_ANIM_MS, 0,
                     !popped_persistent);
    nav_apply_status_visibility();
}

void ui_nav_push_screen(lv_obj_t *screen)
{
    nav_push(screen, NULL);
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
    if (!screen) {
        return;
    }
    if (s_graveyard_count >= UI_NAV_DEPTH) {
        /* Graveyard full: delete the screen now instead of leaking it.
         * The deferred reap is only a cosmetic grace period. */
        ESP_LOGW("ui_manager", "graveyard overflow, deleting screen now");
        lv_obj_delete(screen);
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
    /* App screens (layer >= 2) go to the deferred graveyard; the active
     * top screen is handed to LVGL via auto_del (deleted after the fade)
     * when it is itself an app screen. Layer 1 is the persistent launcher
     * menu: it stays alive in s_stack[1] for the next open. */
    for (int i = 2; i < s_top; ++i) {
        graveyard_add(s_stack[i]);
        s_stack[i] = NULL;
    }
    const bool top_is_app = (s_top >= 2);
    s_top = 0;
    ui_perf_load_begin("home");
    lv_scr_load_anim(s_stack[0], LV_SCR_LOAD_ANIM_NONE, UI_ANIM_MS, 0,
                     top_is_app);
    nav_apply_status_visibility();
}

void ui_nav_open(const char *app_id)
{
    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i]->id, app_id) == 0) {
            /* Apps always sit above the persistent menu: a push from the
             * root would otherwise overwrite s_stack[1] (the launcher)
             * and leak the object tree. */
            if (s_top == 0) {
                ui_nav_open_menu();
            }
            ui_perf_create_begin(s_apps[i]->id);
            lv_obj_t *screen = s_apps[i]->create();
            ui_perf_create_end();
            nav_push(screen, s_apps[i]);
            return;
        }
    }
}

void ui_nav_open_menu(void)
{
    /* The launcher lives persistently at stack layer 1 and opens only
     * from the watchface root; deeper pages reach it through back. */
    if (s_top != 0 || s_stack[1] == NULL) {
        return;
    }
    s_top = 1;
    lv_scr_load_anim(s_stack[1], LV_SCR_LOAD_ANIM_NONE, UI_ANIM_MS, 0, false);
    nav_apply_status_visibility();
}

bool ui_nav_at_home(void)
{
    return s_top == 0;
}

const char *ui_nav_current_id(void)
{
    for (int i = s_top; i >= 0; --i) {
        if (s_stack_app[i] != NULL) {
            return s_stack_app[i]->id;
        }
    }
    return s_top == 1 ? "menu" : "watchface";
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
    lv_obj_set_size(back, UI_TOUCH_MIN, UI_TOUCH_MIN);
    lv_obj_set_pos(back, 8, UI_CONTENT_Y + 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(UI_COL_HAIRLINE), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(UI_COL_ACCENT),
                                  LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, scaffold_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, ui_font_mid(), 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *tick = lv_obj_create(root);
    lv_obj_set_size(tick, 5, 28);
    lv_obj_set_pos(tick, 72, UI_CONTENT_Y + 18);
    lv_obj_set_style_bg_color(tick, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tick, 2, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title_lbl = lv_label_create(root);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, ui_font_title(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(UI_COL_TEXT), 0);
    /* 24 px project CJK title: line height 28 px, vertically centered in
     * the fixed 64 px title row. */
    lv_obj_set_pos(title_lbl, 88, UI_CONTENT_Y + 18);

    lv_obj_t *content = lv_obj_create(root);
    lv_obj_set_size(content, 460, 460 - UI_CONTENT_Y - UI_TITLE_ROW_HEIGHT);
    lv_obj_set_pos(content, 0, UI_CONTENT_Y + UI_TITLE_ROW_HEIGHT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_left(content, UI_SCREEN_PAD, 0);
    lv_obj_set_style_pad_right(content, UI_SCREEN_PAD, 0);
    lv_obj_set_style_pad_top(content, 8, 0);
    lv_obj_set_style_pad_bottom(content, UI_SCREEN_PAD, 0);
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
    lv_obj_set_style_bg_color(toast, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast, 18, 0);
    lv_obj_set_style_border_color(toast, lv_color_hex(UI_COL_HAIRLINE), 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_set_style_pad_left(toast, 18, 0);
    lv_obj_set_style_pad_right(toast, 18, 0);
    lv_obj_set_style_pad_top(toast, 10, 0);
    lv_obj_set_style_pad_bottom(toast, 10, 0);

    lv_obj_t *label = lv_label_create(toast);
    lv_label_set_text(label, text); /* lv_label copies the text */
    lv_obj_set_style_text_font(label, ui_font_body(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT), 0);
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

/* B.4 button language: primary = ACCENT_DIM fill + ACCENT 1 px edge,
 * secondary = SURFACE + HAIRLINE. Press feedback is a color swap only. */
static lv_obj_t *msgbox_button_create(lv_obj_t *parent, const char *text,
                                      bool primary)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 136, UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(primary ? UI_COL_ACCENT_DIM
                                                        : UI_COL_SURFACE),
                              0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(primary ? UI_COL_ACCENT
                                                            : UI_COL_HAIRLINE),
                                  0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_SURFACE_2),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COL_ACCENT),
                                  LV_STATE_PRESSED);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_body(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT), 0);
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
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 20, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COL_HAIRLINE), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_shadow_color(box, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_shadow_width(box, 28, 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(box);
    lv_label_set_text(title_lbl, title);
    /* Title at 24px (was 32 - overflowed the 330px box for long words)
     * with a bounded slot + dot mode. */
    lv_obj_set_style_text_font(title_lbl, ui_font_body(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_width(title_lbl, 290);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *text_lbl = lv_label_create(box);
    lv_label_set_text(text_lbl, text);
    /* Dense 16px wrapped body with a fixed-height scrollable slot: long
     * filenames scroll instead of overflowing the box. */
    lv_obj_set_style_text_font(text_lbl, ui_font_text(), 0);
    lv_obj_set_width(text_lbl, 284);
    lv_obj_set_height(text_lbl, 100);
    lv_label_set_long_mode(text_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(text_lbl, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(text_lbl, LV_ALIGN_CENTER, 0, -16);

    ui_msgbox_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (!ctx) {
        lv_obj_delete(modal);
        return;
    }
    ctx->cb = cb;
    ctx->user = user;
    ctx->modal = modal;

    lv_obj_t *cancel = msgbox_button_create(box, "Cancel", false);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 18, -14);
    lv_obj_set_user_data(cancel, NULL);
    lv_obj_add_event_cb(cancel, msgbox_click_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *ok = msgbox_button_create(box, "OK", true);
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
    if (s_async_queue == NULL) {
        s_async_queue = xQueueCreateStatic(
            UI_ASYNC_QUEUE_DEPTH, sizeof(ui_async_call_t),
            s_async_queue_storage, &s_async_queue_state);
    }
    if (s_nav_queue == NULL) {
        s_nav_queue = xQueueCreateStatic(
            UI_NAV_QUEUE_DEPTH, sizeof(ui_nav_request_t),
            s_nav_queue_storage, &s_nav_queue_state);
    }
    if (!ui_lock()) {
        return;
    }
    if (s_async_queue != NULL && s_async_timer == NULL) {
        s_async_timer = lv_timer_create(ui_async_drain, UI_ASYNC_POLL_MS,
                                        NULL);
    }
    ui_styles_ensure();
    status_bar_build();

    /* Navigation root is the watchface; the launcher menu is pre-created
     * at persistent layer 1 and stays alive for the whole run. */
    s_top = 0;
    s_stack[0] = ui_watchface_create();
    s_stack_app[0] = NULL;
    s_stack[1] = ui_menu_create();
    s_stack_app[1] = NULL;
    ui_perf_attach(lv_display_get_default());
    lv_scr_load_anim(s_stack[0], LV_SCR_LOAD_ANIM_FADE_IN, UI_ANIM_MS, 0, false);

    if (s_clock_timer == NULL) {
        s_clock_timer = lv_timer_create(ui_clock_tick, 1000, NULL);
    }
    /* Populate time/battery immediately instead of waiting one tick. */
    ui_clock_tick(NULL);

    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev) {
        /* NOTE: with the current esp_lvgl_adapter this call is a no-op
         * and is kept only for intent. The adapter already gates the
         * touch reads: the CST820 INT ISR merely gives a semaphore
         * (esp_lv_adapter_input_touch.c), and the read callback performs
         * the I2C transfer only when that semaphore is set - I2C traffic
         * therefore already happens strictly after real interrupts, not
         * on every refresh tick. The 2026-08-21 taskLVGL CPU storm was
         * caused by the scale press transitions (removed that day), not
         * by the indev mode. Do not remove the call without
         * re-verifying the adapter's read path. */
        lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);
        lv_indev_add_event_cb(indev, indev_activity_cb, LV_EVENT_PRESSED, NULL);
        /* PRESSED fires once per touch; PRESSING fires on every indev
         * read while held, so a sustained press keeps resetting the
         * idle-screen timer instead of only its first frame. */
        lv_indev_add_event_cb(indev, indev_activity_cb, LV_EVENT_PRESSING, NULL);
    }
    ui_unlock();
}
