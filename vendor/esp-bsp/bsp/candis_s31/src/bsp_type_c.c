/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fusb303b.h"

#include "bsp/candis_s31.h"
#include "bsp_type_c_internal.h"

static const char *TAG = "candis_type_c";
static fusb303b_handle_t s_type_c;
static SemaphoreHandle_t s_port_lock;
static StaticSemaphore_t s_port_lock_storage;
static portMUX_TYPE s_port_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_host_owned;

static SemaphoreHandle_t port_lock_handle(void)
{
    taskENTER_CRITICAL(&s_port_lock_init_mux);
    if (s_port_lock == NULL) {
        s_port_lock = xSemaphoreCreateRecursiveMutexStatic(&s_port_lock_storage);
    }
    taskEXIT_CRITICAL(&s_port_lock_init_mux);
    return s_port_lock;
}

esp_err_t bsp_type_c_port_lock(void)
{
    SemaphoreHandle_t lock = port_lock_handle();
    ESP_RETURN_ON_FALSE(lock != NULL, ESP_ERR_NO_MEM, TAG,
                        "Type-C port lock creation failed");
    return xSemaphoreTakeRecursive(lock, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_FAIL;
}

void bsp_type_c_port_unlock(void)
{
    SemaphoreHandle_t lock = port_lock_handle();
    if (lock != NULL) {
        xSemaphoreGiveRecursive(lock);
    }
}

bool bsp_type_c_port_host_owned_locked(void)
{
    return s_host_owned;
}

void bsp_type_c_port_set_host_owned_locked(bool owned)
{
    s_host_owned = owned;
}

static fusb303b_role_t role_to_driver(bsp_type_c_role_t role)
{
    return role == BSP_TYPE_C_ROLE_SINK ? FUSB303B_ROLE_SINK :
           role == BSP_TYPE_C_ROLE_SOURCE ? FUSB303B_ROLE_SOURCE :
           role == BSP_TYPE_C_ROLE_DRP ? FUSB303B_ROLE_DRP :
           FUSB303B_ROLE_DISABLED;
}

static bsp_type_c_role_t role_from_driver(fusb303b_role_t role)
{
    return role == FUSB303B_ROLE_SINK ? BSP_TYPE_C_ROLE_SINK :
           role == FUSB303B_ROLE_SOURCE ? BSP_TYPE_C_ROLE_SOURCE :
           role == FUSB303B_ROLE_DRP ? BSP_TYPE_C_ROLE_DRP :
           BSP_TYPE_C_ROLE_DISABLED;
}

static esp_err_t type_c_init_locked(void)
{
    if (s_type_c != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_power_domain_set(BSP_POWER_TYPE_C_CONTROL, true), TAG,
                        "FUSB303B enable failed");
    vTaskDelay(pdMS_TO_TICKS(FUSB303B_ENABLE_TO_I2C_DELAY_MS));

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        bsp_power_domain_set(BSP_POWER_TYPE_C_CONTROL, false);
        return ESP_FAIL;
    }
    const uint8_t addresses[] = {
        BSP_FUSB303B_I2C_ADDRESS_LOW,
        BSP_FUSB303B_I2C_ADDRESS_HIGH,
    };
    esp_err_t last_error = ESP_ERR_NOT_FOUND;
    for (size_t index = 0; index < sizeof(addresses) / sizeof(addresses[0]); ++index) {
        fusb303b_config_t config = FUSB303B_CONFIG_DEFAULT();
        config.device_address = addresses[index];
        last_error = fusb303b_create(bus, &config, &s_type_c);
        if (last_error == ESP_OK) {
            last_error = fusb303b_set_role(s_type_c, FUSB303B_ROLE_DRP,
                                           FUSB303B_CURRENT_DEFAULT);
        }
        if (last_error == ESP_OK) {
            last_error = fusb303b_set_enabled(s_type_c, true);
        }
        if (last_error == ESP_OK) {
            last_error = fusb303b_set_global_interrupt_mask(s_type_c, false);
        }
        if (last_error == ESP_OK) {
            return ESP_OK;
        }
        if (s_type_c != NULL) {
            fusb303b_delete(s_type_c);
            s_type_c = NULL;
        }
    }
    bsp_power_domain_set(BSP_POWER_TYPE_C_CONTROL, false);
    return last_error;
}

static esp_err_t type_c_deinit_locked(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_type_c != NULL) {
        fusb303b_handle_t handle = s_type_c;
        /* Teardown is terminal even if the bus refuses to remove the device:
         * never retain a handle after its control domain is switched off. */
        s_type_c = NULL;
        first_error = fusb303b_delete(handle);
    }
    const esp_err_t power_error =
        bsp_power_domain_set(BSP_POWER_TYPE_C_CONTROL, false);
    return first_error != ESP_OK ? first_error : power_error;
}

static esp_err_t type_c_set_role_locked(bsp_type_c_role_t role,
                                        bsp_type_c_current_t current)
{
    ESP_RETURN_ON_ERROR(type_c_init_locked(), TAG, "FUSB303B is unavailable");
    return fusb303b_set_role(s_type_c, role_to_driver(role),
                             current == BSP_TYPE_C_CURRENT_3_0_A ?
                             FUSB303B_CURRENT_3_0_A :
                             current == BSP_TYPE_C_CURRENT_1_5_A ?
                             FUSB303B_CURRENT_1_5_A :
                             FUSB303B_CURRENT_DEFAULT);
}

esp_err_t bsp_type_c_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_type_c_port_lock(), TAG, "port lock failed");
    const esp_err_t error = type_c_init_locked();
    bsp_type_c_port_unlock();
    return error;
}

esp_err_t bsp_type_c_deinit(void)
{
    ESP_RETURN_ON_ERROR(bsp_type_c_port_lock(), TAG, "port lock failed");
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (!s_host_owned) {
        const esp_err_t boost_error =
            bsp_power_domain_set(BSP_POWER_USB_OTG, false);
        const esp_err_t deinit_error = type_c_deinit_locked();
        error = boost_error != ESP_OK ? boost_error : deinit_error;
    }
    bsp_type_c_port_unlock();
    return error;
}

esp_err_t bsp_type_c_get_status(bsp_type_c_status_t *status, bool clear_interrupts)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    ESP_RETURN_ON_ERROR(bsp_type_c_port_lock(), TAG, "port lock failed");
    esp_err_t error = type_c_init_locked();
    fusb303b_status_t driver_status = {0};
    fusb303b_role_t driver_role = FUSB303B_ROLE_DISABLED;
    if (error == ESP_OK) {
        error = fusb303b_get_role(s_type_c, &driver_role);
    }
    if (error == ESP_OK) {
        error = fusb303b_get_status(s_type_c, &driver_status,
                                    clear_interrupts);
    }
    if (error == ESP_OK) {
        *status = (bsp_type_c_status_t) {
            .i2c_address = driver_status.i2c_address,
            .device_id = driver_status.device_id,
            .device_type = driver_status.device_type,
            .status = driver_status.status,
            .status1 = driver_status.status1,
            .type = driver_status.type,
            .interrupt = driver_status.interrupt,
            .interrupt1 = driver_status.interrupt1,
            .attached = driver_status.attached,
            .vbus_ok = driver_status.vbus_ok,
            .vbus_safe_0v = driver_status.vbus_safe_0v,
            .fault = driver_status.fault,
            .remedy_active = driver_status.remedy_active,
            .orientation = driver_status.orientation,
            .role = role_from_driver(driver_role),
            .advertised_current =
                driver_status.advertised_current == FUSB303B_CURRENT_3_0_A ?
                BSP_TYPE_C_CURRENT_3_0_A :
                driver_status.advertised_current == FUSB303B_CURRENT_1_5_A ?
                BSP_TYPE_C_CURRENT_1_5_A : BSP_TYPE_C_CURRENT_DEFAULT,
        };
    }
    bsp_type_c_port_unlock();
    return error;
}

esp_err_t bsp_type_c_set_role(bsp_type_c_role_t role, bsp_type_c_current_t current)
{
    ESP_RETURN_ON_FALSE(role >= BSP_TYPE_C_ROLE_DISABLED &&
                        role <= BSP_TYPE_C_ROLE_DRP,
                        ESP_ERR_INVALID_ARG, TAG, "invalid Type-C role request");
    /* Type-C2 only advertises the USB 500 mA default; a higher current
     * request must fail explicitly instead of silently downgrading. */
    ESP_RETURN_ON_FALSE(current == BSP_TYPE_C_CURRENT_DEFAULT,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "only the 500 mA default current is supported");
    ESP_RETURN_ON_ERROR(bsp_type_c_port_lock(), TAG, "port lock failed");
    if (s_host_owned) {
        bsp_type_c_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /* Never reprogram the CC role while 5 V is present. A Source request
     * selects CC only; the caller must explicitly arm the boost afterwards. */
    esp_err_t first_error = bsp_power_domain_set(BSP_POWER_USB_OTG, false);
    const esp_err_t role_error = first_error == ESP_OK ?
                                 type_c_set_role_locked(role, current) : ESP_OK;
    bsp_type_c_port_unlock();
    return first_error != ESP_OK ? first_error : role_error;
}

esp_err_t bsp_usb_otg_power_set(bool enable, bsp_type_c_current_t current)
{
    ESP_RETURN_ON_FALSE(current == BSP_TYPE_C_CURRENT_DEFAULT,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "only the 500 mA default current is supported");
    ESP_RETURN_ON_ERROR(bsp_type_c_port_lock(), TAG, "port lock failed");
    if (s_host_owned) {
        bsp_type_c_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t first_error = ESP_OK;
    if (!enable) {
        first_error = bsp_power_domain_set(BSP_POWER_USB_OTG, false);
        if (s_type_c != NULL) {
            const esp_err_t role_error =
                fusb303b_set_role(s_type_c, FUSB303B_ROLE_DISABLED,
                                  FUSB303B_CURRENT_DEFAULT);
            if (first_error == ESP_OK) {
                first_error = role_error;
            }
        }
        const esp_err_t control_error = type_c_deinit_locked();
        if (first_error == ESP_OK) {
            first_error = control_error;
        }
        bsp_type_c_port_unlock();
        return first_error;
    }

    /* Make repeated enable calls obey the same Source-before-boost ordering:
     * remove VBUS before touching any FUSB303B role register. */
    first_error = bsp_power_domain_set(BSP_POWER_USB_OTG, false);
    if (first_error != ESP_OK) {
        bsp_type_c_port_unlock();
        return first_error;
    }

    first_error = type_c_set_role_locked(BSP_TYPE_C_ROLE_SOURCE, current);
    if (first_error == ESP_OK) {
        first_error = bsp_power_domain_set(BSP_POWER_USB_OTG, true);
    }
    if (first_error != ESP_OK) {
        bsp_power_domain_set(BSP_POWER_USB_OTG, false);
        if (s_type_c != NULL) {
            fusb303b_set_role(s_type_c, FUSB303B_ROLE_DISABLED,
                              FUSB303B_CURRENT_DEFAULT);
        }
        type_c_deinit_locked();
    }
    bsp_type_c_port_unlock();
    return first_error;
}
