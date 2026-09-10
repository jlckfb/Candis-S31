/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_indicator.h"

#include "bsp/candis_s31.h"

static const blink_step_t s_led_on[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 0},
    {LED_BLINK_STOP, 0, 0},
};

static const blink_step_t s_led_off[] = {
    {LED_BLINK_HOLD, LED_STATE_OFF, 0},
    {LED_BLINK_STOP, 0, 0},
};

static const blink_step_t s_blink_fast[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 500},
    {LED_BLINK_HOLD, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_blink_slow[] = {
    {LED_BLINK_HOLD, LED_STATE_ON, 1000},
    {LED_BLINK_HOLD, LED_STATE_OFF, 1000},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_breathe_fast[] = {
    {LED_BLINK_BREATHE, LED_STATE_ON, 500},
    {LED_BLINK_HOLD, LED_STATE_ON, 500},
    {LED_BLINK_BREATHE, LED_STATE_OFF, 500},
    {LED_BLINK_HOLD, LED_STATE_OFF, 500},
    {LED_BLINK_LOOP, 0, 0},
};

static const blink_step_t s_breathe_slow[] = {
    {LED_BLINK_BREATHE, LED_STATE_OFF, 2000},
    {LED_BLINK_BREATHE, LED_STATE_ON, 2000},
    {LED_BLINK_LOOP, 0, 0},
};

blink_step_t const *bsp_led_blink_defaults_lists[] = {
    [BSP_LED_ON] = s_led_on,
    [BSP_LED_OFF] = s_led_off,
    [BSP_LED_BLINK_FAST] = s_blink_fast,
    [BSP_LED_BLINK_SLOW] = s_blink_slow,
    [BSP_LED_BREATHE_FAST] = s_breathe_fast,
    [BSP_LED_BREATHE_SLOW] = s_breathe_slow,
    [BSP_LED_MAX] = NULL,
};
