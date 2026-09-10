/*
 * Candis-S31 standalone examples - physical input helper.
 *
 * BOOT key (GPIO61, external 10k pull-up, idle high) is polled every 5 ms
 * with 30 ms debounce; a hold of >= 800 ms reports the long press at the
 * threshold moment, the following release is swallowed (factory_input.c
 * pattern). PWR key events arrive on the TG28 shared IRQ line (GPIO2):
 * the BSP ISR notifies this task, the task drains the line with
 * bsp_shared_irq_service() and reports only the short-press bit
 * (INT_STATUS1 bit3); a PWR long press powers the board off in TG28
 * hardware and never reaches software.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "example_input.h"

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rx8130ce.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define INPUT_TASK_STACK_BYTES 3072
#define INPUT_TASK_PRIORITY    4
#define INPUT_POLL_PERIOD_MS   5

/* EVT1 schematic SW2: KEY_BOOT net drives GPIO61 with an external 10k
 * pull-up; the line idles high and is grounded while pressed. Internal
 * pulls stay disabled so the strap-safe external network is the only
 * bias (matches factory_input.c). */
#define BOOT_BUTTON_GPIO     GPIO_NUM_61
#define BOOT_DEBOUNCE_MS     30
#define BOOT_LONG_PRESS_MS   800

/* TG28_SW INT_STATUS1 low nibble latches the power-key flags; bit3 is
 * the short-press event. Only the short press is reported upward: the
 * long press is a hardware power-off. */
#define PWR_IRQ_BANK_INDEX   1
#define PWR_SHORT_PRESS_BIT  0x08

static const char *TAG = "example_input";

static example_input_cb_t s_callback;
static void *s_user;
static TaskHandle_t s_task;
static bool s_started;

/* Shared IRQ line ISR: the BSP masks the line on entry; only wake the
 * task, servicing happens in task context. */
static void shared_irq_isr(void *arg)
{
    (void)arg;
    if (s_task != NULL) {
        BaseType_t yield = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &yield);
        portYIELD_FROM_ISR(yield);
    }
}

static void input_report(example_input_event_t event)
{
    if (s_callback != NULL) {
        s_callback(event, s_user);
    }
}

/* 5 ms tick state machine for the BOOT key. */
static void boot_key_poll(void)
{
    typedef enum { BOOT_IDLE, BOOT_DEBOUNCING, BOOT_HELD } state_t;
    static state_t state = BOOT_IDLE;
    static int held_ms;
    static bool long_press_fired;

    const bool pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;

    switch (state) {
    case BOOT_IDLE:
        if (pressed) {
            state = BOOT_DEBOUNCING;
            held_ms = INPUT_POLL_PERIOD_MS;
        }
        break;
    case BOOT_DEBOUNCING:
        if (!pressed) {
            state = BOOT_IDLE;
        } else {
            held_ms += INPUT_POLL_PERIOD_MS;
            if (held_ms >= BOOT_DEBOUNCE_MS) {
                state = BOOT_HELD;
                long_press_fired = false;
            }
        }
        break;
    case BOOT_HELD:
        if (!pressed) {
            if (!long_press_fired) {
                input_report(EXAMPLE_INPUT_BOOT_SHORT);
            }
            state = BOOT_IDLE;
        } else {
            held_ms += INPUT_POLL_PERIOD_MS;
            if (!long_press_fired && held_ms >= BOOT_LONG_PRESS_MS) {
                /* Fire at the threshold so the action does not wait for
                 * the release; the release itself is swallowed. */
                long_press_fired = true;
                input_report(EXAMPLE_INPUT_BOOT_LONG);
            }
        }
        break;
    }
}

/* Drain the shared IRQ line and translate TG28 power-key flags. */
static void pwr_key_service(void)
{
    bsp_shared_irq_status_t irq = {0};
    const esp_err_t err = bsp_shared_irq_service(&irq);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "shared IRQ service failed: %s", esp_err_to_name(err));
    }
    if ((irq.pmic[PWR_IRQ_BANK_INDEX] & PWR_SHORT_PRESS_BIT) != 0) {
        input_report(EXAMPLE_INPUT_PWR_SHORT);
    }
    /* RX8130CE alarm flag (AF): the service drained it while releasing
     * the line; forward so an RTC example can observe it without touching
     * the IRQ registration. */
    if ((irq.rtc & RX8130CE_FLAG_AF) != 0) {
        input_report(EXAMPLE_INPUT_RTC_ALARM);
    }
}

static void input_task(void *arg)
{
    (void)arg;
    /* Keep the intended 5 ms poll when the app selects a 1 kHz tick, but
     * never pass a zero timeout to ulTaskNotifyTake on coarser configs. */
    const TickType_t poll_ticks = pdMS_TO_TICKS(INPUT_POLL_PERIOD_MS) > 0
                                      ? pdMS_TO_TICKS(INPUT_POLL_PERIOD_MS)
                                      : 1;
    for (;;) {
        /* The notify wait doubles as the 5 ms poll period. */
        if (ulTaskNotifyTake(pdTRUE, poll_ticks) > 0) {
            pwr_key_service();
        }
        boot_key_poll();
    }
}

esp_err_t example_input_start(example_input_cb_t cb, void *user)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_callback = cb;
    s_user = user;

    const gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   /* external 10k pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&boot_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BOOT key GPIO setup failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(input_task, "example_input", INPUT_TASK_STACK_BYTES, NULL,
                    INPUT_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Register the shared-IRQ callback only after the service task exists:
     * the BSP masks the line inside the ISR, and without the task nothing
     * would service + re-arm it, silently losing PWR key events. */
    err = bsp_shared_irq_register_callback(shared_irq_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shared IRQ registration failed: %s", esp_err_to_name(err));
        vTaskDelete(s_task);
        s_task = NULL;
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "input service started (BOOT=GPIO%d, PWR=shared IRQ)",
             BOOT_BUTTON_GPIO);
    return ESP_OK;
}
