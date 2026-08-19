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
| `safe_state` | Stop owned peripheral activity, release handles, and switch every peripheral rail off through the same best-effort cleanup as `power_all_off` | Yes, disables only |
| `power_status` | Show the current direct power-control levels | No |
| `flash_test` | Check that the detected flash capacity is 16 MB | No |
| `psram_test` | Write and verify a temporary 64 KiB PSRAM allocation | Temporary RAM only |
| `i2c_scan main` | Probe the main I2C bus on GPIO33/GPIO34 | I2C address probes only |
| `i2c_scan lp` | Probe the low-power I2C bus on GPIO6/GPIO7 | I2C address probes only |
| `otp_status` | Read every TG28_SW rail enable/voltage and the DC1SW/DC4SW switch states | No |
| `pmic_test` | Read TG28_SW identity, battery, VBUS, and charger state | No |
| `pmic power_on_source` | Read the raw TG28_SW REG20 boot-source bitmap | No |
| `pmic regs` | Dump the current TG28_SW register map from 0x00 to 0xFF | No |
| `pmic input_limit [100 \| 500 source_verified]` | Read the Type-C1 input limit, restore the 100 mA safe baseline, or explicitly arm the verified 500 mA stage | Optional write |
| `pmic vindpm [MILLIVOLTS]` | Read or set the TG28 input-voltage DPM threshold; valid values are 3880-5080 mV in 80 mV steps | Optional write |
| `pmic charge_current [MILLIAMPS]` | Read or set the exact TG28_SW REG62 charge-current limit | Optional write |
| `pmic temperature` | Read the TG28_SW ADC channels (VBAT/VBUS/VSYS/TS/TDIE) in millivolts | Enables an ADC channel for the measurement, then restores it |
| `charge_test [source_verified]` | Read back input-current limit and charge voltage, then grade charger activity; the token is required if the verified input limit is 500 mA | No |
| `rail NAME status` | Read one TG28_SW regulator setting | No |
| `rail NAME on MILLIVOLTS` | Program and enable one TG28_SW regulator; rejected while an active display/audio owner holds that rail | Yes |
| `rail NAME off` | Disable one TG28_SW regulator; rejected while an active display/audio owner holds that rail | Yes |
| `peripheral_power NAME on\|off [output_only]` | Apply a complete peripheral power sequence; EXT pin 2 requires `external_3v3 on output_only` and no self-powered load | Yes |
| `power_all_off` | Stop activity, release handles, and switch every peripheral rail off; idempotent and best-effort (first error kept, every failure printed) | Yes, disables only |
| `rtc_test` | Read RX8130CE calendar data and validity flags | No |
| `rtc_set YYYY-MM-DD HH:MM:SS WEEKDAY` | Set the RTC, read it back, and verify the stored value; out-of-range fields are rejected before writing | Yes, RTC registers only |
| `rtc_alarm` | Arm an alarm at the next minute boundary and wait for the RX8130CE alarm flag; minute granularity only | Rearms RTC alarm registers |
| `irq_test` | Read and clear TG28_SW/RX8130CE flags until their shared line is released | Yes, clears interrupt flags |
| `buttons` | Wait for a BOOT key pulse on GPIO61, then a short PWR key press reported through the TG28_SW power-key IRQ | No |
| `typec_test` | Read FUSB303B identity and connection state; FAIL when the controller is not in DRP mode or stays silent, WARN for ambiguous CC state | Enables the CC controller only |
| `otg on` | Select source role and arm the protected Type-C2 boost path at the 500 mA default advertisement | Yes, sources VBUS after CC attach |
| `otg off` | Disable Type-C2 VBUS, CC role, and controller power | Yes, disables only |
| `usb_host_test` | Install the USB Host stack at the 500 mA default advertisement, wait up to 20 s for a Type-C2 device, print VID/PID/speed/strings, then tear everything down | Yes, host stack + VBUS during the test |
| `usb_msc_test [SECONDS 10-1800] [overwrite]` | Mount a FAT MSC device, write a deterministic pattern, fsync, verify every block, and remove `CANDIS_USB_STRESS.BIN`; refuses an existing file unless `overwrite` is explicit | Yes, host stack + VBUS and USB-drive writes |
| `wifi_scan` | Scan for access points in station mode; PASS requires at least one AP | No |
| `ble_smoke` | Initialize, enable, disable, and release the BLE controller | No |
| `rf_init` | Enter Espressif PHY RF certification mode | Enables the RF test path |
| `rf_stop` | Stop the active PHY RF TX/RX test | Disables only |
| `wifi_tx CHANNEL RATE BACKOFF LENGTH DELAY COUNT` | Start controlled Wi-Fi TX; count 0 is continuous | Yes, RF transmitter |
| `wifi_rx CHANNEL RATE` | Start controlled Wi-Fi RX counting | Yes, RF receiver |
| `wifi_tone ENABLE CHANNEL BACKOFF` | Start or stop a Wi-Fi carrier-wave tone | Yes, RF transmitter |
| `ble_tx LEVEL CHANNEL LENGTH TYPE SYNC RATE COUNT` | Start controlled BLE TX; level is about `(LEVEL-8)*3 dBm` | Yes, RF transmitter |
| `ble_rx CHANNEL SYNC RATE` | Start controlled BLE RX counting | Yes, RF receiver |
| `bt_tone ENABLE CHANNEL BACKOFF` | Start or stop a Bluetooth carrier-wave tone | Yes, RF transmitter |
| `rf_rx_result` | Read the latest PHY RX correct/total counters and RSSI | No |
| `display_test` | Show red, green, blue, and white AMOLED quadrants, then ask the operator to confirm | Yes |
| `display_brightness PERCENT` | Set CO5300 brightness from 0 through 100 | Yes |
| `display_sleep [deep]` | Enter normal sleep or SLPIN + DSTBON deep standby | Yes |
| `display_wake [deep]` | Wake through SLPOUT or the required deep-standby reset pulse | Yes |
| `display_sleep_test` | Cycle sleep and deep standby with an operator visual check after each enter/exit | Yes |
| `touch_test` | Prompt the four corners in order and require each press to land in its corner zone (orientation errors fail at least one step) | Yes |
| `touch_draw [SECONDS]` | Track touches with an on-screen marker for 5-300 s and log throttled coordinates plus the raw CTP_INT level; diagnostic only, files no report entry | Yes |
| `led_test` | Show red, green, and blue on the addressable LED, then ask the operator to confirm | Yes |
| `sdcard_test` | Mount, write, read, verify, remove, and unmount a test file | Writes the inserted card |
| `speaker_test [FREQ_HZ] [VOLUME] [DURATION_MS]` | Play a square-wave tone, then ask the operator to confirm | Yes |
| `microphone_test [DURATION_MS] [GAIN_DB]` | Capture stereo audio and require both channels to exceed the noise threshold | Yes |
| `camera_test` | Capture five DVP frames and verify frame size, non-blank content, and frame-to-frame change; each frame wait is bounded by a 3 s timeout | Yes |
| `jpeg_encode_test [COUNT] [QUALITY]` | Benchmark the S31 hardware JPEG encoder with a synthetic 800x600 RGB565 frame | No, hardware JPEG codec |
| `mark TEST pass\|fail\|skip [detail]` | Record an operator result; details may contain spaces | Report only |
| `report` | Print every result and a JSON summary | No |
| `report_reset` | Return every collected result to `NOT_RUN` while retaining the most recent safe-state result | Report only |
| `reboot` | Restart the SoC | Yes |

Run `rail`, `peripheral_power`, and `otg` only after checking the matching rail
against the EVT bring-up sheet. Raw rail changes are rejected while a display
or audio owner is active. EXT pin 2 has no reverse-current blocker, so it can
only be enabled with the explicit `output_only` token and every self-powered
load disconnected. `otg on` prints an additional warning because Type-C2 can
source 5 V; the schematic's source indication gate remains part of the
hardware safety path.

Two ordering facts matter for the first passes:

- The boot safe state powers the FUSB303B and every switched peripheral rail
  off. Scan the main bus once in that state to prove those devices are silent,
  then run `typec_test` and scan again: the FUSB303B must answer at the
  schematic strap address 0x21 while its control domain is on. The host runner
  records this second observation as `type_c_power_scan` before USB Host takes
  ownership of the port and tears it down.
  `typec_test` also waits up to 1 s for CC attach debounce after enabling the
  controller, and reports both the FUSB303B identity type and live Type register.
- The BSP panel init sequence programs the CO5300 brightness register to
  30 % before any Display-On, and `bsp_display_backlight_on()` restores the
  last level set through `bsp_display_brightness_set()` instead of forcing
  100 %, so the first light-up never flashes full brightness. Raise the level
  in steps and record current/temperature per step.

The console registers 39 top-level commands including `help`. PMIC init first
clamps the Type-C1 input-current limit to 100 mA. `pmic input_limit 500
source_verified` is the only 500 mA path; use it only after verifying the
source/cable and watching VBUS with a current probe, then restore the safe
baseline with `pmic input_limit 100`. Plain `charge_test` requires that 100 mA
baseline. Only during the instrumented 500 mA step, run `charge_test
source_verified`.

`pmic_test` already reads fuel-gauge SOC and voltage. `pmic charge_current 500`
sets the EVT target current without changing the OTP/default setting at boot.
Use it only with current limiting and battery temperature monitoring, then
restore the confirmed 50 mA default with `pmic charge_current 50`. EVT1 has no
battery NTC (the TS pin is a fixed input), so `pmic temperature` is the
on-board monitoring channel: it reads the TS input and the TDIE die-sensor
voltage trend during the high-current step; watch the cell itself with an
external probe.

USB evidence boundaries, stated once: `otg on` only proves the Type-C2 5 V
boost path; it never installs the USB Host stack and is never a Host PASS.
`usb_host_test` is the host evidence — it enumerates a real device and reads
its descriptors over EP0, which is actual bus traffic, then stops the stack
and drops the boost. USB *device* mode (Type-C2 as a gadget behind an
external host) has no Factory coverage in this round; run the dedicated
TinyUSB-device diagnostic image for that check, per the bring-up plan. On
this hardware the source always advertises the USB 500 mA default; the
1.5 A/3 A enum values are kept only to report the peer's advertised
capability, and any request for a higher source current fails explicitly.

The OV5640 autofocus/VCM path is intentionally not exposed as a command until
the module supplier confirms VCM power and the actuator control interface.

An I2C scan on the main bus is graded against each device's power state.
While `BSP_POWER_TYPE_C_CONTROL` is on, FUSB303B must answer at exactly one of
`0x21`/`0x31` (missing or answering at both is `FAIL`); `0x31` records a
`WARN` because the address strap then mismatches the schematic. Its silence
after `power_all_off` is expected, while any answer with the control domain
off records a `WARN`. Devices behind switched rails (CST820 `0x15`, ES8389
`0x10` — 7-bit on the wire; `0x20` is its 8-bit write address —, OV5640
`0x3C`) may likewise stay silent while their rail is off; a
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

On boot, `bsp_board_init()` first configures the GPIO2 (PMIC/RTC) and GPIO43
(Type-C) interrupt lines as pulled-up inputs, then drives all directly
controlled power domains to their disabled levels before the console starts.
Before either happens, the firmware snapshots every TG28_SW rail over the LP
I2C bus and logs it, because the safe state that follows deliberately disables
the optional rails — a live `otp_status` read after boot can never prove what
the part shipped with. That snapshot equals the OTP defaults **only after a
cold power-on**: the TG28 runs from its own supply, no SoC-only reset reaches
it, and the safe state clears enable bits without rewriting voltage codes, so
after `esp_restart()` the enable bits still show the previous run's safe state
(DCDC4 reads disabled, the opposite of its OTP value) and the voltage codes
still hold whatever a test wrote. The firmware labels the line by boot type,
so only the `OTP boot snapshot:` form is lot evidence. Expected startup text
includes:

```text
Candis-S31 Factory Bring-up
safe_state       PASS     direct power domains disabled
OTP boot snapshot: dcdc1=on@3300mV ...      <- power-on boot; lot evidence
candis-factory>
```

That `PASS` only confirms that the GPIO API accepted the disabled levels. Rail
voltage, leakage, sequencing, and active polarity still require measurement on
EVT1. After a warm reset the same line is logged as a warning that it is not
the OTP state; power-cycle the board before recording lot evidence. Compare
the OTP boot snapshot against the TG28 confirmation sheet for the lot: DCDC1
enabled at 3.3 V and DCDC4 enabled at 1.8 V (step1, no external load, no
measurable node — register evidence only), with DCDC2/DCDC3, the ALDO/BLDO
rails, CPUSLDO, DC1SW, and DC4SW off. A lot whose snapshot does not match is
quarantined, not brought up.

GPIO0 is the TF card-detect input only — it is not a boot strap, so a
board powered with a card fitted samples the pin low. The firmware records
the level early in `app_main`, logs it at boot, and reports it in the
`board_info` `FACTORY_INFO` as `sd_detect_boot_level` (`low` means a card
was fitted at power-on). The real boot strap pins are GPIO36/GPIO37/
GPIO60/GPIO61; GPIO0 is not part of that set. This is observation only; the
boot path itself is unchanged.

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
issues `report_reset` and requires its acknowledgment: results persist in NVS
across power cycles, so a second run on the same board would otherwise mix
stale entries into the summary and can turn a timed-out stage into a false
`PASS`. A missing acknowledgment aborts the run and writes a failed host report;
no hardware stage is sent. The fixed sequence first scans the main bus with
every optional/control rail off, then uses `typec_test` plus a second
host-graded scan (`type_c_power_scan`) to prove the FUSB303B answers at 0x21
only while its control domain is on. It then runs `usb_host_test`; no attached
device scores `WARN`, while successful descriptor enumeration scores the
software-owned USB Host result. Before `rtc_test` it first issues an `rtc_set`
from the host clock (UTC), because a fresh board powers up with an invalid RTC
time. The board identity defaults to the base MAC from `board_info`.
`--non-interactive` supplies safe canned answers for unattended parser
validation; it is not a substitute for fixture observations.
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

`firmware/factory/release/` is the local staging area: it is the default output
of `RELEASE_NAME=<exact-tag> tools/release/pack_factory_release.sh` and is
git-ignored. The script requires a concrete release name, fills
`tools/release/manifest.template.yaml`, generates a matching placeholder-free
Recovery `launchpad.toml`, and archives the merged image, its SHA-256, the
dependency lock, `flash_factory.sh`, the Recovery procedure, and both manifests.
Published artifacts are uploaded as GitHub Release assets only after hardware
validation. The CI `factory` job runs the same script with a concrete
`ci-<commit>` identifier and stores the archive as a build artifact.
