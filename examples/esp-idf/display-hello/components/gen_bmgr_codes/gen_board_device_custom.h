/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * Auto-generated custom device structure definitions
 * DO NOT MODIFY THIS FILE MANUALLY
 *
 * See LICENSE file for details.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Custom device structure definitions
// These structures are dynamically generated based on YAML configuration

// Structure definition for pmic
typedef struct {
    const char *name;           /*!< Custom device name */
    const char *type;           /*!< Device type: "custom" */
    const char *chip;           /*!< Chip name */
    int8_t       i2c_address;
    int32_t      frequency_hz;
    uint8_t     peripheral_count;
    const char *peripheral_names[4];
} dev_custom_pmic_config_t;

// Structure definition for rtc
typedef struct {
    const char *name;           /*!< Custom device name */
    const char *type;           /*!< Device type: "custom" */
    const char *chip;           /*!< Chip name */
    int8_t       i2c_address;
    int32_t      frequency_hz;
    uint8_t     peripheral_count;
    const char *peripheral_names[4];
} dev_custom_rtc_config_t;

// Structure definition for board_power_ctrl
typedef struct {
    const char *name;           /*!< Custom device name */
    const char *type;           /*!< Device type: "custom" */
    const char *chip;           /*!< Chip name */
    int8_t       camera_reset_gpio;
    int8_t       camera_pwdn_gpio;
} dev_custom_board_power_ctrl_custom_config_t;
