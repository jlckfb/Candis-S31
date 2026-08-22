# Candis-S31 system overview and board usage

This document describes how the Candis-S31 product, firmware, and power
architecture fit together, and how the board is meant to be used. Pin-level
and net-level facts live in [`hardware/facts.md`](../hardware/facts.md);
first-power procedures live in [`hardware/bring-up.md`](../hardware/bring-up.md).

## Product definition

Candis-S31 is a compact ESP32-S31 development board built around a square
2.0-inch 460 × 460 AMOLED. The configuration is frozen for EVT1:

- SoC: ESP32-S31NRV16 (in-package 8 MB long-octal PSRAM, rated 120 MHz)
- Flash: external W25Q128, 16 MB QSPI, 3.3 V
- Display: AM200Q460460LK (CO5300 QSPI AMOLED) + CST820 capacitive touch
- Power: TG28 PMIC (DCDC1-4, ALDO1-4, BLDO1/2, DLDO1/2 switches, CPUSLDO,
  charger, VRTC)
- RTC: RX8130CE (no 32.768 kHz crystal on the board)
- Audio: ES8389 codec + NS4150B speaker PA, analog microphones
- Camera: DVP OV5640 target; FPC pin 23/24 expose AF power/ground and the
  schematic feeds pin 23 from camera 2.8 V through R95. R95 was removed on
  2026-08-19 and stays DNP while an OV3660-class module is used; refit it only
  for a verified OV5640-AF module, then rerun the display-100% camera stress.
- USB: two Type-C ports — USB1 debug (CH343P UART), USB2 OTG (FUSB303B CC
  controller, ISL9113 5 V boost, 500 mA only)
- Storage: TF card slot (SDMMC, switched power)
- Other: one U24 WS2812B-1313-V6 RGB LED, EXT GH1.25-4 connector
  (3V3_EXT_SW + I2C), three keys (PWRON / BOOT / RESET)

The product ships without a battery and without an enclosure.

## Software architecture

Three layers, plus a declarative Board Manager track:

```text
examples/ (getting-started, low-power, esp-bsp examples)
        │  consume only the board API
bsp/candis_s31 (esp-bsp worktree)  ← board-level: pin map, power rails,
        │                            display/touch/camera/audio init
components/ (chip-level drivers: tg28_sw, rx8130ce, fusb303b, cst820,
             co5300, es8389, ...)  ← know chips, never the board

esp_friends_boards/candis_s31 (esp-board-manager) — declarative board
definition, a separate integration from the BSP
```

- Components are chip drivers. They must not encode Candis-S31 pin choices.
- The BSP owns every board decision: GPIO map, rail control, reset and
  interrupt wiring, device probe order. Firmware builds against the vendored
  snapshot in `vendor/esp-bsp/` by default; set `CANDIS_S31_BSP_PATH` to a
  live esp-bsp checkout for BSP development.
- The Board Manager definition is a parallel, declarative integration. It
  intentionally omits the Type-C controller and OTG GPIO because its current
  model cannot atomically enforce Source-before-boost and the 500 mA-only
  policy; Type-C2 USB Host must go through the BSP API.
- Toolchain: ESP-IDF `v6.1-beta1-dirty` (`.tools/esp-idf` includes the local
  `esp_phy` link patch), target `esp32s31` is a **preview** target — every
  `idf.py` invocation needs `--preview`.

## Firmware model

Two firmware images, different jobs:

- **factory** (`firmware/factory`): production-line diagnostic console.
  61 application commands plus built-in `help` (62 top-level commands total,
  115200 8N1 on the CH343P debug port).
  Results are tracked as PASS / FAIL / WARN / SKIP / NOT_RUN and persisted in
  NVS, so a power cycle does not lose earlier results; `report` prints the
  final JSON summary. Single 4 MB application partition, no OTA. The boot
  safe state shuts down unused PMIC rails (including the OTP-started,
  unconnected DCDC4) before any test runs.
- **recovery** (`firmware/recovery`): ROM download-mode path for a board that
  no longer boots or flashes. `flash_factory.sh` writes a merged factory
  image at offset 0x0 (bootloader + partitions + app in one pass). A merged
  write erases NVS, so previous factory results are lost — expected on a
  recovery flow.

## Power architecture

Rail-by-rail assignments are in [`hardware/facts.md`](../hardware/facts.md)
(TG28 table). Summary: DCDC1 = always-on 3.3 V main rail; DCDC2 camera core;
ALDO1 display logic, ALDO2 touch, ALDO3 audio, ALDO4 camera analog; BLDO1
camera I/O, BLDO2 EXT connector; VRTC always on for the RTC; DCDC3/DCDC4/
CPUSLDO unconnected (DCDC4 still starts at 1.8 V from OTP and is shut down by
the factory safe state). DLDO1 is the DC1SW RGB-LED switch; DLDO2 is DC4SW and
is unconnected.

Board power states:

```text
OFF ──PWRON key / VBUS attach──▶ BOOT ──▶ ACTIVE (S0-RUN)
                                      │
                                      ▼
                              SCREEN_OFF (S1: panel asleep, HP light sleep)
                                      │
                                      ▼
                              DEEP_SLEEP ──▶ SHUTDOWN (S2)
```

- Deep shutdown is the TG28 soft power-off: REG10 bit0
  (`bsp_pmic_power_off()`). With VBUS attached the board may reboot instead
  of powering off — a known item in the bring-up checklist.
- Wake source: RX8130CE alarm, wired-AND with the TG28 interrupt onto GPIO2
  (active-low, open-drain through U2). The interrupt handler must service
  both devices in a loop until the line releases.
- Constraints: no 32.768 kHz crystal — the deep-sleep slow clock is the
  internal RC oscillator, so wake timing drifts and must be measured, not
  assumed; no battery NTC — the TS pin is a fixed 10 kΩ to GND, so charge
  enable decisions wait for vendor written confirmation.

## Board usage flows

Normal development flow:

1. Power from either Type-C port (USB1 preferred for console).
2. Open the CH343P console at 115200 baud.
3. Flash with esptool: merged factory image at 0x0 for production, or
   `idf.py --preview flash` for development builds.
4. Interact through factory commands, or develop against the BSP
   (`bsp_display_start()` and friends) from an example project.

Usage boundaries:

- Camera and JTAG are mutually exclusive: CAM_PCLK/XCLK/VSYNC/HSYNC share
  GPIO54-57 with JTAG. Disable JTAG before powering the camera.
- EXT connector: 3V3_EXT_SW is limited to 300 mA; externally powered devices
  must not backfeed SDA/SCL; the connector is not hot-plug capable.
- Speaker outputs (CN2) are a differential pair — neither side may be
  grounded.
- Do not enable display bias, OTG boost, or other switched rails before the
  EVT1 power checks in `hardware/bring-up.md` are complete.

## Current status (2026-08-20)

- EVT1 (`v0.5_260803_1544`) boards are on the bench. S1-S4 are closed and
  substantial S5-S7 evidence exists; camera, audio, RF functional closure,
  and S8-S10 remain open. Only recorded board observations count as PASS.
- The Factory image builds successfully on 2026-08-20 with ESP-IDF
  `v6.1-beta1-dirty`.
  The new `temp_read`, `rail dump`, `sleep_test`, `wake_info`, hardened
  `rf_stop`, and richer microphone statistics are compile-verified only and
  still require a deliberately authorized flash and real-board regression.
- Korvo-1 pre-validation verdicts carried into this design: standard I2S
  audio path works; internal USB PHY works; the camera can run from the
  SoC-generated XCLK; deep sleep is usable; the LP core is usable. Known
  caveat N1: for the LP-core mailbox, never mix asynchronous then
  synchronous waits on the same channel.
