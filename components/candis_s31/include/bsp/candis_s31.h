/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ESP BSP: Candis-S31
 *
 * The pin map is based on schematic revision 0.5. Driver interfaces are
 * complete, but electrical behavior must still be validated on EVT1 boards.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "led_indicator.h"
#include "esp_video_device.h"
#include "rx8130ce.h"
#include "sdkconfig.h"

#include "bsp/config.h"
#include "bsp/display.h"
#include "bsp/touch.h"

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
#include "esp_lvgl_port.h"
#include "lvgl.h"
#endif

/** @defgroup boardname Board Name
 *  @brief BSP board identity.
 *  @{
 */
#define BSP_BOARD_CANDIS_S31
#define BSP_BOARD_NAME                         "Candis-S31"
#define BSP_BOARD_REVISION                     "EVT1 schematic v0.5"
/** @} */

/** @defgroup capabilities Capabilities
 *  @brief Features implemented by the BSP.
 *  @{
 */
#define BSP_CAPS_DISPLAY                       1
#define BSP_CAPS_TOUCH                         1
#define BSP_CAPS_BUTTONS                       0
#define BSP_CAPS_KNOB                          0
#define BSP_CAPS_AUDIO                         1
#define BSP_CAPS_AUDIO_SPEAKER                 1
#define BSP_CAPS_AUDIO_MIC                     1
#define BSP_CAPS_SDCARD                        1
#define BSP_CAPS_LED                           1
#define BSP_CAPS_CAMERA                        1
#define BSP_CAPS_BAT                           1
#define BSP_CAPS_IMU                           0
#define BSP_CAPS_HUMITURE                      0
/** @} */

/** @defgroup g01_i2c I2C
 *  @brief Main and low-power I2C buses.
 */
/** @defgroup g02_storage SD Card and SPIFFS
 *  @brief SDMMC storage and SPIFFS interfaces.
 */
/** @defgroup g03_audio Audio
 *  @brief I2S and ES8389 audio interface.
 */
/** @defgroup g04_display Display and Touch
 *  @brief AMOLED, touch, and LVGL interface.
 */
/** @defgroup g06_led LEDs
 *  @brief Addressable RGB LED interface.
 */
/** @defgroup g07_usb USB and Type-C
 *  @brief USB Host, power switch, and Type-C controller interface.
 */
/** @defgroup g09_battery Battery and Power
 *  @brief TG28_SW battery, regulator, and board power interface.
 */
/** @defgroup g12_camera Camera
 *  @brief DVP camera interface.
 */
/** @defgroup g99_others Board Control and RTC
 *  @brief Board initialization, RTC, and shared interrupt service.
 */

/** @addtogroup g02_storage
 *  @{
 */
#define BSP_SD_DET                             GPIO_NUM_0
#define BSP_SD_DET_ACTIVE_LEVEL                0
#define BSP_SD_D0                              GPIO_NUM_20
#define BSP_SD_D1                              GPIO_NUM_21
#define BSP_SD_D2                              GPIO_NUM_22
#define BSP_SD_D3                              GPIO_NUM_23
#define BSP_SD_CLK                             GPIO_NUM_24
#define BSP_SD_CMD                             GPIO_NUM_25
#define BSP_SD_POWER_EN                        GPIO_NUM_36
#define BSP_SD_POWER_EN_ACTIVE_LEVEL           0
#define BSP_SD_EN                              BSP_SD_POWER_EN
#define BSP_SD_MOUNT_POINT                     CONFIG_BSP_SD_MOUNT_POINT
#define BSP_SPIFFS_MOUNT_POINT                 CONFIG_BSP_SPIFFS_MOUNT_POINT
/** @} */

/** @addtogroup g09_battery
 *  @{
 */
#define BSP_TYPE_C_CTRL_EN                     GPIO_NUM_1
#define BSP_TYPE_C_CTRL_EN_ACTIVE_LEVEL        0
#define BSP_PMIC_RTC_INT                       GPIO_NUM_2
#define BSP_PMIC_RTC_INT_ACTIVE_LEVEL          0
#define BSP_TYPE_C_INT                         GPIO_NUM_43
#define BSP_TYPE_C_INT_ACTIVE_LEVEL            0
#define BSP_USB_OTG_EN                         GPIO_NUM_45
#define BSP_USB_OTG_EN_ACTIVE_LEVEL            1
/** @} */

/** @addtogroup g01_i2c
 *  @{
 */
#define BSP_LP_I2C_NUM                         CONFIG_BSP_LP_I2C_NUM
#define BSP_LP_I2C_SCL                         GPIO_NUM_6
#define BSP_LP_I2C_SDA                         GPIO_NUM_7

#define BSP_I2C_NUM                            CONFIG_BSP_I2C_NUM
#define BSP_I2C_SCL                            GPIO_NUM_33
#define BSP_I2C_SDA                            GPIO_NUM_34
/** @} */

/** @addtogroup g04_display
 *  @{
 */
#define BSP_TOUCH_INT                          GPIO_NUM_3
#define BSP_TOUCH_RST                          GPIO_NUM_17
#define BSP_LCD_TOUCH_INT                      BSP_TOUCH_INT
#define BSP_LCD_TOUCH_RST                      BSP_TOUCH_RST
#define BSP_LCD_VBAT_EN                        GPIO_NUM_5
#define BSP_LCD_VBAT_EN_ACTIVE_LEVEL           1
#define BSP_LCD_CS                             GPIO_NUM_10
#define BSP_LCD_RST                            GPIO_NUM_15
#define BSP_LCD_TE                             GPIO_NUM_16
#define BSP_LCD_QSPI_CLK                       GPIO_NUM_12
#define BSP_LCD_QSPI_DATA0                     GPIO_NUM_11
#define BSP_LCD_QSPI_DATA1                     GPIO_NUM_13
#define BSP_LCD_QSPI_DATA2                     GPIO_NUM_14
#define BSP_LCD_QSPI_DATA3                     GPIO_NUM_9
#define BSP_LCD_PCLK                           BSP_LCD_QSPI_CLK
#define BSP_LCD_DATA0                          BSP_LCD_QSPI_DATA0
#define BSP_LCD_DATA1                          BSP_LCD_QSPI_DATA1
#define BSP_LCD_DATA2                          BSP_LCD_QSPI_DATA2
#define BSP_LCD_DATA3                          BSP_LCD_QSPI_DATA3
#define BSP_LCD_VCI_EN                         GPIO_NUM_38
#define BSP_LCD_VCI_EN_ACTIVE_LEVEL            1
#define BSP_LCD_SPI_NUM                        SPI2_HOST
#define BSP_LCD_PIXEL_CLOCK_HZ                 (CONFIG_BSP_LCD_PIXEL_CLOCK_MHZ * 1000 * 1000)
#define BSP_LCD_X_GAP                          10
#define BSP_LCD_Y_GAP                          0
/** @} */

/** @addtogroup g06_led
 *  @{
 */
#define BSP_LED_RGB_IO                         GPIO_NUM_4
#define BSP_LEDS_NUM                           1
/** @} */

/** @addtogroup g03_audio
 *  @{
 */
/* Keep this rate inside the es8389 driver's coefficient table. coeff_div[]
 * (device/es8389/es8389.c, esp_codec_dev 1.6.x) only holds
 * 8000/16000/24000/32000/44100/48000/88200/96000/192000 Hz, and
 * es8389_config_sample() looks it up by Ratio = bits * 4 together with
 * MCLK = rate * bits * 4. At 16 bit, 22050 Hz asks for Ratio 64 @ 1411200 Hz,
 * which the table does not hold (its only 1411200 Hz row is Ratio 32 @
 * 44100 Hz), so get_coeff() fails and es8389_config_sample() returns
 * ESP_CODEC_DEV_NOT_SUPPORT; 16000 Hz resolves to Ratio 64 @ 1024000 Hz,
 * which is present, and is the rate verified on ES8389 hardware.
 * Both logical codec devices use the BCLK path with no_dac_ref=true, so the
 * 16-bit x2 request is Ratio 32 @ 512000 Hz and resolves exactly. */
#define BSP_I2S_SAMPLE_RATE                    16000
#define BSP_I2S_SLOT_MODE                      I2S_SLOT_MODE_STEREO
#define BSP_AUDIO_SPEAKER_CODEC                ES8389
#define BSP_AUDIO_MIC_CODEC                    ES8389
#define BSP_I2S_DOUT                           GPIO_NUM_8
#define BSP_I2S_BCLK                           GPIO_NUM_18
#define BSP_I2S_LRCLK                          GPIO_NUM_19
#define BSP_I2S_MCLK                           GPIO_NUM_35
#define BSP_AUDIO_PA_EN                        GPIO_NUM_42
#define BSP_AUDIO_PA_EN_ACTIVE_LEVEL           1
#define BSP_I2S_DIN                            GPIO_NUM_44
#define BSP_I2S_SCLK                           BSP_I2S_BCLK
#define BSP_I2S_LCLK                           BSP_I2S_LRCLK
#define BSP_I2S_DSIN                           BSP_I2S_DIN
#define BSP_POWER_AMP_IO                       BSP_AUDIO_PA_EN
/** @} */

/** @addtogroup g12_camera
 *  @brief Supported camera sensors: OV5640.
 *  @{
 */
#define BSP_CAMERA_RST                         GPIO_NUM_39
#define BSP_CAMERA_PWDN                        GPIO_NUM_40
#define BSP_CAMERA_D0                          GPIO_NUM_46
#define BSP_CAMERA_D1                          GPIO_NUM_47
#define BSP_CAMERA_D2                          GPIO_NUM_48
#define BSP_CAMERA_D3                          GPIO_NUM_49
#define BSP_CAMERA_D4                          GPIO_NUM_50
#define BSP_CAMERA_D5                          GPIO_NUM_51
#define BSP_CAMERA_D6                          GPIO_NUM_52
#define BSP_CAMERA_D7                          GPIO_NUM_53
#define BSP_CAMERA_PCLK                        GPIO_NUM_54
#define BSP_CAMERA_XCLK                        GPIO_NUM_55
#define BSP_CAMERA_GPIO_XCLK                   BSP_CAMERA_XCLK
#define BSP_CAMERA_VSYNC                       GPIO_NUM_56
#define BSP_CAMERA_HSYNC                       GPIO_NUM_57
#define BSP_CAMERA_DEVICE                      ESP_VIDEO_DVP_DEVICE_NAME
/** Validated OV5640 XCLK frequency on EVT1 (MHz). */
#define BSP_CAMERA_XCLK_CLOCK_MHZ              20
/** @} */

/** @addtogroup g99_others
 *  @{
 */
#define BSP_UART0_TX                           GPIO_NUM_58
#define BSP_UART0_RX                           GPIO_NUM_59

#define BSP_TG28_SW_I2C_ADDRESS                0x34
#define BSP_RX8130CE_I2C_ADDRESS               0x32
#define BSP_FUSB303B_I2C_ADDRESS_LOW           0x21
#define BSP_FUSB303B_I2C_ADDRESS_HIGH          0x31
/* Addresses above are 7-bit wire addresses. The ES8389 codec is addressed
 * through esp_codec_dev, which takes its 8-bit write address 0x20
 * (ES8389_CODEC_DEFAULT_ADDR); on the wire it answers at 7-bit 0x10. */
/** @} */

/* GPIO26/27/28/30/31/32 are reserved for flash and VDD_SPI (there is no
 * GPIO29). GPIO36, GPIO37, GPIO60, and GPIO61 are boot strapping pins.
 * GPIO36 is the VDD_SPI voltage strap (ESP32-S31 datasheet Table 3-4:
 * high = 3.3 V flash, low = 1.8 V flash). This board uses a 3.3 V
 * off-package W25Q128 flash, so VDD_SPI = 3.3 V and GPIO36 must sample
 * high at reset; ESP32-S31 v0.0 erratum SPI-855 also forbids 1.8 V
 * VDD_SPI flash boot. R6 (10 kOhm to VCC_3V3_MAIN) is the required
 * strap pull-up, not an error. GPIO36 also drives the active-low TF power
 * switch; firmware keeps it high (TF off) until an explicit post-boot
 * sdcard power-on, so the strap sample is never disturbed. GPIO54-57 are
 * MTDO/MTCK/MTDI/MTMS and conflict with camera PCLK/XCLK/VSYNC/HSYNC.
 * GPIO41 is unavailable to applications. */

/** @addtogroup g09_battery
 *  @{
 */
/** Directly controlled board power domains. */
typedef enum {
    BSP_POWER_TYPE_C_CONTROL = 0,
    BSP_POWER_DISPLAY_VBAT,
    BSP_POWER_SDCARD,
    BSP_POWER_DISPLAY_VCI,
    BSP_POWER_AUDIO_PA,
    BSP_POWER_USB_OTG,
    BSP_POWER_DOMAIN_COUNT,
} bsp_power_domain_t;

/** Board peripherals with a complete power sequence. */
typedef enum {
    BSP_PERIPHERAL_DISPLAY = 0,
    BSP_PERIPHERAL_TOUCH,
    BSP_PERIPHERAL_AUDIO,
    BSP_PERIPHERAL_CAMERA,
    BSP_PERIPHERAL_SDCARD,
    BSP_PERIPHERAL_EXTERNAL_3V3,
    BSP_PERIPHERAL_COUNT,
} bsp_peripheral_t;

/** TG28_SW rails used by the board. Must stay aligned with tg28_sw_regulator_t. */
typedef enum {
    BSP_PMIC_DCDC1 = 0,
    BSP_PMIC_DCDC2,
    BSP_PMIC_DCDC3,
    BSP_PMIC_DCDC4,
    BSP_PMIC_ALDO1,
    BSP_PMIC_ALDO2,
    BSP_PMIC_ALDO3,
    BSP_PMIC_ALDO4,
    BSP_PMIC_BLDO1,
    BSP_PMIC_BLDO2,
    BSP_PMIC_CPUSLDO,
    /** DLDO1 pin. The TG28 OTP on this board straps it as the DC1SW load
     *  switch, so its voltage register is inert: use BSP_PMIC_SWITCH_DC1SW
     *  with bsp_pmic_switch_enable() instead of the regulator calls. */
    BSP_PMIC_DLDO1,
    /** DLDO2 pin; unconnected spare, same OTP switch caveat (DC4SW). */
    BSP_PMIC_DLDO2,
    BSP_PMIC_REGULATOR_COUNT,
} bsp_pmic_regulator_t;

/**
 * TG28_SW load-switch outputs, mirroring tg28_sw_power_switch_t.
 *
 * A switch passes its input rail straight through, so no voltage programming
 * applies. On Candis-S31 the TG28 OTP (TG28 confirmation sheet V1.3) straps
 * the DLDO1 pin as DC1SW: its input is DCDC1 (3.3 V) and it feeds the WS2812B
 * RGB LED. The rail is OFF after power-on and software must open it
 * explicitly. DC4SW (DLDO2 pin) is an unconnected spare.
 */
typedef enum {
    BSP_PMIC_SWITCH_DC1SW = 0,  /**< DLDO1 pin as a switch, input = DCDC1 */
    BSP_PMIC_SWITCH_DC4SW,      /**< DLDO2 pin as a switch, input = DCDC4 */
    BSP_PMIC_SWITCH_COUNT,
} bsp_pmic_switch_t;

/** Read-only TG28_SW power and battery snapshot. */
typedef struct {
    uint8_t chip_id;
    uint8_t common_status0;
    uint8_t common_status1;
    uint16_t battery_mv;
    /** TG28 SOC estimate; only meaningful when fuel_gauge_valid is true
     * (the factory ROM model was verified or a custom model was programmed).
     * Treat it as meaningless otherwise; never derive a percentage from
     * battery_mv here. */
    uint8_t battery_percent;
    /** True after the factory ROM model is verified or a custom battery model
     * is successfully programmed in this boot. */
    bool fuel_gauge_valid;
    /** True while the active valid model is the TG28 factory ROM model.
     * False means a custom model is active or no model is valid. */
    bool fuel_gauge_reference_model;
    bool battery_present;
    bool vbus_present;
    bool charging;
    bool charge_done;
} bsp_pmic_status_t;

/** Channels of the TG28_SW SAR ADC, mirroring the driver channel list. */
typedef enum {
    BSP_PMIC_ADC_VBAT = 0,
    BSP_PMIC_ADC_TS,
    BSP_PMIC_ADC_VBUS,
    BSP_PMIC_ADC_VSYS,
    BSP_PMIC_ADC_TDIE,
    BSP_PMIC_ADC_COUNT,
} bsp_pmic_adc_channel_t;

/** Valid fields in bsp_pmic_early_snapshot_t. Each register group is read
 *  independently so a transient I2C failure does not discard the other
 *  power-loss evidence. */
typedef enum {
    BSP_PMIC_EARLY_STATUS_VALID = 1U << 0,
    BSP_PMIC_EARLY_POWER_SOURCE_VALID = 1U << 1,
    BSP_PMIC_EARLY_ADC_CONTROL_VALID = 1U << 2,
    BSP_PMIC_EARLY_IRQ_STATUS_VALID = 1U << 3,
} bsp_pmic_early_snapshot_valid_t;

/** First TG28 register values captured immediately after tg28_sw_create().
 *  This happens before the BSP writes the charge profile, programs or resets
 *  the fuel gauge, clears IRQs, or changes TS configuration. The snapshot is
 *  retained until the ESP resets; PMIC deinit/reinit never overwrites it. */
typedef struct {
    uint8_t status[2];       /**< REG00-01: PMU status */
    uint8_t power_source[2]; /**< REG20-21: power-on/off sources */
    uint8_t adc_control;     /**< REG30: ADC channel enables */
    uint8_t irq_status[3];   /**< REG48-4A: latched IRQ status */
    uint8_t valid_mask;      /**< OR of bsp_pmic_early_snapshot_valid_t */
} bsp_pmic_early_snapshot_t;

#define BSP_PMIC_ADC_DIAGNOSTIC_SAMPLE_COUNT 7

/** One timestamped raw ADC acquisition. Raw values retain all 14 register
 *  bits so negative-offset/overflow signatures near 0x3fff are observable. */
typedef struct {
    uint32_t elapsed_ms;
    uint16_t raw[BSP_PMIC_ADC_COUNT];
} bsp_pmic_adc_diagnostic_sample_t;

/** Controlled multi-channel ADC diagnostic. The BSP enables VBAT, VBUS,
 *  VSYS, and TDIE together, samples them for two seconds, then restores the
 *  caller's exact REG30 value even if a sample fails. */
typedef struct {
    uint8_t reg30_original;
    uint8_t reg30_enabled;
    uint8_t reg30_restored;
    bool enable_verified;
    bool restore_verified;
    size_t sample_count;
    bsp_pmic_adc_diagnostic_sample_t samples[BSP_PMIC_ADC_DIAGNOSTIC_SAMPLE_COUNT];
} bsp_pmic_adc_diagnostic_t;
/** @} */

/** @addtogroup g99_others
 *  @{
 */
/** Calendar time stored in the RX8130CE. */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} bsp_rtc_time_t;

/** RX8130CE retained status flags. */
typedef struct {
    uint8_t raw;
    bool time_valid;
    bool alarm;
    bool timer;
    bool update;
    bool reset;
    bool backup_voltage_low;
    bool backup_battery_full;
} bsp_rtc_status_t;

/** Alarm compare settings, aliased from the RX8130CE driver. */
typedef rx8130ce_alarm_t bsp_rtc_alarm_t;
/** @} */

/** @addtogroup g07_usb
 *  @{
 */
typedef enum {
    BSP_TYPE_C_ROLE_DISABLED = 0,
    BSP_TYPE_C_ROLE_SINK,
    BSP_TYPE_C_ROLE_SOURCE,
    BSP_TYPE_C_ROLE_DRP,
} bsp_type_c_role_t;

/** Board source policy accepts only USB Type-C default current (500 mA).
 *  Status reports preserve the partner advertisement, including NONE for Ra
 *  or no attachment; passing any non-default value to a board role or VBUS
 *  API fails with ESP_ERR_NOT_SUPPORTED. */
typedef enum {
    BSP_TYPE_C_CURRENT_DEFAULT = 0,
    BSP_TYPE_C_CURRENT_1_5_A,
    BSP_TYPE_C_CURRENT_3_0_A,
    BSP_TYPE_C_CURRENT_NONE,
} bsp_type_c_current_t;

/** FUSB303B connection snapshot. */
typedef struct {
    uint8_t i2c_address;
    uint8_t device_id;
    uint8_t device_type;
    uint8_t status;
    uint8_t status1;
    uint8_t type;
    uint8_t interrupt;
    uint8_t interrupt1;
    bool attached;
    bool vbus_ok;
    bool vbus_safe_0v;
    bool fault;
    bool remedy_active;
    uint8_t orientation;
    bsp_type_c_role_t role;
    bsp_type_c_current_t advertised_current; /**< Current advertised by the attached partner. */
} bsp_type_c_status_t;
/** @} */

/** @addtogroup g99_others
 *  @{
 */
/** Combined interrupt snapshot for the shared active-low line on GPIO2. */
typedef struct {
    uint8_t pmic[3];
    uint8_t rtc;
    unsigned service_passes;
    bool line_released;
} bsp_shared_irq_status_t;

/** Callback invoked from ISR context when the shared interrupt line asserts. */
typedef void (*bsp_shared_irq_callback_t)(void *arg);
/** @} */

/** @addtogroup g06_led
 *  @{
 */
typedef enum {
    BSP_LED_1 = 0,
    BSP_LED_NUM,
} bsp_led_t;

typedef enum {
    BSP_LED_ON = 0,
    BSP_LED_OFF,
    BSP_LED_BLINK_FAST,
    BSP_LED_BLINK_SLOW,
    BSP_LED_BREATHE_FAST,
    BSP_LED_BREATHE_SLOW,
    BSP_LED_MAX,
} bsp_led_effect_t;
/** @} */

/** @addtogroup g02_storage
 *  @{
 */
typedef struct {
    const esp_vfs_fat_sdmmc_mount_config_t *mount;
    sdmmc_host_t *host;
    union {
        const sdmmc_slot_config_t *sdmmc;
        const sdspi_device_config_t *sdspi;
    } slot;
} bsp_sdcard_cfg_t;
/** @} */

/** @addtogroup g12_camera
 *  @{
 */
typedef struct {
    uint8_t dummy;
} bsp_camera_cfg_t;
/** @} */

/** @addtogroup g07_usb
 *  @{
 */
/** Power source selection kept compatible with the common ESP-BSP USB API. */
typedef enum {
    BSP_USB_HOST_POWER_MODE_USB_DEV = 0,
} bsp_usb_host_power_mode_t;
/** @} */

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup g99_others
 *  @{
 */
/** Initialize the board in its disabled, low-risk state. */
esp_err_t bsp_board_init(void);
/** @} */

/** @addtogroup g01_i2c
 *  @{
 */
/** Initialize the main I2C bus. Multiple calls are allowed. */
esp_err_t bsp_i2c_init(void);
esp_err_t bsp_i2c_deinit(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/** Initialize the low-power I2C bus. Multiple calls are allowed. */
esp_err_t bsp_lp_i2c_init(void);
esp_err_t bsp_lp_i2c_deinit(void);
i2c_master_bus_handle_t bsp_lp_i2c_get_handle(void);
/** @} */

/** @addtogroup g09_battery
 *  @{
 */
/** Application-owned protocol teardown invoked by bsp_power_safe_state()
 *  before any GPIO is parked or rail is removed. The callback must release
 *  active display/audio/camera/storage owners while their supplies are still
 *  present, must not call bsp_power_safe_state() recursively, and should
 *  return its first teardown error after attempting all owned peripherals. */
typedef esp_err_t (*bsp_power_safe_shutdown_cb_t)(void *user_ctx);
/** Install or clear (callback == NULL) the application shutdown callback.
 *  Configure it before tasks can call bsp_power_safe_state(). */
esp_err_t bsp_power_set_safe_shutdown_callback(
    bsp_power_safe_shutdown_cb_t callback, void *user_ctx);
/** Best-effort low-level shutdown. The registered application callback runs
 *  first, then direct domains, optional TG28_SW rails, and back-feed-prone
 *  GPIOs are handled even when an earlier step fails. Returns the first error.
 *  Without a registered callback the application remains responsible for
 *  stopping every active protocol owner before entering this function. */
esp_err_t bsp_power_safe_state(void);
esp_err_t bsp_power_domain_set(bsp_power_domain_t domain, bool enable);
/** Return the last state successfully driven by bsp_power_domain_set(), or
 *  ESP_ERR_INVALID_STATE when this boot has not driven the domain yet. */
esp_err_t bsp_power_domain_get(bsp_power_domain_t domain, bool *enabled);
const char *bsp_power_domain_name(bsp_power_domain_t domain);

/** Apply the board-defined sequence for one complete peripheral supply. */
esp_err_t bsp_peripheral_power_set(bsp_peripheral_t peripheral, bool enable);
const char *bsp_peripheral_name(bsp_peripheral_t peripheral);

/** Safe charge-current default (REG62). The register outlives an
 *  ESP-only reset - the TG28 only resets when it loses both VBUS and
 *  battery - so bsp_pmic_init() forces this value with a verified
 *  readback to collapse any target a previous session raised. Raising
 *  the target afterwards is the application charge controller's
 *  decision. */
#define BSP_PMIC_SAFE_CHARGE_CURRENT_MA 50

/** Charge-profile baseline forced (with exact readback verification) by
 *  bsp_pmic_init(); a mismatch aborts the init. The values follow the
 *  vendor EVB recipe (manual section 4.5 step 5: 61H=0x02, 63H=0x01,
 *  64H=0x03) and the Linux reference default: REG61 precharge 50 mA,
 *  REG63 termination 25 mA with termination enabled, REG64 charge
 *  voltage 4200 mV. The FAQ requires the charge-voltage register to
 *  match the fuel-gauge model CV (the built-in reference model is a
 *  4.2 V-class profile) or the reported SOC jumps. REG64 POR is not
 *  specified, hence the deterministic program. */
#define BSP_PMIC_SAFE_CHARGE_VOLTAGE_MV 4200
#define BSP_PMIC_SAFE_PRECHARGE_CURRENT_MA 50
#define BSP_PMIC_SAFE_TERMINATION_CURRENT_MA 25

/** Board default input-current limit (REG16), applied and exact-readback
 *  verified at every bsp_pmic_init(). Relaxed to 2000 mA and aligned with
 *  the Factory diagnostic image (2026-08-21 user decision). This is a
 *  register ceiling, not a source capability claim: the TG28 backs the
 *  actual charge current off under this input limit/VINDPM while the
 *  system load keeps priority, and firmware cannot classify the C1
 *  source - PC-port current budgets are NOT guaranteed. PC protection
 *  lives in the application charge controller's 200 mA default REG62
 *  ceiling (see the demo svc_power policy). */
#define BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA 2000

/** TG28_SW access. Init preserves regulator voltage/enable OTP state for the
 *  boot snapshot but clamps REG16 input current from its 1500 mA POR value
 *  to BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA, forces the charge-profile
 *  baseline (REG62/64/61/63, see the BSP_PMIC_SAFE_* constants) with exact
 *  readback verification, and programs the built-in reference battery model
 *  best-effort. Runtime requests above the input-limit default are accepted
 *  only from callers that have independently verified the connected source. */
esp_err_t bsp_pmic_init(void);
esp_err_t bsp_pmic_deinit(void);
esp_err_t bsp_pmic_get_status(bsp_pmic_status_t *status);
esp_err_t bsp_pmic_get_power_on_source(uint8_t *source);

/** Set the REG61 precharge current (0-200 mA, 25 mA steps). */
esp_err_t bsp_pmic_set_precharge_current(uint16_t milliamps);
esp_err_t bsp_pmic_get_precharge_current(uint16_t *milliamps);
/** Set the REG63 termination current (0-200 mA, 25 mA steps) and the
 *  termination-enable bit; disabling keeps the current code. */
esp_err_t bsp_pmic_set_termination_current(uint16_t milliamps, bool enable);
esp_err_t bsp_pmic_get_termination_current(uint16_t *milliamps, bool *enabled);

/** Read the TG28_SW REG21 power-off-source latch. The register survives while
 *  the TG28 stays supplied (VBUS or battery), so after an unexpected system
 *  power cut the SoC must be revived with the PWRON key WITHOUT unplugging
 *  VBUS, then this read reveals whether the TG28 itself commanded the off. */
esp_err_t bsp_pmic_get_power_off_source(uint8_t *source);

/** Power off the board through the TG28_SW soft-PWROFF command (REG10 bit0).
 *  The PMU enters its off state as soon as the write is acknowledged: every
 *  rail but the RTCLDO shuts down, the board loses power, and the call does
 *  not return in practice. Flush any pending log output before calling. */
esp_err_t bsp_pmic_power_off(void);

esp_err_t bsp_pmic_set_charge_current(uint16_t milliamps);
esp_err_t bsp_pmic_get_charge_current(uint16_t *milliamps);
esp_err_t bsp_pmic_set_input_current_limit(uint16_t milliamps);
esp_err_t bsp_pmic_get_input_current_limit(uint16_t *milliamps);
esp_err_t bsp_pmic_set_vindpm(uint16_t millivolts);
esp_err_t bsp_pmic_get_vindpm(uint16_t *millivolts);
esp_err_t bsp_pmic_set_charge_voltage(uint16_t millivolts);
esp_err_t bsp_pmic_get_charge_voltage(uint16_t *millivolts);
esp_err_t bsp_pmic_read_registers(uint8_t register_address, uint8_t *values,
                                  size_t count);
/** Dump the TG28 fuel-gauge battery model area (128 bytes) into model.
 *  from_sram selects the programmed/learned SRAM area, false the factory
 *  ROM area. See tg28_sw_read_battery_model() for the procedure and the
 *  EVT open questions. */
esp_err_t bsp_pmic_read_battery_model(bool from_sram, uint8_t *model,
                                      size_t size);
esp_err_t bsp_pmic_program_battery_model(const uint8_t *model, size_t size);
esp_err_t bsp_pmic_regulator_set_voltage(bsp_pmic_regulator_t regulator, uint16_t millivolts);
esp_err_t bsp_pmic_regulator_get_voltage(bsp_pmic_regulator_t regulator, uint16_t *millivolts);
esp_err_t bsp_pmic_regulator_enable(bsp_pmic_regulator_t regulator, bool enable);
esp_err_t bsp_pmic_regulator_is_enabled(bsp_pmic_regulator_t regulator, bool *enabled);

/** Open or close a TG28_SW load-switch output (see bsp_pmic_switch_t). No
 *  voltage programming applies in switch mode; the switch passes its input
 *  rail straight through. BSP_PMIC_SWITCH_DC1SW powers the RGB LED on this
 *  board and is OFF after power-on. */
esp_err_t bsp_pmic_switch_enable(bsp_pmic_switch_t sw, bool enable);
esp_err_t bsp_pmic_switch_is_enabled(bsp_pmic_switch_t sw, bool *enabled);
const char *bsp_pmic_switch_name(bsp_pmic_switch_t sw);

esp_err_t bsp_pmic_get_and_clear_interrupts(uint8_t status[3]);

/** Copy the IRQ status snapshot captured at bsp_pmic_init() time, before the
 *  init-time clear. This preserves events latched across an SoC power
 *  collapse (e.g. a regulator over-current lockout that killed 3V3 while the
 *  TG28 stayed alive on VBUS). *valid is false if the snapshot was never
 *  captured. */
esp_err_t bsp_pmic_get_boot_irq_snapshot(uint8_t status[3], bool *valid);

/** Copy the first post-create/pre-configuration register snapshot. */
esp_err_t bsp_pmic_get_early_snapshot(bsp_pmic_early_snapshot_t *snapshot);
const char *bsp_pmic_regulator_name(bsp_pmic_regulator_t regulator);

/** Read one TG28_SW ADC channel in millivolts. The TDIE channel reports the
 *  die-temperature sensor voltage, not a temperature; EVT1 fixes TS to
 *  ground and has no battery NTC, so a TS value is never a valid battery
 *  temperature. A channel disabled at the OTP level is enabled for the
 *  measurement and restored afterwards. Prefer bsp_pmic_run_adc_diagnostic()
 *  when raw codes, conversion settling, or overflow validity matter. */
esp_err_t bsp_pmic_read_adc_mv(bsp_pmic_adc_channel_t channel, uint16_t *millivolts);

/** Capture raw ADC data at 0/50/100/200/500/1000/2000 ms after enabling
 *  REG30 bits 0,2,3,4 in one write. The original control byte is restored
 *  and verified before return. */
esp_err_t bsp_pmic_run_adc_diagnostic(bsp_pmic_adc_diagnostic_t *diagnostic);
/** @} */

/** @addtogroup g99_others
 *  @{
 */
/** RX8130CE access. */
esp_err_t bsp_rtc_init(void);
esp_err_t bsp_rtc_deinit(void);
esp_err_t bsp_rtc_get_time(bsp_rtc_time_t *time, bsp_rtc_status_t *status);
esp_err_t bsp_rtc_set_time(const bsp_rtc_time_t *time);
esp_err_t bsp_rtc_get_status(bsp_rtc_status_t *status);
esp_err_t bsp_rtc_set_alarm(const bsp_rtc_alarm_t *alarm);
esp_err_t bsp_rtc_get_alarm(bsp_rtc_alarm_t *out_alarm);
esp_err_t bsp_rtc_alarm_irq_enable(bool enable);
/** Report and clear only the RX8130CE alarm flag (AF), leaving UF/TF intact.
 *  alarm_flag may be NULL when the caller only needs to clear AF. */
esp_err_t bsp_rtc_get_and_clear_alarm_flag(bool *alarm_flag);
esp_err_t bsp_rtc_clear_interrupt_flags(uint8_t *flags);

/** Initialize the shared GPIO ISR service once, before display/input consumers.
 *  The display and callback entry points call this automatically. */
esp_err_t bsp_shared_irq_init(void);

/** Service TG28_SW and RX8130CE until their shared interrupt line is released.
 *  When a callback is registered, the line is re-armed before returning. */
esp_err_t bsp_shared_irq_service(bsp_shared_irq_status_t *status);

/** Register a callback for the shared active-low interrupt line on GPIO2.
 *  The line is level-triggered (wire-ORed sources share it), masked in the
 *  ISR, and re-armed by bsp_shared_irq_service(); the callback runs in ISR
 *  context and must only notify a task. Pass NULL to unregister. */
esp_err_t bsp_shared_irq_register_callback(bsp_shared_irq_callback_t cb, void *arg);
/** @} */

/** @addtogroup g07_usb
 *  @{
 */
/** FUSB303B access. Every explicit role selection first clears the 5 V boost
 *  latch and never re-enables it; bsp_usb_otg_power_set(true, ...) performs
 *  the only supported Source-before-boost sequence. Native USB Host owns the
 *  role and boost lifecycle while running, so direct role/power/deinit calls
 *  then fail with ESP_ERR_INVALID_STATE. Type-C2 only advertises the USB
 *  500 mA default: other current requests fail with ESP_ERR_NOT_SUPPORTED. */
esp_err_t bsp_type_c_init(void);
esp_err_t bsp_type_c_deinit(void);
esp_err_t bsp_type_c_get_status(bsp_type_c_status_t *status, bool clear_interrupts);
esp_err_t bsp_type_c_set_role(bsp_type_c_role_t role, bsp_type_c_current_t current);
esp_err_t bsp_usb_otg_power_set(bool enable, bsp_type_c_current_t current);

/** Install or remove the native USB Host library for the Type-C2 connector.
 *  limit_500mA must be true (the only source current this board advertises);
 *  passing false fails with ESP_ERR_NOT_SUPPORTED. */
esp_err_t bsp_usb_host_start(bsp_usb_host_power_mode_t mode, bool limit_500mA);
esp_err_t bsp_usb_host_stop(void);
/** @} */

/** @addtogroup g02_storage
 *  @{
 */
/** SDMMC storage. */
bool bsp_sdcard_is_inserted(void);
void bsp_sdcard_get_sdmmc_host(int slot, sdmmc_host_t *config);
void bsp_sdcard_sdmmc_get_slot(int slot, sdmmc_slot_config_t *config);
void bsp_sdcard_sdspi_get_slot(spi_host_device_t spi_host, sdspi_device_config_t *config);
esp_err_t bsp_sdcard_sdmmc_mount(bsp_sdcard_cfg_t *cfg);
esp_err_t bsp_sdcard_sdspi_mount(bsp_sdcard_cfg_t *cfg);
esp_err_t bsp_sdcard_mount(void);
esp_err_t bsp_sdcard_unmount(void);
sdmmc_card_t *bsp_sdcard_get_handle(void);

/** SPIFFS on the "storage" partition. */
esp_err_t bsp_spiffs_mount(void);
esp_err_t bsp_spiffs_unmount(void);
/** @} */

/** @addtogroup g03_audio
 *  @{
 */
/** I2S and ES8389 audio. */
esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config);
esp_err_t bsp_audio_deinit(void);
const audio_codec_data_if_t *bsp_audio_get_codec_itf(void);
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
/** Open/reopen a BSP-owned codec, preparing its I2S clocks for esp_codec_dev.
 *  Returns ESP_CODEC_DEV_* codes. Serialize with codec I/O, close and deinit. */
int bsp_audio_codec_open(esp_codec_dev_handle_t device,
                         esp_codec_dev_sample_info_t *format);
esp_err_t bsp_audio_codec_deinit(esp_codec_dev_handle_t device);
/** @} */

/** @addtogroup g12_camera
 *  @{
 */
/** DVP camera pipeline. Sensor support is selected in project configuration. */
esp_err_t bsp_camera_start(const bsp_camera_cfg_t *cfg);
/** Reapply the board sensor override after esp_video_open()/VIDIOC_S_FMT,
 *  before REQBUFS/STREAMON; open/format setup overwrites the pre-open pass.
 *  v4l2_pixel_format must be V4L2_PIX_FMT_RGB565X or V4L2_PIX_FMT_UYVY.
 *  Do not apply the profile while the stream is running. */
esp_err_t bsp_camera_apply_workaround(uint32_t v4l2_pixel_format);
esp_err_t bsp_camera_stop(void);
/** @} */

/** @addtogroup g06_led
 *  @{
 */
/** Addressable RGB LED. */
esp_err_t bsp_led_indicator_create(led_indicator_handle_t led_array[], int *led_cnt, int led_array_size);
esp_err_t bsp_led_set(led_indicator_handle_t handle, bool on);
/** @} */

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
/** @addtogroup g04_display
 *  @{
 */
typedef struct {
    lvgl_port_cfg_t lvgl_port_cfg;
    uint32_t buffer_size;
    bool double_buffer;
    struct {
        unsigned int buff_dma : 1;
        unsigned int buff_spiram : 1;
        unsigned int sw_rotate : 1;
        unsigned int direct_mode : 1; /*!< Full-frame buffers, DIRECT render, per-cycle TE-gated band flush */
    } flags;
} bsp_display_cfg_t;

lv_display_t *bsp_display_start(void);
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);
/** Terminal LVGL teardown: even if an LVGL remove operation fails, all BSP
 *  handles are invalidated and panel/touch pins and rails are made safe.
 *  A failed return therefore requires a reboot, not a retry with old handles. */
esp_err_t bsp_display_stop(void);
lv_indev_t *bsp_display_get_input_dev(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
void bsp_display_rotate(lv_display_t *display, lv_display_rotation_t rotation);
esp_err_t bsp_display_enter_sleep(void);
esp_err_t bsp_display_exit_sleep(void);
/** Panel-only variants: sleep the CO5300 (TE gate, backlight, SLPIN/SLPOUT)
 *  without touching the CST820. Use these for screen-off paths that must
 *  keep the touch controller responsive (its auto-standby keeps INT wake
 *  alive); the full variants above deep-sleep the touch controller and
 *  hard-reset it on exit, which a touch-wake design cannot use. */
esp_err_t bsp_display_enter_sleep_panel(void);
esp_err_t bsp_display_exit_sleep_panel(void);
esp_err_t bsp_display_enter_deep_standby(void);
esp_err_t bsp_display_exit_deep_standby(void);
/** @} */
#endif

#ifdef __cplusplus
}
#endif
