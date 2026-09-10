/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

static int command_sdcard_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!bsp_sdcard_is_inserted()) {
        factory_report_set(FACTORY_TEST_SDCARD, FACTORY_STATUS_FAIL,
                           "card detect reports no card");
        factory_report_print_one(FACTORY_TEST_SDCARD);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t error = bsp_sdcard_mount();
    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_SDCARD, error, "mount failed");
        return error;
    }
    const char *path = BSP_SD_MOUNT_POINT "/.candis_factory_test";
    const char payload[] = "Candis-S31 SDMMC factory test\n";
    char readback[sizeof(payload)] = {0};
    bool passed = true;
    const char *phase = "write";
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        passed = false;
    } else {
        if (fwrite(payload, 1, sizeof(payload), file) != sizeof(payload)) {
            passed = false;
        }
        if (fclose(file) != 0) {
            passed = false;
        }
    }
    if (passed) {
        phase = "read";
        file = fopen(path, "rb");
        if (file == NULL) {
            passed = false;
        } else {
            if (fread(readback, 1, sizeof(readback), file) != sizeof(readback)) {
                passed = false;
            }
            if (fclose(file) != 0) {
                passed = false;
            }
        }
    }
    if (passed) {
        phase = "verify";
        passed = memcmp(readback, payload, sizeof(payload)) == 0;
    }
    /* Remove the artifact even after a failed phase, so a retry starts clean. */
    if (unlink(path) != 0 && passed) {
        phase = "delete";
        passed = false;
    }
    const esp_err_t unmount_error = bsp_sdcard_unmount();
    if (unmount_error != ESP_OK) {
        if (passed) {
            phase = "unmount";
        }
        passed = false;
        error = unmount_error;
    }
    char detail[96];
    if (passed) {
        snprintf(detail, sizeof(detail), "mount write read verify unmount passed");
    } else {
        snprintf(detail, sizeof(detail), "%s failed", phase);
    }
    factory_report_set(FACTORY_TEST_SDCARD,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_SDCARD);
    return passed ? ESP_OK : (error != ESP_OK ? error : ESP_FAIL);
}

static int command_sdcard_list(int argc, char **argv)
{
    if (argc != 1) {
        printf("usage: sdcard_list - list files in the TF card root\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_sdcard_is_inserted()) {
        printf("sdcard_list: card detect reports no card\n");
        return ESP_ERR_NOT_FOUND;
    }
    bool mounted_here = false;
    esp_err_t result = ESP_OK;
    if (bsp_sdcard_get_handle() == NULL) {
        result = bsp_sdcard_mount();
        if (result != ESP_OK) {
            printf("sdcard_list: mount failed: %s\n", esp_err_to_name(result));
            return result;
        }
        mounted_here = true;
    }

    DIR *directory = opendir(BSP_SD_MOUNT_POINT);
    if (directory == NULL) {
        printf("sdcard_list: opendir failed: %s\n", strerror(errno));
        result = ESP_FAIL;
        goto cleanup;
    }
    unsigned count = 0;
    int read_error = 0;
    while (true) {
        errno = 0;
        const struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            read_error = errno;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[sizeof(BSP_SD_MOUNT_POINT) + 1U + 256U];
        const int written = snprintf(path, sizeof(path), "%s/%s",
                                     BSP_SD_MOUNT_POINT, entry->d_name);
        struct stat state;
        if (written <= 0 || (size_t)written >= sizeof(path) ||
                stat(path, &state) != 0) {
            printf("sdcard_list: ?          %s\n", entry->d_name);
        } else if (S_ISDIR(state.st_mode)) {
            printf("sdcard_list: <DIR>      %s\n", entry->d_name);
        } else {
            printf("sdcard_list: %10" PRIuMAX " %s\n",
                   (uintmax_t)state.st_size, entry->d_name);
        }
        count++;
    }
    if (read_error != 0) {
        printf("sdcard_list: readdir failed: %s\n", strerror(read_error));
        result = ESP_FAIL;
    }
    closedir(directory);
    printf("sdcard_list: %u entr%s\n", count, count == 1 ? "y" : "ies");

cleanup:
    if (mounted_here) {
        const esp_err_t unmount_error = bsp_sdcard_unmount();
        if (result == ESP_OK && unmount_error != ESP_OK) {
            result = unmount_error;
        }
    }
    return result;
}


esp_err_t factory_storage_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "sdcard_test", .help = "Mount, write, verify, remove, and unmount a test file.", .func = command_sdcard_test},
        {.command = "sdcard_list", .help = "List files in the inserted TF card root.", .func = command_sdcard_list},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
