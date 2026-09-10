/*
 * Candis-S31 USB Type-C2 host MSC example.
 *
 * Starts the host in the validated 500 mA mode, mounts the first MSC device
 * at /usb0 and lists its root directory. The probe never formats the drive
 * or writes files/sectors. Insert the drive before the enumeration deadline;
 * reset the example after a timeout, mount failure, or drive replacement.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "example_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_host_msc";

#define USB_MOUNT_PATH      "/usb0"
#define USB_ENUM_TIMEOUT_MS 15000
#define USB_MOUNT_ATTEMPTS  5

static atomic_bool s_connected;
static atomic_uchar s_device_address;

static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (event->event == MSC_DEVICE_CONNECTED) {
        atomic_store_explicit(&s_device_address, event->device.address,
                              memory_order_release);
        atomic_store_explicit(&s_connected, true, memory_order_release);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        atomic_store_explicit(&s_connected, false, memory_order_release);
        ESP_LOGI(TAG, "USB drive removed; reset with a drive inserted to probe again");
    }
}

static void list_root_directory(void)
{
    DIR *dir = opendir(USB_MOUNT_PATH);
    if (dir == NULL) {
        ESP_LOGW(TAG, "cannot open " USB_MOUNT_PATH);
        return;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ESP_LOGI(TAG, "dir: %s", entry->d_name);
        ++count;
    }
    closedir(dir);
    ESP_LOGI(TAG, "dir: %d root entr%s", count, count == 1 ? "y" : "ies");
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    }));

    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true));
    const msc_host_driver_config_t cfg = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = msc_event_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(msc_host_install(&cfg));
    ESP_LOGI(TAG, "MSC host up; insert a USB drive on Type-C2");

    const int64_t deadline =
        esp_timer_get_time() + USB_ENUM_TIMEOUT_MS * INT64_C(1000);
    while (!atomic_load_explicit(&s_connected, memory_order_acquire) &&
           esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!atomic_load_explicit(&s_connected, memory_order_acquire)) {
        ESP_LOGW(TAG, "no USB drive detected within %d ms; insert one and reset",
                 USB_ENUM_TIMEOUT_MS);
        return;
    }

    msc_host_device_handle_t device = NULL;
    esp_err_t error = msc_host_install_device(
        atomic_load_explicit(&s_device_address, memory_order_acquire),
        &device);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install_device failed: %s",
                 esp_err_to_name(error));
        return;
    }
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 0,
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };
    msc_host_device_info_t info = {0};
    if (msc_host_get_device_info(device, &info) == ESP_OK) {
        ESP_LOGI(TAG, "drive: vid=0x%04x pid=0x%04x sector=%lu count=%lu",
                 info.idVendor, info.idProduct,
                 (unsigned long)info.sector_size,
                 (unsigned long)info.sector_count);
    }
    /* A freshly attached stick can refuse the first mount while its internal
     * controller is still settling, so retry before giving up. */
    msc_host_vfs_handle_t vfs = NULL;
    for (unsigned attempt = 0; attempt < USB_MOUNT_ATTEMPTS; ++attempt) {
        error = msc_host_vfs_register(device, USB_MOUNT_PATH, &mount_cfg, &vfs);
        if (error == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "mount attempt %u failed: %s", attempt + 1,
                 esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (error != ESP_OK) {
        /* Sector 0 may be an MBR, not a FAT boot sector. Report its actual
         * partition-table offsets without treating boot code as OEM/FAT text.
         * A type byte alone does not establish the filesystem (0x07 is shared
         * by multiple formats); mount errors can also mean corruption or I/O. */
        if (info.sector_size >= 512) {
            uint8_t *boot = malloc(info.sector_size);
            if (boot == NULL) {
                ESP_LOGE(TAG, "cannot allocate sector buffer for volume diagnosis");
            } else {
                const esp_err_t read_error =
                    msc_host_read_sector(device, 0, boot, info.sector_size);
                if (read_error == ESP_OK) {
                    const uint8_t *partition = boot + 0x1BE;
                    const uint32_t first_lba = (uint32_t)partition[8] |
                        ((uint32_t)partition[9] << 8) |
                        ((uint32_t)partition[10] << 16) |
                        ((uint32_t)partition[11] << 24);
                    ESP_LOGI(TAG, "sector0: signature=%02x%02x "
                             "partition0_type=0x%02x first_lba=%lu "
                             "(partition fields apply only to an MBR)",
                             boot[510], boot[511], partition[4],
                             (unsigned long)first_lba);
                } else {
                    ESP_LOGE(TAG, "sector0 read failed: %s", esp_err_to_name(read_error));
                }
                free(boot);
            }
        }
        ESP_LOGE(TAG, "FAT mount failed: %s; FAT12/16/32 required, "
                 "volume may be unsupported, corrupt, or unreadable; "
                 "no data written, use a compatible drive and reset",
                 esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG, "USB drive mounted at " USB_MOUNT_PATH);

    list_root_directory();
}
