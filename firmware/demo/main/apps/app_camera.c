/*
 * Candis-S31 watch demo - camera placeholder app.
 *
 * EVT1 has no camera FPC pin-order adapter board, so this page only shows
 * a notice. It must NOT pull camera headers or call bsp_camera_start():
 * the DVP rail (DCDC1 path) is not validated on EVT1.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "demo_apps.h"
#include "ui/ui_manager.h"

lv_obj_t *app_camera_create(void)
{
    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("相机", &content);

    lv_obj_t *icon = lv_label_create(content);
    lv_label_set_text(icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(icon, ui_font_big(), 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -46);

    lv_obj_t *text = lv_label_create(content);
    lv_label_set_text(text, "EVT1 摄像头等 FPC 线序转接板,本期不启用");
    lv_obj_set_width(text, 320);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(text, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, "转接板到位后再开放预览与拍照");
    lv_obj_set_width(hint, 320);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 70);

    return root;
}
