# EVT1 bring-up

> **Board status (2026-08-19).** EVT1 schematic revision 0.5 went to fabrication on
> 2026-08-03 as `v0.5_260803_1544`; boards are in hand and powered. **S1-S4 are
> closed, S5 is in progress, S6/S7 have produced results, S8-S10 have not started.**
> Per-stage status and evidence paths are in the stage sections below. Items still
> without hardware evidence are marked `NOT_RUN` individually — the document as a
> whole is no longer in a "boards have not arrived" state.
>
> One rework is applied to the current board: **`R95` removed 2026-08-19**
> (see `facts.md`, "Post-fabrication reworks"). Results recorded after that date
> reflect the reworked state.

## How to use this document

This is the stage index for EVT1 bring-up. Each stage has an entry condition, a
quantified exit criterion, the evidence that must be kept, and a failure fallback.
Stages are ordered, with one exception: **S7 is triggered, not scheduled** — any
stage that produces a whole-board power-off, deadlock or silent no-boot enters S7
immediately and does not resume until the root cause is closed.

Three companion documents:

- `facts.md` — as-fabricated netlist facts, DNP positions, reworks, I2C addresses.
- `../AGENT-AI.md` (repo-external working notes) — running incident log and decisions.
- `../firmware/factory/README.md` — Factory command reference.

A compile result is not bring-up evidence. A successful display transfer, LED
update or audio write is not evidence that the external device lit up or made
sound; visual and audible items require an operator verdict.

## Stage map

S1 incoming inspection → S2 power tree without the SoC → S3 programming link →
S4 minimum firmware core domains → S5 BSP peripheral domains → S6 subsystem
performance → **S7 anomaly forensics (triggered)** → S8 hardware accelerators →
S9 low power and power budget → S10 soak, thermal, multi-board and DVT freeze.

| Stage | Scope | Status | Evidence |
|---|---|---|---|
| S1 | Incoming inspection before any power | **closed** | polarity / bridge / dry-joint inspection passed |
| S2 | PMIC rails with the SoC held off | **closed** | VBUS 5.02 V, TG28_VBUS 5.02 V, VSYS ≈ 4.92 V, VTPS switching verified |
| S3 | ROM download, UART, flash | **closed** | CH343P enumerates; TX/RX series resistors verified at 115200 / 460800 / 2 M; ROM version string read |
| S4 | Minimum firmware core domains | **closed 8/8** | `.work/evt1/s4_evt_summary.md` |
| S5 | BSP peripheral domains one at a time | **in progress** | Wi-Fi ✅ BLE ✅ AMOLED ✅ CST820 ✅ USB host ✅ USB device ✅ · camera **partial** (link/color-bar pass, real scene blocked) · audio `NOT_RUN` on current EVT1 |
| S6 | Subsystem performance and physical limits | **display closed** | TE 16.529 ms (60.5 Hz); `display_motion ball` 60.5 fps; `full` 29.7 fps |
| S7 | Anomaly forensics (triggered) | **1 closed, 1 open** | RF/TG28 collapse closed; camera/DCDC1 collapse root-caused to `R95`, 20/20 after removal |
| S8 | Hardware accelerators and compute | `NOT_RUN` | image builds, not yet flashed |
| S9 | Low power and power budget | `NOT_RUN` | — |
| S10 | Soak, thermal, multi-board, DVT freeze | `NOT_RUN` | — |

## Safety gates that apply to every stage

### G1 Physical changes need an explicit decision

Do not change supply, connectors, modules or rework state without the operator
agreeing first. Camera and display modules must have their pinout confirmed
against the connector definition in `facts.md` **before** mating — pins 23/24 of
the 24-pin camera FPC are not consistent across module families and have already
destroyed one board-level test session (S7 case 2).

### G2 DCDC1 85 % undervoltage protection is the board's top failure mode

TG28 `REG23` reads `0x3f` at power-on reset: the 85 % undervoltage shutdown is
enabled for DCDC1-DCDC5 (datasheet §6.5.4.3, POR default). If a transient drags
the DCDC1 output below 3.3 V × 85 % = **2.805 V**, TG28 performs a deliberate
whole-board power-off. Observable signature:

- console dies mid-command, `VCC_3V3_MAIN` goes to 0 V, whole-board current falls
  to roughly 3-5 mA (only RTCLDO remains);
- the short PWRON press does nothing; **a long PWRON press restores the board**;
- after recovery `REG21` = `0x20` (bit 5 = DCDC undervoltage as power-off source)
  and `REG20` = `0x01` (POK long press as power-on source);
- the boot interrupt snapshot is all zeros, because this is a controlled
  power-off and not a latched fault.

This is not a hypothetical: it has fired repeatedly under
"display at 100 % + camera init", and once during flashing. Treat any sudden
current collapse to a few mA as this mechanism until proven otherwise.

### G3 Evidence before recovery after any collapse

**Do not remove the cable first.** TG28 only resets when VBUS is removed, so
unplugging destroys the register evidence that identifies the power-off source.
Order of operations after a collapse:

1. long-press PWRON to bring the board back;
2. immediately run `pmic regs`, `pmic power_off_source`, `pmic power_on_source`,
   `pmic irq_snapshot`;
3. save the current-meter CSV and the console log;
4. only then change anything physical.

`factory_run.py` resets at SoC level (`CHIP_PU`) and does not disturb TG28
registers, so a firmware-level reset is safe to use while preserving evidence.

### G4 Input current limit and charge current are different quantities

Do not conflate them, and do not follow any older "raise to 500 mA then restore
50 mA" instruction:

| Quantity | Value | Where it is set |
|---|---|---|
| Type-C1 **input current limit** | **500 mA, written into the BSP default** (final decision 2026-08-18; no manual raise needed) | `BSP_PMIC_SAFE_INPUT_CURRENT_LIMIT_MA`, applied at boot |
| Same limit, **Factory diagnostic image only** | overridden to **2000 mA** at boot for lab work while the DCDC1 investigation is open | `FACTORY_INPUT_CURRENT_LIMIT_MA` |
| Battery **charge current** | **50 mA** agreed default, unchanged | TG28 `REG62`, verified with `pmic charge_current` |

Record the value `pmic input_limit` actually reports at the start of every
session. If a board has been reflashed with pre-2026-08-17 firmware its default
returns to 100 mA, and RF work must not start until the limit is raised.

### G5 Current meter and RF are mutually exclusive

The IoT Power meter in series adds impedance in front of VBUS. Wi-Fi TX peaks
around 377 mA and BLE TX around 340 mA at the chip supply, which is exactly the
transient that provoked the first collapse family. **Never run RF tests with the
meter in series**: Type-C1 goes directly to the host for RF work, and the meter is
only inserted for the S9 power measurements. Conversely, `rf_stop` must be issued
after every TX/RX command, and every family starts at reduced power with a finite
packet count.

Antenna path as built: `R2` (ceramic branch) fitted, `R1` (IPEX branch) and `L1`
(RF shunt) DNP. Radiated/near-field evaluation is possible; a calibrated conducted
measurement requires an approved `R1`/`R2` rework first.

### G6 Silent no-boot: first response

If the board produces no output at all — not even the ROM boot line — do not
reflash repeatedly. Measure BOOT at the non-ground end of `SW2`:

- BOOT at 0 V with its 10 kΩ pull-up (`R19`) intact means something is actively
  sinking the pin. Unplug Type-C1 for about 20 s for a full cold start; this has
  recovered the board every time so far, which means the latch is a powered state
  and not a hard short.
- The flash image is very unlikely to be the problem — read it back and verify
  before considering a reflash.

The full CH343P line-state analysis and the proposed ECO options are in the
working notes; the important operational rule is cold-start first, evidence
second, reflash last.

### G7 Interrupt and rail ownership

- **GPIO16 (display TE) belongs to `esp_lvgl_port`.** A GPIO has exactly one ISR
  slot, so no diagnostic code may call `gpio_isr_handler_add/remove` on it.
  Measure TE only through `lvgl_port_display_te_observer_set()`; the Factory
  `display_te` command is the reference implementation.
- GPIO2 is a shared, active-low interrupt from TG28 and RX8130CE; see
  "Shared interrupt" below.
- Switched rails have owners. Do not drive a rail directly while a protocol
  owner (panel, touch, codec, camera) holds it.

---

## S1 Incoming inspection

**Goal.** Reject fatal assembly defects before any energy reaches the board.
**Entry.** At least two boards from the lot, plus the fabrication package.
**Exit.** Every item in "Incoming inspection" of the per-board checklist ticked, with
the eleven DNP positions individually confirmed empty and all rail-to-GND resistances
recorded.
**Evidence.** Inspection photos, resistance table, DNP verification table.
**Fallback.** Any short, reversed polarity or suspect DNP position quarantines the
board — do not power it.
**Status: closed.** Polarity, bridging and dry-joint inspection passed.

## S2 Power tree without the SoC

**Goal.** Prove the PMIC produces correct rails while the SoC is held off.
**Entry.** S1 passed; current-limited supply; **no lithium cell fitted and charging
off** (gate 1 in "First power-on order"); TG28 vendor confirmations in writing.
**Exit.** Every rail in "Rail map and measurement points" within its stated tolerance
at its measurement point; the OTP readback matches confirmation sheet V1.3; VTPS
switching verified.
**Evidence.** Meter readings per rail, the power-on-reset register snapshot, and the
DCDC4 oscilloscope capture required by gate 2.
**Fallback.** Any rail out of tolerance stops bring-up here; fix the power tree before
attempting S3.
**Status: closed.** VBUS 5.02 V, TG28_VBUS 5.02 V, VSYS ≈ 4.92 V, VTPS switching verified.

## S3 Programming link

**Goal.** Establish a repeatable download and console path.
**Entry.** S2 passed.
**Exit.** ROM sync succeeds; flashing verified through the UART series resistors at
115200, 460800 and the intended high baud with no framing errors; `VDD_SPI` measures
3.2-3.3 V; the flash image is read back and its SHA-256 recorded.
**Evidence.** Console logs at each baud, the ROM version string, the flash backup and
its checksum.
**Fallback.** Silent no-boot follows gate G6 — measure BOOT, cold start, do not reflash
blindly. If flashing itself fails, use `../firmware/recovery/`.
**Status: closed.** CH343P enumerates; series resistors verified at 115200 / 460800 / 2 M;
ROM version string read. 2 M baud is the working default (about 44 s per flash cycle);
the recovery path deliberately stays at 115200.

## S4 Minimum firmware, core domains

**Goal.** Confirm SoC, PMIC, RTC, card slot and keys with no external load attached.
**Entry.** S3 passed; Factory image flashed.
**Exit.** All eight core items PASS — power, boot, charge path, safe-state,
buttons (including the shared-interrupt edge), RTC (three timekeeping runs),
SD card (three cold starts with a fresh card), PMIC — and the final summary reports
zero failures.
**Evidence.** Per-command console output, the OTP boot snapshot line captured after a
true power-on boot, and the aggregated summary.
**Fallback.** A single I2C device not answering is isolatable: power that domain down,
mark it BLOCKED, continue. Power, strap or reset anomalies are a global stop.
**Status: closed, 8/8 PASS.**

## S5 BSP peripheral domains

**Goal.** Bring up one peripheral domain at a time, each with its own rail evidence.
**Entry.** S4 passed. Measure the domain's rail before and after enabling it.
**Exit.** Each domain independently PASS with an operator verdict where the result is
visual or audible; off-state residual voltage in spec; `power_all_off` returns every
switched rail to off between domains.
**Evidence.** One log per domain plus the rail table; visual/audible verdict recorded
explicitly, never inferred from a successful transfer.
**Fallback.** A failing domain is powered down and marked BLOCKED while independent
domains continue. **A whole-board power-off escalates to S7 immediately.**

**Status: in progress.**

| Domain | Result |
|---|---|
| Wi-Fi | PASS — scan and association; alternating Wi-Fi/BLE stress 10/10 after the input-limit fix |
| BLE | PASS — controller init/enable/disable/deinit |
| AMOLED (CO5300) | PASS — panel lit; colour verification after the RGB565 byte-order change is still an open operator item |
| Touch (CST820) | PASS — ordered four-corner orientation check |
| USB host / Type-C2 | PASS — source/sink, enumeration, 600 s MSC read/write stress |
| USB device (CDC) | PASS — host COM port enumerated, two rounds of command loopback |
| Camera (DVP) | **PARTIAL** — sensor detection, DVP streaming, and built-in color-bar path pass; real-scene image quality remains blocked by the module path |
| Audio (speaker + microphones) | `NOT_RUN` |

**Camera status (updated 2026-09-02).**
The FPC adapter board is installed and the OV5640-compatible module responds at
SCCB address `0x3c` with PID `0x5640`. The headless `camera_test` firmware has
completed repeated 800x600, 960000-byte DVP capture cycles, including stream
teardown and restart; the on-screen color-bar path is stable. The tested clock
and receiver A/B matrix did not change the real-scene "paint stirring" image.
During streaming, the board-side rails read approximately DVDD 1.5 V and
AVDD/DOVDD 2.8 V, so the remaining isolation target is the module/adapter
pixel or analog path (or a confirmed-good module for A/B), not a new display
byte-order workaround. Keep the camera product path marked unverified until a
real-scene image is accepted by an operator.

The Demo camera page additionally contains an S31 hardware-JPEG capture path,
but no JPEG file has yet been accepted on EVT1; do not count that path as a
camera PASS until a captured file is checked on the host.

## S6 Subsystem performance

**Goal.** Move a domain from "works" to "meets a number", and state the physical limit
where one exists.
**Entry.** The domain already PASSed in S5.
**Exit.** Quantified figures with both a measurement and a supporting calculation or
code reference. A ceiling that follows from physics is recorded as the criterion, not
treated as a defect.
**Evidence.** Benchmark logs with frame-rate, TE-period and throughput statistics.
**Fallback.** If a target is unreachable, first establish whether it is a physical
limit; if it is, restate the criterion and stop optimising.
**Status: display closed.** TE period 16.529 ms (60.5 Hz) with a 419 µs high time;
partial-area animation 60.5 fps locked to TE; full-screen 29.7 fps. Full-screen 60 fps
is arithmetically impossible at the panel's 50 MHz QSPI ceiling — one full frame needs
about 17.6 ms against a 16.67 ms frame period — so **30 fps full-screen is the accepted
limit, not a regression to chase**. Because the vertical blanking interval is only
419 µs, TE aligns the start of a write but a full-frame write necessarily crosses the
scan-out window; do not claim tear-free full-screen output.

## S7 Anomaly forensics (triggered)

**Goal.** Explain any whole-board power-off, deadlock or silent no-boot with an
evidence chain, then close it with a verified fix.
**Entry.** Triggered by an anomaly in any other stage. Not scheduled.
**Exit.** A root cause supported by measurements or register state rather than
inference; a reproduction procedure; a solution matrix with what was tried and
rejected; and a regression run of at least 20 consecutive passes of the case that
used to fail.
**Evidence.** Follow gate G3 — register dumps and current logs captured **before**
anything is unplugged — plus the reproduction script.
**Fallback.** Until the root cause is closed, the test that triggered it cannot be
marked PASS and dependent stages stay suspended.

**Case 1 — RF activity powered the board off. Closed 2026-08-17.**
Root cause: the VBUS→VMID path inside the PMIC could not hold up during RF bursts,
so the 3.3 V converter's input sagged and its 85 % undervoltage protection shut the
board down. Storage along the path is asymmetric — 22 µF at VBUS, 10 µF at VMID and
roughly 48 µF at VSYS — leaving VMID the weakest node while the analog/buffer LDO
inputs hang off VSYS. Fix: the boot-time input current limit is written to 500 mA,
which passed 10/10 alternating Wi-Fi/BLE stress; a diagnostic VMID-to-VBUS jumper also
worked but bypasses the protection path and is not a product option. Adding VMID bulk
capacitance was evaluated and **rejected** — the 500 mA default is the final answer.

**Case 2 — camera plus display powered the board off. Root cause closed 2026-08-19.**
The board's 24-pin camera FPC follows the OV5640-AF convention (pin 23 = AF supply,
pin 24 = ground), but the module fitted was an ESP32-S31-Korvo-1 OV3660 whose pin 23
is an LED supply and pin 24 an IR-cut coil. Mating them energised that coil
continuously, overloading the camera analog LDO; together with a 100 % display load and
the camera initialisation transient this pushed the 3.3 V rail through its 85 %
threshold. `REG21` read `0x20` (converter undervoltage) and `REG20` `0x01` (long-press
restart) on every occurrence, and the interrupt snapshot was always zero — a
deliberate power-off, not a latched fault. The experiment matrix ruled out the input
current limit (500 mA and 2000 mA both failed), VMID bulk (10 µF and 32 µF both
failed), the current meter and the cable, and staged pre-powering of all three camera
rails (which survived, proving the rail inrush was not the trigger). Removing `R95`
turned the same 4-times-failing case into 20/20 passes.
Outstanding: measure pin 23 to pin 24 on the removed module to close the last link,
and re-run the display-plus-camera stress if a genuine AF module ever needs `R95` back.

## S8 Hardware accelerators and compute

**Goal.** Exercise the JPEG codec, pixel-processing accelerator, sample-rate converter,
CORDIC unit, bit-scrambler and the DMA paths that feed them.
**Entry.** S5 base domains stable and **no open power-related root cause in S7**.
**Exit.** Every accelerator diagnostic passes with a zero failure mask, each with a
hardware-versus-software timing comparison so the acceleration is demonstrated rather
than assumed.
**Evidence.** Aggregate diagnostic output plus the per-block timing table.
**Fallback.** A single failing block is isolated and the rest continue; anything that
looks like a DMA or PSRAM crash escalates to S7.
**Status: `NOT_RUN`.** The image builds; it has not been flashed. Note that the
certification build follows the upstream example in disabling the interrupt and task
watchdogs — acceptable for a diagnostic image, but it must be revisited before product
firmware, and it removes deadlock detection during long runs.

## S9 Low power and power budget

**Goal.** Anchor a real current figure to every defined power state.
**Entry.** S5 main domains PASS. Do not run S8 concurrently.
**Exit.** Every state in the matrix has a measured value, taken at a stated measurement
point, with the meter's range code recorded; sub-milliamp states verified on the range
that actually resolves them. **"Can it wake" and "how accurately does it wake" are two
separate conclusions** — the board has no 32.768 kHz crystal, so deep-sleep timing
comes from an internal RC oscillator and drift must be measured, not assumed.
**Evidence.** One current log per state plus the matching console log, with the
measurement point and range code written down.
**Fallback.** If a state misses its target, work through the interference list before
changing the criterion. **Never run RF with the meter in series (gate G5).**
**Status: `NOT_RUN`.**

Measurement points differ and are not interchangeable: the VBUS side includes the
charger current and the PMIC's own quiescent draw, and host-side USB enumeration alone
is a milliamp-scale load that swamps a microamp measurement — so microamp states must
be measured on the battery side with Type-C1 unplugged. The Factory image currently has
no sleep support at all, so this stage needs either the low-power example image or new
sleep commands.

## S10 Soak, thermal, multi-board and DVT freeze

**Goal.** Show the design is reproducible across boards and stable over time, then
freeze the change list for DVT.
**Entry.** The main items of S4-S9 closed.
**Exit.** At least three boards through the same flow with consistent results; a
sustained combined-load soak with no undervoltage shutdown, no reboot and stable
temperatures; a filled report per board; and every DVT change either accepted with
evidence or explicitly deferred.
**Evidence.** One report per board with serial number and rework state, thermal
observations, and the final change list.
**Fallback.** A soak failure returns to the owning stage — a long-run test must never
be used to paper over an unresolved single-domain failure.
**Status: `NOT_RUN`.**

---

# Reference material

The sections below are the detailed procedures and tables the stages above refer
to. They are reference data, not a separate running order.

## Checks before fabrication

| Item | Question to close | Safe fallback |
|---|---|---|
| PMIC interrupt and reset | Confirm TG28_SW pin 38 behavior and the `CHIP_PU/PWROK` reset chain | Rework the interrupt or reset network |
| Display supply | Confirm the LCD connector pinout and panel supply requirements | Do not populate or power the display |
| U14 pin 13 / LCD_VDD | **Resolved**: AM200 Rev V1.0 §8 marks pin 13 `LCD_VDD 3.3V` as NC. The real supply pins are pin 14 `LCD_IOVCC`, pin 15 `VBAT` (3.3–5.5 V), and pin 1 `VCI_EN`. On fabrication baseline `v0.5_260803_1544`, U14 pin 13 shares `LCD_3V3_SW` with pin 14, so the NC pin is a harmless no-load branch; U18 is the TPS22917 load switch, not the panel connector. ALDO1 must still feed pin 14. First light-up order: ALDO1 → measure U14 pin 14 → VBAT/U14 pin 15 → only then raise `VCI_EN`/U14 pin 1 | Leave the panel unpopulated and keep both display enables off |
| Battery path | Confirm BATFET, eFuse, and VBUS wake behavior | Revise the PMIC control and protection |
| TG28 OTP | Read back or measure VRTC=3.0 V, REG62 default=50 mA, and the agreed BATFET/VBUS/BAT power-on bits on incoming parts; confirm over the LP I2C bus (0x34) that DCDC1 and DCDC4 are the OTP step1 rails (DCDC1 3.3 V, DCDC4 1.8 V per confirmation sheet V1.3) and that DCDC2/DCDC3, ALDO1-4, BLDO1/2, CPUSLDO, and DLDO2 (DC4SW) read back disabled. DCDC4 has no external load and no measurable node, so its OTP state is proven only by the pre-safe-state register snapshot in the Factory boot log; the Factory safe state then disables DCDC4 at runtime, which is the intended safety action | Keep charging and optional rails disabled; quarantine or reprogram the lot |
| GPIO36 strap | **已核实为正确配置**：GPIO36 是 VDD_SPI 电压绑带（数据手册 Table 3-4：高=3.3V Flash，低=1.8V Flash）。本板 Flash 为外置 W25Q128（3.3V 器件，2.7–3.6V），故 VDD_SPI=3.3V；ESP32-S31 v0.0 勘误 SPI-855 亦禁止 1.8V VDD_SPI 启动。R6（10kΩ 上拉至 VCC_3V3_MAIN）是乐鑫硬件设计指南要求的绑带上拉，必需且正确。GPIO36 兼作 TF 低有效电源使能，固件仅在启动后显式 `peripheral_power sdcard on` 时拉低，复位采样窗口不受扰动。回板仅须实测 VDD_SPI=3.3V（SoC pin39 / W25Q128 VCC pin8）即可放行 | 无需拆 R6；若实测 VDD_SPI 异常（如设计误接 1.8V）则停止上电，先修电源树 |
| UART0 series resistors | Repeat ROM sync and verified flashing through the series resistors — TX chain R17+R37 (499 ohm each, 998 ohm total), RX chain R36 (499 ohm) — at 115200, 460800, and the intended high baud without framing errors | Use the highest repeatable lower baud or rework the series resistors |
| Main-bus I2C addresses | In boot safe state FUSB303B must be silent; after `typec_test` it must answer at the assembled EVT1 strap address 0x21 with valid identity. Treat any 0x31 response or dual response as an assembly/design failure. With their own rails on, ES8389 must answer at 0x10 (7-bit on the wire; 0x20 is its 8-bit write address), CST820 at 0x15, and OV5640 at 0x3C | Do not initialize a conflicting device; quarantine the board and rework its address strap |
| Camera / JTAG mux | Confirm GPIO54-57 are released from JTAG before DVP use and that camera capture is stable; document the alternative debug route | Disable camera while JTAG is active, or disable JTAG before powering the camera |
| Type-C2 source mode | Test DRP, short circuit, dual-plug, backfeed, and temperature behavior | Keep source mode disabled or DNP |
| Reset key | Confirm the TG28_SW `TG28_PWROK` output type, sink current, and timing | Isolate or rework the key input |
| Deep-sleep counter retention | Run the low-power example through S0→S1→deep-sleep→wake and confirm the `RTC_NOINIT_ATTR` cycle counter survives the wake and escalates to shutdown at the configured count | If the counter is lost across deep sleep the machine loops in deep sleep instead of escalating; the RTC-timer wake still recovers the board, so treat shutdown escalation as best-effort until this passes |
| Deep-sleep wake accuracy and current | Record "can wake" and "wake accuracy / current in spec" as two separate acceptance results. There is no 32.768 kHz crystal, so the slow clock is the internal RC oscillator; measure and record its drift and the wake-time error against the RTC alarm target | Accept wider alarm margins as best-effort timing; do not claim sleep-current acceptance until measured |
| Soft power-off with VBUS | Trigger `bsp_pmic_power_off` (TG28 REG10 bit0) with VBUS attached and confirm whether the board powers off, reboots, or needs a guard — the Linux reference reboots instead of powering off in this case | If it reboots or misbehaves with VBUS present, add a VBUS-present guard (deliberate reboot, or only allow power-off when VBUS is absent) |

## First power-on order

Two Go/No-Go gates apply before the numbered sequence:

1. **First power-up runs without a lithium cell and with charging kept off.** The NTC behavior (TS pin = R29 10 kΩ fixed to GND) is not yet confirmed in writing by the vendor; until that confirmation exists, do not connect a real cell and do not run `charge_test`. After confirmation, use only cells with a protection board.
2. **DCDC4 starts at 1.8 V from OTP with LX4/FB4 floating.** The Factory safe-state runtime shutdown is the earliest safety action, but during ROM download mode, bootloader failure, or crash windows DCDC4 remains in its OTP state. First boards must confirm this rail's behavior with an oscilloscope and be accepted against the vendor's written conclusion. The TG28 written confirmations (DCDC4 floating connection, PWROK output structure and sink current, VBUS wake, NTC judgment) are power-on Go/No-Go items.

1. Use a current-limited supply. Validate the charger, PMIC, and off-state rails before fitting or enabling large loads.
2. Measure 3.3 V, VRTC, VDD_SPI, the PSRAM rail, and `CHIP_PU`.
3. Check flash access and the 40 MHz clock.
4. Add RTC, display, audio, camera, TF card, and RGB one subsystem at a time.
5. Record voltage, ripple, inrush current, steady current, temperature, and off-state voltage for every controlled rail.
6. Test Type-C2 source behavior last.

## Suggested Factory order

Use the serial commands one at a time. Faults are graded, not all fatal:
**global stop** for overcurrent, wrong voltage, abnormal heating, VBUS
shoot-through or backfeed, abnormal PMIC state, unreliable strap or reset
behavior, unexpected DCDC4 switching, or any battery/charge-path anomaly —
halt the whole session and investigate. **Isolatable** for a single I2C
device not acknowledging, a peripheral ID mismatch, display initialization
failure with rails in spec, TF card compatibility issues, or a single
software timeout — power that domain off, mark the item BLOCKED, log it,
and continue with independent modules. If the console does not come
up, or flashing fails, stop and follow
[`firmware/recovery/`](../firmware/recovery/README.md) for download-mode
recovery instead of re-running stages blindly.

1. Run `board_info`, `power_status`, `flash_test`, and `psram_test` without any external load attached. Copy the `OTP boot snapshot` line from the boot log into the board record — it is lot evidence only when the log shows it after a power-on boot, because a warm reset leaves the TG28 exactly as the previous run left it (the firmware then downgrades the line to a warning). `otp_status` re-reads the live rails at any time but cannot prove the OTP after the boot safe state ran.
2. Run `i2c_scan lp`, `pmic_test`, `pmic power_on_source`, and `rtc_test`;
   compare battery and VBUS readings with a meter. Confirm
   `pmic charge_current` reports the agreed 50 mA default and leave it there —
   see gate G4: the 500 mA figure is the **Type-C1 input current limit**, which
   the firmware already applies at boot, and it must not be written to the
   charge-current register. Record what `pmic input_limit` reports instead of
   changing it. EVT1 has no battery NTC (the TS pin is a
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
9. Run `wifi_scan` (PASS requires at least one AP in range) and `ble_smoke`
   to confirm both radio stacks initialize and release cleanly. Capture the
   3V3_MAIN and VDD_SPI droop waveforms during scan, association, and
   transmit, and during BLE advertising; keep the scan output and waveforms
   with the board evidence.
10. Run one main-bus `i2c_scan` in the boot safe state and confirm FUSB303B and every switched-rail device are silent. Then run `typec_test` with no cable and with known sink/source fixtures, followed immediately by another main-bus scan; FUSB303B must answer at 0x21 while its control domain is on (the host runner records this as `type_c_power_scan`). Test `otg` only after the CC and boost path measurements are ready, and remember `otg on` proves only the 5 V boost path. For host data-path evidence run `usb_host_test` with a device attached (500 mA budget; this hardware never advertises 1.5 A/3 A); for device-mode enumeration use the dedicated TinyUSB diagnostic image. Between peripheral blocks, `power_all_off` returns every switched rail to off; verify off-state residual voltages with a meter.
11. Concurrency and thermal: run display + camera + PSRAM together, then TF
    write + Wi-Fi together, and record the full-load temperature rise.
12. Run `report` and save the complete console log with the board serial number and rework state.

Visual and audible commands do not mark themselves as passed. A display
transfer, LED update, or audio write can succeed while the external device is
dark or silent.

## Expected I2C addresses

With the matching rails on, the buses should show exactly these devices:

| Bus | Address | Device | Prerequisite |
|---|---|---|---|
| Main (GPIO33/34) | 0x15 | CST820 touch | LCD_CTP_3V3_SW (ALDO2) on |
| Main | 0x10 | ES8389 audio codec (7-bit on the wire; 0x20 = 8-bit write address used by esp_codec_dev) | AUDIO_3V3_SW (ALDO3) on |
| Main | 0x21 | FUSB303B Type-C controller | `bsp_type_c_init()` / `typec_test` has driven active-low `TYPEC_EN_N` low |
| Main | 0x3C | OV5640 SCCB | Camera rails on |
| Low power (GPIO6/7) | 0x32 | RX8130CE RTC | Always present |
| Low power | 0x34 | TG28_SW PMIC | Always present |

FUSB303B is strapped to 0x21 by the R46 pull-down (909kΩ, the datasheet's
recommended value to cut standby current); a response at 0x31
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

EXT connector limits: 3V3_EXT_SW (BLDO2) supplies at most 300 mA; externally
attached devices must not backfeed SDA/SCL from their own supply, and the
connector is not hot-plug capable (Q6 has no ESD protection).

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
known limitation. An item without this evidence stays `NOT_RUN`; see the stage
map for what is already closed.

## Per-board checklist

Copy this section into the board's report file and tick items off as they are
completed — one checklist per physical board.

- [ ] Board serial / fixture position: __________
- [ ] Base MAC recorded from `board_info`: __________
- [ ] Factory firmware commit and ESP-IDF version recorded: __________
- [ ] Supply current limit / input voltage recorded: __________

### Incoming inspection (at least two boards)

- [ ] Board revision, silkscreen, and BOM substitutions confirmed against the fabrication package
- [ ] Rework state recorded against `facts.md` "Post-fabrication reworks" — on the current
      board `R95` is **removed** (camera FPC pin 23 left open). A board that still has `R95`
      fitted must not be mated with an OV3660-class module: see S7 case 2
- [ ] All ten PCB-present DNP positions confirmed genuinely empty (table in [`facts.md`](facts.md), "DNP positions as fabricated"): `R9` `R22` `R24` `R1` `L1` (esp32-s31/RF), `C74` (display), `R62` `R65` (audio), `R41` `R42` (usb-c2-otg). All are 0201 except `C74` 0603
- [ ] `R9` verified empty **visually** (0201 pad on the esp32-s31 side) — this is the one DNP that blocks boot. Do not try to prove it with an ohmmeter: with power off, GPIO36-to-GND reads ~10 kΩ through R9 if fitted, but also reads R6 10 kΩ in series with an unknown powered-down rail impedance if it is not, so the two cases are not separable. The decisive test is the powered VDD_SPI/GPIO36 voltage check in the first power-on section below
- [ ] `R24` and `R22` verified empty visually, and understood as correct-by-design rather than defects: VDD_SPI is an internally supplied output. Do not "repair" them. An ohmmeter cannot confirm `R24` either — the SoC's internal VDD3P3-to-VDD_SPI path (RSPI ≈ 3Ω, datasheet Table 5-3) bridges the same two nodes
- [ ] `R41`/`R42` verified empty. Here a meter does discriminate: with both unfitted, U17.3 to GND and to VCC_3V3_MAIN is only the 3-state pin's own leakage, so expect a high reading (MΩ or open), never ~10 kΩ. A 10 kΩ reading means one of the two got fitted and the Type-C2 role strap is no longer float. The datasheet's `Zfloat` row is the *detection threshold* — impedance to VDD or GND of roughly 1-4 MΩ is still read as FLOAT — which is exactly why neither divider leg may be present
- [ ] QFN soldering (U4, U6) inspected for bridges, voids, and orientation
- [ ] Rail-to-GND resistances and rail-to-rail short checks recorded
- [ ] Battery connector (CN1) polarity confirmed
- [ ] D1 (BAS70WS) diode-checked by pin number: forward drop with red probe on pin 2 (anode, `CHIP_PU` side via R28) and black on pin 1 (cathode, `KEY_RST_N`); reverse reads open. An open reading with red on `KEY_RST_N` is expected, not a defect
- [ ] U14 BTB and FPC1 contact-face orientation confirmed before mating
- [ ] No unintended continuity between CC1/CC2 and VBUS on either Type-C connector

### First power-on

- [ ] Gate 1: first power-up without a lithium cell, charging kept off; TG28 vendor written confirmations received (DCDC4 floating, NTC, PWROK, VBUS wake)
- [ ] Gate 2: DCDC4 rail behavior scoped across ROM download / bootloader failure / crash windows; accepted against vendor written conclusion
- [ ] Current-limited supply set; charger, PMIC, and off-state rails validated before enabling large loads
- [ ] 3.3 V, VRTC, PSRAM rail, and `CHIP_PU` measured and recorded
- [ ] VDD_SPI measured at U4 pad 39 and U5 pin 8 (same net) after reset release: expect **~3.2-3.3 V**, supplied internally through RSPI ≈ 3Ω. A 1.8 V reading means GPIO36 sampled low (check `R9`/`R6`) and boot will fail; 0 V with the rest of the board powered means the internal switch never opened — stop and fix the power tree before trusting flash
- [ ] Flash access and the 40 MHz clock confirmed
- [ ] RTC, display, audio, camera, TF card, and RGB added one subsystem at a time
- [ ] Voltage, ripple, inrush, steady current, temperature, and off-state voltage recorded for every controlled rail
- [ ] Type-C2 source behavior tested last
- [ ] Boot-mode straps checked using both official scopes: datasheet v0.5 exposes GPIO36/37/60/61, while ESP-IDF GPIO docs/io_mux additionally label GPIO38/39/40 as secondary Boot Mode select 0/1/2. Do not infer a normal warm-reset risk from GPIO38/39/40: they are don't-care for documented SPI Boot (GPIO61=1) and Joint Download (GPIO61=0, GPIO60=1). R18/R19 pull GPIO60/61 high for default SPI Boot. Scope GPIO61 through CHIP_PU release (hold at least 3 ms), confirm Joint Download with GPIO61 low/GPIO60 high, and record raw `boot:0xNN`/`GPIO_STRAP_REG` if an undocumented combination is suspected

### Factory tests (manual order)

- [ ] `board_info`, `power_status`, `flash_test`, `psram_test` with no external load
- [ ] `i2c_scan lp`, `pmic_test`, `pmic power_on_source`, `charge_test source_verified`, and `rtc_test`; verify the source/cable and VBUS before the load step, record the firmware-default `input_limit=500mA` (no manual raise/restore), cross-check battery/VBUS with a meter, confirm the charger current remains the agreed 50 mA, and check RTC set + power-cycle retention
- [ ] `rtc_alarm` and `buttons` exercised; shared GPIO2 released after each event
- [ ] `i2c_scan main` in the boot safe state records FUSB303B and switched devices silent; then `typec_test` without cable and with known sink/source fixtures, followed by `type_c_power_scan`/`i2c_scan main` proving only 0x21 while the Type-C control domain is on
- [ ] Each optional rail measured on and off with `peripheral_power`; enable EXT pin 2 only as `peripheral_power external_3v3 on output_only`, with every self-powered load disconnected
- [ ] Display: `display_brightness 30` first, then `display_test`, `display_sleep_test`, `display_sleep`/`display_wake` with `deep` variants; `mark display` recorded
- [ ] `touch_test`, `led_test`, `sdcard_test` with the required parts fitted
- [ ] `speaker_test` at the supplied low level (audible result recorded) and `microphone_test`
- [ ] `camera_test` with the ESP Video sensor log kept
- [ ] `irq_test` once with the shared line idle and once after a known RTC/PMIC event; GPIO2 released
- [ ] `wifi_scan` and `ble_smoke`; 3V3_MAIN/VDD_SPI droop waveforms captured during scan/associate/transmit and BLE advertising; scan output kept with the board evidence
- [ ] `otg` and `usb_host_test` only after CC and boost-path measurements; VBUS confirmed off after teardown
- [ ] `power_all_off` run at each stage boundary; closing `i2c_scan main` records FUSB303B silent with its control domain off
- [ ] Concurrency and thermal: display + camera + PSRAM together, then TF write + Wi-Fi together; full-load temperature rise recorded
- [ ] Deep-sleep acceptance recorded separately: wake capability vs wake accuracy / sleep current (internal RC slow clock, no 32.768 kHz crystal)
- [ ] Final `report` and complete console log saved with the board serial number and rework state
- [ ] Report filled in from the template above, with the per-command table and attached evidence
