# Firmware

This directory is reserved for firmware that supports a physical Candis-S31 board.

| Directory | Purpose |
|---|---|
| [`factory/`](factory) | Factory source, build instructions, and release contract |
| [`recovery/`](recovery) | ROM download and restoration procedure |

Normal user examples belong in [`examples/`](../examples); the camera preview, power-measurement helper, and Type-C2 USB CDC device projects now live there as [`esp-idf/camera-test`](../examples/esp-idf/camera-test), [`esp-idf/power-cycle`](../examples/esp-idf/power-cycle), and [`esp-idf/usb-cdc-device`](../examples/esp-idf/usb-cdc-device). Factory diagnostics are not examples, and recovery must remain possible even when an application image is broken.

All firmware projects inject the repository-owned board component from
[`components/candis_s31/`](../components/candis_s31) through
[`cmake/candis_components.cmake`](../cmake/candis_components.cmake). Its
reusable drivers resolve from [`vendor/idf-extra-components/`](../vendor/idf-extra-components),
and the declarative Board Manager definition is mirrored under
[`vendor/esp-board-manager/`](../vendor/esp-board-manager).

Released binaries should be attached to a GitHub Release. This directory records the manifest, checksums, flash command, and source commit needed to use those files safely.
