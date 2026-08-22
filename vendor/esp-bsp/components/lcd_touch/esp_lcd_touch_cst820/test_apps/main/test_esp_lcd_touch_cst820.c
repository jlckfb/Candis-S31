/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_cst820.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "unity_test_runner.h"

/* Hardcoded wiring of the Candis-S31 test board this test app runs on
 * (same convention as the other lcd_touch test apps). Adjust these defines
 * when running on a different board. On Candis-S31 the CST820 sits on the
 * main I2C bus (GPIO33/34) behind the ALDO2 rail, with RST on GPIO17 and
 * INT on GPIO3; GPIO6/7 are the low-power bus of the PMIC and RTC and must
 * never be driven here. */
#define TEST_TOUCH_I2C_PORT       (0)
#define TEST_TOUCH_I2C_SDA        (GPIO_NUM_34)
#define TEST_TOUCH_I2C_SCL        (GPIO_NUM_33)
#define TEST_TOUCH_GPIO_INT       (GPIO_NUM_3)
#define TEST_TOUCH_GPIO_RST       (GPIO_NUM_17)
/* Only needs to cover the controller's native coordinate range; not tied to
 * the display panel resolution. */
#define TEST_TOUCH_H_RES          (460)
#define TEST_TOUCH_V_RES          (460)

/* Shared bring-up for tests that need a live controller. */
static void cst820_test_open(i2c_master_bus_handle_t *i2c_bus,
                             esp_lcd_panel_io_handle_t *touch_io,
                             esp_lcd_touch_handle_t *touch)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = TEST_TOUCH_I2C_PORT,
        .sda_io_num = TEST_TOUCH_I2C_SDA,
        .scl_io_num = TEST_TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    TEST_ESP_OK(i2c_new_master_bus(&bus_config, i2c_bus));

    const esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    TEST_ESP_OK(esp_lcd_new_panel_io_i2c(*i2c_bus, &io_config, touch_io));

    const esp_lcd_touch_config_t touch_config = {
        .x_max = TEST_TOUCH_H_RES,
        .y_max = TEST_TOUCH_V_RES,
        .rst_gpio_num = TEST_TOUCH_GPIO_RST,
        .int_gpio_num = TEST_TOUCH_GPIO_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
    };
    TEST_ESP_OK(esp_lcd_touch_new_i2c_cst820(*touch_io, &touch_config, touch));
}

static void cst820_test_close(i2c_master_bus_handle_t i2c_bus,
                              esp_lcd_panel_io_handle_t touch_io,
                              esp_lcd_touch_handle_t touch)
{
    TEST_ESP_OK(esp_lcd_touch_del(touch));
    TEST_ESP_OK(esp_lcd_panel_io_del(touch_io));
    TEST_ESP_OK(i2c_del_master_bus(i2c_bus));
}

TEST_CASE("CST820 initializes over I2C", "[cst820][i2c]")
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_touch_handle_t touch = NULL;

    cst820_test_open(&i2c_bus, &touch_io, &touch);
    cst820_test_close(i2c_bus, touch_io, touch);
}

TEST_CASE("CST820 sleep and monitor APIs reject NULL handles", "[cst820][no-hw]")
{
    /* No hardware needed: every implementation validates the handle before
     * touching the bus or any GPIO. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_touch_cst820_sleep(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_touch_cst820_wakeup(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_touch_cst820_enter_monitor_mode(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_touch_cst820_exit_monitor_mode(NULL));

    /* Framework wrappers must route to the same validation. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_touch_enter_sleep(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_touch_exit_sleep(NULL));
}

TEST_CASE("CST820 raw register accessors reject NULL handles", "[cst820][no-hw]")
{
    uint8_t byte = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_touch_cst820_read_reg(NULL, 0x00, &byte, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_touch_cst820_write_reg(NULL, 0x00, 0x00));
}

TEST_CASE("CST820 deep-sleep round-trip", "[cst820][hw]")
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_touch_handle_t touch = NULL;

    cst820_test_open(&i2c_bus, &touch_io, &touch);

    /* Enter deep sleep. The 0xA5 <- 0x03 command is an assumption pending
     * EVT verification (see the driver source); an I2C NAK here means this
     * firmware variant does not implement the command at all. */
    TEST_ESP_OK(esp_lcd_touch_cst820_sleep(touch));

    /* While asleep the controller must not answer I2C. Treat the outcome as
     * a diagnostic: if the chip still responds, the sleep command was a
     * no-op on this firmware (also pending EVT verification). */
    const esp_err_t probe = esp_lcd_touch_read_data(touch);
    if (probe == ESP_OK) {
        printf("cst820: controller still answering after sleep command "
               "(command may be a no-op on this firmware, pending EVT verification)\n");
    } else {
        printf("cst820: controller silent after sleep command, as expected\n");
    }

    /* Wake through the datasheet reset path and confirm the bus is back. */
    TEST_ESP_OK(esp_lcd_touch_cst820_wakeup(touch));
    TEST_ESP_OK(esp_lcd_touch_read_data(touch));

    cst820_test_close(i2c_bus, touch_io, touch);
}

TEST_CASE("CST820 monitor mode wakes the host on touch", "[cst820][hw][waketest]")
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_touch_handle_t touch = NULL;

    cst820_test_open(&i2c_bus, &touch_io, &touch);

    /* Standby tier: the controller keeps scanning at low frequency and
     * pulses INT when the panel is touched (datasheet standby mode). */
    TEST_ESP_OK(esp_lcd_touch_cst820_enter_monitor_mode(touch));

    /* Arm INT (GPIO3, active low on Candis-S31) as the light-sleep wake
     * source. Level-based wake: if EVT shows the standby IRQ pulse is too
     * short to be sampled, revisit the wake configuration. */
    TEST_ESP_OK(gpio_wakeup_enable(TEST_TOUCH_GPIO_INT, GPIO_INTR_LOW_LEVEL));
    TEST_ESP_OK(esp_sleep_enable_gpio_wakeup());

    printf("cst820: entering light sleep, touch the panel to wake up...\n");
    vTaskDelay(pdMS_TO_TICKS(100));   /* Let the console line drain first */

    TEST_ESP_OK(esp_light_sleep_start());

    /* Execution resumes here after the touch. */
    TEST_ASSERT_BIT_HIGH(ESP_SLEEP_WAKEUP_GPIO, esp_sleep_get_wakeup_causes());
    printf("cst820: woke up by touch interrupt\n");

    TEST_ESP_OK(gpio_wakeup_disable(TEST_TOUCH_GPIO_INT));
    TEST_ESP_OK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO));

    /* Best effort: the touch that woke the host may already be released by
     * the time we read, so only log the report instead of asserting on it. */
    if (esp_lcd_touch_read_data(touch) == ESP_OK) {
        esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
        uint8_t point_count = 0;
        if (esp_lcd_touch_get_data(touch, points, &point_count,
                                   CONFIG_ESP_LCD_TOUCH_MAX_POINTS) == ESP_OK && point_count > 0) {
            printf("cst820: wake report: id=%u x=%u y=%u\n",
                   points[0].track_id, points[0].x, points[0].y);
        }
    }

    /* Force the controller back to dynamic mode without waiting for another
     * touch (reset path; a touch would have exited standby on its own). */
    TEST_ESP_OK(esp_lcd_touch_cst820_exit_monitor_mode(touch));

    cst820_test_close(i2c_bus, touch_io, touch);
}

void app_main(void)
{
    printf("CST820 test application\n");
    unity_run_menu();
}
