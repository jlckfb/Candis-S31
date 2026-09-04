# Examples

Every directory listed here is a standalone project. Open the project directory itself; the repository root is not a build target.

## Available now

| Environment | Project | Validation |
|---|---|---|
| ESP-IDF | [`esp-idf/getting-started`](esp-idf/getting-started) | Compiles with ESP-IDF `v6.1-rc1`; hardware not tested |
| ESP-IDF | [`esp-idf/display-hello`](esp-idf/display-hello) | Compiles with local Board Manager snapshots; hardware not tested |
| ESP-IDF | [`esp-idf/low-power`](esp-idf/low-power) | Compiles with ESP-IDF `v6.1-rc1`; hardware not tested |
| ESP-IDF | [`esp-idf/player`](esp-idf/player) | TF-card audio/video player; compiles with ESP-IDF `v6.1-rc1` |

Arduino and PlatformIO examples are intentionally absent. Arduino first needs a released ESP32-S31 core; PlatformIO first needs ESP32-S31 platform, tool, and framework support. Their Candis-S31 board metadata and examples will be added only after the normal public installation path works in CI.

## Rules for examples

- The project must build from its own directory.
- It must follow the normal layout of its framework.
- It must not require files to be copied into an SDK installation.
- Generated build directories and downloaded dependencies are not committed.
- Expected behavior and hardware assumptions belong in the project's README.
- A successful compile must not be described as a hardware test.

The watch-style visual demo is maintained as firmware under [`firmware/demo/`](../firmware/demo) and is validated on the EVT1 board. Factory diagnostics and recovery images are maintained under [`firmware/`](../firmware), not as user examples.

The Board Manager example uses the committed board definition under
[`vendor/esp-board-manager/`](../vendor/esp-board-manager). The advanced board
runtime remains in [`components/candis_s31/`](../components/candis_s31) because
Board Manager describes devices but does not encode this board's complete
power, Type-C, shared-interrupt, camera-clock, or display-transition policy.
