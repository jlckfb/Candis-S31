#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "services/svc_power.h"

static pthread_mutex_t s_display_mutex = PTHREAD_MUTEX_INITIALIZER;
static lv_indev_t *s_input_dev;
static struct timespec s_start_time;
static bool s_start_time_ready;

static void sim_clock_init(void)
{
    if (!s_start_time_ready) {
        clock_gettime(CLOCK_MONOTONIC, &s_start_time);
        s_start_time_ready = true;
    }
}

int64_t esp_timer_get_time(void)
{
    sim_clock_init();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t sec = (int64_t)now.tv_sec - s_start_time.tv_sec;
    const int64_t nsec = (int64_t)now.tv_nsec - s_start_time.tv_nsec;
    return sec * 1000000 + nsec / 1000;
}

uint32_t esp_random(void)
{
    static bool seeded;
    if (!seeded) {
        seeded = true;
        srand((unsigned)time(NULL));
    }
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    default: return "ESP_ERR_UNKNOWN";
    }
}

void sim_log_print(const char *level, const char *tag, const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "[%s] %s: ", level, tag ? tag : "sim");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return pthread_mutex_lock(&s_display_mutex) == 0;
}

void bsp_display_unlock(void)
{
    (void)pthread_mutex_unlock(&s_display_mutex);
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return s_input_dev;
}

void sim_bsp_set_input_dev(lv_indev_t *indev)
{
    s_input_dev = indev;
}

esp_err_t bsp_rtc_get_time(bsp_rtc_time_t *out, bsp_rtc_status_t *status)
{
    if (out == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) == NULL) {
        status->time_valid = false;
        return ESP_FAIL;
    }
    out->year = (uint16_t)(local.tm_year + 1900);
    out->month = (uint8_t)(local.tm_mon + 1);
    out->day = (uint8_t)local.tm_mday;
    out->weekday = (uint8_t)local.tm_wday;
    out->hour = (uint8_t)local.tm_hour;
    out->minute = (uint8_t)local.tm_min;
    out->second = (uint8_t)local.tm_sec;
    status->time_valid = true;
    return ESP_OK;
}

static int s_preview_battery = 83;
static bool s_preview_charging;
static bool s_preview_present = true;
static bool s_screen_off;
static bool s_source_verified;
static int s_screen_timeout_s = 30;
static svc_power_cb_t s_power_cb;
static void *s_power_cb_user;
static bool s_power_task_started;

void sim_power_set_preview_state(const char *state)
{
    s_preview_battery = 83;
    s_preview_charging = false;
    s_preview_present = true;
    if (state == NULL) {
        return;
    }
    if (strcmp(state, "charging") == 0) {
        s_preview_battery = 61;
        s_preview_charging = true;
    } else if (strcmp(state, "low-battery") == 0) {
        s_preview_battery = 12;
    } else if (strcmp(state, "no-battery") == 0) {
        s_preview_battery = -1;
        s_preview_present = false;
    }
}

void svc_power_get_status(svc_power_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->battery_mv = s_preview_present ? 4019 : 0;
    out->percent = s_preview_battery;
    out->present = s_preview_present;
    out->vbus = s_preview_charging;
    out->charging = s_preview_charging;
    out->charge_done = false;
    out->fuel_gauge_valid = s_preview_present;
    out->fuel_gauge_reference_model = true;
    out->charge_target_ma = s_preview_charging ?
                            (s_source_verified ? 500 : 200) : 0;
    out->charge_phase = s_preview_charging ? SVC_POWER_CHARGE_HOLD
                                           : SVC_POWER_CHARGE_IDLE;
    out->source_verified = s_source_verified;
    out->charge_ceiling_ma = s_source_verified ? 500 : 200;
}

static void sim_power_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (s_power_cb == NULL) {
            continue;
        }
        /* A slow charge drift keeps the status bar alive while previewing
         * the charging state. */
        if (s_preview_charging && s_preview_battery < 100) {
            ++s_preview_battery;
        }
        svc_power_status_t status;
        svc_power_get_status(&status);
        s_power_cb(&status, SVC_POWER_EV_UPDATE, s_power_cb_user);
    }
}

esp_err_t svc_power_start(svc_power_cb_t cb, void *user)
{
    s_power_cb = cb;
    s_power_cb_user = user;
    if (!s_power_task_started) {
        s_power_task_started = true;
        if (xTaskCreate(sim_power_task, "svc_power", 4096, NULL, 4, NULL)
                != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t svc_power_set_external_source_verified(bool verified)
{
    s_source_verified = verified;
    return ESP_OK;
}

void svc_power_activity(void)
{
}

void svc_power_set_screen_timeout(int seconds)
{
    s_screen_timeout_s = seconds;
    (void)s_screen_timeout_s;
}

esp_err_t svc_power_screen_off(void)
{
    s_screen_off = true;
    if (s_power_cb != NULL) {
        svc_power_status_t status;
        svc_power_get_status(&status);
        s_power_cb(&status, SVC_POWER_EV_SCREEN_OFF, s_power_cb_user);
    }
    return ESP_OK;
}

esp_err_t svc_power_screen_on(void)
{
    s_screen_off = false;
    if (s_power_cb != NULL) {
        svc_power_status_t status;
        svc_power_get_status(&status);
        s_power_cb(&status, SVC_POWER_EV_SCREEN_ON, s_power_cb_user);
    }
    return ESP_OK;
}

bool svc_power_is_screen_off(void)
{
    return s_screen_off;
}

esp_err_t svc_power_deep_sleep(int wake_after_min)
{
    ESP_LOGW("sim_power", "deep sleep %d min requested (no-op in sim)",
             wake_after_min);
    return ESP_OK;
}

esp_err_t svc_power_shutdown(void)
{
    ESP_LOGW("sim_power", "shutdown requested (no-op in sim)");
    return ESP_OK;
}
