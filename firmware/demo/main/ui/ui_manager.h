/*
 * Candis-S31 watch demo - UI manager: navigation, status bar, theme.
 *
 * Threading contract:
 *  - ui_lock()/ui_unlock() wrap bsp_display_lock(); every direct LVGL call
 *    must hold the lock.
 *  - ui_async() schedules fn on the LVGL thread (lock + lv_async_call) and is
 *    the ONLY way service tasks may touch UI state indirectly. Service
 *    callbacks run on service task context; app code inside such callbacks
 *    must forward UI work through ui_async().
 *
 * Screen model: each app create() returns a full-screen container (not yet
 * loaded). The manager fades it in. ui_nav_back() deletes the current screen
 * and restores the previous one. Watchface and menu stay alive for the whole
 * run; app screens are created on open and deleted on exit.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Theme palette (AMOLED dark). */
#define UI_COLOR_BG        0x000000
#define UI_COLOR_SURFACE   0x1A1A1E
#define UI_COLOR_ACCENT    0x4A9EFF
#define UI_COLOR_TEXT      0xF0F0F0
#define UI_COLOR_TEXT_DIM  0x8A8A92
#define UI_COLOR_WARN      0xFFB020
#define UI_COLOR_OK        0x30D158
#define UI_COLOR_ERR       0xFF453A

#define UI_STATUS_BAR_HEIGHT 28
#define UI_CONTENT_Y        UI_STATUS_BAR_HEIGHT
#define UI_CONTENT_H        (460 - UI_STATUS_BAR_HEIGHT)

typedef lv_obj_t *(*ui_app_create_fn_t)(void);

typedef struct {
    const char *id;          /**< unique key, e.g. "recorder" */
    const char *title;       /**< Chinese display name, e.g. "录音机" */
    const char *icon;        /**< LV_SYMBOL_* string */
    ui_app_create_fn_t create; /**< returns a screen-sized container, not loaded */
    bool hide_status_bar;    /**< games may hide the status bar */
} ui_app_t;

/** Build status bar on lv_layer_top() and load the watchface. Called once. */
void ui_manager_init(void);

/** Register one app; called by demo_apps_register_all() for every app. */
void ui_app_register(const ui_app_t *app);

/* App registry access (used by the launcher menu). */
int ui_app_count(void);
const ui_app_t *ui_app_at(int index);

/** Navigation. Safe to call only with ui_lock held (or from LVGL ctx). */
void ui_nav_home(void);                  /**< back to watchface */
void ui_nav_open_menu(void);             /**< open the app launcher menu */
void ui_nav_open(const char *app_id);    /**< open registered app */
void ui_nav_back(void);                  /**< pop current app screen */
bool ui_nav_at_home(void);               /**< true when watchface is active */

/**
 * Standard app scaffold: a full-screen container with a title row (back
 * button "‹" wired to ui_nav_back(), title label). Returns the container;
 * *content_out (if non-NULL) receives the content parent below the title.
 */
lv_obj_t *ui_app_scaffold(const char *title, lv_obj_t **content_out);

/** Transient message on the top layer; the text is copied internally.
 *  LVGL ctx or ui_async only. */
void ui_toast(const char *text);

/** Modal confirm box; cb(true/false, user) on OK/cancel. LVGL ctx only. */
void ui_msgbox(const char *title, const char *text,
               void (*cb)(bool ok, void *user), void *user);

/** Run fn(arg) on the LVGL thread. Safe from any task. */
void ui_async(void (*fn)(void *), void *arg);

bool ui_lock(void);   /**< bsp_display_lock(1000), false on timeout */
void ui_unlock(void);

/* Status bar setters. THREAD-SAFE (internally forward via ui_async). */
void ui_status_set_time(int hour, int minute);              /* -1 = unknown */
void ui_status_set_battery(int percent, bool charging, bool present);
void ui_status_set_wifi(int state);   /**< 0=off 1=connecting 2=connected */
void ui_status_set_ble(bool active);
void ui_status_set_sd(bool mounted);
void ui_status_set_usb(int role);     /**< 0=none 1=host 2=device */

/** Notify the manager of user activity (input events); resets idle timer. */
void ui_activity_ping(void);

/* Font helpers. */
const lv_font_t *ui_font_text(void);  /**< UI default (CJK 16) */
const lv_font_t *ui_font_mid(void);   /**< Montserrat 32 */
const lv_font_t *ui_font_big(void);   /**< Montserrat 48 */

#ifdef __cplusplus
}
#endif
