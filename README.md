# Candis-S31

[简体中文](README_ZH.md)

Candis-S31 is a compact ESP32-S31 development board built around a square 2.0-inch 460 × 460 AMOLED. It also includes touch, battery charging and power management, RTC, two USB Type-C ports, audio, a DVP camera connector, a TF card slot, buttons, and one RGB LED.

> **Hardware status:** EVT1 (schematic v0.5, fabricated 2026-08-03 as
> `v0.5_260803_1544`) is in hand and bring-up is well advanced. Validated on
> physical boards: AMOLED display (460 x 460 QSPI with TE-synchronized LVGL
> pipeline), capacitive touch, microSD, USB Type-C2 host and device roles,
> Wi-Fi, BLE, RTC, and the PMIC/battery-charging domain (including a
> watch-style LVGL demo with 16 applications). The ES8389 codec initializes and
> digital audio paths have passed their available checks; speaker/microphone
> listening and recording acceptance on the current EVT1 board remain open.
> The camera link, sensor detection, and built-in color-bar path have been
> exercised on EVT1 with the FPC adapter; real-scene image quality and the
> JPEG capture path still require hardware confirmation. Long-run soak and the
> low-power budget characterization are pending.

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
| ESP-IDF | Starter, the repository-owned board runtime, and the local Board Manager definition compile with `v6.1-rc1`; the four reusable drivers are mapped to local snapshots until their releases are published |
| Arduino | Waiting for the ESP32-S31 core, followed by the Candis-S31 board and variant |
| PlatformIO | Waiting for ESP32-S31 platform, tool, and framework support before adding a board manifest |

Arduino and PlatformIO projects will be added only after they build with their normal public tools. A board variant or JSON manifest cannot add a new SoC by itself. This repository does not provide a private ESP-IDF fork or patched framework; repository-local component mirrors are identified under `components/`, `vendor/`, and [UPSTREAM.md](UPSTREAM.md).

### Build verification — 2026-09-04

Baseline: ESP-IDF `v6.1-rc1`, target `esp32s31` (preview). `tools/build-all.sh` compiles the fourteen default targets serially; the matrix is designed to run from a plain clone. Build logs are local artifacts and are intentionally not shipped; reproduce the matrix with `tools/build-all.sh` and inspect its printed summary. The `display-hello` target also validates the local Board Manager generation output and its relative component overrides.

| Target (`tools/build-all.sh --list`) | Source | Status |
|---|---|---|
| `factory` | `firmware/factory` | Compiles |
| `getting-started` | `examples/esp-idf/getting-started` | Compiles |
| `low-power` | `examples/esp-idf/low-power` | Compiles |
| `player` | `examples/esp-idf/player` | Compiles |
| `display-hello` | `examples/esp-idf/display-hello` | Compiles; local Board Manager path |
| `demo` | `firmware/demo` | Compiles |
| `camera-test` | `firmware/camera_test` | Compiles |
| `powercycle` | `firmware/powercycle` | Compiles |
| `usb-cdc-device` | `firmware/usb_cdc_device` | Compiles |
| `testapp:candis_s31` | `components/candis_s31/test_apps` | Compiles |
| `testapp:tg28_sw` | `vendor/idf-extra-components/tg28_sw/test_apps` | Compiles |
| `testapp:rx8130ce` | `vendor/idf-extra-components/rx8130ce/test_apps` | Compiles |
| `testapp:fusb303b` | `vendor/idf-extra-components/fusb303b/test_apps` | Compiles |
| `testapp:cst820` | `vendor/idf-extra-components/esp_lcd_touch_cst820/test_apps` | Compiles |

The board runtime is maintained in [`components/candis_s31/`](components/candis_s31), and the reusable drivers are mapped to [`vendor/idf-extra-components/`](vendor/idf-extra-components). The declarative Board Manager sources are mirrored under [`vendor/esp-board-manager/`](vendor/esp-board-manager). No external BSP checkout is required.

## Repository layout

```text
.
├── hardware/                  # Schematic, pinout, and EVT notes
├── docs/                      # System overview and board usage
├── cmake/                     # Shared standalone-project wiring
├── components/
│   ├── candis_s31/            # Repository-owned board runtime component
│   └── esp_lvgl_port/         # Board-local LVGL compatibility port
├── examples/
│   └── esp-idf/
│       ├── getting-started/   # Toolchain smoke test
│       ├── display-hello/     # Board Manager + LVGL display smoke test
│       ├── low-power/         # S0/S1/deep-sleep/S2 state-machine prototype
│       └── player/             # TF-card audio/video player (ESP-GMF)
├── firmware/
│   ├── camera_test/           # Camera/DVP diagnostic with live AMOLED preview
│   ├── demo/                  # Watch-style comprehensive LVGL demo
│   ├── factory/               # Factory Bring-up source and release contract
│   ├── powercycle/            # Power-measurement helper firmware
│   ├── recovery/              # Recovery procedure
│   └── usb_cdc_device/        # Type-C2 USB CDC device diagnostics
├── tools/                     # Build verification and release scripts
├── vendor/
│   ├── esp-board-manager/     # Local Board Manager and friends-board snapshot
│   └── idf-extra-components/  # Local reusable-driver snapshot
└── .github/workflows/         # Build verification
```

Each standalone ESP-IDF project builds from its own directory. The repository
root intentionally has no top-level application `CMakeLists.txt`.

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

The repository follows the ownership split requested by the Espressif
maintainers in [esp-bsp#823](https://github.com/espressif/esp-bsp/pull/823):

- The Candis-S31 runtime component is maintained here at
  [`components/candis_s31/`](components/candis_s31). It contains board pins,
  power sequencing, Type-C policy, camera clock workarounds, and the validated
  AMOLED/LVGL path; no ESP-BSP checkout is required.
- The reusable TG28_SW, RX8130CE, FUSB303B, and CST820 drivers are staged in
  [`vendor/idf-extra-components/`](vendor/idf-extra-components) for later
  manual submission to `espressif/idf-extra-components`.
- The declarative board description is staged in
  [`vendor/esp-board-manager/esp_friends_boards/`](vendor/esp-board-manager/esp_friends_boards)
  for later manual submission to `esp_friends_boards`. The
  [`display-hello`](examples/esp-idf/display-hello) example demonstrates its
  local Board Manager integration.

Board Manager describes device and peripheral wiring, but it does not encode
this board's complete shared-interrupt, charging, Type-C, camera-clock, or
display-transition policy. The advanced firmware therefore consumes the
repository-owned runtime component while the small display example uses the
Board Manager path directly.

## License

Source code is licensed under Apache-2.0 unless a file states otherwise. Hardware documents remain subject to the notices included with those files.
