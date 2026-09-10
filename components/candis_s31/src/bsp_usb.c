/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include "bsp/candis_s31.h"
#include "bsp_type_c_internal.h"

#define USB_HOST_TASK_STACK_SIZE 4096
#define USB_HOST_TASK_PRIORITY   5
#define USB_HOST_STOP_TIMEOUT_MS 1000

typedef enum {
    USB_HOST_STATE_OFF = 0,
    USB_HOST_STATE_STARTING,
    USB_HOST_STATE_RUNNING,
    USB_HOST_STATE_STOPPING,
    USB_HOST_STATE_ERROR,
} usb_host_state_t;

static const char *TAG = "candis_usb";
static TaskHandle_t s_usb_host_task;
static volatile bool s_stop_requested;
static volatile bool s_task_exited;
static volatile bool s_all_devices_free;
static bool s_host_installed;
static usb_host_state_t s_host_state;

static void record_first_error(esp_err_t error, esp_err_t *first_error)
{
    if (*first_error == ESP_OK && error != ESP_OK) {
        *first_error = error;
    }
}

static void usb_host_event_task(void *argument)
{
    (void)argument;
    while (!s_stop_requested) {
        uint32_t event_flags = 0;
        const esp_err_t error = usb_host_lib_handle_events(portMAX_DELAY,
                                &event_flags);
        if (error == ESP_ERR_INVALID_STATE) {
            break;
        }
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "USB Host event handling failed: %s",
                     esp_err_to_name(error));
            continue;
        }
        if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
            const esp_err_t free_error = usb_host_device_free_all();
            if (free_error == ESP_OK) {
                s_all_devices_free = true;
            }
        }
        if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0) {
            s_all_devices_free = true;
        }
    }

    s_task_exited = true;
    vTaskDelete(NULL);
}

static esp_err_t usb_host_event_task_start(void)
{
    s_stop_requested = false;
    s_task_exited = false;
    s_all_devices_free = false;
    s_usb_host_task = NULL;
    const BaseType_t created = xTaskCreate(usb_host_event_task, "usb_host_lib",
                                           USB_HOST_TASK_STACK_SIZE, NULL,
                                           USB_HOST_TASK_PRIORITY,
                                           &s_usb_host_task);
    if (created != pdPASS) {
        s_task_exited = true;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t wait_for_flag(volatile bool *flag)
{
    const TickType_t timeout = pdMS_TO_TICKS(USB_HOST_STOP_TIMEOUT_MS);
    const TickType_t start = xTaskGetTickCount();
    while (!*flag) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

/* The caller owns the recursive Type-C port lock. Every failure path still
 * removes Type-C2 VBUS; a later stop call can retry library cleanup from the
 * ERROR state after clients have deregistered. */
static esp_err_t usb_host_stop_locked(void)
{
    s_host_state = USB_HOST_STATE_STOPPING;
    esp_err_t first_error = ESP_OK;

    if (s_host_installed && !s_all_devices_free) {
        const esp_err_t free_error = usb_host_device_free_all();
        if (free_error == ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "USB Host clients must be deregistered before stop");
            record_first_error(free_error, &first_error);
        } else if (free_error == ESP_OK) {
            s_all_devices_free = true;
        } else if (free_error != ESP_ERR_NOT_FINISHED) {
            record_first_error(free_error, &first_error);
        }
        if (first_error == ESP_OK && !s_all_devices_free) {
            const esp_err_t wait_error = wait_for_flag(&s_all_devices_free);
            if (wait_error != ESP_OK) {
                ESP_LOGE(TAG, "USB devices did not become free");
                record_first_error(wait_error, &first_error);
            }
        }
    }
    /* Quiesce the root port while the host library object is still alive.
     * The upstream USB 1.5.0 teardown can leave a deferred HCD port callback;
     * powering the root port down first lets the event task drain it before
     * usb_host_uninstall() clears its global object. */
    if (first_error == ESP_OK && s_host_installed) {
        const esp_err_t root_stop_error =
            usb_host_lib_set_root_port_power(false);
        if (root_stop_error != ESP_OK &&
                root_stop_error != ESP_ERR_INVALID_STATE) {
            record_first_error(root_stop_error, &first_error);
        }
        if (root_stop_error == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (first_error == ESP_OK && s_host_installed && !s_task_exited) {
        s_stop_requested = true;
        const esp_err_t unblock_error = usb_host_lib_unblock();
        if (unblock_error != ESP_OK) {
            ESP_LOGE(TAG, "USB Host event task unblock failed");
            record_first_error(unblock_error, &first_error);
        }
        if (first_error == ESP_OK) {
            const esp_err_t wait_error = wait_for_flag(&s_task_exited);
            if (wait_error != ESP_OK) {
                ESP_LOGE(TAG, "USB Host event task did not stop");
                record_first_error(wait_error, &first_error);
            }
        }
    }

    if (first_error == ESP_OK && s_host_installed && s_task_exited) {
        s_usb_host_task = NULL;
        const esp_err_t uninstall_error = usb_host_uninstall();
        if (uninstall_error == ESP_OK) {
            s_host_installed = false;
        } else {
            ESP_LOGE(TAG, "USB Host uninstall failed");
            record_first_error(uninstall_error, &first_error);
        }
    }

    /* Host ownership blocks direct Type-C calls. Temporarily release it under
     * the same recursive lock so the board helper can disable and deinit the
     * controller. If the Host library remains installed, restore ownership:
     * ERROR means cleanup must be retried through bsp_usb_host_stop(), not
     * that direct Type-C operations may tear down a live Host library. */
    bsp_type_c_port_set_host_owned_locked(false);
    const esp_err_t power_error =
        bsp_usb_otg_power_set(false, BSP_TYPE_C_CURRENT_DEFAULT);
    record_first_error(power_error, &first_error);
    bsp_type_c_port_set_host_owned_locked(s_host_installed);

    s_host_state = first_error == ESP_OK ?
                   USB_HOST_STATE_OFF : USB_HOST_STATE_ERROR;
    return first_error;
}

esp_err_t bsp_usb_host_start(bsp_usb_host_power_mode_t mode, bool limit_500mA)
{
    ESP_RETURN_ON_FALSE(mode == BSP_USB_HOST_POWER_MODE_USB_DEV,
                        ESP_ERR_INVALID_ARG, TAG, "invalid USB power mode");
    if (!limit_500mA) {
        /* The board has no verified unlimited host mode. Requesting one is an
         * expected API constraint rather than a runtime fault, so board code
         * reports it at warning level and leaves error logs to real failures. */
        ESP_LOGW(TAG, "only the 500 mA-limited mode is supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(bsp_type_c_port_lock(), TAG, "port lock failed");
    if (s_host_state == USB_HOST_STATE_RUNNING) {
        bsp_type_c_port_unlock();
        return ESP_OK;
    }
    if (s_host_state != USB_HOST_STATE_OFF) {
        bsp_type_c_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_host_state = USB_HOST_STATE_STARTING;

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t error = usb_host_install(&host_config);
    if (error == ESP_OK) {
        s_host_installed = true;
        error = usb_host_event_task_start();
    }
    if (error == ESP_OK) {
        error = bsp_usb_otg_power_set(true, BSP_TYPE_C_CURRENT_DEFAULT);
    }
    if (error == ESP_OK) {
        bsp_type_c_port_set_host_owned_locked(true);
        s_host_state = USB_HOST_STATE_RUNNING;
        bsp_type_c_port_unlock();
        return ESP_OK;
    }

    if (s_host_installed) {
        const esp_err_t cleanup_error = usb_host_stop_locked();
        if (cleanup_error != ESP_OK) {
            ESP_LOGE(TAG, "USB Host startup cleanup failed: %s",
                     esp_err_to_name(cleanup_error));
        }
    } else {
        s_host_state = USB_HOST_STATE_OFF;
    }
    bsp_type_c_port_unlock();
    return error;
}

esp_err_t bsp_usb_host_stop(void)
{
    ESP_RETURN_ON_ERROR(bsp_type_c_port_lock(), TAG, "port lock failed");
    if (s_host_state == USB_HOST_STATE_OFF && !s_host_installed) {
        const esp_err_t error =
            bsp_usb_otg_power_set(false, BSP_TYPE_C_CURRENT_DEFAULT);
        bsp_type_c_port_unlock();
        return error;
    }
    if (s_host_state == USB_HOST_STATE_STARTING ||
            s_host_state == USB_HOST_STATE_STOPPING) {
        bsp_type_c_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = usb_host_stop_locked();
    bsp_type_c_port_unlock();
    return error;
}
