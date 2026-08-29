/*
 * Candis-S31 watch demo - UI manager: navigation, status bar, theme.
 *
 * Threading contract:
 *  - ui_lock()/ui_unlock() wrap bsp_display_lock(); every direct LVGL call
 *    must hold the lock.
 *  - ui_async() schedules fn on the LVGL thread through a fixed FreeRTOS
 *    queue drained by an LVGL timer and is
 *    the ONLY way service tasks may touch UI state indirectly. A false return
 *    means the callback was not queued and ownership of arg stays with the
 *    caller. Service
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

#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Theme palette lives in ui_theme.h (redesign spec B.1); every page
 * uses the UI_COL_* tokens directly. */

#define UI_STATUS_BAR_HEIGHT 32
#define UI_CONTENT_Y        UI_STATUS_BAR_HEIGHT
#define UI_CONTENT_H        (460 - UI_STATUS_BAR_HEIGHT)

/* Fixed-density layout tokens for the 2-inch 460 x 460 panel. */
#define UI_SCREEN_PAD        16
#define UI_GAP               12
#define UI_RADIUS_CARD       18
#define UI_TOUCH_MIN         56
#define UI_TITLE_ROW_HEIGHT  64

typedef enum {
    UI_APP_AVAILABLE = 0,
    UI_APP_UNAVAILABLE,
} ui_app_availability_t;

typedef lv_obj_t *(*ui_app_create_fn_t)(void);

typedef struct {
    const char *id;          /**< unique key, e.g. "recorder" */
    const char *title;       /**< Chinese display name, e.g. "录音机" */
    const char *icon;        /**< LV_SYMBOL_* string */
    ui_app_create_fn_t create; /**< returns a screen-sized container, not loaded */
    bool hide_status_bar;    /**< games may hide the status bar */
    const char *subtitle;    /**< short launcher description, may be NULL */
    ui_app_availability_t availability;
    const char *availability_text; /**< launcher badge/reason, may be NULL */
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
void ui_nav_open(const char *app_id);    /**< open registered app */
void ui_nav_open_menu(void);             /**< open launcher (watchface only) */
void ui_nav_back(void);                  /**< pop current app screen */
bool ui_nav_at_home(void);               /**< true when watchface is active */
/** Current top-of-stack identity: app id, "menu" for the launcher,
 *  "watchface" at the root. Shell-owned push screens (app == NULL)
 *  resolve to the app beneath them. */
const char *ui_nav_current_id(void);

/**
 * Push a shell-owned screen that is not a registered app (e.g. test-center
 * detail/run views, spec A.1-5). The screen is auto-deleted on pop like an
 * app screen. Safe to call only with ui_lock held (or from LVGL ctx).
 */
void ui_nav_push_screen(lv_obj_t *screen);

typedef enum {
    UI_NAV_REQUEST_BACK_OR_MENU = 0,
    UI_NAV_REQUEST_HOME,
} ui_nav_request_t;

/**
 * Post a physical-key navigation request from a service task.
 *
 * This uses a dedicated fixed queue, independent of ui_async(). If that
 * queue is momentarily full, a fixed coalescing mailbox preserves the
 * request; HOME takes precedence when multiple overflow requests coexist.
 * Returns false only before the UI manager is initialized or for an invalid
 * request.
 */
bool ui_nav_request(ui_nav_request_t request);

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

/**
 * Run fn(arg) on the LVGL thread. Safe from any task (not ISR context).
 *
 * Returns true only after the callback has been queued. On false (including
 * an uninitialized/full queue), fn will not run and the caller retains
 * ownership of arg.
 */
bool ui_async(void (*fn)(void *), void *arg);

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
const lv_font_t *ui_font_text(void);    /**< UI default (CJK 16) */
const lv_font_t *ui_font_body(void);    /**< Montserrat 24 (symbols/digits) */
const lv_font_t *ui_font_body_lg(void); /**< Project CJK 20 + CJK 16 fallback */
const lv_font_t *ui_font_title(void);   /**< Project CJK 24 + CJK 16 fallback */
const lv_font_t *ui_font_mid(void);     /**< Montserrat 32 */
const lv_font_t *ui_font_big(void);     /**< Montserrat 48 */

#ifdef __cplusplus
}
#endif
