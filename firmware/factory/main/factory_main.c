/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"

#include "factory_console.h"
#include "factory_report.h"

static const char *TAG = "candis_factory";

void app_main(void)
{
    factory_report_init();

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
        ESP_LOGE(TAG, "Unable to start the factory console: %s", esp_err_to_name(console_err));
    }
}
