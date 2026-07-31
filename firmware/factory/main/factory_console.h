/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

/** Register Factory commands and start the standard I/O console. */
esp_err_t factory_console_start(void);

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
