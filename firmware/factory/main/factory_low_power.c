/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_modules.h"
#include "factory_peripherals.h"

#define FACTORY_SLEEP_RECORD_MAGIC UINT32_C(0x53333153)
#define FACTORY_SLEEP_MIN_SECONDS  1U
#define FACTORY_SLEEP_MAX_SECONDS  (24U * 60U * 60U)
#define FACTORY_SLEEP_SETTLE_MS    100U
#define FACTORY_TEMP_MAX_SAMPLES   120U
#define FACTORY_TEMP_MAX_INTERVAL_MS 10000U

typedef enum {
    FACTORY_SLEEP_MODE_NONE = 0,
    FACTORY_SLEEP_MODE_LIGHT,
    FACTORY_SLEEP_MODE_DEEP,
} factory_sleep_mode_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint32_t mode;
    uint32_t requested_seconds;
    uint64_t entered_rtc_us;
    uint64_t measured_us;
    uint32_t wake_causes;
    int32_t result;
} factory_sleep_record_t;

static RTC_DATA_ATTR factory_sleep_record_t s_sleep_record;

static bool parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
                      uint32_t *value)
{
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static const char *sleep_mode_name(factory_sleep_mode_t mode)
{
    switch (mode) {
    case FACTORY_SLEEP_MODE_LIGHT:
        return "light";
    case FACTORY_SLEEP_MODE_DEEP:
        return "deep";
    default:
        return "none";
    }
}

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

static bool wake_cause_is_set(uint32_t causes, esp_sleep_source_t source)
{
    return (causes & (UINT32_C(1) << source)) != 0;
}

static void format_wake_causes(uint32_t causes, char *out, size_t out_size)
{
    static const struct {
        esp_sleep_source_t source;
        const char *name;
    } known[] = {
        { ESP_SLEEP_WAKEUP_EXT0, "ext0" },
        { ESP_SLEEP_WAKEUP_EXT1, "ext1" },
        { ESP_SLEEP_WAKEUP_TIMER, "timer" },
        { ESP_SLEEP_WAKEUP_TOUCHPAD, "touch" },
        { ESP_SLEEP_WAKEUP_ULP, "ulp" },
        { ESP_SLEEP_WAKEUP_GPIO, "gpio" },
        { ESP_SLEEP_WAKEUP_UART0, "uart0" },
        { ESP_SLEEP_WAKEUP_UART1, "uart1" },
        { ESP_SLEEP_WAKEUP_WIFI, "wifi" },
        { ESP_SLEEP_WAKEUP_COCPU, "cocpu" },
        { ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG, "cocpu_trap" },
        { ESP_SLEEP_WAKEUP_BT, "bt" },
        { ESP_SLEEP_WAKEUP_VAD, "vad" },
        { ESP_SLEEP_WAKEUP_VBAT_UNDER_VOLT, "vbat_uv" },
    };

    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    size_t used = 0;
    for (size_t index = 0; index < sizeof(known) / sizeof(known[0]); ++index) {
        if (!wake_cause_is_set(causes, known[index].source)) {
            continue;
        }
        const int written = snprintf(out + used, out_size - used, "%s%s",
                                     used == 0 ? "" : ",", known[index].name);
        if (written < 0 || (size_t)written >= out_size - used) {
            out[out_size - 1] = '\0';
            return;
        }
        used += (size_t)written;
    }
    if (used == 0) {
        snprintf(out, out_size, "none");
    }
}

static void sleep_record_begin(factory_sleep_mode_t mode, uint32_t seconds)
{
    const uint32_t next_sequence =
        s_sleep_record.magic == FACTORY_SLEEP_RECORD_MAGIC ?
        s_sleep_record.sequence + 1U : 1U;
    memset(&s_sleep_record, 0, sizeof(s_sleep_record));
    s_sleep_record.magic = FACTORY_SLEEP_RECORD_MAGIC;
    s_sleep_record.sequence = next_sequence;
    s_sleep_record.mode = (uint32_t)mode;
    s_sleep_record.requested_seconds = seconds;
    s_sleep_record.result = ESP_OK;
}

void factory_low_power_note_boot(void)
{
    if (s_sleep_record.magic != FACTORY_SLEEP_RECORD_MAGIC ||
            s_sleep_record.mode != FACTORY_SLEEP_MODE_DEEP ||
            s_sleep_record.result != ESP_OK ||
            esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        return;
    }

    const uint64_t woke_rtc_us = esp_rtc_get_time_us();
    s_sleep_record.measured_us =
        woke_rtc_us >= s_sleep_record.entered_rtc_us ?
        woke_rtc_us - s_sleep_record.entered_rtc_us : 0;
    s_sleep_record.wake_causes = esp_sleep_get_wakeup_causes();
}

static int command_temp_read(int argc, char **argv)
{
    uint32_t samples = 1;
    uint32_t interval_ms = 1000;
    if (argc > 3 ||
            (argc >= 2 && !parse_u32(argv[1], 1, FACTORY_TEMP_MAX_SAMPLES,
                                     &samples)) ||
            (argc >= 3 && !parse_u32(argv[2], 0,
                                     FACTORY_TEMP_MAX_INTERVAL_MS,
                                     &interval_ms))) {
        printf("usage: temp_read [SAMPLES 1-%u] [INTERVAL_MS 0-%u]\n",
               FACTORY_TEMP_MAX_SAMPLES, FACTORY_TEMP_MAX_INTERVAL_MS);
        return ESP_ERR_INVALID_ARG;
    }

    temperature_sensor_handle_t sensor = NULL;
    temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t result = temperature_sensor_install(&config, &sensor);
    if (result == ESP_OK) {
        result = temperature_sensor_enable(sensor);
    }
    if (result != ESP_OK) {
        printf("temp_read: sensor setup failed: %s\n", esp_err_to_name(result));
        if (sensor != NULL) {
            temperature_sensor_uninstall(sensor);
        }
        return result;
    }

    float minimum = 0.0f;
    float maximum = 0.0f;
    float total = 0.0f;
    for (uint32_t index = 0; index < samples; ++index) {
        float celsius = 0.0f;
        result = temperature_sensor_get_celsius(sensor, &celsius);
        if (result != ESP_OK) {
            printf("temp_read: sample=%" PRIu32 " failed: %s\n", index,
                   esp_err_to_name(result));
            break;
        }
        if (index == 0 || celsius < minimum) {
            minimum = celsius;
        }
        if (index == 0 || celsius > maximum) {
            maximum = celsius;
        }
        total += celsius;
        printf("temp_sample=%" PRIu32 " die_c=%.2f\n", index, celsius);
        if (index + 1U < samples && interval_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
        }
    }

    const esp_err_t disable_error = temperature_sensor_disable(sensor);
    const esp_err_t uninstall_error = temperature_sensor_uninstall(sensor);
    if (result == ESP_OK && disable_error != ESP_OK) {
        result = disable_error;
    }
    if (result == ESP_OK && uninstall_error != ESP_OK) {
        result = uninstall_error;
    }
    if (result != ESP_OK) {
        printf("temp_read: cleanup/read failed: %s\n", esp_err_to_name(result));
        return result;
    }

    const float average = total / samples;
    printf("temp_summary samples=%" PRIu32 " interval_ms=%" PRIu32
           " min_die_c=%.2f max_die_c=%.2f avg_die_c=%.2f "
           "range_c=-10..80 nominal_max_error_c=1 not_ambient=true\n",
           samples, interval_ms, minimum, maximum, average);
    printf("FACTORY_TEMP {\"samples\":%" PRIu32
           ",\"interval_ms\":%" PRIu32
           ",\"min_die_c\":%.2f,\"max_die_c\":%.2f"
           ",\"avg_die_c\":%.2f,\"range_c\":\"-10..80\""
           ",\"nominal_max_error_c\":1,\"not_ambient\":true}\n",
           samples, interval_ms, minimum, maximum, average);
    printf("temp_read: SoC die estimate only; it is not ambient, battery, "
           "or TG28 temperature and does not close a thermal verdict\n");
    return ESP_OK;
}

static int command_sleep_test(int argc, char **argv)
{
    uint32_t seconds = 0;
    factory_sleep_mode_t mode = FACTORY_SLEEP_MODE_NONE;
    if (argc == 3 && strcmp(argv[1], "light") == 0) {
        mode = FACTORY_SLEEP_MODE_LIGHT;
    } else if (argc == 3 && strcmp(argv[1], "deep") == 0) {
        mode = FACTORY_SLEEP_MODE_DEEP;
    }
    if (mode == FACTORY_SLEEP_MODE_NONE ||
            !parse_u32(argv[2], FACTORY_SLEEP_MIN_SECONDS,
                       FACTORY_SLEEP_MAX_SECONDS, &seconds)) {
        printf("usage: sleep_test light|deep SECONDS %u-%u\n",
               FACTORY_SLEEP_MIN_SECONDS, FACTORY_SLEEP_MAX_SECONDS);
        return ESP_ERR_INVALID_ARG;
    }

    sleep_record_begin(mode, seconds);
    esp_err_t result = factory_rf_prepare_for_sleep();
    if (result != ESP_OK) {
        s_sleep_record.result = result;
        return result;
    }

    result = factory_peripherals_power_all_off();
    if (result != ESP_OK) {
        s_sleep_record.result = result;
        printf("sleep_test: safe-state cleanup failed; sleep aborted: %s\n",
               esp_err_to_name(result));
        return result;
    }
    result = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (result == ESP_OK) {
        result = esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    }
    if (result != ESP_OK) {
        s_sleep_record.result = result;
        printf("sleep_test: timer wake configuration failed: %s\n",
               esp_err_to_name(result));
        return result;
    }

    printf("sleep_test: mode=%s seconds=%" PRIu32
           " sequence=%" PRIu32 "\n",
           sleep_mode_name(mode), seconds, s_sleep_record.sequence);
    printf("sleep_test: every switched peripheral is off; this tests ESP "
           "sleep, not TG28 soft power-off\n");
    printf("sleep_test: SoC timer sleep + board safe-state smoke test only; "
           "UART, TG28, RTC, LP-I2C, VBUS, and board leakage remain in scope\n");
    if (mode == FACTORY_SLEEP_MODE_DEEP) {
        printf("sleep_test: deep sleep resets the application; run wake_info "
               "after the console returns\n");
    } else {
        printf("sleep_test: peripherals remain off after light-sleep wake\n");
    }
    if (fflush(stdout) != 0) {
        s_sleep_record.result = ESP_FAIL;
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        printf("sleep_test: stdout flush failed; sleep aborted\n");
        return ESP_FAIL;
    }
    result = uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);
    if (result != ESP_OK) {
        s_sleep_record.result = result;
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        printf("sleep_test: UART drain failed; sleep aborted: %s\n",
               esp_err_to_name(result));
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(FACTORY_SLEEP_SETTLE_MS));

    s_sleep_record.entered_rtc_us = esp_rtc_get_time_us();
    if (mode == FACTORY_SLEEP_MODE_DEEP) {
        result = esp_deep_sleep_try_to_start();
        s_sleep_record.result = result;
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        printf("sleep_test: deep-sleep request returned without sleeping: %s\n",
               esp_err_to_name(result));
        return result;
    }

    result = esp_light_sleep_start();
    const uint64_t woke_rtc_us = esp_rtc_get_time_us();
    s_sleep_record.measured_us =
        woke_rtc_us >= s_sleep_record.entered_rtc_us ?
        woke_rtc_us - s_sleep_record.entered_rtc_us : 0;
    s_sleep_record.wake_causes = esp_sleep_get_wakeup_causes();
    s_sleep_record.result = result;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    printf("sleep_wake mode=light requested_s=%" PRIu32
           " measured_us=%" PRIu64 " causes=0x%08" PRIx32
           " result=%s\n",
           seconds, s_sleep_record.measured_us, s_sleep_record.wake_causes,
           esp_err_to_name(result));
    return result;
}

static int command_wake_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const uint32_t current_causes = esp_sleep_get_wakeup_causes();
    char current_cause_names[128];
    format_wake_causes(current_causes, current_cause_names,
                       sizeof(current_cause_names));
    const bool record_valid =
        s_sleep_record.magic == FACTORY_SLEEP_RECORD_MAGIC &&
        s_sleep_record.mode >= FACTORY_SLEEP_MODE_LIGHT &&
        s_sleep_record.mode <= FACTORY_SLEEP_MODE_DEEP;

    if (!record_valid) {
        printf("wake_info reset=%s causes=0x%08" PRIx32
               " cause_names=%s retained_record=none\n",
               reset_reason_name(reset_reason), current_causes,
               current_cause_names);
        printf("FACTORY_WAKE {\"reset\":\"%s\",\"causes\":%" PRIu32
               ",\"cause_names\":\"%s\",\"record\":false}\n",
               reset_reason_name(reset_reason), current_causes,
               current_cause_names);
        return ESP_OK;
    }

    const factory_sleep_mode_t mode =
        (factory_sleep_mode_t)s_sleep_record.mode;
    uint64_t measured_us = s_sleep_record.measured_us;
    uint32_t wake_causes = s_sleep_record.wake_causes;

    char wake_cause_names[128];
    format_wake_causes(wake_causes, wake_cause_names,
                       sizeof(wake_cause_names));
    const bool timer_wake =
        wake_cause_is_set(wake_causes, ESP_SLEEP_WAKEUP_TIMER);
    const char *record_state = "matched";
    if (s_sleep_record.result != ESP_OK) {
        record_state = "rejected";
    } else if (mode == FACTORY_SLEEP_MODE_DEEP &&
            reset_reason != ESP_RST_DEEPSLEEP) {
        record_state = "mismatch";
    } else if (mode == FACTORY_SLEEP_MODE_LIGHT &&
               s_sleep_record.measured_us == 0) {
        record_state = "incomplete";
    }
    printf("wake_info reset=%s mode=%s sequence=%" PRIu32
           " requested_s=%" PRIu32 " measured_us=%" PRIu64
           " causes=0x%08" PRIx32 " cause_names=%s timer=%s "
           "record_state=%s result=%s\n",
           reset_reason_name(reset_reason), sleep_mode_name(mode),
           s_sleep_record.sequence, s_sleep_record.requested_seconds,
           measured_us, wake_causes, wake_cause_names,
           timer_wake ? "yes" : "no", record_state,
           esp_err_to_name((esp_err_t)s_sleep_record.result));
    printf("FACTORY_WAKE {\"reset\":\"%s\",\"mode\":\"%s\""
           ",\"sequence\":%" PRIu32 ",\"requested_s\":%" PRIu32
           ",\"measured_us\":%" PRIu64 ",\"causes\":%" PRIu32
           ",\"cause_names\":\"%s\",\"timer\":%s"
           ",\"record_state\":\"%s\",\"result\":\"%s\"}\n",
           reset_reason_name(reset_reason), sleep_mode_name(mode),
           s_sleep_record.sequence, s_sleep_record.requested_seconds,
           measured_us, wake_causes, wake_cause_names,
           timer_wake ? "true" : "false", record_state,
           esp_err_to_name((esp_err_t)s_sleep_record.result));
    return ESP_OK;
}

esp_err_t factory_low_power_register(void)
{
    const esp_console_cmd_t commands[] = {
        {
            .command = "temp_read",
            .help = "Read the ESP32-S31 internal temperature sensor: temp_read [SAMPLES] [INTERVAL_MS].",
            .func = command_temp_read,
        },
        {
            .command = "sleep_test",
            .help = "Power peripherals off, then use timer wake: sleep_test light|deep SECONDS.",
            .func = command_sleep_test,
        },
        {
            .command = "wake_info",
            .help = "Print reset cause and retained timing from the latest sleep_test.",
            .func = command_wake_info,
        },
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
