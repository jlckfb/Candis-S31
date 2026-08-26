/*
 * Candis-S31 full-screen MJPEG/AVI player demo entry point.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "audio_service.h"
#include "board_init.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "input_service.h"
#include "storage_service.h"
#include "ui_player.h"
#include "video_player.h"

static const char *TAG = "player_main";

static void on_storage(storage_event_t ev, void *user)
{
    (void)user;
    if (ev == STORAGE_EV_MOUNTED) {
        ESP_LOGI(TAG, "TF card mounted");
        ui_player_refresh_files();
    } else if (ev == STORAGE_EV_UNMOUNTED) {
        ESP_LOGI(TAG, "TF card removed");
        ui_player_refresh_files();
    }
}

static void on_input(input_event_t ev, void *user)
{
    (void)user;
    if (ev == INPUT_EV_PWR_SHORT) {
        ui_player_show_overlay(true);
    } else if (ev == INPUT_EV_BOOT_LONG) {
        video_player_stop();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(player_board_init());
    ESP_ERROR_CHECK(video_player_init());
    ESP_ERROR_CHECK(audio_start());
    ESP_ERROR_CHECK(storage_start(on_storage, NULL));
    ESP_ERROR_CHECK(input_start(on_input, NULL));
    ESP_ERROR_CHECK(ui_player_init());

    ESP_LOGI(TAG, "Candis-S31 player demo started");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
