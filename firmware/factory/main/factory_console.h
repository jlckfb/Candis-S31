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
 * Snapshot every TG28_SW rail's enable state and programmed voltage.
 *
 * Must run before bsp_power_safe_state() when the goal is to capture the
 * power-on (OTP) state: the safe state deliberately disables optional rails,
 * so a snapshot taken after it reflects the safe state, not the OTP.
 * bsp_pmic_init() is called internally when needed; it only opens the LP I2C
 * device and services interrupt flags, never regulator configuration.
 *
 * The output is a single-line "name=on@3300mV ..." report truncated to fit.
 *
 * @param out destination buffer
 * @param out_size buffer size in bytes; pass at least FACTORY_DETAIL_LENGTH
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE when the report was
 *         truncated, or the underlying PMIC error.
 */
esp_err_t factory_console_capture_otp_boot_snapshot(char *out, size_t out_size);

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
