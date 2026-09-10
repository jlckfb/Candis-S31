# ESP-IDF getting started

This project checks the ESP32-S31 build, flash, and serial path without enabling any Candis-S31 peripheral. It is deliberately small so a toolchain problem can be separated from display, touch, or power-sequencing work.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB |
| Managed components | None |
| Compile status | Verified |

## What it prints

The application reports:

- the configured IDF target;
- CPU core count and silicon revision;
- detected flash capacity;
- free heap after startup.

It does not initialize the AMOLED, touch controller, PMIC, RTC, audio, camera, TF card, RGB LED, or USB power control.

## Install ESP-IDF

ESP32-S31 is currently a preview target. Install the official `v6.1-rc1` tag:

```bash
git clone -b v6.1-rc1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s31
. ./export.sh
```

If ESP-IDF is already installed through Espressif Installation Manager (EIM), select its `v6.1-rc1` environment. A Candis-specific IDF installation is not required.

## Build from a terminal

Open a terminal in this directory:

```bash
idf.py --preview set-target esp32s31
idf.py --preview build
```

`set-target` creates a local `sdkconfig`. That file is generated for the current checkout and is ignored by Git. Shared settings remain in `sdkconfig.defaults` and `sdkconfig.defaults.esp32s31`.

## Build with VS Code

1. Install the official **Espressif IDF** extension.
2. Install or select ESP-IDF `v6.1-rc1` through EIM.
3. Open this `getting-started` directory, not the Candis-S31 repository root.
4. Run **ESP-IDF: Select Current ESP-IDF Version** and choose the same environment.
5. Set the target to `esp32s31` and keep preview-target support enabled.
6. Run **ESP-IDF: Build your project**.

The VS Code extension and command line use the same CMake files. No project-specific plugin is needed.

## Flash and monitor

Connect the programming USB port and replace `PORT` with the serial device:

```bash
idf.py --preview -p PORT flash monitor
```

Exit the monitor with `Ctrl-]`.

Expected application messages include:

```text
Candis-S31 getting started
target=esp32s31 ...
flash=16 MiB
board peripherals are not initialized by this example
```

The ROM and bootloader print additional lines before the application starts.
Capture **5 s** from reset and require the target, flash size, free heap and
final `board peripherals are not initialized by this example` line.

## Clean rebuild

Run a full clean after switching ESP-IDF versions:

```bash
idf.py fullclean
idf.py --preview set-target esp32s31
idf.py --preview build
```

Do not copy `sdkconfig`, `build/`, or `managed_components/` from an older environment.

## Why there is no dependencies.lock

This project has no `idf_component.yml` and no managed component dependencies, so Component Manager has nothing to lock. Examples that use registry components commit their generated `dependencies.lock`; lock files must not be edited by hand.

Candis-S31 board support is not copied into a local `components/` directory. A peripheral example declares the Board Manager and board-collection dependencies in `main/idf_component.yml`. This project stays dependency-free.

## Troubleshooting

- **Unknown target:** confirm that the selected IDF version explicitly supports ESP32-S31, then retain `--preview`.
- **Wrong IDF in VS Code:** select the intended EIM installation and check the version in the status bar.
- **No serial port:** check the USB data cable, connector, permissions, and device name.
- **Flash succeeds but the application does not start:** save the complete ROM and bootloader log before erasing anything.
- **The screen remains black:** this project never powers the display; a black screen is expected.

Read the [board bring-up notes](../../../hardware/bring-up.md) before adding board GPIO or power control.
