# Examples

Every directory listed here is a standalone project. Open the project directory itself; the repository
root is not a build target.

| Project | Purpose |
|---|---|
| [`esp-idf/getting-started`](esp-idf/getting-started) | Toolchain and target smoke test: SoC, flash size and heap |
| [`esp-idf/display-hello`](esp-idf/display-hello) | Minimal AMOLED startup through the committed Board Manager definition |
| [`esp-idf/display-touch`](esp-idf/display-touch) | Cross marker on the AMOLED with live touch coordinates |
| [`esp-idf/display-benchmark`](esp-idf/display-benchmark) | Full-screen and partial refresh throughput |
| [`esp-idf/camera-test`](esp-idf/camera-test) | OV5640 DVP capture with a live 460 x 460 AMOLED preview |
| [`esp-idf/player`](esp-idf/player) | TF-card audio and video player on the ESP-GMF pipeline |
| [`esp-idf/audio-recorder`](esp-idf/audio-recorder) | Microphone capture to a WAV file on the TF card |
| [`esp-idf/audio-player`](esp-idf/audio-player) | Sine tone generation and WAV playback |
| [`esp-idf/storage`](esp-idf/storage) | TF card mount, capacity, directory listing and file read/write check |
| [`esp-idf/usb-host-msc`](esp-idf/usb-host-msc) | Type-C2 host mode mounting a USB mass-storage device |
| [`esp-idf/usb-cdc-device`](esp-idf/usb-cdc-device) | Type-C2 native USB CDC device console |
| [`esp-idf/led`](esp-idf/led) | WS2812B RGB LED steady, breathe and blink effects |
| [`esp-idf/buttons`](esp-idf/buttons) | BOOT, PWR and RTC alarm button events |
| [`esp-idf/rtc`](esp-idf/rtc) | RX8130CE time keeping and minute alarm over the shared interrupt |
| [`esp-idf/pmic`](esp-idf/pmic) | TG28 rail, battery, charger and ADC dump with speaker amplifier control |
| [`esp-idf/power-cycle`](esp-idf/power-cycle) | Power state cycling and battery domain operation |
| [`esp-idf/low-power`](esp-idf/low-power) | Light sleep, deep sleep and screen-off state machine |
| [`esp-idf/wifi`](esp-idf/wifi) | Wi-Fi station scan and connection |
| [`esp-idf/ble`](esp-idf/ble) | NimBLE advertising and scanning |

Factory firmware and the recovery procedure live under [`firmware/`](../firmware) and are not user
examples.

## Rules for examples

- The project must build from its own directory.
- It must follow the normal layout of its framework.
- It must not require files to be copied into an SDK installation.
- Generated build directories and downloaded dependencies are not committed.
- Expected behaviour and hardware assumptions belong in the project's README.

The Board Manager example uses the committed board definition under
[`vendor/esp-board-manager/`](../vendor/esp-board-manager). The advanced board runtime lives in
[`components/candis_s31/`](../components/candis_s31) because Board Manager describes devices but does not
encode this board's power, Type-C, shared-interrupt, camera-clock or display-transition policy.
