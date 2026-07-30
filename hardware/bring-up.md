# EVT1 bring-up notes

EVT1 is still in layout. The current schematic is revision 0.5, exported on 2026-07-28.

## Checks before fabrication

| Item | Question to close | Safe fallback |
|---|---|---|
| PMIC interrupt and reset | Confirm TG28_SW pin 38 behavior and the `CHIP_PU/PWROK` reset chain | Rework the interrupt or reset network |
| Display supply | Confirm the LCD connector pinout and panel supply requirements | Do not populate or power the display |
| U18 pin 13 / LCD_VDD | Pass only after the panel vendor confirms the pin-13 function, allowed voltage, and power sequence, and the measured rail stays in that range | Leave the panel unpopulated and keep both display enables off |
| Battery path | Confirm BATFET, eFuse, and VBUS wake behavior | Revise the PMIC control and protection |
| TG28 OTP | Read back or measure VRTC=3.0 V, DCDC4=1.8 V, REG62 default=50 mA, and the agreed BATFET/VBUS/BAT power-on bits on incoming parts | Keep charging and optional rails disabled; quarantine or reprogram the lot |
| GPIO36 strap | Scope TF_PWR_EN_N through the strap-sampling window and pass only if its pull-up is compatible with VDD_SPI=3.3 V and boot is repeatable | Keep GPIO36 high-impedance through sampling; rework the pull or SD power gate |
| UART0 series resistors | Repeat ROM sync and verified flashing through the 998 ohm TX/RX resistors at 115200, 460800, and the intended high baud without framing errors | Use the highest repeatable lower baud or rework the series resistors |
| Main-bus I2C addresses | Scan with the required rails on; pass only if ES8389 responds at 0x20 and FUSB303B at exactly one of 0x21/0x31 with valid identity | Do not initialize the conflicting device; rework its address strap |
| OV5640 AF/VCM | Obtain module-vendor confirmation of VCM voltage, supply ownership, and actuator command protocol, then demonstrate repeatable near/far focus | Leave autofocus disabled and keep any unverified VCM supply off |
| Camera / JTAG mux | Confirm GPIO54-57 are released from JTAG before DVP use and that camera capture is stable; document the alternative debug route | Disable camera while JTAG is active, or disable JTAG before powering the camera |
| Type-C2 source mode | Test DRP, short circuit, dual-plug, backfeed, and temperature behavior | Keep source mode disabled or DNP |
| Reset key | Confirm the TG28_SW `TG28_PWROK` output type, sink current, and timing | Isolate or rework the key input |

## First power-on order

1. Use a current-limited supply. Validate the charger, PMIC, and off-state rails before fitting or enabling large loads.
2. Measure 3.3 V, VRTC, VDD_SPI, the PSRAM rail, and `CHIP_PU`.
3. Check flash access and the 40 MHz clock.
4. Add RTC, display, audio, camera, TF card, and RGB one subsystem at a time.
5. Record voltage, ripple, inrush current, steady current, temperature, and off-state voltage for every controlled rail.
6. Test Type-C2 source behavior last.

## Suggested Factory order

Use the serial commands one at a time. Stop at the first unexpected voltage,
current, temperature, reset, or console error.

1. Run `board_info`, `power_status`, `flash_test`, and `psram_test` without any external load attached.
2. Run `i2c_scan lp`, `pmic_test`, `pmic power_on_source`, and `rtc_test`;
   compare battery and VBUS readings with a meter. Confirm
   `pmic charge_current` reports the agreed 50 mA default. Under current
   limiting and battery temperature monitoring, set 500 mA only for the target
   current test, then restore 50 mA. If the RTC reports the recovery epoch, set
   a known value with `rtc_set`, power-cycle the board, and check it again.
3. Measure each optional rail while enabling and disabling it with `peripheral_power`. Do not connect the panel, camera, speaker, or card until its off-state and on-state voltage are correct.
4. Run `display_test`, inspect all four colors, and exercise
   `display_brightness`, `display_sleep`/`display_wake`, and their `deep`
   variants. Confirm deep wake includes a reset-low pulse longer than 3 ms,
   then record `mark display pass|fail`.
5. Run `touch_test`, `led_test`, and `sdcard_test` with the required parts fitted.
6. Run `speaker_test` at the supplied low level, record the audible result, then run `microphone_test`.
7. Run `camera_test` and keep the ESP Video sensor log.
8. Run `irq_test` once with the shared line idle and again after a known RTC or PMIC event. The command must release GPIO2 after clearing both devices.
9. Run `typec_test` with no cable, then with known sink/source fixtures. Test `otg` only after the CC and boost path measurements are ready.
10. Run `report` and save the complete console log with the board serial number and rework state.

Visual and audible commands do not mark themselves as passed. A display
transfer, LED update, or audio write can succeed while the external device is
dark or silent.

## Shared interrupt

GPIO2 is shared by TG28_SW and RX8130. The interrupt handler must service both devices and continue until the line returns high. A single read-and-clear operation is not sufficient when both devices assert at the same time.

The Factory `irq_test` command performs this loop and records the accumulated
flags, number of service passes, and final line state.

## Evidence to keep

- board revision and serial number;
- supply current limit and input voltage;
- oscilloscope captures for reset and switched rails;
- firmware commit and ESP-IDF version;
- result of each test, including skipped and not-run items;
- rework applied to the board.

A compile result is not bring-up evidence.

## Bring-up report template

Create one report per physical board and attach the complete serial log and
measurement evidence.

| Field | Value |
|---|---|
| Board serial / fixture position | |
| Hardware revision and rework state | |
| Factory firmware commit | |
| BSP revision printed by `board_info` | |
| ESP-IDF and esptool versions | |
| Supply voltage / current limit | |
| Operator / UTC timestamp | |

For every command, record the exact command line, start/end timestamp, complete
console output, measured values or fixture result, and the final
`PASS`/`FAIL`/`SKIP` decision with rationale. Attach the final `report` JSON
summary, ROM boot log, oscilloscope captures, thermal observations, and any
known limitation. Hardware-dependent items remain **待 EVT1** until this
evidence exists.
