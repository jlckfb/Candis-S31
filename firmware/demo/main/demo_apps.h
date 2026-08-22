/*
 * Candis-S31 watch demo - application registry.
 *
 * demo_apps_register_all() is called once by demo_main after ui_manager_init
 * and registers every feature app in menu order. Each app exposes a create
 * function returning a full-screen LVGL container (see ui_manager.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *app_recorder_create(void);
lv_obj_t *app_player_create(void);
lv_obj_t *app_test_center_create(void);
lv_obj_t *app_display_test_create(void);
lv_obj_t *app_wifi_create(void);
lv_obj_t *app_ble_create(void);
lv_obj_t *app_files_create(void);
lv_obj_t *app_usb_create(void);
lv_obj_t *app_led_create(void);
lv_obj_t *app_settings_create(void);
lv_obj_t *app_power_create(void);
lv_obj_t *app_camera_create(void);
lv_obj_t *app_sysinfo_create(void);
lv_obj_t *game_2048_create(void);
lv_obj_t *game_snake_create(void);
lv_obj_t *game_breakout_create(void);

void demo_apps_register_all(void);

#ifdef __cplusplus
}
#endif
