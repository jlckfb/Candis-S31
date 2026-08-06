# Candis-S31

[简体中文](README_ZH.md)

Candis-S31 is a compact ESP32-S31 development board built around a square 2.0-inch 460 × 460 AMOLED. It also includes touch, battery charging and power management, RTC, two USB Type-C ports, audio, a DVP camera connector, a TF card slot, buttons, and one RGB LED.

> **Hardware status:** EVT1 is still in layout and has not been fabricated. The ESP-IDF starter and Factory Bring-up project are compile-tested, but no board function has been verified on hardware.

## Getting started

The repository root is not a build project. Open the standalone ESP-IDF example:

```bash
cd examples/esp-idf/getting-started
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p PORT flash monitor
```

The current baseline is ESP-IDF `v6.1-beta1`. Installation Manager (EIM), the official VS Code extension, and the command line all use the same project. Read the [example guide](examples/esp-idf/getting-started/README.md) before building.

## Support status

| Environment | Status |
|---|---|
| ESP-IDF | Starter, Factory, the complete local BSP, and the local Board Manager definition compile with `v6.1-beta1`; hardware validation and upstream release are pending |
| Arduino | Waiting for the ESP32-S31 core, followed by the Candis-S31 board and variant |
| PlatformIO | Waiting for ESP32-S31 platform, tool, and framework support before adding a board manifest |

Arduino and PlatformIO projects will be added only after they build with their normal public tools. A board variant or JSON manifest cannot add a new SoC by itself. This repository does not provide a private ESP-IDF fork, patched framework, or copied third-party libraries.

### Build verification — 2026-08-05

Baseline: ESP-IDF `v6.1-beta1`, target `esp32s31` (preview). `tools/build-all.sh` compiles the fifteen targets below serially; as of 2026-08-06 all fifteen pass. "Compiles" means exactly a successful compile — **no target has run on hardware; every board function is pending EVT measurement**.

| Target (`tools/build-all.sh --list`) | Source | Status (2026-08-06) |
|---|---|---|
| `example:display` | esp-bsp worktree `examples/display` | Compiles; pending EVT hardware test |
| `example:display_camera_video` | esp-bsp worktree `examples/display_camera_video` | Compiles; pending EVT hardware test |
| `example:display_lvgl_demos` | esp-bsp worktree `examples/display_lvgl_demos` | Compiles; pending EVT hardware test |
| `example:display_lvgl_benchmark` | esp-bsp worktree `examples/display_lvgl_benchmark` | Compiles; pending EVT hardware test |
| `example:display_sdcard` | esp-bsp worktree `examples/display_sdcard` | Compiles; pending EVT hardware test |
| `example:display_usb_hid` | esp-bsp worktree `examples/display_usb_hid` | Compiles; pending EVT hardware test |
| `example:audio` | esp-bsp worktree `examples/audio` | Compiles; pending EVT hardware test |
| `example:display_audio_photo` | esp-bsp worktree `examples/display_audio_photo` | Compiles; pending EVT hardware test |
| `testapp:tg28_sw` | esp-bsp worktree `components/tg28_sw/test_apps` | Compiles; pending EVT hardware test |
| `testapp:rx8130ce` | esp-bsp worktree `components/rx8130ce/test_apps` | Compiles; pending EVT hardware test |
| `testapp:fusb303b` | esp-bsp worktree `components/fusb303b/test_apps` | Compiles; pending EVT hardware test |
| `testapp:cst820` | esp-bsp worktree `components/lcd_touch/esp_lcd_touch_cst820/test_apps` | Compiles; pending EVT hardware test |
| `factory` | `firmware/factory` (with `CANDIS_S31_BSP_PATH`) | Compiles; pending EVT hardware test |
| `getting-started` | `examples/esp-idf/getting-started` | Compiles; pending EVT hardware test |
| `low-power` | `examples/esp-idf/low-power` (with `CANDIS_S31_BSP_PATH`) | Compiles; pending EVT hardware test |

## Repository layout

```text
.
├── hardware/                  # Schematic, pinout, and EVT notes
├── examples/
│   └── esp-idf/
│       └── getting-started/   # Standalone ESP-IDF project
├── firmware/
│   ├── factory/               # Factory Bring-up source and release contract
│   └── recovery/              # Recovery procedure
└── .github/workflows/         # Build verification
```

There is intentionally no top-level `CMakeLists.txt`, `components/`, or framework installation inside this repository.

## Hardware resources

- [Hardware overview](hardware/README.md)
- [Schematic](hardware/schematic/Easy-S31_SCH_v0.5_2026-07-28_0924.pdf)
- [Preliminary pinout](hardware/pinout/README.md)
- [EVT1 bring-up notes](hardware/bring-up.md)
- [Factory firmware](firmware/factory/README.md)
- [Recovery](firmware/recovery/README.md)
- [Upstream ownership and contribution map](UPSTREAM.md)

Do not enable display bias, USB OTG, or other switched rails before the EVT1 power checks are complete.

## Board support

Candis-S31 support is split by ownership:

- a Candis-S31 BSP is developed in an `esp-bsp` worktree and validated by the Factory project before any upstream proposal;
- the complete ESP-IDF board definition belongs in ESP Board Manager's [`espressif/esp_friends_boards`](https://components.espressif.com/components/espressif/esp_friends_boards) collection;
- reusable device drivers belong in their component source repositories and the [ESP Component Registry](https://components.espressif.com/);
- the Arduino board entry and variant belong in [Arduino-ESP32](https://github.com/espressif/arduino-esp32), after its ESP32-S31 core is available;
- the board manifest belongs in [PlatformIO Espressif32](https://github.com/platformio/platform-espressif32), after that platform supports ESP32-S31.

Generic ESP32-S31 fixes belong in ESP-IDF itself. Board pin assignments and device choices do not normally require changes to the ESP-IDF core repository. The local BSP follows ESP-BSP APIs so Factory firmware and upstream examples exercise the same implementation. A complete ESP-BSP contribution is proposed only after EVT validation and maintainer agreement; the Board Manager definition remains a separate integration.

The BSP implementation is not mirrored back into this repository. During development, Factory firmware loads it from a separate checkout through `CANDIS_S31_BSP_PATH`; public examples consume released support. See [Upstream ownership](UPSTREAM.md) for the repository-by-repository contribution map.

The local BSP covers display and touch, TG28_SW power management, RX8130CE RTC, FUSB303B and USB Host, SDMMC, ES8389 audio, the DVP camera pipeline, and the RGB LED. The matching Board Manager definition also generates and compiles against these development components. This is an implementation milestone, not a hardware qualification result; every EVT result stays `NOT_RUN` until measured on a physical board.

## License

Source code is licensed under Apache-2.0 unless a file states otherwise. Hardware documents remain subject to the notices included with those files.
