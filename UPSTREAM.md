# Upstream ownership

Candis-S31 support is maintained where users already install their tools. This repository carries product documentation, standalone examples, factory releases, and recovery information; it does not mirror framework ports or maintain private forks.

## Contribution map

| Change | Canonical upstream | Stored here? |
|---|---|---|
| Generic ESP32-S31 target, HAL, driver, build, flash, or debug fix | [ESP-IDF](https://github.com/espressif/esp-idf) or the affected Espressif tool | No |
| Reusable Candis-S31 BSP API and implementation | A dedicated Candis-S31 branch in [ESP-BSP](https://github.com/espressif/esp-bsp); propose upstream after EVT validation and maintainer agreement | No |
| Complete Candis-S31 board description for ESP-IDF | [`espressif/esp_friends_boards`](https://components.espressif.com/components/espressif/esp_friends_boards) in [ESP Board Manager](https://github.com/espressif/esp-board-manager) | No |
| CO5300 LCD controller fix | [`espressif/esp_lcd_co5300`](https://components.espressif.com/components/espressif/esp_lcd_co5300) source in [ESP-IoT-Solution](https://github.com/espressif/esp-iot-solution) | No |
| Reusable CST820 touch driver | `components/lcd_touch/esp_lcd_touch_cst820` in the ESP-BSP contribution, published independently from the board component | No |
| ES8389 audio codec fix | [`espressif/esp_codec_dev`](https://components.espressif.com/components/espressif/esp_codec_dev) | No |
| DVP controller or camera sensor fix | [`esp_video` and `esp_cam_sensor`](https://github.com/espressif/esp-video-components) | No |
| Reusable TG28_SW, RX8130CE, or FUSB303B driver | A standalone component in the ESP-BSP contribution, published independently from the board component | No |
| Arduino board menu entry and pin variant | [Arduino-ESP32](https://github.com/espressif/arduino-esp32), after generic ESP32-S31 core support is released | No |
| PlatformIO board manifest | [PlatformIO Espressif32](https://github.com/platformio/platform-espressif32), after the platform, tools, and selected framework support ESP32-S31 | No |
| HMI, multimedia, or agent application adaptation | The relevant application repository, such as [ESP-Brookesia](https://github.com/espressif/esp-brookesia), [ESP-GMF](https://github.com/espressif/esp-gmf), or [ESP-Claw](https://github.com/espressif/esp-claw) | No |
| Schematic, pinout, bring-up notes, factory release, recovery, and user examples | This repository | Yes |

Board pins, device choices, panel power sequencing, and product behavior do not belong in the ESP-IDF core. ESP-BSP provides reusable C APIs, while `esp_friends_boards/candis_s31` provides Board Manager metadata and generated configuration. They solve different integration problems.

The Candis-S31 BSP is developed in a separate ESP-BSP worktree so Factory firmware can validate the same API shape used by ESP-BSP examples. This local implementation does not imply that an upstream pull request has been accepted. Before proposing the complete BSP, confirm the contribution scope with the maintainers and provide hardware evidence. Generic device drivers remain separate components and must not contain Candis-S31 pins or product policy.

## Board Manager shape

The development board directory is:

```text
esp_friends_boards/candis_s31/
├── board_info.yaml
├── board_peripherals.yaml
├── board_devices.yaml
├── sdkconfig.defaults.board
└── setup_device.c
```

The three YAML files describe board metadata, peripheral buses, and devices. `sdkconfig.defaults.board` contains only settings required by the hardware. `setup_device.c` handles logic that cannot be expressed in YAML, such as the CO5300 panel factory and the board power sequence.

The display uses the Board Manager SPI data lines and `quad_mode`. TG28-controlled rails use a custom `power_ctrl`; the PMIC, RTC, and Type-C controller are custom devices backed by independent reusable drivers. Board-local `packages/` must not hide drivers that should be usable by another board or framework.

## Framework gates

Adding board metadata is the last board-specific step, not the first platform step:

- **ESP-IDF:** the target already exists as a preview target. Candis-S31 needs released components and the `esp_friends_boards` definition, not an IDF fork or IDE plugin.
- **Arduino:** generic ESP32-S31 core support, build packages, and library compatibility must be available before the Candis-S31 board entry and variant are useful.
- **PlatformIO:** the platform, toolchain packages, flashing tools, debugging support, and chosen framework must support ESP32-S31 before `boards/candis-s31.json` can work. PlatformIO Espressif32 is maintained by PlatformIO, not by the Espressif GitHub organization.
- **Camera:** new ESP-IDF work uses `esp_video` and `esp_cam_sensor`. A Candis-S31 DVP pin map is not a reason to add a board definition to the legacy `esp32-camera` component.
- **VS Code and EIM:** both use the selected official ESP-IDF environment. A standard ESP-IDF project does not need a Candis-specific extension.

## Contribution workflow

1. Implement the focused `candis_s31` BSP in a clean ESP-BSP worktree without product factory logic.
2. Build the Factory project against that local BSP and keep every hardware result `NOT_RUN` before EVT1.
3. Confirm each hardware fact against the latest schematic and a physical board.
4. Reproduce generic issues in an unmodified upstream environment.
5. Submit reusable device drivers independently, with English comments, tests, and hardware evidence.
6. Publish the component versions needed by the BSP and board definition.
7. Discuss and submit the ESP-BSP contribution with focused commits and hardware logs.
8. Submit `esp_friends_boards/candis_s31` using only released dependencies and resolved hardware values.
9. After public upstream releases are installable, update the examples, compatibility statements, portable lock files, and CI baseline here.
10. Add Arduino, PlatformIO, and application-framework examples only when their normal public installation path works.

Keep each upstream pull request focused on one independently reviewable concern. Do not combine a board definition, several new device drivers, and framework examples into one cross-repository change. An open pull request is not released support.

## Do not submit

- Candis-S31 pin assignments, product demos, or factory logic to the ESP-IDF core.
- Factory commands, fixture policy, or product-only acceptance rules to ESP-BSP.
- A PlatformIO board JSON before ESP32-S31 platform and framework support exists.
- An Arduino variant presented as complete ESP32-S31 core support.
- Candis-S31 DVP pins to `esp32-camera` as a board definition.
- Board power sequencing, PMIC policy, or confidential supplier material inside a generic device driver.
- Unverified GPIOs, addresses, active levels, register behavior, or initialization tables.

## Local verification

Temporary upstream worktrees may live beside this repository, but they are not nested inside it and are never committed here. Build, flash, monitor, and recovery checks use the existing `idf` tmux session and its isolated official environment without modifying the SDK installation.

During local Bring-up development, `firmware/factory` loads
`esp-bsp/bsp/candis_s31` through `CANDIS_S31_BSP_PATH`. The build records the
BSP Git revision and marks a modified checkout as dirty. No absolute local path
is committed to the project.

As of 2026-07-31 the local verification environment is ESP-IDF `v6.1-beta1`
(commit `b1d13e9f`, checked out at `.tools/esp-idf` in the workspace root, two
levels above this repository) plus the local ESP-BSP worktree (commit
`522c90f1` with uncommitted changes on top). The Factory project compiled and
merged successfully in this combination on 2026-07-31 (merged image about
1.9 MB against the 4 MB factory partition), and the Board Manager definition
in `esp_friends_boards/candis_s31` generated and compiled an isolated ESP-IDF
application. None of this represents a hardware result.

The current compatibility baseline is:

| Item | Version | Validation |
|---|---|---|
| ESP-IDF | `v6.1-beta1` | Getting-started and Factory compile only |
| Local Candis-S31 BSP | `1.1.0` development component | Factory and six upstream ESP-BSP examples compile; hardware not run |
| Local TG28_SW driver | `0.2.0` development component | Standalone component and BSP integration compile; hardware not run |
| Local RX8130CE driver | `0.2.0` development component | Standalone component and BSP integration compile; hardware not run |
| Local FUSB303B driver | `0.1.0` development component | Standalone component and BSP integration compile; hardware not run |
| Local CST820 touch driver | `1.1.0` development component | Standalone component and BSP integration compile; hardware not run |
| Factory Bring-up firmware | Development source | Full peripheral command set compile-tested; no image released |
| Candis-S31 hardware | EVT1 schematic revision 0.5 | Not fabricated |
| ESP Friends Boards definition | `candis_s31` development definition | Board generation and an isolated ESP-IDF application compile; not released and hardware not run |
| Arduino ESP32-S31 core and Candis board | Not released | Not available |
| PlatformIO ESP32-S31 platform and Candis board | Not released | Not available |

Local rows record worktree validation only. Public support is claimed only after the corresponding artifact can be installed and tested through its normal channel. A compile result is not recorded as hardware validation.
