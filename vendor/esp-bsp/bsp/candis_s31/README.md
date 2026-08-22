# BSP: Candis-S31

| [Hardware repository](https://github.com/jlckfb/Candis-S31) | [API](API.md) | [Examples](#compatible-bsp-examples) | [![Component Registry](https://components.espressif.com/components/espressif/candis_s31/badge.svg)](https://components.espressif.com/components/espressif/candis_s31) | ![maintenance-status](https://img.shields.io/badge/maintenance-actively--developed-brightgreen.svg) |
| --- | --- | --- | --- | --- |

## Overview

Candis-S31 is a compact ESP32-S31 board built around a 2.0-inch 460 × 460
AMOLED. The board also carries capacitive touch, battery charging and power
management, an RTC, two USB Type-C connectors, an ES8389 audio codec, a DVP
camera connector, a microSD slot, and one addressable RGB LED.

This BSP follows schematic revision 0.5. The implementation and examples are
compile-tested with ESP-IDF 6.1, and the EVT1 hardware has completed bring-up
for the display, touch, audio, storage, USB, RTC, and PMIC domains. Camera
support is pending a corrected FPC adapter board; treat remaining
electrical-behavior notes as EVT1-validated unless marked otherwise.

## Capabilities and dependencies

<div align="center">
<!-- START_DEPENDENCIES -->

|     Available    |       Capability       |Controller/Codec|                                                  Component                                                 |   Version  |
|------------------|------------------------|----------------|------------------------------------------------------------------------------------------------------------|------------|
|:heavy_check_mark:|     :pager: DISPLAY    |     co5300     |                                                     idf                                                    |    >=6.1   |
|:heavy_check_mark:|:black_circle: LVGL_PORT|                |                                                                                                            |            |
|:heavy_check_mark:|    :point_up: TOUCH    |     cst820     |[espressif/esp_lcd_touch_cst820](https://components.espressif.com/components/espressif/esp_lcd_touch_cst820)|   ^1.0.0   |
|        :x:       | :radio_button: BUTTONS |                |                                                                                                            |            |
|        :x:       |   :white_circle: KNOB  |                |                                                                                                            |            |
|:heavy_check_mark:|  :musical_note: AUDIO  |                |       [espressif/esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev)       |   ^1.6.2   |
|:heavy_check_mark:| :speaker: AUDIO_SPEAKER|     es8389     |                                                                                                            |            |
|:heavy_check_mark:| :microphone: AUDIO_MIC |     es8389     |                                                                                                            |            |
|:heavy_check_mark:|  :floppy_disk: SDCARD  |                |                                                     idf                                                    |    >=6.1   |
|:heavy_check_mark:|       :bulb: LED       |                |   idf<br/>[espressif/led_indicator](https://components.espressif.com/components/espressif/led_indicator)   |>=6.1<br/>^2|
|:heavy_check_mark:|     :camera: CAMERA    |     OV5640     |           [espressif/esp_video](https://components.espressif.com/components/espressif/esp_video)           |    ~2.2    |
|:heavy_check_mark:|      :battery: BAT     |                |                                                     idf                                                    |    >=6.1   |
|        :x:       |    :video_game: IMU    |                |                                                                                                            |            |
|        :x:       | :thermometer: HUMITURE |                |                                                                                                            |            |

<!-- END_DEPENDENCIES -->
</div>

The TG28_SW driver is a reusable device component. Board-specific rail names,
voltage choices, enable ordering, and shutdown behavior stay in this BSP.
RX8130CE RTC and FUSB303B Type-C support are implemented locally because no
matching registry components are currently used by this board.

The board uses the switch-charger variant of the TG28 (I2C address 0x34 on
the low-power bus). It exposes thirteen rails through `bsp_pmic_regulator_*`:
DCDC1-DCDC4, ALDO1-ALDO4, BLDO1-BLDO2, CPUSLDO, and DLDO1-DLDO2. The
linear-charger variant's DCDC5 does not exist here. CPUSLDO is unconnected on
this board and stays off.

The DLDO1 pin is not an LDO on this board: the TG28 OTP (confirmation sheet
V1.3) straps it as the DC1SW load switch, so its voltage register is inert and
the output passes DCDC1 (3.3V) straight through to the WS2812B RGB LED. The
rail is OFF after power-on and software must open it explicitly, which is what
`bsp_pmic_switch_enable(BSP_PMIC_SWITCH_DC1SW, true)` does; the
`bsp_pmic_regulator_*` calls must not be used for it. `bsp_led_indicator_create()`
opens the switch and `bsp_power_safe_state()` closes it again. DLDO2 (DC4SW) is
an unconnected spare.

The TG28 driver can represent the discrete input-current limits
(100/500/900/1000/1500/2000mA), but this board has fixed Type-C1 Rd and no Rp
detector. `bsp_pmic_init()` therefore forces the REG16 input limit to
`BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA` (2000 mA) with exact readback
verification before other PMIC setup. That value is a register ceiling, not a
source capability claim: the TG28 backs the actual charge current off under
the input limit/VINDPM while the system load keeps priority, and firmware
cannot classify the C1 source, so PC-port current budgets are NOT guaranteed
- PC protection lives in the application charge controller's REG62 charge-current
ceiling. The public `bsp_pmic_set/get_input_current_limit` accepts the full
hardware whitelist; requests above the boot default are accepted only from
callers that have independently verified the connected source. Charger
control separately covers the battery constant-current limit (0-200mA in
25mA steps, then 300-1500mA in 100mA steps) and the discrete termination
voltage (4000/4100/4200/4350/4400mV). PMIC init also clears latched
interrupt status before enabling the power-key interrupts.

`bsp_pmic_init()` also forces a deterministic charge-profile baseline with
exact readback verification before any other setup (any mismatch aborts the
init and deletes the handle): REG62 charge current to
`BSP_PMIC_SAFE_CHARGE_CURRENT_MA` (50 mA; the register outlives an ESP-only
reset, so a ceiling raised by an earlier session is collapsed before a weak
source can be plugged in), REG64 charge voltage to
`BSP_PMIC_SAFE_CHARGE_VOLTAGE_MV` (4200 mV; POR is unspecified and the
vendor FAQ requires the register to match the fuel-gauge model CV or the
SOC jumps), REG61 precharge to 50 mA and REG63 termination to 25 mA with
termination enabled (vendor EVB recipe, manual section 4.5 step 5). The
matching public setters/getters are `bsp_pmic_set/get_charge_current`,
`bsp_pmic_set/get_charge_voltage`,
`bsp_pmic_set/get_precharge_current` and
`bsp_pmic_set/get_termination_current`. Init then programs the built-in
reference fuel-gauge model best-effort
(`src/bsp_pmic_reference_model.c`): the vendor generic 4.2 V-class
fallback, GPL-origin data that is NOT Apache-2.0 - redistribution requires
a license review, and replacing it with a properly licensed cell-specific
model is an upstream/external-release blocker. SOC accuracy with the
reference model is provisional; a materially different battery SKU or
chemistry needs a new model, not per-unit calibration. A download failure
only logs a warning; charging and the PMIC stay alive. A later
cell-specific model can replace the reference at runtime through
`bsp_pmic_program_battery_model()` (re-verifies; the driver-level
create-time `battery_model` hook is unused because it fails the whole
create on a download error). `bsp_pmic_status_t.fuel_gauge_valid` reports
whether the SOC estimate is currently backed by a model and
`fuel_gauge_reference_model` whether that model is the reference default
(reference accuracy) or a custom override; when invalid, treat
`battery_percent` as meaningless and never convert `battery_mv`
into a percentage instead. Validity is dropped before a runtime override
attempt and on init failure, so a partial download is never reported.

`bsp_power_safe_state()` is a low-level best-effort rail/GPIO sweep. Stop
active display, audio, camera, and USB owners first so they can release handles
and issue their protocol-level shutdown commands.

## Third-party notices

- The CO5300 initialization sequence in `bsp_display.c` is converted from the
  AM200Q460460LK module supplier's reference material. Its license status is
  being confirmed with the supplier; treat the sequence as supplier-provided
  reference data until that confirmation is complete.
- Touch support resolves `espressif/esp_lcd_touch_cst820` to the in-tree copy
  under `components/lcd_touch/esp_lcd_touch_cst820` (Apache-2.0) through
  `override_path` instead of pulling a registry package. The in-tree component
  is a self-maintained implementation, independent of the same-named
  `kodediy/esp_lcd_touch_cst820` registry package that earlier revisions of
  this BSP referenced; it keeps the module-specific CST820 report handling
  maintainable. It remains the Espressif `esp_lcd_touch` driver and is
  functionally equivalent.

## Compatible BSP examples

<div align="center">
<!-- START_EXAMPLES -->

| Example | Description | Try with ESP Launchpad |
| ------- | ----------- | ---------------------- |
| [Audio Example](https://github.com/espressif/esp-bsp/tree/master/examples/audio) | Play and record WAV file | [Flash Example](https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-bsp/config.toml&app=audio-) |
| [Display Example](https://github.com/espressif/esp-bsp/tree/master/examples/display) | Show an image on the screen with a simple startup animation (LVGL) | [Flash Example](https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-bsp/config.toml&app=display-) |
| [Display, Audio and Photo Example](https://github.com/espressif/esp-bsp/tree/master/examples/display_audio_photo) | Complex demo: browse files from filesystem and play/display JPEG, WAV, or TXT files (LVGL) | [Flash Example](https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-bsp/config.toml&app=display_audio_photo-) |
| [Camera Example](https://github.com/espressif/esp-bsp/tree/master/examples/display_camera_video) | Stream camera output to display (LVGL) | [Flash Example](https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-bsp/config.toml&app=display_camera_video) |
| [LVGL Benchmark Example](https://github.com/espressif/esp-bsp/tree/master/examples/display_lvgl_benchmark) | Run LVGL benchmark tests | - |
| [LVGL Demos Example](https://github.com/espressif/esp-bsp/tree/master/examples/display_lvgl_demos) | Run the LVGL demo player - all LVGL examples are included (LVGL) | [Flash Example](https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-bsp/config.toml&app=display_lvgl_demos-) |
| [Display SD card Example](https://github.com/espressif/esp-bsp/tree/master/examples/display_sdcard) | Example of mounting an SD card using SD-MMC/SPI with display interaction. This example is also supported on boards without a display. | [Flash Example](https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-bsp/config.toml&app=display_sdcard) |

<!-- END_EXAMPLES -->
</div>

## Using the BSP before release

For local bring-up, add `bsp/candis_s31` to `EXTRA_COMPONENT_DIRS` and require
the `candis_s31` component from the application. The common include is:

```c
#include "bsp/esp-bsp.h"
```

Call `bsp_board_init()` first. It sets direct enables and optional rails to a
disabled state; peripherals are initialized only when their individual BSP API
is called. See [API.md](API.md) for resource ownership and shutdown rules.

The reset, power-on, and boot keys are dedicated to the reset path, PMIC, and
boot strapping. They are not normal application GPIOs. `BSP_CAPS_BUTTONS` is
therefore zero: the generic audio example (`examples/audio`) compiles its
button handling out via `#if BSP_CAPS_BUTTONS`, so playback and recording
cannot be triggered from the board itself. See
`examples/audio/sdkconfig.bsp.candis_s31` for the resulting runtime
limitations. `bsp_spiffs_mount()` mounts the example SPIFFS partition
(`CONFIG_BSP_SPIFFS_MOUNT_POINT`, label `CONFIG_BSP_SPIFFS_PARTITION_LABEL`)
so example file content ships in the flash image.

## RTC and shared interrupt line

The RX8130CE RTC sits on the low-power I2C bus. `bsp_rtc_get_time()` and
`bsp_rtc_set_time()` handle the calendar; `bsp_rtc_set_alarm()` programs the
alarm compare fields, and `bsp_rtc_alarm_irq_enable()` switches the RTC's
active-low `/IRQ` output (`AIE`) without touching the compare settings. In
`bsp_rtc_alarm_t` each `*_en` flag includes its field in the comparison;
day-of-month and weekday are mutually exclusive because both share one
register in the RTC. With every field disabled the alarm fires once per
minute.

The RTC `/IRQ` and the TG28_SW interrupt are wired-ANDed onto
`BSP_PMIC_RTC_INT` (GPIO2, active low) through a buffer. Once
`bsp_rtc_alarm_irq_enable(true)` is in effect, a latched RTC alarm pulls
GPIO2 low and keeps it low until the flag is cleared; the line stays low
while either device has a pending event.

Use `bsp_rtc_get_and_clear_alarm_flag()` when only AF should be acknowledged;
it leaves the update and timer flags untouched. Use
`bsp_rtc_clear_interrupt_flags()` or `bsp_shared_irq_service()` when every
pending RTC source must be reported and cleared to release the shared line.

`bsp_shared_irq_service()` drains both devices over I2C until the line
releases and reports the combined flags. It can be polled from a task, or
driven by an interrupt: `bsp_shared_irq_register_callback()` installs a
low-level GPIO handler (edge triggering would lose an event that asserts
while the other device still holds the line low). The handler masks the
line and runs the callback in ISR context, which must only notify (for
example `xTaskNotifyFromISR()`); the actual I2C servicing still happens in
task context by calling `bsp_shared_irq_service()`, which re-arms the
interrupt before returning. Passing a NULL callback removes the handler
again. Register the callback after `bsp_board_init()`, which configures the
pin with interrupts disabled.

## Display and touch

The on-board 2.0-inch CO5300 AMOLED (QSPI, 460x460 active area inside a 470x460 GRAM window; the supplier init code sets the column window 10..469) and the CST820 capacitive touch panel (I2C, `BSP_I2C_NUM`) are both initialized by `bsp_display_start()`.

- **Sleep:** `bsp_display_enter_sleep()` / `bsp_display_exit_sleep()` put the panel into/out of sleep-in mode and put the CST820 into deep sleep. The touch controller has no wake pin, so `bsp_display_exit_sleep()` resets it over its RST GPIO and re-checks its chip ID.
- **Deep standby:** `bsp_display_enter_deep_standby()` additionally sends the CO5300 deep-standby command (RAM content lost). After `bsp_display_exit_deep_standby()` the full display pipeline is rebuilt, but LVGL widgets/screens are not recreated automatically; the application must show its screen again (e.g. `lv_screen_load()`).
- **Rotation:** `bsp_display_rotate()` rotates in software (LVGL rendering). On LVGL 9.5 the touch coordinates follow the display rotation automatically. 90/180-degree rotation may show a small offset on this panel; verify on hardware before relying on it.
- **TE synchronization:** with `CONFIG_BSP_LCD_TE_SYNC` (default y) the LVGL refresh is gated on the panel's tearing-effect output (GPIO16) through the esp_lvgl_adapter TE_SYNC profile: each TE-gated flush transfers the whole frame. At the 48 MHz QSPI limit a full frame takes ~17.6 ms, so full-screen motion tops out at ~30 fps while local animations track the 60 Hz TE beat; sparse, local invalidations keep the UI tear-free. Disable the option only for diagnostics - the adapter then flushes without TE alignment.
- **Touch is optional:** if the CST820 is missing or fails to initialize, `bsp_display_start()` still succeeds and logs a warning; the display keeps working without touch input.

See [API.md](API.md) for the full function reference.

## Camera module

The connector exposes an 8-bit DVP bus. The production camera module uses an
OV5640, and the checked-in camera example selects its
800 x 600 RGB565 DVP mode. Select the corresponding `esp_cam_sensor` option if
a different module is fitted.

The sensor is clocked with a 24 MHz XCLK (`BSP_CAMERA_XCLK_CLOCK_MHZ`); all
OV5640 register tables in `esp_cam_sensor` assume 24 MHz, and `bsp_camera.c`
enforces this with a compile-time check. Only the DVP video device is
initialized (`ESP_VIDEO_INIT_FLAGS_DVP`).

XCLK comes from the CAM controller, which divides it down from PLL_F160M and
drives it on `dvp_pin.xclk_io` whenever `xclk_io >= 0` and `xclk_freq > 0`
(`esp_cam_ctlr_dvp_output_clock()` in `esp_video_init.c`). No LEDC channel is
needed: the GPIO output matrix only keeps the signal attached last, so a second
source on the same pin is disconnected in practice. `CONFIG_BSP_CAMERA_XCLK_USE_LEDC`
can still route XCLK through LEDC for a diagnostic experiment and is disabled
by default; `CONFIG_BSP_CAMERA_XCLK_LEDC_CH` only applies when it is enabled.

This board has no autofocus hardware: the schematic carries no VCM driver and
the camera FPC's AF_VCC pin is tied to the 2.8 V camera rail through a 0 ohm
resistor with no I2C control path. The OV5640 therefore runs fixed focus, so
`esp_video_init_config_t.cam_motor` stays unset and only the DVP video device
is initialized. See the note in `bsp_camera.c`.

## Audio codec

The ES8389 codec sits on the main I2C bus (address 0x20) and on I2S
(MCLK=GPIO35, BCLK=GPIO18, WS=GPIO19, DOUT=GPIO8, DIN=GPIO44); the speaker
amplifier enable is GPIO42. Both speaker and microphone logical devices clock
the codec from BCLK with `no_dac_ref=true`; at 16 kHz/16-bit stereo this is an
exact 512 kHz, ratio-32 coefficient-table match.

`BSP_I2S_SAMPLE_RATE` is 16000 Hz and `bsp_audio_init()` uses it for its
default `i2s_std_config_t`. Keep any override inside the es8389 driver's
coefficient table (`coeff_div[]` in esp_codec_dev `1.6.x`:
8000/16000/24000/32000/44100/48000/88200/96000/192000 Hz) - the previous
22050 Hz default has no row there, so `es8389_config_sample()` cannot resolve
codec clocks for it on any BCLK-clocked path.

Speaker and microphone remain separate logical `esp_codec_dev` instances over
the same chip. Version 1.6.2 reference-counts that physical ES8389 and avoids
the repeated whole-chip reset in 1.5.11; the two BSP configs intentionally use
the same clock/reference fields so initialization order cannot change ADC
routing.

[![pre-commit](https://img.shields.io/badge/pre--commit-enabled-brightgreen?logo=pre-commit&logoColor=white)](https://github.com/pre-commit/pre-commit)
