/*
 * Candis-S31 simulator - NVS shim (RAM blob store).
 *
 * Only the blob API the demo's sleep-record/settings paths use. State is
 * process-local: persistence across sim runs is intentionally absent.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *ns_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key,
                      const char *value);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value,
                      size_t *length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);

#ifdef __cplusplus
}
#endif
