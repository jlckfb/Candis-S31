# Firmware

This directory is reserved for firmware that supports a physical Candis-S31 board.

| Directory | Purpose | Current state |
|---|---|---|
| [`factory/`](factory) | Factory source, build instructions, and release contract | Peripheral command set compile-tested; no image released |
| [`demo/`](demo) | Watch-style comprehensive demo for supported board peripherals (LVGL UI) | Builds clean; core peripheral paths validated on EVT1; camera real-scene/JPEG path pending |
| [`camera_test/`](camera_test) | Automatic camera/DVP driver diagnostic with live AMOLED preview and operator PASS/FAIL confirmation | Sensor detection, DVP stream, and built-in color-bar path exercised on EVT1; real-scene image quality pending |
| [`usb_cdc_device/`](usb_cdc_device) | Dedicated Type-C2 USB CDC device diagnostics | Windows COM6 enumeration and command loop validated on EVT1 |
| [`powercycle/`](powercycle) | Power-measurement helper firmware | Documented; compiles; full ACTIVE/deep-sleep cycle remains a pending validation item |
| [`recovery/`](recovery) | ROM download and restoration procedure | Scripted and compile-independent; full recovery run remains hardware-validation pending |

Normal user examples belong in [`examples/`](../examples). Factory diagnostics are not examples, and recovery must remain possible even when an application image is broken.

All firmware projects inject the repository-owned board component from
[`components/candis_s31/`](../components/candis_s31) through
[`cmake/candis_components.cmake`](../cmake/candis_components.cmake). Its
reusable drivers resolve from [`vendor/idf-extra-components/`](../vendor/idf-extra-components),
and the declarative Board Manager definition is mirrored under
[`vendor/esp-board-manager/`](../vendor/esp-board-manager).

Released binaries should be attached to a GitHub Release. This directory records the manifest, checksums, flash command, source commit, and validation status needed to use those files safely.
