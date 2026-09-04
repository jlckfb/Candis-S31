# Changelog

## 0.5.2

### Bug Fixes

- Renamed the `init_level` field to `default_level` for the **ESP32-S3-BOX-2** LCD read-strobe and backlight GPIO peripherals to match the current peripheral schema.
- Added the missing `<stdlib.h>` and `<string.h>` includes to the **ESP32-S3-BOX-2** setup source so it compiles cleanly.

## 0.5.1

### Modifications

- Renamed **ESP32-S3-Korvo-2L** board directory and board ID from `esp32_s3_korvo2l` to `esp32_s3_korvo_2l` to align with Board Manager naming conventions.
- Updated README board names for ESP32-C5-Spot, ESP32-S3-BOX-2, and ESP32-S3-Korvo-2L.

## 0.5.0 (Initial Release)

### Features

- Initial release with non-official/friend board definitions:
  - ESP-HI (`esp_hi`)
  - ESP32-C5-Spot (`esp32_c5_spot`)
  - ESP32-S3-BOX-2 (`esp32_s3_box_2`)
  - ESP32-S3-Korvo-2L (`esp32_s3_korvo2l`)
