/*
 * Candis-S31 watch demo - entry point.
 *
 * Boots the board, starts the UI shell, then the background services. Only
 * the board bring-up is fatal: every service degrades gracefully so a
 * missing TF card or a BT init fault never blacks out the demo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"

#include "demo_apps.h"
#include "demo_board.h"
#include "services/svc_audio.h"
#include "services/svc_input.h"
#include "services/svc_net.h"
#include "services/svc_power.h"
#include "services/svc_storage.h"
#include "ui/ui_manager.h"

static const char *TAG = "candis_demo";

static void svc_log_start(const char *name, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s service failed to start: %s (feature degraded)",
                 name, esp_err_to_name(err));
    }
}

/* Runs on the LVGL thread (via ui_async). */
static void nav_action(void *arg)
{
    switch ((int)(intptr_t)arg) {
    case SVC_INPUT_BOOT_SHORT:
        if (ui_nav_at_home()) {
            ui_nav_open_menu();
        } else {
            ui_nav_back();
        }
        break;
    case SVC_INPUT_PWR_SHORT:
        ui_nav_home();
        break;
    default:
        break;
    }
}

static void on_input(svc_input_event_t ev, void *user)
{
    (void)user;
    if (svc_power_is_screen_off()) {
        svc_power_activity(); /* wake only: swallow the key that woke us */
        return;
    }
    svc_power_activity();
    if (ev == SVC_INPUT_BOOT_LONG) {
        svc_power_screen_off();
        return;
    }
    ui_async(nav_action, (void *)(intptr_t)ev);
}

/* Runs on the LVGL thread; arg is a string literal. */
static void toast_literal(void *arg)
{
    ui_toast((const char *)arg);
}

static void on_power(const svc_power_status_t *st, svc_power_event_t ev,
                     void *user)
{
    (void)user;
    ui_status_set_battery(st->percent, st->charging, st->present);
    switch (ev) {
    case SVC_POWER_EV_CHARGE_START:
        svc_power_screen_on();
        ui_async(toast_literal, "充电中");
        break;
    case SVC_POWER_EV_CHARGE_DONE:
        ui_async(toast_literal, "已充满");
        break;
    default:
        break;
    }
}

static void on_sd(svc_sd_event_t ev, void *user)
{
    (void)user;
    ui_status_set_sd(ev == SVC_SD_EV_MOUNTED);
    if (ev == SVC_SD_EV_MOUNTED) {
        ui_async(toast_literal, "TF 卡已挂载");
    } else if (ev == SVC_SD_EV_UNMOUNTED) {
        ui_async(toast_literal, "TF 卡已移除");
    } else if (ev == SVC_SD_EV_MOUNT_FAIL) {
        ui_async(toast_literal, "TF 卡挂载失败");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(demo_board_init());

    ui_manager_init();
    demo_apps_register_all();

    svc_log_start("input", svc_input_start(on_input, NULL));
    svc_log_start("power", svc_power_start(on_power, NULL));
    svc_log_start("storage", svc_storage_start(on_sd, NULL));
    svc_log_start("audio", svc_audio_start());
    svc_log_start("net", svc_net_start());

    ESP_LOGI(TAG, "Candis-S31 watch demo started");
}
