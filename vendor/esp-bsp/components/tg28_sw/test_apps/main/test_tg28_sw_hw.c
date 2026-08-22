/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* On-hardware tests for the TG28 switch-charger driver (groups H1-H8).
 *
 * This file is compiled only when CONFIG_TG28_SW_TEST_WITH_HARDWARE is set
 * (see Kconfig.projbuild); the default test-app build uses test_tg28_sw.c
 * and needs no hardware. Run this on a Candis-S31 board or a TG28 EVM with
 * the PMIC on the main I2C bus: SCL=GPIO33, SDA=GPIO34, IRQ=GPIO2, address
 * 0x34 (mirroring include/bsp/candis_s31.h of the candis_s31 BSP).
 *
 * Several checks need an operator: H2/H3 ask for multimeter/ammeter readings
 * and H5 waits for key presses and VBUS plug/unplug. Group H9 of the fix
 * spec (bsp_power_safe_state rail audit) is a BSP-level test and cannot run
 * at component level, so it is intentionally not implemented here.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tg28_sw.h"

#if defined(CONFIG_TG28_SW_TEST_WITH_HARDWARE)

#define TEST_I2C_PORT   I2C_NUM_0
#define TEST_I2C_SCL    CONFIG_TG28_SW_TEST_I2C_SCL
#define TEST_I2C_SDA    CONFIG_TG28_SW_TEST_I2C_SDA
#define TEST_PMIC_IRQ   CONFIG_TG28_SW_TEST_PMIC_IRQ

static const char *TAG = "tg28_sw_hw";

static i2c_master_bus_handle_t s_bus;
static tg28_sw_handle_t s_pmic;

static void test_setup(void)
{
    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TEST_I2C_PORT,
        .scl_io_num = TEST_I2C_SCL,
        .sda_io_num = TEST_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_bus));

    tg28_sw_config_t config = TG28_SW_CONFIG_DEFAULT();
    config.device_address = CONFIG_TG28_SW_TEST_I2C_ADDRESS;
    ESP_ERROR_CHECK(tg28_sw_create(s_bus, &config, &s_pmic));

    const gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << TEST_PMIC_IRQ,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&irq_config));
}

/* H1: read back the OTP baseline before any software change. The expected
 * values come from the customer confirmation form V1.3: ICC = 50mA (REG62
 * code 2), DLDO1/DLDO2 off, DCDC1/DCDC4 auto-started. */
static void test_h1_otp_baseline(void)
{
#if !CONFIG_TG28_SW_TEST_OTP_BASELINE
    ESP_LOGI(TAG, "H1: OTP baseline assertions skipped (non-Candis OTP)");
    return;
#endif
    ESP_LOGI(TAG, "H1: OTP baseline read-back");
    uint16_t value = 0;
    bool enabled = false;

    ESP_ERROR_CHECK(tg28_sw_get_charge_current(s_pmic, &value));
    ESP_LOGI(TAG, "H1: REG62 ICC = %u mA (expect 50)", value);
    assert(value == 50);

    /* The IIN POR default is 1500mA but the OTP may override it; record the
     * actual value instead of asserting it. */
    ESP_ERROR_CHECK(tg28_sw_get_input_current_limit(s_pmic, &value));
    ESP_LOGI(TAG, "H1: REG16 IIN limit = %u mA (record only)", value);

    ESP_ERROR_CHECK(tg28_sw_get_charge_voltage(s_pmic, &value));
    ESP_LOGI(TAG, "H1: REG64 CV = %u mV (expect 4200)", value);
    assert(value == 4200);

    ESP_ERROR_CHECK(tg28_sw_regulator_is_enabled(s_pmic, TG28_SW_DLDO1,
                    &enabled));
    ESP_LOGI(TAG, "H1: DLDO1 enabled = %d (expect 0)", enabled);
    assert(!enabled);
    ESP_ERROR_CHECK(tg28_sw_regulator_is_enabled(s_pmic, TG28_SW_DLDO2,
                    &enabled));
    assert(!enabled);
    ESP_ERROR_CHECK(tg28_sw_regulator_is_enabled(s_pmic, TG28_SW_DCDC1,
                    &enabled));
    ESP_LOGI(TAG, "H1: DCDC1 enabled = %d (expect 1)", enabled);
    assert(enabled);
    ESP_ERROR_CHECK(tg28_sw_regulator_is_enabled(s_pmic, TG28_SW_DCDC4,
                    &enabled));
    assert(enabled);
}

/* H2: DC1SW passes DCDC1 (3.3V) through to the WS2812B RGB supply. The
 * voltage judgment is manual: measure WS2812B_PWR with a multimeter. */
static void test_h2_dc1sw_powers_rgb(void)
{
    bool enabled = false;

    ESP_LOGI(TAG, "H2: enabling DC1SW; measure WS2812B_PWR ~= 3.3V now");
    ESP_ERROR_CHECK(tg28_sw_switch_enable(s_pmic, TG28_SW_SWITCH_DC1SW, true));
    ESP_ERROR_CHECK(tg28_sw_switch_is_enabled(s_pmic, TG28_SW_SWITCH_DC1SW,
                    &enabled));
    assert(enabled);
    vTaskDelay(pdMS_TO_TICKS(5000));   /* operator measurement window */

    ESP_LOGI(TAG, "H2: disabling DC1SW; the rail must fall and the RGB LED "
             "must go dark");
    ESP_ERROR_CHECK(tg28_sw_switch_enable(s_pmic, TG28_SW_SWITCH_DC1SW, false));
    ESP_ERROR_CHECK(tg28_sw_switch_is_enabled(s_pmic, TG28_SW_SWITCH_DC1SW,
                    &enabled));
    assert(!enabled);
    vTaskDelay(pdMS_TO_TICKS(2000));   /* operator measurement window */

    /* Re-enable so later tests run with the RGB rail powered. */
    ESP_ERROR_CHECK(tg28_sw_switch_enable(s_pmic, TG28_SW_SWITCH_DC1SW, true));
}

/* H3: raise the charge current from the OTP 50mA to 300mA (REG62 code 9).
 * With VBUS attached and a non-full battery, a series ammeter must read
 * about 300mA during the CC phase (thermal regulation and input DPM can
 * pull it lower; record the actual reading). */
static void test_h3_charge_current(void)
{
    ESP_LOGI(TAG, "H3: set ICC to 300mA; with VBUS attached and a non-full "
             "battery a series ammeter must read ~= 300mA in the CC phase");
    ESP_ERROR_CHECK(tg28_sw_set_charge_current(s_pmic, 300));
    uint16_t value = 0;
    ESP_ERROR_CHECK(tg28_sw_get_charge_current(s_pmic, &value));
    assert(value == 300);
}

/* H4: input limit / CV / VINDPM / precharge / termination write then
 * read-back. */
static void test_h4_charge_parameters(void)
{
    uint16_t value = 0;
    bool enabled = false;

    ESP_ERROR_CHECK(tg28_sw_set_input_current_limit(s_pmic, 500));
    ESP_ERROR_CHECK(tg28_sw_get_input_current_limit(s_pmic, &value));
    assert(value == 500);

    ESP_ERROR_CHECK(tg28_sw_set_charge_voltage(s_pmic, 4200));
    ESP_ERROR_CHECK(tg28_sw_get_charge_voltage(s_pmic, &value));
    assert(value == 4200);

    ESP_ERROR_CHECK(tg28_sw_set_vindpm(s_pmic, 4360));
    ESP_ERROR_CHECK(tg28_sw_get_vindpm(s_pmic, &value));
    assert(value == 4360);

    ESP_ERROR_CHECK(tg28_sw_set_precharge_current(s_pmic, 125));
    ESP_ERROR_CHECK(tg28_sw_get_precharge_current(s_pmic, &value));
    assert(value == 125);

    ESP_ERROR_CHECK(tg28_sw_set_termination_current(s_pmic, 125, true));
    ESP_ERROR_CHECK(tg28_sw_get_termination_current(s_pmic, &value, &enabled));
    assert(value == 125 && enabled);
}

/* H5: interrupt events. The operator must short-press and long-press the
 * power key and plug then unplug VBUS while the test polls the status
 * registers; every latched event is logged with its bank and bits. Expected:
 * REG49 bit3 short press, bit2 long press, bit7 VBUS insert, bit6 VBUS
 * remove; a charge completion shows as REG4A bit4. Temperature- and
 * level-conditioned bits stay latched until the condition clears, so a bit
 * that survives the write-one-to-clear pass is an ongoing condition, not a
 * clear failure. */
static void test_h5_irq_events(void)
{
    ESP_LOGI(TAG, "H5: within 120s short-press and long-press the power key, "
             "then plug and unplug VBUS; every event is logged");
    ESP_ERROR_CHECK(tg28_sw_configure_power_key_interrupts(s_pmic,
                    TG28_SW_POWER_KEY_IRQ_SHORT_PRESS |
                    TG28_SW_POWER_KEY_IRQ_LONG_PRESS));
    ESP_ERROR_CHECK(tg28_sw_set_irq_enable_bit(s_pmic, TG28_SW_IRQ_VINSERT,
                    true));
    ESP_ERROR_CHECK(tg28_sw_set_irq_enable_bit(s_pmic, TG28_SW_IRQ_VREMOVE,
                    true));

    uint8_t seen[3] = {0};
    uint8_t status[3] = {0};
    ESP_ERROR_CHECK(tg28_sw_get_and_clear_interrupts(s_pmic, status));
    for (int i = 0; i < 240; ++i) {
        ESP_ERROR_CHECK(tg28_sw_get_and_clear_interrupts(s_pmic, status));
        for (int bank = 0; bank < 3; ++bank) {
            const uint8_t new_bits = status[bank] & ~seen[bank];
            if (new_bits != 0) {
                ESP_LOGI(TAG, "H5: bank%d new IRQ bits 0x%02x", bank, new_bits);
            }
            seen[bank] |= status[bank];
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "H5: captured REG48=0x%02x REG49=0x%02x REG4A=0x%02x; "
             "verify the bit mapping against the log manually",
             seen[0], seen[1], seen[2]);
    /* At least one bank1 event (bits 7:2) must have been captured. */
    assert((seen[TG28_SW_IRQ_BANK1] & 0xFC) != 0);
}

/* H6: enabling one IRQ bit must not disturb the other bits of its bank. */
static void test_h6_irq_enable_bit_isolated(void)
{
    const uint8_t bit = (uint8_t)(1U << (TG28_SW_IRQ_VINSERT % 8));
    uint8_t before = 0;
    uint8_t after = 0;
    bool enabled = false;

    ESP_ERROR_CHECK(tg28_sw_get_irq_enable(s_pmic, TG28_SW_IRQ_BANK1, &before));
    ESP_ERROR_CHECK(tg28_sw_set_irq_enable_bit(s_pmic, TG28_SW_IRQ_VINSERT,
                    true));
    ESP_ERROR_CHECK(tg28_sw_get_irq_enable(s_pmic, TG28_SW_IRQ_BANK1, &after));
    assert((after & bit) != 0);
    assert((after & (uint8_t)~bit) == (before & (uint8_t)~bit));
    ESP_ERROR_CHECK(tg28_sw_get_irq_enable_bit(s_pmic, TG28_SW_IRQ_VINSERT,
                    &enabled));
    assert(enabled);
}

/* H7: ADC channels versus a multimeter; VBAT/VBUS/VSYS should agree within
 * 1%. The TS pin is a fixed external input on this board, so its reading
 * only needs to be stable. */
static void test_h7_adc_channels(void)
{
    static const tg28_sw_adc_channel_t channels[] = {
        TG28_SW_ADC_CHANNEL_VBAT, TG28_SW_ADC_CHANNEL_VBUS,
        TG28_SW_ADC_CHANNEL_VSYS, TG28_SW_ADC_CHANNEL_TS,
    };
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i) {
        bool was_enabled = false;
        ESP_ERROR_CHECK(tg28_sw_get_adc_channel_enable(s_pmic, channels[i],
                        &was_enabled));
        ESP_ERROR_CHECK(tg28_sw_set_adc_channel_enable(s_pmic, channels[i],
                        true));
        vTaskDelay(pdMS_TO_TICKS(50));   /* channel settling time */
        uint16_t mv = 0;
        ESP_ERROR_CHECK(tg28_sw_read_adc_channel(s_pmic, channels[i], &mv));
        ESP_LOGI(TAG, "H7: ADC channel %d = %u mV", (int)channels[i], mv);
        ESP_ERROR_CHECK(tg28_sw_set_adc_channel_enable(s_pmic, channels[i],
                        was_enabled));
    }
}

/* H8: SOC read-out. Without a battery model file the fuel gauge only
 * self-learns and SOC accuracy is not guaranteed, so this just samples the
 * readout. Programming a supplier model (tg28_sw_program_battery_model)
 * needs battery-specific model data that is not available for this board
 * yet. */
static void test_h8_soc_readback(void)
{
    tg28_sw_status_t status = {0};
    for (int i = 0; i < 5; ++i) {
        ESP_ERROR_CHECK(tg28_sw_get_status(s_pmic, &status));
        ESP_LOGI(TAG, "H8: SOC = %u%%, VBAT = %u mV, charging = %d",
                 status.battery_percent, status.battery_mv, status.charging);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    test_setup();
    /* H1 must run first: it captures the OTP baseline before H2-H4 change
     * any register. */
    test_h1_otp_baseline();
    test_h2_dc1sw_powers_rgb();
    test_h3_charge_current();
    test_h4_charge_parameters();
    test_h5_irq_events();
    test_h6_irq_enable_bit_isolated();
    test_h7_adc_channels();
    test_h8_soc_readback();
    ESP_LOGI(TAG, "on-hardware test run finished");
}

#endif /* CONFIG_TG28_SW_TEST_WITH_HARDWARE */
