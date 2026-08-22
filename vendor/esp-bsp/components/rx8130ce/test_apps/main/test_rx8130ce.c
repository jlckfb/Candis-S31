/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>

#include "rx8130ce.h"

void app_main(void)
{
    /* The default config keeps backup charging off (primary cell policy). */
    const rx8130ce_config_t default_config = RX8130CE_CONFIG_DEFAULT();
    assert(default_config.device_address == RX8130CE_I2C_ADDRESS_DEFAULT &&
           default_config.scl_speed_hz == RX8130CE_I2C_CLOCK_HZ &&
           !default_config.backup_charge_enable);

    const rx8130ce_time_t leap_day = {
        .year = 2028,
        .month = 2,
        .day = 29,
        .weekday = 2,
        .hour = 23,
        .minute = 59,
        .second = 59,
    };
    assert(rx8130ce_time_is_valid(&leap_day));

    rx8130ce_time_t invalid = leap_day;
    invalid.year = 2027;
    assert(!rx8130ce_time_is_valid(&invalid));
    invalid = leap_day;
    invalid.weekday = 7;
    assert(!rx8130ce_time_is_valid(&invalid));
    invalid = leap_day;
    invalid.second = 60;
    assert(!rx8130ce_time_is_valid(&invalid));
    assert(!rx8130ce_time_is_valid(NULL));

    /* Alarm validation. */
    const rx8130ce_alarm_t daily = {
        .minute_en = true,
        .minute = 30,
        .hour_en = true,
        .hour = 7,
    };
    assert(rx8130ce_alarm_is_valid(&daily));

    /* Every field disabled is valid: an interrupt once per minute. */
    const rx8130ce_alarm_t every_minute = {0};
    assert(rx8130ce_alarm_is_valid(&every_minute));

    /* Disabled fields are ignored, even when out of range. */
    rx8130ce_alarm_t alarm = daily;
    alarm.minute_en = false;
    alarm.minute = 99;
    assert(rx8130ce_alarm_is_valid(&alarm));

    /* Enabled fields must be in range. */
    alarm = daily;
    alarm.minute = 60;
    assert(!rx8130ce_alarm_is_valid(&alarm));
    alarm = daily;
    alarm.hour = 24;
    assert(!rx8130ce_alarm_is_valid(&alarm));
    alarm = daily;
    alarm.day_en = true;
    alarm.day = 0;
    assert(!rx8130ce_alarm_is_valid(&alarm));
    alarm = daily;
    alarm.day_en = true;
    alarm.day = 32;
    assert(!rx8130ce_alarm_is_valid(&alarm));
    alarm = daily;
    alarm.weekday_en = true;
    alarm.weekday = 7;
    assert(!rx8130ce_alarm_is_valid(&alarm));

    /* Day and weekday share register 19h: enabling both is rejected. */
    alarm = daily;
    alarm.day_en = true;
    alarm.day = 15;
    alarm.weekday_en = true;
    alarm.weekday = 5;
    assert(!rx8130ce_alarm_is_valid(&alarm));
    assert(!rx8130ce_alarm_is_valid(NULL));

    /* AE assembly: AE is active-low, so enabled fields keep bit 7 clear. */
    uint8_t registers[3];
    bool use_day_alarm = false;
    rx8130ce_alarm_encode(&daily, registers, &use_day_alarm);
    assert(registers[0] == 0x30 && registers[1] == 0x07 &&
           registers[2] == 0x80 && !use_day_alarm);

    rx8130ce_alarm_encode(&every_minute, registers, &use_day_alarm);
    assert(registers[0] == 0x80 && registers[1] == 0x80 &&
           registers[2] == 0x80 && !use_day_alarm);

    const rx8130ce_alarm_t monthly = {
        .minute_en = true,
        .minute = 59,
        .hour_en = true,
        .hour = 23,
        .day_en = true,
        .day = 31,
    };
    rx8130ce_alarm_encode(&monthly, registers, &use_day_alarm);
    assert(registers[0] == 0x59 && registers[1] == 0x23 &&
           registers[2] == 0x31 && use_day_alarm);

    const rx8130ce_alarm_t weekly = {
        .hour_en = true,
        .hour = 18,
        .weekday_en = true,
        .weekday = 6,
    };
    rx8130ce_alarm_encode(&weekly, registers, &use_day_alarm);
    assert(registers[0] == 0x80 && registers[1] == 0x18 &&
           registers[2] == 0x40 && !use_day_alarm);

    /* Absolute-time alarm helper ("wake at a specific moment"). */
    const rx8130ce_time_t now = {
        .year = 2026, .month = 8, .day = 5, .weekday = 3,
        .hour = 10, .minute = 30, .second = 45,
    };
    rx8130ce_time_t target = now;

    /* One minute ahead on the same day: minute/hour/day compare, day layout. */
    target.minute = 31;
    target.second = 0;
    assert(rx8130ce_alarm_from_time(&now, &target, &alarm) == ESP_OK);
    assert(alarm.minute_en && alarm.minute == 31 &&
           alarm.hour_en && alarm.hour == 10 &&
           alarm.day_en && alarm.day == 5 && !alarm.weekday_en);
    rx8130ce_alarm_encode(&alarm, registers, &use_day_alarm);
    assert(registers[0] == 0x31 && registers[1] == 0x10 &&
           registers[2] == 0x05 && use_day_alarm);

    /* The target minute must lie strictly ahead of now. */
    target = now;
    assert(rx8130ce_alarm_from_time(&now, &target, &alarm) ==
           ESP_ERR_INVALID_ARG);
    target.minute = 29;
    assert(rx8130ce_alarm_from_time(&now, &target, &alarm) ==
           ESP_ERR_INVALID_ARG);
    target = now;
    target.hour = 9;
    target.minute = 59;
    assert(rx8130ce_alarm_from_time(&now, &target, &alarm) ==
           ESP_ERR_INVALID_ARG);

    /* Horizon: 27 days out is accepted, 28 days is rejected. */
    target = now;
    target.month = 9;
    target.day = 1;
    assert(rx8130ce_alarm_from_time(&now, &target, &alarm) == ESP_OK);
    assert(alarm.day_en && alarm.day == 1);
    target.day = 2;
    assert(rx8130ce_alarm_from_time(&now, &target, &alarm) ==
           ESP_ERR_INVALID_ARG);

    /* Month roll-over and leap years go through the day count. */
    const rx8130ce_time_t month_end = {
        .year = 2026, .month = 8, .day = 31, .weekday = 1,
        .hour = 23, .minute = 59, .second = 0,
    };
    target = month_end;
    target.month = 9;
    target.day = 1;
    target.hour = 0;
    target.minute = 0;
    assert(rx8130ce_alarm_from_time(&month_end, &target, &alarm) == ESP_OK);

    const rx8130ce_time_t leap_eve = {
        .year = 2028, .month = 2, .day = 28, .weekday = 1,
        .hour = 23, .minute = 59, .second = 0,
    };
    target = leap_eve;
    target.day = 29;
    target.hour = 0;
    target.minute = 0;
    assert(rx8130ce_alarm_from_time(&leap_eve, &target, &alarm) == ESP_OK);

    const rx8130ce_time_t common_eve = {
        .year = 2027, .month = 2, .day = 28, .weekday = 0,
        .hour = 23, .minute = 59, .second = 0,
    };
    target = common_eve;
    target.month = 3;
    target.day = 1;
    target.hour = 0;
    target.minute = 0;
    assert(rx8130ce_alarm_from_time(&common_eve, &target, &alarm) == ESP_OK);

    /* Invalid calendar inputs and NULL pointers are rejected. */
    target = now;
    target.month = 13;
    assert(rx8130ce_alarm_from_time(&now, &target, &alarm) ==
           ESP_ERR_INVALID_ARG);
    assert(rx8130ce_alarm_from_time(NULL, &now, &alarm) ==
           ESP_ERR_INVALID_ARG);
    assert(rx8130ce_alarm_from_time(&now, NULL, &alarm) ==
           ESP_ERR_INVALID_ARG);
    assert(rx8130ce_alarm_from_time(&now, &now, NULL) ==
           ESP_ERR_INVALID_ARG);

    /* Timer validation. */
    const rx8130ce_timer_t one_hour = {
        .enable = true,
        .source_clock = RX8130CE_TIMER_SOURCE_1HZ,
        .count = 3600,
    };
    assert(rx8130ce_timer_is_valid(&one_hour));

    /* A stopped timer still validates; enable does not affect validity. */
    rx8130ce_timer_t timer = one_hour;
    timer.enable = false;
    assert(rx8130ce_timer_is_valid(&timer));

    /* The preset range is 1-65535. */
    timer = one_hour;
    timer.count = 0;
    assert(!rx8130ce_timer_is_valid(&timer));

    /* Only the five defined source clocks are accepted. */
    timer = one_hour;
    timer.source_clock = (rx8130ce_timer_source_t)5;
    assert(!rx8130ce_timer_is_valid(&timer));
    assert(!rx8130ce_timer_is_valid(NULL));

    /* Timer encode: the preset goes low byte first, TSEL stays raw. */
    uint8_t timer_registers[2];
    uint8_t tsel_bits = 0;
    rx8130ce_timer_encode(&one_hour, timer_registers, &tsel_bits);
    assert(timer_registers[0] == 0x10 && timer_registers[1] == 0x0E &&
           tsel_bits == RX8130CE_TIMER_SOURCE_1HZ);

    const rx8130ce_timer_t min_fast = {
        .enable = true,
        .source_clock = RX8130CE_TIMER_SOURCE_4096HZ,
        .count = 1,
    };
    rx8130ce_timer_encode(&min_fast, timer_registers, &tsel_bits);
    assert(timer_registers[0] == 0x01 && timer_registers[1] == 0x00 &&
           tsel_bits == RX8130CE_TIMER_SOURCE_4096HZ);

    const rx8130ce_timer_t max_slow = {
        .enable = false,
        .source_clock = RX8130CE_TIMER_SOURCE_1_3600HZ,
        .count = 65535,
    };
    rx8130ce_timer_encode(&max_slow, timer_registers, &tsel_bits);
    assert(timer_registers[0] == 0xFF && timer_registers[1] == 0xFF &&
           tsel_bits == RX8130CE_TIMER_SOURCE_1_3600HZ);

    /* Digital offset encode: range, sign regions, enable bit. */
    uint8_t offset_register = 0;
    assert(rx8130ce_digital_offset_encode(true, 0, &offset_register) == ESP_OK &&
           offset_register == 0x80);
    assert(rx8130ce_digital_offset_encode(true, 63, &offset_register) == ESP_OK &&
           offset_register == (0x80 | 63));
    assert(rx8130ce_digital_offset_encode(true, -64, &offset_register) == ESP_OK &&
           offset_register == (0x80 | 64));
    assert(rx8130ce_digital_offset_encode(false, -1, &offset_register) == ESP_OK &&
           offset_register == 127);
    assert(rx8130ce_digital_offset_encode(true, 64, &offset_register) == ESP_ERR_INVALID_ARG);
    assert(rx8130ce_digital_offset_encode(true, -65, &offset_register) == ESP_ERR_INVALID_ARG);
    assert(rx8130ce_digital_offset_encode(true, 0, NULL) == ESP_ERR_INVALID_ARG);

    /* Digital offset decode: exact round-trips across both sign regions. */
    bool offset_enabled = false;
    int8_t offset_steps = 0;
    for (int8_t steps = -64; steps <= 63; steps++) {
        assert(rx8130ce_digital_offset_encode(true, steps, &offset_register) == ESP_OK);
        rx8130ce_digital_offset_decode(offset_register, &offset_enabled, &offset_steps);
        assert(offset_enabled && offset_steps == steps);
    }
    rx8130ce_digital_offset_decode(0x00, &offset_enabled, &offset_steps);
    assert(!offset_enabled && offset_steps == 0);
}
