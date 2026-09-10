<h1 align="center">Candis-S31</h1>

<p align="center">
  Open hardware and firmware reference design for the ESP32-S31
</p>

<p align="center">
  <img src="https://img.shields.io/badge/target-ESP32--S31-E7352C" alt="Target: ESP32-S31">
  <img src="https://img.shields.io/badge/ESP--IDF-v6.1--rc1-3C3C3C" alt="ESP-IDF v6.1-rc1">
  <img src="https://img.shields.io/badge/hardware-v0.5--Beta1%20%2F%20EVT1-2F6FEB" alt="Hardware revision v0.5-Beta1 / EVT1">
  <img src="https://img.shields.io/badge/license-Apache--2.0-2F6FEB" alt="Apache-2.0">
</p>

<p align="center">
  <a href="README_ZH.md">简体中文</a> ·
  <a href="QUICKSTART.md">Quick start</a> ·
  <a href="#examples">Examples</a> ·
  <a href="#specifications">Specifications</a>
</p>

---

Candis-S31 integrates a square AMOLED display and touch, audio, a DVP camera connector, a TF card slot,
two USB Type-C ports and TG28 power management on one board, providing a reusable hardware and firmware
baseline for ESP32-S31 products.

## Hardware at a glance

<p align="center">
  <img src="img/board-overview.png" alt="Candis-S31 front and back with component callouts" width="760">
</p>

| # | Block | Description |
|---|---|---|
| 01 | **ESP32-S31** | Dual-core RISC-V, targeting graphics, multimedia and wireless interaction |
| 02 | **CO5300 AMOLED** | 2.0-inch square panel, 460 × 460, QSPI interface |
| 03 | **CST820** | On-board capacitive touch controller, completing the display interaction path |
| 04 | **TG28 PMIC** | Multiple power rails, battery charging and RTC supply |
| 05 | **ES8389** | Audio codec, dual analog microphones and a speaker amplifier |
| 06 | **Dual Type-C** | Separate debug interface and native USB OTG interface |

## ESP32-S31 platform

<p align="center">
  <img src="img/esp32-s31.png" alt="Espressif ESP32-S31" width="560">
</p>

| Capability | Detail |
|---|---|
| **Processor** | Dual-core high-performance 32-bit RISC-V, up to 320 MHz |
| **Memory** | 512 KB SRAM, 16 MB / 32 MB in-package Octal PSRAM |
| **Flash** | 16 MB W25Q128 QSPI flash |
| **Graphics** | JPEG codec and 2D DMA ("PPA") hardware |
| **Wireless** | 2.4 GHz Wi-Fi 6, Bluetooth 5.4, IEEE 802.15.4 |

## Power and interfaces

| Port | Interface | Description |
|---|---|---|
| C1 | Debug Type-C | CH343P USB-to-serial bridge: download, serial console and power input |
| C2 | USB OTG | Native ESP32-S31 USB with FUSB303B role control and ISL9113 5 V boost |
| PWR | TG28 PMIC | Multiple DCDC/LDO rails, battery charging, power on/off control and RTC supply |
| RTC | RX8130CE | Standalone real-time clock with an integrated crystal |
| I²C | EXT header | GH1.25-4 carrying GND, switched 3.3 V and an independent I²C bus |
| RGB | Buttons and light | PWRON, BOOT and RESET buttons plus one WS2812 RGB LED |

## Specifications

| Item | Detail |
|---|---|
| Main processor | ESP32-S31 dual-core 32-bit RISC-V, up to 320 MHz |
| Low-power coprocessor | Single-core 32-bit RISC-V, up to 40 MHz |
| On-chip memory | 512 KB SRAM |
| PSRAM | 16 MB / 32 MB in-package Octal PSRAM |
| Flash | 16 MB W25Q128 QSPI flash, 3.3 V |
| Wireless | 2.4 GHz Wi-Fi 6, Bluetooth 5.4, IEEE 802.15.4 |
| Display | 2.0-inch 460 × 460 AMOLED, CO5300 QSPI controller |
| Touch | CST820 capacitive touch controller |
| Audio | ES8389 codec, dual analog microphones, NS4150B differential speaker amplifier |
| Camera | 24-pin FPC, 8-bit DVP data interface |
| Storage | TF / microSD slot on the SDMMC bus with a switched supply |
| USB Type-C 1 | CH343P USB-to-serial: download, console and power input |
| USB Type-C 2 | Native USB OTG with FUSB303B Type-C control and ISL9113 5 V boost |
| Power management | TG28 PMIC: DCDC/LDO rails, battery charging, soft power on/off and VRTC |
| Real-time clock | RX8130CE with an integrated 32.768 kHz crystal |
| Expansion | GH1.25-4: GND, switched 3.3 V, I²C SDA, I²C SCL |
| Other resources | PWRON / BOOT / RESET buttons, WS2812B-1313-V6 RGB LED |
| Hardware revision | v0.5-Beta1 / EVT1 |

> **Notes**
>
> - The board ships without a battery or an enclosure.
> - USB Type-C 2 is limited to 5 V / 500 mA when used as a power output; the switched 3.3 V supply on the
>   EXT header is limited to 300 mA.
> - A camera module must match the 24-pin FPC pinout.
> - The speaker output is differential — neither side may be grounded.
> - The flash supply (VDD_SPI) is 3.3 V because the external W25Q128 is a 3.3 V part; R6 holds GPIO36 high
>   at reset accordingly. Keep R6 fitted.

## Gallery

<p align="center">
  <img src="img/gallery-1.jpg" alt="Candis-S31 powered from a battery" width="290">
  <img src="img/gallery-2.jpg" alt="Candis-S31 AMOLED in use" width="290">
  <img src="img/gallery-3.jpg" alt="Candis-S31 connectors and buttons" width="290">
</p>

---

## Getting started

The repository root is not a build project; every example is a standalone ESP-IDF project.

```bash
cd examples/esp-idf/getting-started
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p PORT flash monitor
```

`tools/build-all.sh` compiles the whole matrix serially, which is the recommended first check after a
clone:

```bash
tools/build-all.sh --list      # targets and their source directories
tools/build-all.sh             # build every target
```

The validated baseline is ESP-IDF `v6.1-rc1` (preview target `esp32s31`). Espressif Installation Manager,
the official VS Code extension and the command line all drive the same project. Environment setup,
flashing and the board caveats are covered in **[QUICKSTART.md](QUICKSTART.md)**.

## Examples

| Example | Purpose |
|---|---|
| [`getting-started`](examples/esp-idf/getting-started) | Toolchain and target smoke test |
| [`display-hello`](examples/esp-idf/display-hello) | Minimal AMOLED startup through the Board Manager definition |
| [`display-touch`](examples/esp-idf/display-touch) | AMOLED cross marker with live touch coordinates |
| [`display-benchmark`](examples/esp-idf/display-benchmark) | Full-screen and partial refresh throughput |
| [`camera-test`](examples/esp-idf/camera-test) | OV5640 DVP capture with a live 460 × 460 AMOLED preview |
| [`player`](examples/esp-idf/player) | TF-card audio and video player on the ESP-GMF pipeline |
| [`audio-recorder`](examples/esp-idf/audio-recorder) | Microphone capture to a WAV file on the TF card |
| [`audio-player`](examples/esp-idf/audio-player) | Sine tone generation and WAV playback |
| [`storage`](examples/esp-idf/storage) | TF card mount and file read/write check |
| [`usb-host-msc`](examples/esp-idf/usb-host-msc) | Type-C2 host mode mounting a USB mass-storage device |
| [`usb-cdc-device`](examples/esp-idf/usb-cdc-device) | Type-C2 native USB CDC device console |
| [`led`](examples/esp-idf/led) | WS2812B RGB LED effects |
| [`buttons`](examples/esp-idf/buttons) | BOOT and PWR button events |
| [`rtc`](examples/esp-idf/rtc) | RX8130CE time keeping and alarms |
| [`pmic`](examples/esp-idf/pmic) | TG28 rail dump and speaker amplifier control |
| [`power-cycle`](examples/esp-idf/power-cycle) | Board power state and battery domain helper |
| [`low-power`](examples/esp-idf/low-power) | Light sleep, deep sleep and screen-off state machine |
| [`wifi`](examples/esp-idf/wifi) | Wi-Fi station scan and connection |
| [`ble`](examples/esp-idf/ble) | NimBLE advertising and scanning |
| [`factory`](firmware/factory) | Production console for board checks |

The full index with per-project notes is in [`examples/README.md`](examples/README.md).

## Repository layout

```text
.
├── cmake/                     # Shared standalone-project wiring
├── components/
│   ├── candis_s31/            # Board runtime component (pins, power, display, audio, camera)
│   └── esp_lvgl_port/         # Board-local LVGL port
├── examples/esp-idf/          # Standalone example projects
├── firmware/
│   ├── factory/               # Factory console and release contract
│   └── recovery/              # Recovery procedure
├── img/                       # Images used by the documentation
├── tools/                     # Build matrix and release scripts
├── vendor/
│   ├── esp-board-manager/     # Board Manager and friends-board snapshot
│   └── idf-extra-components/  # Local reusable-driver snapshot
└── .github/workflows/         # Build verification
```

Every standalone project builds from its own directory; the repository root intentionally has no
top-level application `CMakeLists.txt`.

## Documentation

| Document | Content |
|---|---|
| [QUICKSTART.md](QUICKSTART.md) | Toolchain setup, flashing, serial ports, hardware notes |
| [examples/README.md](examples/README.md) | Example index |
| [components/candis_s31](components/candis_s31/README.md) | Board runtime component and [API reference](components/candis_s31/API.md) |
| [firmware/factory](firmware/factory/README.md) | Factory console and its release contract |
| [firmware/recovery](firmware/recovery/README.md) | Recovery procedure |
| [OSHWHub project page](https://oshwhub.com/li-chuang-kai-fa-ban/project_spaoolal) | Schematic, PCB and pinout files |

## Board support

- The Candis-S31 runtime component lives in [`components/candis_s31/`](components/candis_s31) and owns the
  board pins, power sequencing, Type-C policy, camera clock setup and the AMOLED/LVGL path. No ESP-BSP
  checkout is required.
- The reusable TG28_SW, RX8130CE, FUSB303B and CST820 drivers are mirrored in
  [`vendor/idf-extra-components/`](vendor/idf-extra-components), and the declarative board description in
  [`vendor/esp-board-manager/esp_friends_boards/`](vendor/esp-board-manager/esp_friends_boards).
  [`display-hello`](examples/esp-idf/display-hello) uses that Board Manager definition directly.
- Board Manager describes device and peripheral wiring; the board's shared-interrupt, charging, Type-C,
  camera-clock and display-transition policy lives in the runtime component. Advanced firmware therefore
  uses the component, while the small display example uses the Board Manager path.

## License

Source code is licensed under Apache-2.0 unless a file states otherwise. Hardware documents remain subject
to the notices included with those files.
