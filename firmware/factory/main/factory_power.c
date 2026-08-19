/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

static int command_pmic_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_pmic_status_t status;
    uint8_t power_on_source = 0;
    uint16_t charge_current = 0;
    esp_err_t error = bsp_pmic_get_status(&status);
    if (error == ESP_OK) {
        error = bsp_pmic_get_power_on_source(&power_on_source);
    }
    if (error == ESP_OK) {
        error = bsp_pmic_get_charge_current(&charge_current);
    }
    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_PMIC, error, "TG28_SW status read failed");
        return error;
    }
    printf("chip_id=0x%02x vbat=%u mV soc=%u%% battery=%s vbus=%s "
           "charging=%s done=%s charge_current=%u mA power_on_source=0x%02x "
           "status0=0x%02x status1=0x%02x\n",
           status.chip_id, status.battery_mv, status.battery_percent,
           status.battery_present ? "present" : "absent",
           status.vbus_present ? "present" : "absent",
           status.charging ? "yes" : "no", status.charge_done ? "yes" : "no",
           charge_current, power_on_source, status.common_status0,
           status.common_status1);
    const bool known_id = status.chip_id == 0x47 || status.chip_id == 0x4a;
    char detail[96];
    snprintf(detail, sizeof(detail), "id=0x%02x vbat=%u soc=%u battery=%s vbus=%s",
             status.chip_id, status.battery_mv, status.battery_percent,
             status.battery_present ? "yes" : "no",
             status.vbus_present ? "yes" : "no");
    factory_report_set(FACTORY_TEST_PMIC,
                       known_id ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_PMIC);
    return known_id ? ESP_OK : ESP_FAIL;
}

static int command_pmic(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "power_on_source") == 0) {
        uint8_t source = 0;
        const esp_err_t error = bsp_pmic_get_power_on_source(&source);
        if (error == ESP_OK) {
            printf("power_on_source=0x%02x\n", source);
        }
        return error;
    }
    if (argc == 2 && strcmp(argv[1], "power_off_source") == 0) {
        /* REG21 survives while the TG28 stays supplied: after an unexpected
         * power cut, revive with the PWRON key (never unplug VBUS) and read
         * this before any power cycle to attribute the shutdown. */
        uint8_t source = 0;
        const esp_err_t error = bsp_pmic_get_power_off_source(&source);
        if (error == ESP_OK) {
            printf("power_off_source=0x%02x\n", source);
        }
        return error;
    }
    if (argc == 2 && strcmp(argv[1], "irq_snapshot") == 0) {
        uint8_t status[3] = {0};
        bool valid = false;
        const esp_err_t error = bsp_pmic_get_boot_irq_snapshot(status, &valid);
        if (error == ESP_OK) {
            printf("boot_irq_snapshot=0x%02x 0x%02x 0x%02x (%s)\n",
                   status[0], status[1], status[2],
                   valid ? "captured at boot, pre-clear" : "no snapshot");
        }
        return error;
    }
    if (argc == 2 && strcmp(argv[1], "temperature") == 0) {
        /* EVT1 has no battery NTC: the TS pin is a fixed input. TDIE is the
         * die-temperature sensor voltage (not degC); watch its trend during
         * the 500 mA charge test and monitor the cell externally. */
        static const struct {
            bsp_pmic_adc_channel_t channel;
            const char *label;
        } channels[] = {
            { BSP_PMIC_ADC_VBAT, "vbat" },
            { BSP_PMIC_ADC_VBUS, "vbus" },
            { BSP_PMIC_ADC_VSYS, "vsys" },
            { BSP_PMIC_ADC_TS,   "ts"   },
            { BSP_PMIC_ADC_TDIE, "tdie" },
        };
        for (unsigned index = 0; index < sizeof(channels) / sizeof(channels[0]);
                ++index) {
            uint16_t millivolts = 0;
            const esp_err_t error = bsp_pmic_read_adc_mv(channels[index].channel,
                                                         &millivolts);
            if (error != ESP_OK) {
                printf("%s read failed: %s\n", channels[index].label,
                       esp_err_to_name(error));
                return error;
            }
            printf("%s_mv=%u%s\n", channels[index].label, millivolts,
                   channels[index].channel == BSP_PMIC_ADC_TS ? " (fixed input)" :
                   channels[index].channel == BSP_PMIC_ADC_TDIE ?
                   " (die sensor voltage, not degC)" : "");
        }
        return ESP_OK;
    }
    if (argc >= 2 && strcmp(argv[1], "input_limit") == 0) {
        esp_err_t error = ESP_OK;
        if (argc > 2) {
            char *end = NULL;
            const long milliamps = strtol(argv[2], &end, 10);
            const bool safe_default = argc == 3 && milliamps == 100;
            const bool known_level = milliamps == 500 || milliamps == 900 ||
                                     milliamps == 1000 || milliamps == 1500 ||
                                     milliamps == 2000;
            const bool verified_high = argc == 4 && known_level &&
                                       strcmp(argv[3], "source_verified") == 0;
            if (end == argv[2] || *end != '\0' ||
                    (!safe_default && !verified_high)) {
                printf("usage: pmic input_limit [100 | {500|900|1000|1500|2000} source_verified]\n");
                return ESP_ERR_INVALID_ARG;
            }
            if (verified_high) {
                printf("WARNING: %ld mA requires a verified Type-C1 source, "
                       "current probe, and weak-source voltage check\n",
                       milliamps);
            }
            error = bsp_pmic_set_input_current_limit((uint16_t)milliamps);
        }
        uint16_t actual = 0;
        if (error == ESP_OK) {
            error = bsp_pmic_get_input_current_limit(&actual);
        }
        if (error == ESP_OK) {
            printf("input_limit=%u mA\n", actual);
        }
        return error;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "charge_current") == 0) {
        esp_err_t error = ESP_OK;
        if (argc == 3) {
            char *end = NULL;
            const long milliamps = strtol(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' ||
                    milliamps < 0 || milliamps > UINT16_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            printf("WARNING: changing the battery charge-current limit; "
                   "monitor battery voltage and temperature\n");
            error = bsp_pmic_set_charge_current((uint16_t)milliamps);
            if (error != ESP_OK) {
                printf("valid charge currents: 0-200 mA in 25 mA steps, "
                       "300-1500 mA in 100 mA steps\n");
            }
        }
        uint16_t actual = 0;
        if (error == ESP_OK) {
            error = bsp_pmic_get_charge_current(&actual);
        }
        if (error == ESP_OK) {
            printf("charge_current=%u mA\n", actual);
        }
        return error;
    }
    printf("usage: pmic power_on_source | pmic power_off_source | pmic irq_snapshot | pmic input_limit [100 | {500|900|1000|1500|2000} source_verified] | pmic charge_current [MILLIAMPS] | pmic temperature\n");
    return ESP_ERR_INVALID_ARG;
}

static int command_charge_test(int argc, char **argv)
{
    const bool verified_500 = argc == 2 &&
                              strcmp(argv[1], "source_verified") == 0;
    if (argc > 2 || (argc == 2 && !verified_500)) {
        printf("usage: charge_test [source_verified]\n");
        return ESP_ERR_INVALID_ARG;
    }
    bsp_pmic_status_t status;
    uint16_t input_limit = 0;
    uint16_t charge_voltage = 0;
    esp_err_t error = bsp_pmic_get_status(&status);
    if (error == ESP_OK) {
        error = bsp_pmic_get_input_current_limit(&input_limit);
    }
    if (error == ESP_OK) {
        error = bsp_pmic_get_charge_voltage(&charge_voltage);
    }
    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_CHARGE, error, "TG28_SW charger read failed");
        return error;
    }
    printf("vbus=%s charging=%s done=%s input_limit=%u mA charge_voltage=%u mV "
           "vbat=%u mV battery=%s\n",
           status.vbus_present ? "present" : "absent",
           status.charging ? "yes" : "no", status.charge_done ? "yes" : "no",
           input_limit, charge_voltage, status.battery_mv,
           status.battery_present ? "present" : "absent");

    factory_status_t result;
    char detail[96];
    const bool limit_is_baseline = input_limit == BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA;
    const bool limit_is_verified = verified_500 &&
            (input_limit == 500 || input_limit == 900 || input_limit == 1000 ||
             input_limit == 1500 || input_limit == 2000);
    if (!limit_is_baseline && !limit_is_verified) {
        result = FACTORY_STATUS_FAIL;
        snprintf(detail, sizeof(detail),
                 "unverified input limit=%u mA (expected %u mA)",
                 input_limit, BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA);
    } else if (status.vbus_present &&
               (status.charging || status.charge_done)) {
        result = FACTORY_STATUS_PASS;
        snprintf(detail, sizeof(detail), "%s, limit=%u mA target=%u mV",
                 status.charging ? "charging" : "charge done",
                 input_limit, charge_voltage);
    } else if (status.vbus_present) {
        result = FACTORY_STATUS_WARN;
        snprintf(detail, sizeof(detail),
                 "vbus present but not charging (limit=%u mA target=%u mV)",
                 input_limit, charge_voltage);
    } else {
        result = FACTORY_STATUS_WARN;
        snprintf(detail, sizeof(detail), "no vbus; charger path not exercised");
    }
    factory_report_set(FACTORY_TEST_CHARGE, result, detail);
    factory_report_print_one(FACTORY_TEST_CHARGE);
    return ESP_OK;
}

static int command_rail(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        printf("usage: rail NAME status|on|off [millivolts]\n");
        return ESP_ERR_INVALID_ARG;
    }
    bsp_pmic_regulator_t regulator = BSP_PMIC_REGULATOR_COUNT;
    for (int index = 0; index < BSP_PMIC_REGULATOR_COUNT; ++index) {
        if (strcmp(argv[1], bsp_pmic_regulator_name(index)) == 0) {
            regulator = (bsp_pmic_regulator_t)index;
            break;
        }
    }
    if (regulator == BSP_PMIC_REGULATOR_COUNT) {
        printf("unknown rail: %s\n", argv[1]);
        return ESP_ERR_INVALID_ARG;
    }
    const bool changes_state = strcmp(argv[2], "status") != 0;
    const bool display_owned = factory_display_started() &&
                               (regulator == BSP_PMIC_ALDO1 ||
                                regulator == BSP_PMIC_ALDO2);
    const bool audio_owned = factory_audio_busy() &&
                             regulator == BSP_PMIC_ALDO3;
    if (changes_state && (display_owned || audio_owned)) {
        printf("rail is owned by an active peripheral; stop the peripheral "
               "before raw rail control\n");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = ESP_OK;
    if (strcmp(argv[2], "status") == 0 && argc == 3) {
        uint16_t millivolts = 0;
        bool enabled = false;
        error = bsp_pmic_regulator_get_voltage(regulator, &millivolts);
        if (error == ESP_OK) {
            error = bsp_pmic_regulator_is_enabled(regulator, &enabled);
        }
        if (error == ESP_OK) {
            printf("%s %s %u mV\n", argv[1], enabled ? "enabled" : "disabled",
                   millivolts);
        }
    } else if (strcmp(argv[2], "off") == 0 && argc == 3) {
        error = bsp_pmic_regulator_enable(regulator, false);
    } else if (strcmp(argv[2], "on") == 0 && argc == 4) {
        char *end = NULL;
        const long millivolts = strtol(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' || millivolts < 0 || millivolts > UINT16_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        error = bsp_pmic_regulator_set_voltage(regulator, (uint16_t)millivolts);
        if (error == ESP_OK) {
            error = bsp_pmic_regulator_enable(regulator, true);
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    if (error != ESP_OK) {
        printf("rail operation failed: %s\n", esp_err_to_name(error));
    }
    return error;
}

static int command_peripheral_power(int argc, char **argv)
{
    if (argc < 3 || argc > 4 ||
            (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
        printf("usage: peripheral_power NAME on|off [output_only]\n");
        return ESP_ERR_INVALID_ARG;
    }
    bsp_peripheral_t peripheral = BSP_PERIPHERAL_COUNT;
    for (int index = 0; index < BSP_PERIPHERAL_COUNT; ++index) {
        if (strcmp(argv[1], bsp_peripheral_name(index)) == 0) {
            peripheral = (bsp_peripheral_t)index;
            break;
        }
    }
    if (peripheral == BSP_PERIPHERAL_COUNT) {
        printf("unknown peripheral: %s\n", argv[1]);
        return ESP_ERR_INVALID_ARG;
    }
    const bool enable = strcmp(argv[2], "on") == 0;
    if (peripheral == BSP_PERIPHERAL_EXTERNAL_3V3 && enable) {
        if (argc != 4 || strcmp(argv[3], "output_only") != 0) {
            printf("external_3v3 is output-only: disconnect self-powered "
                   "loads and append output_only\n");
            return ESP_ERR_INVALID_ARG;
        }
        printf("WARNING: EXT pin 2 has no reverse-current blocker; do not "
               "parallel it with an externally powered 3.3 V rail\n");
    } else if (argc != 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (factory_display_started() &&
            (peripheral == BSP_PERIPHERAL_DISPLAY ||
             peripheral == BSP_PERIPHERAL_TOUCH)) {
        if (peripheral == BSP_PERIPHERAL_DISPLAY && !enable) {
            return factory_display_stop();
        }
        printf("display/touch power is owned by the active display; run "
               "peripheral_power display off first\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (factory_audio_busy() && peripheral == BSP_PERIPHERAL_AUDIO) {
        printf("audio power is owned by an active codec; run power_all_off\n");
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_peripheral_power_set(peripheral, enable);
}

/* power_all_off cleanup: log each failed item, keep only the first error. */

esp_err_t factory_power_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "pmic_test", .help = "Read TG28_SW identity, battery, VBUS, and charge state.", .func = command_pmic_test},
        {.command = "pmic", .help = "Read PMIC state or set charge/input limits with explicit safety gates.", .func = command_pmic},
        {.command = "charge_test", .help = "Check charger state: charge_test [source_verified].", .func = command_charge_test},
        {.command = "rail", .help = "Inspect/control an unowned TG28_SW rail.", .func = command_rail},
        {.command = "peripheral_power", .help = "Apply a complete, owner-aware peripheral power sequence.", .func = command_peripheral_power},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
