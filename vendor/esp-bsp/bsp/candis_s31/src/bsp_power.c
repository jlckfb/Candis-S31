/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/candis_s31.h"

typedef struct {
    const char *name;
    gpio_num_t gpio;
    uint8_t enabled_level;
} power_domain_config_t;

static const char *TAG = "candis_power";
static portMUX_TYPE s_safe_shutdown_lock = portMUX_INITIALIZER_UNLOCKED;
static bsp_power_safe_shutdown_cb_t s_safe_shutdown_callback;
static void *s_safe_shutdown_context;
static bool s_safe_state_running;

static const power_domain_config_t s_power_domains[BSP_POWER_DOMAIN_COUNT] = {
    [BSP_POWER_TYPE_C_CONTROL] = {"type_c_control", BSP_TYPE_C_CTRL_EN,
        BSP_TYPE_C_CTRL_EN_ACTIVE_LEVEL
    },
    [BSP_POWER_DISPLAY_VBAT] = {"display_vbat", BSP_LCD_VBAT_EN,
        BSP_LCD_VBAT_EN_ACTIVE_LEVEL
    },
    [BSP_POWER_SDCARD] = {"sdcard", BSP_SD_POWER_EN,
        BSP_SD_POWER_EN_ACTIVE_LEVEL
    },
    [BSP_POWER_DISPLAY_VCI] = {"display_vci", BSP_LCD_VCI_EN,
        BSP_LCD_VCI_EN_ACTIVE_LEVEL
    },
    [BSP_POWER_AUDIO_PA] = {"audio_pa", BSP_AUDIO_PA_EN,
        BSP_AUDIO_PA_EN_ACTIVE_LEVEL
    },
    [BSP_POWER_USB_OTG] = {"usb_otg", BSP_USB_OTG_EN,
        BSP_USB_OTG_EN_ACTIVE_LEVEL
    },
};

static const char *s_peripheral_names[BSP_PERIPHERAL_COUNT] = {
    [BSP_PERIPHERAL_DISPLAY] = "display",
    [BSP_PERIPHERAL_TOUCH] = "touch",
    [BSP_PERIPHERAL_AUDIO] = "audio",
    [BSP_PERIPHERAL_CAMERA] = "camera",
    [BSP_PERIPHERAL_SDCARD] = "sdcard",
    [BSP_PERIPHERAL_EXTERNAL_3V3] = "external_3v3",
};

static bool domain_is_valid(bsp_power_domain_t domain)
{
    return domain >= 0 && domain < BSP_POWER_DOMAIN_COUNT;
}

static bool peripheral_is_valid(bsp_peripheral_t peripheral)
{
    return peripheral >= 0 && peripheral < BSP_PERIPHERAL_COUNT;
}

static esp_err_t configure_output(gpio_num_t gpio, uint32_t level)
{
    /* Load the output latch before changing direction to avoid enable pulses. */
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio, level), TAG, "GPIO latch failed");
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
                             .mode = GPIO_MODE_OUTPUT,
                             .pull_up_en = GPIO_PULLUP_DISABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "GPIO config failed");
    return gpio_set_level(gpio, level);
}

/* Park the given pins as floating inputs (no pull-up/pull-down) so they
 * cannot back-feed a peripheral whose supply is about to be removed. */
static esp_err_t tristate_pins(const gpio_num_t *gpios, size_t count)
{
    uint64_t mask = 0;
    for (size_t index = 0; index < count; ++index) {
        mask |= 1ULL << gpios[index];
    }
    const gpio_config_t config = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

/* Camera control and DVP pins. All of them are parked as floating inputs
 * before the sensor rails come down so none can back-feed the 2.8 V DOVDD;
 * camera power-up re-drives PWDN/RST through camera_control_pins(). */
static const gpio_num_t s_camera_pins[] = {
    BSP_CAMERA_PWDN, BSP_CAMERA_RST,
    BSP_CAMERA_D0, BSP_CAMERA_D1, BSP_CAMERA_D2, BSP_CAMERA_D3,
    BSP_CAMERA_D4, BSP_CAMERA_D5, BSP_CAMERA_D6, BSP_CAMERA_D7,
    BSP_CAMERA_PCLK, BSP_CAMERA_XCLK, BSP_CAMERA_VSYNC, BSP_CAMERA_HSYNC,
};

/* Panel control and QSPI pins. The panel keeps VBAT from TG28_VSYS on EVT1,
 * but loses IOVCC/VCI, so every SoC-driven input must be high impedance
 * before those rails fall. */
static const gpio_num_t s_display_pins[] = {
    BSP_LCD_RST, BSP_LCD_TE, BSP_LCD_CS, BSP_LCD_QSPI_CLK,
    BSP_LCD_QSPI_DATA0, BSP_LCD_QSPI_DATA1,
    BSP_LCD_QSPI_DATA2, BSP_LCD_QSPI_DATA3,
};

/* Codec/PA serial pins. AUDIO_PA is disabled separately; these lines must
 * float before ALDO3 removes the ES8389 I/O supply. */
static const gpio_num_t s_audio_pins[] = {
    BSP_I2S_DOUT, BSP_I2S_BCLK, BSP_I2S_LRCLK,
    BSP_I2S_MCLK, BSP_I2S_DIN,
};

/* Card-detect keeps a pull-up while the card is powered; the detect and bus
 * pins float before the SD rail opens. */
static const gpio_num_t s_sd_pins[] = {
    BSP_SD_DET, BSP_SD_D0, BSP_SD_D1, BSP_SD_D2, BSP_SD_D3,
    BSP_SD_CLK, BSP_SD_CMD,
};

/* Requested state of each directly controlled power domain. The domains are
 * pure output pads with the input buffer disabled, so reading the pin back
 * is meaningless; track what the BSP actually drove. A domain the BSP never
 * drove stays UNKNOWN and bsp_power_domain_get() reports that instead of
 * pretending the hardware state is known. */
typedef enum {
    BSP_POWER_DOMAIN_STATE_UNKNOWN = 0,
    BSP_POWER_DOMAIN_STATE_ON,
    BSP_POWER_DOMAIN_STATE_OFF,
} bsp_power_domain_state_t;

static bsp_power_domain_state_t s_domain_states[BSP_POWER_DOMAIN_COUNT];

static esp_err_t regulator_start(bsp_pmic_regulator_t regulator,
                                 uint16_t millivolts)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_regulator_set_voltage(regulator, millivolts),
                        TAG, "cannot set peripheral voltage");
    const esp_err_t error = bsp_pmic_regulator_enable(regulator, true);
    if (error != ESP_OK) {
        /* The enable write may have reached the PMIC even when its I2C
         * completion failed. Best-effort disable leaves a failed transaction
         * in the safer state. */
        bsp_pmic_regulator_enable(regulator, false);
    }
    return error;
}

static void rollback_note(const char *item, esp_err_t error)
{
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%s rollback failed: %s", item, esp_err_to_name(error));
    }
}

/* Best-effort safe-state step: log the failure, keep the first error, and
 * continue so every rail still comes down. */
static esp_err_t safe_state_note(esp_err_t first_error, esp_err_t error,
                                 const char *item)
{
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", item, esp_err_to_name(error));
        if (first_error == ESP_OK) {
            first_error = error;
        }
    }
    return first_error;
}

static esp_err_t camera_control_pins(void)
{
    ESP_RETURN_ON_ERROR(configure_output(BSP_CAMERA_PWDN, 1), TAG,
                        "camera PWDN config failed");
    const esp_err_t rst_error = configure_output(BSP_CAMERA_RST, 0);
    if (rst_error != ESP_OK) {
        /* Roll back PWDN to a floating input so a partially-configured
         * state is never left on the sensor pads. */
        const gpio_num_t pwdn_pin[] = { BSP_CAMERA_PWDN };
        (void)tristate_pins(pwdn_pin, sizeof(pwdn_pin) / sizeof(pwdn_pin[0]));
    }
    return rst_error;
}

esp_err_t bsp_power_set_safe_shutdown_callback(
    bsp_power_safe_shutdown_cb_t callback, void *user_ctx)
{
    taskENTER_CRITICAL(&s_safe_shutdown_lock);
    if (s_safe_state_running) {
        taskEXIT_CRITICAL(&s_safe_shutdown_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_safe_shutdown_callback = callback;
    s_safe_shutdown_context = callback != NULL ? user_ctx : NULL;
    taskEXIT_CRITICAL(&s_safe_shutdown_lock);
    return ESP_OK;
}

esp_err_t bsp_power_safe_state(void)
{
    esp_err_t first_error = ESP_OK;
    bsp_power_safe_shutdown_cb_t shutdown_callback;
    void *shutdown_context;
    taskENTER_CRITICAL(&s_safe_shutdown_lock);
    if (s_safe_state_running) {
        taskEXIT_CRITICAL(&s_safe_shutdown_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_safe_state_running = true;
    shutdown_callback = s_safe_shutdown_callback;
    shutdown_context = s_safe_shutdown_context;
    taskEXIT_CRITICAL(&s_safe_shutdown_lock);

    if (shutdown_callback != NULL) {
        first_error = safe_state_note(first_error,
                                      shutdown_callback(shutdown_context),
                                      "application protocol teardown");
    }

    first_error = safe_state_note(first_error, bsp_usb_host_stop(),
                                  "USB Host and Type-C shutdown");

    /* Park every SoC-driven line that could back-feed an unpowered peripheral
     * BEFORE its supply is removed. Active owners must already be stopped, as
     * documented by bsp_power_safe_state(). */
    first_error = safe_state_note(first_error,
                                  tristate_pins(s_camera_pins,
                                          sizeof(s_camera_pins) /
                                          sizeof(s_camera_pins[0])),
                                  "camera pins hi-Z");
    const gpio_num_t touch_pins[] = { BSP_TOUCH_RST, BSP_TOUCH_INT };
    first_error = safe_state_note(first_error,
                                  tristate_pins(touch_pins,
                                          sizeof(touch_pins) /
                                          sizeof(touch_pins[0])),
                                  "touch pins hi-Z");
    first_error = safe_state_note(first_error,
                                  tristate_pins(s_display_pins,
                                          sizeof(s_display_pins) /
                                          sizeof(s_display_pins[0])),
                                  "LCD interface pins hi-Z");
    first_error = safe_state_note(first_error,
                                  tristate_pins(s_audio_pins,
                                          sizeof(s_audio_pins) /
                                          sizeof(s_audio_pins[0])),
                                  "audio interface pins hi-Z");
    first_error = safe_state_note(first_error,
                                  tristate_pins(s_sd_pins,
                                          sizeof(s_sd_pins) /
                                          sizeof(s_sd_pins[0])),
                                  "SD card pins hi-Z");

    /* Display rails in the required power-down order: VCI first, at least
     * 2ms, then VBAT, then the ALDO1 source. The display domains are
     * deliberately not part of the domain loop below, which would power them
     * down in enum order (VBAT before VCI). */
    first_error = safe_state_note(first_error,
                                  bsp_power_domain_set(BSP_POWER_DISPLAY_VCI, false),
                                  "LCD VCI disable");
    vTaskDelay(pdMS_TO_TICKS(2));
    first_error = safe_state_note(first_error,
                                  bsp_power_domain_set(BSP_POWER_DISPLAY_VBAT, false),
                                  "LCD VBAT disable");
    first_error = safe_state_note(first_error,
                                  bsp_pmic_regulator_enable(BSP_PMIC_ALDO1, false),
                                  "LCD ALDO1 disable");

    /* Remaining direct GPIO domains. */
    const bsp_power_domain_t domains[] = {
        BSP_POWER_TYPE_C_CONTROL, BSP_POWER_SDCARD,
        BSP_POWER_AUDIO_PA, BSP_POWER_USB_OTG,
    };
    for (size_t index = 0; index < sizeof(domains) / sizeof(domains[0]); ++index) {
        first_error = safe_state_note(first_error,
                                      bsp_power_domain_set(domains[index], false),
                                      "direct power domain disable");
    }

    /* The RGB data line floats before its DC1SW rail opens, so neither the
     * unpowered LED strip nor the panel can be held by a high pin. */
    const gpio_num_t rgb_pin[] = { BSP_LED_RGB_IO };
    first_error = safe_state_note(first_error,
                                  tristate_pins(rgb_pin,
                                          sizeof(rgb_pin) /
                                          sizeof(rgb_pin[0])),
                                  "RGB data hi-Z");

    /* These rails feed only optional peripherals in schematic revision 0.5.
     * ALDO1 is listed again as a safety net for the display sequence above;
     * re-disabling an already-off rail is a harmless no-op. */
    const bsp_pmic_regulator_t optional_rails[] = {
        BSP_PMIC_ALDO1, BSP_PMIC_ALDO2, BSP_PMIC_ALDO3, BSP_PMIC_ALDO4,
        BSP_PMIC_BLDO1, BSP_PMIC_BLDO2, BSP_PMIC_DCDC2, BSP_PMIC_DCDC4,
    };
    first_error = safe_state_note(first_error, bsp_pmic_init(),
                                  "TG28_SW safe-state access");
    /* The RGB LED rail is the DC1SW load switch (DLDO1 pin strapped in SWITCH
     * mode by OTP, input = DCDC1), not a programmable LDO, so it is closed
     * through the switch API rather than the regulator list below. */
    first_error = safe_state_note(first_error,
                                  bsp_pmic_switch_enable(BSP_PMIC_SWITCH_DC1SW, false),
                                  "TG28_SW RGB LED switch disable");
    for (size_t index = 0; index < sizeof(optional_rails) / sizeof(optional_rails[0]); ++index) {
        first_error = safe_state_note(first_error,
                                      bsp_pmic_regulator_enable(optional_rails[index], false),
                                      "TG28_SW optional rail disable");
    }
    taskENTER_CRITICAL(&s_safe_shutdown_lock);
    s_safe_state_running = false;
    taskEXIT_CRITICAL(&s_safe_shutdown_lock);
    return first_error;
}

esp_err_t bsp_power_domain_set(bsp_power_domain_t domain, bool enable)
{
    ESP_RETURN_ON_FALSE(domain_is_valid(domain), ESP_ERR_INVALID_ARG, TAG,
                        "invalid power domain");
    const power_domain_config_t *config = &s_power_domains[domain];
    const uint32_t level = enable ? config->enabled_level
                                  : !config->enabled_level;
    /* gpio_config() reserves output GPIOs in ESP-IDF 6.1. Reconfiguring an
     * already-owned direct power-domain pin works but emits a misleading
     * "conflict found" warning. These enables stay as outputs for the BSP's
     * lifetime, so configure each one once and use the output latch after
     * that; this also avoids needless direction rewrites on live rails. */
    const esp_err_t error = s_domain_states[domain] == BSP_POWER_DOMAIN_STATE_UNKNOWN
                            ? configure_output(config->gpio, level)
                            : gpio_set_level(config->gpio, level);
    if (error == ESP_OK) {
        s_domain_states[domain] = enable ? BSP_POWER_DOMAIN_STATE_ON
                                  : BSP_POWER_DOMAIN_STATE_OFF;
    }
    return error;
}

esp_err_t bsp_power_domain_get(bsp_power_domain_t domain, bool *enabled)
{
    ESP_RETURN_ON_FALSE(domain_is_valid(domain) && enabled != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid power state request");
    const bsp_power_domain_state_t state = s_domain_states[domain];
    ESP_RETURN_ON_FALSE(state != BSP_POWER_DOMAIN_STATE_UNKNOWN,
                        ESP_ERR_INVALID_STATE, TAG,
                        "power domain %s has not been driven by the BSP",
                        s_power_domains[domain].name);
    *enabled = state == BSP_POWER_DOMAIN_STATE_ON;
    return ESP_OK;
}

const char *bsp_power_domain_name(bsp_power_domain_t domain)
{
    return domain_is_valid(domain) ? s_power_domains[domain].name : "invalid";
}

esp_err_t bsp_peripheral_power_set(bsp_peripheral_t peripheral, bool enable)
{
    ESP_RETURN_ON_FALSE(peripheral_is_valid(peripheral), ESP_ERR_INVALID_ARG, TAG,
                        "invalid peripheral");

    switch (peripheral) {
    case BSP_PERIPHERAL_DISPLAY:
        if (enable) {
            /* Power-up sequence required by the CO5300 QSPI AMOLED:
             * ALDO1 sources the VCI rail, then VBAT, then VCI after 2ms,
             * and the panel needs ~10ms before it accepts commands. */
            esp_err_t error = regulator_start(BSP_PMIC_ALDO1, 3300);
            if (error != ESP_OK) {
                return error;
            }
            error = bsp_power_domain_set(BSP_POWER_DISPLAY_VBAT, true);
            if (error != ESP_OK) {
                rollback_note("LCD ALDO1",
                              bsp_pmic_regulator_enable(BSP_PMIC_ALDO1, false));
                return error;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            error = bsp_power_domain_set(BSP_POWER_DISPLAY_VCI, true);
            if (error != ESP_OK) {
                rollback_note("LCD VBAT",
                              bsp_power_domain_set(BSP_POWER_DISPLAY_VBAT, false));
                rollback_note("LCD ALDO1",
                              bsp_pmic_regulator_enable(BSP_PMIC_ALDO1, false));
                return error;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            return ESP_OK;
        }
        /* Power-down is the reverse order: VCI first, then VBAT after 2ms,
         * and finally the ALDO1 source. */
        esp_err_t display_error = ESP_OK;
        display_error = safe_state_note(
                            display_error,
                            bsp_power_domain_set(BSP_POWER_DISPLAY_VCI, false),
                            "LCD VCI disable");
        vTaskDelay(pdMS_TO_TICKS(2));
        display_error = safe_state_note(
                            display_error,
                            bsp_power_domain_set(BSP_POWER_DISPLAY_VBAT, false),
                            "LCD VBAT disable");
        return safe_state_note(
                   display_error,
                   bsp_pmic_regulator_enable(BSP_PMIC_ALDO1, false),
                   "LCD ALDO1 disable");

    case BSP_PERIPHERAL_TOUCH:
        if (enable) {
            ESP_RETURN_ON_ERROR(regulator_start(BSP_PMIC_ALDO2, 3300), TAG,
                                "touch rail failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            return ESP_OK;
        }
        return bsp_pmic_regulator_enable(BSP_PMIC_ALDO2, false);

    case BSP_PERIPHERAL_AUDIO:
        if (enable) {
            ESP_RETURN_ON_ERROR(regulator_start(BSP_PMIC_ALDO3, 3300), TAG,
                                "audio rail failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            return ESP_OK;
        }
        esp_err_t audio_error = safe_state_note(
                                    ESP_OK,
                                    bsp_power_domain_set(BSP_POWER_AUDIO_PA, false),
                                    "audio PA disable");
        return safe_state_note(
                   audio_error,
                   bsp_pmic_regulator_enable(BSP_PMIC_ALDO3, false),
                   "audio ALDO3 disable");

    case BSP_PERIPHERAL_CAMERA:
        if (enable) {
            ESP_RETURN_ON_ERROR(camera_control_pins(), TAG,
                                "camera control pins failed");
            esp_err_t error = regulator_start(BSP_PMIC_BLDO1, 2800);
            if (error != ESP_OK) {
                return error;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            error = regulator_start(BSP_PMIC_ALDO4, 2800);
            if (error != ESP_OK) {
                rollback_note("camera DOVDD",
                              bsp_pmic_regulator_enable(BSP_PMIC_BLDO1, false));
                return error;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            error = regulator_start(BSP_PMIC_DCDC2, 1500);
            if (error != ESP_OK) {
                rollback_note("camera AVDD",
                              bsp_pmic_regulator_enable(BSP_PMIC_ALDO4, false));
                rollback_note("camera DOVDD",
                              bsp_pmic_regulator_enable(BSP_PMIC_BLDO1, false));
                return error;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            return ESP_OK;
        }
        /* Park PWDN, RST and the DVP data/sync pins as floating inputs
         * before removing the supplies: a driven or pulled-up line would
         * back-feed the unpowered sensor through its pads (2.8 V DOVDD).
         * The video driver and camera_control_pins() reconfigure these pins
         * on the next power-up. */
        esp_err_t camera_error = safe_state_note(
                                     ESP_OK,
                                     tristate_pins(s_camera_pins,
                                             sizeof(s_camera_pins) /
                                             sizeof(s_camera_pins[0])),
                                     "camera pins hi-Z");
        camera_error = safe_state_note(
                           camera_error,
                           bsp_pmic_regulator_enable(BSP_PMIC_DCDC2, false),
                           "camera DVDD disable");
        camera_error = safe_state_note(
                           camera_error,
                           bsp_pmic_regulator_enable(BSP_PMIC_ALDO4, false),
                           "camera AVDD disable");
        return safe_state_note(
                   camera_error,
                   bsp_pmic_regulator_enable(BSP_PMIC_BLDO1, false),
                   "camera DOVDD disable");

    case BSP_PERIPHERAL_SDCARD:
        if (enable) {
            ESP_RETURN_ON_ERROR(bsp_power_domain_set(BSP_POWER_SDCARD, true), TAG,
                                "SD card power switch failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            /* Restore a neutral pin state once the rail is up: card-detect
             * gets its pull-up back and the bus pins float. The sdmmc
             * driver applies its own pin configuration when the card is
             * mounted, so this only covers the unmounted idle state. */
            const gpio_config_t detect_config = {
                .pin_bit_mask = 1ULL << BSP_SD_DET,
                                     .mode = GPIO_MODE_INPUT,
                                     .pull_up_en = GPIO_PULLUP_ENABLE,
                                     .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                     .intr_type = GPIO_INTR_DISABLE,
            };
            esp_err_t error = gpio_config(&detect_config);
            if (error == ESP_OK) {
                const gpio_num_t sd_bus_pins[] = {
                    BSP_SD_D0, BSP_SD_D1, BSP_SD_D2, BSP_SD_D3,
                    BSP_SD_CLK, BSP_SD_CMD,
                };
                error = tristate_pins(sd_bus_pins,
                                      sizeof(sd_bus_pins) /
                                      sizeof(sd_bus_pins[0]));
            }
            if (error != ESP_OK) {
                rollback_note("SD card power",
                              bsp_power_domain_set(BSP_POWER_SDCARD, false));
            }
            return error;
        }
        /* Hi-Z the card-detect and bus pins before opening the P-MOS
         * (BSP_SD_POWER_EN goes high) so they cannot back-feed the
         * unpowered card, then give the rail a moment to collapse. */
        esp_err_t sd_error = safe_state_note(
                                 ESP_OK,
                                 tristate_pins(s_sd_pins,
                                               sizeof(s_sd_pins) /
                                               sizeof(s_sd_pins[0])),
                                 "SD card pins hi-Z");
        sd_error = safe_state_note(
                       sd_error,
                       bsp_power_domain_set(BSP_POWER_SDCARD, false),
                       "SD card power switch disable");
        vTaskDelay(pdMS_TO_TICKS(2));
        return sd_error;

    case BSP_PERIPHERAL_EXTERNAL_3V3:
        if (enable) {
            return regulator_start(BSP_PMIC_BLDO2, 3300);
        }
        return bsp_pmic_regulator_enable(BSP_PMIC_BLDO2, false);

    default:
        return ESP_ERR_INVALID_ARG;
    }
}

const char *bsp_peripheral_name(bsp_peripheral_t peripheral)
{
    return peripheral_is_valid(peripheral) ? s_peripheral_names[peripheral] : "invalid";
}
