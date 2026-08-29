#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} bsp_rtc_time_t;

typedef struct {
    bool time_valid;
} bsp_rtc_status_t;

bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
lv_indev_t *bsp_display_get_input_dev(void);
esp_err_t bsp_rtc_get_time(bsp_rtc_time_t *out, bsp_rtc_status_t *status);
esp_err_t bsp_rtc_set_time(const bsp_rtc_time_t *time);
esp_err_t bsp_rtc_get_status(bsp_rtc_status_t *status);

/* TG28 input limit (mA); fixed 2000 board default in the sim. */
esp_err_t bsp_pmic_get_input_current_limit(uint16_t *out_ma);

typedef enum {
    BSP_LED_1 = 0,
    BSP_LED_NUM,
} bsp_led_t;

/* led_indicator led_convert.h color packing (pure bit math, copied so the
 * LED page keeps its firmware color table without the led_indicator dep). */
#define SET_RGB(r, g, b) \
        ((((r) & 0xFF) << 16) | (((g) & 0xFF) << 8) | ((b) & 0xFF))
#define SET_IRGB(index, r, g, b) \
        ((((index) & 0x7F) << 25) | SET_RGB(r, g, b))

typedef enum {
    BSP_LED_ON = 0,
    BSP_LED_OFF,
    BSP_LED_BLINK_FAST,
    BSP_LED_BLINK_SLOW,
    BSP_LED_BREATHE_FAST,
    BSP_LED_BREATHE_SLOW,
    BSP_LED_MAX,
} bsp_led_effect_t;

/* Backlight bookkeeping (no real backlight on the host canvas). */
esp_err_t bsp_display_brightness_set(int brightness_percent);
int sim_display_get_brightness(void);

void sim_power_set_preview_state(const char *state);
/* The simulator entry installs the SDL pointer device here. */
void sim_bsp_set_input_dev(lv_indev_t *indev);

#ifdef __cplusplus
}
#endif
