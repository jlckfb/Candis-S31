/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/candis_s31.h"
#include "bsp_err_check.h"

typedef struct {
    i2c_port_num_t port;
    gpio_num_t scl;
    gpio_num_t sda;
    i2c_master_bus_handle_t handle;
} candis_i2c_bus_t;

static candis_i2c_bus_t s_main_bus = {
    .port = BSP_I2C_NUM,
    .scl = BSP_I2C_SCL,
    .sda = BSP_I2C_SDA,
};

static candis_i2c_bus_t s_lp_bus = {
    .port = BSP_LP_I2C_NUM,
    .scl = BSP_LP_I2C_SCL,
    .sda = BSP_LP_I2C_SDA,
};

static esp_err_t bus_init(candis_i2c_bus_t *bus)
{
    if (bus->handle != NULL) {
        return ESP_OK;
    }
    if (s_main_bus.port == s_lp_bus.port) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_master_bus_config_t config = {
        .i2c_port = bus->port,
        .sda_io_num = bus->sda,
        .scl_io_num = bus->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
#if defined(CONFIG_BSP_I2C_INTERNAL_PULLUPS) && CONFIG_BSP_I2C_INTERNAL_PULLUPS
        .flags.enable_internal_pullup = true,
#else
        .flags.enable_internal_pullup = false,
#endif
    };

    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&config, &bus->handle));
    return ESP_OK;
}

static esp_err_t bus_deinit(candis_i2c_bus_t *bus)
{
    if (bus->handle == NULL) {
        return ESP_OK;
    }

    BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(bus->handle));
    bus->handle = NULL;
    return ESP_OK;
}

static i2c_master_bus_handle_t bus_get_handle(candis_i2c_bus_t *bus)
{
    if (bus_init(bus) != ESP_OK) {
        return NULL;
    }
    return bus->handle;
}

esp_err_t bsp_i2c_init(void)
{
    return bus_init(&s_main_bus);
}

esp_err_t bsp_i2c_deinit(void)
{
    return bus_deinit(&s_main_bus);
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return bus_get_handle(&s_main_bus);
}

esp_err_t bsp_lp_i2c_init(void)
{
    return bus_init(&s_lp_bus);
}

esp_err_t bsp_lp_i2c_deinit(void)
{
    return bus_deinit(&s_lp_bus);
}

i2c_master_bus_handle_t bsp_lp_i2c_get_handle(void)
{
    return bus_get_handle(&s_lp_bus);
}
