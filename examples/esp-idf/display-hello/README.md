# Board Manager display hello

This small example exercises the Candis-S31 definition in the local
`vendor/esp-board-manager/esp_friends_boards` snapshot. It initializes the
board through ESP Board Manager, registers the CO5300 panel with the official
`esp_lvgl_adapter`, and renders one LVGL label.

The generated files under `components/gen_bmgr_codes/` are committed so a
plain clone builds with only the official ESP-IDF environment. They are
regenerated from the vendored board YAML when the board definition changes.

## Build

From the repository root:

```bash
cd examples/esp-idf/display-hello
idf.py --preview set-target esp32s31
idf.py --preview build
```

The project manifest maps `esp_board_manager`, `esp_friends_boards`, and the
four unreleased Candis drivers to repository-local paths. Other dependencies
are resolved from the ESP Component Registry and pinned in `dependencies.lock`.
Do not edit the lock file by hand.

To regenerate the board configuration after editing the YAML snapshot, load
the local Board Manager action and run:

```bash
export IDF_EXTRA_ACTIONS_PATH="$PWD/../../../vendor/esp-board-manager/esp_board_manager"
idf.py --preview bmgr -b candis_s31
```

After regeneration, retain the ownership-normalization block in
`components/gen_bmgr_codes/CMakeLists.txt`. It copies the selected
`setup_device.c` into the generated component's build directory, avoiding
ESP-IDF v6.1 cross-component source warnings without modifying Board Manager.

The board is mounted upside down. The Candis definition therefore enables
both CO5300 mirror axes and both CST820 touch mirror axes. This example uses
QSPI at 48 MHz, a 48-line internal-RAM partial buffer, and the required 2x2
window rounding. GPIO16 TE synchronization and the measured 30 fps full-screen
/ 60 fps local-update paths remain in the repository-owned product Demo; this
static Board Manager hello screen is not a performance test.

Runtime initialization and visual output still require a connected Candis-S31
board. A successful compile is not hardware validation.
