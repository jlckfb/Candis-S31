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
        { "tests", "Test center", LV_SYMBOL_OK, app_test_center_create, false,
          "EVT overview", UI_APP_AVAILABLE, NULL },
        { "display", "Screen test", LV_SYMBOL_EYE_OPEN,
          app_display_test_create, true, "Color+touch", UI_APP_AVAILABLE, NULL },
        { "recorder", "Recorder", LV_SYMBOL_AUDIO, app_recorder_create, false,
          "Mic+NR", UI_APP_AVAILABLE, NULL },
        { "player", "Player", LV_SYMBOL_PLAY, app_player_create, false,
          "TF - WAV", UI_APP_AVAILABLE, NULL },
        { "wifi", "WiFi", LV_SYMBOL_WIFI, app_wifi_create, false,
          "Scan+connect", UI_APP_AVAILABLE, NULL },
        { "ble", "Bluetooth", LV_SYMBOL_BLUETOOTH, app_ble_create, false,
          "Scan+GATT", UI_APP_AVAILABLE, NULL },
        { "files", "Files", LV_SYMBOL_DIRECTORY, app_files_create, false,
          "TF files", UI_APP_AVAILABLE, NULL },
        { "usb", "USB OTG", LV_SYMBOL_USB, app_usb_create, false,
          "Host-Device", UI_APP_AVAILABLE, NULL },
        { "game2048", "2048", LV_SYMBOL_PLUS, game_2048_create, true,
          "Number game", UI_APP_AVAILABLE, NULL },
        { "snake", "Snake", LV_SYMBOL_LOOP, game_snake_create, true,
          "Touch game", UI_APP_AVAILABLE, NULL },
        { "breakout", "Breakout", LV_SYMBOL_STOP, game_breakout_create, true,
          "Brick game", UI_APP_AVAILABLE, NULL },
        { "led", "LED", LV_SYMBOL_TINT, app_led_create, false,
          "Color+fx", UI_APP_AVAILABLE, NULL },
        { "camera", "Camera", LV_SYMBOL_IMAGE, app_camera_create, false,
          "Preview+capture", UI_APP_AVAILABLE, NULL },
        { "settings", "Settings", LV_SYMBOL_SETTINGS, app_settings_create, false,
          "Display+audio", UI_APP_AVAILABLE, NULL },
        { "power", "Power", LV_SYMBOL_POWER, app_power_create, false,
          "Battery+sleep", UI_APP_AVAILABLE, NULL },
        { "sysinfo", "System info", LV_SYMBOL_LIST, app_sysinfo_create, false,
          "Perf+diag", UI_APP_AVAILABLE, NULL },
    };
    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); ++i) {
        ui_app_register(&apps[i]);
    }
}
