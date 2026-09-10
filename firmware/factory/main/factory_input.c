/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_convert.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

#define BUTTON_TEST_TIMEOUT_S 20

/* EVT1 schematic SW2: the KEY_BOOT net drives GPIO61 and has an external
 * 10k pull-up, so the line idles high and is grounded while pressed. */
#define BOOT_BUTTON_GPIO GPIO_NUM_61

/* TG28_SW INT_STATUS1 low nibble latches the power-key edge/press flags. */
#define TG28_POWER_KEY_IRQ_MASK 0x0f

static led_indicator_handle_t s_led;

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
        factory_report_error(FACTORY_TEST_BUTTONS, error, "BOOT key GPIO setup failed");
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
        if (error == ESP_OK && (pmic_irq[1] & TG28_POWER_KEY_IRQ_MASK) != 0) {
            power_seen = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    printf("PWR key %s\n", power_seen ? "detected" : "not detected");

    if (error != ESP_OK) {
        factory_report_error(FACTORY_TEST_BUTTONS, error, "TG28_SW interrupt read failed");
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
            factory_report_error(FACTORY_TEST_RGB_LED,
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
            factory_report_error(FACTORY_TEST_RGB_LED, error, "RGB update failed");
            return error;
        }
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    led_indicator_set_on_off(s_led, false);
    const char answer = factory_console_ask_operator(
                            "rgb_led", "Did the LED cycle red, green, blue?",
                            OPERATOR_PROMPT_TIMEOUT_S);
    factory_report_operator_verdict(FACTORY_TEST_RGB_LED, answer,
                            "operator confirmed red green blue sequence",
                            "operator rejected the LED sequence",
                            "red green blue sequence sent; inspect LED then use mark");
    return ESP_OK;
}


esp_err_t factory_led_stop(void)
{
    if (s_led == NULL) {
        return ESP_OK;
    }
    const esp_err_t error = led_indicator_delete(s_led);
    if (error == ESP_OK) {
        s_led = NULL;
    }
    return error;
}

esp_err_t factory_input_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "buttons", .help = "Wait for BOOT (GPIO61) and PWR (TG28_SW IRQ) key presses.", .func = command_buttons_test},
        {.command = "led_test", .help = "Show red, green, and blue on the addressable LED.", .func = command_led_test},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
