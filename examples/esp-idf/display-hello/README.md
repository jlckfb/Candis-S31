# Board Manager display hello

This small example exercises the Candis-S31 definition in the local
`vendor/esp-board-manager/esp_friends_boards` snapshot. It initializes only
`display_lcd` through the standard `esp_board_manager_init_device_by_name`
API, registers the CO5300 panel with the official `esp_lvgl_adapter`, and
renders one LVGL label. The power controller acquires its peripheral
references lazily; audio, storage, camera and touch devices are not started.

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
window rounding. GPIO16 TE synchronization and the 30 fps full-screen / 60 fps
local-update targets are covered by the repository-owned product Demo and
[display benchmark](../display-benchmark/README.md). The latest TE-enabled
measurements remain below those strict targets; this static Board Manager
hello screen is not a performance test.

The final-source EVT1 run (2026-09-10) initialized only `display_lcd` and
reported `460x460, QSPI 48MHz, MX|MY 180deg` at 1.670 s. There was no audio
device initialization or codec-probe failure. This verifies the intended
display-only lifecycle; panel appearance still needs a visual check.

The full board definition retains both audio devices for applications that
need them. Its power callback probes the ES8389 at the correct **7-bit 0x10**
after ALDO3 rises; the audio YAML retains the codec driver's **8-bit 0x20**
address convention. The separate real-board probe initialized and released
DAC-first/ADC-second, then ADC-first/DAC-second after power teardown;
both rounds returned `ESP_OK`. This is codec initialization evidence, not
Board Manager PCM playback/recording or a listening test.
