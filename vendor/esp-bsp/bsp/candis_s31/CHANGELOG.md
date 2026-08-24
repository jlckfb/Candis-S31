# ChangeLog

## v1.2.0 - 2026-08-22

### Features

* PMIC: add `bsp_pmic_read_adc_mv()` with the `bsp_pmic_adc_channel_t` channel list (VBAT/TS/VBUS/VSYS/TDIE); channels disabled at the OTP level are enabled for the measurement and restored
* PMIC: add the load-switch API `bsp_pmic_switch_enable()`/`bsp_pmic_switch_is_enabled()`/`bsp_pmic_switch_name()` with the `bsp_pmic_switch_t` list (`BSP_PMIC_SWITCH_DC1SW`/`BSP_PMIC_SWITCH_DC4SW`), wrapping the tg28_sw switch channels
* Power: add `bsp_power_set_safe_shutdown_callback()` so an application can release active protocol owners before `bsp_power_safe_state()` parks pins and removes rails
* PMIC: force a deterministic charge-profile baseline with exact readback verification at init (any mismatch aborts): REG62 charge current 50 mA (`BSP_PMIC_SAFE_CHARGE_CURRENT_MA`), REG64 charge voltage 4200 mV (`BSP_PMIC_SAFE_CHARGE_VOLTAGE_MV`; POR unspecified, must match the fuel-gauge model CV per vendor FAQ), REG61 precharge 50 mA and REG63 termination 25 mA with termination enabled (vendor EVB recipe section 4.5 step 5); adds the thin `bsp_pmic_set/get_precharge_current` and `bsp_pmic_set/get_termination_current` wrappers over the existing tg28_sw APIs
* PMIC: program the vendor generic 4.2 V-class reference fuel-gauge model at init, best-effort (failure logs a warning and leaves the gauge invalid without touching charging). The 128-byte table in `src/bsp_pmic_reference_model.c` is verbatim GPL-origin data - not Apache-2.0 - and replacing it with a properly licensed cell-specific model is an upstream/external-release blocker; `bsp_pmic_program_battery_model()` remains the runtime override
* PMIC: `bsp_pmic_status_t` gains `fuel_gauge_valid` (true only while the TG28 SOC estimate is backed by a programmed model; no voltage-to-percent substitution when false) and `fuel_gauge_reference_model` (true = the active valid model is the BSP reference default, SOC is reference accuracy, never per-battery calibrated); model validity is dropped before a runtime override attempt and on init failure/deinit so a partial or failed download can never be reported valid
* PMIC: add the `BSP_PMIC_CPUSLDO` rail enum, following the tg28_sw 0.3.0 rail table (unconnected on this board, kept off); rail count is now thirteen
* RTC: pass `backup_charge_enable = false` explicitly at driver creation (rx8130ce 0.3.0 config field); Candis-S31 uses a primary backup cell
* Power: select the board's TS input through the generalized `tg28_sw_set_ts_config` API (external fixed TS, current source off)

### Fixed

* Storage: roll back the SPIFFS registration when the post-mount information query fails, preserving the original error so callers can retry cleanly; add fault-injection coverage for the mount lifecycle
* Audio: require `esp_codec_dev` 1.6.2 and give the speaker/microphone instances one BCLK/no-DAC-reference policy. Version 1.5.11 reset the shared ES8389 for each logical instance; `mic -> speaker -> mic` left registers 0x02/0x23/0xF0 in the speaker policy and returned an exact-zero left ADC channel. The 1.6.x physical-codec reference manager plus matching configs removes that creation-order dependency
* Audio: `bsp_audio_init()` built its default `i2s_std_config_t` from a hard-coded 22050 Hz and ignored `BSP_I2S_SAMPLE_RATE` entirely, so overriding the macro had no effect; it now derives the rate from the macro. The default itself moves from 22050 Hz to 16000 Hz: 22050 Hz has no row in the es8389 coefficient table (`coeff_div[]` holds only 8000/16000/24000/32000/44100/48000/88200/96000/192000 Hz). The shared BCLK policy now resolves the 16 kHz/16-bit x2 row exactly instead of leaving the codec clocks at their open-time defaults
* Power: DLDO1 is modelled as the DC1SW load switch its OTP straps it to instead of as an adjustable LDO. The RGB LED rail is opened with `bsp_pmic_switch_enable(BSP_PMIC_SWITCH_DC1SW, true)` in `bsp_led_indicator_create()` and closed again in `bsp_power_safe_state()`; no voltage is programmed on DLDO1 any more
* Camera: XCLK is no longer driven twice. The CAM controller derives the 24 MHz sensor clock itself and drives it on `dvp_pin.xclk_io`, so the redundant LEDC channel is now behind `CONFIG_BSP_CAMERA_XCLK_USE_LEDC`, disabled by default (diagnostics only); `CONFIG_BSP_CAMERA_XCLK_LEDC_CH` only applies when it is enabled
* Interrupts: the shared PMIC/RTC line on GPIO2 is now level-triggered (`GPIO_INTR_LOW_LEVEL`) instead of falling-edge; an event asserted while the other device still holds the line low no longer goes unnoticed. The ISR masks the line and `bsp_shared_irq_service()` re-arms it after draining both devices. Re-registering a callback now replaces the previous handler instead of failing
* Power: make safe-state and peripheral shutdown best-effort so a failed GPIO/I2C step cannot skip later rails; invoke the application teardown callback first, remove display power in VCI → VBAT → ALDO1 order, park camera/display/touch/audio/SD/RGB interfaces before rail removal, and report direct-domain state from successful BSP writes instead of a disabled GPIO input buffer
* Display: send Display-Off and Sleep-In before teardown, park the complete QSPI/control interface after driver deletion, invalidate BSP LVGL handles even when an LVGL removal fails, keep power-safety teardown running, roll touch startup failures back, and reuse the CO5300 driver's 10 ms/150 ms reset timing when leaving deep standby
* PMIC: force the REG16 input-current limit to `BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA` (2000 mA) with exact readback verification at every init - the register outlives an ESP-only reset, so a limit raised by a previous session is collapsed before a weak source can be plugged in. The value is a register ceiling, not a source capability claim; requests above the default are accepted only from callers that have independently verified the connected source
* USB Host: drop the Type-C2 boost on every stop path, including client, event-task, and uninstall failures; terminal Type-C teardown no longer retains a controller handle after its supply is removed
* Interrupts: attempt both shared-line devices and re-arm GPIO2 even when either LP-I2C drain fails, preserving the first error without permanently masking later PMIC/RTC events

### Notes

* Driver dependencies raised to tg28_sw `^0.4.0`, rx8130ce `^0.4.0`, fusb303b `^0.2.0`

…

### Features

* Display: configure the CO5300 TE input (GPIO16 via R55) and fix the QSPI write command encoding; `BSP_LCD_BIGENDIAN` is now 1
* Display: panel sleep and deep-standby entry/exit through the CO5300 command path
* Power: display VBAT/VCI rail sequencing (ALDO1-sourced VCI, 2 ms staging, reverse order on power-down) and pin tristating for the camera and SD domains
* PMIC: replace the `BSP_PMIC_DCDC5` rail with `BSP_PMIC_DLDO1`/`BSP_PMIC_DLDO2`; expose input current limit, charge voltage, and VINDPM settings
* RTC: alarm support via `bsp_rtc_set_alarm`/`bsp_rtc_get_alarm`/`bsp_rtc_alarm_irq_enable` (`bsp_rtc_alarm_t`)
* Interrupts: shared PMIC/RTC IRQ line service with GPIO ISR dispatch and `bsp_shared_irq_register_callback`
* Camera: OV5640 DVP XCLK changed to 24 MHz; drop unused flip/rotation macros

### Notes

* Build baseline: ESP-IDF `v6.1-beta1`
