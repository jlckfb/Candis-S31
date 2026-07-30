/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_convert.h"

#include "factory_peripherals.h"
#include "factory_report.h"

#define TOUCH_TEST_SECONDS 15
#define AUDIO_SAMPLE_RATE  22050
#define AUDIO_FRAME_COUNT  512

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
    const esp_err_t error = bsp_pmic_get_status(&status);
    if (error != ESP_OK) {
        report_error(FACTORY_TEST_PMIC, error, "TG28_SW status read failed");
        return error;
    }
    printf("chip_id=0x%02x vbat=%u mV soc=%u%% battery=%s vbus=%s "
           "charging=%s done=%s status0=0x%02x status1=0x%02x\n",
           status.chip_id, status.battery_mv, status.battery_percent,
           status.battery_present ? "present" : "absent",
           status.vbus_present ? "present" : "absent",
           status.charging ? "yes" : "no", status.charge_done ? "yes" : "no",
           status.common_status0, status.common_status1);
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
    if (argc != 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
        printf("usage: peripheral_power NAME on|off\n");
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
    return bsp_peripheral_power_set(peripheral, strcmp(argv[2], "on") == 0);
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
    if (end == argv[3] || *end != '\0' || weekday < 0 || weekday > 6 ||
            year > UINT16_MAX || month > UINT8_MAX || day > UINT8_MAX ||
            hour > UINT8_MAX || minute > UINT8_MAX || second > UINT8_MAX) {
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
    char detail[96];
    snprintf(detail, sizeof(detail), "addr=0x%02x id=0x%02x type=0x%02x attached=%s vbus=%s orientation=%u",
             status.i2c_address, status.device_id, status.device_type,
             status.attached ? "yes" : "no", status.vbus_ok ? "yes" : "no",
             status.orientation);
    factory_report_set(FACTORY_TEST_TYPE_C, FACTORY_STATUS_PASS, detail);
    factory_report_print_one(FACTORY_TEST_TYPE_C);
    return ESP_OK;
}

static int command_otg(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        return bsp_usb_otg_power_set(false, BSP_TYPE_C_CURRENT_DEFAULT);
    }
    if (argc != 3 || strcmp(argv[1], "on") != 0) {
        printf("usage: otg off | otg on default|1.5|3.0\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(argv[2], "default") != 0 && strcmp(argv[2], "1.5") != 0 &&
            strcmp(argv[2], "3.0") != 0) {
        printf("usage: otg off | otg on default|1.5|3.0\n");
        return ESP_ERR_INVALID_ARG;
    }
    const bsp_type_c_current_t current = strcmp(argv[2], "3.0") == 0 ?
                                         BSP_TYPE_C_CURRENT_3_0_A :
                                         strcmp(argv[2], "1.5") == 0 ?
                                         BSP_TYPE_C_CURRENT_1_5_A :
                                         BSP_TYPE_C_CURRENT_DEFAULT;
    printf("WARNING: enabling the USB OTG boost rail; verify VBUS before connecting a load\n");
    return bsp_usb_otg_power_set(true, current);
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

static int command_display_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_display == NULL) {
        s_display = bsp_display_start();
    }
    if (s_display == NULL) {
        report_error(FACTORY_TEST_DISPLAY, ESP_FAIL, "display initialization failed");
        return ESP_FAIL;
    }
    if (!bsp_display_lock(1000)) {
        report_error(FACTORY_TEST_DISPLAY, ESP_ERR_TIMEOUT, "LVGL lock failed");
        return ESP_ERR_TIMEOUT;
    }
    create_display_pattern();
    bsp_display_unlock();
    factory_report_set(FACTORY_TEST_DISPLAY, FACTORY_STATUS_NOT_RUN,
                       "color pattern active; inspect panel then use mark");
    factory_report_print_one(FACTORY_TEST_DISPLAY);
    return ESP_OK;
}

static int command_touch_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_display == NULL && command_display_test(0, NULL) != ESP_OK) {
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
    factory_report_set(FACTORY_TEST_RGB_LED, FACTORY_STATUS_NOT_RUN,
                       "red green blue sequence sent; inspect LED then use mark");
    factory_report_print_one(FACTORY_TEST_RGB_LED);
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
    FILE *file = fopen(path, "wb");
    bool passed = file != NULL && fwrite(payload, 1, sizeof(payload), file) == sizeof(payload);
    if (file != NULL) {
        passed = fclose(file) == 0 && passed;
    }
    file = passed ? fopen(path, "rb") : NULL;
    passed = file != NULL && fread(readback, 1, sizeof(readback), file) == sizeof(readback) &&
             memcmp(readback, payload, sizeof(payload)) == 0;
    if (file != NULL) {
        passed = fclose(file) == 0 && passed;
    }
    if (passed) {
        passed = unlink(path) == 0;
    }
    const esp_err_t unmount_error = bsp_sdcard_unmount();
    if (unmount_error != ESP_OK) {
        passed = false;
        error = unmount_error;
    }
    factory_report_set(FACTORY_TEST_SDCARD,
                       passed ? FACTORY_STATUS_PASS : FACTORY_STATUS_FAIL,
                       passed ? "mount write read verify unmount passed" : "file verification failed");
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
    int16_t samples[AUDIO_FRAME_COUNT * 2];
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
    factory_report_set(FACTORY_TEST_SPEAKER, FACTORY_STATUS_NOT_RUN,
                       "tone sent; confirm sound then use mark");
    factory_report_print_one(FACTORY_TEST_SPEAKER);
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
    int16_t samples[AUDIO_FRAME_COUNT * 2];
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

static int command_camera_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t error = bsp_camera_start(NULL);
    if (error == ESP_OK) {
        const int file = open(BSP_CAMERA_DEVICE, O_RDONLY);
        if (file < 0) {
            error = ESP_ERR_NOT_FOUND;
        } else {
            close(file);
        }
    }
    const esp_err_t stop_error = bsp_camera_stop();
    if (error == ESP_OK && stop_error != ESP_OK) {
        error = stop_error;
    }
    if (error != ESP_OK) {
        report_error(FACTORY_TEST_CAMERA, error, "camera probe failed");
        return error;
    }
    factory_report_set(FACTORY_TEST_CAMERA, FACTORY_STATUS_PASS,
                       "sensor probed and DVP video node opened");
    factory_report_print_one(FACTORY_TEST_CAMERA);
    return ESP_OK;
}

static bool test_accepts_manual_result(factory_test_id_t test)
{
    return test == FACTORY_TEST_DISPLAY || test == FACTORY_TEST_RGB_LED ||
           test == FACTORY_TEST_SPEAKER;
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
        {.command = "rail", .help = "Inspect or explicitly control one TG28_SW rail.", .func = command_rail},
        {.command = "peripheral_power", .help = "Apply a complete peripheral power sequence.", .func = command_peripheral_power},
        {.command = "rtc_test", .help = "Read RX8130CE time and retained status flags.", .func = command_rtc_test},
        {.command = "rtc_set", .help = "Set and verify RX8130CE time: rtc_set YYYY-MM-DD HH:MM:SS WEEKDAY.", .func = command_rtc_set},
        {.command = "irq_test", .help = "Service and verify the shared PMIC/RTC interrupt line.", .func = command_irq_test},
        {.command = "typec_test", .help = "Read FUSB303B connection state without changing its role.", .func = command_type_c_test},
        {.command = "otg", .help = "Explicitly enable or disable USB source power.", .func = command_otg},
        {.command = "display_test", .help = "Show a four-color AMOLED inspection pattern.", .func = command_display_test},
        {.command = "touch_test", .help = "Require a touch in all four display quadrants.", .func = command_touch_test},
        {.command = "led_test", .help = "Show red, green, and blue on the addressable LED.", .func = command_led_test},
        {.command = "sdcard_test", .help = "Mount, write, verify, remove, and unmount a test file.", .func = command_sdcard_test},
        {.command = "speaker_test", .help = "Play a short low-level square-wave tone.", .func = command_speaker_test},
        {.command = "microphone_test", .help = "Capture audio and check for a non-zero signal.", .func = command_microphone_test},
        {.command = "camera_test", .help = "Probe the DVP sensor and open its ESP Video node.", .func = command_camera_test},
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
