# Firmware

This directory is reserved for firmware that supports a physical Candis-S31 board.

| Directory | Purpose | Current state |
|---|---|---|
| [`factory/`](factory) | Factory source, build instructions, and release contract | Peripheral command set compile-tested; no image released |
| [`usb_cdc_device/`](usb_cdc_device) | Dedicated Type-C2 USB CDC device diagnostics | Windows COM6 enumeration and command loop validated on EVT1 |
| [`recovery/`](recovery) | ROM download and restoration procedure | Draft; hardware not tested |

Normal user examples belong in [`examples/`](../examples). Factory diagnostics are not examples, and recovery must remain possible even when an application image is broken.

The Factory project consumes the Candis-S31 BSP from its upstream development
checkout. It does not carry a private copy of the BSP or reusable device
drivers.

Released binaries should be attached to a GitHub Release. This directory records the manifest, checksums, flash command, source commit, and validation status needed to use those files safely.
