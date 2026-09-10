# Candis-S31 system overview and board usage

This document describes how the Candis-S31 product, firmware, and power
architecture fit together, and how the board is meant to be used. Pin-level
and net-level facts live in [`hardware/facts.md`](../hardware/facts.md);
first-power procedures live in [`hardware/bring-up.md`](../hardware/bring-up.md).

## Product definition

Candis-S31 is a compact ESP32-S31 development board built around a square
2.0-inch 460 × 460 AMOLED. The configuration is frozen for EVT1:

- SoC: ESP32-S31 with the validated 32 MB in-package Octal PSRAM
- Flash: external W25Q128, 16 MB QSPI, 3.3 V
- Display: AM200Q460460LK (CO5300 QSPI AMOLED) + CST820 capacitive touch
- Power: TG28 PMIC (DCDC1-4, ALDO1-4, BLDO1/2, DLDO1/2 switches, CPUSLDO,
  charger, VRTC)
- RTC: RX8130CE (no 32.768 kHz crystal on the board)
- Audio: ES8389 codec + NS4150B speaker PA, analog microphones
- Camera: DVP OV5640 target; FPC pin 23/24 expose AF power/ground and the
  schematic feeds pin 23 from camera 2.8 V through R95. Pin 23/24 assignment
  differs between module families, so check the current R95 fit state in
  [`hardware/facts.md`](../hardware/facts.md) before mating a module.
- USB: two Type-C ports — USB1 debug (CH343P UART), USB2 OTG (FUSB303B CC
  controller, ISL9113 5 V boost, 500 mA only)
- Storage: TF card slot (SDMMC, switched power)
- Other: one U24 WS2812B-1313-V6 RGB LED, EXT GH1.25-4 connector
  (3V3_EXT_SW + I2C), three keys (PWRON / BOOT / RESET)

The product ships without a battery and without an enclosure.

## Software architecture

The repository has three runtime layers plus a declarative Board Manager track:

```text
examples/ and firmware/  ── consume the board runtime or Board Manager APIs
          │
components/candis_s31     ── repository-owned board runtime: pins, rails,
          │                   display/touch/camera/audio/USB policy
components/esp_lvgl_port  ── board-local LVGL compatibility and TE diagnostics
          │
vendor/idf-extra-components ── reusable chip drivers (TG28/RX8130CE/
                                FUSB303B/CST820), never board policy

vendor/esp-board-manager/esp_friends_boards/candis_s31
                         ── declarative wiring and generated Board Manager demo
```

- Reusable drivers know only their chip protocol and public interfaces.
- `components/candis_s31/` owns every board decision: GPIO map, rail control,
  reset and interrupt wiring, device probe order, Type-C policy, camera clock
  configuration, and display transitions.
- `vendor/esp-board-manager/` is the staged declarative board definition.
  Board Manager does not replace the runtime component because its generic
  model cannot encode this board's complete charging, shared-interrupt,
  Type-C, camera-clock, or LVGL/TE policy.
- Toolchain: ESP-IDF `v6.1-rc1`; target `esp32s31` is a **preview** target — every
  `idf.py` invocation needs `--preview`.


## Firmware model

Two firmware images, different jobs:

- **factory** (`firmware/factory`): production-line diagnostic console.
  61 application commands plus built-in `help` (62 top-level commands total,
  115200 8N1 on the CH343P debug port).
  Results are tracked per command and persisted in NVS, so a power cycle does
  not lose earlier results; `report` prints the final JSON summary. Single 4 MB
  application partition, no OTA. The boot safe state shuts down unused PMIC
  rails (including the OTP-started, unconnected DCDC4) before any test runs.
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
  (`bsp_pmic_power_off()`); `examples/esp-idf/low-power` guards that write
  against the VBUS-present case.
- Wake source: RX8130CE alarm, wired-AND with the TG28 interrupt onto GPIO2
  (active-low, open-drain through U2). The interrupt handler must service
  both devices in a loop until the line releases.
- Constraints: no 32.768 kHz crystal — the deep-sleep slow clock is the
  internal RC oscillator, which has a wider tolerance than a crystal, so wake
  timing is measured rather than assumed; no battery NTC — the TS pin is a
  fixed 10 kΩ to GND, so the charger has no cell-temperature input for
  charge-enable decisions.

## Board usage flows

Normal development flow:

1. Power from either Type-C port (USB1 preferred for console).
2. Open the CH343P console at 115200 baud.
3. Flash with esptool: merged factory image at 0x0 for production, or
   `idf.py --preview flash` for development builds.
4. Interact through factory commands, or develop against
   `components/candis_s31/` (`bsp_display_start()` and friends) from an
   example project. The Board Manager path is demonstrated by
   `examples/esp-idf/display-hello`.

Usage boundaries:

- Camera and JTAG are mutually exclusive: CAM_PCLK/XCLK/VSYNC/HSYNC share
  GPIO54-57 with JTAG. Disable JTAG before powering the camera.
- EXT connector: 3V3_EXT_SW is limited to 300 mA; externally powered devices
  must not backfeed SDA/SCL; the connector is not hot-plug capable.
- Speaker outputs (CN2) are a differential pair — neither side may be
  grounded.
- Do not enable display bias, OTG boost, or other switched rails before the
  EVT1 power checks in `hardware/bring-up.md` are complete.

