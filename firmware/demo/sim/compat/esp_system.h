/*
 * Candis-S31 simulator - esp_system shim.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
} esp_reset_reason_t;

/* The sim always "boots" from a cold power-on. */
esp_reset_reason_t esp_reset_reason(void);

/* Logs and exits the sim process (a real restart is meaningless here). */
void esp_restart(void);

#ifdef __cplusplus
}
#endif
