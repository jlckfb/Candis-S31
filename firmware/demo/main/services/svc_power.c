/*
 * Candis-S31 watch demo - power service implementation.
 *
 * Polls the TG28 every 2 s for battery/charge state and reports edges,
 * tracks UI idle time for the automatic screen-off, and owns the
 * review-locked sleep/shutdown sequences:
 *
 *   screen off: hold LVGL lock, stop invalidation + indev, unlock,
 *               bsp_display_enter_sleep_panel(). The CST820 is left
 *               untouched (no sleep command, no reset): it auto-enters
 *               standby and asserts INT on touch. The radios are parked
 *               for the dark period (svc_net_suspend_rf: WiFi driver
 *               stop, BLE scan cancel) and restored on wake.
 *   screen on:  bsp_display_exit_sleep_panel(), hold lock, re-enable
 *               invalidation + indev, full-screen invalidate, unlock.
 *               Zero touch-controller resets (a reset on a touched panel
 *               poisons the baseline and latches phantom touches).
 *               lv_indev_wait_release() is armed only when the CST820
 *               still reports a physical finger; an unconditional arm
 *               would swallow the first real touch after a no-finger
 *               (key) wake (AGENT-AI.md §0F / §8-22).
 *   wake detection while off: 50 ms poll of BSP_TOUCH_INT (GPIO3) low
 *               plus a direct CST820 report poll over I2C (the indev is
 *               disabled while off; the INT pulse width in the
 *               screen-off power tier is unverified on EVT), both with
 *               a two consecutive-sample confirm; BOOT/PWR wake through
 *               svc_power_activity() = turn on.
 *   deep sleep: drain shared IRQ, RX8130CE alarm (no 32.768 kHz xtal on
 *               this board), EXT1 ANY_LOW on GPIO2 with rtc_gpio pull-up
 *               (low_power_main.c 861-879 recipe), bsp_power_safe_state(),
 *               esp_deep_sleep_start(). Wake is a fresh reset.
 *   shutdown:   VBUS guard first (soft-PWROFF with VBUS attached reboots
 *               instead of cutting power), then bsp_power_safe_state(),
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
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "rx8130ce.h"

#include "services/svc_audio.h"
#include "services/svc_net.h"
#include "services/svc_storage.h"
#include "ui/ui_manager.h"

#define POWER_TASK_STACK_BYTES 4096
#define POWER_TASK_PRIORITY    4
#define POWER_TICK_MS          50   /* INT wake poll cadence while screen off */
#define PMIC_POLL_MS           2000
#define CMD_QUEUE_DEPTH        8

#define CMD_SCREEN_OFF 1
#define CMD_SCREEN_ON  2
#define CMD_SOURCE_VERIFIED 3
#define CMD_SOURCE_UNSAFE 4
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

/* Touch-wake diagnostics (power task only), reset at every screen-off:
 * raw GPIO3 level transitions seen between the 50 ms polls plus the
 * direct CST820 report-poll results. Printed only while advancing and
 * once at wake, so a touch that fails to wake the panel can be
 * classified from the log: int edges > 0 -> the CST820 does assert INT
 * but too briefly for a 50 ms level poll; edges == 0 while i2c press
 * climbs -> no INT in the screen-off power tier at all. */
#define POWER_DIAG_TICKS 100   /* 5 s between diagnostic prints */
static int s_wake_int_edges;
static int s_wake_int_low;
static int s_wake_i2c_pressed;
static int s_wake_i2c_fail;
static int s_wake_int_last_level = -1;
static int s_wake_diag_last_edges;
static int s_wake_diag_last_pressed;
static int s_wake_diag_last_fail;
static int s_wake_diag_ticks;

/* Bounded wake-failure retry (power task only): a failed SLPOUT is
 * retried every 500 ms, at most 3 times, then falls back to waiting for
 * the next user event. Both fields are touched on the power task only. */
#define POWER_WAKE_RETRY_MAX   3
#define POWER_WAKE_RETRY_MS    500
static int s_wake_retry_count;
static int64_t s_wake_retry_due_us;

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

/* AGENT-AI.md §0F / §8 rule 22: never arm lv_indev_wait_release()
 * unconditionally on a wake/restore path. The CST820 input device runs in
 * LV_INDEV_MODE_EVENT (vendor esp-bsp esp_lvgl_port_touch.c: event mode is
 * set whenever the INT GPIO is configured), so after a RELEASED report the
 * LVGL read timer stays paused until the next INT edge. An armed
 * wait-release with no finger down is therefore cleared only by the NEXT
 * real press, and that press is dropped by indev_pointer_proc() - the
 * first touch after a no-finger wake is swallowed (reproduced on hardware
 * by factory, fixed the same way in BSP display_lvgl_touch_resume_locked()).
 * Read the CST820 physical state first and arm wait-release only when a
 * finger is still down. A failed read cannot prove "no finger": keep the
 * historical arm-on-restore behaviour (protects against a stale PRESSED
 * becoming a phantom click) rather than silently dropping the guard. */
static void power_indev_wait_release_if_pressed(lv_indev_t *indev)
{
    if (indev == NULL) {
        return;
    }
    bool pressed = true;
    esp_lcd_touch_handle_t touch = bsp_touch_get_handle();
    if (touch != NULL) {
        uint8_t point_count = 0;
        esp_lcd_touch_point_data_t point = {0};
        if (esp_lcd_touch_read_data(touch) == ESP_OK &&
                esp_lcd_touch_get_data(touch, &point, &point_count, 1) == ESP_OK) {
            pressed = point_count > 0;
        } else {
            ESP_LOGW(TAG, "touch state read failed, keeping wait-release");
        }
    }
    if (pressed) {
        lv_indev_wait_release(indev);
    }
}

/* One screen-off touch sample. Reads the CST820 report registers over
 * I2C - the LVGL indev is disabled while the screen is off, so nothing
 * else polls the controller - and treats a low INT level as a wake
 * signal too. The I2C poll exists because the CST820 INT behaviour in
 * the screen-off power tier is unverified on EVT (driver provenance
 * note in esp_lcd_touch_cst820.c): a short INT pulse can fall entirely
 * between the 50 ms level samples, while the report registers answer
 * regardless of INT shape. Power task only. */
static bool power_touch_sample(void)
{
    const int level = gpio_get_level(BSP_TOUCH_INT);
    if (level != s_wake_int_last_level) {
        if (s_wake_int_last_level >= 0) {
            ++s_wake_int_edges;
        }
        s_wake_int_last_level = level;
    }
    if (level == 0) {
        ++s_wake_int_low;
    }

    esp_lcd_touch_handle_t touch = bsp_touch_get_handle();
    if (touch == NULL) {
        return level == 0;
    }
    bool pressed = false;
    if (esp_lcd_touch_read_data(touch) == ESP_OK) {
        uint8_t point_count = 0;
        esp_lcd_touch_point_data_t point = {0};
        if (esp_lcd_touch_get_data(touch, &point, &point_count, 1) == ESP_OK) {
            pressed = point_count > 0;
        } else {
            ++s_wake_i2c_fail;
        }
    } else {
        ++s_wake_i2c_fail;
    }
    if (pressed) {
        ++s_wake_i2c_pressed;
    }
    return pressed || level == 0;
}

static void power_wake_diag_log(const char *when)
{
    ESP_LOGI(TAG, "wake diag (%s): int edges=%d low=%d i2c press=%d i2c fail=%d",
             when, s_wake_int_edges, s_wake_int_low, s_wake_i2c_pressed,
             s_wake_i2c_fail);
}

static void screen_off_run(void)
{

    if (s_screen_off) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    lv_indev_t *indev = bsp_display_get_input_dev();

    /* Freeze LVGL first so no new frame or input races the panel
     * transition, then release the lock: the hardware sequence must not
     * run under it. */
    if (!ui_lock()) {
        ESP_LOGW(TAG, "screen off skipped: LVGL lock unavailable");
        return;
    }
    if (disp != NULL) {
        lv_display_enable_invalidation(disp, false);
    }
    if (indev != NULL) {
        lv_indev_enable(indev, false);
        /* §8 rule 21 counterpart: where the BSP swaps in a RELEASED-only
         * read callback it also lv_indev_reset()s the device so no pressed/
         * scroll target survives the transition. The demo blocks reads by
         * disabling the indev instead (the CST820 never enters hardware
         * sleep here), but the reset is still required: without it a stale
         * PRESSED could outlive screen-off, and once wake only conditionally
         * arms wait-release (§8-22) nothing else would contain it. */
        lv_indev_reset(indev, NULL);
    }
    ui_unlock();

    /* Panel-only sleep: the touch controller is deliberately NOT put to
     * deep sleep (that state cannot be touch-woken) and NOT reset. Left
     * running, the CST820 drops into its auto-standby tier on its own and
     * pulses INT on touch, which is what the poll below waits for. Every
     * hard reset in the old sequence (wakeup + "monitor mode" enter) was
     * both ineffective and a phantom-touch source on wake. */
    /* The hardware sequence (backlight off, SLPIN, then a >=120 ms settle
     * delay) runs OUTSIDE the LVGL lock: holding the lock across it
     * blocked taskLVGL for the whole transition. Invalidation is already
     * disabled, so nothing new renders while the panel parks. */
    const esp_err_t sleep_err = bsp_display_enter_sleep_panel();
    if (sleep_err != ESP_OK) {
        ESP_LOGE(TAG, "display sleep-in failed: %s", esp_err_to_name(sleep_err));
        /* Roll the LVGL freeze back (same recovery the old in-lock path
         * performed). A failed lock here leaves the UI frozen until the
         * next screen-off attempt, which the idle-timeout tick retries. */
        if (!ui_lock()) {
            ESP_LOGE(TAG, "screen off rollback skipped: LVGL lock unavailable");
            return;
        }
        if (disp != NULL) {
            lv_display_enable_invalidation(disp, true);
        }
        if (indev != NULL) {
            lv_indev_enable(indev, true);
            power_indev_wait_release_if_pressed(indev);
        }
        ui_unlock();
        return;
    }

    s_screen_off = true;
    s_touch_wake_confirm = false;
    s_wake_retry_count = 0;
    s_wake_retry_due_us = 0;
    s_wake_int_edges = 0;
    s_wake_int_low = 0;
    s_wake_i2c_pressed = 0;
    s_wake_i2c_fail = 0;
    s_wake_int_last_level = gpio_get_level(BSP_TOUCH_INT);
    s_wake_diag_last_edges = 0;
    s_wake_diag_last_pressed = 0;
    s_wake_diag_last_fail = 0;
    s_wake_diag_ticks = 0;
    /* Park the radios for the dark period: a running BLE scan and the
     * WiFi driver dominate the screen-off power budget. Queued through
     * svc_net's own task; failures are logged by that service and never
     * block or fail the sleep sequence. */
    (void)svc_net_suspend_rf();
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
    lv_display_t *disp = lv_display_get_default();
    lv_indev_t *indev = bsp_display_get_input_dev();

    /* Wake the panel (SLPOUT, >=120 ms settle, backlight) OUTSIDE the LVGL
     * lock, mirroring screen_off_run(): holding the lock across the wake
     * transition blocked taskLVGL for the whole sequence. While the screen
     * is off, invalidation is disabled, so nothing new renders during the
     * wake and any residual flush degrades to an unsynchronized write. */
    const esp_err_t wake_err = bsp_display_exit_sleep_panel();
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "display sleep-out failed: %s", esp_err_to_name(wake_err));
        /* Bounded auto-retry from the power task (no locks held here):
         * the panel may miss one SLPOUT right after a hot plug. After
         * POWER_WAKE_RETRY_MAX attempts give up and wait for the next
         * user event; s_screen_off stays set either way. */
        if (s_wake_retry_count < POWER_WAKE_RETRY_MAX) {
            ++s_wake_retry_count;
            s_wake_retry_due_us =
                esp_timer_get_time() + POWER_WAKE_RETRY_MS * INT64_C(1000);
            ESP_LOGW(TAG, "wake retry %d/%d scheduled",
                     s_wake_retry_count, POWER_WAKE_RETRY_MAX);
        } else {
            s_wake_retry_count = 0;
            s_wake_retry_due_us = 0;
            ESP_LOGE(TAG, "wake retries exhausted, waiting for user event");
        }
        return;
    }

    if (!ui_lock()) {
        ESP_LOGW(TAG, "screen on skipped: LVGL lock unavailable");
        return;
    }
    if (disp != NULL) {
        lv_display_enable_invalidation(disp, true);
    }
    /* Zero touch activity on wake: the CST820 stayed powered and the touch
     * which woke the panel may still be latched in LVGL's indev state. */
    if (indev != NULL) {
        power_indev_wait_release_if_pressed(indev);
        lv_indev_enable(indev, true);
    }
    lv_obj_invalidate(lv_screen_active()); /* full redraw on next cycle */
    ui_unlock();

    s_screen_off = false;
    /* Radios back on: svc_net restarts the WiFi driver (reconnecting
     * when NVS credentials exist) and a BLE scan that was parked. */
    (void)svc_net_resume_rf();
    s_wake_retry_count = 0;
    s_wake_retry_due_us = 0;
    s_last_activity_us = esp_timer_get_time();
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

esp_err_t svc_power_set_external_source_verified(bool verified)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t cmd = verified ? CMD_SOURCE_VERIFIED : CMD_SOURCE_UNSAFE;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Charge controller (service task, 2 s cadence)                       */
/* ------------------------------------------------------------------ */

/* Exact REG62 steps. The controller writes a verified ceiling; the actual
 * charge current is decided by the TG28 power path and may be lower at any
 * time (VINDPM back-off), so nothing here may be reported as a measured
 * current. The 500 mA ceiling is pending cell/connector thermal sign-off;
 * there is no cell-temperature loop (fixed TS divider, no NTC). */
static const uint16_t k_charge_targets[] = {50, 100, 200, 300, 400, 500};
#define CHARGE_TARGET_COUNT       (sizeof(k_charge_targets) / sizeof(k_charge_targets[0]))
/* Timestamps (esp_timer_get_time, monotonic us) rather than poll counters:
 * exactly one full 2 s settle at 50 mA after a battery is first seen in
 * the VBUS session, then the first rise; every later rise is exactly 4 s
 * after the previous successful one. */
#define CHARGE_SETTLE_US          (2 * 1000000LL)
#define CHARGE_STEP_US            (4 * 1000000LL)
#define CHARGE_VBAT_MIN_MV        3000
#define CHARGE_VBAT_HEADROOM_MV   50
#define CHARGE_VBAT_ABS_MAX_MV    4450
#define CHARGE_VBUS_MARGIN_MV     320
#define CHARGE_MAX_DROOPS         2
#define CHARGE_MAX_PMIC_FAILS     3

typedef struct {
    svc_power_charge_phase_t phase;
    int idx;            /* index into k_charge_targets */
    int target_ma;      /* last readback-verified REG62 value */
    int64_t settle_start_us; /* first poll with a battery present; 0 = none */
    int64_t last_change_us; /* last verified target write */
    int droops;         /* VBUS droop events this session */
    int pmic_fails;     /* consecutive failed controller polls */
    uint16_t vbus_mv;   /* latest gate VBUS ADC sample (evidence) */
    uint16_t vindpm_mv; /* configured values, read once per session */
    uint16_t vchg_mv;
    bool limits_loaded;
    /* Per-session external-source confirmation (user action only); false
     * at boot, on unplug and on memset - never persisted. Gates only the
     * REG62 charge ceiling (200 safe / 500 confirmed); REG16 is a fixed
     * relaxed 2000 mA board default and is never switched by this. */
    bool source_verified;
    /* FAULT only: false from the moment collapse begins, true only after
     * a dedicated set-50 + get-50 exact verification succeeded. The
     * cached target/idx cannot decide this - a set whose readback I2C
     * failed may have reached the hardware while the software still
     * records the old value. */
    bool fault_park_verified;
} charge_state_t;

static charge_state_t s_chg;

/* PC-safe default: without a per-session user confirmation the ramp stops
 * at 200 mA - risk reduction for computer debugging, NOT a hard
 * total-input guarantee: REG16 stays at the relaxed 2000 mA default and
 * the TG28 hardware backs the charge current off under VINDPM/input
 * limit while the system load keeps priority. Firmware cannot classify a
 * PC port vs a charger on C1 - the confirmation is mandatory and never
 * assumed. */
#define CHARGE_SAFE_CEILING_MA 200

static int charge_ceiling_idx(void)
{
    if (s_chg.source_verified) {
        return (int)CHARGE_TARGET_COUNT - 1;
    }
    for (int i = 0; i < (int)CHARGE_TARGET_COUNT; ++i) {
        if (k_charge_targets[i] >= CHARGE_SAFE_CEILING_MA) {
            return i;
        }
    }
    return 0;
}

/* Power-task handler for the public source-verification control. It only
 * gates the REG62 charge ceiling (200 safe / 500 confirmed); REG16 is a
 * fixed relaxed 2000 mA board default and is never switched here.
 *
 * enable:  publish the confirmed ceiling; RAMP resumes the gated 4 s
 *          steps up to 500 while VBUS/battery/gates allow. Ignored in
 *          FAULT (the PMIC is already misbehaving).
 * disable: latch the reobservation state BEFORE the park attempt (same
 *          ordering as battery removal) and drive the verified target to
 * <= 200; TRICKLE owns retries and never rises above 200. */
static bool charge_write_target(int idx);

static void source_verified_command(bool enable)
{
    if (enable) {
        if (s_chg.phase == SVC_POWER_CHARGE_FAULT) {
            ESP_LOGW(TAG, "source verify ignored in FAULT");
            return;
        }
        if (!s_chg.source_verified) {
            s_chg.source_verified = true;
            ESP_LOGI(TAG, "external source confirmed: charge ceiling %d mA "
                     "(input limit stays %d mA)",
                     (int)k_charge_targets[charge_ceiling_idx()],
                     (int)BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA);
        }
        return;
    }
    s_chg.source_verified = false;
    if (s_chg.phase == SVC_POWER_CHARGE_RAMP ||
            s_chg.phase == SVC_POWER_CHARGE_HOLD) {
        s_chg.phase = SVC_POWER_CHARGE_TRICKLE;
        s_chg.settle_start_us = 0;
        if (s_chg.idx > charge_ceiling_idx()) {
            (void)charge_write_target(charge_ceiling_idx());
        }
    }
    ESP_LOGI(TAG, "PC-safe charge ceiling %d mA (risk reduction; input "
             "limit stays %d mA)",
             CHARGE_SAFE_CEILING_MA,
             (int)BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA);
}

static void charge_collapse(const char *reason);

/* One sticky flag per controller poll: a logical operation that failed in
 * whole or in part (set+get+readback mismatch, limit load, VBUS ADC).
 * Sub-transaction successes never clear it - only a poll in which every
 * attempted operation completed cleanly resets the failure streak, so
 * three genuinely failed polls in a row reach the collapse threshold. */
static bool s_chg_op_failed;

static void charge_account_poll(void)
{
    if (s_chg_op_failed) {
        if (++s_chg.pmic_fails >= CHARGE_MAX_PMIC_FAILS) {
            charge_collapse("repeated controller PMIC failures");
        }
    } else {
        s_chg.pmic_fails = 0;
    }
}

static void charge_collapse(const char *reason)
{
    if (s_chg.phase == SVC_POWER_CHARGE_FAULT) {
        return;
    }
    ESP_LOGW(TAG, "charge controller collapsed to %d mA: %s "
             "(droops=%d fails=%d)",
             (int)k_charge_targets[0], reason, s_chg.droops,
             s_chg.pmic_fails);
    /* The park below is best-effort; until a dedicated set-50 + get-50
     * verification succeeds (here or in a later FAULT poll), the FAULT
     * state must keep retrying regardless of any cached target value. */
    s_chg.fault_park_verified = false;
    uint16_t parked = 0;
    if (bsp_pmic_set_charge_current(k_charge_targets[0]) == ESP_OK &&
            bsp_pmic_get_charge_current(&parked) == ESP_OK &&
            parked == k_charge_targets[0]) {
        s_chg.idx = 0;
        s_chg.target_ma = parked;
        s_chg.fault_park_verified = true;
    }
    s_chg.phase = SVC_POWER_CHARGE_FAULT;
}

static bool charge_write_target(int idx)
{
    const uint16_t target = k_charge_targets[idx];
    uint16_t readback = 0;
    if (bsp_pmic_set_charge_current(target) != ESP_OK ||
            bsp_pmic_get_charge_current(&readback) != ESP_OK) {
        s_chg_op_failed = true;
        return false;
    }
    if (readback != target) {
        ESP_LOGE(TAG, "charge target readback %u mA != %u mA",
                 readback, target);
        s_chg_op_failed = true;
        return false;
    }
    s_chg.idx = idx;
    s_chg.target_ma = target;
    s_chg.last_change_us = esp_timer_get_time();
    return true;
}

/* Evidence gates. All must hold before any rise and continuously while
 * raised. *droop distinguishes "source cannot keep up" (the only failure
 * that counts toward the droop-collapse budget) from the others.
 * Configured VINDPM and charge voltage are read from the PMIC, never
 * assumed from POR. */
static bool charge_gates(const bsp_pmic_status_t *pmic, bool *droop)
{
    *droop = false;
    if (!pmic->vbus_present || !pmic->battery_present) {
        return false;
    }
    if (!pmic->charging && !pmic->charge_done) {
        return false;
    }
    if (pmic->battery_mv < CHARGE_VBAT_MIN_MV) {
        return false;
    }
    const int vbat_cap = s_chg.vchg_mv + CHARGE_VBAT_HEADROOM_MV;
    if (pmic->battery_mv > (vbat_cap < CHARGE_VBAT_ABS_MAX_MV ?
                            vbat_cap : CHARGE_VBAT_ABS_MAX_MV)) {
        return false;
    }
    uint16_t vbus_mv = 0;
    if (bsp_pmic_read_adc_mv(BSP_PMIC_ADC_VBUS, &vbus_mv) != ESP_OK) {
        s_chg_op_failed = true;
        return false;
    }
    s_chg.vbus_mv = vbus_mv; /* keep the latest sample as rise evidence */
    if (vbus_mv < (uint16_t)(s_chg.vindpm_mv + CHARGE_VBUS_MARGIN_MV)) {
        *droop = true;
        return false;
    }
    return true;
}

static void charge_load_limits(void)
{
    if (s_chg.limits_loaded) {
        return;
    }
    /* One logical operation: both configured values or neither counts. */
    uint16_t vindpm = 0;
    uint16_t vchg = 0;
    if (bsp_pmic_get_vindpm(&vindpm) == ESP_OK &&
            bsp_pmic_get_charge_voltage(&vchg) == ESP_OK) {
        s_chg.vindpm_mv = vindpm;
        s_chg.vchg_mv = vchg;
        s_chg.limits_loaded = true;
        ESP_LOGI(TAG, "charge limits: VINDPM %u mV, Vchg %u mV",
                 s_chg.vindpm_mv, s_chg.vchg_mv);
    } else {
        s_chg_op_failed = true;
    }
}

static void charge_step_down(const bsp_pmic_status_t *pmic, bool droop)
{
    if (droop && ++s_chg.droops >= CHARGE_MAX_DROOPS) {
        charge_collapse("repeated VBUS droop");
        return;
    }
    if (s_chg.idx > 0) {
        if (charge_write_target(s_chg.idx - 1)) {
            ESP_LOGI(TAG, "charge stepped down to %d mA (droop=%d "
                     "fails=%d VBAT=%u mV VBUS=%u mV)",
                     s_chg.target_ma, droop, s_chg.pmic_fails,
                     pmic->battery_mv, s_chg.vbus_mv);
        }
    }
    /* Already at 50 mA: nothing to lower; the TG28 handles termination. */
}

/* Battery gone while a raised target may be active: latch the fresh-
 * observation state (TRICKLE + cleared settle) BEFORE attempting the
 * verified 50 mA park, so even a failed park write can never leave a
 * reinserted cell resuming in RAMP/HOLD against an unverified register;
 * TRICKLE's retry path then owns the 50 mA restoration on later polls.
 * Used by RAMP and HOLD alike. */
static void charge_park_and_reobserve(void)
{
    s_chg.phase = SVC_POWER_CHARGE_TRICKLE;
    s_chg.settle_start_us = 0;
    (void)charge_write_target(0);
}

static void charge_session_start(void)
{
    memset(&s_chg, 0, sizeof(s_chg));
    s_chg.phase = SVC_POWER_CHARGE_TRICKLE;
    /* Start/restore the safe 50 mA ceiling; the BSP already forced it at
     * boot, this also covers a session started while that write was still
     * failing. Verified write; retried by the TRICKLE state on failure.
     * The charge-ceiling confirmation is per-session: memset leaves it
     * unconfirmed, so an unknown/PC source ramps 50->100->200 only. */
    if (charge_write_target(0)) {
        ESP_LOGI(TAG, "VBUS session: charge starts at %d mA", s_chg.target_ma);
    }
}

static void charge_unplug(void)
{
    /* End of session: park the charge target at 50 mA best-effort (logged
     * on failure) so a raised ceiling does not survive into the next
     * session; REG16 is a fixed 2000 mA board default and is untouched
     * here. The per-session charge-ceiling confirmation dies with the
     * memset either way. */
    uint16_t parked = 0;
    if (bsp_pmic_set_charge_current(k_charge_targets[0]) == ESP_OK &&
            bsp_pmic_get_charge_current(&parked) == ESP_OK &&
            parked == k_charge_targets[0]) {
        s_chg.target_ma = parked;
    } else {
        ESP_LOGW(TAG, "failed to park charge target at %d mA on unplug",
                 (int)k_charge_targets[0]);
    }
    memset(&s_chg, 0, sizeof(s_chg));
    s_chg.phase = SVC_POWER_CHARGE_IDLE;
}

static void charge_tick(const bsp_pmic_status_t *pmic)
{
    const int64_t now = esp_timer_get_time();
    bool droop = false;
    switch (s_chg.phase) {
    case SVC_POWER_CHARGE_IDLE:
        break;
    case SVC_POWER_CHARGE_TRICKLE:
        charge_load_limits();
        if (s_chg.target_ma != k_charge_targets[0]) {
            /* The session-start write failed; retry once per poll. When
             * this poll already logged a failed logical attempt (the
             * session-start write itself), do not immediately retry
             * again - one logical attempt per 2 s poll, next poll
             * retries. */
            if (!s_chg_op_failed) {
                charge_write_target(0);
            }
            break;
        }
        if (!pmic->battery_present) {
            /* No cell: the 2 s observation window starts when one is
             * first seen, not at the VBUS edge - and restarts after
             * every removal. */
            s_chg.settle_start_us = 0;
            break;
        }
        if (s_chg.settle_start_us == 0) {
            s_chg.settle_start_us = now;
            break; /* first poll with a battery: begin the observation */
        }
        if (now - s_chg.settle_start_us < CHARGE_SETTLE_US) {
            break; /* exactly one full 2 s settle at 50 mA */
        }
        if (!s_chg.limits_loaded) {
            break; /* no rise judgment before the configured limits are in */
        }
        if (pmic->charge_done) {
            s_chg.phase = SVC_POWER_CHARGE_HOLD;
            break;
        }
        if (!charge_gates(pmic, &droop)) {
            break;
        }
        /* Settle complete with all gates green: the first rise happens
         * now (audit contract S0 -> S1), ~2 s after the battery was
         * first observed. */
        if (s_chg.idx >= charge_ceiling_idx()) {
            s_chg.phase = SVC_POWER_CHARGE_HOLD;
            break;
        }
        if (charge_write_target(s_chg.idx + 1)) {
            ESP_LOGI(TAG, "charge rise to %d mA verified "
                     "(VBUS=%u mV VBAT=%u mV charging=%d done=%d)",
                     s_chg.target_ma, s_chg.vbus_mv, pmic->battery_mv,
                     pmic->charging, pmic->charge_done);
            s_chg.phase = SVC_POWER_CHARGE_RAMP;
        }
        break;
    case SVC_POWER_CHARGE_RAMP:
        if (!pmic->battery_present) {
            /* Cell removal is checked FIRST - before charge_done - so a
             * stale done flag in the same snapshot cannot delay the
             * verified 50 mA park by one poll. Parking is immediate and
             * demands a fresh settle for the next cell (mirror of HOLD). */
            charge_park_and_reobserve();
            break;
        }
        if (pmic->charge_done) {
            s_chg.phase = SVC_POWER_CHARGE_HOLD;
            break;
        }
        if (!charge_gates(pmic, &droop)) {
            charge_step_down(pmic, droop);
            break;
        }
        if (now - s_chg.last_change_us < CHARGE_STEP_US) {
            break; /* exactly 4 s between successful target writes */
        }
        if (s_chg.idx >= charge_ceiling_idx()) {
            s_chg.phase = SVC_POWER_CHARGE_HOLD;
            ESP_LOGI(TAG, "charge target at %d mA ceiling", s_chg.target_ma);
            break;
        }
        if (charge_write_target(s_chg.idx + 1)) {
            ESP_LOGI(TAG, "charge rise to %d mA verified "
                     "(VBUS=%u mV VBAT=%u mV charging=%d done=%d)",
                     s_chg.target_ma, s_chg.vbus_mv, pmic->battery_mv,
                     pmic->charging, pmic->charge_done);
        }
        break;
    case SVC_POWER_CHARGE_HOLD:
        if (!pmic->battery_present) {
            charge_park_and_reobserve();
            break;
        }
        if (!charge_gates(pmic, &droop)) {
            /* Continuous gates while raised: ANY failure (battery/charge
             * state, VBAT sanity, source droop) steps the ceiling down
             * toward 50 mA; only a droop consumes the collapse budget.
             * An elevated target is never retained through a gate
             * failure. */
            charge_step_down(pmic, droop);
        }
        break;
    case SVC_POWER_CHARGE_FAULT:
        /* Latched until unplug, but the 50 mA park must actually verify:
         * the cached target/idx cannot prove the hardware state (a set
         * whose readback failed may still have reached REG62), so retry
         * a dedicated set-50 + get-50 verification once per 2 s poll
         * until it succeeds. */
        if (!s_chg.fault_park_verified && charge_write_target(0)) {
            s_chg.fault_park_verified = true;
            ESP_LOGI(TAG, "FAULT park verified at %d mA", s_chg.target_ma);
        }
        break;
    default:
        break;
    }
    charge_account_poll();
}

/* ------------------------------------------------------------------ */
/* PMIC polling and charge edges                                       */
/* ------------------------------------------------------------------ */

static void pmic_poll(void)
{
    bsp_pmic_status_t pmic = {0};
    if (bsp_pmic_get_status(&pmic) != ESP_OK) {
        if (s_chg.phase != SVC_POWER_CHARGE_IDLE) {
            s_chg_op_failed = true;
            charge_account_poll();
        }
        return;
    }

    svc_power_status_t prev;
    portENTER_CRITICAL(&s_status_lock);
    prev = s_status;
    s_status.battery_mv = pmic.battery_mv;
    s_status.percent = (pmic.battery_present && pmic.fuel_gauge_valid) ?
                       pmic.battery_percent : -1;
    s_status.present = pmic.battery_present;
    s_status.vbus = pmic.vbus_present;
    s_status.charging = pmic.charging;
    s_status.charge_done = pmic.charge_done;
    s_status.fuel_gauge_valid = pmic.fuel_gauge_valid;
    s_status.fuel_gauge_reference_model = pmic.fuel_gauge_reference_model;
    portEXIT_CRITICAL(&s_status_lock);

    /* One failure-accounting epoch per poll: sticky through all controller
     * operations below, counted once by charge_tick's tail. */
    s_chg_op_failed = false;

    /* Charge-controller session edges run before the tick so a fresh
     * session always begins with the verified 50 mA write. */
    if (!prev.vbus && pmic.vbus_present) {
        charge_session_start();
    } else if (prev.vbus && !pmic.vbus_present) {
        charge_unplug();
    }
    charge_tick(&pmic);

    portENTER_CRITICAL(&s_status_lock);
    s_status.charge_target_ma =
        s_chg.phase == SVC_POWER_CHARGE_IDLE ? 0 : s_chg.target_ma;
    s_status.charge_phase = s_chg.phase;
    s_status.source_verified = s_chg.source_verified;
    s_status.charge_ceiling_ma = k_charge_targets[charge_ceiling_idx()];
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
    /* First poll immediately: no blind 2 s window before the board state
     * is observed, so a VBUS session already present at boot reaches
     * charge_session_start() right away. */
    pmic_poll();
    for (;;) {
        uint32_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd == CMD_SCREEN_OFF) {
                screen_off_run();
            } else if (cmd == CMD_SCREEN_ON) {
                screen_on_run();
            } else if (cmd == CMD_SOURCE_VERIFIED) {
                source_verified_command(true);
            } else if (cmd == CMD_SOURCE_UNSAFE) {
                source_verified_command(false);
            }
        }

        if (s_screen_off && s_wake_retry_due_us != 0 &&
                esp_timer_get_time() >= s_wake_retry_due_us) {
            s_wake_retry_due_us = 0;
            screen_on_run();
        }

        if (s_screen_off) {
            /* Two independent detectors, both with a two-sample confirm
             * (>=50-100 ms of evidence, which gates out a residual edge
             * from the sleep-in transition): (a) the CST820 INT level on
             * GPIO3 - the datasheet contract says the line stays low
             * until the report is read; (b) a direct report-register poll
             * over I2C (power_touch_sample) - the indev is disabled, so
             * nothing else reads the controller, and the report answers
             * regardless of the INT pulse shape. */
            const bool wake_signal = power_touch_sample();
            if (wake_signal) {
                if (s_touch_wake_confirm) {
                    s_touch_wake_confirm = false;
                    power_wake_diag_log("wake");
                    screen_on_run();
                } else {
                    s_touch_wake_confirm = true;
                }
            } else {
                s_touch_wake_confirm = false;
            }
            /* Diagnostics: print only while the counters advance, so an
             * untouched screen stays silent in the log. */
            if (++s_wake_diag_ticks >= POWER_DIAG_TICKS) {
                s_wake_diag_ticks = 0;
                if (s_wake_int_edges != s_wake_diag_last_edges ||
                        s_wake_i2c_pressed != s_wake_diag_last_pressed ||
                        s_wake_i2c_fail != s_wake_diag_last_fail) {
                    s_wake_diag_last_edges = s_wake_int_edges;
                    s_wake_diag_last_pressed = s_wake_i2c_pressed;
                    s_wake_diag_last_fail = s_wake_i2c_fail;
                    power_wake_diag_log("poll");
                }
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
 * Close the storage lease gate and require a clean unmount while the supply
 * is still present. Display,
 * audio and network owners are handled by the BSP-level rail/pin parking
 * (a richer app callback can be layered on later without changing this
 * registration). */
static esp_err_t power_safe_shutdown_cb(void *ctx)
{
    (void)ctx;
    return svc_storage_quiesce_and_unmount();
}

/* BSP safe-state continues parking pins and cutting rails even when its
 * application callback fails. Therefore all refusal-capable application
 * work must complete before entering BSP safe-state; the callback above is
 * only an idempotent last line of defence. */
static esp_err_t power_safe_state_preflight(void)
{
    if (svc_audio_is_recording() || svc_audio_is_playing()) {
        ESP_LOGW(TAG, "power transition refused while audio is active");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = svc_storage_quiesce_and_unmount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage quiesce failed: %s", esp_err_to_name(err));
    }
    return err;
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
static void deep_sleep_abort_restore(bool panel_parked)
{
    if (!panel_parked) {
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    /* Panel wake (SLPOUT, >=120 ms settle, backlight) runs OUTSIDE the
     * LVGL lock: it is pure hardware, and invalidation is still disabled
     * from the park step, so nothing renders during the wake. */
    const esp_err_t wake_err = bsp_display_exit_sleep_panel();
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "deep-sleep abort display restore failed: %s",
                 esp_err_to_name(wake_err));
        return;
    }
    if (!ui_lock()) {
        ESP_LOGE(TAG, "deep-sleep abort restore skipped: LVGL lock unavailable");
        return;
    }
    if (disp != NULL) {
        lv_display_enable_invalidation(disp, true);
    }
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev != NULL) {
        power_indev_wait_release_if_pressed(indev);
        lv_indev_enable(indev, true);
    }
    lv_obj_invalidate(lv_screen_active());
    ui_unlock();
}

esp_err_t svc_power_deep_sleep(int wake_after_min)
{
    if (wake_after_min < 0 || wake_after_min > 1440) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Park the panel cleanly while its rails are still up (skip when the
     * screen is already off). Refresh is stopped so LVGL does not queue
     * new frames during the transition. */
    bool panel_parked = false;
    if (!s_screen_off) {
        lv_display_t *disp = lv_display_get_default();
        lv_indev_t *indev = bsp_display_get_input_dev();
        if (!ui_lock()) {
            return ESP_ERR_TIMEOUT;
        }
        if (disp != NULL) {
            lv_display_enable_invalidation(disp, false);
        }
        if (indev != NULL) {
            lv_indev_enable(indev, false);
            /* Same §8 rule 21 counterpart as screen_off_run(): clear any
             * pressed/scroll target at the freeze so it cannot outlive the
             * transition when wake arms wait-release only conditionally. */
            lv_indev_reset(indev, NULL);
        }
        ui_unlock();

        /* Keep the touch controller powered until the final safe-state
         * sequence. This makes an aborted deep-sleep attempt recoverable
         * without a CST820 reset or a stale LVGL touch state. */
        /* Panel parking runs OUTSIDE the LVGL lock, same as
         * screen_off_run(): invalidation is already disabled above, so
         * nothing renders while the panel sleeps. */
        const esp_err_t sleep_err = bsp_display_enter_sleep_panel();
        if (sleep_err != ESP_OK) {
            ESP_LOGE(TAG, "display sleep-in before deep sleep failed: %s",
                     esp_err_to_name(sleep_err));
            if (ui_lock()) {
                if (disp != NULL) {
                    lv_display_enable_invalidation(disp, true);
                }
                if (indev != NULL) {
                    power_indev_wait_release_if_pressed(indev);
                    lv_indev_enable(indev, true);
                }
                ui_unlock();
            } else {
                ESP_LOGE(TAG,
                         "deep-sleep abort rollback skipped: LVGL lock unavailable");
            }
            return sleep_err;
        }
        panel_parked = true;
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
        deep_sleep_abort_restore(panel_parked);
        return ESP_ERR_TIMEOUT;
    }

    bool alarm_armed = false;
    if (wake_after_min > 0) {
        const esp_err_t alarm_err = arm_rtc_wakeup_alarm(wake_after_min);
        if (alarm_err != ESP_OK) {
            ESP_LOGE(TAG, "RTC alarm arm failed: %s", esp_err_to_name(alarm_err));
            deep_sleep_abort_restore(panel_parked);
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
            deep_sleep_abort_restore(panel_parked);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* Refusal-capable teardown must finish before BSP safe-state, because
     * the BSP deliberately continues rail removal after callback errors. */
    const esp_err_t preflight_err = power_safe_state_preflight();
    if (preflight_err != ESP_OK) {
        if (alarm_armed) {
            (void)bsp_rtc_alarm_irq_enable(false);
        }
        deep_sleep_abort_restore(panel_parked);
        return preflight_err;
    }

    /* Peripheral power-down: idempotent registered shutdown callback, USB
     * host stop, pin parking and rail removal. */
    const esp_err_t safe_err = bsp_power_safe_state();
    if (safe_err != ESP_OK) {
        /* bsp_power_safe_state() keeps tearing rails/parking pins after a
         * failed step, so the board is in a partially-unpowered state with
         * unknown contents: the audio codec/I2S rails may already be down
         * while the service still holds persistent codec handles, and any
         * rail may follow. Resuming the UI here would run on top of that.
         * The only system-wide-safe continuation is a fresh boot. */
        ESP_LOGE(TAG, "safe-state failed (%s) after teardown began: restarting",
                 esp_err_to_name(safe_err));
        if (alarm_armed) {
            (void)bsp_rtc_alarm_irq_enable(false);
        }
        vTaskDelay(pdMS_TO_TICKS(50)); /* let the log drain */
        esp_restart();
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
    const esp_err_t pmic_err = bsp_pmic_get_status(&pmic);
    if (pmic_err != ESP_OK) {
        ESP_LOGE(TAG, "cannot verify VBUS state: %s", esp_err_to_name(pmic_err));
        return pmic_err;
    }
    if (pmic.vbus_present) {
        ESP_LOGW(TAG, "VBUS present: power-off refused, unplug USB first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Refusal-capable teardown must finish before BSP safe-state, because
     * that function may already have removed rails when reporting an error. */
    const esp_err_t preflight_err = power_safe_state_preflight();
    if (preflight_err != ESP_OK) {
        return preflight_err;
    }

    /* Safe-shutdown chain: idempotent application callback, then BSP pin
     * parking and rail removal. */
    const esp_err_t safe_err = bsp_power_safe_state();
    if (safe_err != ESP_OK) {
        /* Same reasoning as deep sleep: safe-state tears rails even when
         * it returns an error, so returning to the running UI would use
         * stale peripheral state (including the persistent audio codec
         * handles). A failed power-off attempt ends in a restart. */
        ESP_LOGE(TAG, "safe-state failed (%s) after teardown began: restarting",
                 esp_err_to_name(safe_err));
        vTaskDelay(pdMS_TO_TICKS(50)); /* let the log drain */
        esp_restart();
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
