# Examples

Every directory listed here is a standalone project. Open the project directory itself; the repository root is not a build target.

## Available now

| Environment | Project | Validation |
|---|---|---|
| ESP-IDF | [`esp-idf/getting-started`](esp-idf/getting-started) | Compiles with ESP-IDF `v6.1-beta1`; hardware not tested |
| ESP-IDF | [`esp-idf/low-power`](esp-idf/low-power) | Compiles with ESP-IDF `v6.1-beta1`; hardware not tested |

Arduino and PlatformIO examples are intentionally absent. Arduino first needs a released ESP32-S31 core; PlatformIO first needs ESP32-S31 platform, tool, and framework support. Their Candis-S31 board metadata and examples will be added only after the normal public installation path works in CI.

## Rules for examples

- The project must build from its own directory.
- It must follow the normal layout of its framework.
- It must not require files to be copied into an SDK installation.
- Generated build directories and downloaded dependencies are not committed.
- Expected behavior and hardware assumptions belong in the project's README.
- A successful compile must not be described as a hardware test.

The first visual demo will cover the AMOLED, touch, and RGB LED after EVT1 validation. Factory diagnostics and recovery images are maintained under [`firmware/`](../firmware), not as user examples.

Board support code is developed in the appropriate upstream repository. For ESP-IDF, the complete board definition belongs in ESP Board Manager's `esp_friends_boards` collection and reusable drivers belong in Component Registry source repositories. These examples are consumers: after support is released, they reference it through the framework's normal dependency or board-selection mechanism.
