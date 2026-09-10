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

static esp_err_t print_pmic_early_snapshot(void)
{
    bsp_pmic_early_snapshot_t snapshot = {0};
    const esp_err_t error = bsp_pmic_get_early_snapshot(&snapshot);
    if (error != ESP_OK) {
        printf("early_snapshot read failed: %s\n", esp_err_to_name(error));
        return error;
    }

    printf("early_snapshot valid_mask=0x%02x", snapshot.valid_mask);
    if ((snapshot.valid_mask & BSP_PMIC_EARLY_STATUS_VALID) != 0) {
        printf(" REG00-01=%02x:%02x",
               snapshot.status[0], snapshot.status[1]);
    } else {
        printf(" REG00-01=INVALID");
    }
    if ((snapshot.valid_mask & BSP_PMIC_EARLY_POWER_SOURCE_VALID) != 0) {
        printf(" REG20-21=%02x:%02x",
               snapshot.power_source[0], snapshot.power_source[1]);
    } else {
        printf(" REG20-21=INVALID");
    }
    if ((snapshot.valid_mask & BSP_PMIC_EARLY_ADC_CONTROL_VALID) != 0) {
        printf(" REG30=%02x", snapshot.adc_control);
    } else {
        printf(" REG30=INVALID");
    }
    if ((snapshot.valid_mask & BSP_PMIC_EARLY_IRQ_STATUS_VALID) != 0) {
        printf(" REG48-4A=%02x:%02x:%02x",
               snapshot.irq_status[0], snapshot.irq_status[1],
               snapshot.irq_status[2]);
    } else {
        printf(" REG48-4A=INVALID");
    }
    printf(" (post-create, before BSP writes/gauge reset/IRQ clear)\n");
    return ESP_OK;
}

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
    print_pmic_early_snapshot();
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

static int command_pmic_registers(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "regs") != 0) {
        printf("usage: pmic regs\n");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t values[16] = {0};
    unsigned failed = 0;
    printf("TG28_REGISTER_DUMP_BEGIN\n");
    for (unsigned base = 0; base <= 0xF0; base += 0x10) {
        bool row_errors[16] = {false};
        esp_err_t error = bsp_pmic_read_registers(base, values, sizeof(values));
        if (error != ESP_OK) {
            for (unsigned index = 0; index < sizeof(values); ++index) {
                const uint8_t register_address = (uint8_t)(base + index);
                error = bsp_pmic_read_registers(register_address,
                                                &values[index], 1);
                row_errors[index] = error != ESP_OK;
                if (error != ESP_OK) {
                    ++failed;
                }
            }
        }
        printf("%02x-%02x:", base, (uint8_t)(base + 0x0F));
        for (unsigned index = 0; index < sizeof(values); ++index) {
            if (row_errors[index]) {
                printf(" ERR");
            } else {
                printf(" %02x", values[index]);
            }
        }
        printf("\n");
    }
    printf("TG28_REGISTER_DUMP_END failed=%u\n", failed);
    return failed == 0 ? ESP_OK : ESP_FAIL;
}

static int command_pmic(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "regs") == 0) {
        return command_pmic_registers(argc, argv);
    }
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
    if (argc >= 2 && strcmp(argv[1], "model_dump") == 0) {
        /* Fuel-gauge model area dump: captures the factory ROM default or
         * the programmed/self-learned SRAM model for battery characterization
         * review. Usage: pmic model_dump [rom|sram]  (default sram). */
        const bool from_sram = !(argc == 3 && strcmp(argv[2], "rom") == 0);
        uint8_t model[128] = {0};
        const esp_err_t error = bsp_pmic_read_battery_model(from_sram, model, sizeof(model));
        if (error != ESP_OK) {
            return error;
        }
        printf("battery_model[%s] 128 bytes:\n", from_sram ? "sram" : "rom");
        for (int i = 0; i < (int)sizeof(model); i += 16) {
            printf("%02x:", i);
            for (int j = 0; j < 16; ++j) {
                printf(" %02x", model[i + j]);
            }
            printf("\n");
        }
        return ESP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "irq_snapshot") == 0) {
        uint8_t status[3] = {0};
        bool valid = false;
        const esp_err_t error = bsp_pmic_get_boot_irq_snapshot(status, &valid);
        if (error == ESP_OK) {
            printf("boot_irq_snapshot=0x%02x 0x%02x 0x%02x (%s)\n",
                   status[0], status[1], status[2],
                   valid ? "post-create, pre-config/pre-clear" : "no snapshot");
        }
        return error;
    }
    if (argc == 2 && strcmp(argv[1], "early_snapshot") == 0) {
        return print_pmic_early_snapshot();
    }
    if (argc == 2 && strcmp(argv[1], "temperature") == 0) {
        bsp_pmic_status_t status = {0};
        esp_err_t status_error = bsp_pmic_get_status(&status);
        if (status_error != ESP_OK) {
            printf("PMIC status read failed before ADC diagnostic: %s\n",
                   esp_err_to_name(status_error));
            return status_error;
        }

        bsp_pmic_adc_diagnostic_t diagnostic = {0};
        const esp_err_t diagnostic_error =
            bsp_pmic_run_adc_diagnostic(&diagnostic);
        printf("adc_control REG30 orig=0x%02x enabled_readback=0x%02x "
               "enable_verified=%s restored_readback=0x%02x "
               "restore_verified=%s samples=%u\n",
               diagnostic.reg30_original, diagnostic.reg30_enabled,
               diagnostic.enable_verified ? "yes" : "no",
               diagnostic.reg30_restored,
               diagnostic.restore_verified ? "yes" : "no",
               (unsigned)diagnostic.sample_count);
        bool vbus_stale_seen = false;
        uint32_t vbus_last_stale_ms = 0;
        uint32_t vbus_first_valid_ms = UINT32_MAX;
        for (size_t index = 0; index < diagnostic.sample_count; ++index) {
            const bsp_pmic_adc_diagnostic_sample_t *sample =
                &diagnostic.samples[index];
            printf("adc_sample t=%" PRIu32 "ms raw "
                   "vbat=0x%04x ts=0x%04x vbus=0x%04x "
                   "vsys=0x%04x tdie=0x%04x\n",
                   sample->elapsed_ms,
                   sample->raw[BSP_PMIC_ADC_VBAT],
                   sample->raw[BSP_PMIC_ADC_TS],
                   sample->raw[BSP_PMIC_ADC_VBUS],
                   sample->raw[BSP_PMIC_ADC_VSYS],
                   sample->raw[BSP_PMIC_ADC_TDIE]);
            const uint16_t sample_vbus = sample->raw[BSP_PMIC_ADC_VBUS];
            if (vbus_first_valid_ms == UINT32_MAX) {
                if (sample_vbus == 0) {
                    vbus_stale_seen = true;
                    vbus_last_stale_ms = sample->elapsed_ms;
                } else if (sample_vbus <= 0x2000) {
                    vbus_first_valid_ms = sample->elapsed_ms;
                }
            }
        }
        if (diagnostic_error != ESP_OK) {
            printf("ADC diagnostic failed: %s\n",
                   esp_err_to_name(diagnostic_error));
            return diagnostic_error;
        }
        if (diagnostic.sample_count == 0) {
            printf("ADC diagnostic produced no samples\n");
            return ESP_FAIL;
        }

        bsp_pmic_status_t final_status = {0};
        status_error = bsp_pmic_get_status(&final_status);
        if (status_error != ESP_OK) {
            printf("PMIC status read failed after ADC diagnostic: %s\n",
                   esp_err_to_name(status_error));
            return status_error;
        }
        if (status.vbus_present != final_status.vbus_present) {
            printf("vbus_presence_changed_during_adc=%s->%s\n",
                   status.vbus_present ? "present" : "absent",
                   final_status.vbus_present ? "present" : "absent");
        }
        if (final_status.vbus_present && vbus_stale_seen &&
                vbus_first_valid_ms != UINT32_MAX) {
            printf("vbus_adc_settle=stale_through_%" PRIu32
                   "ms first_valid_sample=%" PRIu32
                   "ms; use >=%" PRIu32 "ms after REG30 enable\n",
                   vbus_last_stale_ms, vbus_first_valid_ms,
                   vbus_first_valid_ms);
        }

        const bsp_pmic_adc_diagnostic_sample_t *final =
            &diagnostic.samples[diagnostic.sample_count - 1];
        const uint16_t vbat_raw = final->raw[BSP_PMIC_ADC_VBAT];
        const uint16_t ts_raw = final->raw[BSP_PMIC_ADC_TS];
        const uint16_t vbus_raw = final->raw[BSP_PMIC_ADC_VBUS];
        const uint16_t vsys_raw = final->raw[BSP_PMIC_ADC_VSYS];
        const uint16_t tdie_raw = final->raw[BSP_PMIC_ADC_TDIE];

        printf("adc_final t=%" PRIu32 "ms vbat=%u mV vbus=%u mV "
               "vsys=%u mV\n",
               final->elapsed_ms, vbat_raw, vbus_raw, vsys_raw);
        if (final_status.vbus_present && vbus_raw == 0) {
            printf("vbus_adc=INVALID/STALE (REG00 says VBUS present, raw=0)\n");
        } else {
            printf("vbus_adc=%s (REG00 VBUS=%s, raw=0x%04x)\n",
                   vbus_raw > 0x2000 ? "INVALID/OVERFLOW" : "VALID",
                   final_status.vbus_present ? "present" : "absent",
                   vbus_raw);
        }

        /* EVT1 deliberately has no battery NTC and fixes TS to ground.
         * TG28 FAQ 2.12 says raw > 0x2000 is the negative-offset/overflow
         * signature (often near 0x3fff), not a large positive voltage. */
        if (ts_raw > 0x2000) {
            printf("ts_adc=INVALID (EVT1 fixed-GND input; "
                   "negative-offset/overflow raw=0x%04x)\n", ts_raw);
        } else {
            printf("ts_adc=INVALID (EVT1 fixed-GND input, no battery NTC; "
                   "raw=0x%04x)\n", ts_raw);
        }

        if (tdie_raw == 0) {
            printf("tdie_adc=INVALID/STALE (raw=0)\n");
        } else if (tdie_raw > 0x2000) {
            printf("tdie_adc=INVALID/OVERFLOW (raw=0x%04x)\n", tdie_raw);
        } else {
            /* TG28 FAQ V1.0 section 2.8: T = 22 + (7274 - ADC) / 20.
             * Keep 0.05 C/count exactly by calculating centi-degrees. */
            const int32_t centi_c = 2200 +
                (7274 - (int32_t)tdie_raw) * 5;
            const uint32_t magnitude = centi_c < 0 ?
                (uint32_t)(-centi_c) : (uint32_t)centi_c;
            printf("tdie_c=%c%" PRIu32 ".%02" PRIu32
                   " (raw=0x%04x; TG28 FAQ formula)\n",
                   centi_c < 0 ? '-' : '+', magnitude / 100,
                   magnitude % 100, tdie_raw);
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
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "vindpm") == 0) {
        esp_err_t error = ESP_OK;
        if (argc == 3) {
            char *end = NULL;
            const long millivolts = strtol(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' ||
                    millivolts < 0 || millivolts > UINT16_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            printf("WARNING: changing VINDPM changes the TG28 input-voltage "
                   "regulation threshold\n");
            error = bsp_pmic_set_vindpm((uint16_t)millivolts);
            if (error != ESP_OK) {
                printf("valid VINDPM values: 3880-5080 mV in 80 mV steps\n");
                return error;
            }
        }
        uint16_t actual = 0;
        if (error == ESP_OK) {
            error = bsp_pmic_get_vindpm(&actual);
        }
        if (error == ESP_OK) {
            const uint8_t code = (uint8_t)((actual - 3880) / 80);
            printf("vindpm=%u mV (REG15[3:0]=0x%02x)\n", actual, code);
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
    printf("usage: pmic regs | pmic power_on_source | pmic power_off_source | pmic early_snapshot | pmic irq_snapshot | pmic model_dump [rom|sram] | pmic input_limit [100 | {500|900|1000|1500|2000} source_verified] | pmic vindpm [MILLIVOLTS] | pmic charge_current [MILLIAMPS] | pmic temperature\n");
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
                 "input limit=%u mA without source_verified (expected %u mA)",
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

static bool rail_uses_otp_switch(bsp_pmic_regulator_t regulator,
                                 bsp_pmic_switch_t *sw)
{
    if (regulator == BSP_PMIC_DLDO1) {
        *sw = BSP_PMIC_SWITCH_DC1SW;
        return true;
    }
    if (regulator == BSP_PMIC_DLDO2) {
        *sw = BSP_PMIC_SWITCH_DC4SW;
        return true;
    }
    return false;
}

static esp_err_t remember_first_error(esp_err_t first_error, esp_err_t error)
{
    return first_error == ESP_OK ? error : first_error;
}

static esp_err_t rail_dump_all(void)
{
    esp_err_t first_error = ESP_OK;
    printf("RAIL_DUMP_BEGIN\n");
    for (int index = 0; index < BSP_PMIC_REGULATOR_COUNT; ++index) {
        const bsp_pmic_regulator_t regulator = (bsp_pmic_regulator_t)index;
        bsp_pmic_switch_t unused_switch = BSP_PMIC_SWITCH_COUNT;
        if (rail_uses_otp_switch(regulator, &unused_switch)) {
            continue;
        }

        bool enabled = false;
        uint16_t millivolts = 0;
        const esp_err_t enable_error =
            bsp_pmic_regulator_is_enabled(regulator, &enabled);
        const esp_err_t voltage_error =
            bsp_pmic_regulator_get_voltage(regulator, &millivolts);
        if (enable_error != ESP_OK) {
            first_error = remember_first_error(first_error, enable_error);
        }
        if (voltage_error != ESP_OK) {
            first_error = remember_first_error(first_error, voltage_error);
        }

        printf("RAIL name=%s kind=regulator enabled=",
               bsp_pmic_regulator_name(regulator));
        if (enable_error == ESP_OK) {
            printf("%s", enabled ? "yes" : "no");
        } else {
            printf("error");
        }
        printf(" programmed_mv=");
        if (voltage_error == ESP_OK) {
            printf("%u", (unsigned)millivolts);
        } else {
            printf("error");
        }
        printf(" status=%s", enable_error == ESP_OK && voltage_error == ESP_OK ?
               "ok" : "error");
        if (enable_error != ESP_OK) {
            printf(" enable_error=%s", esp_err_to_name(enable_error));
        }
        if (voltage_error != ESP_OK) {
            printf(" voltage_error=%s", esp_err_to_name(voltage_error));
        }
        printf("\n");
    }

    static const struct {
        bsp_pmic_switch_t sw;
        const char *pin_name;
    } switches[] = {
        { BSP_PMIC_SWITCH_DC1SW, "dldo1" },
        { BSP_PMIC_SWITCH_DC4SW, "dldo2" },
    };
    for (size_t index = 0; index < sizeof(switches) / sizeof(switches[0]);
            ++index) {
        bool enabled = false;
        const esp_err_t error =
            bsp_pmic_switch_is_enabled(switches[index].sw, &enabled);
        if (error != ESP_OK) {
            first_error = remember_first_error(first_error, error);
        }
        printf("RAIL name=%s kind=otp_switch alias=%s state=%s "
               "programmed_mv=n/a status=%s",
               switches[index].pin_name,
               bsp_pmic_switch_name(switches[index].sw),
               error == ESP_OK ? (enabled ? "closed" : "open") : "error",
               error == ESP_OK ? "ok" : "error");
        if (error != ESP_OK) {
            printf(" state_error=%s", esp_err_to_name(error));
        }
        printf("\n");
    }

    printf("RAIL_DUMP_END result=%s\n", esp_err_to_name(first_error));
    printf("rail dump note: programmed_mv is the TG28 register setting, not "
           "a measured voltage; verify rails with a meter or oscilloscope\n");
    return first_error;
}

static int command_rail(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "dump") == 0) {
        return rail_dump_all();
    }
    if (argc < 3 || argc > 4) {
        printf("usage: rail dump | rail NAME status|on|off [millivolts]\n");
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
    bsp_pmic_switch_t otp_switch = BSP_PMIC_SWITCH_COUNT;
    if (rail_uses_otp_switch(regulator, &otp_switch)) {
        if (argc != 3 || strcmp(argv[2], "status") != 0) {
            printf("%s is OTP-configured as %s, not an adjustable LDO; "
                   "rail supports status only\n",
                   argv[1], bsp_pmic_switch_name(otp_switch));
            return ESP_ERR_INVALID_ARG;
        }
        bool enabled = false;
        const esp_err_t error = bsp_pmic_switch_is_enabled(otp_switch, &enabled);
        if (error == ESP_OK) {
            printf("name=%s kind=otp_switch alias=%s state=%s "
                   "programmed_mv=n/a\n",
                   argv[1], bsp_pmic_switch_name(otp_switch),
                   enabled ? "closed" : "open");
        } else {
            printf("rail status failed: %s\n", esp_err_to_name(error));
        }
        return error;
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
            printf("name=%s kind=regulator enabled=%s programmed_mv=%u\n",
                   argv[1], enabled ? "yes" : "no", millivolts);
            printf("rail status note: programmed_mv is not a measured voltage\n");
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
        {.command = "rail", .help = "Dump all TG28_SW rails or inspect/control one unowned rail.", .func = command_rail},
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
