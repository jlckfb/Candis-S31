/*
 * Candis-S31 player demo - physical input service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "input_service.h"

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "player_input";

#define TASK_STACK_BYTES 3072
#define TASK_PRIORITY    4
#define POLL_PERIOD_MS   5
#define BOOT_DEBOUNCE_MS 30
#define BOOT_LONG_MS     800
#define PWR_BANK_INDEX   1
#define PWR_SHORT_BIT    0x08

#define BOOT_BUTTON_GPIO GPIO_NUM_61

static input_cb_t s_callback;
static void *s_user;
static TaskHandle_t s_task;
static bool s_started;

static void report(input_event_t ev)
{
    if (s_callback != NULL) {
        s_callback(ev, s_user);
    }
}

static void boot_poll(void)
{
    typedef enum { IDLE, DEBOUNCE, HELD } state_t;
    static state_t state = IDLE;
    static int held_ms;
    static bool long_fired;
    const bool pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;

    switch (state) {
    case IDLE:
        if (pressed) {
            state = DEBOUNCE;
            held_ms = POLL_PERIOD_MS;
        }
        break;
    case DEBOUNCE:
        if (!pressed) {
            state = IDLE;
        } else {
            held_ms += POLL_PERIOD_MS;
            if (held_ms >= BOOT_DEBOUNCE_MS) {
                state = HELD;
                long_fired = false;
            }
        }
        break;
    case HELD:
        if (!pressed) {
            if (!long_fired) {
                report(INPUT_EV_BOOT_SHORT);
            }
            state = IDLE;
        } else {
            held_ms += POLL_PERIOD_MS;
            if (!long_fired && held_ms >= BOOT_LONG_MS) {
                long_fired = true;
                report(INPUT_EV_BOOT_LONG);
            }
        }
        break;
    }
}

static void pwr_service(void)
{
    bsp_shared_irq_status_t irq = {0};
    const esp_err_t err = bsp_shared_irq_service(&irq);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "shared IRQ service failed: %s", esp_err_to_name(err));
    }
    if ((irq.pmic[PWR_BANK_INDEX] & PWR_SHORT_BIT) != 0) {
        report(INPUT_EV_PWR_SHORT);
    }
}

static void input_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_PERIOD_MS)) > 0) {
            pwr_service();
        }
        boot_poll();
    }
}

static void shared_irq_isr(void *arg)
{
    (void)arg;
    if (s_task != NULL) {
        BaseType_t yield = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &yield);
        portYIELD_FROM_ISR(yield);
    }
}

esp_err_t input_start(input_cb_t cb, void *user)
{
    if (s_started) {
        return ESP_OK;
    }
    s_callback = cb;
    s_user = user;

    const gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&boot_cfg), TAG, "BOOT GPIO setup failed");

    if (xTaskCreate(input_task, "player_input", TASK_STACK_BYTES, NULL,
                    TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(bsp_shared_irq_register_callback(shared_irq_isr, NULL),
                        TAG, "shared IRQ register failed");
    s_started = true;
    return ESP_OK;
}
