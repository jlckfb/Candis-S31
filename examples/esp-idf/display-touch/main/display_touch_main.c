/*
 * Candis-S31 display + touch example.
 *
 * AMOLED in its physical 180-degree mounting orientation (applied by the
 * BSP), a cross marker follows the finger, and each press and release is
 * reported on the serial console.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp-bsp.h"
#include "example_board.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "display_touch";

#define CROSS_ARM_PX   20
#define CROSS_THICK_PX 5

static struct {
    lv_obj_t *hbar;
    lv_obj_t *vbar;
    lv_obj_t *dot;
} s_ui;

static void marker_move(int32_t x, int32_t y)
{
    lv_obj_set_pos(s_ui.hbar, x - CROSS_ARM_PX, y - CROSS_THICK_PX / 2);
    lv_obj_set_pos(s_ui.vbar, x - CROSS_THICK_PX / 2, y - CROSS_ARM_PX);
    lv_obj_set_pos(s_ui.dot, x - CROSS_THICK_PX / 2, y - CROSS_THICK_PX / 2);
}

/* LVGL input-device event: already in LVGL context, so the marker moves
 * immediately. PRESSING fires for every sample of a drag, so only the
 * press and release points are reported on the console. */
static void touch_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const int32_t x = LV_CLAMP(0, (int32_t)point.x, BSP_LCD_H_RES - 1);
    const int32_t y = LV_CLAMP(0, (int32_t)point.y, BSP_LCD_V_RES - 1);
    marker_move(x, y);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSING) {
        ESP_LOGI(TAG, "touch %s: %ld,%ld",
                 code == LV_EVENT_PRESSED ? "press" : "release",
                 (long)x, (long)y);
    }
}


static lv_obj_t *marker_part_create(lv_obj_t *parent, int32_t w, int32_t h,
                                    bool circle)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x30A0FF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    if (circle) {
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_MAIN);
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = true,
        .start_display = true,
        .brightness_percent = 60,
    }));

    ESP_ERROR_CHECK(bsp_display_lock(1000) ? ESP_OK : ESP_ERR_TIMEOUT);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101014), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    s_ui.hbar = marker_part_create(screen, 2 * CROSS_ARM_PX, CROSS_THICK_PX, false);
    s_ui.vbar = marker_part_create(screen, CROSS_THICK_PX, 2 * CROSS_ARM_PX, false);
    s_ui.dot  = marker_part_create(screen, CROSS_THICK_PX, CROSS_THICK_PX, true);
    marker_move(BSP_LCD_H_RES / 2, BSP_LCD_V_RES / 2);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "Touch and drag - coordinates on UART");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xC0C0C8), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_add_event_cb(screen, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(screen, touch_cb, LV_EVENT_RELEASED, NULL);


    bsp_display_unlock();
    ESP_LOGI(TAG, "display-touch ready: %dx%d, cross marker follows the finger",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
}
