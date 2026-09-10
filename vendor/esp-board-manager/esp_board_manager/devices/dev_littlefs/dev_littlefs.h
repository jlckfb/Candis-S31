/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#pragma once

#include <stdint.h>
#include "esp_littlefs.h"
#include "driver/sdmmc_host.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

#define ESP_BOARD_DEVICE_LITTLEFS_TYPE            "littlefs"
#define ESP_BOARD_DEVICE_LITTLEFS_SUB_TYPE_FLASH  "flash"
#define ESP_BOARD_DEVICE_LITTLEFS_SUB_TYPE_SDMMC  "sdmmc"
#define ESP_BOARD_DEVICE_LITTLEFS_SUB_TYPE_SPI    "spi"

/**
 * @brief  SDMMC pin configuration for LittleFS SD card backend.
 *
 *         This structure defines the GPIO pins used by the SDMMC host, including
 *         clock, command, data lines, and optional card-detect/write-protect pins.
 *         Use -1 for pins that are not connected or not used by the selected bus width.
 */
typedef struct {
    int  clk;  /*!< SDMMC clock pin number */
    int  cmd;  /*!< SDMMC command pin number */
    int  d0;   /*!< SDMMC data line 0 pin number */
    int  d1;   /*!< SDMMC data line 1 pin number */
    int  d2;   /*!< SDMMC data line 2 pin number */
    int  d3;   /*!< SDMMC data line 3 pin number */
    int  d4;   /*!< SDMMC data line 4 pin number, used by 8-bit mode */
    int  d5;   /*!< SDMMC data line 5 pin number, used by 8-bit mode */
    int  d6;   /*!< SDMMC data line 6 pin number, used by 8-bit mode */
    int  d7;   /*!< SDMMC data line 7 pin number, used by 8-bit mode */
    int  cd;   /*!< Card-detect pin number */
    int  wp;   /*!< Write-protect pin number */
} dev_littlefs_sdmmc_pins_t;

/**
 * @brief  SDMMC backend configuration for LittleFS.
 *
 *         The SDMMC backend initializes an SD card with the ESP-IDF SDMMC host
 *         before registering LittleFS with the card handle.
 */
typedef struct {
    uint8_t                    slot;         /*!< SDMMC slot number, for example SDMMC_HOST_SLOT_1 */
    uint8_t                    bus_width;    /*!< SDMMC bus width, usually 1 or 4 bits */
    uint16_t                   slot_flags;   /*!< SDMMC slot flags, for example SDMMC_SLOT_FLAG_INTERNAL_PULLUP */
    dev_littlefs_sdmmc_pins_t  pins;         /*!< SDMMC GPIO pin configuration */
    int8_t                     ldo_chan_id;  /*!< On-chip LDO channel ID for SDMMC IO power, or -1 when unused */
} dev_littlefs_sdmmc_sub_config_t;

/**
 * @brief  SDSPI backend configuration for LittleFS.
 *
 *         The SDSPI backend uses an existing BMGR SPI master peripheral and
 *         attaches an SD card device to that bus before registering LittleFS.
 */
typedef struct {
    int         cs_gpio_num;   /*!< SD card chip-select GPIO number */
    const char *spi_bus_name;  /*!< Referenced BMGR SPI master peripheral name */
} dev_littlefs_spi_sub_config_t;

/**
 * @brief  LittleFS device handle.
 *
 *         For flash subtype, only mount_point is valid. For sdmmc and spi subtypes,
 *         card and host are also valid and may be used by applications for SD card
 *         diagnostics such as sdmmc_card_print_info().
 */
typedef struct {
    sdmmc_card_t *card;         /*!< SD card handle, valid for sdmmc and spi subtypes */
    sdmmc_host_t  host;         /*!< SD card host handle, valid for sdmmc and spi subtypes */
    char         *mount_point;  /*!< VFS mount point path */
} dev_littlefs_handle_t;

/**
 * @brief  LittleFS device configuration.
 *
 *         The vfs_config field maps to esp_vfs_littlefs_conf_t. Runtime-only
 *         fields such as partition, sdcard, and blockdev are owned or resolved
 *         by BMGR and should not be supplied from board YAML.
 */
typedef struct {
    const char              *name;        /*!< Device name */
    const char              *sub_type;    /*!< LittleFS backend subtype: flash, sdmmc, or spi */
    esp_vfs_littlefs_conf_t  vfs_config;  /*!< LittleFS VFS mount configuration */
    uint32_t                 frequency;   /*!< SD card clock frequency in kHz, used by sdmmc and spi subtypes */
    union {
        dev_littlefs_sdmmc_sub_config_t  sdmmc;  /*!< SDMMC backend configuration */
        dev_littlefs_spi_sub_config_t    spi;    /*!< SDSPI backend configuration */
    } sub_cfg;                                   /*!< Backend-specific configuration selected by sub_type */
} dev_littlefs_config_t;

/**
 * @brief  Initialize a LittleFS device.
 *
 *         This function dispatches to the selected backend subtype. Flash mode
 *         mounts a LittleFS partition directly. SDMMC and SPI modes initialize
 *         the SD card first and then mount LittleFS on that card.
 *
 * @param[in]   cfg            Pointer to dev_littlefs_config_t
 * @param[in]   cfg_size       Size of dev_littlefs_config_t
 * @param[out]  device_handle  Pointer to receive dev_littlefs_handle_t
 *
 * @return
 *       - 0   On success
 *       - -1  On failure
 */
int dev_littlefs_init(void *cfg, int cfg_size, void **device_handle);

/**
 * @brief  Deinitialize a LittleFS device.
 *
 *         This function unregisters the mounted LittleFS instance and releases
 *         resources owned by the selected backend subtype.
 *
 * @param[in]  device_handle  Handle returned by dev_littlefs_init
 *
 * @return
 *       - 0   On success
 *       - -1  On failure
 */
int dev_littlefs_deinit(void *device_handle);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
