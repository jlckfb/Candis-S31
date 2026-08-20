/*
 * Candis-S31 watch demo - power service implementation.
 *
 * Polls the TG28 every 2 s for battery/charge state and reports edges,
 * tracks UI idle time for the automatic screen-off, and owns the
 * review-locked sleep/shutdown sequences:
 *
 *   screen off: hold LVGL lock, stop invalidation + indev, unlock,
 *               bsp_display_enter_sleep(), CST820 wakeup() then
 *               enter_monitor_mode() (low_power_main.c 488-540 pattern).
 *   screen on:  exit_monitor_mode(), bsp_display_exit_sleep(), hold lock,
 *               re-enable invalidation + indev, full-screen invalidate,
 *               unlock.
 *   wake detection while off: 200 ms poll of BSP_TOUCH_INT (GPIO3) low;
 *               BOOT/PWR wake through svc_power_activity() = turn on.
 *   deep sleep: drain shared IRQ, RX8130CE alarm (no 32.768 kHz xtal on
 *               this board), EXT1 ANY_LOW on GPIO2 with rtc_gpio pull-up
 *               (low_power_main.c 861-879 recipe), bsp_power_safe_state(),
 *               esp_deep_sleep_start(). Wake is a fresh reset.
 *   shutdown:   bsp_power_safe_state() first, then VBUS guard (soft-PWROFF
 *               with VBUS attached reboots instead of cutting power),
 *               then bsp_pmic_power_off().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc_power.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "demo_board.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "rx8130ce.h"
#include "esp_lcd_touch_cst820.h"

#include "services/svc_storage.h"
#include "ui/ui_manager.h"

#define POWER_TASK_STACK_BYTES 4096
#define POWER_TASK_PRIORITY    4
#define POWER_TICK_MS          200
#define PMIC_POLL_MS           2000
#define CMD_QUEUE_DEPTH        8

#define CMD_SCREEN_OFF 1
#define CMD_SCREEN_ON  2

static const char *TAG = "svc_power";

static svc_power_cb_t s_callback;
static void *s_user;
static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_task;
static bool s_started;

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static svc_power_status_t s_status = { .percent = -1 };

static volatile int64_t s_last_activity_us;
static volatile int s_screen_timeout_s;
static volatile bool s_screen_off;
static bool s_touch_wake_confirm; /* require two low samples on GPIO3 */

/* ------------------------------------------------------------------ */
/* Status snapshot                                                     */
/* ------------------------------------------------------------------ */

void svc_power_get_status(svc_power_status_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

/* ------------------------------------------------------------------ */
/* Screen off / on sequences                                           */
/* ------------------------------------------------------------------ */

static void power_emit(const svc_power_status_t *snapshot, svc_power_event_t ev)
{
    if (s_callback != NULL) {
        s_callback(snapshot, ev, s_user);
    }
}

static void screen_off_run(void)
{
    if (s_screen_off) {
        return;
    }
    /* Stop UI refresh first so no new frame is queued while the panel
     * goes to sleep. The lock is held only around the LVGL flag flips. */
    lv_display_t *disp = lv_display_get_default();
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (ui_lock()) {
        if (disp != NULL) {
            lv_display_enable_invalidation(disp, false);
        }
        if (indev != NULL) {
            lv_indev_enable(indev, false);
        }
        ui_unlock();
    }

    const esp_err_t sleep_err = bsp_display_enter_sleep();
    if (sleep_err != ESP_OK) {
        ESP_LOGE(TAG, "display sleep-in failed: %s", esp_err_to_name(sleep_err));
    }

    /* bsp_display_enter_sleep() parks the CST820 in its deep sleep, which
     * cannot be woken by touch; restore the touch-wakeable standby via the
     * reset cycle (low_power_main.c display_and_touch_sleep pattern). */
    esp_lcd_touch_handle_t touch = bsp_touch_get_handle();
    if (touch != NULL) {
        const esp_err_t wake_err = esp_lcd_touch_cst820_wakeup(touch);
        if (wake_err != ESP_OK) {
            ESP_LOGW(TAG, "CST820 wakeup failed: %s", esp_err_to_name(wake_err));
        }
        const esp_err_t mon_err = esp_lcd_touch_cst820_enter_monitor_mode(touch);
        if (mon_err != ESP_OK) {
            ESP_LOGW(TAG, "CST820 monitor mode failed: %s", esp_err_to_name(mon_err));
        }
    }

    s_screen_off = true;
    s_touch_wake_confirm = false;
    ESP_LOGI(TAG, "screen off");
    svc_power_status_t snapshot;
    svc_power_get_status(&snapshot);
    power_emit(&snapshot, SVC_POWER_EV_SCREEN_OFF);
}

static void screen_on_run(void)
{
    if (!s_screen_off) {
        return;
    }
    esp_lcd_touch_handle_t touch = bsp_touch_get_handle();
    if (touch != NULL) {
        const esp_err_t mon_err = esp_lcd_touch_cst820_exit_monitor_mode(touch);
        if (mon_err != ESP_OK) {
            ESP_LOGW(TAG, "CST820 monitor exit failed: %s", esp_err_to_name(mon_err));
        }
    }
    const esp_err_t wake_err = bsp_display_exit_sleep();
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "display sleep-out failed: %s", esp_err_to_name(wake_err));
    }

    s_screen_off = false;
    s_last_activity_us = esp_timer_get_time();

    lv_display_t *disp = lv_display_get_default();
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (ui_lock()) {
        if (disp != NULL) {
            lv_display_enable_invalidation(disp, true);
        }
        if (indev != NULL) {
            lv_indev_enable(indev, true);
        }
        lv_obj_invalidate(lv_screen_active()); /* full redraw on next cycle */
        ui_unlock();
    }
    ESP_LOGI(TAG, "screen on");
    svc_power_status_t snapshot;
    svc_power_get_status(&snapshot);
    power_emit(&snapshot, SVC_POWER_EV_SCREEN_ON);
}

/* ------------------------------------------------------------------ */
/* Public controls                                                     */
/* ------------------------------------------------------------------ */

void svc_power_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_screen_off && s_cmd_queue != NULL) {
        /* Any user activity while the screen is off means "wake up"; the
         * key/touch that caused it is swallowed by the caller. */
        const uint32_t cmd = CMD_SCREEN_ON;
        xQueueSend(s_cmd_queue, &cmd, 0);
    }
}

void svc_power_set_screen_timeout(int seconds)
{
    s_screen_timeout_s = seconds > 0 ? seconds : 0;
    svc_power_activity();
}

esp_err_t svc_power_screen_off(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t cmd = CMD_SCREEN_OFF;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

esp_err_t svc_power_screen_on(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t cmd = CMD_SCREEN_ON;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

bool svc_power_is_screen_off(void)
{
    return s_screen_off;
}

/* ------------------------------------------------------------------ */
/* PMIC polling and charge edges                                       */
/* ------------------------------------------------------------------ */

static void pmic_poll(void)
{
    bsp_pmic_status_t pmic = {0};
    if (bsp_pmic_get_status(&pmic) != ESP_OK) {
        return;
    }

    svc_power_status_t prev;
    portENTER_CRITICAL(&s_status_lock);
    prev = s_status;
    s_status.battery_mv = pmic.battery_mv;
    s_status.percent = pmic.battery_present ? pmic.battery_percent : -1;
    s_status.present = pmic.battery_present;
    s_status.vbus = pmic.vbus_present;
    s_status.charging = pmic.charging;
    s_status.charge_done = pmic.charge_done;
    svc_power_status_t cur = s_status;
    portEXIT_CRITICAL(&s_status_lock);

    svc_power_event_t ev = SVC_POWER_EV_UPDATE;
    if ((!prev.vbus && cur.vbus) || (!prev.charging && cur.charging)) {
        ev = SVC_POWER_EV_CHARGE_START;
    } else if (!prev.charge_done && cur.charge_done) {
        ev = SVC_POWER_EV_CHARGE_DONE;
    } else if (prev.vbus && !cur.vbus) {
        ev = SVC_POWER_EV_VBUS_OFF;
    }
    power_emit(&cur, ev);
}

/* ------------------------------------------------------------------ */
/* Power task                                                          */
/* ------------------------------------------------------------------ */

static void power_task(void *arg)
{
    (void)arg;
    int pmic_ticks = 0;
    for (;;) {
        uint32_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd == CMD_SCREEN_OFF) {
                screen_off_run();
            } else if (cmd == CMD_SCREEN_ON) {
                screen_on_run();
            }
        }

        if (s_screen_off) {
            /* CST820 monitor mode pulses INT low on touch. Require two
             * consecutive low samples so a residual pulse right after
             * sleep-in cannot bounce the screen straight back on. */
            if (gpio_get_level(BSP_TOUCH_INT) == 0) {
                if (s_touch_wake_confirm) {
                    s_touch_wake_confirm = false;
                    screen_on_run();
                } else {
                    s_touch_wake_confirm = true;
                }
            } else {
                s_touch_wake_confirm = false;
            }
        } else {
            const int timeout_s = s_screen_timeout_s;
            if (timeout_s > 0) {
                const int64_t idle_us =
                    esp_timer_get_time() - s_last_activity_us;
                if (idle_us >= (int64_t)timeout_s * 1000000) {
                    ESP_LOGI(TAG, "idle %ds, turning screen off", timeout_s);
                    screen_off_run();
                }
            }
        }

        if (++pmic_ticks >= PMIC_POLL_MS / POWER_TICK_MS) {
            pmic_ticks = 0;
            pmic_poll();
        }
        vTaskDelay(pdMS_TO_TICKS(POWER_TICK_MS));
    }
}

/* ------------------------------------------------------------------ */
/* Safe-shutdown chain hook                                            */
/* ------------------------------------------------------------------ */

/* Application protocol teardown registered with the BSP safe-state chain.
 * svc_power itself owns no protocol owner; unmount the TF card best-effort
 * so the FAT is left clean while the supply is still present. Display,
 * audio and network owners are handled by the BSP-level rail/pin parking
 * (a richer app callback can be layered on later without changing this
 * registration). */
static esp_err_t power_safe_shutdown_cb(void *ctx)
{
    (void)ctx;
    if (svc_storage_mounted()) {
        const esp_err_t err = bsp_sdcard_unmount();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "safe-state TF unmount failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Deep sleep                                                          */
/* ------------------------------------------------------------------ */

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t lengths[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12) {
        return 31;
    }
    if (month == 2 &&
            ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return lengths[month - 1];
}

/* Advance a calendar value by whole minutes, carrying into days/months
 * (low_power_main.c time_add_minutes). */
static void time_add_minutes(rx8130ce_time_t *time, uint16_t minutes)
{
    uint32_t total = (uint32_t)time->minute + minutes;
    time->minute = (uint8_t)(total % 60U);
    uint32_t hours = (uint32_t)time->hour + total / 60U;
    time->hour = (uint8_t)(hours % 24U);
    uint32_t days = hours / 24U;
    while (days-- > 0) {
        const uint8_t length = days_in_month(time->year, time->month);
        if (time->day < length) {
            time->day++;
        } else {
            time->day = 1;
            if (time->month == 12) {
                time->month = 1;
                time->year++;
            } else {
                time->month++;
            }
        }
        time->weekday = (uint8_t)((time->weekday + 1U) % 7U);
    }
}

/* Arm the RX8130CE alarm wake_minutes from now. The board carries no
 * 32.768 kHz crystal, so the RTC alarm is the only timed wake path that
 * survives deep sleep. Mirrors low_power_main.c arm_rtc_wakeup_alarm(). */
static esp_err_t arm_rtc_wakeup_alarm(int wake_minutes)
{
    bsp_rtc_time_t bsp_now = {0};
    bsp_rtc_status_t status = {0};
    esp_err_t err = bsp_rtc_get_time(&bsp_now, &status);
    if (err != ESP_OK) {
        return err;
    }
    if (!status.time_valid) {
        /* VLF=1: calendar and alarm registers are meaningless; refusing is
         * safer than sleeping on an alarm that will never fire. */
        ESP_LOGE(TAG, "RTC time invalid (VLF set), cannot arm alarm");
        return ESP_ERR_INVALID_STATE;
    }

    const rx8130ce_time_t now = {
        .year = bsp_now.year,
        .month = bsp_now.month,
        .day = bsp_now.day,
        .weekday = bsp_now.weekday,
        .hour = bsp_now.hour,
        .minute = bsp_now.minute,
        .second = bsp_now.second,
    };
    rx8130ce_time_t target = now;
    time_add_minutes(&target, (uint16_t)wake_minutes);

    bsp_rtc_alarm_t alarm = {0};
    err = rx8130ce_alarm_from_time(&now, &target, &alarm);
    if (err != ESP_OK) {
        return err;
    }
    err = bsp_rtc_set_alarm(&alarm);
    if (err != ESP_OK) {
        return err;
    }
    err = bsp_rtc_alarm_irq_enable(true);
    if (err != ESP_OK) {
        return err;
    }
    /* Clear only the latched alarm flag before draining the wire-ORed
     * line, then require the line released. */
    err = bsp_rtc_get_and_clear_alarm_flag(NULL);
    if (err != ESP_OK) {
        return err;
    }
    bsp_shared_irq_status_t irq = {0};
    (void)bsp_shared_irq_service(&irq);
    if (!irq.line_released) {
        ESP_LOGE(TAG, "shared IRQ line still asserted after alarm arm");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "RTC alarm armed: +%d min", wake_minutes);
    return ESP_OK;
}

/* Restore panel + refresh state when a deep-sleep attempt is refused
 * after the panel was already parked at entry. */
static void deep_sleep_abort_restore(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (ui_lock()) {
        if (disp != NULL) {
            lv_display_enable_invalidation(disp, true);
        }
        lv_obj_invalidate(lv_screen_active());
        ui_unlock();
    }
    (void)bsp_display_exit_sleep();
}

esp_err_t svc_power_deep_sleep(int wake_after_min)
{
    if (wake_after_min < 0 || wake_after_min > 1440) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Park the panel cleanly while its rails are still up (skip when the
     * screen is already off). Refresh is stopped so LVGL does not queue
     * new frames during the transition. */
    if (!s_screen_off) {
        lv_display_t *disp = lv_display_get_default();
        if (ui_lock()) {
            if (disp != NULL) {
                lv_display_enable_invalidation(disp, false);
            }
            ui_unlock();
        }
        const esp_err_t sleep_err = bsp_display_enter_sleep();
        if (sleep_err != ESP_OK) {
            ESP_LOGW(TAG, "display sleep-in before deep sleep failed: %s",
                     esp_err_to_name(sleep_err));
        }
    }

    /* Drain the shared line first so no stale edge is captured by EXT1. */
    bsp_shared_irq_status_t irq = {0};
    for (int attempt = 0; attempt < 3; ++attempt) {
        (void)bsp_shared_irq_service(&irq);
        if (irq.line_released) {
            break;
        }
    }
    if (!irq.line_released) {
        ESP_LOGE(TAG, "shared IRQ line busy, deep sleep refused");
        deep_sleep_abort_restore();
        return ESP_ERR_TIMEOUT;
    }

    bool alarm_armed = false;
    if (wake_after_min > 0) {
        const esp_err_t alarm_err = arm_rtc_wakeup_alarm(wake_after_min);
        if (alarm_err != ESP_OK) {
            ESP_LOGE(TAG, "RTC alarm arm failed: %s", esp_err_to_name(alarm_err));
            deep_sleep_abort_restore();
            return alarm_err;
        }
        alarm_armed = true;
    } else {
        /* Key-only wake: gate the alarm off the line. */
        (void)bsp_rtc_alarm_irq_enable(false);
    }

    /* EXT1 ANY_LOW recipe from low_power_main.c: GPIO2 is pulled up through
     * R31 to the always-on TG28_VRTC rail; enable the internal pull-up too
     * so the pad keeps a defined level. A floating EXT1 pin keeps the chip
     * in deep sleep forever. */
    esp_err_t ext1_err = rtc_gpio_init(BSP_PMIC_RTC_INT);
    if (ext1_err == ESP_OK) {
        ext1_err = rtc_gpio_set_direction(BSP_PMIC_RTC_INT,
                                          RTC_GPIO_MODE_INPUT_ONLY);
    }
    if (ext1_err == ESP_OK) {
        ext1_err = rtc_gpio_pullup_en(BSP_PMIC_RTC_INT);
    }
    if (ext1_err == ESP_OK) {
        ext1_err = esp_sleep_enable_ext1_wakeup_io(BIT(BSP_PMIC_RTC_INT),
                                                   ESP_EXT1_WAKEUP_ANY_LOW);
    }
    if (ext1_err != ESP_OK) {
        ESP_LOGE(TAG, "EXT1 wake arm failed: %s", esp_err_to_name(ext1_err));
        if (!alarm_armed) {
            /* No wake source would remain armed: deep sleep would be
             * permanent. Refuse it. */
            deep_sleep_abort_restore();
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* Peripheral power-down: registered safe-shutdown callback, USB host
     * stop, pin parking and rail removal. */
    const esp_err_t safe_err = bsp_power_safe_state();
    if (safe_err != ESP_OK) {
        ESP_LOGW(TAG, "safe-state reported: %s (continuing to sleep)",
                 esp_err_to_name(safe_err));
    }

    ESP_LOGW(TAG, "entering deep sleep (wake: %s)",
             alarm_armed ? "RTC alarm or PWR key" : "PWR key only");
    vTaskDelay(pdMS_TO_TICKS(50)); /* let the log drain */
    esp_deep_sleep_start();

    /* Not reached: waking from deep sleep reboots the chip. */
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Shutdown                                                            */
/* ------------------------------------------------------------------ */

esp_err_t svc_power_shutdown(void)
{
    ESP_LOGW(TAG, "shutdown requested");

    /* VBUS guard first: with USB power present the TG28 soft-PWROFF
     * reboots the board instead of cutting power, so refuse BEFORE the
     * safe-state chain drops any rail (the system keeps running). */
    bsp_pmic_status_t pmic = {0};
    if (bsp_pmic_get_status(&pmic) == ESP_OK && pmic.vbus_present) {
        ESP_LOGW(TAG, "VBUS present: power-off refused, unplug USB first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Safe-shutdown chain: the registered application callback, then BSP
     * pin parking and rail removal. */
    const esp_err_t safe_err = bsp_power_safe_state();
    if (safe_err != ESP_OK) {
        ESP_LOGE(TAG, "safe-state failed: %s (continuing, rails may be partial)",
                 esp_err_to_name(safe_err));
    }

    return bsp_pmic_power_off();
}

/* ------------------------------------------------------------------ */
/* Start                                                               */
/* ------------------------------------------------------------------ */

esp_err_t svc_power_start(svc_power_cb_t cb, void *user)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_callback = cb;
    s_user = user;
    s_screen_timeout_s = demo_settings()->screen_timeout_s;
    s_last_activity_us = esp_timer_get_time();

    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(uint32_t));
    if (s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Register before any code path can reach bsp_power_safe_state(). */
    const esp_err_t cb_err =
        bsp_power_set_safe_shutdown_callback(power_safe_shutdown_cb, NULL);
    if (cb_err != ESP_OK) {
        ESP_LOGW(TAG, "safe-shutdown callback registration failed: %s",
                 esp_err_to_name(cb_err));
    }

    if (xTaskCreate(power_task, "svc_power", POWER_TASK_STACK_BYTES, NULL,
                    POWER_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "power service started (screen timeout %ds)",
             (int)s_screen_timeout_s);
    return ESP_OK;
}
