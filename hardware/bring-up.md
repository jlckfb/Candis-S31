# EVT1 bring-up notes

EVT1 is still in layout. The current schematic is revision 0.5, exported on 2026-07-28.

## Checks before fabrication

| Item | Question to close | Safe fallback |
|---|---|---|
| PMIC interrupt and reset | Confirm TG28_SW pin 38 behavior and the `CHIP_PU/PWROK` reset chain | Rework the interrupt or reset network |
| Display supply | Confirm the LCD connector pinout and panel supply requirements | Do not populate or power the display |
| U18 pin 13 / LCD_VDD | Pass only after the panel vendor confirms the pin-13 function, allowed voltage, and power sequence, and the measured rail stays in that range | Leave the panel unpopulated and keep both display enables off |
| Battery path | Confirm BATFET, eFuse, and VBUS wake behavior | Revise the PMIC control and protection |
| TG28 OTP | Read back or measure VRTC=3.0 V, REG62 default=50 mA, and the agreed BATFET/VBUS/BAT power-on bits on incoming parts; confirm over the LP I2C bus (0x34) that DCDC3, DCDC4, CPUSLDO, and DLDO2 (DC4SW) read back disabled, matching the unconnected schematic outputs | Keep charging and optional rails disabled; quarantine or reprogram the lot |
| GPIO36 strap | Scope TF_PWR_EN_N through the strap-sampling window and pass only if its pull-up is compatible with VDD_SPI=3.3 V and boot is repeatable | Keep GPIO36 high-impedance through sampling; rework the pull or SD power gate |
| UART0 series resistors | Repeat ROM sync and verified flashing through the series resistors — TX chain R17+R37 (499 ohm each, 998 ohm total), RX chain R36 (499 ohm) — at 115200, 460800, and the intended high baud without framing errors | Use the highest repeatable lower baud or rework the series resistors |
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
current, temperature, reset, or console error. If the console does not come
up, or flashing fails, stop and follow
[`firmware/recovery/`](../firmware/recovery/README.md) for download-mode
recovery instead of re-running stages blindly.

1. Run `board_info`, `power_status`, `flash_test`, and `psram_test` without any external load attached.
2. Run `i2c_scan lp`, `pmic_test`, `pmic power_on_source`, and `rtc_test`;
   compare battery and VBUS readings with a meter. Confirm
   `pmic charge_current` reports the agreed 50 mA default. Under current
   limiting and battery temperature monitoring, set 500 mA only for the target
   current test, then restore 50 mA. EVT1 has no battery NTC (the TS pin is a
   fixed input), so monitor the cell with an external probe and watch the
   TG28 die-sensor trend through `pmic temperature` (TDIE is a sensor
   voltage, not degrees). If the RTC reports the recovery epoch, set
   a known value with `rtc_set`, power-cycle the board, and check it again;
   results persist across the power cycle in NVS, so the final `report`
   still contains the pre-cycle tests.
3. Measure each optional rail while enabling and disabling it with `peripheral_power`. Do not connect the panel, camera, speaker, or card until its off-state and on-state voltage are correct.
4. On the first AMOLED light-up, start at low brightness
   (`display_brightness 30`) and check current draw and image before raising
   brightness. Then run `display_test`, inspect all four colors, and exercise
   `display_brightness`, `display_sleep`/`display_wake`, and their `deep`
   variants. Confirm deep wake includes a reset-low pulse longer than 3 ms,
   then record `mark display pass|fail`.
5. Run `touch_test`, `led_test`, and `sdcard_test` with the required parts fitted.
6. Run `speaker_test` at the supplied low level, record the audible result, then run `microphone_test`.
7. Run `camera_test` and keep the ESP Video sensor log.
8. Run `irq_test` once with the shared line idle and again after a known RTC or PMIC event. The command must release GPIO2 after clearing both devices.
9. Run `typec_test` with no cable, then with known sink/source fixtures. Test `otg` only after the CC and boost path measurements are ready.
10. Run `wifi_scan` (PASS requires at least one AP in range) and `ble_smoke`
    to confirm both radio stacks initialize and release cleanly; keep the
    scan output with the board evidence.
11. Run `report` and save the complete console log with the board serial number and rework state.

Visual and audible commands do not mark themselves as passed. A display
transfer, LED update, or audio write can succeed while the external device is
dark or silent.

## Expected I2C addresses

With the matching rails on, the buses should show exactly these devices:

| Bus | Address | Device | Prerequisite |
|---|---|---|---|
| Main (GPIO33/34) | 0x15 | CST820 touch | LCD_CTP_3V3_SW (ALDO2) on |
| Main | 0x20 | ES8389 audio codec | AUDIO_3V3_SW (ALDO3) on |
| Main | 0x21 | FUSB303B Type-C controller | None |
| Main | 0x3C | OV5640 SCCB | Camera rails on |
| Main | 0x0C | DW9714 VCM | Pending module-vendor confirmation |
| Low power (GPIO6/7) | 0x32 | RX8130CE RTC | Always present |
| Low power | 0x34 | TG28_SW PMIC | Always present |

FUSB303B is strapped to 0x21 by the R46 pull-down; a response at 0x31
instead means the address strap does not match the schematic and must be
recorded. A main-bus device that does not answer while its switched rail is
off is expected, not a failure. The Factory `i2c_scan lp`
check passes only when both low-power addresses answer.

During boot, `bsp_pmic_init()` writes to the TG28 (0x34 on the low-power bus)
to clear latched interrupt status before enabling the power-key interrupts.
Low-power bus traffic at 0x34 during boot is therefore expected.

## Rail map and measurement points

TG28_SW rail assignments from schematic revision 0.5. Unless noted, the
acceptance criterion is nominal ±5 %, measured at the net with the rail
loaded to its limit. Unconnected rails have no node to measure.

| Rail | Net | Voltage / limit | Tolerance criterion | Note |
|---|---|---|---|---|
| DCDC1 | VCC_3V3_MAIN | 3.3 V, 2 A | ±5 % (3.14–3.47 V) | Always on |
| DCDC2 | CAM_DVDD_1V5_SW | 1.5 V, 2 A | ±5 % (1.43–1.58 V) | Camera digital core |
| DCDC3 | — | — | n/a (no node) | Unconnected; confirm disabled in OTP readback, no node to measure |
| DCDC4 | — | — | n/a (no node) | Unconnected; confirm disabled in OTP readback, no node to measure |
| CPUSLDO | — | — | n/a (no node) | Unconnected |
| DLDO1 (DC1SW) | WS2812B_PWR_SW | 3.3 V pass-through of DCDC1 | ±5 % of 3.3 V, follows DCDC1 (no regulation of its own) | Default off; no output until the BSP LED init enables it |
| DLDO2 (DC4SW) | — | — | n/a (no node) | Unconnected |
| ALDO1 | LCD_3V3_SW | 3.3 V, 300 mA | ±5 % (3.14–3.47 V) | Display logic |
| ALDO2 | LCD_CTP_3V3_SW | 3.3 V, 300 mA | ±5 % (3.14–3.47 V) | Touch |
| ALDO3 | AUDIO_3V3_SW | 3.3 V, 300 mA | ±5 % (3.14–3.47 V), measured after U21 | Through the U21 (TPS22917) load switch |
| ALDO4 | CAM_AVDD_2V8_SW | 2.8 V, 300 mA | ±5 % (2.66–2.94 V) | Camera analog |
| BLDO1 | CAM_DOVDD_2V8_SW | 2.8 V, 300 mA | ±5 % (2.66–2.94 V) | Camera I/O |
| BLDO2 | 3V3_EXT_SW | 3.3 V, 300 mA | ±5 % (3.14–3.47 V) | EXT connector pin 2 |
| RTCLDO | TG28_VRTC | 3.0 V | ±5 % (2.85–3.15 V); keep within the RX8130CE VBAT input range | Always on; also feeds the RX8130CE VBAT input |

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

Record every executed command as one line in a `command | result | notes`
table so later stages can be re-checked against the serial log:

| Command | Result | Notes |
|---|---|---|
| `board_info` | PASS | MAC aa:bb:cc:dd:ee:ff; reset power_on |
| `i2c_scan main` | WARN | FUSB303B at 0x31: strap mismatch |
| `report` | FAIL | sdcard FAIL (no card), camera NOT_RUN |

For every command, record the exact command line, start/end timestamp, complete
console output, measured values or fixture result, and the final
`PASS`/`FAIL`/`SKIP` decision with rationale. Attach the final `report` JSON
summary, ROM boot log, oscilloscope captures, thermal observations, and any
known limitation. Hardware-dependent items remain **待 EVT1** until this
evidence exists.

## Per-board checklist

Copy this section into the board's report file and tick items off as they are
completed — one checklist per physical board.

- [ ] Board serial / fixture position: __________
- [ ] Base MAC recorded from `board_info`: __________
- [ ] Factory firmware commit and ESP-IDF version recorded: __________
- [ ] Supply current limit / input voltage recorded: __________

### First power-on

- [ ] Current-limited supply set; charger, PMIC, and off-state rails validated before enabling large loads
- [ ] 3.3 V, VRTC, VDD_SPI, PSRAM rail, and `CHIP_PU` measured and recorded
- [ ] Flash access and the 40 MHz clock confirmed
- [ ] RTC, display, audio, camera, TF card, and RGB added one subsystem at a time
- [ ] Voltage, ripple, inrush, steady current, temperature, and off-state voltage recorded for every controlled rail
- [ ] Type-C2 source behavior tested last

### Factory tests (manual order)

- [ ] `board_info`, `power_status`, `flash_test`, `psram_test` with no external load
- [ ] `i2c_scan lp`, `pmic_test`, `pmic power_on_source`, `rtc_test`; battery/VBUS cross-checked with a meter; `pmic charge_current` confirmed at 50 mA; RTC set + power-cycle retention check
- [ ] Each optional rail measured on and off with `peripheral_power`
- [ ] Display: `display_brightness 30` first, then `display_test` with sleep/wake and `deep` variants; `mark display` recorded
- [ ] `touch_test`, `led_test`, `sdcard_test` with the required parts fitted
- [ ] `speaker_test` at the supplied low level (audible result recorded) and `microphone_test`
- [ ] `camera_test` with the ESP Video sensor log kept
- [ ] `irq_test` once with the shared line idle and once after a known RTC/PMIC event; GPIO2 released
- [ ] `typec_test` without cable and with known sink/source fixtures; `otg` only after CC and boost path measurements
- [ ] `wifi_scan` and `ble_smoke`; scan output kept with the board evidence
- [ ] Final `report` and complete console log saved with the board serial number and rework state
- [ ] Report filled in from the template above, with the per-command table and attached evidence
