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
| `pmic power_on_source` | Read the raw TG28_SW REG20 boot-source bitmap | No |
| `pmic charge_current [MILLIAMPS]` | Read or set the exact TG28_SW REG62 charge-current limit | Optional write |
| `pmic temperature` | Read the TG28_SW ADC channels (VBAT/VBUS/VSYS/TS/TDIE) in millivolts | Enables an ADC channel for the measurement, then restores it |
| `charge_test` | Read back the input-current limit and charge voltage, then grade charger activity from VBUS and charge flags | No |
| `rail NAME status` | Read one TG28_SW regulator setting | No |
| `rail NAME on MILLIVOLTS` | Program and enable one TG28_SW regulator | Yes |
| `rail NAME off` | Disable one TG28_SW regulator | Yes |
| `peripheral_power NAME on\|off` | Apply a complete peripheral power sequence | Yes |
| `rtc_test` | Read RX8130CE calendar data and validity flags | No |
| `rtc_set YYYY-MM-DD HH:MM:SS WEEKDAY` | Set the RTC, read it back, and verify the stored value; out-of-range fields are rejected before writing | Yes, RTC registers only |
| `rtc_alarm` | Arm an alarm at the next minute boundary and wait for the RX8130CE alarm flag; minute granularity only | Rearms RTC alarm registers |
| `irq_test` | Read and clear TG28_SW/RX8130CE flags until their shared line is released | Yes, clears interrupt flags |
| `buttons` | Wait for a BOOT key pulse on GPIO61, then a short PWR key press reported through the TG28_SW power-key IRQ | No |
| `typec_test` | Read FUSB303B identity and connection state; FAIL when the controller is not in DRP mode or stays silent, WARN for ambiguous CC state | Enables the CC controller only |
| `otg on default\|1.5\|3.0` | Select source role and arm the protected Type-C2 boost path | Yes, sources VBUS after CC attach |
| `otg off` | Disable Type-C2 VBUS, CC role, and controller power | Yes, disables only |
| `wifi_scan` | Scan for access points in station mode; PASS requires at least one AP | No |
| `ble_smoke` | Initialize, enable, disable, and release the BLE controller | No |
| `display_test` | Show red, green, blue, and white AMOLED quadrants, then ask the operator to confirm | Yes |
| `display_brightness PERCENT` | Set CO5300 brightness from 0 through 100 | Yes |
| `display_sleep [deep]` | Enter normal sleep or SLPIN + DSTBON deep standby | Yes |
| `display_wake [deep]` | Wake through SLPOUT or the required deep-standby reset pulse | Yes |
| `display_sleep_test` | Cycle sleep and deep standby with an operator visual check after each enter/exit | Yes |
| `touch_test` | Require a touch in every display quadrant within 15 seconds | Yes |
| `led_test` | Show red, green, and blue on the addressable LED, then ask the operator to confirm | Yes |
| `sdcard_test` | Mount, write, read, verify, remove, and unmount a test file | Writes the inserted card |
| `speaker_test` | Play a short, low-level square-wave tone, then ask the operator to confirm | Yes |
| `microphone_test` | Capture audio and check its peak level | Yes |
| `camera_test` | Capture five DVP frames and verify frame size, non-blank content, and frame-to-frame change; each frame wait is bounded by a 3 s timeout | Yes |
| `mark TEST pass\|fail\|skip [detail]` | Record an operator result; details may contain spaces | Report only |
| `report` | Print every result and a JSON summary | No |
| `report_reset` | Return every collected result to `NOT_RUN` (keeps `safe_state`, which only runs at boot) | Report only |
| `reboot` | Restart the SoC | Yes |

Run `rail`, `peripheral_power`, and `otg` only after checking the matching rail
against the EVT bring-up sheet. They are explicit commands so no switched load
is enabled during boot. `otg on` prints an additional warning because Type-C2
can source 5 V; the schematic's source indication gate remains part of the
hardware safety path.

The console registers 36 top-level commands including `help`. `pmic_test`
already reads the fuel-gauge SOC and voltage; `pmic charge_current 500` adds
the EVT target-current check without changing the OTP/default setting at boot.
Use it only with current limiting and battery temperature monitoring, then
restore the confirmed 50 mA default with `pmic charge_current 50`. EVT1 has no
battery NTC (the TS pin is a fixed input), so `pmic temperature` is the
on-board monitoring channel: it reads the TS input and the TDIE die-sensor
voltage trend during the high-current step; watch the cell itself with an
external probe.

The OV5640 autofocus/VCM path is intentionally not exposed as a command until
the module supplier confirms VCM power and the actuator control interface.

An I2C scan on the main bus is graded against the expected device set:
FUSB303B must answer at exactly one of `0x21`/`0x31` (missing or answering at
both is `FAIL`); `0x31` records a `WARN` because the address strap then
mismatches the schematic. Devices behind switched rails (CST820 `0x15`,
ES8389 `0x20`, OV5640 `0x3C`) may stay silent while their rail is off; a
powered device that does not answer, an unexpected address, or an answer at
the unconfirmed VCM address `0x0C` records a `WARN`. The low-power bus passes
only when both the RX8130CE at `0x32` and TG28_SW at `0x34` respond.

## Operator checks and manual results

`display_test`, `led_test`, `speaker_test`, and `display_sleep_test` emit a
machine-readable `FACTORY_PROMPT` line and wait up to 30 seconds for the
operator to answer `y` (confirmed), `n` (failed), or `s` (skip). A `y` records
`PASS`, an `n` records `FAIL`, and a skip or timeout leaves the entry
`NOT_RUN`. Software activity by itself is not evidence that light or sound
reached the outside of the board, so an unanswered prompt never becomes a
`PASS`. When a prompt could not be answered — for example over a log-only
connection — record the observation afterwards with `mark`.

`buttons` waits for real key events instead of asking: first a press-and-release
of the BOOT key (SW2, KEY_BOOT net on GPIO61, idle-high through an external
10k pull-up), then a **short** press of the PWR key, which the TG28_SW reports
through its power-key interrupt flags. A long PWR press powers the board off,
so the command prints that warning before waiting. `rtc_alarm` needs no
operator input; it targets the next minute boundary because the RX8130CE alarm
compares whole minutes at the finest.

Only the operator-judged tests — `display`, `rgb_led`, `speaker`, `buttons`,
and `display_sleep` — accept a manually entered `PASS` or `FAIL` through
`mark`. Automated tests must be run through their own command. Any test can be
marked `SKIP` when the omission is intentional and documented.

`camera_test` bounds each frame wait with a 3 s `VIDIOC_S_DQBUF_TIMEOUT`. A
dead or unpowered sensor now fails the command with `ESP_ERR_TIMEOUT` instead
of stalling the console; investigate the sensor power and DVP wiring, then
rerun.

Valid `rail` names are `dcdc1` through `dcdc4`, `aldo1` through `aldo4`,
`bldo1` through `bldo2`, `cpusldo`, and `dldo1` through `dldo2`. `cpusldo` is
unconnected on this board and is expected to stay off (OTP-disabled), so
`rail cpusldo status` reading back disabled is the pass condition, not a
measurement failure. Valid
`peripheral_power` names are `display`, `touch`, `audio`, `camera`, `sdcard`,
and `external_3v3`. The BSP rejects voltages that do not have an exact TG28_SW
register encoding.

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
small non-destructive allocation test. `sdkconfig.defaults` also enables the OV5640 DVP sensor in
RGB565 big-endian 800x600 at 10 fps and the BLE controller for `ble_smoke`.
The BLE host stack is disabled (`BT_CONTROLLER_ONLY`): the smoke test talks to
the controller directly. Because the diagnostic build exceeds the 1 MB default
app partition, the project ships a custom `partitions.csv` with a 4 MB factory
partition; flashing an older board image layout requires a full reflash of the
bootloader, table, and application.

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
override paths into `dependencies.lock`. That generated file cannot reproduce
the build on another machine, so the release package ships it only as build
evidence next to the recorded ESP-IDF and BSP commits. Once the BSP and its
drivers are available through their public component sources, the project can
resolve a portable lock file and include it with the matching release
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
- `WARN`: the check ran, but the result is ambiguous and needs human review
  (for example `charge_test` without VBUS, or a Type-C attach without a valid
  CC orientation);
- `SKIP`: the operator intentionally skipped an applicable check;
- `NOT_RUN`: no result has been collected since boot or the last `report_reset`.

Test results persist in the `factory` NVS namespace, so the bring-up steps
that power-cycle the board keep their history: after a reboot the `report`
command still shows results collected before the power cut. `report_reset`
clears both the live and the persisted state (keeping only `safe_state`,
which re-runs at every boot). A blob written by a different firmware layout
is ignored and the state falls back to `NOT_RUN`.

Commands print a readable line followed by JSON Lines output:

```text
flash            PASS     size=16777216 expected=16777216
FACTORY_RESULT {"test":"flash","status":"PASS","detail":"size=16777216 expected=16777216"}
```

`report` ends with `FACTORY_SUMMARY`, which counts each status in a `warn`
field alongside `pass`/`fail`/`skip`/`not_run`. The overall state is `FAIL`
when any test fails, then `WARN` when any test warns, then `NOT_RUN` while any
required test has not run, and only then `PASS`. A missing or unimplemented
peripheral is never converted to `PASS`.

If the console itself fails to start, the firmware logs the error, waits five
seconds, and restarts so a transient stdio or heap failure cannot strand a
board on the line. After three consecutive failures it halts with the error
visible instead of reboot-looping; power-cycle to retry.

## Host automation

`host_tools/run_evt.py` drives the whole flow from a PC over the console UART:

```bash
python3 host_tools/run_evt.py --port /dev/ttyUSB0 --board-id EVT-0042
```

The script sends each test command in a fixed order, parses `FACTORY_INFO`,
`FACTORY_RESULT`, `FACTORY_PROMPT`, and `FACTORY_SUMMARY` lines, relays local
operator answers to the board's prompts, and writes `evt_<board>_<timestamp>.json`
plus the complete `.log` under `evt_logs/`. Before the first test stage it
issues `report_reset` and waits for its acknowledgment: results persist in NVS
across power cycles, so a second run on the same board would otherwise mix
stale entries into the summary and can turn a timed-out stage into a false
`PASS`. Before `rtc_test` it first issues an `rtc_set` from the host clock
(UTC), because a fresh board powers up with an invalid RTC time. The board
identity defaults to the base MAC from `board_info`. `--non-interactive`
answers `s` (skip) to every prompt for log-only runs.
`python3 host_tools/run_evt.py --self-test` verifies the parser and report
writer against a scripted fake firmware on a pseudo terminal and needs no
hardware.

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

`firmware/factory/release/` is the local staging area: it is the default
output of `tools/release/pack_factory_release.sh` and is git-ignored. The
script fills `tools/release/manifest.template.yaml` with build facts and
archives the merged image, its SHA-256, the dependency lock, and the manifest.
Published artifacts are uploaded as GitHub Release assets only after hardware
validation; the CI `factory` job runs the same script as a dry-run and stores
the archive as a build artifact.
