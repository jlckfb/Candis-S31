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

/**
 * Scoped permission to access the mounted TF filesystem.
 *
 * Keep the lease held for the complete lifetime of every FILE/DIR and for
 * path operations such as stat(), mkdir() and unlink(). A lease acquisition
 * fails once card removal has started, and removal waits for all acquired
 * leases to be released before calling the BSP unmount function.
 */
typedef struct {
    bool held;
} svc_storage_lease_t;

/* UNMOUNTED is emitted only after the BSP reports a successful unmount. */

/** Start the storage task (stack 4096, prio 3). Single subscriber. */
esp_err_t svc_storage_start(svc_sd_cb_t cb, void *user);

bool svc_storage_mounted(void);

/** Acquire a TF I/O lease; returns INVALID_STATE when absent/removing. */
esp_err_t svc_storage_lease_acquire(svc_storage_lease_t *lease);

/**
 * Release a previously acquired lease. Safe to call on a zero-initialized or
 * already released lease; callers must initialize lease storage with {0}.
 */
void svc_storage_lease_release(svc_storage_lease_t *lease);

/**
 * Permanently close the TF I/O lease gate for the current boot, wait up to
 * three seconds for existing leases to drain, then cleanly unmount the BSP
 * filesystem. This operation is idempotent and is intended for the final
 * deep-sleep/shutdown safe-state sequence.
 *
 * On drain or BSP-unmount failure the mounted state remains truthful and the
 * lease gate stays closed; callers must abort rail removal/power-off.
 */
esp_err_t svc_storage_quiesce_and_unmount(void);

/** "/sdcard" (BSP_SD_MOUNT_POINT). Valid regardless of mount state. */
const char *svc_storage_mount_point(void);

/** FAT capacity in bytes; ESP_ERR_INVALID_STATE when not mounted. */
esp_err_t svc_storage_get_info(uint64_t *total_bytes, uint64_t *free_bytes);

/* --- Simulator extensions ------------------------------------------------ */

/** Toggle the emulated card presence at runtime (mounted by default). */
void sim_storage_set_mounted(bool mounted);

#ifdef __cplusplus
}
#endif
