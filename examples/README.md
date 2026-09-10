# Examples

Every directory listed here is a standalone project. Open the project directory itself; the repository root is not a build target.

## Available now

| Environment | Project | Validation |
|---|---|---|
| ESP-IDF | [`esp-idf/getting-started`](esp-idf/getting-started) | Build, flash and boot verified on EVT1 (target, flash size and heap printed) |
| ESP-IDF | [`esp-idf/display-hello`](esp-idf/display-hello) | EVT1: display-only Board Manager startup at 1.67 s, 460x460, QSPI 48 MHz and MX+MY 180°; no unrelated codec probe; panel appearance remains a visual check |
| ESP-IDF | [`esp-idf/low-power`](esp-idf/low-power) | EVT1: two S0/S1 cycles and two 20 s timer deep-sleeps verified; S2 correctly refuses shutdown while VBUS is present; battery-only soft-off not tested |
| ESP-IDF | [`esp-idf/player`](esp-idf/player) | EVT1: the pipeline starts, the TF card mounts at 40 MHz and three media files are listed; a temporary probe played the bundled AVI to EOF/100% in 5.60 s; touch selection and visual/listening quality remain separate |
| ESP-IDF | [`esp-idf/camera-test`](esp-idf/camera-test) | EVT1: reset to preview at 2.066 s, capture 39.3 fps with no dropped/superseded frames; the 112/109 colour trim measures R/G 0.990 and B/G 1.025 over the displayed window on a full-frame grey card; panel appearance still needs an operator check |
| ESP-IDF | [`esp-idf/power-cycle`](esp-idf/power-cycle) | EVT1: four ACTIVE entries and three successful safe-state / timer deep-sleep cycles in 75 s; current consumption not measured |
| ESP-IDF | [`esp-idf/usb-cdc-device`](esp-idf/usb-cdc-device) | EVT1: device reports ready and waits for a host; the command loop needs a PC on Type-C2 |
| ESP-IDF | [`esp-idf/buttons`](esp-idf/buttons) | EVT1: real BOOT GPIO transitions through the UART bridge produced SHORT and LONG events; mechanical switches and PWR still require a physical press |
| ESP-IDF | [`esp-idf/led`](esp-idf/led) | EVT1: steady, breathe and blink effects cycle on schedule; the colours need a visual check |
| ESP-IDF | [`esp-idf/rtc`](esp-idf/rtc) | EVT1: calendar read and a minute alarm fired over the TG28/RX8130CE shared IRQ line |
| ESP-IDF | [`esp-idf/pmic`](esp-idf/pmic) | EVT1: full rail, battery, charger and ADC dump plus one AUDIO_PA off/on/restore |
| ESP-IDF | [`esp-idf/storage`](esp-idf/storage) | EVT1: TF mount, capacity, root listing and a write/read-back/delete probe all passed |
| ESP-IDF | [`esp-idf/display-touch`](esp-idf/display-touch) | EVT1: display and CST820 initialization verified; each press and release is reported once; the physical touch trail still needs a manual check |
| ESP-IDF | [`esp-idf/display-benchmark`](esp-idf/display-benchmark) | EVT1: TE off 45.454/498.500 fps PASS; TE on 29.949/59.903 fps remains BELOW_TARGET under the unchanged strict 30/60 thresholds |
| ESP-IDF | [`esp-idf/audio-recorder`](esp-idf/audio-recorder) | EVT1: 5.0 s / 320000 B WAV captured, saved and closed cleanly; separate PA-toggle microphone-response evidence collected; listening quality not certified |
| ESP-IDF | [`esp-idf/audio-player`](esp-idf/audio-player) | EVT1: 1 kHz tone and all 320000 B of the TF-card WAV played with successful teardown; separate PA-toggle microphone-response evidence collected |
| ESP-IDF | [`esp-idf/wifi`](esp-idf/wifi) | EVT1: STA up and 21 access points scanned; connecting needs a configured SSID |
| ESP-IDF | [`esp-idf/ble`](esp-idf/ble) | EVT1: independent host received 100 board advertisements; active scan completed with 32 distinct peers and no repeated output |
| ESP-IDF | [`esp-idf/usb-host-msc`](esp-idf/usb-host-msc) | EVT1: SanDisk 0781:5581 enumerates and, on a FAT32 volume, mounts at /usb0 and lists its root; the factory `usb_msc_test` writes and verifies on the same drive |

Arduino and PlatformIO examples are intentionally absent. Arduino first needs a released ESP32-S31 core; PlatformIO first needs ESP32-S31 platform, tool, and framework support. Their Candis-S31 board metadata and examples will be added only after the normal public installation path works in CI.

## Rules for examples

- The project must build from its own directory.
- It must follow the normal layout of its framework.
- It must not require files to be copied into an SDK installation.
- Generated build directories and downloaded dependencies are not committed.
- Expected behavior and hardware assumptions belong in the project's README.
- A successful compile must not be described as a hardware test.

Every project listed above is a single-purpose example. Factory diagnostics and recovery images remain under [`firmware/`](../firmware) and are not user examples.

The Board Manager example uses the committed board definition under
[`vendor/esp-board-manager/`](../vendor/esp-board-manager). The advanced board
runtime remains in [`components/candis_s31/`](../components/candis_s31) because
Board Manager describes devices but does not encode this board's complete
power, Type-C, shared-interrupt, camera-clock, or display-transition policy.
