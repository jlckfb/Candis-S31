/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fusb303b.h"
#include "led_convert.h"
#include "linux/videodev2.h"
#include "esp_video_ioctl.h"
#include "usb/usb_host.h"

#include "factory_console.h"
#include "factory_peripherals.h"
#include "factory_report.h"

#define TOUCH_TEST_SECONDS        15
/* Follow the BSP instead of pinning a rate here: the default must stay a row of
 * the es8389 driver's coeff_div[] table, and the BSP owns that decision. */
#define AUDIO_SAMPLE_RATE         BSP_I2S_SAMPLE_RATE
#define AUDIO_FRAME_COUNT         512
#define OPERATOR_PROMPT_TIMEOUT_S 30
#define BUTTON_TEST_TIMEOUT_S     20
#define CAMERA_CAPTURE_FRAMES     5
#define CAMERA_BUFFER_COUNT       2
#define CAMERA_DQBUF_TIMEOUT_MS   3000
#define RTC_ALARM_POLL_MS         200

/* EVT1 schematic SW2: the KEY_BOOT net drives GPIO61 and has an external
 * 10k pull-up, so the line idles high and is grounded while pressed. */
#define BOOT_BUTTON_GPIO GPIO_NUM_61

/* TG28_SW INT_STATUS1 low nibble latches the power-key edge/press flags. */
#define TG28_POWER_KEY_IRQ_MASK 0x0f

static lv_display_t *s_display;
static led_indicator_handle_t s_led;
static esp_codec_dev_handle_t s_speaker;
static esp_codec_dev_handle_t s_microphone;

static void report_error(factory_test_id_t test, esp_err_t error, const char *action)
{
    char detail[96];
    snprintf(detail, sizeof(detail), "%s: %s", action, esp_err_to_name(error));
    factory_report_set(test, FACTORY_STATUS_FAIL, detail);
    factory_report_print_one(test);
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
        report_error(FACTORY_TEST_PMIC, error, "TG28_SW status read failed");
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
            const bool verified_500 = argc == 4 && milliamps == 500 &&
                                      strcmp(argv[3], "source_verified") == 0;
            if (end == argv[2] || *end != '\0' ||
                    (!safe_default && !verified_500)) {
                printf("usage: pmic input_limit [100 | 500 source_verified]\n");
                return ESP_ERR_INVALID_ARG;
            }
            if (verified_500) {
                printf("WARNING: 500 mA requires a verified Type-C1 source, "
                       "current probe, and weak-source voltage check\n");
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
    printf("usage: pmic power_on_source | pmic input_limit [100 | 500 source_verified] | pmic charge_current [MILLIAMPS] | pmic temperature\n");
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
        report_error(FACTORY_TEST_CHARGE, error, "TG28_SW charger read failed");
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
    if (input_limit != BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA &&
            !(verified_500 && input_limit == 500)) {
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
    const bool display_owned = s_display != NULL &&
                               (regulator == BSP_PMIC_ALDO1 ||
                                regulator == BSP_PMIC_ALDO2);
    const bool audio_owned = (s_speaker != NULL || s_microphone != NULL) &&
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
    if (s_display != NULL &&
            (peripheral == BSP_PERIPHERAL_DISPLAY ||
             peripheral == BSP_PERIPHERAL_TOUCH)) {
        if (peripheral == BSP_PERIPHERAL_DISPLAY && !enable) {
            const esp_err_t error = bsp_display_stop();
            if (error == ESP_OK) {
                s_display = NULL;
            }
            return error;
        }
        printf("display/touch power is owned by the active display; run "
               "peripheral_power display off first\n");
        return ESP_ERR_INVALID_STATE;
    }
    if ((s_speaker != NULL || s_microphone != NULL) &&
            peripheral == BSP_PERIPHERAL_AUDIO) {
        printf("audio power is owned by an active codec; run power_all_off\n");
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_peripheral_power_set(peripheral, enable);
}

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
    if (s_display != NULL) {
        const esp_err_t error = bsp_display_stop();
        first_error = power_off_note("display+touch stop", error, first_error);
        if (error == ESP_OK) {
            s_display = NULL;
        }
    }
    if (s_led != NULL) {
        const esp_err_t error = led_indicator_delete(s_led);
        first_error = power_off_note("LED delete", error, first_error);
        if (error == ESP_OK) {
            s_led = NULL;
        }
    }
    if (s_speaker != NULL) {
        const esp_err_t error = bsp_audio_codec_deinit(s_speaker);
        first_error = power_off_note("speaker codec release", error, first_error);
        if (error == ESP_OK) {
            s_speaker = NULL;
        }
    }
    if (s_microphone != NULL) {
        const esp_err_t error = bsp_audio_codec_deinit(s_microphone);
        first_error = power_off_note("microphone codec release", error, first_error);
        if (error == ESP_OK) {
            s_microphone = NULL;
        }
    }
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

static int command_rtc_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_rtc_time_t time;
    bsp_rtc_status_t status;
    const esp_err_t error = bsp_rtc_get_time(&time, &status);
    if (error != ESP_OK) {
        report_error(FACTORY_TEST_RTC, error, "RX8130CE read failed");
        return error;
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "%04u-%02u-%02u %02u:%02u:%02u valid=%s flags=0x%02x",
             time.year, time.month, time.day, time.hour, time.minute, time.second,
             status.time_valid ? "yes" : "no", status.raw);
    factory_report_set(FACTORY_TEST_RTC,
                       status.time_valid ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_RTC);
    return status.time_valid ? ESP_OK : ESP_FAIL;
}

static bool parse_rtc_time(int argc, char **argv, bsp_rtc_time_t *time)
{
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    char trailing = '\0';

    if (argc != 4 ||
            sscanf(argv[1], "%u-%u-%u%c", &year, &month, &day, &trailing) != 3 ||
            sscanf(argv[2], "%u:%u:%u%c", &hour, &minute, &second, &trailing) != 3) {
        return false;
    }

    char *end = NULL;
    const long weekday = strtol(argv[3], &end, 10);
    /* Mirror the RX8130CE driver's rx8130ce_time_is_valid() rules so an
     * out-of-range value is rejected before it reaches the chip. */
    if (end == argv[3] || *end != '\0' || weekday < 0 || weekday > 6 ||
            year < 2000 || year > 2099 || month < 1 || month > 12 ||
            hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    static const uint8_t days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    unsigned days = days_per_month[month - 1];
    if (month == 2 && (year % 4) == 0) {
        ++days;
    }
    if (day < 1 || day > days) {
        return false;
    }

    *time = (bsp_rtc_time_t) {
        .year = (uint16_t)year,
        .month = (uint8_t)month,
        .day = (uint8_t)day,
        .weekday = (uint8_t)weekday,
        .hour = (uint8_t)hour,
        .minute = (uint8_t)minute,
        .second = (uint8_t)second,
    };
    return true;
}

static bool rtc_time_matches(const bsp_rtc_time_t *expected,
                             const bsp_rtc_time_t *actual)
{
    return expected->year == actual->year &&
           expected->month == actual->month &&
           expected->day == actual->day &&
           expected->weekday == actual->weekday &&
           expected->hour == actual->hour &&
           expected->minute == actual->minute &&
           expected->second == actual->second;
}

static int command_rtc_set(int argc, char **argv)
{
    bsp_rtc_time_t requested;
    if (!parse_rtc_time(argc, argv, &requested)) {
        printf("usage: rtc_set YYYY-MM-DD HH:MM:SS WEEKDAY\n");
        printf("WEEKDAY uses 0=Sunday through 6=Saturday\n");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = bsp_rtc_set_time(&requested);
    bsp_rtc_time_t actual = {0};
    bsp_rtc_status_t status = {0};
    if (error == ESP_OK) {
        error = bsp_rtc_get_time(&actual, &status);
    }

    const bool passed = error == ESP_OK && status.time_valid &&
                        rtc_time_matches(&requested, &actual);
    char detail[96];
    if (error == ESP_OK) {
        snprintf(detail, sizeof(detail),
                 "readback=%04u-%02u-%02u %02u:%02u:%02u weekday=%u valid=%s",
                 actual.year, actual.month, actual.day, actual.hour, actual.minute,
                 actual.second, actual.weekday, status.time_valid ? "yes" : "no");
    } else {
        snprintf(detail, sizeof(detail), "RTC set/readback failed: %s",
                 esp_err_to_name(error));
    }
    factory_report_set(FACTORY_TEST_RTC,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_RTC);
    return passed ? ESP_OK : (error != ESP_OK ? error : ESP_FAIL);
}

static int command_irq_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_shared_irq_status_t status = {0};
    const esp_err_t error = bsp_shared_irq_service(&status);
    const bool passed = error == ESP_OK && status.line_released;
    char detail[96];
    snprintf(detail, sizeof(detail),
             "passes=%u released=%s pmic=%02x:%02x:%02x rtc=0x%02x result=%s",
             status.service_passes, status.line_released ? "yes" : "no",
             status.pmic[0], status.pmic[1], status.pmic[2], status.rtc,
             esp_err_to_name(error));
    factory_report_set(FACTORY_TEST_SHARED_IRQ,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_SHARED_IRQ);
    return passed ? ESP_OK : (error != ESP_OK ? error : ESP_FAIL);
}

static void rtc_alarm_shared_irq(void *arg)
{
    *(volatile bool *)arg = true;
}

static int command_rtc_alarm_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_rtc_time_t now;
    bsp_rtc_status_t status;
    esp_err_t error = bsp_rtc_get_time(&now, &status);
    if (error != ESP_OK) {
        report_error(FACTORY_TEST_RTC_ALARM, error, "RX8130CE read failed");
        return error;
    }

    /* The RX8130CE compares minute/hour/day fields only; the fastest
     * self-contained check targets the next minute boundary. */
    bsp_rtc_alarm_t alarm = {0};
    alarm.minute_en = true;
    alarm.minute = (uint8_t)((now.minute + 1) % 60);

    volatile bool irq_seen = false;
    uint8_t cleared = 0;
    bsp_shared_irq_status_t serviced;
    /* Drain stale PMIC/RTC flags so a fresh alarm can assert the line. */
    error = bsp_shared_irq_service(&serviced);
    if (error == ESP_OK) {
        error = bsp_shared_irq_register_callback(rtc_alarm_shared_irq,
                                                 (void *)&irq_seen);
    }
    if (error == ESP_OK) {
        error = bsp_rtc_set_alarm(&alarm);
    }
    if (error == ESP_OK) {
        error = bsp_rtc_alarm_irq_enable(true);
    }

    bool fired = false;
    unsigned wait_s = 0;
    if (error == ESP_OK) {
        /* Re-read the clock: some seconds may have passed since "now". */
        if (bsp_rtc_get_time(&now, &status) != ESP_OK) {
            now.second = 0;
        }
        wait_s = 60u - now.second + 8u;
        printf("Waiting up to %u s for the alarm at minute=%02u\n",
               wait_s, alarm.minute);
        const int64_t deadline = esp_timer_get_time() + (int64_t)wait_s * 1000000;
        while (esp_timer_get_time() < deadline) {
            if (bsp_rtc_get_status(&status) == ESP_OK && status.alarm) {
                fired = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(RTC_ALARM_POLL_MS));
        }
    }

    const bsp_rtc_alarm_t disarm = {0};
    bsp_rtc_alarm_irq_enable(false);
    bsp_rtc_set_alarm(&disarm);
    bsp_rtc_clear_interrupt_flags(&cleared);
    bsp_shared_irq_register_callback(NULL, NULL);

    if (error != ESP_OK) {
        report_error(FACTORY_TEST_RTC_ALARM, error, "RX8130CE alarm setup failed");
        return error;
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "minute=%02u %s shared_irq_notified=%s",
             alarm.minute, fired ? "fired" : "no flag within timeout",
             irq_seen ? "yes" : "no");
    factory_report_set(FACTORY_TEST_RTC_ALARM,
                       fired ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_RTC_ALARM);
    return fired ? ESP_OK : ESP_FAIL;
}

static bool wait_button_pulse(int gpio, unsigned timeout_s)
{
    /* Polarity-independent: a press is the non-idle level held for at least
     * 30 ms, followed by a release back to idle before the deadline. A key
     * still held at the deadline is not a pulse. */
    const int idle = gpio_get_level(gpio);
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    while (esp_timer_get_time() < deadline) {
        if (gpio_get_level(gpio) == idle) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const int64_t press_start = esp_timer_get_time();
        bool held_30ms = false;
        bool released = false;
        while (esp_timer_get_time() < deadline) {
            if (gpio_get_level(gpio) == idle) {
                released = true;
                break;
            }
            if (esp_timer_get_time() - press_start >= 30000) {
                held_30ms = true;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (released) {
            if (held_30ms) {
                return true;
            }
            /* Too short to count as a press: keep waiting for a real one. */
            continue;
        }
        return false;
    }
    return false;
}

static int command_buttons_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const gpio_config_t boot = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, /* external 10k pull-up on EVT1 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&boot);
    if (error != ESP_OK) {
        report_error(FACTORY_TEST_BUTTONS, error, "BOOT key GPIO setup failed");
        return error;
    }

    printf("Press and release the BOOT key within %u s\n", BUTTON_TEST_TIMEOUT_S);
    const bool boot_seen = wait_button_pulse(BOOT_BUTTON_GPIO, BUTTON_TEST_TIMEOUT_S);
    printf("BOOT key %s\n", boot_seen ? "detected" : "not detected");

    bool power_seen = false;
    uint8_t pmic_irq[3] = {0};
    if (error == ESP_OK) {
        printf("Short-press the PWR key within %u s; "
               "a long press powers the board off\n", BUTTON_TEST_TIMEOUT_S);
        error = bsp_pmic_get_and_clear_interrupts(pmic_irq);
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)BUTTON_TEST_TIMEOUT_S * 1000000;
    while (error == ESP_OK && !power_seen && esp_timer_get_time() < deadline) {
        error = bsp_pmic_get_and_clear_interrupts(pmic_irq);
        if (error == ESP_OK && (pmic_irq[0] & TG28_POWER_KEY_IRQ_MASK) != 0) {
            power_seen = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    printf("PWR key %s\n", power_seen ? "detected" : "not detected");

    if (error != ESP_OK) {
        report_error(FACTORY_TEST_BUTTONS, error, "TG28_SW interrupt read failed");
        return error;
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "boot=%s power=%s",
             boot_seen ? "yes" : "no", power_seen ? "yes" : "no");
    const bool passed = boot_seen && power_seen;
    factory_report_set(FACTORY_TEST_BUTTONS,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_BUTTONS);
    return passed ? ESP_OK : ESP_FAIL;
}

static int command_type_c_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bsp_type_c_status_t status;
    const esp_err_t error = bsp_type_c_get_status(&status, true);
    if (error != ESP_OK) {
        report_error(FACTORY_TEST_TYPE_C, error, "FUSB303B read failed");
        return error;
    }
    printf("addr=0x%02x id=0x%02x type=0x%02x attached=%s vbus=%s "
           "safe0v=%s fault=%s remedy=%s orientation=%u role=%u "
           "peer_current=%u\n",
           status.i2c_address, status.device_id, status.device_type,
           status.attached ? "yes" : "no", status.vbus_ok ? "yes" : "no",
           status.vbus_safe_0v ? "yes" : "no",
           status.fault ? "yes" : "no",
           status.remedy_active ? "yes" : "no", status.orientation,
           (unsigned)status.role, (unsigned)status.advertised_current);

    factory_status_t result = FACTORY_STATUS_PASS;
    const char *verdict;
    if (status.device_type != FUSB303B_DEVICE_TYPE_VALUE) {
        result = FACTORY_STATUS_FAIL;
        verdict = "FUSB303B identity mismatch";
    } else if (status.role != BSP_TYPE_C_ROLE_DRP) {
        result = FACTORY_STATUS_FAIL;
        verdict = "FUSB303B is not in DRP role";
    } else if (status.fault || status.remedy_active) {
        result = FACTORY_STATUS_FAIL;
        verdict = status.fault ? "CC fault active" : "remedy state active";
    } else if (status.attached &&
               (status.orientation == 1 || status.orientation == 2)) {
        verdict = status.vbus_ok ? "cable attached, vbus ok"
                  : "cable attached, no vbus";
    } else if (!status.attached && status.orientation == 0) {
        verdict = "no cable attached";
    } else {
        result = FACTORY_STATUS_WARN;
        verdict = status.attached ? "attached but orientation unknown"
                  : "detached but orientation not cleared";
    }

    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "%s (type=0x%02x role=%u peer_current=%u fault=%u remedy=%u)",
             verdict, status.device_type, (unsigned)status.role,
             (unsigned)status.advertised_current, (unsigned)status.fault,
             (unsigned)status.remedy_active);
    factory_report_set(FACTORY_TEST_TYPE_C, result, detail);
    factory_report_print_one(FACTORY_TEST_TYPE_C);
    return result == FACTORY_STATUS_FAIL ? ESP_FAIL : ESP_OK;
}

static int command_otg(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        return bsp_usb_otg_power_set(false, BSP_TYPE_C_CURRENT_DEFAULT);
    }
    if (argc != 2 || strcmp(argv[1], "on") != 0) {
        printf("usage: otg on | otg off\n");
        return ESP_ERR_INVALID_ARG;
    }
    printf("WARNING: enabling the USB OTG boost rail; verify VBUS before connecting a load\n");
    /* Type-C2 only advertises the USB 500 mA default; high-current source
     * requests are rejected by the BSP. */
    return bsp_usb_otg_power_set(true, BSP_TYPE_C_CURRENT_DEFAULT);
}

#define USB_HOST_ENUM_TIMEOUT_S 20

static void usb_host_test_event_cb(const usb_host_client_event_msg_t *message,
                                   void *arg)
{
    if (message->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        *(volatile uint8_t *)arg = message->new_dev.address;
    }
}

/* Best-effort UTF-16LE descriptor to printable ASCII for the log line. */
static void usb_string_to_ascii(const usb_str_desc_t *descriptor, char *out,
                                size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (descriptor == NULL || descriptor->bLength < 2 ||
            (descriptor->bLength & 1U) != 0) {
        return;
    }
    const size_t chars = (descriptor->bLength - 2U) / 2U;
    size_t used = 0;
    for (size_t index = 0; index < chars && used + 1 < out_size; ++index) {
        const uint16_t code = descriptor->wData[index];
        out[used++] = code >= 0x20 && code < 0x7f ? (char)code : '?';
    }
    out[used] = '\0';
}

static int command_usb_host_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Host start also arms the Type-C2 5 V boost at the 500 mA default
     * advertisement; this board never advertises 1.5 A/3 A. Enumeration
     * below performs real control transfers on EP0, so a PASS is data-path
     * evidence, not just "5 V present". */
    esp_err_t error = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (error != ESP_OK) {
        report_error(FACTORY_TEST_USB_HOST, error, "USB Host start failed");
        return error;
    }

    volatile uint8_t new_address = 0;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = usb_host_test_event_cb,
            .callback_arg = (void *)&new_address,
        },
    };
    usb_host_client_handle_t client = NULL;
    if (error == ESP_OK) {
        error = usb_host_client_register(&client_config, &client);
    }

    printf("Attach a USB device to Type-C2 within %u s (500 mA budget)\n",
           USB_HOST_ENUM_TIMEOUT_S);
    usb_device_handle_t device = NULL;
    usb_device_info_t info = {0};
    const usb_device_desc_t *descriptor = NULL;
    const int64_t deadline = esp_timer_get_time() +
                             (int64_t)USB_HOST_ENUM_TIMEOUT_S * 1000000;
    while (error == ESP_OK && new_address == 0 &&
            esp_timer_get_time() < deadline) {
        const esp_err_t event_error = usb_host_client_handle_events(
                                          client, pdMS_TO_TICKS(200));
        if (event_error != ESP_OK && event_error != ESP_ERR_TIMEOUT) {
            error = event_error;
        }
    }
    if (error == ESP_OK && new_address != 0) {
        error = usb_host_device_open(client, new_address, &device);
    }
    if (error == ESP_OK && device != NULL) {
        error = usb_host_device_info(device, &info);
    }
    if (error == ESP_OK && device != NULL) {
        error = usb_host_get_device_descriptor(device, &descriptor);
    }

    char manufacturer[24] = {0};
    char product[24] = {0};
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    unsigned device_speed = 0;
    bool enumerated = false;
    if (error == ESP_OK && descriptor != NULL) {
        usb_string_to_ascii(info.str_desc_manufacturer, manufacturer,
                            sizeof(manufacturer));
        usb_string_to_ascii(info.str_desc_product, product, sizeof(product));
        vendor_id = descriptor->idVendor;
        product_id = descriptor->idProduct;
        device_speed = (unsigned)info.speed;
        enumerated = true;
        printf("addr=%u vid=0x%04x pid=0x%04x speed=%u config=%u "
               "manufacturer=\"%s\" product=\"%s\"\n",
               info.dev_addr, vendor_id, product_id, device_speed,
               (unsigned)info.bConfigurationValue, manufacturer, product);
    }

    /* Full teardown on every path: close the device, deregister the client,
     * then stop the stack and the 5 V boost. */
    if (device != NULL) {
        const esp_err_t close_error = usb_host_device_close(client, device);
        if (error == ESP_OK && close_error != ESP_OK) {
            error = close_error;
        }
    }
    if (client != NULL) {
        const esp_err_t dereg_error = usb_host_client_deregister(client);
        if (error == ESP_OK && dereg_error != ESP_OK) {
            error = dereg_error;
        }
    }
    const esp_err_t stop_error = bsp_usb_host_stop();
    if (error == ESP_OK && stop_error != ESP_OK) {
        error = stop_error;
    }

    if (error != ESP_OK) {
        report_error(FACTORY_TEST_USB_HOST, error, "USB Host enumeration failed");
        return error;
    }
    if (!enumerated) {
        factory_report_set(FACTORY_TEST_USB_HOST, FACTORY_STATUS_WARN,
                           "no device attached within timeout");
        factory_report_print_one(FACTORY_TEST_USB_HOST);
        return ESP_ERR_NOT_FOUND;
    }
    char detail[FACTORY_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail), "vid=0x%04x pid=0x%04x speed=%u",
             vendor_id, product_id, device_speed);
    factory_report_set(FACTORY_TEST_USB_HOST, FACTORY_STATUS_PASS, detail);
    factory_report_print_one(FACTORY_TEST_USB_HOST);
    return ESP_OK;
}

static void create_display_pattern(void)
{
    static const lv_color_t colors[] = {
        LV_COLOR_MAKE(255, 0, 0), LV_COLOR_MAKE(0, 255, 0),
        LV_COLOR_MAKE(0, 0, 255), LV_COLOR_MAKE(255, 255, 255),
    };
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    for (unsigned index = 0; index < 4; ++index) {
        lv_obj_t *bar = lv_obj_create(screen);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, BSP_LCD_H_RES / 2, BSP_LCD_V_RES / 2);
        lv_obj_set_pos(bar, (index & 1U) ? BSP_LCD_H_RES / 2 : 0,
                       (index & 2U) ? BSP_LCD_V_RES / 2 : 0);
        lv_obj_set_style_bg_color(bar, colors[index], LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    }
}

static esp_err_t ensure_display_started(void)
{
    if (s_display == NULL) {
        s_display = bsp_display_start();
    }
    return s_display != NULL ? ESP_OK : ESP_FAIL;
}

static esp_err_t show_display_pattern(void)
{
    if (ensure_display_started() != ESP_OK) {
        return ESP_FAIL;
    }
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    create_display_pattern();
    bsp_display_unlock();
    return ESP_OK;
}

static void record_operator_verdict(factory_test_id_t test, char answer,
                                    const char *pass_detail,
                                    const char *fail_detail,
                                    const char *pending_detail)
{
    if (answer == 'y') {
        factory_report_set(test, FACTORY_STATUS_PASS, pass_detail);
    } else if (answer == 'n') {
        factory_report_set(test, FACTORY_STATUS_FAIL, fail_detail);
    } else {
        factory_report_set(test, FACTORY_STATUS_NOT_RUN, pending_detail);
    }
    factory_report_print_one(test);
}

static int command_display_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const esp_err_t error = show_display_pattern();
    if (error != ESP_OK) {
        report_error(FACTORY_TEST_DISPLAY, error, "display start/lock failed");
        return error;
    }
    const char answer = factory_console_ask_operator(
                            "display", "Four-color pattern visible and correct?",
                            OPERATOR_PROMPT_TIMEOUT_S);
    record_operator_verdict(FACTORY_TEST_DISPLAY, answer,
                            "operator confirmed color pattern",
                            "operator rejected color pattern",
                            "color pattern active; inspect panel then use mark");
    return ESP_OK;
}

static int command_display_brightness(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: display_brightness PERCENT\n");
        return ESP_ERR_INVALID_ARG;
    }
    char *end = NULL;
    const long brightness = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || brightness < 0 || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_display_started(), "factory_display",
                        "display initialization failed");
    return bsp_display_brightness_set((int)brightness);
}

static int command_display_sleep(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "deep") != 0)) {
        printf("usage: display_sleep [deep]\n");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_display_started(), "factory_display",
                        "display initialization failed");
    return argc == 2 ? bsp_display_enter_deep_standby() :
           bsp_display_enter_sleep();
}

static int command_display_wake(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "deep") != 0)) {
        printf("usage: display_wake [deep]\n");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_display_started(), "factory_display",
                        "display initialization failed");
    return argc == 2 ? bsp_display_exit_deep_standby() :
           bsp_display_exit_sleep();
}

static int command_display_sleep_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (ensure_display_started() != ESP_OK) {
        report_error(FACTORY_TEST_DISPLAY_SLEEP, ESP_FAIL,
                     "display initialization failed");
        return ESP_FAIL;
    }

    static const struct {
        esp_err_t (*enter)(void);
        esp_err_t (*leave)(void);
        const char *name;
    } stages[] = {
        { bsp_display_enter_sleep, bsp_display_exit_sleep, "sleep" },
        { bsp_display_enter_deep_standby, bsp_display_exit_deep_standby,
          "deep standby" },
    };

    unsigned confirmed = 0;
    for (unsigned index = 0; index < sizeof(stages) / sizeof(stages[0]); ++index) {
        esp_err_t error = show_display_pattern();
        if (error == ESP_OK) {
            error = stages[index].enter();
        }
        if (error != ESP_OK) {
            report_error(FACTORY_TEST_DISPLAY_SLEEP, error,
                         stages[index].name);
            return error;
        }

        char question[96];
        snprintf(question, sizeof(question), "%s: did the panel go dark?",
                 stages[index].name);
        char answer = factory_console_ask_operator("display_sleep", question,
                                                   OPERATOR_PROMPT_TIMEOUT_S);
        error = stages[index].leave();
        if (error != ESP_OK) {
            report_error(FACTORY_TEST_DISPLAY_SLEEP, error,
                         stages[index].name);
            return error;
        }
        if (answer == 'n') {
            char detail[96];
            snprintf(detail, sizeof(detail),
                     "operator reports panel stayed on in %s",
                     stages[index].name);
            factory_report_set(FACTORY_TEST_DISPLAY_SLEEP, FACTORY_STATUS_FAIL,
                               detail);
            factory_report_print_one(FACTORY_TEST_DISPLAY_SLEEP);
            return ESP_FAIL;
        }
        if (answer == 'y') {
            ++confirmed;
        }

        error = show_display_pattern();
        if (error != ESP_OK) {
            report_error(FACTORY_TEST_DISPLAY_SLEEP, error, "pattern redraw failed");
            return error;
        }
        snprintf(question, sizeof(question), "%s: did the pattern return?",
                 stages[index].name);
        answer = factory_console_ask_operator("display_sleep", question,
                                              OPERATOR_PROMPT_TIMEOUT_S);
        if (answer == 'n') {
            char detail[96];
            snprintf(detail, sizeof(detail),
                     "operator reports no recovery after %s", stages[index].name);
            factory_report_set(FACTORY_TEST_DISPLAY_SLEEP, FACTORY_STATUS_FAIL,
                               detail);
            factory_report_print_one(FACTORY_TEST_DISPLAY_SLEEP);
            return ESP_FAIL;
        }
        if (answer == 'y') {
            ++confirmed;
        }
    }

    char detail[96];
    if (confirmed == 4) {
        snprintf(detail, sizeof(detail),
                 "sleep and deep standby cycles visually confirmed");
        factory_report_set(FACTORY_TEST_DISPLAY_SLEEP, FACTORY_STATUS_PASS, detail);
    } else {
        snprintf(detail, sizeof(detail), "confirmed=%u/4; use mark", confirmed);
        factory_report_set(FACTORY_TEST_DISPLAY_SLEEP, FACTORY_STATUS_NOT_RUN,
                           detail);
    }
    factory_report_print_one(FACTORY_TEST_DISPLAY_SLEEP);
    return ESP_OK;
}

static int command_touch_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_display == NULL && show_display_pattern() != ESP_OK) {
        report_error(FACTORY_TEST_TOUCH, ESP_FAIL, "display initialization failed");
        return ESP_FAIL;
    }
    lv_indev_t *input = bsp_display_get_input_dev();
    if (input == NULL) {
        report_error(FACTORY_TEST_TOUCH, ESP_ERR_INVALID_STATE, "touch input unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    printf("Touch each of the four display quadrants within %d seconds\n",
           TOUCH_TEST_SECONDS);
    uint8_t quadrants = 0;
    unsigned samples = 0;
    const int64_t deadline = esp_timer_get_time() + TOUCH_TEST_SECONDS * INT64_C(1000000);
    while (esp_timer_get_time() < deadline && quadrants != 0x0f) {
        if (bsp_display_lock(100)) {
            if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) {
                lv_point_t point;
                lv_indev_get_point(input, &point);
                const unsigned quadrant = (point.x >= BSP_LCD_H_RES / 2 ? 1U : 0U) |
                                          (point.y >= BSP_LCD_V_RES / 2 ? 2U : 0U);
                quadrants |= 1U << quadrant;
                ++samples;
            }
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "quadrants=0x%02x samples=%u", quadrants, samples);
    factory_report_set(FACTORY_TEST_TOUCH,
                       quadrants == 0x0f ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_TOUCH);
    return quadrants == 0x0f ? ESP_OK : ESP_FAIL;
}

static int command_led_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_led == NULL) {
        led_indicator_handle_t handles[BSP_LED_NUM] = {0};
        int count = 0;
        const esp_err_t error = bsp_led_indicator_create(handles, &count,
                                                          BSP_LED_NUM);
        if (error != ESP_OK || count != BSP_LED_NUM) {
            report_error(FACTORY_TEST_RGB_LED,
                         error != ESP_OK ? error : ESP_FAIL,
                         "RGB LED initialization failed");
            return error != ESP_OK ? error : ESP_FAIL;
        }
        s_led = handles[BSP_LED_1];
    }
    const uint32_t colors[] = {
        SET_IRGB(0, 64, 0, 0), SET_IRGB(0, 0, 64, 0), SET_IRGB(0, 0, 0, 64),
    };
    for (unsigned index = 0; index < sizeof(colors) / sizeof(colors[0]); ++index) {
        const esp_err_t error = led_indicator_set_rgb(s_led, colors[index]);
        if (error != ESP_OK) {
            report_error(FACTORY_TEST_RGB_LED, error, "RGB update failed");
            return error;
        }
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    led_indicator_set_on_off(s_led, false);
    const char answer = factory_console_ask_operator(
                            "rgb_led", "Did the LED cycle red, green, blue?",
                            OPERATOR_PROMPT_TIMEOUT_S);
    record_operator_verdict(FACTORY_TEST_RGB_LED, answer,
                            "operator confirmed red green blue sequence",
                            "operator rejected the LED sequence",
                            "red green blue sequence sent; inspect LED then use mark");
    return ESP_OK;
}

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
        report_error(FACTORY_TEST_SDCARD, error, "mount failed");
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

static esp_codec_dev_handle_t audio_device(bool speaker)
{
    esp_codec_dev_handle_t *handle = speaker ? &s_speaker : &s_microphone;
    if (*handle == NULL) {
        *handle = speaker ? bsp_audio_codec_speaker_init() :
                            bsp_audio_codec_microphone_init();
    }
    return *handle;
}

static esp_codec_dev_sample_info_t audio_format(void)
{
    return (esp_codec_dev_sample_info_t) {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
}

static int command_speaker_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_codec_dev_handle_t speaker = audio_device(true);
    if (speaker == NULL) {
        report_error(FACTORY_TEST_SPEAKER, ESP_FAIL, "speaker codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t format = audio_format();
    int result = esp_codec_dev_open(speaker, &format);
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_out_vol(speaker, 30);
    }
    static int16_t samples[AUDIO_FRAME_COUNT * 2];
    for (unsigned frame = 0; frame < AUDIO_FRAME_COUNT; ++frame) {
        const int16_t value = (frame % (AUDIO_SAMPLE_RATE / 880)) <
                              (AUDIO_SAMPLE_RATE / 1760) ? 2200 : -2200;
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    for (unsigned block = 0; result == ESP_CODEC_DEV_OK && block < 65; ++block) {
        result = esp_codec_dev_write(speaker, samples, sizeof(samples));
    }
    esp_codec_dev_close(speaker);
    if (result != ESP_CODEC_DEV_OK) {
        report_error(FACTORY_TEST_SPEAKER, result, "speaker write failed");
        return result;
    }
    const char answer = factory_console_ask_operator(
                            "speaker", "Did you hear the tone?",
                            OPERATOR_PROMPT_TIMEOUT_S);
    record_operator_verdict(FACTORY_TEST_SPEAKER, answer,
                            "operator confirmed the tone",
                            "operator did not hear the tone",
                            "tone sent; confirm sound then use mark");
    return ESP_OK;
}

static int command_microphone_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_codec_dev_handle_t microphone = audio_device(false);
    if (microphone == NULL) {
        report_error(FACTORY_TEST_MICROPHONE, ESP_FAIL, "microphone codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t format = audio_format();
    int result = esp_codec_dev_open(microphone, &format);
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_in_gain(microphone, 24.0f);
    }
    static int16_t samples[AUDIO_FRAME_COUNT * 2];
    uint16_t peak = 0;
    for (unsigned block = 0; result == ESP_CODEC_DEV_OK && block < 20; ++block) {
        result = esp_codec_dev_read(microphone, samples, sizeof(samples));
        for (unsigned index = 0; index < sizeof(samples) / sizeof(samples[0]); ++index) {
            const int32_t value = samples[index] < 0 ? -(int32_t)samples[index] : samples[index];
            if (value > peak) {
                peak = (uint16_t)value;
            }
        }
    }
    esp_codec_dev_close(microphone);
    const bool passed = result == ESP_CODEC_DEV_OK && peak > 64;
    char detail[96];
    snprintf(detail, sizeof(detail), "peak=%u", peak);
    factory_report_set(FACTORY_TEST_MICROPHONE,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       detail);
    factory_report_print_one(FACTORY_TEST_MICROPHONE);
    return passed ? ESP_OK : (result != ESP_CODEC_DEV_OK ? result : ESP_FAIL);
}

static uint32_t camera_frame_crc(const uint8_t *pixels, uint32_t length,
                                 bool *uniform)
{
    /* Sample one byte every 4 KiB: enough to catch a blank or frozen pipe
     * without adding a full-frame pass to a 10 fps capture. */
    uint32_t crc = 5381;
    const uint8_t first = pixels[0];
    *uniform = true;
    for (uint32_t offset = 0; offset < length; offset += 4096) {
        crc = crc * 33 + pixels[offset];
        if (pixels[offset] != first) {
            *uniform = false;
        }
    }
    return crc;
}

static uint32_t camera_expected_frame_bytes(const struct v4l2_format *format)
{
    /* Enforce an exact byte count only for formats with a known fixed pixel
     * size; anything else is checked by buffer bound and content alone. */
    if (format->fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565X) {
        return format->fmt.pix.width * format->fmt.pix.height * 2;
    }
    return 0;
}

static int command_camera_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t error = bsp_camera_start(NULL);
    int file = -1;
    if (error == ESP_OK) {
        file = open(BSP_CAMERA_DEVICE, O_RDONLY);
        if (file < 0) {
            error = ESP_ERR_NOT_FOUND;
        }
    }
    if (error == ESP_OK) {
        /* A dead or unpowered sensor must not wedge the console until
         * power-off: bound the per-frame DQBUF wait. */
        const struct timeval dqbuf_timeout = {
            .tv_sec = CAMERA_DQBUF_TIMEOUT_MS / 1000,
            .tv_usec = (CAMERA_DQBUF_TIMEOUT_MS % 1000) * 1000,
        };
        if (ioctl(file, VIDIOC_S_DQBUF_TIMEOUT, &dqbuf_timeout) != 0) {
            error = ESP_FAIL;
        }
    }

    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format format = {0};
    format.type = type;
    uint8_t *buffers[CAMERA_BUFFER_COUNT] = {NULL};
    uint32_t buffer_lengths[CAMERA_BUFFER_COUNT] = {0};
    bool streaming = false;
    unsigned frames = 0;
    uint32_t frame_bytes = 0;
    bool size_ok = true;
    bool content_ok = true;
    bool change_seen = false;
    bool have_previous = false;
    uint32_t previous_crc = 0;

    if (error == ESP_OK && ioctl(file, VIDIOC_G_FMT, &format) != 0) {
        /* No driver default: request the EVT1 sensor format explicitly. */
        memset(&format, 0, sizeof(format));
        format.type = type;
        format.fmt.pix.width = 800;
        format.fmt.pix.height = 600;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565X;
        if (ioctl(file, VIDIOC_S_FMT, &format) != 0) {
            error = ESP_FAIL;
        }
    }

    if (error == ESP_OK) {
        struct v4l2_requestbuffers request = {0};
        request.count = CAMERA_BUFFER_COUNT;
        request.type = type;
        request.memory = V4L2_MEMORY_MMAP;
        if (ioctl(file, VIDIOC_REQBUFS, &request) != 0) {
            error = ESP_FAIL;
        }
    }
    for (int index = 0; error == ESP_OK && index < CAMERA_BUFFER_COUNT; ++index) {
        struct v4l2_buffer query = {0};
        query.type = type;
        query.memory = V4L2_MEMORY_MMAP;
        query.index = index;
        if (ioctl(file, VIDIOC_QUERYBUF, &query) != 0) {
            error = ESP_FAIL;
            break;
        }
        buffers[index] = mmap(NULL, query.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, file, query.m.offset);
        if (buffers[index] == MAP_FAILED) {
            buffers[index] = NULL;
            error = ESP_FAIL;
            break;
        }
        buffer_lengths[index] = query.length;
        if (ioctl(file, VIDIOC_QBUF, &query) != 0) {
            error = ESP_FAIL;
            break;
        }
    }
    if (error == ESP_OK) {
        if (ioctl(file, VIDIOC_STREAMON, &type) != 0) {
            error = ESP_FAIL;
        } else {
            streaming = true;
        }
    }

    while (error == ESP_OK && frames < CAMERA_CAPTURE_FRAMES) {
        struct v4l2_buffer done = {0};
        done.type = type;
        done.memory = V4L2_MEMORY_MMAP;
        /* DQBUF waits at most CAMERA_DQBUF_TIMEOUT_MS per frame (set above). */
        if (ioctl(file, VIDIOC_DQBUF, &done) != 0) {
            error = ESP_ERR_TIMEOUT;
            break;
        }
        if ((done.flags & V4L2_BUF_FLAG_DONE) != 0 && done.index < CAMERA_BUFFER_COUNT) {
            const uint32_t expected = camera_expected_frame_bytes(&format);
            if ((expected != 0 && done.bytesused != expected) ||
                    done.bytesused > buffer_lengths[done.index]) {
                size_ok = false;
            } else {
                bool uniform = true;
                const uint32_t crc = camera_frame_crc(buffers[done.index],
                                                      done.bytesused, &uniform);
                if (uniform) {
                    content_ok = false;
                }
                if (have_previous && crc != previous_crc) {
                    change_seen = true;
                }
                previous_crc = crc;
                have_previous = true;
                frame_bytes = done.bytesused;
            }
            ++frames;
        }
        if (ioctl(file, VIDIOC_QBUF, &done) != 0) {
            error = ESP_FAIL;
            break;
        }
    }

    if (streaming) {
        ioctl(file, VIDIOC_STREAMOFF, &type);
    }
    for (int index = 0; index < CAMERA_BUFFER_COUNT; ++index) {
        if (buffers[index] != NULL) {
            munmap(buffers[index], buffer_lengths[index]);
        }
    }
    if (file >= 0) {
        close(file);
    }
    const esp_err_t stop_error = bsp_camera_stop();
    if (error == ESP_OK && stop_error != ESP_OK) {
        error = stop_error;
    }

    if (error != ESP_OK) {
        report_error(FACTORY_TEST_CAMERA, error, "camera capture failed");
        return error;
    }
    const bool pass = frames >= 3 && size_ok && content_ok && change_seen;
    /* A stream that delivers valid but identical frames is suspect, not
     * conclusive: flag it for the operator instead of failing outright. */
    const factory_status_t result = pass ? FACTORY_STATUS_PASS :
                                    frames >= 3 && size_ok && content_ok ?
                                    FACTORY_STATUS_WARN : FACTORY_STATUS_FAIL;
    char detail[96];
    snprintf(detail, sizeof(detail),
             "%ux%u fmt=0x%08lx frames=%u bytes=%" PRIu32 "%s%s",
             (unsigned)format.fmt.pix.width, (unsigned)format.fmt.pix.height,
             (unsigned long)format.fmt.pix.pixelformat,
             frames, frame_bytes,
             size_ok ? "" : " bad_size", content_ok ? "" : " blank");
    factory_report_set(FACTORY_TEST_CAMERA, result, detail);
    factory_report_print_one(FACTORY_TEST_CAMERA);
    return result == FACTORY_STATUS_FAIL ? ESP_FAIL : ESP_OK;
}

static bool test_accepts_manual_result(factory_test_id_t test)
{
    return test == FACTORY_TEST_DISPLAY || test == FACTORY_TEST_RGB_LED ||
           test == FACTORY_TEST_SPEAKER || test == FACTORY_TEST_BUTTONS ||
           test == FACTORY_TEST_DISPLAY_SLEEP;
}

static int command_mark(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: mark TEST pass|fail|skip [detail]\n");
        return ESP_ERR_INVALID_ARG;
    }
    const factory_test_id_t test = factory_report_find(argv[1]);
    if (test == FACTORY_TEST_COUNT) {
        printf("unknown test: %s\n", argv[1]);
        return ESP_ERR_INVALID_ARG;
    }
    const factory_status_t status = strcmp(argv[2], "pass") == 0 ? FACTORY_STATUS_PASS :
                                            strcmp(argv[2], "fail") == 0 ? FACTORY_STATUS_FAIL :
                                            strcmp(argv[2], "skip") == 0 ? FACTORY_STATUS_SKIP :
                                                                            FACTORY_STATUS_NOT_RUN;
    if (status == FACTORY_STATUS_NOT_RUN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (status != FACTORY_STATUS_SKIP && !test_accepts_manual_result(test)) {
        printf("%s is software-scored; run its test command instead\n", argv[1]);
        return ESP_ERR_INVALID_ARG;
    }

    char detail[96] = "operator marked";
    if (argc > 3) {
        detail[0] = '\0';
        for (int index = 3; index < argc; ++index) {
            const size_t used = strlen(detail);
            snprintf(detail + used, sizeof(detail) - used, "%s%s",
                     used > 0 ? " " : "", argv[index]);
            if (strlen(detail) == sizeof(detail) - 1) {
                break;
            }
        }
    }
    factory_report_set(test, status, detail);
    factory_report_print_one(test);
    return ESP_OK;
}

esp_err_t factory_peripherals_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "pmic_test", .help = "Read TG28_SW identity, battery, VBUS, and charge state.", .func = command_pmic_test},
        {.command = "pmic", .help = "Read PMIC state or set charge/input limits with explicit safety gates.", .func = command_pmic},
        {.command = "charge_test", .help = "Check charger state: charge_test [source_verified].", .func = command_charge_test},
        {.command = "rail", .help = "Inspect/control an unowned TG28_SW rail.", .func = command_rail},
        {.command = "peripheral_power", .help = "Apply a complete, owner-aware peripheral power sequence.", .func = command_peripheral_power},
        {.command = "power_all_off", .help = "Stop activity and switch every peripheral rail off (best effort, idempotent).", .func = command_power_all_off},
        {.command = "rtc_test", .help = "Read RX8130CE time and retained status flags.", .func = command_rtc_test},
        {.command = "rtc_set", .help = "Set and verify RX8130CE time: rtc_set YYYY-MM-DD HH:MM:SS WEEKDAY.", .func = command_rtc_set},
        {.command = "rtc_alarm", .help = "Fire an RX8130CE alarm at the next minute and check the flag.", .func = command_rtc_alarm_test},
        {.command = "irq_test", .help = "Service and verify the shared PMIC/RTC interrupt line.", .func = command_irq_test},
        {.command = "buttons", .help = "Wait for BOOT (GPIO61) and PWR (TG28_SW IRQ) key presses.", .func = command_buttons_test},
        {.command = "typec_test", .help = "Read FUSB303B connection state without changing its role.", .func = command_type_c_test},
        {.command = "otg", .help = "Explicitly enable or disable USB source power.", .func = command_otg},
        {.command = "usb_host_test", .help = "Install the USB Host stack and enumerate one Type-C2 device (500 mA budget).", .func = command_usb_host_test},
        {.command = "display_test", .help = "Show a four-color AMOLED inspection pattern.", .func = command_display_test},
        {.command = "display_brightness", .help = "Set AMOLED brightness from 0 to 100 percent.", .func = command_display_brightness},
        {.command = "display_sleep", .help = "Enter AMOLED sleep or deep standby: display_sleep [deep].", .func = command_display_sleep},
        {.command = "display_wake", .help = "Wake AMOLED from sleep or deep standby: display_wake [deep].", .func = command_display_wake},
        {.command = "display_sleep_test", .help = "Cycle AMOLED sleep and deep standby with operator checks.", .func = command_display_sleep_test},
        {.command = "touch_test", .help = "Require a touch in all four display quadrants.", .func = command_touch_test},
        {.command = "led_test", .help = "Show red, green, and blue on the addressable LED.", .func = command_led_test},
        {.command = "sdcard_test", .help = "Mount, write, verify, remove, and unmount a test file.", .func = command_sdcard_test},
        {.command = "speaker_test", .help = "Play a short low-level square-wave tone.", .func = command_speaker_test},
        {.command = "microphone_test", .help = "Capture audio and check for a non-zero signal.", .func = command_microphone_test},
        {.command = "camera_test", .help = "Capture DVP frames and verify size, content, and motion.", .func = command_camera_test},
        {.command = "mark", .help = "Record a manual result: mark TEST pass|fail|skip [detail].", .func = command_mark},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
