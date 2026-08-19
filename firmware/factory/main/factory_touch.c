/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

#define TOUCH_TEST_SECONDS 15

static int command_touch_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!factory_display_started() && factory_display_show_pattern() != ESP_OK) {
        factory_report_error(FACTORY_TEST_TOUCH, ESP_FAIL, "display initialization failed");
        return ESP_FAIL;
    }
    lv_indev_t *input = bsp_display_get_input_dev();
    if (input == NULL) {
        factory_report_error(FACTORY_TEST_TOUCH, ESP_ERR_INVALID_STATE, "touch input unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    printf("Touch each of the four display quadrants within %d seconds\n",
           TOUCH_TEST_SECONDS);
    uint8_t quadrants = 0;
    unsigned samples = 0;
    const int64_t deadline = esp_timer_get_time() + TOUCH_TEST_SECONDS * INT64_C(1000000);
    while (esp_timer_get_time() < deadline && quadrants != 0x0f) {
        if (bsp_display_lock(100)) {
            if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) {
                lv_point_t point;
                lv_indev_get_point(input, &point);
                const unsigned quadrant = (point.x >= BSP_LCD_H_RES / 2 ? 1U : 0U) |
                                          (point.y >= BSP_LCD_V_RES / 2 ? 2U : 0U);
                quadrants |= 1U << quadrant;
                ++samples;
            }
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "quadrants=0x%02x samples=%u", quadrants, samples);
    factory_report_set(FACTORY_TEST_TOUCH,
                       quadrants == 0x0f ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_TOUCH);
    return quadrants == 0x0f ? ESP_OK : ESP_FAIL;
}


esp_err_t factory_touch_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "touch_test", .help = "Require a touch in all four display quadrants.", .func = command_touch_test},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
