# Changelog

## 0.6.1

### Features

- Added a `custom` sub-type for the `power_ctrl` device, for boards that need a PMIC, IO expander, multiple power rails, or a specific power-up sequence. Board code registers `dev_power_ctrl_custom_ops_t` (`init`/`deinit`/`set_power`) under the `power_ctrl` device's own name via `DEVICE_EXTRA_FUNC_REGISTER`; the framework references/unreferences any peripherals declared under `peripherals:` around the controller's lifecycle, and forwards an optional strongly-typed `config:` block as `sub_cfg.custom.user_cfg`. New Kconfig option: `ESP_BOARD_DEV_POWER_CTRL_SUB_CUSTOM_SUPPORT`.

### Bug Fixes

- Fixed `dev_audio_codec` never applying the configured I2C `frequency` to the codec control interface (`i2c_cfg.clock_speed_hz` was parsed from YAML but never assigned). As part of this fix, the default `frequency` for the codec's I2C peripheral entry was lowered from `400000` to `100000` Hz.
- Fixed `periph_i2s` shared full-duplex channel bookkeeping: TX and RX ownership are now tracked independently, so a TX channel kept enabled purely as an RX clock helper (STD/TDM full-duplex) is only torn down once no consumer still owns either direction. `periph_i2s_deinit()` no longer force-deletes a peer channel that another consumer still owns, and now returns an error if the handle it was given is not a currently-owned channel.
- Fixed a resource leak in `esp_board_device_power_ctrl()`: when it auto-initializes a `power_ctrl` device that was not yet initialized, a later failure (sub_type lookup, dispatch function lookup, or the `set_power` call itself) now deinitializes the auto-initialized power control device instead of leaving it and its referenced peripherals dangling.

### Modifications

- `power_ctrl` `custom` sub-type devices now reject a device name that collides with the framework's internal `<sub_type>_power_ctrl` dispatch key (validated at parse time in `dev_power_ctrl.py`).
- The auto-generated custom-device struct header (`gen_board_device_custom.h`) no longer unconditionally includes `dev_custom.h`.
- Bumped the `espressif/esp_codec_dev` dependency to `~1.6`.
- Rewrote the `esp_board_manager` test app (`test_apps/main`) as an interactive console (`bmgr`/`case` commands) backed by a reusable test-case registry, replacing the previous fixed `app_main` test sequence; CI drives it via `bmgr init` followed by `case run-all`.

## 0.6.0~1

### Bug Fixes

- Fixed SoC capability validation for I2S instance limits so input/output entries sharing the same `config.port` are counted as one hardware I2S instance.
- Added `--skip-soc-capability-check` and `ESP_BOARD_MANAGER_SKIP_SOC_CAPABILITY_CHECK=1` to bypass parser-stage SoC capability validation when needed.

## 0.6.0

### ⚠️ Breaking Changes

- **Boards extracted to standalone components.** All built-in board definitions have been removed from `esp_board_manager/boards/` and are now distributed as independent IDF components. Add the relevant component(s) to your project's `idf_component.yml` to continue using them:
  - `espressif/esp_boards` — official Espressif devkits, already included as a default dependency of `esp_board_manager`
  - `espressif/esp_friends_boards` — partner/friend boards (add manually if needed)
  - `espressif/m5stack_boards` — M5Stack boards (add manually if needed)

- **Board names aligned with official Espressif devkit names.** Several boards have been renamed. Update any `-b` argument or hard-coded board name references:

  | Old name | New name | Component |
  |---|---|---|
  | `esp_box_3` | `esp32_s3_box_3` | `esp_boards` |
  | `esp_box_lite` | `esp32_s3_box_lite` | `esp_boards` |
  | `esp32_p4_function_ev` | `esp32_p4_function_ev_board` | `esp_boards` |
  | `esp32_s3_korvo2_v3` | `esp32_s3_korvo_2_3` | `esp_boards` |
  | `esp32_s3_korvo2l` | `esp32_s3_korvo_2l` | `esp_friends_boards` |
  | `esp32_s31_korvo1` | `esp32_s31_korvo_1` | `esp_boards` |
  | `esp_vocat_board_v1_0` | `esp_vocat_1_0` | `esp_boards` |
  | `esp_vocat_board_v1_2` | `esp_vocat_1_2` | `esp_boards` |
  | `lyrat_mini_v1_1` | `esp32_lyrat_mini_1_1` | `esp_boards` |

- **`dual_eyes_board_v1_0` removed.** This internal prototype board definition has been deleted and is not available in any component.

- **`espressif/esp_video` minimum version bumped to `^2.1.0`** (was `^2.0.0`). Projects using the camera device must update their `esp_video` dependency accordingly.

- **Legacy `dev_lcd_touch_i2c` device removed.** The deprecated `dev_lcd_touch_i2c` device (implementation, parser, `dev_lcd_touch_i2c.h` header, and the `CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT` compatibility symbol) has been removed. Use `type: lcd_touch` with `sub_type: i2c`, the `dev_lcd_touch_config_t` / `dev_lcd_touch_handles_t` types, and `CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT` / `CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUB_I2C_SUPPORT` switches. See the [migration guide](https://docs.espressif.com/projects/esp-board-manager/en/latest/migration/migrate-to-0.6.html).

### ⚠️ Important Changes

- **`SDKCONFIG_DEFAULTS` priority changed.** Board-level defaults now always take precedence over the project's `sdkconfig.defaults`; use amend for board-specific sdkconfig overrides. (This reverses the 0.5.15 behavior where project defaults could override ordinary board defaults.). `SDKCONFIG_DEFAULTS` is now ordered: project `sdkconfig.defaults` (lowest) → `board_manager.defaults` → env `SDKCONFIG_DEFAULTS` / `-D SDKCONFIG_DEFAULTS` (highest).
- **Removed the FatalError** that rejected Board Manager managed symbols in project `sdkconfig.defaults`; it is replaced by a non-blocking warning that points to the board's precedence and recommends amend for board-level overrides.
- Added a SoC capability catalog and validation flow. Board generation now selects the matching SoC capability profile by target chip and active ESP-IDF version, then validates device/peripheral availability and SoC-dependent hardware limits before parser dispatch.
- Added bundled SoC capability catalog snapshots for ESP-IDF v5.4, v5.5, v6.0, v6.1, and v6.2. The catalog is generated from ESP-IDF SoC headers and maintained through `esp_board_soc_requirements.yml`.
- Added GPIO validation before writing `gen_board_metadata.yaml`. Extracted GPIOs are checked against the selected chip catalog, invalid SoC GPIOs are rejected, and duplicate GPIO usage is reported as warnings

### Features

- **Camera (`dev_camera`)**: Adapted to `esp_video` per-camera-type init/deinit APIs, so CSI, DVP, and SPI cameras are initialized and deinitialized independently.
- **Auto-amend**: a directory named after the selected board and containing `board_amend.yaml` (discovered under the same scan paths as boards, including `-c` paths, and not itself a full board directory) is applied automatically, so every board can share the same `idf.py bmgr` command. Explicit `-a/--amend` still works and takes the highest precedence. Disable auto-discovery with `ESP_BOARD_MANAGER_DISABLE_AUTO_AMEND=1`.
- **`-c/--customer-path` now accepts multiple semicolon-separated paths** (for example `-c "overlays_a;overlays_b"`); paths are scanned in order and later paths take precedence.

### Bug Fixes

- Corrected the ESP-IDF version requirements for two configuration fields from v6.0 to v6.0.1. These struct members do not exist in the v6.0.0 release and previously caused a compile error (`has no member named 'flags'`) when building against IDF v6.0.0:
  - `periph_dsi` `flags.clock_lane_force_hs` (`esp_lcd_dsi_bus_config_t`)
  - `display_lcd` I80 `bus_config.flags.allow_pd` (`esp_lcd_i80_bus_config_t`)

## 0.5.15

### ⚠️ Important Changes

- Load generated `board_manager.defaults` before project/user defaults when `sdkconfig` is absent, allowing user defaults to override ordinary board defaults while rejecting user defaults that modify Board Manager managed symbols.

### Bug Fixes

- Corrected the ESP-IDF version requirements for several version-specific configuration fields from v6.0 to v6.1:
  - `display_lcd` SPI `io_spi_config.flags.psram_dma_direct`
  - `lcd_touch` I2C `io_i2c_config.transaction_timeout_ms`
  - `periph_spi` `spi_bus_config.dma_burst_size`

### Features

- Updated **M5Stack Tab5** support to handle ST7121 panel/touch variant through runtime probing and board-specific setup.
- Added board support for **ESP32-LyraT V4.3**, including ES8388 audio codec and SD card.

## 0.5.14

### Features

- Added **littlefs** filesystem device (`type: littlefs`) with `flash`, `sdmmc`, and `spi` sub-types
- Added ESP-IDF v6.0 specific configuration fields. They are emitted only when building against IDF v6.0 or later, and a warning is printed (the field ignored) on older IDF versions:
  - `display_lcd` SPI `io_spi_config.flags.psram_dma_direct`
  - `display_lcd` I80 `bus_config.flags.allow_pd`
  - `lcd_touch` I2C `io_i2c_config.transaction_timeout_ms`
  - `periph_dsi` `flags.clock_lane_force_hs`
  - `periph_spi` `spi_bus_config.dma_burst_size`
- The `display_lcd` `parlio` sub-type now validates that the active ESP-IDF is v5.5 or later

### Modifications

- Updated **M5Stack Tab5** defaults with the ESP32-P4 chip revision options (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, `CONFIG_ESP32P4_REV_MIN_100`)

## 0.5.13

### Bug Fixes

- Fixed same-named board selection priority after the 0.5.12 board source metadata change. Later scanned sources now override earlier ones again, so `-c/--customer-path` boards correctly take priority over component and built-in boards.

## 0.5.12

### Important Changes

- Board scan/list output now keeps source metadata and groups boards by component/customer source, making managed-component and custom-board results easier to distinguish.

### Features

- Support for `depends_on` device dependency configuration. The `depends_on` field allows you to specify one or more prerequisite devices. During initialization, dependencies are initialized automatically. During de-initialization, shared dependencies are kept alive while active devices still rely on them.
- Built-in device sub-type entries are now registered with `type_sub_type` names through `ESP_BOARD_SUBTYPE_ENTRY_IMPLEMENT`, avoiding collisions between common sub-type names such as `gpio`, `spi`, and `i2c`.
- Support short `-a/--amend` paths under the selected board directory, for example `-a sub_board_800_480_lcd`.
- Add support for I80 LCD (`display_lcd` with `sub_type: i80`)
- Add support for custom button (`button` with `sub_type: custom`)
- Added full board support for **ESP32-S3 BOX2**, including GPIO expander, ES8389 audio codec, SPI SD card, I80 ST7789 LCD, and custom buttons.
- Added full board support for **ESP32-P4-EYE**, including PDM microphone, SDMMC SD card, SPI LCD, CSI camera, GPIO buttons, flashlight LED, and power controls.
- Added the `display_lvgl` example for initializing a Board Manager LCD device and running an LVGL test UI.
- Updated **M5Stack Tab5** support to handle both ILI9881C/GT911 and ST7123 panel/touch variants through runtime probing and board-specific setup.

### Bug Fixes

- Reject duplicate device and peripheral names when parsing board YAML, preventing ambiguous generated configs.

## 0.5.11

### ⚠️ Important Changes

- Added `-a/--amend` for board generation. The new mode requires a directory containing a `board_amend.yaml` manifest. Every file that should participate — YAML fragments, C sources, headers, `sdkconfig.defaults.board`, `Kconfig.projbuild` — must be listed explicitly in the `apply:` list. There is **no auto-discovery and no directory items**; the manifest is the single source of truth. Later list entries override earlier ones.
- Added support for `gen_skip` keywords, Configuring `gen_skip` for unused devices/peripherals in the amend file will skip the parsing and generation of those devices/peripherals during the board manager's build phase. Users need to be aware of dependencies and potential compilation issues.
- Added sdkconfig default override marking: when a later defaults source sets the same `CONFIG_` symbol, the earlier active line is commented with `BMGR_CONFIG_OVERRIDE`.

### Features

- Appended sdkconfig defaults in deterministic order: generated Board Manager defaults, base-board `sdkconfig.defaults.board`, then every `apply:` entry whose basename is `sdkconfig.defaults.board` (in manifest order).
- Appended `Kconfig.projbuild` in the same order: generated section, base-board `Kconfig.projbuild`, then every `apply:` entry whose basename is `Kconfig.projbuild`.
- Added schema validation helpers for board device/peripheral YAML fields, including warnings for unsupported fields.
- Added support for **esp32_s31_function_coreboard_1** board, supported devices includes:
  Codec and LED Strip

### Docs

- Updated `README.md` / `README_CN.md` and `docs/how_to_customize_board*.md` for the new amend flow.

### Tests

- Added `test_apps/components/test_amend_features/` (plus an out-of-tree `boards/test_amend_features_extra/`) and `test_amend_features_board.py`, covering happy paths and every documented error case.

## 0.5.10

### ⚠️ Breaking Change

- Added generic **lcd_touch** device support with I2C sub_type, and migrated supported boards from legacy LCD touch definitions. Code depending on the legacy `ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT` macro may be incompatible

### Docs

- Updated README quick start to recommend `esp-bmgr-assist` as the preferred setup flow
- Added `dev_lcd_touch_i2c` to `dev_lcd_touch` migration guides and an optional AI Skill for assisted migration
- Removed the prebuild script and related documentation; it is no longer a recommended workflow

### Features

- Added IDF-version based component source selection for peripherals, and updated RMT/MCPWM implementations for IDF v6.x compatibility
- Added I2C effective address tracking and query API for devices that support address probing or override
- Added **led_strip** device support with RMT and SPI sub_type implementations
- Added **rgb** sub_type support for **display_lcd**
- Added support for **esp32_s31_korvo1** board, supported devices includes:
  Codec, RGB LCD, LCD Touch, LED Strip and Power Control

### Bug Fixes

- Fixed resource cleanup and shared bus reference handling for several devices during init/deinit failure paths
- Fixed IDF v6.x I2S compatibility when the active target already defines `SOC_I2S_NUM`
- Fixed audio codec PA default pin handling by using `-1` when no PA control peripheral is configured
- Fixed PDM RX HP-filter config generation to emit HP-filter fields only when the active chip supports `SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER`

## 0.5.9

### ⚠️ Important Change

- Added a unified board metadata generation flow that produces gen_board_metadata.yaml, consolidating board devices, peripherals, dependencies and IO metadata
- Added runtime device config override APIs: `esp_board_device_override_config()` and `esp_board_device_restore_config()`, and updated **init**, **get_config**, and **callback_register** to use the effective runtime config

### Features

- Extended generated device descriptors to include sub_type
- Added or updated sdkconfig.defaults.board for several boards
- Added support for **esp_hi** board, supported devices includes:
  ADC Mic and PDM Speaker, LCD, GPIO Button
- Added global callback default log info

## 0.5.8

### ⚠️ Important Change

- Add preferred IDF action `idf.py bmgr` with the same options as `gen-bmgr-config`; keep `idf.py gen-bmgr-config` as a legacy alias
- When switching boards, back up the removed `sdkconfig` to `components/gen_bmgr_codes/sdkconfig.bmgr_board.old` instead of the project root
- Clarify the intended role of the YAML **`version`** field in Board Manager (schema / parsing contract vs. other meanings such as Component Manager `dependencies`)
- Emit **warnings** at generation time when an unsupported schema **`version`** tag is present (supported forms documented in README and customization guides)
- Keep `gen_codes/Kconfig.in` static for device/peripheral capability symbols only, and generate the selected board symbols in `components/gen_bmgr_codes/Kconfig.projbuild`
- Reduce maximum recursive scan depth from 10 to 3 across all scan paths, including default components directory, override paths defined in main/idf_component.yml, and custom paths specified via -c

### Features

- Reject mixing `bmgr` and `gen-bmgr-config` in a single `idf.py` invocation; reject board name/index as a stray positional argument (use `-b`)
- Support scanning board roots referenced by `main/idf_component.yml` local `override_path` / `path` entries for board-style dependencies

### Bug Fixes

- Use a resolved project path when deleting cached `sdkconfig` JSON after writing board defaults
- Unify project directory resolution for `idf.py -C <project>` and standalone `--project-dir`, so board scanning, relative `--customer-path`, and `bmgr -n` all resolve against the selected project directory
- Include `dev_fs_fat.h` and `dev_power_ctrl.h` in `esp_board_manager_includes.h` when the matching Kconfig options are enabled
- Add `#include "driver/i2s_common.h"` in `dev_audio_codec` for build issues
- Improve script error handling by exiting immediately when YAML parsing fails

### Docs

- Add **board_config_template** document, reduce the difficulty of adapting development boards, quickly obtain the required configuration

## 0.5.7

### ⚠️ Breaking Change

- Replace the board macro definitions from `CONFIG_BOARD_XXX` to `CONFIG_ESP_BOARD_XXX`

### Bug Fixes

- Fixed an issue where board manager configurations were not correctly reflected in `sdkconfig` in certain use cases, and enhanced the validation mechanism

### Important Changes

- Add sdkconfig consistency check on `-b` command, auto-fix mismatched symbols by default, remain skip flag `--skip-sdkconfig-check`
- Add sdkconfig consistency check on global callback, raise warning messages while mismatches occurred, use environment variable `ESP_BOARD_MANAGER_SKIP_SDKCONFIG_CHECK` to skip
- Additionally generate devices and peripherals symbols to `board_manager.defaults`
- Avoid `Kconfig.in` modifications, put the macro definitions of custom boards into `gen_bmgr_codes/Kconfig.projbuild` files

### Features

- Split the `adc_single` and `adc_multi` of the button in the file, keeping it consistent with other devices
- Support ADC Mic and PA-only speaker in `dev_audio_codec`
- Added support for `parlio` sub_type in `dev_display_lcd`
- Added support for `spi` sub_type in `dev_camera`

## 0.5.6

### Modifications

- Upgrade dependency `esp_video` to v2.0.0

## 0.5.5

### Modifications

- Rename `echoear_core_board_v1_0/v1_2` to `esp_vocat_board_v1_0/v1_2`

## 0.5.4

### Bug Fixes

- Fixed a compilation issue caused by default `Kconfig.in`: when re-pulling the component and compiling, the default `Kconfig.in` may cause sdkconfig to be regenerated
- Simplified the script's `root_dir` selection logic, using the script path directly as `root_dir` to avoid potential issues that may arise from specifying multiple IDF_EXTRA_ACTIONS_PATHs

### Features

- Optimize sdkconfig handling: preserve sdkconfig if the target board matches the current board to avoid unnecessary re-configuration
- Clarify that the naming of the development board is based on the folder name, and add name detection to ensure that the name in `board_info.yaml` is consistent with the folder name after the board is determined
- Update csi camera compatibility, support configuring xclk, add ldo periph dependency, automatically initialize ldo when initializing the camera
- Optimize the custom device parsing script to support parsing nested configurations
- Add `board_name` validation, only allow letters (a-z, A-Z), numbers (0-9), and underscores (_). If the name does not comply, this board will be unavailable.

### Modifications

- Reduce unnecessary configurations and files: delete the Kconfig file under the board path, and delete the board and chip fields in `board_devices.yaml` and `board_peripherals.yaml`
- Optimized the scanning logic of the development board, and instead of using the Kconfig file to determine whether it is the development board path, it scans three files: `board_devices.yaml`, `board_info.yaml`, and `board_peripherals.yaml`

## 0.5.3

### Features

- Added [`play_embed_music`](examples/play_embed_music/README.md) example to show how to use board manager to initialize codec and play flash wav audio file
- Added support for M5STACK TAB5, supported devices includes:
  Codec, LCD, LCD Touch, Camera and SD card
- Added support for ESP_BOX_LITE, supported devices includes:
  Codec and LCD
- Adapt to the new version of `esp_io_expander` component by adding support for configuring IO output modes and enabling pull-up/pull-down resistors

### Bug Fixes

- Modified the effective scope of the `board_manager_global_callback` function to ensure that build-related operations can correctly include `board_manager.defaults`
- Fixed an issue where the `pa_pin` configuration of ES8156 did not take effect correctly
- Fixed ledc timer parsing script error for `timer_sel`

### Modifications

- Change the output path of `board_manager.defaults` to `components/gen_bmgr_config/` to exclude it from git diffs

## 0.5.2

### Bug Fixes

- Fixed an issue where the dev_display_lcd mirror_x/y and swap_xy configurations were not taking effect correctly
- Fixed compilation issue when the `sdkconfig.defaults` file not exists

## 0.5.1

### ⚠️ Important Changes

- Changed board defaults to use `board_manager.defaults` file instead of appending to `sdkconfig.defaults`. Board-specific configurations are now automatically applied via `SDKCONFIG_DEFAULTS` environment variable during build/menuconfig/reconfigure.
- Changed backup file from `sdkconfig.bmgr_board.backup` to `sdkconfig.bmgr_board.old`
- Use `dev_display_lcd` to replace `dev_display_lcd_spi` in esp_vocat_board_v1_1/v1_2, esp32_s3_korvo2_v3, esp_box_3, m5stack_cores3, dual_eyes_board_v1_0
- Use `dev_fs_fat` to replace `dev_fatfs_sdcard` and `dev_fatfs_sdcard_spi` in esp_vocat_board_v1_1/v1_2, esp32_s3_korvo2_v3, esp_box_3, m5stack_cores3, esp32_p4_function_ev, lyrat_mini_v1_1
- Add `dev_power_ctrl` device support for esp_vocat_board_v1_1/v1_2, replace `dev_gpio_ctrl` used to apply power
- Removed support for `dev_display_lcd`, `dev_fatfs_sdcard`, `dev_fatfs_sdcard_spi` and related test code

### New Script

- Add a new script `create_board.py` to simplify the process of creating a new board.
  This script simplifies the process of copying configurations from different peripheral and device directories when users add new boards, provides an interactive new board creation process, allowing sequential selection of chip, device, and peripherals. After passing the peripheral dependency check, the script automatically creates four configuration files required for the board.
  - Usage:

    ```python
    # Create a new board in default path: components/<board_name>
    idf.py gen-bmgr-config -n <board_name>

    # Create a new board in specific path:
    idf.py gen-bmgr-config -n path/to/board/<board_name>
    ```

  - Note: It is recommended not to run create_board.py directly as there may be path issues.

### Features

- Added prebuild script for convenient compilation
- Added global callback to auto-inject board defaults via SDKCONFIG_DEFAULTS mechanism
- Supported `-D SDKCONFIG_DEFAULTS=xxx` parameter with automatic merging

### Modifications

- Added support for IDF v6.x (trial version)
  - Added adapter for periph_i2s
  - Added warning for periph_rmt, periph_pcnt, periph_mcpwm, dev_display_lcd_sub_dsi (not supported by IDF v6.x yet)
- Refine sub device support for dev_button, splitting the sub_type `adc` into `adc_single` and `adc_multi`, splitting the `multi_click` configuration into `multi_click` and `click_counts`. Keep the enabling of `multi_click` and the specific number of clicks as two separate configuration items, consistent with the configuration approach used for `long_press_start` and `long_press_up`
- Modify CMakeLists.txt to fix the driver/xxx.h header file undecleared issue in IDF v6.x
- Renamed `apply_board_sdkconfig_defaults` to `generate_board_manager_defaults` for clarity
- Removed legacy methods for sdkconfig.defaults manipulation

### Bug Fixes

- Fixed an issue that find_project_root finding unrelated projects
- Fixed an issue where `CMakeLists.txt` generated on the Windows platform would cause compilation errors due to the `\` in paths

## 0.5.0

### ⚠️ Important Changes

- Board Switching Now Deletes `sdkconfig`: When switching boards, the script automatically backs up `sdkconfig` to `sdkconfig.bmgr_board.backup` and removes the original to prevent configuration pollution. Additionally, board-specific configurations from `boards/<board_name>/sdkconfig.defaults.board` are automatically appended to your project's `sdkconfig.defaults`.
- Simplified board selection - board name/index as direct parameter (gen_bmgr_config_codes.py only)
- Removed automatic sdkconfig modification (breaking change)

### Features

- Added `-x, --clean` command to clean generated files and reset CMakeLists.txt and idf_component.yml
- Enhanced board selection - board name/index as direct parameter (gen_bmgr_config_codes.py only)
- Added comprehensive test suite with Python pytest
- Added 7 test boards for testing coverage
- Enhanced board scanning mechanism for better discovery
- Enhanced board parameter handling in test cases
- Added support for `sdkconfig.defaults.board` file in each board directory. When switching boards, the script automatically appends board-specific configurations to the project's `sdkconfig.defaults` file.
- Added automatic `sdkconfig` backup to `sdkconfig.bmgr_board.backup` when switching boards
- Implement sub_type field for device configurations, add hierarchical Kconfig generation (ESP_BOARD_DEV_SUB<SUB_TYPE>_SUPPORT)
- Add unified entry registration system using linker sections
- Implement device extra function registration interface `EXTRA_FUNC_IMPLEMENT`
- Devices type support:
  - Create FS_FAT device with SDMMC/SPI sub-type implementations, `dev_fs_sdcard` and `dev_fs_sdcard_spi` while be deprecated in feature version
  - Create DISPLAY_LCD device with SPI/DSI sub-type implementations, `dev_display_lcd_spi` while be deprecated in feature version
  - Create BUTTON device with GPIO/ADC sub-type implementations
  - Create CAMERA device with DVP/CSI sub-type implementations
  - Create POWER_CTRL device, added `power_ctrl_device` attribute in `esp_board_device_desc_t`, for devices configured with this attribute, the power will be automatically turned on during initialization
- Board support:
  - Added lcd, camera support for `esp32_p4_function_ev` board
- Peripheral type support:
  - UART, ADC, RMT, PCNT, DAC, SDM, MCPWM, ANACMPR, LDO and DSI.
- Added [`play_sdcard_music`](example/play_sdcard_music/README.md) example to show how to use board manager to initialize codec and play wav audio files
- Added [`record_to_sdcard`](example/record_to_sdcard/README.md) example to show how to use board manager to initialize codec and record wav audio files

### Modifications

- Added `clk_src` configuration for `dev_audio_codec`
- Reorganized the peripherals directory structure
- Modify the periph role macro definition from string to enum
- Removed the unused periph type macro definition and added the commonly used periph name macro definition

### Bug Fixes

- Fixed pa active_level configuration for `esp32_p4_function_ev` board

## 0.4.8

### Bug Fixes

- Fixed the issue where the configuration of the periph_ledc was not correctly obtained during the initialization of dev_ledc_ctrl
- Added `need_reset` configuration to control whether to reset lcd during the initialization of `display_lcd_spi`
- Modified the default configuration of esp_vocat_board_v1_2 to prevent screen flickering during initialization
- Fixed incorrect PA gain assigned for codec_device

### Features

- Added gpio_led support for esp_vocat_board_v1_2

## 0.4.7

### Features

- Added board support for M5STACK CORES3
  - Complete YAML configuration files (board_info.yaml, board_peripherals.yaml, board_devices.yaml)
  - Support for AW88298 DAC, ES7210 ADC, ILI9342 LCD, FT5X06 LCD TOUCH, AW9523B GPIO EXPANDER, AXP2101 PMU and SD card devices
  - I2C, I2S, SPI and GPIO peripherals configuration
- Supported configuring invert_color during lcd_spi initialization

### Bug Fixes

- Fixed the issue where the codec's gain configuration was not taking effect correctly
- Fixed the issue with incorrect usage of handle for sdcard and sdcard_spi
- Modified the GPIO expander and customer device test cases to make them more generic
- Fixed the test app to allow sdcard_spi to also function properly in codec testing

## 0.4.6

### Bug Fixes

- Fixed the SD card mount failure issue

## 0.4.5

### Bug Fixes

- Fix `periph_i2s_deinit` channel delete
- Fix i2s config `total_slot` for esp32_s3_korvo2_v3

## 0.4.4

### Bug Fixes

- Delete build/config/sdkconfig.json after updating sdkconfig to ensure component manager uses the latest configuration

## v0.4.3

### Bug Fixes

- Reduce the default spi max_transfer_sz value for esp_vocat_board_v1_2 to avoid allocating excessive memory

## v0.4.2

### Features

- Added full board support for ESP32 Lyrat Mini V1.1
  - Complete YAML configuration files (board_info.yaml, board_peripherals.yaml, board_devices.yaml)
  - Support for ES8311 DAC, ES7243E ADC, and SD card devices
  - I2C, I2S, and GPIO peripherals configuration
- Enhanced I2S peripheral support with improved configuration handling
- Added `board_utils.py` helper functions for board configuration

### Bug Fixes

- Fixed build errors caused by incorrect use of `extern "C"` guards in `esp_board_manager_includes.h`
- Removed unused dependency on `test_apps`
- Updated audio codec device implementation for better compatibility
- Appended `esp_board_manager_defs.h` to `esp_board_manager_includes.h` to ensure all necessary header files are included

## v0.4.1

### Features

- Renamed esp_board_manager_ext.py to idf_ext.py for compatibility with ESP-IDF v6.0 auto-discovery
- Update `Kconfig.in` with new configuration options to hide the board selection menu
- Update the README to include information about how to add new boards and the development roadmap
- Added the esp_board_periph_ref_handle and esp_board_periph_unref_handle APIs to obtain and release a handle
- Added esp_board_find_utils.h and esp_board_manager_includes.h
- Added `esp_board_device_get_config_by_handle()` API for finding device configuration by handle

### Bug Fixes

- Delete only `build/CMakeCache.txt` to avoid a full clean when switching boards
- Fixed the configuration of the sdmmc sdcard host slot, ensuring the configuration for the host slot in the yml file takes effect correctly
- Using slot 0 to drive the sdcard for p4_function board, avoiding conflict with wifi hosted
- Correct the error codes

## v0.4.0

### Features

- Add `init_skip` field to control device auto-initialization
- Add custom device type with test support
- Add gpio expander device type with test support
- Add camera device type with test support, support dvp bus type now
- Instead of modifying idf_component.yml, the board manager uses a Kconfig option to handle dynamic dependencies
- Add `adc_channel_labels` field to audio codec for better channel identification
- Add board selection priority documentation and sdkconfig troubleshooting guide
- Instead of copying board source files to gen_bmgr_codes, now reference them directly via SRC_DIRS and INCLUDE_DIRS to avoid duplication.
- Add device dependencies support with `${BOARD_PATH}` variable resolution
- Add sdspi sdcard device type
- Add `esp_board_device_find_by_handle()` API for finding device by handle
- Add board information output to CMakeLists.txt generation
- Update esp_codec_dev dependency to version `~1.5`
- Refine dependency for esp_codec_dev
- Update README with dependencies usage guide
- Update `CONFIG_IDF_TARGET` in sdkconfig according to the chip name specified in `board_info.yaml`
- Clear the build directory to get the correct dependencies when switching boards

### Supported Boards

- **ESP32-S3 Korvo2 V3**: Full LCD, LCD Touch, DVP Camera support
- **ESP32-P4 Function-EV**: Codec, SD card supported

## v0.3.0 (Initial Release)

### Core Features

- **ESP Board Manager**: Initial release of comprehensive board configuration management system
- **YAML-based Configuration**: Support for `board_peripherals.yaml` and `board_devices.yaml` configuration files
- **Automatic Code Generation**: Modular code generation system with 8-step streamlined process
- **Multi-path Board Scanning**: Support for Default, Customer, and Components board paths
- **Automatic Dependency Management**: Smart component dependency detection and `idf_component.yml` updates
- **SDK Configuration Automation**: Automatic ESP-IDF feature enabling based on board requirements

### Supported Boards

- **ESP-Vocat Board V1.0**: Full audio, LCD, touch, and SD card support
- **ESP-BOX-3**: ESP32-S3 development board with I2C, I2S, SPI, LEDC, and GPIO support
- **Dual Eyes Board V1.0**: Dual LCD display with touch support
- **ESP32-S3 Korvo2 V3**: Full audio and SD card support
- **ESP32-S3 Korvo2L**: Full audio and SD card support
- **ESP32 Lyrat Mini V1.1**: YAML configurations support
- **ESP32-C5 Spot**: YAML configurations support

### Peripheral Support

- **I2C**: Full support with type safety
- **I2S**: Complete audio interface support
- **SPI**: Full peripheral and device support
- **LEDC**: PWM and LED control support
- **GPIO**: GPIO support

### Device Support

- **Audio Codecs**: ES8311, ES7210, and other audio codec devices
- **SD Cards**: FATFS filesystem support
- **LCD Displays**: SPI LCD with LVGL integration
- **Touch Input**: I2C touch controller support
- **SPIFFS Filesystem**: Embedded filesystem support
- **LEDC Control**: LED brightness and PWM control
- **GPIO Control**: GPIO output control

### APP Interfaces

- **Board Manager API**: `esp_board_manager_init()`, `esp_board_manager_deinit()`, `esp_board_manager_print()`
- **Peripheral API**: `esp_board_manager_get_periph_config()`, `esp_board_manager_get_periph_handle()`
- **Device API**: `esp_board_manager_get_device_config()`, `esp_board_manager_get_device_handle()`
- **Error Handling**: Comprehensive error codes and macros (`ESP_BOARD_RETURN_ON_ERROR`)

### Documentation & Testing

- **Comprehensive Documentation**: README, API documentation, and configuration guides
- **Test Applications**: Complete test suite for all peripherals and devices
- **Configuration Rules**: Detailed YAML configuration rules and best practices
