#include "sim_ui.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "services/svc_audio.h"
#include "services/svc_net.h"
#include "services/svc_power.h"
#include "services/svc_storage.h"
#include "tests/svc_test.h"
#include "ui/ui_manager.h"
#include "sim_app_registry.h"

static void sim_open_initial_screen(const char *screen)
{
    if (screen == NULL || screen[0] == '\0' || strcmp(screen, "watchface") == 0) {
        return;
    }
    if (!ui_lock()) {
        return;
    }
    if (strcmp(screen, "menu") == 0 || strcmp(screen, "apps") == 0) {
        ui_nav_open_menu();
    } else {
        ui_nav_open(screen);
    }
    ui_unlock();
}

/* Mirrors demo_main.c on_power: status bar battery from the power svc. */
static void sim_on_power(const svc_power_status_t *st,
                         svc_power_event_t ev, void *user)
{
    (void)user;
    ui_status_set_battery(st->percent, st->charging, st->present);
    (void)ev;
}

static void sim_on_sd(svc_sd_event_t ev, void *user)
{
    (void)user;
    ui_status_set_sd(ev == SVC_SD_EV_MOUNTED);
}

static void sim_services_start(void)
{
    /* Same order as demo_main app_main; svc_input (BOOT/PWR keys) has no
     * host counterpart and is skipped on purpose. */
    if (svc_power_start(sim_on_power, NULL) != ESP_OK) {
        ESP_LOGW("sim", "svc_power_start failed");
    }
    if (svc_storage_start(sim_on_sd, NULL) != ESP_OK) {
        ESP_LOGW("sim", "svc_storage_start failed");
    }
    if (svc_audio_start() != ESP_OK) {
        ESP_LOGW("sim", "svc_audio_start failed");
    }
    if (svc_net_start() != ESP_OK) {
        ESP_LOGW("sim", "svc_net_start failed");
    }
    if (svc_test_start() != ESP_OK) {
        ESP_LOGW("sim", "svc_test_start failed");
    }
}

void sim_ui_init(const char *screen, const char *state)
{
    sim_power_set_preview_state(state);
    sim_register_apps();
    ui_manager_init();
    sim_services_start();

    svc_power_status_t power;
    svc_power_get_status(&power);
    ui_status_set_battery(power.percent, power.charging, power.present);
    ui_status_set_wifi(state != NULL && strcmp(state, "wifi") == 0 ? 2 : 0);
    ui_status_set_ble(state != NULL && strcmp(state, "ble") == 0);
    ui_status_set_sd(state != NULL && strcmp(state, "sd") == 0);
    ui_status_set_usb(state != NULL && strcmp(state, "usb") == 0 ? 1 : 0);
    sim_open_initial_screen(screen);
}
