# Upstream ownership

Candis-S31 support is maintained in this repository and is split by the
boundary of each upstream project. The repository does not depend on an
ESP-BSP checkout. The official answer in [espressif/esp-bsp#823](https://github.com/espressif/esp-bsp/pull/823)
confirms that ESP-BSP currently maintains only Espressif and M5Stack boards;
third-party boards should remain in their own repository or the vendor's
repository.

## Contribution map

| Change | Canonical upstream | Stored here? |
|---|---|---|
| Generic ESP32-S31 target, HAL, driver, build, flash, or debug fix | [ESP-IDF](https://github.com/espressif/esp-idf) or the affected Espressif tool | No |
| Candis-S31 board runtime, pin map, power policy, and product integration | This repository, [`components/candis_s31/`](components/candis_s31) | Yes |
| Board-local LVGL compatibility and TE diagnostic integration | This repository, [`components/esp_lvgl_port/`](components/esp_lvgl_port) | Yes |
| Candis-S31 board description expressible with safe Board Manager semantics | [`espressif/esp_friends_boards`](https://components.espressif.com/components/espressif/esp_friends_boards) in [ESP Board Manager](https://github.com/espressif/esp-board-manager) | Staged in `vendor/esp-board-manager/` |
| Reusable CST820, TG28_SW, RX8130CE, or FUSB303B driver | [`espressif/idf-extra-components`](https://github.com/espressif/idf-extra-components) | Staged in `vendor/idf-extra-components/` |
| CO5300 LCD controller fix | [`espressif/esp_lcd_co5300`](https://components.espressif.com/components/espressif/esp_lcd_co5300) source in [ESP-IoT-Solution](https://github.com/espressif/esp-iot-solution) | No |
| ES8389 audio codec fix | [`espressif/esp_codec_dev`](https://components.espressif.com/components/espressif/esp_codec_dev) | No |
| DVP controller or camera sensor fix | [`esp_video` and `esp_cam_sensor`](https://github.com/espressif/esp-video-components) | No |
| Arduino board menu entry and pin variant | [Arduino-ESP32](https://github.com/espressif/arduino-esp32), after generic ESP32-S31 core support is released | No |
| PlatformIO board manifest | [PlatformIO Espressif32](https://github.com/platformio/platform-espressif32), after the platform, tools, and selected framework support ESP32-S31 | No |
| HMI, multimedia, or agent application adaptation | The relevant application repository, such as [ESP-Brookesia](https://github.com/espressif/esp-brookesia), [ESP-GMF](https://github.com/espressif/esp-gmf), or [ESP-Claw](https://github.com/espressif/esp-claw) | No |
| Schematic, pinout, bring-up notes, factory release, recovery, and user examples | This repository | Yes |

The board runtime is intentionally first-party. It contains the behavior that
cannot be represented safely by a generic device component: charge-current
policy, shared PMIC/RTC interrupt servicing, Type-C Source-before-boost
sequencing, camera LEDC clock workaround, display sleep transactions, and
LVGL/TE performance settings. The reusable drivers do not contain Candis-S31
pins or product policy.

## Board Manager shape

The staged board package keeps the declarative description in:

```text
vendor/esp-board-manager/esp_friends_boards/candis_s31/
├── board_info.yaml
├── board_peripherals.yaml
├── board_devices.yaml
├── sdkconfig.defaults.board
└── setup_device.c
```

The Board Manager definition describes buses and generic devices. It
intentionally omits the Type-C controller and OTG GPIO because its current
model cannot atomically enforce this board's Source-before-boost and 500 mA
source policy. The advanced firmware therefore uses the repository-owned board
runtime; [`examples/esp-idf/display-hello`](examples/esp-idf/display-hello)
exercises the Board Manager path directly with committed generated output.

`display-hello/components/gen_bmgr_codes/` is generated from the staged YAML
with `idf.py bmgr -b candis_s31`, but the generated files are committed so a
plain clone does not require an extra Python package or action-extension setup.

## Framework gates

Adding board metadata is the last board-specific step, not the first platform
step:

- **ESP-IDF:** the target already exists as a preview target. Candis-S31 uses
a standard ESP-IDF project, the local board runtime, released public
components, and the staged Board Manager definition; no IDF fork is needed.
- **Arduino:** generic ESP32-S31 core support, build packages, and library
compatibility must be available before a board entry and variant are useful.
- **PlatformIO:** the platform, toolchain packages, flashing tools, debugging
support, and chosen framework must support ESP32-S31 before a board manifest can
work.
- **Camera:** new ESP-IDF work uses `esp_video` and `esp_cam_sensor`. A
Candis-S31 DVP pin map is not a reason to add a board definition to legacy
camera components.
- **VS Code and EIM:** both use the selected official ESP-IDF environment. A
Candis-specific extension is not required for the firmware projects.

## Contribution workflow

1. Maintain the board runtime and product examples in this repository.
2. Keep generic device drivers independent of Candis-S31 pins and policy.
3. Validate each driver with its local example and test app under
   `vendor/idf-extra-components/`.
4. Validate `esp_friends_boards/candis_s31` with the local Board Manager
   generator and the `display-hello` project.
5. Confirm every hardware fact against the latest schematic and a physical
   board before publishing a claim.
6. Submit reusable drivers to `idf-extra-components` independently when the
   package metadata and hardware evidence are ready.
7. Submit `esp_friends_boards/candis_s31` independently using only released
   dependency versions and resolved hardware values.
8. After public releases are installable, update manifests, lock files,
   compatibility statements, and CI to use the normal registry path.
9. Add Arduino, PlatformIO, and application-framework examples only when their
   normal public installation path works.

Keep each upstream pull request focused on one independently reviewable
concern. An open pull request is not released support.

## Do not submit

- Candis-S31 pin assignments, product demos, or factory logic to the ESP-IDF core.
- Factory commands, fixture policy, or product-only acceptance rules to generic
  driver components or Board Manager core.
- A PlatformIO board JSON before ESP32-S31 platform and framework support exists.
- An Arduino variant presented as complete ESP32-S31 core support.
- Candis-S31 DVP pins to a legacy camera component as a board definition.
- Board power sequencing or Type-C policy inside a generic device driver.
- Unverified GPIOs, addresses, active levels, register behavior, or
  initialization tables.

## Local verification

The firmware projects build against `components/candis_s31/` through
`cmake/candis_components.cmake`. Its manifest maps the four staged reusable
drivers to `vendor/idf-extra-components/` with relative `override_path`
entries. The display example maps both Board Manager packages and the same
drivers to local vendor paths.

Local note (2026-09-05): the standalone example set now covers
`camera-test`, `power-cycle`, and `usb-cdc-device` at their new
`examples/esp-idf/` locations, plus twelve feature examples (buttons, led,
rtc, pmic, storage, display-touch, display-benchmark, audio-recorder,
audio-player, wifi, ble, usb-host-msc) extracted from the watch demo; the
upstream ownership table and release status below are unchanged.

The current compatibility baseline is:

| Item | Version | Validation |
|---|---|---|
| ESP-IDF | `v6.1-rc1` | Targeted and matrix builds run with `esp32s31 --preview` |
| Candis-S31 board runtime | `1.2.0` (repository component) | Display path retains 180-degree mirror, TE sync, and 48 MHz QSPI settings |
| Board-local LVGL compatibility port | `2.9.0` | TE observer and display diagnostics compile |
| TG28_SW driver | `0.4.0` | Local component test app |
| RX8130CE driver | `0.4.0` | Local component test app |
| FUSB303B driver | `0.3.0` | Local component test app |
| CST820 touch driver | `1.2.0` | Local component test app |
| ESP Friends Boards definition | `candis_s31` | Local generation and `display-hello` build |
| Arduino ESP32-S31 core and Candis board | Not released | Not available |
| PlatformIO ESP32-S31 platform and Candis board | Not released | Not available |

The repository records local snapshot validation only. Public support is
claimed after the corresponding artifact can be installed and tested through
its normal channel. Hardware limitations remain documented in `BETA.md` and
`hardware/bring-up.md`.

## Vendor snapshot baseline and upstream drift

Locked reproducible baseline (2026-09): the staged Board Manager packages in
`vendor/esp-board-manager/` are `esp_board_manager` `0.6.1` and
`esp_friends_boards` `0.5.2` (snapshot: LeenixP/esp-board-manager local
worktree, commit `202225d`, synced 2026-09-04, zero dirty files). The
`display-hello` example uses this pair; advanced firmware uses the separate
repository-owned board runtime.

At this review, official `main` declares
[`esp_board_manager` `0.7.2`](https://github.com/espressif/esp-board-manager/blob/main/esp_board_manager/idf_component.yml)
and [`esp_friends_boards` `0.6.1`](https://github.com/espressif/esp-board-manager/blob/main/esp_friends_boards/idf_component.yml),
with two breaking migrations in between:

- `esp_friends_boards` `0.5.3` migrated friend-board audio codec
  configurations to the `esp_codec_dev` 2.0 initialization layout
  ([0.5.3 changelog](https://components.espressif.com/components/espressif/esp_friends_boards/versions/0.5.3/changelog));
- `esp_friends_boards` `0.6.0` and later board packs use role-specific
  peripheral selectors (`i2c_name`, `spi_name`, `pa_name`, `reset_name`,
  ...) and require `esp_board_manager` `>= 0.7.1`
  ([official migration guide](https://docs.espressif.com/projects/esp-board-manager/en/latest/migration/migrate-to-0.7.html)).

Upstream pull requests prepared from this snapshot therefore need a separate
adaptation, `idf.py bmgr` regeneration, and validation pass against the
newer versions. This repository does not silently upgrade the snapshot;
those passes belong to each future contribution, not to this one.

## Upstream acceptance gates

The submission targets in [Contribution workflow](#contribution-workflow)
  stay as written. Before any release, acceptance must be confirmed with the
  receiving maintainers: which repository hosts the component and under
  which namespace. There is no open issue or pull request for this
  snapshot, and no namespace has been chosen unilaterally. The earlier
  `esp-bsp#823`, `idf-extra-components#824`, and
  `esp-board-manager#7` pull requests are closed; they did not establish
  acceptance or maintenance ownership.
- The [`idf-extra-components` README](https://github.com/espressif/idf-extra-components#adding-new-components)
  states that the repository "is intended for components maintained by
  Espressif developers" and asks external authors to publish their
  components from a separate repository. A driver submission there is
  therefore not guaranteed and must be confirmed with the maintainers.
- In [espressif/esp-bsp#823](https://github.com/espressif/esp-bsp/pull/823),
  the maintainer suggested `idf-extra-components` as another location for
  reusable drivers. The author offered to move FUSB303B if Espressif would
  accept and maintain it. The final reply limits `esp-bsp` to Espressif and
  M5Stack boards; it does not commit another team to maintaining the driver.
