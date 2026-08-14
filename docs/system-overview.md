# Candis-S31 system overview and board usage

This document describes how the Candis-S31 product, firmware, and power
architecture fit together, and how the board is meant to be used. Pin-level
and net-level facts live in [`hardware/facts.md`](../hardware/facts.md);
first-power procedures live in [`hardware/bring-up.md`](../hardware/bring-up.md).

## Product definition

Candis-S31 is a compact ESP32-S31 development board built around a square
2.0-inch 460 × 460 AMOLED. The configuration is frozen for EVT1:

- SoC: ESP32-S31NRV16 (in-package 16 MB Octal PSRAM)
- Flash: external W25Q128, 16 MB QSPI, 3.3 V
- Display: AM200Q460460LK (CO5300 QSPI AMOLED) + CST820 capacitive touch
- Power: TG28 PMIC (DCDC1/2, ALDO1-4, BLDO1/2, charger, VRTC)
- RTC: RX8130CE (no 32.768 kHz crystal on the board)
- Audio: ES8389 codec + NS4150B speaker PA, analog microphones
- Camera: OV5640 DVP, fixed focus (no VCM control hardware)
- USB: two Type-C ports — USB1 debug (CH343P UART), USB2 OTG (FUSB303B CC
  controller, ISL9113 5 V boost, 500 mA only)
- Storage: TF card slot (SDMMC, switched power)
- Other: one WS2812E RGB LED, EXT GH1.25-4 connector (3V3_EXT_SW + I2C),
  three keys (PWRON / BOOT / RESET)

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
  interrupt wiring, device probe order. Factory firmware and examples load it
  through `CANDIS_S31_BSP_PATH` / `ESP_BSP_ROOT`; it is not mirrored into this
  repository.
- The Board Manager definition is a parallel, declarative integration. It
  intentionally omits the Type-C controller and OTG GPIO because its current
  model cannot atomically enforce Source-before-boost and the 500 mA-only
  policy; Type-C2 USB Host must go through the BSP API.
- Toolchain: ESP-IDF `v6.1-beta1`, target `esp32s31` is a **preview** target —
  every `idf.py` invocation needs `--preview`.

## Firmware model

Two firmware images, different jobs:

- **factory** (`firmware/factory`): production-line diagnostic console.
  39 top-level serial commands (115200 8N1 on the CH343P debug port).
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
the factory safe state).

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

## Current status (2026-08-14)

- EVT1 (`v0.5_260803_1544`) fabricated 2026-08-03; boards have not arrived.
  Every hardware-dependent result remains NOT_RUN.
- Build regression 2026-08-11: 15/15 targets compile with ESP-IDF
  v6.1-beta1 (preview) — compile-only evidence, not bring-up evidence.
- Korvo-1 pre-validation verdicts carried into this design: standard I2S
  audio path works; internal USB PHY works; the camera can run from the
  SoC-generated XCLK; deep sleep is usable; the LP core is usable. Known
  caveat N1: for the LP-core mailbox, never mix asynchronous then
  synchronous waits on the same channel.
