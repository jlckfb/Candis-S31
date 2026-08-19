/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* Peripheral umbrella: owns the power_all_off orchestration that cuts across
 * every domain module, and dispatches command registration to them. The
 * domain commands live in factory_<domain>.c; see factory_modules.h for the
 * internal contract. */

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_err.h"

#include "factory_modules.h"
#include "factory_peripherals.h"
#include "factory_report.h"

/* power_all_off cleanup: log each failed item, keep only the first error. */
static esp_err_t power_off_note(const char *item, esp_err_t error,
                                esp_err_t first_error)
{
    if (error == ESP_OK) {
        return first_error;
    }
    printf("power_all_off: %s failed: %s\n", item, esp_err_to_name(error));
    return first_error == ESP_OK ? error : first_error;
}

esp_err_t factory_peripherals_power_all_off(void)
{
    esp_err_t first_error = ESP_OK;

    /* 1. Stop activity and release business-level handles first, so nothing
     * keeps driving a peripheral while its supply is removed. Every step is
     * best-effort and safe to repeat; cleanup never stops at a failure. */
    first_error = power_off_note("display+touch stop", factory_display_stop(),
                                 first_error);
    first_error = power_off_note("LED delete", factory_led_stop(), first_error);
    first_error = power_off_note("audio codec release", factory_audio_stop(),
                                 first_error);
    first_error = power_off_note("audio deinit", bsp_audio_deinit(), first_error);
    if (bsp_sdcard_get_handle() != NULL) {
        first_error = power_off_note("SD card unmount", bsp_sdcard_unmount(),
                                     first_error);
    }
    first_error = power_off_note("camera stop", bsp_camera_stop(), first_error);
    first_error = power_off_note("USB host stop", bsp_usb_host_stop(), first_error);

    /* 2. Board-level power-down sequence for every switched peripheral,
     * then the RGB load switch. Powering an already-off block down again is
     * harmless, which is what makes the command idempotent. */
    for (int index = 0; index < BSP_PERIPHERAL_COUNT; ++index) {
        first_error = power_off_note(bsp_peripheral_name((bsp_peripheral_t)index),
                                     bsp_peripheral_power_set((bsp_peripheral_t)index, false),
                                     first_error);
    }
    first_error = power_off_note("DC1SW (RGB rail) open",
                                 bsp_pmic_switch_enable(BSP_PMIC_SWITCH_DC1SW, false),
                                 first_error);

    /* 3. Final sweep: Type-C controller, direct power domains, camera
     * control pins, RGB data low, touch reset low, and the optional
     * TG28_SW rails. */
    first_error = power_off_note("power safe state", bsp_power_safe_state(),
                                 first_error);

    if (first_error == ESP_OK) {
        printf("power_all_off: activity stopped, every peripheral rail off\n");
    } else {
        printf("power_all_off: finished with failures; first error: %s\n",
               esp_err_to_name(first_error));
    }
    printf("power_all_off is software state only: verify off-state residual "
           "voltages with a meter before the next stage\n");
    return first_error;
}

static int command_power_all_off(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return factory_peripherals_power_all_off();
}

esp_err_t factory_peripherals_register(void)
{
    const esp_console_cmd_t command = {
        .command = "power_all_off",
        .help = "Stop activity and switch every peripheral rail off (best effort, idempotent).",
        .func = command_power_all_off,
    };
    esp_err_t error = esp_console_cmd_register(&command);
    if (error != ESP_OK) {
        return error;
    }
    esp_err_t (*const modules[])(void) = {
        factory_power_register,
        factory_rtc_register,
        factory_input_register,
        factory_usb_register,
        factory_display_register,
        factory_touch_register,
        factory_storage_register,
        factory_audio_register,
        factory_camera_register,
        factory_diag_register,
        factory_rf_register,
        factory_accel_register,
    };
    for (size_t index = 0; index < sizeof(modules) / sizeof(modules[0]); ++index) {
        error = modules[index]();
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
