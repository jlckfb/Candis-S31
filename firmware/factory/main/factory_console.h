/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

/** Register Factory commands and start the standard I/O console. */
esp_err_t factory_console_start(void);

/** Initialize the default NVS partition, erasing and retrying once when the
 *  partition is full or has an incompatible layout. */
esp_err_t factory_console_ensure_nvs(void);

/**
 * Record the GPIO0 level sampled by app_main before the console starts.
 *
 * GPIO0 is both a boot strapping pin and the TF card-detect switch, so the
 * boot-time level records whether the board was powered with a card fitted.
 * board_info reports the stored level in its FACTORY_INFO line.
 */
void factory_console_note_gpio0_boot_level(int level);

/**
 * Ask the operator a yes/no question on the console.
 *
 * Emits a machine-readable FACTORY_PROMPT line first so host tooling can
 * answer automatically. Drains pending input, then waits up to timeout_s
 * seconds for one of y/Y (yes), n/N (no), or s/S (skip).
 *
 * @return 'y', 'n', 's', or 0 when the timeout elapsed without an answer.
 */
char factory_console_ask_operator(const char *test_name, const char *question,
                                  unsigned timeout_s);
