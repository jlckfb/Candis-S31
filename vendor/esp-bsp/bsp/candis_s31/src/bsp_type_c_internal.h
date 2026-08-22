/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* One recursive lock owns the FUSB303B handle, Type-C role, OTG boost latch,
 * and native USB Host lifecycle. Callers may nest public Type-C helpers while
 * holding it. */
esp_err_t bsp_type_c_port_lock(void);
void bsp_type_c_port_unlock(void);

/* These accessors require the port lock. USB Host owns every role/power
 * transition while this flag is set; direct public role/deinit calls fail. */
bool bsp_type_c_port_host_owned_locked(void);
void bsp_type_c_port_set_host_owned_locked(bool owned);
