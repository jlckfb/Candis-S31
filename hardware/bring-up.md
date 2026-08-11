# EVT1 bring-up notes

EVT1 schematic revision 0.5 went to fabrication on 2026-08-03 as `v0.5_260803_1544`; the boards have not arrived yet. Hardware-dependent results remain `NOT_RUN`.

## Checks before fabrication

| Item | Question to close | Safe fallback |
|---|---|---|
| PMIC interrupt and reset | Confirm TG28_SW pin 38 behavior and the `CHIP_PU/PWROK` reset chain | Rework the interrupt or reset network |
| Display supply | Confirm the LCD connector pinout and panel supply requirements | Do not populate or power the display |
| U14 pin 13 / LCD_VDD | **Resolved**: AM200 Rev V1.0 §8 marks pin 13 `LCD_VDD 3.3V` as NC. The real supply pins are pin 14 `LCD_IOVCC`, pin 15 `VBAT` (3.3–5.5 V), and pin 1 `VCI_EN`. On fabrication baseline `v0.5_260803_1544`, U14 pin 13 shares `LCD_3V3_SW` with pin 14, so the NC pin is a harmless no-load branch; U18 is the TPS22917 load switch, not the panel connector. ALDO1 must still feed pin 14. First light-up order: ALDO1 → measure U14 pin 14 → VBAT/U14 pin 15 → only then raise `VCI_EN`/U14 pin 1 | Leave the panel unpopulated and keep both display enables off |
| Battery path | Confirm BATFET, eFuse, and VBUS wake behavior | Revise the PMIC control and protection |
| TG28 OTP | Read back or measure VRTC=3.0 V, REG62 default=50 mA, and the agreed BATFET/VBUS/BAT power-on bits on incoming parts; confirm over the LP I2C bus (0x34) that DCDC1 and DCDC4 are the OTP step1 rails (DCDC1 3.3 V, DCDC4 1.8 V per confirmation sheet V1.3) and that DCDC2/DCDC3, ALDO1-4, BLDO1/2, CPUSLDO, and DLDO2 (DC4SW) read back disabled. DCDC4 has no external load and no measurable node, so its OTP state is proven only by the pre-safe-state register snapshot in the Factory boot log; the Factory safe state then disables DCDC4 at runtime, which is the intended safety action | Keep charging and optional rails disabled; quarantine or reprogram the lot |
| GPIO36 strap | **已确认原理图冲突，打板前必须改**：ESP32-S31 数据手册将 GPIO36 定义为 VDD_SPI 电压绑带，板上 VDD_SPI=1.8 V 时要求采样为低；当前 `Schematic_3` 的 R6（4.7 kΩ、`Add into BOM=yes`）却把 `TF_PWR_EN_N/GPIO36` 上拉到 3.3 V，且固件需要该网低有效才能给 TF 卡上电 | 不得靠固件或示波器放行；移除/改接 R6，并用不影响绑带采样的门控实现 TF 默认断电，重新做 ERC/绑带审查后再投板 |
| UART0 series resistors | Repeat ROM sync and verified flashing through the series resistors — TX chain R17+R37 (499 ohm each, 998 ohm total), RX chain R36 (499 ohm) — at 115200, 460800, and the intended high baud without framing errors | Use the highest repeatable lower baud or rework the series resistors |
| Main-bus I2C addresses | In boot safe state FUSB303B must be silent; after `typec_test` it must answer at the assembled EVT1 strap address 0x21 with valid identity. Treat any 0x31 response or dual response as an assembly/design failure. With their own rails on, ES8389 must answer at 0x20, CST820 at 0x15, and OV5640 at 0x3C | Do not initialize a conflicting device; quarantine the board and rework its address strap |
| Camera / JTAG mux | Confirm GPIO54-57 are released from JTAG before DVP use and that camera capture is stable; document the alternative debug route | Disable camera while JTAG is active, or disable JTAG before powering the camera |
| Type-C2 source mode | Test DRP, short circuit, dual-plug, backfeed, and temperature behavior | Keep source mode disabled or DNP |
| Reset key | Confirm the TG28_SW `TG28_PWROK` output type, sink current, and timing | Isolate or rework the key input |
| Deep-sleep counter retention | Run the low-power example through S0→S1→deep-sleep→wake and confirm the `RTC_NOINIT_ATTR` cycle counter survives the wake and escalates to shutdown at the configured count | If the counter is lost across deep sleep the machine loops in deep sleep instead of escalating; the RTC-timer wake still recovers the board, so treat shutdown escalation as best-effort until this passes |
| Soft power-off with VBUS | Trigger `bsp_pmic_power_off` (TG28 REG10 bit0) with VBUS attached and confirm whether the board powers off, reboots, or needs a guard — the Linux reference reboots instead of powering off in this case | If it reboots or misbehaves with VBUS present, add a VBUS-present guard (deliberate reboot, or only allow power-off when VBUS is absent) |

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

1. Run `board_info`, `power_status`, `flash_test`, and `psram_test` without any external load attached. Copy the `OTP boot snapshot` line from the boot log into the board record — it is lot evidence only when the log shows it after a power-on boot, because a warm reset leaves the TG28 exactly as the previous run left it (the firmware then downgrades the line to a warning). `otp_status` re-reads the live rails at any time but cannot prove the OTP after the boot safe state ran.
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
9. Run one main-bus `i2c_scan` in the boot safe state and confirm FUSB303B and every switched-rail device are silent. Then run `typec_test` with no cable and with known sink/source fixtures, followed immediately by another main-bus scan; FUSB303B must answer at 0x21 while its control domain is on (the host runner records this as `type_c_power_scan`). Test `otg` only after the CC and boost path measurements are ready, and remember `otg on` proves only the 5 V boost path. For host data-path evidence run `usb_host_test` with a device attached (500 mA budget; this hardware never advertises 1.5 A/3 A); for device-mode enumeration use the dedicated TinyUSB diagnostic image. Between peripheral blocks, `power_all_off` returns every switched rail to off; verify off-state residual voltages with a meter.
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
| Main | 0x21 | FUSB303B Type-C controller | `bsp_type_c_init()` / `typec_test` has driven active-low `TYPEC_EN_N` low |
| Main | 0x3C | OV5640 SCCB | Camera rails on |
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
| DCDC4 | — | — | n/a (no node) | OTP step1 rail: enabled at 1.8 V per confirmation sheet V1.3, but unconnected with no node to measure; prove via the Factory boot OTP snapshot. The Factory safe state disables it at runtime (intended) |
| CPUSLDO | — | — | n/a (no node) | Unconnected |
| DLDO1 (DC1SW) | WS2812B_PWR_SW | 3.3 V pass-through of DCDC1 | ±5 % of 3.3 V, follows DCDC1 (no regulation of its own) | Default off; no output until the BSP LED init enables it |
| DLDO2 (DC4SW) | — | — | n/a (no node) | Unconnected |
| ALDO1 | LCD_3V3_SW | 3.3 V, 300 mA | ±5 % (3.14–3.47 V) | Display logic |
| ALDO2 | LCD_CTP_3V3_SW | 3.3 V, 300 mA | ±5 % (3.14–3.47 V) | Touch |
| ALDO3 | AUDIO_3V3_SW | 3.3 V, 300 mA | ±5 % (3.14–3.47 V), measured on the switched rail | Direct TG28 ALDO3 supply for ES8389 and both microphones; also drives U20 TPS22917 ON |
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
- [ ] `i2c_scan lp`, `pmic_test`, `pmic power_on_source`, plain `charge_test` at the 100 mA safe input baseline, and `rtc_test`; battery/VBUS cross-checked with a meter; `pmic charge_current` confirmed at 50 mA; RTC set + power-cycle retention check. For the instrumented high-current step only: verify source/cable and VBUS first, run `pmic input_limit 500 source_verified`, then `charge_test source_verified`, and restore `pmic input_limit 100`
- [ ] `rtc_alarm` and `buttons` exercised; shared GPIO2 released after each event
- [ ] `i2c_scan main` in the boot safe state records FUSB303B and switched devices silent; then `typec_test` without cable and with known sink/source fixtures, followed by `type_c_power_scan`/`i2c_scan main` proving only 0x21 while the Type-C control domain is on
- [ ] Each optional rail measured on and off with `peripheral_power`; enable EXT pin 2 only as `peripheral_power external_3v3 on output_only`, with every self-powered load disconnected
- [ ] Display: `display_brightness 30` first, then `display_test`, `display_sleep_test`, `display_sleep`/`display_wake` with `deep` variants; `mark display` recorded
- [ ] `touch_test`, `led_test`, `sdcard_test` with the required parts fitted
- [ ] `speaker_test` at the supplied low level (audible result recorded) and `microphone_test`
- [ ] `camera_test` with the ESP Video sensor log kept
- [ ] `irq_test` once with the shared line idle and once after a known RTC/PMIC event; GPIO2 released
- [ ] `otg` and `usb_host_test` only after CC and boost-path measurements; VBUS confirmed off after teardown
- [ ] `power_all_off` run at each stage boundary; closing `i2c_scan main` records FUSB303B silent with its control domain off
- [ ] `wifi_scan` and `ble_smoke`; scan output kept with the board evidence
- [ ] Final `report` and complete console log saved with the board serial number and rework state
- [ ] Report filled in from the template above, with the per-command table and attached evidence
