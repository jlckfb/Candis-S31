/*
 * Candis-S31 power-state cycle harness (measurement aid, not a product app).
 *
 * Each cycle:
 *   1. ACTIVE (10 s): display on, white frame, maximum brightness.
 *   2. LOW POWER (10 s): force every peripheral into the board safe state,
 *      preserve the charger/gauge policy, then enter timer-only deep sleep.
 *   3. The timer wake is a fresh reset, which starts the next cycle.
 *
 * A series meter (e.g. across the lifted C1 pad) reads the steady-state
 * current of each phase directly; the cycle repeats indefinitely.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "bsp/candis_s31.h"
#include "bsp/display.h"

static const char *TAG = "powercycle";

#define ACTIVE_PHASE_MS  10000
#define LP_PHASE_US      (10ULL * 1000000ULL)


static void dump_pmic_state(const char *label)
{
    ESP_LOGI(TAG, "--- PMIC dump %s ---", label);
    for (bsp_pmic_regulator_t reg = BSP_PMIC_DCDC1; reg < BSP_PMIC_REGULATOR_COUNT; ++reg) {
        bool enabled = false;
        if (bsp_pmic_regulator_is_enabled(reg, &enabled) == ESP_OK) {
            ESP_LOGI(TAG, "    rail[%d] %s", (int)reg, enabled ? "ON " : "off");
        }
    }
    bsp_pmic_status_t st = {0};
    if (bsp_pmic_get_status(&st) == ESP_OK) {
        ESP_LOGI(TAG, "    bat=%u mV soc=%u%% chg=%d done=%d vbus=%d gauge_valid=%d ref_model=%d",
                 st.battery_mv, st.battery_percent, (int)st.charging,
                 (int)st.charge_done, (int)st.vbus_present,
                 (int)st.fuel_gauge_valid, (int)st.fuel_gauge_reference_model);
    }
    ESP_LOGI(TAG, "----------------------");
}

void app_main(void)
{
    /* Mirror the demo boot: drive every direct GPIO-controlled supply off
     * first, in the only safe display order, so the wake reset restarts from
     * a known power state. */
    static const bsp_power_domain_t shutdown_order[] = {
        BSP_POWER_DISPLAY_VCI,
        BSP_POWER_DISPLAY_VBAT,
        BSP_POWER_TYPE_C_CONTROL,
        BSP_POWER_SDCARD,
        BSP_POWER_AUDIO_PA,
        BSP_POWER_USB_OTG,
    };
    for (size_t index = 0;
            index < sizeof(shutdown_order) / sizeof(shutdown_order[0]);
            ++index) {
        if (index == 1) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        ESP_ERROR_CHECK(bsp_power_domain_set(shutdown_order[index], false));
    }

    ESP_ERROR_CHECK(bsp_board_init());
    ESP_ERROR_CHECK(bsp_pmic_init());
    ESP_LOGI(TAG, "boot wake causes=0x%08x", (unsigned)esp_sleep_get_wakeup_causes());

    /* C1 is the debug bridge. USB_OTG controls the C2 boost switch and must
     * stay off unless the Type-C source policy explicitly authorizes it. */

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "display start failed; continuing to the LP phase");
    } else {
        ESP_ERROR_CHECK(bsp_display_brightness_set(100));
        if (bsp_display_lock(1000)) {
            lv_obj_t *scr = lv_screen_active();
            lv_obj_remove_style_all(scr);
            lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
            bsp_display_unlock();
        } else {
            ESP_LOGE(TAG, "display lock failed; active white frame not rendered");
        }
    }

    ESP_LOGI(TAG, "ACTIVE %u ms: display %s",
             (unsigned)ACTIVE_PHASE_MS, disp != NULL ? "started at maximum brightness" : "unavailable");
    vTaskDelay(pdMS_TO_TICKS(ACTIVE_PHASE_MS));

    /* DEEP SLEEP DIAGNOSTIC: dump rails before and after the safe state,
     * then deep sleep with a timer wake. Measurement window opens at the
     * deep sleep entry. */
    ESP_LOGI(TAG, "ACTIVE phase done - entering deep sleep diagnostic");
    /* Charger/gauge enables persist in TG28 across SoC resets and reflashing.
     * Leave them unchanged: this measures the normal board sleep state,
     * including any battery charging current, not isolated SoC current. */
    dump_pmic_state("before safe");
    ESP_ERROR_CHECK(bsp_display_stop());
    const esp_err_t safe_err = bsp_power_safe_state();
    ESP_LOGI(TAG, "safe-state returned %s", esp_err_to_name(safe_err));
    ESP_ERROR_CHECK(safe_err);
    dump_pmic_state("after safe");
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(LP_PHASE_US));
    ESP_LOGI(TAG, "deep sleep: timer wake in %u ms", (unsigned)(LP_PHASE_US / 1000));
    esp_deep_sleep_start();
}
