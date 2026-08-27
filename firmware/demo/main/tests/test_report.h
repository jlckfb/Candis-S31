/*
 * Candis-S31 watch demo - test report export (spec C.6).
 *
 * RAM-session results are exported on demand to the TF card as
 * /sdcard/demo_report_YYYYMMDD_HHMMSS.jsonl (RTC timestamp; falls back to
 * uptime seconds when the RTC is invalid): one FACTORY_RESULT {json}
 * compatible line per test plus a trailing FACTORY_SUMMARY line, matching
 * the factory host tooling (run_evt.py) parsing format.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Export every registered test result to a JSONL file on the TF card.
 *
 * path_out (optional) receives the written path. Returns
 * ESP_ERR_INVALID_STATE when no card is mounted (UI greys out Export),
 * ESP_ERR_NO_MEM/ESP_FAIL on lease or I/O failures.
 */
esp_err_t test_report_export_to_sd(char *path_out, size_t path_cap);

#ifdef __cplusplus
}
#endif
