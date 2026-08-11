/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

/** Register commands that exercise board peripherals through the BSP. */
esp_err_t factory_peripherals_register(void);

/** Stop owned activity before switching any board peripheral supply off. */
esp_err_t factory_peripherals_power_all_off(void);
