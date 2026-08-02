/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "factory_console.h"
#include "factory_report.h"

#define FACTORY_NVS_NAMESPACE        "factory"
#define FACTORY_NVS_KEY_CONFAIL      "console_fail"
#define FACTORY_CONSOLE_MAX_FAILURES 3

static const char *TAG = "candis_factory";

/* Best-effort restart accounting so a persistent console-start fault halts
 * the board with the original error visible instead of reboot-looping. */
static unsigned console_failure_count_bump(void)
{
    nvs_handle_t handle;
    if (nvs_open(FACTORY_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return 0; /* no bookkeeping without NVS: allow the restart */
    }
    uint8_t failures = 0;
    nvs_get_u8(handle, FACTORY_NVS_KEY_CONFAIL, &failures);
    if (failures < UINT8_MAX) {
        ++failures;
    }
    nvs_set_u8(handle, FACTORY_NVS_KEY_CONFAIL, failures);
    nvs_commit(handle);
    nvs_close(handle);
    return failures;
}

static void console_failure_count_clear(void)
{
    nvs_handle_t handle;
    if (nvs_open(FACTORY_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, FACTORY_NVS_KEY_CONFAIL);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void app_main(void)
{
    const esp_err_t nvs_err = factory_console_ensure_nvs();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s; results will not persist",
                 esp_err_to_name(nvs_err));
    }
    /* Restore results from before any power cycle; falls back to NOT_RUN. */
    factory_report_load();

    /* Establish disabled levels before any optional board peripheral is used. */
    const esp_err_t safe_state_err = bsp_power_safe_state();
    factory_report_set(FACTORY_TEST_SAFE_STATE,
                       safe_state_err == ESP_OK ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       safe_state_err == ESP_OK ? "direct power domains disabled" : esp_err_to_name(safe_state_err));

    ESP_LOGI(TAG, "Candis-S31 Factory Bring-up");
    ESP_LOGW(TAG, "EVT1 hardware has not been tested; use commands one stage at a time");
    factory_report_print_one(FACTORY_TEST_SAFE_STATE);

    const esp_err_t console_err = factory_console_start();
    if (console_err != ESP_OK) {
        const unsigned failures = console_failure_count_bump();
        if (failures < FACTORY_CONSOLE_MAX_FAILURES) {
            /* A factory image without a console cannot be driven at all; give
             * the host a few seconds to read the log, then try again cleanly. */
            ESP_LOGE(TAG, "Unable to start the factory console: %s; "
                     "restarting in 5 s (attempt %u/%u)",
                     esp_err_to_name(console_err), failures,
                     FACTORY_CONSOLE_MAX_FAILURES);
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();
        }
        ESP_LOGE(TAG, "Factory console failed to start %u times (%s); "
                 "halting instead of reboot-looping",
                 failures, esp_err_to_name(console_err));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
    console_failure_count_clear();
}
