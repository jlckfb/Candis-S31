/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
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
    /* GPIO0 is both a boot strapping pin and the TF card-detect switch: a
     * card fitted at power-on holds the strap low through the sampling
     * window. Record the level so the EVT log can note "card inserted at
     * boot"; this only reads the pin and never changes boot behavior. The
     * read config matches bsp_sdcard_is_inserted(). */
    const gpio_config_t sd_detect = {
        .pin_bit_mask = 1ULL << BSP_SD_DET,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    int gpio0_boot_level = -1;
    if (gpio_config(&sd_detect) == ESP_OK) {
        gpio0_boot_level = gpio_get_level(BSP_SD_DET);
    }
    factory_console_note_gpio0_boot_level(gpio0_boot_level);
    if (gpio0_boot_level < 0) {
        ESP_LOGW(TAG, "GPIO0 (TF card detect / boot strap) read failed");
    } else {
        ESP_LOGI(TAG, "GPIO0 (TF card detect / boot strap) at boot: %s (%s)",
                 gpio0_boot_level ? "high" : "low",
                 gpio0_boot_level == BSP_SD_DET_ACTIVE_LEVEL ?
                 "TF card fitted at power-on" : "no TF card at power-on");
    }

    const esp_err_t nvs_err = factory_console_ensure_nvs();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s; results will not persist",
                 esp_err_to_name(nvs_err));
    }
    /* Restore results from before any power cycle; falls back to NOT_RUN. */
    factory_report_load();

    /* Capture the TG28_SW rail state BEFORE the board init applies its safe
     * state: the safe state deliberately turns DCDC4 and the other optional
     * rails off, so any read taken afterwards can no longer show the
     * power-on state. bsp_pmic_init() only opens the LP I2C device and
     * services interrupt flags; it never writes regulator configuration.
     *
     * This equals the OTP defaults only after a cold power-on. The TG28 runs
     * from its own supply and no SoC-only reset reaches it, while the safe
     * state clears enable bits without rewriting voltage codes: after
     * esp_restart() the enable bits still show the previous run's safe state
     * (DCDC4 reads disabled, the opposite of its OTP value) and the voltage
     * codes still hold whatever a test wrote (camera leaves ALDO4 at 2800mV
     * and DCDC2 at 1500mV). The snapshot is not a report verdict: the
     * operator compares it against the TG28 confirmation sheet per lot. */
    char otp_boot_detail[FACTORY_DETAIL_LENGTH] = {0};
    esp_err_t otp_boot_err = factory_console_capture_otp_boot_snapshot(
                                 otp_boot_detail, sizeof(otp_boot_detail));
    if (otp_boot_err != ESP_OK) {
        ESP_LOGW(TAG, "OTP boot snapshot failed: %s",
                 esp_err_to_name(otp_boot_err));
    }

    /* bsp_board_init() first configures the interrupt inputs (GPIO2
     * PMIC/RTC, GPIO43 Type-C) as pulled-up inputs, then applies the power
     * safe state. Direct bsp_power_safe_state() would leave those lines
     * floating until something else claimed them. */
    const esp_err_t safe_state_err = bsp_board_init();
    factory_report_set(FACTORY_TEST_SAFE_STATE,
                       safe_state_err == ESP_OK ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       safe_state_err == ESP_OK ? "direct power domains disabled" : esp_err_to_name(safe_state_err));

    ESP_LOGI(TAG, "Candis-S31 Factory Bring-up");
    ESP_LOGW(TAG, "EVT1 hardware has not been tested; use commands one stage at a time");
    factory_report_print_one(FACTORY_TEST_SAFE_STATE);
    if (otp_boot_err == ESP_OK) {
        /* Label the line by boot type so a warm-reset read can never be
         * filed as OTP evidence for the lot. */
        if (esp_reset_reason() == ESP_RST_POWERON) {
            ESP_LOGI(TAG, "OTP boot snapshot: %s", otp_boot_detail);
        } else {
            ESP_LOGW(TAG, "boot rail snapshot is NOT the OTP state (warm reset "
                     "leaves the TG28 as the previous run left it; power-cycle "
                     "the board to read the OTP defaults): %s", otp_boot_detail);
        }
    }

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
