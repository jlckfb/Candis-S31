/*
 * Candis-S31 watch demo - SD card storage service.
 *
 * SDMMC 4-bit via BSP; card-detect is GPIO0 (active low), polled once per
 * second (no interrupt hardware). Auto-mount on insert, auto-unmount on
 * removal. I/O errors while mounted are tolerated; a vanished card is
 * unmounted lazily.
 *
 * Callbacks run on the service task context; UI work must go through
 * ui_async().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SVC_SD_EV_MOUNTED = 0,
    SVC_SD_EV_UNMOUNTED,   /**< clean unmount (also before removal event) */
    SVC_SD_EV_MOUNT_FAIL,
} svc_sd_event_t;

typedef void (*svc_sd_cb_t)(svc_sd_event_t ev, void *user);

/** Start the storage task (stack 4096, prio 3). Single subscriber. */
esp_err_t svc_storage_start(svc_sd_cb_t cb, void *user);

bool svc_storage_mounted(void);

/** "/sdcard" (BSP_SD_MOUNT_POINT). Valid regardless of mount state. */
const char *svc_storage_mount_point(void);

/** FAT capacity in bytes; ESP_ERR_INVALID_STATE when not mounted. */
esp_err_t svc_storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif
