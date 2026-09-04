/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "rx8130ce.h"

#include "bsp/candis_s31.h"

static const char *TAG = "candis_rtc";
static rx8130ce_handle_t s_rtc;

static void copy_time_from_driver(const rx8130ce_time_t *source,
                                  bsp_rtc_time_t *destination)
{
    *destination = (bsp_rtc_time_t) {
        .year = source->year,
        .month = source->month,
        .day = source->day,
        .weekday = source->weekday,
        .hour = source->hour,
        .minute = source->minute,
        .second = source->second,
    };
}

static void copy_status_from_driver(const rx8130ce_status_t *source,
                                    bsp_rtc_status_t *destination)
{
    *destination = (bsp_rtc_status_t) {
        .raw = source->raw,
        .time_valid = source->time_valid,
        .alarm = source->alarm,
        .timer = source->timer,
        .update = source->update,
        .reset = source->reset,
        .backup_voltage_low = source->backup_voltage_low,
        .backup_battery_full = source->backup_battery_full,
    };
}

esp_err_t bsp_rtc_init(void)
{
    if (s_rtc != NULL) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = bsp_lp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_FAIL, TAG, "low-power I2C init failed");
    /* Candis-S31 carries a primary (non-rechargeable) backup cell on VBAT:
     * enable automatic supply switchover but never charge the cell. */
    const rx8130ce_config_t config = {
        .device_address = BSP_RX8130CE_I2C_ADDRESS,
        .scl_speed_hz = RX8130CE_I2C_CLOCK_HZ,
        .backup_charge_enable = false,
        .backup_charge_cutoff = RX8130CE_CHARGE_CUTOFF_3_02V,
        .backup_voltage_low_detect = true,
    };
    return rx8130ce_create(bus, &config, &s_rtc);
}

esp_err_t bsp_rtc_deinit(void)
{
    if (s_rtc == NULL) {
        return ESP_OK;
    }
    const esp_err_t error = rx8130ce_delete(s_rtc);
    if (error == ESP_OK) {
        s_rtc = NULL;
    }
    return error;
}

esp_err_t bsp_rtc_get_status(bsp_rtc_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "RX8130CE is unavailable");
    rx8130ce_status_t driver_status;
    ESP_RETURN_ON_ERROR(rx8130ce_get_status(s_rtc, &driver_status), TAG,
                        "RX8130CE status read failed");
    copy_status_from_driver(&driver_status, status);
    return ESP_OK;
}

esp_err_t bsp_rtc_get_time(bsp_rtc_time_t *time, bsp_rtc_status_t *status)
{
    ESP_RETURN_ON_FALSE(time != NULL, ESP_ERR_INVALID_ARG, TAG, "time is NULL");
    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "RX8130CE is unavailable");
    rx8130ce_time_t driver_time;
    rx8130ce_status_t driver_status;
    ESP_RETURN_ON_ERROR(rx8130ce_get_time(s_rtc, &driver_time,
                                          status != NULL ? &driver_status : NULL),
                        TAG, "RX8130CE time read failed");
    copy_time_from_driver(&driver_time, time);
    if (status != NULL) {
        copy_status_from_driver(&driver_status, status);
    }
    return ESP_OK;
}

esp_err_t bsp_rtc_set_time(const bsp_rtc_time_t *time)
{
    ESP_RETURN_ON_FALSE(time != NULL, ESP_ERR_INVALID_ARG, TAG, "time is NULL");
    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "RX8130CE is unavailable");
    const rx8130ce_time_t driver_time = {
        .year = time->year,
        .month = time->month,
        .day = time->day,
        .weekday = time->weekday,
        .hour = time->hour,
        .minute = time->minute,
        .second = time->second,
    };
    return rx8130ce_set_time(s_rtc, &driver_time);
}

esp_err_t bsp_rtc_set_alarm(const bsp_rtc_alarm_t *alarm)
{
    ESP_RETURN_ON_FALSE(alarm != NULL, ESP_ERR_INVALID_ARG, TAG, "alarm is NULL");
    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "RX8130CE is unavailable");
    return rx8130ce_set_alarm(s_rtc, alarm);
}

esp_err_t bsp_rtc_get_alarm(bsp_rtc_alarm_t *out_alarm)
{
    ESP_RETURN_ON_FALSE(out_alarm != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "alarm is NULL");
    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "RX8130CE is unavailable");
    return rx8130ce_get_alarm(s_rtc, out_alarm);
}

esp_err_t bsp_rtc_alarm_irq_enable(bool enable)
{
    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "RX8130CE is unavailable");
    return rx8130ce_alarm_irq_enable(s_rtc, enable);
}

esp_err_t bsp_rtc_get_and_clear_alarm_flag(bool *alarm_flag)
{
    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "RX8130CE is unavailable");
    return rx8130ce_get_and_clear_alarm_flag(s_rtc, alarm_flag);
}

esp_err_t bsp_rtc_clear_interrupt_flags(uint8_t *flags)
{
    ESP_RETURN_ON_ERROR(bsp_rtc_init(), TAG, "RX8130CE is unavailable");
    return rx8130ce_get_and_clear_interrupts(s_rtc, flags);
}
