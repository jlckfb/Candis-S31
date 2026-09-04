# ESP Friends Boards

[![Component Registry](https://components.espressif.com/components/espressif/esp_friends_boards/badge.svg)](https://components.espressif.com/components/espressif/esp_friends_boards)

[中文](README_CN.md)

Community, partner, and non-official board definitions for ESP Board Manager.

This component is not a default dependency of `esp_board_manager`. Add it to a project explicitly when these boards are needed:

```yaml
dependencies:
  espressif/esp_friends_boards:
    version: "^0.5.0"
```

This component provides board-level configuration files that can be recognized and used by ESP Board Manager, including board metadata, peripheral and device configuration, and board-level default sdkconfig options. After adding this component, use ESP Board Manager commands to list boards or select a board to generate configuration code.

For more information about ESP Board Manager, see the [`esp_board_manager` component documentation](https://github.com/espressif/esp-board-manager/blob/main/esp_board_manager/README.md).

## Supported Boards

| Board | Chip | Audio | SD Card | LCD | LCD Touch | Camera | Button | LED Strip |
|---|---|---|---|---|---|---|---|---|
| ESP-HI | ESP32-C3 | Built-in ADC + PDM speaker | - | ILI9341 | - | - | GPIO button | - |
| [`ESP32-C5-Spot`](https://oshwhub.com/esp-college/esp-spot) | ESP32-C5 | ES8311 dual | - | - | - | - | - | - |
| ESP32-S3-BOX-2 | ESP32-S3 | ES8389 + ES7210 | SPI | ST7789 | - | - | GPIO button | - |
| ESP32-S3-Korvo-2L | ESP32-S3 | ES8311 | SDMMC | - | - | - | - | - |
| Candis-S31 | ESP32-S31 | ES8389 | SDMMC | CO5300 | CST820 | DVP | - | WS2812 |
