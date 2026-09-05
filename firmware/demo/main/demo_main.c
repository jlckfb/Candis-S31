/*
 * Candis-S31 watch demo - entry point.
 *
 * Boots the board, starts the UI shell, then the background services. Only
 * the board bring-up is fatal: every service degrades gracefully so a
 * missing TF card or a BT init fault never blacks out the demo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"

#include "demo_apps.h"
#include "demo_board.h"
#include "services/svc_audio.h"
#include "services/svc_input.h"
#include "services/svc_net.h"
#include "services/svc_power.h"
#include "services/svc_storage.h"
#include "tests/svc_test.h"
#include "tests/test_registry.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_walk.h"

static const char *TAG = "candis_demo";

#ifdef CANDIS_DEMO_AUTOTEST
static const char *test_status_name(test_status_t status)
{
    switch (status) {
    case TEST_ST_PASS:
        return "PASS";
    case TEST_ST_FAIL:
        return "FAIL";
    case TEST_ST_SKIP:
        return "SKIP";
    case TEST_ST_WARN:
        return "WARN";
    case TEST_ST_NOT_RUN:
    default:
        return "NOT_RUN";
    }
}

static void on_autotest_event(svc_test_event_t event, const char *id,
                              void *user)
{
    (void)user;
    if (event == SVC_TEST_EV_RESULT && id != NULL) {
        test_result_t result;
        test_result_get(id, &result);
        ESP_LOGI(TAG,
                 "AUTOTEST_RESULT id=%s status=%s duration_ms=%lu evidence=%s",
                 id, test_status_name(result.st),
                 (unsigned long)result.duration_ms, result.evidence);
        return;
    }
    if (event != SVC_TEST_EV_QUEUE_DRAINED) {
        return;
    }

    int pass = 0;
    int fail = 0;
    int warn = 0;
    int skip = 0;
    int not_run = 0;
    const int total = test_count();
    for (int i = 0; i < total; ++i) {
        const test_case_t *test = test_at(i);
        test_result_t result;
        test_result_get(test->id, &result);
        switch (result.st) {
        case TEST_ST_PASS:
            ++pass;
            break;
        case TEST_ST_FAIL:
            ++fail;
            break;
        case TEST_ST_WARN:
            ++warn;
            break;
        case TEST_ST_SKIP:
            ++skip;
            break;
        case TEST_ST_NOT_RUN:
        default:
            ++not_run;
            break;
        }
    }
    ESP_LOGI(TAG,
             "AUTOTEST_SUMMARY total=%d pass=%d fail=%d warn=%d skip=%d "
             "not_run=%d",
             total, pass, fail, warn, skip, not_run);
}
#endif

static void svc_log_start(const char *name, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s service failed to start: %s (feature degraded)",
                 name, esp_err_to_name(err));
    }
}

static void on_input(svc_input_event_t ev, void *user)
{
    (void)user;
    if (svc_power_is_screen_off()) {
        svc_power_activity(); /* wake only: swallow the key that woke us */
        return;
    }
    svc_power_activity();
    if (ev == SVC_INPUT_BOOT_LONG) {
        svc_power_screen_off();
        return;
    }
    ui_nav_request_t request;
    switch (ev) {
    case SVC_INPUT_BOOT_SHORT:
        request = UI_NAV_REQUEST_BACK_OR_MENU;
        break;
    case SVC_INPUT_PWR_SHORT:
        request = UI_NAV_REQUEST_HOME;
        break;
    default:
        return;
    }
    if (!ui_nav_request(request)) {
        ESP_LOGW(TAG, "navigation request rejected before UI init");
    }
}

/* Runs on the LVGL thread; arg is a string literal. */
static void toast_literal(void *arg)
{
    ui_toast((const char *)arg);
}
#ifdef CANDIS_DEMO_CAMERA_BENCH
static void open_camera_benchmark(void *arg)
{
    (void)arg;
    ui_nav_open("camera");
}
#endif


static void on_power(const svc_power_status_t *st, svc_power_event_t ev,
                     void *user)
{
    (void)user;
    ui_status_set_battery(st->percent, st->charging, st->present,
                          st->fuel_gauge_reference_model);
    switch (ev) {
    case SVC_POWER_EV_CHARGE_START:
        svc_power_screen_on();
        ui_async(toast_literal, "Charging");
        break;
    case SVC_POWER_EV_CHARGE_DONE:
        ui_async(toast_literal, "Full");
        break;
    default:
        break;
    }
}

static void on_sd(svc_sd_event_t ev, void *user)
{
    (void)user;
    ui_status_set_sd(ev == SVC_SD_EV_MOUNTED);
    if (ev == SVC_SD_EV_MOUNTED) {
        ui_async(toast_literal, "TF card mounted");
    } else if (ev == SVC_SD_EV_UNMOUNTED) {
        ui_async(toast_literal, "TF card removed");
    } else if (ev == SVC_SD_EV_MOUNT_FAIL) {
        ui_async(toast_literal, "TF card mount failed");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(demo_board_init());

    demo_apps_register_all();
    ui_manager_init();

    svc_log_start("input", svc_input_start(on_input, NULL));
    svc_log_start("power", svc_power_start(on_power, NULL));
    svc_log_start("storage", svc_storage_start(on_sd, NULL));
    svc_log_start("audio", svc_audio_start());
    svc_log_start("net", svc_net_start());
    const esp_err_t test_error = svc_test_start();
    svc_log_start("test", test_error);

#ifdef CANDIS_DEMO_AUTOTEST
    if (test_error == ESP_OK) {
        svc_test_subscribe(on_autotest_event, NULL);
        const esp_err_t error = svc_test_run_all_auto();
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "AUTOTEST_QUEUE count=%d",
                     test_auto_count((test_domain_t)-1));
        } else {
            ESP_LOGE(TAG, "AUTOTEST_QUEUE failed=%s",
                     esp_err_to_name(error));
        }
    }
#endif
#ifdef CANDIS_DEMO_CAMERA_BENCH
    ui_async(open_camera_benchmark, NULL);
#endif


#if CONFIG_DEMO_PAGE_WALK
    /* One-shot navigation timing walk (see ui_page_walk.c); one ui_perf
     * log line per transition on the monitor. */
    ui_page_walk_start();
#endif
    ESP_LOGI(TAG, "Candis-S31 watch demo started");
}
