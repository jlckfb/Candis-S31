# Firmware

This directory is reserved for firmware that supports a physical Candis-S31 board.

| Directory | Purpose | Current state |
|---|---|---|
| [`factory/`](factory) | Factory source, build instructions, and release contract | Peripheral command set compile-tested; no image released |
| [`demo/`](demo) | Watch-style comprehensive demo covering every on-board peripheral (LVGL UI) | Builds clean; validated on the EVT1 board |
| [`camera_test/`](camera_test) | Automatic camera/DVP driver diagnostic with live AMOLED preview and operator PASS/FAIL confirmation | Camera link validated on EVT1 with the FPC adapter board; automatic checks and live preview pass |
| [`usb_cdc_device/`](usb_cdc_device) | Dedicated Type-C2 USB CDC device diagnostics | Windows COM6 enumeration and command loop validated on EVT1 |
| [`powercycle/`](powercycle) | Power-measurement helper firmware | Documented; compiles, and the ACTIVE/deep-sleep cycle runs on EVT1 boards |
| [`recovery/`](recovery) | ROM download and restoration procedure | Draft; hardware not tested |

Normal user examples belong in [`examples/`](../examples). Factory diagnostics are not examples, and recovery must remain possible even when an application image is broken.

All firmware projects build against the vendored BSP snapshot in
`vendor/esp-bsp/` by default. Setting `CANDIS_S31_BSP_PATH` to a live esp-bsp
checkout switches the build to that checkout for BSP development.

Released binaries should be attached to a GitHub Release. This directory records the manifest, checksums, flash command, source commit, and validation status needed to use those files safely.
