/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* Internal contract between the peripheral domain modules. Not installed;
 * only main/ sources include this header. */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Registration entry points, one per peripheral domain module. Each module
 * registers its own console commands; factory_peripherals_register() calls
 * every one of these. */
esp_err_t factory_power_register(void);
esp_err_t factory_rtc_register(void);
esp_err_t factory_input_register(void);
esp_err_t factory_usb_register(void);
esp_err_t factory_display_register(void);
esp_err_t factory_touch_register(void);
esp_err_t factory_storage_register(void);
esp_err_t factory_audio_register(void);
esp_err_t factory_camera_register(void);
esp_err_t factory_diag_register(void);
esp_err_t factory_rf_register(void);
esp_err_t factory_accel_register(void);

/* Cross-domain state accessors used by the power orchestration paths
 * (command_rail, command_peripheral_power, factory_peripherals_power_all_off).
 * Display/audio/LED own their rails while active, so raw rail control must go
 * through these guards. */
bool factory_display_started(void);
esp_err_t factory_display_ensure_started(void);
esp_err_t factory_display_show_pattern(void);
/** Stop the BSP display/touch stack and forget the handle; OK when idle. */
esp_err_t factory_display_stop(void);

bool factory_audio_busy(void);
/** Release the speaker/microphone codec handles; OK when idle. Does not call
 *  bsp_audio_deinit() - the power orchestrator owns that step. */
esp_err_t factory_audio_stop(void);

/** Delete the RGB LED indicator handle; OK when idle. */
esp_err_t factory_led_stop(void);
