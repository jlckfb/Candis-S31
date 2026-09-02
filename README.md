# Candis-S31

[简体中文](README_ZH.md)

Candis-S31 is a compact ESP32-S31 development board built around a square 2.0-inch 460 × 460 AMOLED. It also includes touch, battery charging and power management, RTC, two USB Type-C ports, audio, a DVP camera connector, a TF card slot, buttons, and one RGB LED.

> **Hardware status:** EVT1 (schematic v0.5, fabricated 2026-08-03 as
> `v0.5_260803_1544`) is in hand and bring-up is well advanced. Validated on
> physical boards: AMOLED display (460 × 460 QSPI with TE-synchronized LVGL
> pipeline), capacitive touch, audio output (speaker) and input (dual
> microphones), microSD, USB Type-C2 host and device roles, Wi-Fi, BLE, RTC,
> and the PMIC/battery-charging domain (including a watch-style LVGL demo
> with 16 applications). The camera works: the FPC adapter board has arrived,
> and DVP streaming, built-in color bars, and on-screen live preview were
> validated on EVT1 hardware (2026-09-01/02); captured photos are currently
> stored as raw RGB565 frames (JPEG capture in development). Long-run soak
> and the low-power budget characterization are pending.
>
> **Beta testers:** see [BETA.md](BETA.md)（中文内测指南）— every firmware
> project builds after a plain clone with only an ESP-IDF environment.

> **VDD_SPI strap:** GPIO36 is the VDD_SPI voltage strap (datasheet
> Table 3-4) and also the active-low TF-card power enable. The board uses
> a 3.3 V off-package W25Q128 flash, so VDD_SPI = 3.3 V; R6 (10 kΩ pull-up
> to 3.3 V) correctly holds GPIO36 high at reset, matching ESP32-S31 v0.0
> erratum SPI-855 (1.8 V VDD_SPI cannot boot). No R6 rework is needed; do
> not remove R6.

## Getting started

The repository root is not a build project. Open the standalone ESP-IDF example:

```bash
cd examples/esp-idf/getting-started
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p PORT flash monitor
```

The current baseline is ESP-IDF `v6.1-rc1`. Installation Manager (EIM), the official VS Code extension, and the command line all use the same project. Read the [example guide](examples/esp-idf/getting-started/README.md) before building.

## Support status

| Environment | Status |
|---|---|
| ESP-IDF | Starter, Factory, the complete local BSP, and the local Board Manager definition compile with `v6.1-rc1`; hardware validation and upstream release are pending |
| Arduino | Waiting for the ESP32-S31 core, followed by the Candis-S31 board and variant |
| PlatformIO | Waiting for ESP32-S31 platform, tool, and framework support before adding a board manifest |

Arduino and PlatformIO projects will be added only after they build with their normal public tools. A board variant or JSON manifest cannot add a new SoC by itself. This repository does not provide a private ESP-IDF fork, patched framework, or copied third-party libraries.

### Build verification — 2026-09-02

Baseline: ESP-IDF `v6.1-rc1`, target `esp32s31` (preview). `tools/build-all.sh` compiles the thirteen default targets below serially; the complete 2026-09-02 regression passed 13/13 (logs under `build-logs/p0-fix/` and `build-logs/review-20260902/`). The `example:*` targets are maintainer-only: they build esp-bsp examples from an external checkout (`ESP_BSP_ROOT`) and are reported as SKIP when that checkout is absent — see the header of `tools/build-all.sh`. The `display_usb_hid` example is not part of the matrix: it drives HID inputs through `esp_lvgl_port` helpers that this `esp_lvgl_adapter`-based BSP does not use.

| Target (`tools/build-all.sh --list`) | Source | Status (2026-09-02) |
|---|---|---|
| `factory` | `firmware/factory` | Compiles |
| `getting-started` | `examples/esp-idf/getting-started` | Compiles |
| `low-power` | `examples/esp-idf/low-power` | Compiles |
| `player` | `examples/esp-idf/player` | Compiles |
| `demo` | `firmware/demo` | Compiles |
| `camera-test` | `firmware/camera_test` | Compiles |
| `powercycle` | `firmware/powercycle` | Compiles |
| `usb-cdc-device` | `firmware/usb_cdc_device` | Compiles |
| `testapp:candis_s31` | `vendor/esp-bsp/bsp/candis_s31/test_apps` | Compiles |
| `testapp:tg28_sw` | `vendor/esp-bsp/components/tg28_sw/test_apps` | Compiles |
| `testapp:rx8130ce` | `vendor/esp-bsp/components/rx8130ce/test_apps` | Compiles |
| `testapp:fusb303b` | `vendor/esp-bsp/components/fusb303b/test_apps` | Compiles |
| `testapp:cst820` | `vendor/esp-bsp/components/lcd_touch/esp_lcd_touch_cst820/test_apps` | Compiles |

All firmware targets build against the vendored BSP snapshot in
[`vendor/esp-bsp/`](vendor/esp-bsp/README.md), so a plain clone compiles
without extra checkouts or environment variables.

## Repository layout

```text
.
├── hardware/                  # Schematic, pinout, and EVT notes
├── docs/                      # System overview and board usage
├── examples/
│   └── esp-idf/
│       ├── getting-started/   # Standalone ESP-IDF project
│       ├── low-power/         # S0/S1/deep-sleep/S2 state-machine prototype
│       └── player/            # TF-card audio/video player (ESP-GMF)
├── firmware/
│   ├── camera_test/           # Camera/DVP diagnostic with live AMOLED preview
│   ├── demo/                  # Watch-style comprehensive LVGL demo
│   ├── factory/               # Factory Bring-up source and release contract
│   ├── powercycle/            # Power-measurement helper firmware
│   ├── recovery/              # Recovery procedure
│   └── usb_cdc_device/        # Type-C2 USB CDC device diagnostics
├── tools/                     # Build verification, BSP sync, and release scripts
├── vendor/                    # Vendored esp-bsp BSP snapshot
└── .github/workflows/         # Build verification
```

There is intentionally no top-level `CMakeLists.txt`, `components/`, or framework installation inside this repository.

## Hardware resources

- [System overview and board usage](docs/system-overview.md)
- [Hardware overview](hardware/README.md)
- [Schematic](hardware/schematic/SCH_Schematic_3_2026-08-10.pdf)
- [Preliminary pinout](hardware/pinout/README.md)
- [EVT1 bring-up notes](hardware/bring-up.md)
- [Factory firmware](firmware/factory/README.md)
- [Recovery](firmware/recovery/README.md)
- [Upstream ownership and contribution map](UPSTREAM.md)

Do not enable display bias, USB OTG, or other switched rails before the EVT1 power checks are complete.

## Board support

Candis-S31 support is split by ownership:

- the Candis-S31 BSP lives in the public [`LeenixP/esp-bsp`](https://github.com/LeenixP/esp-bsp) fork (branch `feat/candis-s31`), while the reusable TG28_SW / RX8130CE / FUSB303B / CST820 drivers are submitted to [`espressif/idf-extra-components`](https://github.com/espressif/idf-extra-components) per the official guidance — this repository vendors a build-ready snapshot at [`vendor/esp-bsp/`](vendor/esp-bsp/README.md) so every firmware project compiles from a plain clone;
- the complete ESP-IDF board definition belongs in ESP Board Manager's [`espressif/esp_friends_boards`](https://components.espressif.com/components/espressif/esp_friends_boards) collection;
- reusable device drivers belong in their component source repositories and the [ESP Component Registry](https://components.espressif.com/);
- the Arduino board entry and variant belong in [Arduino-ESP32](https://github.com/espressif/arduino-esp32), after its ESP32-S31 core is available;
- the board manifest belongs in [PlatformIO Espressif32](https://github.com/platformio/platform-espressif32), after that platform supports ESP32-S31.

Generic ESP32-S31 fixes belong in ESP-IDF itself. Board pin assignments and device choices do not normally require changes to the ESP-IDF core repository. The BSP follows ESP-BSP APIs so Factory firmware and upstream examples exercise the same implementation. Per the official esp-bsp maintainers' answer (espressif/esp-bsp#823), ESP-BSP accepts only Espressif and M5Stack boards, so the reusable drivers go to idf-extra-components instead and the Board Manager definition remains a separate integration.

For BSP development, point `CANDIS_S31_BSP_PATH` at a live esp-bsp checkout and the build uses it instead of the vendored snapshot; maintainers refresh the snapshot with `tools/sync_bsp.sh`. See [Upstream ownership](UPSTREAM.md) for the repository-by-repository contribution map.

The BSP covers display and touch, TG28_SW power management, RX8130CE RTC, FUSB303B and USB Host, SDMMC, ES8389 audio, the DVP camera pipeline, and the RGB LED. The Board Manager definition generates and compiles against the development components but intentionally omits the Type-C controller and OTG GPIO: its current model cannot atomically enforce Source-before-boost and this board's 500 mA-only policy, so Type-C2 USB Host must use the BSP API.

## License

Source code is licensed under Apache-2.0 unless a file states otherwise. Hardware documents remain subject to the notices included with those files.
