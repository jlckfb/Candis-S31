#include "sim_app_registry.h"

#include "demo_apps.h"
#include "ui/ui_manager.h"

/* Sim menu mirrors firmware/demo/main/demo_apps.c order and titles;
 * pages that need unavailable hardware buses (USB host stack, camera
 * V4L2 pipeline) keep a BOARD ONLY placeholder instead of the real page. */

static lv_obj_t *sim_unavailable_screen(const char *title,
                                         const char *detail)
{
    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold(title, &content);
    lv_obj_t *label = lv_label_create(content);
    lv_label_set_text(label, detail);
    lv_obj_set_width(label, 396);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, ui_font_body_lg(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 16);
    return root;
}

#define SIM_UNAVAILABLE_FN(name, title) \
    static lv_obj_t *name(void) \
    { \
        return sim_unavailable_screen(title, \
            "This page drives a hardware bus that the host simulator " \
            "does not model. Preview it on the real Candis-S31 board."); \
    }

SIM_UNAVAILABLE_FN(sim_camera_create, "Camera")
SIM_UNAVAILABLE_FN(sim_usb_create, "USB OTG")

/* app_usb_host_busy / app_camera_stream_active are C.5 arbitration queries
 * used by the test framework; the sim placeholder pages never hold the
 * peripheral, so both are always false. */
bool app_usb_host_busy(void)
{
    return false;
}

bool app_camera_stream_active(void)
{
    return false;
}

void sim_register_apps(void)
{
    static const ui_app_t apps[] = {
        { "tests", "Test center", LV_SYMBOL_OK, app_test_center_create, false,
          "EVT overview", UI_APP_AVAILABLE, NULL },
        { "display", "Screen test", LV_SYMBOL_EYE_OPEN,
          app_display_test_create, true, "Color+touch", UI_APP_AVAILABLE,
          NULL },
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
        { "usb", "USB OTG", LV_SYMBOL_USB, sim_usb_create, false,
          "Host-Device", UI_APP_UNAVAILABLE, "BOARD ONLY" },
        { "game2048", "2048", LV_SYMBOL_PLUS, game_2048_create, true,
          "Number game", UI_APP_AVAILABLE, NULL },
        { "snake", "Snake", LV_SYMBOL_LOOP, game_snake_create, true,
          "Touch game", UI_APP_AVAILABLE, NULL },
        { "breakout", "Breakout", LV_SYMBOL_STOP, game_breakout_create, true,
          "Brick game", UI_APP_AVAILABLE, NULL },
        { "led", "LED", LV_SYMBOL_TINT, app_led_create, false,
          "Color+fx", UI_APP_AVAILABLE, NULL },
        { "camera", "Camera", LV_SYMBOL_IMAGE, sim_camera_create, false,
          "Preview+capture", UI_APP_UNAVAILABLE, "BOARD ONLY" },
        { "settings", "Settings", LV_SYMBOL_SETTINGS, app_settings_create,
          false, "Display+audio", UI_APP_AVAILABLE, NULL },
        { "power", "Power", LV_SYMBOL_POWER, app_power_create, false,
          "Battery+sleep", UI_APP_AVAILABLE, NULL },
        { "sysinfo", "System info", LV_SYMBOL_LIST, app_sysinfo_create,
          false, "Perf+diag", UI_APP_AVAILABLE, NULL },
    };

    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); ++i) {
        ui_app_register(&apps[i]);
    }
}
