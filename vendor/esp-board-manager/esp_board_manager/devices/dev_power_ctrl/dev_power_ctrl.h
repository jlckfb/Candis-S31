/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_board_device.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief  GPIO power control sub-configuration structure
 *
 *         This structure defines the configuration for GPIO-based power control,
 *         including the GPIO peripheral name and the active level for enabling power.
 */
typedef struct {
    const char *gpio_name;     /*!< GPIO peripheral name */
    uint8_t     active_level;  /*!< Active level to enable power: 0 or 1 */
} dev_power_ctrl_gpio_sub_config_t;

/**
 * @brief  Custom power control sub-configuration structure
 *
 *         This structure defines optional dependent peripherals that should be
 *         initialized before the user-registered custom power control callback
 *         is invoked.
 */
typedef struct {
    const char *const *periph_names;   /*!< Optional dependent peripheral names */
    uint8_t            periph_count;   /*!< Number of dependent peripherals */
    const void        *user_cfg;       /*!< Board-specific custom configuration */
    uint16_t           user_cfg_size;  /*!< Size of board-specific configuration */
} dev_power_ctrl_custom_sub_config_t;

typedef struct dev_power_ctrl_config dev_power_ctrl_config_t;

/**
 * @brief  Initialize a board-specific custom power controller.
 */
typedef int (*dev_power_ctrl_custom_init_func_t)(const dev_power_ctrl_config_t *config, void **context);

/**
 * @brief  Deinitialize a board-specific custom power controller.
 */
typedef int (*dev_power_ctrl_custom_deinit_func_t)(void *context);

/**
 * @brief  Set the power state of a consumer device.
 */
typedef int (*dev_power_ctrl_custom_set_power_func_t)(void *context, const char *device_name, bool power_on);

/**
 * @brief  Board-specific custom power controller operations.
 */
typedef struct {
    dev_power_ctrl_custom_init_func_t       init;       /*!< Optional controller initialization */
    dev_power_ctrl_custom_deinit_func_t     deinit;     /*!< Optional controller deinitialization */
    dev_power_ctrl_custom_set_power_func_t  set_power;  /*!< Required consumer power operation */
} dev_power_ctrl_custom_ops_t;

/**
 * @brief  Power control device handle structure
 *
 *         This structure contains the handle for the power control device.
 *         The custom subtype keeps no peripheral bookkeeping here: its dependent
 *         peripherals are referenced/unreferenced by name from the device config,
 *         and a callback that needs a dependent handle fetches it by name via
 *         esp_board_periph_get_handle().
 */
typedef struct {
    void                              *periph_handle;   /*!< Primary peripheral handle, used by gpio subtype */
    const dev_power_ctrl_custom_ops_t *custom_ops;      /*!< Custom controller operations */
    void                              *custom_context;  /*!< Board-owned custom controller context */
} dev_power_ctrl_handle_t;

/**
 * @brief  Power control device configuration structure
 *
 *         This structure contains all the configuration parameters needed to initialize
 *         a power control device, including device name, type, sub-type, and sub-type
 *         specific configuration.
 */
struct dev_power_ctrl_config {
    const char *name;      /*!< Power control device name */
    const char *type;      /*!< Device type: "power_ctrl" */
    const char *sub_type;  /*!< Power sub-type: "gpio", "power_ic(todo)", etc. */
    union {
        dev_power_ctrl_gpio_sub_config_t    gpio;    /*!< GPIO sub-type configuration */
        dev_power_ctrl_custom_sub_config_t  custom;  /*!< Custom sub-type configuration */
    } sub_cfg;                                       /*!< Sub-type specific configuration */
};

/**
 * @brief  Initialize power control device
 *
 *         This function initializes a power control device using the provided configuration structure.
 *         It sets up the necessary hardware interface based on the sub-type configuration.
 *         The resulting device handle can be used for power control operations.
 *
 * @param[in]   cfg            Pointer to the power control configuration structure
 * @param[in]   cfg_size       Size of the configuration structure
 * @param[out]  device_handle  Pointer to a variable to receive the dev_power_ctrl_handle_t handle
 *
 * @return
 *       - 0               On success
 *       - Negative_value  On failure
 */
int dev_power_ctrl_init(void *cfg, int cfg_size, void **device_handle);

/**
 * @brief  Deinitialize power control device
 *
 *         This function deinitializes the power control device and frees the allocated resources.
 *         It should be called when the device is no longer needed to prevent resource leaks.
 *
 * @param[in]  device_handle  Pointer to the device handle to be deinitialized
 *
 * @return
 *       - 0               On success
 *       - Negative_value  On failure
 */
int dev_power_ctrl_deinit(void *device_handle);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
