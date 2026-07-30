/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "factory_console.h"
#include "factory_peripherals.h"
#include "factory_report.h"

#define FACTORY_EXPECTED_FLASH_SIZE (16U * 1024U * 1024U)
#define FACTORY_PSRAM_TEST_SIZE     (64U * 1024U)
#define FACTORY_I2C_PROBE_TIMEOUT_MS 20

#ifndef CANDIS_S31_BSP_GIT_REV
#define CANDIS_S31_BSP_GIT_REV "unknown"
#endif

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return "other";
    }
}

static int command_board_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_err_t flash_err = esp_flash_get_size(NULL, &flash_size);

    printf("board=%s\n", BSP_BOARD_NAME);
    printf("board_revision=%s\n", BSP_BOARD_REVISION);
    printf("idf_target=%s\n", CONFIG_IDF_TARGET);
    printf("idf_version=%s\n", app->idf_ver);
    printf("app_version=%s\n", app->version);
    printf("bsp_revision=%s\n", CANDIS_S31_BSP_GIT_REV);
    printf("cores=%u\n", (unsigned)chip_info.cores);
    printf("silicon_revision=%u.%u\n",
           (unsigned)(chip_info.revision / 100),
           (unsigned)(chip_info.revision % 100));
    printf("reset_reason=%s\n", reset_reason_name(esp_reset_reason()));
    printf("flash_bytes=%" PRIu32 "\n", flash_err == ESP_OK ? flash_size : 0);
    printf("psram_bytes=%zu\n", esp_psram_get_size());
    printf("free_internal_heap=%zu\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("FACTORY_INFO {\"board\":\"%s\",\"board_revision\":\"%s\","
           "\"target\":\"%s\",\"idf\":\"%s\",\"app\":\"%s\","
           "\"bsp\":\"%s\",\"reset\":\"%s\"}\n",
           BSP_BOARD_NAME, BSP_BOARD_REVISION, CONFIG_IDF_TARGET, app->idf_ver,
           app->version, CANDIS_S31_BSP_GIT_REV, reset_reason_name(esp_reset_reason()));
    return ESP_OK;
}

static int command_safe_state(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const esp_err_t err = bsp_power_safe_state();
    char detail[64];
    snprintf(detail, sizeof(detail), "bsp_power_safe_state: %s", esp_err_to_name(err));
    factory_report_set(FACTORY_TEST_SAFE_STATE,
                       err == ESP_OK ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_SAFE_STATE);
    return err;
}

static int command_power_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    for (int domain = 0; domain < BSP_POWER_DOMAIN_COUNT; ++domain) {
        bool enabled = false;
        const esp_err_t err = bsp_power_domain_get((bsp_power_domain_t)domain, &enabled);
        printf("%-16s %s%s%s\n",
               bsp_power_domain_name((bsp_power_domain_t)domain),
               err == ESP_OK ? (enabled ? "ENABLED" : "disabled") : "unknown",
               err == ESP_OK ? "" : ": ",
               err == ESP_OK ? "" : esp_err_to_name(err));
    }
    return ESP_OK;
}

static int command_flash_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t flash_size = 0;
    const esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    char detail[64];
    if (err != ESP_OK) {
        snprintf(detail, sizeof(detail), "read failed: %s", esp_err_to_name(err));
        factory_report_set(FACTORY_TEST_FLASH, FACTORY_STATUS_FAIL, detail);
    } else {
        snprintf(detail, sizeof(detail), "size=%" PRIu32 " expected=%u",
                 flash_size, FACTORY_EXPECTED_FLASH_SIZE);
        factory_report_set(FACTORY_TEST_FLASH,
                           flash_size == FACTORY_EXPECTED_FLASH_SIZE ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                           detail);
    }
    factory_report_print_one(FACTORY_TEST_FLASH);
    return err != ESP_OK ? err :
           (flash_size == FACTORY_EXPECTED_FLASH_SIZE ? ESP_OK : ESP_FAIL);
}

static int command_psram_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!esp_psram_is_initialized()) {
        factory_report_set(FACTORY_TEST_PSRAM, FACTORY_STATUS_FAIL, "PSRAM is not initialized");
        factory_report_print_one(FACTORY_TEST_PSRAM);
        return ESP_FAIL;
    }

    uint32_t *buffer = heap_caps_malloc(FACTORY_PSRAM_TEST_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        factory_report_set(FACTORY_TEST_PSRAM, FACTORY_STATUS_FAIL, "64 KiB allocation failed");
        factory_report_print_one(FACTORY_TEST_PSRAM);
        return ESP_ERR_NO_MEM;
    }

    const size_t words = FACTORY_PSRAM_TEST_SIZE / sizeof(*buffer);
    for (size_t index = 0; index < words; ++index) {
        buffer[index] = UINT32_C(0xa5a50000) ^ (uint32_t)index;
    }

    size_t failed_index = words;
    for (size_t index = 0; index < words; ++index) {
        const uint32_t expected = UINT32_C(0xa5a50000) ^ (uint32_t)index;
        if (buffer[index] != expected) {
            failed_index = index;
            break;
        }
    }
    free(buffer);

    char detail[64];
    if (failed_index == words) {
        snprintf(detail, sizeof(detail), "tested=%u total=%zu",
                 FACTORY_PSRAM_TEST_SIZE, esp_psram_get_size());
        factory_report_set(FACTORY_TEST_PSRAM, FACTORY_STATUS_PASS, detail);
    } else {
        snprintf(detail, sizeof(detail), "mismatch_at_word=%zu", failed_index);
        factory_report_set(FACTORY_TEST_PSRAM, FACTORY_STATUS_FAIL, detail);
    }
    factory_report_print_one(FACTORY_TEST_PSRAM);
    return failed_index == words ? ESP_OK : ESP_FAIL;
}

static esp_err_t scan_i2c_bus(i2c_master_bus_handle_t handle,
                              bool addresses[128], unsigned *found)
{
    memset(addresses, 0, 128 * sizeof(addresses[0]));
    *found = 0;
    for (uint16_t address = 0x08; address <= 0x77; ++address) {
        const esp_err_t err = i2c_master_probe(handle, address, FACTORY_I2C_PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            printf("found I2C device at 0x%02x\n", address);
            addresses[address] = true;
            ++(*found);
        } else if (err != ESP_ERR_NOT_FOUND) {
            return err;
        }
    }
    return ESP_OK;
}

static int command_i2c_scan(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "main") != 0 && strcmp(argv[1], "lp") != 0)) {
        printf("usage: i2c_scan main|lp\n");
        return ESP_ERR_INVALID_ARG;
    }

    const bool use_lp_bus = strcmp(argv[1], "lp") == 0;
    const factory_test_id_t test = use_lp_bus ? FACTORY_TEST_I2C_LOW_POWER : FACTORY_TEST_I2C_MAIN;
    i2c_master_bus_handle_t handle = use_lp_bus ? bsp_lp_i2c_get_handle() : bsp_i2c_get_handle();
    if (handle == NULL) {
        factory_report_set(test, FACTORY_STATUS_FAIL, "bus initialization failed");
        factory_report_print_one(test);
        return ESP_FAIL;
    }

    unsigned found = 0;
    bool addresses[128];
    const esp_err_t err = scan_i2c_bus(handle, addresses, &found);
    const bool expected_devices_found = use_lp_bus ?
                                        addresses[BSP_RX8130CE_I2C_ADDRESS] &&
                                        addresses[BSP_TG28_SW_I2C_ADDRESS] :
                                        found > 0;
    char detail[64];
    if (err != ESP_OK) {
        snprintf(detail, sizeof(detail), "scan error: %s", esp_err_to_name(err));
        factory_report_set(test, FACTORY_STATUS_FAIL, detail);
    } else if (use_lp_bus) {
        snprintf(detail, sizeof(detail), "devices=%u rtc=%s pmic=%s", found,
                 addresses[BSP_RX8130CE_I2C_ADDRESS] ? "yes" : "no",
                 addresses[BSP_TG28_SW_I2C_ADDRESS] ? "yes" : "no");
        factory_report_set(test,
                           expected_devices_found ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                           detail);
    } else {
        snprintf(detail, sizeof(detail), "devices=%u", found);
        factory_report_set(test,
                           expected_devices_found ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                           detail);
    }
    factory_report_print_one(test);
    return err != ESP_OK ? err : (expected_devices_found ? ESP_OK : ESP_FAIL);
}

static int command_report(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    factory_report_print();
    return ESP_OK;
}

static int command_report_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    factory_report_init();
    printf("All factory results reset to NOT_RUN\n");
    return ESP_OK;
}

static int command_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Restarting Candis-S31\n");
    fflush(stdout);
    esp_restart();
    return ESP_OK;
}

esp_err_t factory_console_start(void)
{
    esp_err_t err = esp_console_register_help_command();
    if (err != ESP_OK) {
        return err;
    }

    const esp_console_cmd_t commands[] = {
        {
            .command = "board_info",
            .help = "Print board, firmware, flash, and memory information.",
            .func = command_board_info,
        },
        {
            .command = "safe_state",
            .help = "Disable every directly controlled power domain.",
            .func = command_safe_state,
        },
        {
            .command = "power_status",
            .help = "Show direct power-domain control states.",
            .func = command_power_status,
        },
        {
            .command = "flash_test",
            .help = "Check the detected flash size without writing flash.",
            .func = command_flash_test,
        },
        {
            .command = "psram_test",
            .help = "Test a temporary 64 KiB PSRAM allocation.",
            .func = command_psram_test,
        },
        {
            .command = "i2c_scan",
            .help = "Scan one bus: i2c_scan main|lp.",
            .func = command_i2c_scan,
        },
        {
            .command = "report",
            .help = "Print all test results and the JSON summary.",
            .func = command_report,
        },
        {
            .command = "report_reset",
            .help = "Reset every collected result to NOT_RUN.",
            .func = command_report_reset,
        },
        {
            .command = "reboot",
            .help = "Restart the SoC without erasing flash.",
            .func = command_reboot,
        },
    };

    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        err = esp_console_cmd_register(&commands[index]);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = factory_peripherals_register();
    if (err != ESP_OK) {
        return err;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "candis-factory>";
    repl_config.max_cmdline_length = 128;

    esp_console_repl_t *repl = NULL;
    err = esp_console_new_repl_stdio(&repl_config, &repl);
    if (err != ESP_OK) {
        return err;
    }
    return esp_console_start_repl(repl);
}
