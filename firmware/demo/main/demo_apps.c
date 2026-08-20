/*
 * Candis-S31 watch demo - application registry.
 *
 * Menu order is the registration order below.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "demo_apps.h"

#include "ui/ui_manager.h"

void demo_apps_register_all(void)
{
    static const ui_app_t apps[] = {
        { "recorder", "录音机", LV_SYMBOL_AUDIO, app_recorder_create, false },
        { "player", "播放器", LV_SYMBOL_PLAY, app_player_create, false },
        { "wifi", "WiFi", LV_SYMBOL_WIFI, app_wifi_create, false },
        { "ble", "蓝牙", LV_SYMBOL_BLUETOOTH, app_ble_create, false },
        { "files", "文件", LV_SYMBOL_DIRECTORY, app_files_create, false },
        { "usb", "USB OTG", LV_SYMBOL_USB, app_usb_create, false },
        { "game2048", "2048", LV_SYMBOL_PLUS, game_2048_create, true },
        { "snake", "贪吃蛇", LV_SYMBOL_LOOP, game_snake_create, true },
        { "led", "彩灯", LV_SYMBOL_TINT, app_led_create, false },
        { "camera", "相机", LV_SYMBOL_IMAGE, app_camera_create, false },
        { "settings", "设置", LV_SYMBOL_SETTINGS, app_settings_create, false },
        { "power", "电源", LV_SYMBOL_POWER, app_power_create, false },
        { "sysinfo", "系统信息", LV_SYMBOL_LIST, app_sysinfo_create, false },
    };
    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); ++i) {
        ui_app_register(&apps[i]);
    }
}
