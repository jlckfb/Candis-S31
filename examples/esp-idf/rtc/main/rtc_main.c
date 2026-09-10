/*
 * Candis-S31 RTC example.
 *
 * Reads the RX8130CE calendar, optionally sets it to a fixed time
 * (CONFIG_EXAMPLE_RTC_SET_ON_BOOT), then arms the minute-compare alarm for
 * the next minute boundary and reports it over the shared IRQ line. The
 * RX8130CE alarm compares minute/hour/day fields only, so the next minute
 * boundary is the fastest self-contained target.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdatomic.h>

#include "bsp/esp-bsp.h"
#include "example_board.h"
#include "example_input.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rtc";

/* Fixed value written when CONFIG_EXAMPLE_RTC_SET_ON_BOOT=y. */
#define EXAMPLE_RTC_SET_YEAR   2026
#define EXAMPLE_RTC_SET_MONTH  1
#define EXAMPLE_RTC_SET_DAY    1

static atomic_bool s_alarm_fired;

static void on_input(example_input_event_t ev, void *user)
{
    (void)user;
    if (ev == EXAMPLE_INPUT_RTC_ALARM) {
        atomic_store(&s_alarm_fired, true);
    }
}

static void print_time(const char *prefix, const bsp_rtc_time_t *time,
                       const bsp_rtc_status_t *status)
{
    ESP_LOGI(TAG, "%s%04u-%02u-%02u %02u:%02u:%02u flags=0x%02x%s",
             prefix, time->year, time->month, time->day, time->hour,
             time->minute, time->second, status->raw,
             status->backup_voltage_low ? " backup-low" : "");
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    }));

#if CONFIG_EXAMPLE_RTC_SET_ON_BOOT
    const bsp_rtc_time_t fixed = {
        .year = EXAMPLE_RTC_SET_YEAR,
        .month = EXAMPLE_RTC_SET_MONTH,
        .day = EXAMPLE_RTC_SET_DAY,
        .weekday = 0,
        .hour = 0,
        .minute = 0,
        .second = 0,
    };
    ESP_ERROR_CHECK(bsp_rtc_set_time(&fixed));
    ESP_LOGI(TAG, "RTC set to %04u-%02u-%02u 00:00:00 (EXAMPLE_RTC_SET_ON_BOOT)",
             fixed.year, fixed.month, fixed.day);
#endif

    bsp_rtc_time_t now;
    bsp_rtc_status_t status;
    ESP_ERROR_CHECK(bsp_rtc_get_time(&now, &status));
    print_time("time: ", &now, &status);
    if (!status.time_valid) {
        ESP_LOGW(TAG, "RTC time is not valid (VLF); enable EXAMPLE_RTC_SET_ON_BOOT or set it elsewhere");
    }

    /* Drain stale PMIC/RTC flags so a fresh alarm can assert the line. */
    bsp_shared_irq_status_t serviced;
    ESP_ERROR_CHECK(bsp_shared_irq_service(&serviced));
    ESP_ERROR_CHECK(example_input_start(on_input, NULL));

    /* Minute-granularity compare: target the next minute boundary. */
    bsp_rtc_alarm_t alarm = {0};
    alarm.minute_en = true;
    alarm.minute = (uint8_t)((now.minute + 1) % 60);
    ESP_ERROR_CHECK(bsp_rtc_set_alarm(&alarm));
    ESP_ERROR_CHECK(bsp_rtc_alarm_irq_enable(true));

    /* Re-read: seconds may have passed while setting up. */
    if (bsp_rtc_get_time(&now, &status) != ESP_OK) {
        now.second = 0;
    }
    const unsigned wait_s = 60u - now.second + 8u;
    ESP_LOGI(TAG, "alarm armed for minute %02u; waiting up to %u s",
             alarm.minute, wait_s);

    bool fired = false;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(wait_s * 1000u);
    while (xTaskGetTickCount() < deadline) {
        if (atomic_load(&s_alarm_fired)) {
            fired = true;
            break;
        }
        /* Poll fallback: the latched AF flag stays visible even if the
         * input task is stalled (bsp_rtc_get_status does not clear flags). */
        if (bsp_rtc_get_status(&status) == ESP_OK && status.alarm) {
            fired = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Full cleanup chain: disable the alarm IRQ, disarm the compare, clear AF. */
    const bsp_rtc_alarm_t disarm = {0};
    bsp_rtc_alarm_irq_enable(false);
    bsp_rtc_set_alarm(&disarm);
    bsp_rtc_get_and_clear_alarm_flag(NULL);

    if (fired) {
        ESP_LOGI(TAG, "RTC alarm fired");
    } else {
        ESP_LOGE(TAG, "RTC alarm did not fire within %u s", wait_s);
    }
}
