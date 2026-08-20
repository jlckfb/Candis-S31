/*
 * Candis-S31 watch demo - RGB LED app.
 *
 * WS2812B on GPIO4, fed through the TG28 DC1SW switch which the BSP opens
 * inside bsp_led_indicator_create(). Colors go through led_indicator_set_rgb
 * (factory_input.c pattern); the six effects map 1:1 onto the BSP blink
 * lists (bsp_led_effect_t), executed by the led_indicator engine. Leaving
 * the page always deletes the indicator so no blink context survives into
 * the shutdown path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>

#include "bsp/esp-bsp.h"

#include "demo_apps.h"
#include "ui/ui_manager.h"

#define LED_COLOR_COUNT  7
#define LED_EFFECT_COUNT 6

typedef struct {
    const char *name;
    uint32_t rgb; /* SET_IRGB ready value, moderate intensity */
} led_color_t;

typedef struct {
    const char *name;
    bsp_led_effect_t effect;
} led_effect_t;

static const led_color_t s_colors[LED_COLOR_COUNT] = {
    { "红", SET_IRGB(0, 64, 0, 0) },
    { "绿", SET_IRGB(0, 0, 64, 0) },
    { "蓝", SET_IRGB(0, 0, 0, 64) },
    { "白", SET_IRGB(0, 44, 44, 44) },
    { "紫", SET_IRGB(0, 52, 0, 56) },
    { "青", SET_IRGB(0, 0, 52, 52) },
    { "黄", SET_IRGB(0, 56, 42, 0) },
};

static const led_effect_t s_effects[LED_EFFECT_COUNT] = {
    { "常亮", BSP_LED_ON },
    { "快闪", BSP_LED_BLINK_FAST },
    { "慢闪", BSP_LED_BLINK_SLOW },
    { "快呼吸", BSP_LED_BREATHE_FAST },
    { "慢呼吸", BSP_LED_BREATHE_SLOW },
    { "关", BSP_LED_OFF },
};

typedef struct {
    bool active;
    led_indicator_handle_t led;
    int color;  /* selected index into s_colors */
    int effect; /* selected index into s_effects */
    lv_obj_t *color_btn[LED_COLOR_COUNT];
    lv_obj_t *effect_btn[LED_EFFECT_COUNT];
} led_state_t;

static led_state_t s;

static void led_select_style(lv_obj_t *btn, bool selected)
{
    lv_obj_set_style_border_color(btn, lv_color_hex(selected ? UI_COLOR_ACCENT
                                                             : UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_width(btn, selected ? 3 : 1, 0);
}

static void led_apply_effect(int index)
{
    if (!s.active || s.led == NULL || index < 0 || index >= LED_EFFECT_COUNT) {
        return;
    }
    const bsp_led_effect_t next = s_effects[index].effect;
    if (next != s_effects[s.effect].effect) {
        led_indicator_stop(s.led, s_effects[s.effect].effect);
        led_indicator_start(s.led, next);
    }
    led_select_style(s.effect_btn[s.effect], false);
    s.effect = index;
    led_select_style(s.effect_btn[s.effect], true);
}

static void led_apply_color(int index)
{
    if (!s.active || s.led == NULL || index < 0 || index >= LED_COLOR_COUNT) {
        return;
    }
    led_indicator_set_rgb(s.led, s_colors[index].rgb);
    led_select_style(s.color_btn[s.color], false);
    s.color = index;
    led_select_style(s.color_btn[s.color], true);
    /* Selecting a color while the LED is off feels dead; light it up. */
    if (s_effects[s.effect].effect == BSP_LED_OFF) {
        led_apply_effect(0);
    }
}

static void color_click_cb(lv_event_t *event)
{
    led_apply_color((int)(intptr_t)lv_event_get_user_data(event));
}

static void effect_click_cb(lv_event_t *event)
{
    led_apply_effect((int)(intptr_t)lv_event_get_user_data(event));
}

static void led_root_delete_cb(lv_event_t *event)
{
    (void)event;
    s.active = false;
    if (s.led != NULL) {
        led_indicator_stop(s.led, s_effects[s.effect].effect);
        led_indicator_set_on_off(s.led, false);
        led_indicator_delete(s.led);
        s.led = NULL;
    }
}

lv_obj_t *app_led_create(void)
{
    s = (led_state_t){ .active = true, .color = 3, .effect = 0 };

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("彩灯", &content);
    lv_obj_add_event_cb(root, led_root_delete_cb, LV_EVENT_DELETE, NULL);

    led_indicator_handle_t handles[BSP_LED_NUM] = {0};
    int count = 0;
    const esp_err_t err = bsp_led_indicator_create(handles, &count, BSP_LED_NUM);
    if (err != ESP_OK || count != BSP_LED_NUM) {
        lv_obj_t *label = lv_label_create(content);
        lv_label_set_text(label, "彩灯初始化失败");
        lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_ERR), 0);
        lv_obj_center(label);
        return root;
    }
    s.led = handles[BSP_LED_1];

    lv_obj_t *title_color = lv_label_create(content);
    lv_label_set_text(title_color, "颜色");
    lv_obj_set_style_text_color(title_color, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(title_color, LV_ALIGN_TOP_LEFT, 12, 6);

    /* Color presets: filled round buttons tinted with the LED color. */
    static const uint32_t swatch[LED_COLOR_COUNT] = {
        0xE03030, 0x30C040, 0x3060E0, 0xC0C0C0, 0xA030C0, 0x30B0B0, 0xE0B030,
    };
    for (int i = 0; i < LED_COLOR_COUNT; ++i) {
        lv_obj_t *btn = lv_button_create(content);
        lv_obj_set_size(btn, 52, 52);
        lv_obj_set_pos(btn, 12 + i * 62, 34);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(swatch[i]), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(btn, color_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s.color_btn[i] = btn;
    }
    led_select_style(s.color_btn[s.color], true);

    lv_obj_t *title_effect = lv_label_create(content);
    lv_label_set_text(title_effect, "效果");
    lv_obj_set_style_text_color(title_effect, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(title_effect, LV_ALIGN_TOP_LEFT, 12, 106);

    /* Effects: three per row, mapped onto the BSP blink lists. */
    for (int i = 0; i < LED_EFFECT_COUNT; ++i) {
        lv_obj_t *btn = lv_button_create(content);
        lv_obj_set_size(btn, 138, 52);
        lv_obj_set_pos(btn, 12 + (i % 3) * 146, 134 + (i / 3) * 62);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_add_event_cb(btn, effect_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, s_effects[i].name);
        lv_obj_center(label);
        s.effect_btn[i] = btn;
    }
    led_select_style(s.effect_btn[s.effect], true);

    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, "WS2812B · 由电源开关 DC1SW 供电");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 12, -10);

    /* Start with a visible steady white matching the default selection. */
    led_indicator_set_rgb(s.led, s_colors[s.color].rgb);
    led_indicator_start(s.led, BSP_LED_ON);

    return root;
}
