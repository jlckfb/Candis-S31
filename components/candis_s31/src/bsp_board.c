/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"

#include "bsp/candis_s31.h"

static const char *TAG = "candis_board";

esp_err_t bsp_board_init(void)
{
    const gpio_config_t interrupt_inputs = {
        .pin_bit_mask = (1ULL << BSP_PMIC_RTC_INT) | (1ULL << BSP_TYPE_C_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&interrupt_inputs), TAG,
                        "interrupt GPIO setup failed");
    return bsp_power_safe_state();
}
