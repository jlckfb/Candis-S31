# Candis-S31 board support component

| [Hardware repository](https://github.com/jlckfb/Candis-S31) | [API](API.md) | [Examples](#compatible-examples) |
| --- | --- | --- |

## Overview

Candis-S31 is a compact ESP32-S31 board built around a 2.0-inch 460 × 460
AMOLED. The board also carries capacitive touch, battery charging and power
management, an RTC, two USB Type-C connectors, an ES8389 audio codec, a DVP
camera connector, a microSD slot, and one addressable RGB LED.

This repository-owned board component follows schematic revision 0.5. The
implementation and examples are compile-tested with ESP-IDF 6.1, and the
component provides display, touch, storage, USB, RTC, PMIC, audio, camera and
RGB LED support for this board.

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
voltage choices, enable ordering, and shutdown behavior stay in this board component.
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
verification before other PMIC setup. That value is a register ceiling rather
than a source capability claim: the TG28 backs the actual charge current off
under the input limit/VINDPM while the system load keeps priority, and PC-port
protection lives in the application charge controller's REG62 charge-current
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
vendor FAQ requires the register to match the fuel-gauge model CV or the SOC
jumps), REG61 precharge to 50 mA and REG63 termination to 25 mA with
termination enabled (vendor EVB recipe, manual section 4.5 step 5). The
matching public setters/getters are `bsp_pmic_set/get_charge_current`,
`bsp_pmic_set/get_charge_voltage`,
`bsp_pmic_set/get_precharge_current` and
`bsp_pmic_set/get_termination_current`.

Init then verifies the TG28 factory ROM model without embedding its bytes: it
reads the model once through `tg28_sw_read_battery_model()` and uses a
successful read as the default model-availability check. The model bytes
remain in the PMIC and are never copied into this Apache-2.0 component. A read
failure only logs a warning; charging and the PMIC stay alive. A
battery-specific model obtained under a compatible license can replace the
factory model at runtime through `bsp_pmic_program_battery_model()` (the
driver-level create-time `battery_model` hook is unused because it fails the
whole create on a download error). `bsp_pmic_status_t.fuel_gauge_valid`
reports whether the current model was successfully verified or programmed,
and `fuel_gauge_reference_model` identifies the factory ROM model; when
invalid, treat `battery_percent` as meaningless and never convert
`battery_mv` into a percentage instead. Validity is dropped before a runtime
override attempt and on init failure, so a partial download is never reported.


`bsp_power_safe_state()` is a low-level best-effort rail/GPIO sweep. Stop
active display, audio, camera, and USB owners first so they can release handles
and issue their protocol-level shutdown commands.

## Third-party notices

- The CO5300 initialization sequence in `bsp_display.c` is converted from the
  AM200Q460460LK module supplier's reference material.
- Touch support uses the published `espressif/esp_lcd_touch_cst820`
  component (Apache-2.0), which follows the module-specific CST820 report
  format and exposes the common `esp_lcd_touch` API. Its framework sleep hooks
  map to the documented monitor-mode entry/exit helpers.

## Compatible examples

<div align="center">
<!-- START_EXAMPLES -->

| Example | Description |
| ------- | ----------- |
| [`examples/esp-idf/display-hello`](../../../examples/esp-idf/display-hello) | Board Manager plus LVGL display smoke test |
| [`examples/esp-idf/camera-test`](../../../examples/esp-idf/camera-test) | Continuous OV5640 preview on the AMOLED |
| [`examples/esp-idf/player`](../../../examples/esp-idf/player) | TF-card audio/video player |
| [`examples/esp-idf/low-power`](../../../examples/esp-idf/low-power) | Light sleep, deep sleep and screen-off state machine |

<!-- END_EXAMPLES -->
</div>

## Using the component

Standalone projects in this repository include `cmake/candis_components.cmake`,
which injects this component without an external checkout. The common include is:

```c
#include "bsp/esp-bsp.h"
```

Call `bsp_board_init()` first. It sets direct enables and optional rails to a
disabled state; peripherals are initialized only when their individual board
API is called. See [API.md](API.md) for resource ownership and shutdown rules.
The four reusable chip drivers are mapped to `vendor/idf-extra-components/`
by this component manifest.

The reset, power-on, and boot keys are dedicated to the reset path, PMIC, and
boot strapping. They are not normal application GPIOs. `BSP_CAPS_BUTTONS` is
therefore zero. The user-facing player example exposes its own
application controls instead of assuming generic board buttons.

`bsp_spiffs_mount()` mounts the example SPIFFS partition
(`CONFIG_BSP_SPIFFS_MOUNT_POINT`, label `CONFIG_BSP_SPIFFS_PARTITION_LABEL`)
when that partition is enabled by the consuming project.

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

`bsp_shared_irq_init()` installs the shared GPIO ISR service once. The BSP
display and input entry points reuse it, so registering PMIC/RTC callbacks
after TE/touch setup does not reinstall the service. Do not uninstall the
GPIO ISR service while any BSP display or input consumer is active.

## Display and touch

The on-board 2.0-inch CO5300 AMOLED (QSPI, 460x460 active area inside a 470x460 GRAM window; the supplier init code sets the column window 10..469) and the CST820 capacitive touch panel (I2C, `BSP_I2C_NUM`) are both initialized by `bsp_display_start()`.

- **Sleep:** `bsp_display_enter_sleep()` / `bsp_display_exit_sleep()` put the
  panel into/out of sleep-in mode and use the CST820 framework sleep hooks to
  enter/exit its documented monitor-mode path. Exiting resets the controller
  over its RST GPIO and re-checks its chip ID.
- **Deep standby:** `bsp_display_enter_deep_standby()` additionally sends the
  CO5300 deep-standby command (GRAM content lost). After
  `bsp_display_exit_deep_standby()` the full display pipeline is rebuilt; the
  CO5300 v2.1 driver replays its cached MADCTL rotation, but LVGL widgets/screens
  are not recreated automatically and the application must show its screen again.
- **Rotation:** `BSP_DISPLAY_ROTATE_180` defaults to enabled for the upside-down
  EVT1 panel and applies a hardware MX|MY mirror during the common panel init;
  it also mirrors default CST820 coordinates. `bsp_display_rotate()` uses the
  panel hardware callbacks when software rotation is disabled, or LVGL software
  rotation when `sw_rotate` is enabled. Disable the Kconfig option for an upright
  board spin and keep the touch transform matched.
- **TE synchronization:** with `CONFIG_BSP_LCD_TE_SYNC` (default y) the LVGL
  refresh is gated on the panel's tearing-effect output (GPIO16). In the BSP's
  partial-buffer LVGL port, the first chunk waits for TE; subsequent chunks
  use the double-buffered DMA pipeline. A 460x460 RGB565 update needs at least
  17.6 ms at 48 MHz QSPI, longer than this panel's measured 16.69 ms period.
  The measured TE-on rates are about 29.95 fps full-screen and 59.90 fps for
  local updates.

- **Touch is optional:** if the CST820 is missing or fails to initialize, `bsp_display_start()` still succeeds and logs a warning; the display keeps working without touch input.

See [API.md](API.md) for the full function reference. The declarative Board Manager definition is mirrored under `vendor/esp-board-manager/`.

## Camera module

The connector exposes an 8-bit DVP bus. The production camera module uses an
OV5640, and the checked-in camera example selects its
800 x 600 RGB565 DVP mode. Select the corresponding `esp_cam_sensor` option if
a different module is fitted.

EVT1 drives an exact 20 MHz XCLK from the XTAL-backed LEDC path. After the
upstream 800 x 600 mode table runs, the board profile programs a 760 MHz
sensor PLL1, 95 MHz 8-bit DVP byte clock (47.5 Mpixel/s RGB565), HTS=1896 and
VTS=835, for a 30.003 fps target. Relative to the measured
`0x3034/0x3035/0x3036/0x3037 = 0x1a/0x21/0xb0/0x13` profile, the final
`0x18/0x21/0x4c/0x12` dividers lower PCLK and VTS by about 19% while retaining
30 fps. PLL1 is 760 MHz and PCLK stays below the OV5640's 96 MHz maximum.
HTS remains unchanged, following OmniVision's dummy-line guidance. Matched
50/60 Hz banding caps exposure at 30.06/25.03 ms instead of 44-72 ms.

`CONFIG_BSP_CAMERA_XCLK_USE_LEDC=y` keeps the board-owned clock source and
passes `xclk_io=GPIO_NUM_NC` / `xclk_freq=0` to esp_video. Disabling it
selects esp_video's controller-driven 20 MHz XCLK, which is an exact divisor
of the ESP32-S31 160 MHz CAM source.

The BSP configures DVP capture for the OV5640 module: `bsp_camera_start()`
brings up power, XCLK, the DVP route and the sensor profile, and the
application drives the V4L2 stream.

## Audio codec

The ES8389 codec sits on the main I2C bus (7-bit address 0x10; the codec
configuration uses the 8-bit value 0x20) and on I2S
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

Open every BSP-owned codec with `bsp_audio_codec_open(device, &format)`.
It returns `ESP_CODEC_DEV_*` codes and restores the I2S running state needed
by the upstream format-change path after a previous close. The microphone
also needs TX running for the shared clock. The remaining stream operations
still use `esp_codec_dev_read/write/close`; `bsp_audio_deinit()` closes both
devices, releases I2S and powers the audio path off. Do not run operations
on a codec concurrently with its open, close or deinitialization.

[![pre-commit](https://img.shields.io/badge/pre--commit-enabled-brightgreen?logo=pre-commit&logoColor=white)](https://github.com/pre-commit/pre-commit)
