# Firmware

This directory is reserved for firmware that supports a physical Candis-S31 board.

| Directory | Purpose | Current state |
|---|---|---|
| [`factory/`](factory) | Factory source, build instructions, and release contract | Peripheral command set compile-tested; no image released |
| [`demo/`](demo) | Watch-style comprehensive demo covering every on-board peripheral (LVGL UI) | Builds clean; EVT1 board validation pending |
| [`camera_test/`](camera_test) | Headless automatic camera/DVP driver diagnostic; reports SCCB, V4L2, frame, timing, and teardown results over UART | Three-cycle hardware run passed |
| [`usb_cdc_device/`](usb_cdc_device) | Dedicated Type-C2 USB CDC device diagnostics | Windows COM6 enumeration and command loop validated on EVT1 |
| [`recovery/`](recovery) | ROM download and restoration procedure | Draft; hardware not tested |

Normal user examples belong in [`examples/`](../examples). Factory diagnostics are not examples, and recovery must remain possible even when an application image is broken.

The Factory project consumes the Candis-S31 BSP from its upstream development
checkout. It does not carry a private copy of the BSP or reusable device
drivers.

Released binaries should be attached to a GitHub Release. This directory records the manifest, checksums, flash command, source commit, and validation status needed to use those files safely.
