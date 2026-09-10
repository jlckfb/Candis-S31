/*
 * Candis-S31 TF card storage example.
 *
 * Mounts the SDMMC card through the BSP, prints FAT capacity, lists the
 * root directory, then writes, reads back, and deletes a probe file. With
 * no card inserted it reports the prerequisite and ends; insert one and reset.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "example_board.h"

static const char *TAG = "storage";

#define EXAMPLE_FILE_PATH BSP_SD_MOUNT_POINT "/example_storage.txt"
#define EXAMPLE_FILE_PAYLOAD "Candis-S31 storage example\n"

static void list_root_directory(void)
{
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "cannot open " BSP_SD_MOUNT_POINT);
        return;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "dir: %s", entry->d_name);
        ++count;
    }
    closedir(dir);
    ESP_LOGI(TAG, "dir: %d entr%s", count, count == 1 ? "y" : "ies");
}

static void probe_file(void)
{
    /* Never truncate a same-named file already owned by the card's user. */
    FILE *file = fopen(EXAMPLE_FILE_PATH, "wx");
    if (file == NULL) {
        ESP_LOGE(TAG, "write failed: cannot create new %s (%s); existing files are preserved",
                 EXAMPLE_FILE_PATH, strerror(errno));
        return;
    }
    const size_t expected = sizeof(EXAMPLE_FILE_PAYLOAD) - 1;
    const size_t written = fwrite(EXAMPLE_FILE_PAYLOAD, 1, expected, file);
    const int write_close = fclose(file);
    if (written != expected || write_close != 0) {
        ESP_LOGE(TAG, "write failed: %u/%u bytes, close=%d",
                 (unsigned)written, (unsigned)expected, write_close);
        goto cleanup;
    }
    ESP_LOGI(TAG, "write ok (%u bytes)", (unsigned)written);

    char buffer[64] = {0};
    file = fopen(EXAMPLE_FILE_PATH, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "read failed: cannot reopen %s", EXAMPLE_FILE_PATH);
        goto cleanup;
    }
    const size_t read_back = fread(buffer, 1, sizeof(buffer) - 1, file);
    const bool read_error = ferror(file) != 0;
    const int read_close = fclose(file);
    if (!read_error && read_close == 0 && read_back == expected &&
            strcmp(buffer, EXAMPLE_FILE_PAYLOAD) == 0) {
        ESP_LOGI(TAG, "read ok (%u bytes, content match)", (unsigned)read_back);
    } else {
        ESP_LOGE(TAG, "read mismatch: got %u bytes '%s'", (unsigned)read_back, buffer);
    }

cleanup:
    if (remove(EXAMPLE_FILE_PATH) == 0) {
        ESP_LOGI(TAG, "delete ok");
    } else {
        ESP_LOGE(TAG, "delete failed");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_board_init(&(example_board_cfg_t){
        .require_psram = false,
        .start_display = false,
        .brightness_percent = 0,
    }));

    if (!bsp_sdcard_is_inserted()) {
        ESP_LOGW(TAG, "insert TF card, then reset to run the storage probe");
        return;
    }

    const esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    ESP_ERROR_CHECK(esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total_bytes, &free_bytes));
    ESP_LOGI(TAG, "capacity: %llu KB total, %llu KB free",
             (unsigned long long)(total_bytes / 1024),
             (unsigned long long)(free_bytes / 1024));

    list_root_directory();
    probe_file();
}
