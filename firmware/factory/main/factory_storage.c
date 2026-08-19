/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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


esp_err_t factory_storage_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "sdcard_test", .help = "Mount, write, verify, remove, and unmount a test file.", .func = command_sdcard_test},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
