# Factory Bring-up Firmware

This project is the controlled diagnostic application used while bringing up
Candis-S31 hardware. It is not a product demo. Tests are run individually from
the serial console so an unverified peripheral is never enabled as a side
effect of booting the firmware.

> EVT1 has not been fabricated. The project compiles with ESP-IDF `v6.1-beta1`,
> but every electrical and peripheral result remains **NOT RUN** until it is
> recorded on a physical board.

## Design

The application and the board implementation are deliberately separate:

- this directory owns test order, result tracking, console commands, and the
  eventual factory image;
- the local `candis_s31` component in an ESP-BSP checkout owns the pin map,
  buses, power controls, and reusable board APIs;
- reusable chip drivers remain independent components instead of being copied
  into either project.

The firmware requires `CANDIS_S31_BSP_PATH` at configure time. No personal
absolute path is stored in the repository.

## Current command set

| Command | Purpose | Writes board state? |
|---|---|---|
| `help` | List commands and their usage | No |
| `board_info` | Print chip, firmware, BSP, flash, PSRAM, and reset information | No |
| `safe_state` | Reapply disabled levels to direct power controls | Yes, disables only |
| `power_status` | Show the current direct power-control levels | No |
| `flash_test` | Check that the detected flash capacity is 16 MB | No |
| `psram_test` | Write and verify a temporary 64 KiB PSRAM allocation | Temporary RAM only |
| `i2c_scan main` | Probe the main I2C bus on GPIO33/GPIO34 | I2C address probes only |
| `i2c_scan lp` | Probe the low-power I2C bus on GPIO6/GPIO7 | I2C address probes only |
| `pmic_test` | Read TG28_SW identity, battery, VBUS, and charger state | No |
| `rail NAME status` | Read one TG28_SW regulator setting | No |
| `rail NAME on MILLIVOLTS` | Program and enable one TG28_SW regulator | Yes |
| `rail NAME off` | Disable one TG28_SW regulator | Yes |
| `peripheral_power NAME on\|off` | Apply a complete peripheral power sequence | Yes |
| `rtc_test` | Read RX8130CE calendar data and validity flags | No |
| `rtc_set YYYY-MM-DD HH:MM:SS WEEKDAY` | Set the RTC, read it back, and verify the stored value | Yes, RTC registers only |
| `irq_test` | Read and clear TG28_SW/RX8130CE flags until their shared line is released | Yes, clears interrupt flags |
| `typec_test` | Read FUSB303B identity and connection state | Enables the CC controller only |
| `otg on default\|1.5\|3.0` | Select source role and arm the protected Type-C2 boost path | Yes, sources VBUS after CC attach |
| `otg off` | Disable Type-C2 VBUS, CC role, and controller power | Yes, disables only |
| `display_test` | Show red, green, blue, and white AMOLED quadrants | Yes |
| `touch_test` | Require a touch in every display quadrant within 15 seconds | Yes |
| `led_test` | Show red, green, and blue on the addressable LED | Yes |
| `sdcard_test` | Mount, write, read, verify, remove, and unmount a test file | Writes the inserted card |
| `speaker_test` | Play a short, low-level square-wave tone | Yes |
| `microphone_test` | Capture audio and check its peak level | Yes |
| `camera_test` | Power and probe the DVP sensor, then open its ESP Video node | Yes |
| `mark TEST pass\|fail\|skip [detail]` | Record an operator result; details may contain spaces | Report only |
| `report` | Print every result and a JSON summary | No |
| `report_reset` | Return every collected result to `NOT_RUN` | Report only |
| `reboot` | Restart the SoC | Yes |

Run `rail`, `peripheral_power`, and `otg` only after checking the matching rail
against the EVT bring-up sheet. They are explicit commands so no switched load
is enabled during boot. `otg on` prints an additional warning because Type-C2
can source 5 V; the schematic's source indication gate remains part of the
hardware safety path.

An I2C scan with no responding address is recorded as `FAIL`. This is expected
on the main bus while its switched peripheral rails are disabled; it prevents
an electrically unverified or unpowered bus from being reported as working.
The low-power bus passes only when both the RX8130CE at `0x32` and TG28_SW at
`0x34` respond.

`display_test`, `led_test`, and `speaker_test` leave the corresponding report
entry as `NOT_RUN` after sending the test output. The operator must observe the
panel, LED, or speaker and record the result with `mark`. Software activity by
itself is not evidence that light or sound reached the outside of the board.
Only those three tests accept a manually entered `PASS` or `FAIL`; automated
tests must be run through their own command. Any test can be marked `SKIP` when
the omission is intentional and documented.

Valid `rail` names are `dcdc1` through `dcdc5`, `aldo1` through `aldo4`, and
`bldo1` through `bldo2`. Valid `peripheral_power` names are `display`, `touch`,
`audio`, `camera`, `sdcard`, and `external_3v3`. The BSP rejects voltages that
do not have an exact TG28_SW register encoding.

## Build

Use an official ESP-IDF environment with ESP32-S31 preview-target support.
Point the project at the local BSP checkout and build this directory, not the
repository root:

```bash
cd firmware/factory
export CANDIS_S31_BSP_PATH=/path/to/esp-bsp/bsp/candis_s31
idf.py --preview set-target esp32s31
idf.py --preview build
```

The initial configuration uses 16 MB flash, DIO at 40 MHz, and octal PSRAM at
40 MHz. PSRAM-not-found is tolerated during boot so the console remains
available to report the failure. The explicit `psram_test` command performs a
small non-destructive allocation test.

After switching ESP-IDF or BSP revisions, remove the generated configuration
and rebuild:

```bash
idf.py fullclean
rm -f sdkconfig sdkconfig.old
idf.py --preview set-target esp32s31
idf.py --preview build
```

`build/`, `sdkconfig`, and `sdkconfig.old` are generated locally and are not
committed.

To create one local Bring-up image containing the bootloader, partition table,
and application, run:

```bash
idf.py --preview merge-bin --output candis_s31_factory_merged.bin --format raw
```

The raw merged image is written under `build/` and is intended for flash offset
`0x0`. It remains a local artifact until the matching board revision and flash
procedure have been verified.

During local BSP development, Component Manager writes checkout-relative
override paths into `dependencies.lock`. That generated file is ignored here
because it cannot reproduce the build on another machine. The Factory release
manifest must still record the ESP-IDF commit and BSP commit. Once the BSP and
its drivers are available through their public component sources, the project
can resolve a portable lock file and include it with the matching release
evidence. Do not edit a generated lock by hand.

## Serial session

After the programming connector and ROM download sequence have been verified
for the board revision, flash and monitor with:

```bash
idf.py --preview -p PORT flash monitor
```

Do not erase the complete flash unless the matching recovery procedure calls
for it. Save the ROM log and the complete Factory console output for every
board tested.

On boot, all directly controlled power domains are driven to their disabled
levels before the console starts. Expected startup text includes:

```text
Candis-S31 Factory Bring-up
safe_state       PASS     direct power domains disabled
candis-factory>
```

That `PASS` only confirms that the GPIO API accepted the disabled levels. Rail
voltage, leakage, sequencing, and active polarity still require measurement on
EVT1.

## Result format

Every implemented test is one of:

- `PASS`: the documented check ran and met its current criterion;
- `FAIL`: the check ran and did not meet the criterion;
- `SKIP`: the operator intentionally skipped an applicable check;
- `NOT_RUN`: no result has been collected since boot.

Commands print a readable line followed by JSON Lines output:

```text
flash            PASS     size=16777216 expected=16777216
FACTORY_RESULT {"test":"flash","status":"PASS","detail":"size=16777216 expected=16777216"}
```

`report` ends with `FACTORY_SUMMARY`. The overall state remains `NOT_RUN` while
any required test has not run, and becomes `FAIL` when any test fails. A missing
or unimplemented peripheral is never converted to `PASS`.

## Release requirements

The first hardware-verified release must include:

- one merged binary or the complete image and offset list;
- SHA-256 checksums;
- application and BSP source commits;
- exact ESP-IDF version and generated dependency lock;
- supported board revision;
- flash command and expected ROM/application output;
- fixture version and required measurement equipment;
- a completed test manifest and known limitations.

Release binaries belong in GitHub Release assets. Build directories and
downloaded components do not belong in Git.
