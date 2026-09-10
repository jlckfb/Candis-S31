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
#include "factory_modules.h"
#include "factory_report.h"

#define FACTORY_NVS_NAMESPACE        "factory"
#define FACTORY_NVS_KEY_CONFAIL      "console_fail"
#define FACTORY_CONSOLE_MAX_FAILURES 3

/* Factory-lab override: raise the Type-C1 input-current limit from the BSP
 * 500 mA boot default to the TG28 maximum. The lab default is locked at
 * 2000 mA. This is an application-level choice; the BSP stays
 * source-agnostic. */
#define FACTORY_INPUT_CURRENT_LIMIT_MA 2000

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
    factory_low_power_note_boot();

    /* Remove every direct GPIO-controlled supply before filesystem/NVS work
     * or any optional peripheral initialization. Put the display enables in
     * the only safe removal order even after a software reset: VCI, 2 ms,
     * then VBAT. This does not touch the TG28 rail state needed by the
     * incoming-lot OTP snapshot below. */
    static const bsp_power_domain_t shutdown_order[] = {
        BSP_POWER_DISPLAY_VCI,
        BSP_POWER_DISPLAY_VBAT,
        BSP_POWER_TYPE_C_CONTROL,
        BSP_POWER_SDCARD,
        BSP_POWER_AUDIO_PA,
        BSP_POWER_USB_OTG,
    };
    _Static_assert(sizeof(shutdown_order) / sizeof(shutdown_order[0]) ==
                   BSP_POWER_DOMAIN_COUNT,
                   "shutdown order must cover every direct power domain");
    esp_err_t boot_safe_err = ESP_OK;
    for (size_t index = 0;
            index < sizeof(shutdown_order) / sizeof(shutdown_order[0]);
            ++index) {
        if (index == 1) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        const esp_err_t error =
            bsp_power_domain_set(shutdown_order[index], false);
        if (boot_safe_err == ESP_OK && error != ESP_OK) {
            boot_safe_err = error;
        }
    }
    /* GPIO0 is the TF card-detect input (not a boot strap): a card fitted
     * at power-on holds the pin low through the sampling window. Record the
     * level so the EVT log can note "card inserted at boot"; this only
     * reads the pin and never changes boot behavior. The read config
     * matches bsp_sdcard_is_inserted(). */
    const gpio_config_t sd_detect = {
        .pin_bit_mask = 1ULL << BSP_SD_DET,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    int sd_detect_boot_level = -1;
    if (gpio_config(&sd_detect) == ESP_OK) {
        sd_detect_boot_level = gpio_get_level(BSP_SD_DET);
    }
    factory_console_note_sd_detect_boot_level(sd_detect_boot_level);
    if (sd_detect_boot_level < 0) {
        ESP_LOGW(TAG, "GPIO0 (TF card detect) read failed");
    } else {
        ESP_LOGI(TAG, "GPIO0 (TF card detect) at boot: %s (%s)",
                 sd_detect_boot_level ? "high" : "low",
                 sd_detect_boot_level == BSP_SD_DET_ACTIVE_LEVEL ?
                 "TF card fitted at power-on" : "no TF card at power-on");
    }


    /* Capture the TG28_SW rail state BEFORE the board init applies its safe
     * state: the safe state deliberately turns DCDC4 and the other optional
     * rails off, so any read taken afterwards can no longer show the
     * power-on state. bsp_pmic_init() opens the LP I2C device and clamps the
     * Type-C1 input-current limit, but does not change any regulator state.
     *
     * This equals the OTP defaults only after a cold power-on. The TG28 runs
     * from its own supply and no SoC-only reset reaches it, while the safe
     * state clears enable bits without rewriting voltage codes: after
     * esp_restart() the enable bits still show the previous run's safe state
     * (DCDC4 reads disabled, the opposite of its OTP value) and the voltage
     * codes still hold whatever a test wrote (camera leaves ALDO4 at 2800mV
     * and DCDC2 at 1500mV). The snapshot is not a report verdict: the
     * operator compares it against the TG28 confirmation sheet per lot. */
    char otp_boot_detail[FACTORY_OTP_SNAPSHOT_LENGTH] = {0};
    esp_err_t otp_boot_err = factory_console_capture_otp_boot_snapshot(
                                 otp_boot_detail, sizeof(otp_boot_detail));
    if (otp_boot_err != ESP_OK) {
        ESP_LOGW(TAG, "OTP boot snapshot failed: %s",
                 esp_err_to_name(otp_boot_err));
    }

    /* bsp_pmic_init() captured these before its first safety/profile write
     * and before the fuel-gauge programming sequence clears or creates any
     * evidence. Keep the validity mask in the log: each group is acquired
     * independently and zero is a legitimate register value. */
    bsp_pmic_early_snapshot_t pmic_early = {0};
    const esp_err_t pmic_early_err =
        bsp_pmic_get_early_snapshot(&pmic_early);
    if (pmic_early_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "TG28 early snapshot: valid=0x%02x REG00-01=%02x:%02x "
                 "REG20-21=%02x:%02x REG30=%02x REG48-4A=%02x:%02x:%02x",
                 pmic_early.valid_mask,
                 pmic_early.status[0], pmic_early.status[1],
                 pmic_early.power_source[0], pmic_early.power_source[1],
                 pmic_early.adc_control,
                 pmic_early.irq_status[0], pmic_early.irq_status[1],
                 pmic_early.irq_status[2]);
    } else {
        ESP_LOGW(TAG, "TG28 early snapshot unavailable: %s",
                 esp_err_to_name(pmic_early_err));
    }

    /* bsp_board_init() first configures the interrupt inputs (GPIO2
     * PMIC/RTC, GPIO43 Type-C) as pulled-up inputs, then applies the power
     * safe state. Direct bsp_power_safe_state() would leave those lines
     * floating until something else claimed them. */
    const esp_err_t board_init_err = bsp_board_init();
    if (boot_safe_err == ESP_OK) {
        boot_safe_err = board_init_err;
    }
    const esp_err_t limit_err =
        bsp_pmic_set_input_current_limit(FACTORY_INPUT_CURRENT_LIMIT_MA);
    if (limit_err == ESP_OK) {
        uint16_t applied = 0;
        bsp_pmic_get_input_current_limit(&applied);
        ESP_LOGI(TAG, "input current limit: %u mA (factory override)",
                 applied);
    } else {
        ESP_LOGW(TAG, "input current limit override failed: %s",
                 esp_err_to_name(limit_err));
    }
    const esp_err_t nvs_err = factory_console_ensure_nvs();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s; results will not persist",
                 esp_err_to_name(nvs_err));
    }
    /* Restore results from before any power cycle; falls back to NOT_RUN. */
    factory_report_load();
    factory_report_set(FACTORY_TEST_SAFE_STATE,
                       boot_safe_err == ESP_OK ?
                       FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       boot_safe_err == ESP_OK ?
                       "direct power domains disabled" :
                       esp_err_to_name(boot_safe_err));

    ESP_LOGI(TAG, "Candis-S31 Factory Bring-up");
    ESP_LOGI(TAG, "Run commands one stage at a time; each test reports its "
             "own result");
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
