/*
 * Candis-S31 player demo - SD card storage service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STORAGE_EV_MOUNTED = 0,
    STORAGE_EV_UNMOUNTED,
    STORAGE_EV_MOUNT_FAIL,
} storage_event_t;

typedef void (*storage_cb_t)(storage_event_t ev, void *user);

typedef struct {
    bool held;
} storage_lease_t;

esp_err_t storage_start(storage_cb_t cb, void *user);
bool storage_mounted(void);
const char *storage_mount_point(void);
esp_err_t storage_lease_acquire(storage_lease_t *lease);
void storage_lease_release(storage_lease_t *lease);

#ifdef __cplusplus
}
#endif
