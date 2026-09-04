#include <stdint.h>

#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "esp_board_manager_includes.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_co5300.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"

#define DISPLAY_WIDTH  460
#define DISPLAY_HEIGHT 460
#define DISPLAY_VISIBLE_BRIGHTNESS_PERCENT 30

static const char *TAG = "display_hello";

static void round_window(lv_area_t *area, void *user_data)
{
    (void)user_data;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
    if (area->x2 >= DISPLAY_WIDTH) {
        area->x2 = DISPLAY_WIDTH - 1;
    }
    if (area->y2 >= DISPLAY_HEIGHT) {
        area->y2 = DISPLAY_HEIGHT - 1;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "initializing Candis-S31 through ESP Board Manager");
    ESP_ERROR_CHECK(esp_board_manager_init());

    dev_display_lcd_handles_t *lcd = NULL;
    ESP_ERROR_CHECK(esp_board_manager_get_device_handle(
        "display_lcd", (void **)&lcd));
    if (lcd == NULL || lcd->panel_handle == NULL || lcd->io_handle == NULL) {
        ESP_LOGE(TAG, "display_lcd did not return usable handles");
        return;
    }

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_priority = 5;
    adapter_config.task_core_id = 1;
    adapter_config.tick_period_ms = 1;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t display_config =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
            lcd->panel_handle, lcd->io_handle, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            ESP_LV_ADAPTER_ROTATE_0);
    /* The Board Manager example uses the validated internal-RAM stripe size.
     * The product Demo owns GPIO16 TE synchronization and the measured
     * full-screen/local-update performance paths. */
    display_config.profile.buffer_height = 48;

    lv_display_t *display = esp_lv_adapter_register_display(&display_config);
    if (display == NULL) {
        ESP_LOGE(TAG, "failed to register LVGL display");
        esp_lv_adapter_deinit();
        return;
    }
    ESP_ERROR_CHECK(esp_lv_adapter_set_area_rounder_cb(
        display, round_window, NULL));

    dev_lcd_touch_handles_t *touch = NULL;
    if (esp_board_manager_check_name("lcd_touch") &&
        esp_board_manager_get_device_handle("lcd_touch", (void **)&touch) == ESP_OK &&
        touch != NULL && touch->touch_handle != NULL) {
        esp_lv_adapter_touch_config_t touch_config =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch->touch_handle);
        (void)esp_lv_adapter_register_touch(&touch_config);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Candis-S31");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_center(title);
    lv_refr_now(display);
    esp_lv_adapter_unlock();

    /* The board factory keeps WRDISBV at 0 while unknown GRAM is replaced.
     * Restore the validated first-light level only after that hidden refresh. */
    ESP_ERROR_CHECK(esp_lcd_panel_co5300_set_brightness(
        lcd->panel_handle, DISPLAY_VISIBLE_BRIGHTNESS_PERCENT));

    ESP_LOGI(TAG, "Board Manager display ready: 460x460, QSPI 48MHz, MX|MY 180deg");
}
