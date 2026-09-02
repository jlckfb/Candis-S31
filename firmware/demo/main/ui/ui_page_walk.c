/*
 * Candis-S31 watch demo - one-shot page walk for navigation latency data.
 *
 * Boots a timed sequence: opens every registered app (menu-first), dwells,
 * then returns home. Each transition is already instrumented by ui_perf
 * (one ESP_LOG line per nav), so a single monitor capture yields the
 * per-page create/open cost without any human interaction.
 *
 * Skipped by design: "tests" (subscribes to the runner event stream and
 * would interfere with a concurrent worker), "camera" (camera-domain test
 * arbitration) and "usb" (role flips would disturb the shared debug
 * console link).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui/ui_manager.h"

#define PAGE_WALK_STACK   4096
#define PAGE_WALK_PRIO    2
#define PAGE_WALK_DELAY_S 3    /* time to boot services before walking */
#define PAGE_WALK_DWELL_S 2    /* dwell on each page */
#define PAGE_WALK_GAP_MS  300  /* let each page settle after loading */

static const char *const s_walk_ids[] = {
    "display", "recorder", "player", "wifi", "ble", "files",
    "game2048", "snake", "breakout", "led", "settings", "power",
    "sysinfo",
};
#define PAGE_WALK_COUNT (int)(sizeof(s_walk_ids) / sizeof(s_walk_ids[0]))

static void page_walk_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(PAGE_WALK_DELAY_S * 1000));

    if (!ui_lock()) {
        return;
    }
    ui_nav_open_menu();
    ui_unlock();

    for (int i = 0; i < PAGE_WALK_COUNT; ++i) {
        if (!ui_lock()) {
            return;
        }
        ui_nav_open(s_walk_ids[i]);
        ui_unlock();

        vTaskDelay(pdMS_TO_TICKS(PAGE_WALK_DWELL_S * 1000));

        if (!ui_lock()) {
            return;
        }
        ui_nav_home();
        ui_unlock();

        vTaskDelay(pdMS_TO_TICKS(PAGE_WALK_GAP_MS));
    }
    vTaskDelete(NULL);
}

void ui_page_walk_start(void)
{
    xTaskCreate(page_walk_task, "page_walk", PAGE_WALK_STACK, NULL,
                PAGE_WALK_PRIO, NULL);
}
