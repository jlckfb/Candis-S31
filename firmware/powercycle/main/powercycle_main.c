/*
 * Candis-S31 power-state cycle harness (measurement aid, not a product app).
 *
 * Each cycle:
 *   1. ACTIVE (10 s): display on, white frame, maximum brightness.
 *   2. LOW POWER (10 s): disable charger/gauge, force every peripheral into
 *      the board safe state, then enter timer-only SoC deep sleep.
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
#include "tg28_sw.h"
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
    };
    for (size_t index = 0;
            index < sizeof(shutdown_order) / sizeof(shutdown_order[0]);
            ++index) {
        if (index == 1) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        bsp_power_domain_set(shutdown_order[index], false);
    }

    ESP_ERROR_CHECK(bsp_board_init());
    ESP_ERROR_CHECK(bsp_pmic_init());

    /* safe_state() cuts the USB-serial bridge rail during the LP phase and
     * the PMIC register survives reset, so the bridge stays off forever
     * unless re-powered. Re-assert it at the top of each cycle: the console
     * then returns for every ACTIVE phase (idf.py monitor reconnects on its
     * own), while the LP phase remains meter-only. */
    (void)bsp_power_domain_set(BSP_POWER_USB_OTG, true);

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "display start failed; continuing to the LP phase");
    } else {
        bsp_display_brightness_set(100);
        if (bsp_display_lock(1000)) {
            lv_obj_t *scr = lv_screen_active();
            lv_obj_remove_style_all(scr);
            lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
            bsp_display_unlock();
        }
    }

    ESP_LOGI(TAG, "ACTIVE %u ms: display on at maximum brightness",
             (unsigned)ACTIVE_PHASE_MS);
    vTaskDelay(pdMS_TO_TICKS(ACTIVE_PHASE_MS));

    /* DEEP SLEEP DIAGNOSTIC: dump rails before and after the safe state,
     * then deep sleep with a timer wake. Measurement window opens at the
     * deep sleep entry. */
    ESP_LOGI(TAG, "ACTIVE phase done - entering deep sleep diagnostic");
    /* The charger and the fuel gauge keep running after safe_state()
     * (REG18 bits 1 and 3 are not cut with the rails) and burn their
     * block quiescent + conversion current. Turn both off on a private
     * TG28 handle over the BSP's LP-I2C bus. */
    tg28_sw_handle_t pmic = NULL;
    const tg28_sw_config_t pmic_cfg = TG28_SW_CONFIG_DEFAULT();
    if (tg28_sw_create(bsp_lp_i2c_get_handle(), &pmic_cfg, &pmic) == ESP_OK) {
        tg28_sw_set_charge_enable(pmic, false);
        tg28_sw_set_gauge_enable(pmic, false);
        tg28_sw_delete(pmic);
        ESP_LOGI(TAG, "charger and fuel gauge disabled for the LP window");
    } else {
        ESP_LOGW(TAG, "private TG28 handle failed; LP window keeps them on");
    }
    dump_pmic_state("before safe");
    const esp_err_t safe_err = bsp_power_safe_state();
    ESP_LOGW(TAG, "safe-state returned %s", esp_err_to_name(safe_err));
    dump_pmic_state("after safe");
    esp_sleep_enable_timer_wakeup(LP_PHASE_US);
    esp_deep_sleep_start();
}
