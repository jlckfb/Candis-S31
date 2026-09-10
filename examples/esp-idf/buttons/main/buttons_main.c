/*
 * Candis-S31 buttons example.
 *
 * Prints every physical input event on the serial console: BOOT short and
 * long press (GPIO61), PWR short press (TG28 shared IRQ), and the RX8130CE
 * alarm flag. Events are reported by the input service callback.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "example_board.h"
#include "example_input.h"
#include "esp_log.h"

static const char *TAG = "buttons";

static const char *event_name(example_input_event_t ev)
{
    switch (ev) {
    case EXAMPLE_INPUT_BOOT_SHORT: return "BOOT_SHORT";
    case EXAMPLE_INPUT_BOOT_LONG:  return "BOOT_LONG";
    case EXAMPLE_INPUT_PWR_SHORT:  return "PWR_SHORT";
    case EXAMPLE_INPUT_RTC_ALARM:  return "RTC_ALARM";
    default:                       return "UNKNOWN";
    }
}

static void on_input(example_input_event_t ev, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "event: %s", event_name(ev));
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    }));
    ESP_ERROR_CHECK(example_input_start(on_input, NULL));
    ESP_LOGI(TAG, "buttons example ready: short/long press BOOT, short press PWR");
}
