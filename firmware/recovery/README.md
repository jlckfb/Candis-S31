# Recovery

Candis-S31 recovery starts with the ESP32-S31 ROM download mode. It does not depend on a working application, display, touch controller, or filesystem.

> EVT1 revision 0.5 (`v0.5_260803_1544`) is in hand. ROM download and the
> serial adapter are known-good; the complete merged-image recovery procedure
> remains a hardware-validation item and must be run only with a matching
> release artifact.


## Recovery order

1. Disconnect the battery and external peripherals if the hardware condition is unknown.
2. Connect the Type-C1 programming port (CH343P) with a data cable. Type-C2 is
   the OTG connector and is not the programming port.
3. Enter ROM download mode using the procedure for the released board revision.
4. Confirm that the ROM loader is visible before erasing or writing flash.
5. Obtain `candis_s31_factory_merged.bin` and its adjacent `.sha256` file from the matching Factory GitHub Release (a plain source clone does not contain release binaries).
6. Verify the SHA-256, then run `flash_factory.sh` from the configured tmux `idf` session.
7. Reset the board and save the complete serial log.


Do not guess flash offsets. They come from the build that produced the release and may change with the partition table or bootloader.

## Using an ESP-IDF environment

Activate the ESP-IDF installation that supports ESP32-S31 in the tmux `idf`
session. Recovery requires esptool 5.3.0 or newer. Check the tool and ROM
communication first:

```bash
esptool version
esptool --chip esp32s31 -p PORT chip-id
```

The installed package must contain the ESP32-S31 flasher stub. If no EVT board
is attached, confirming the `esp32s31` stub in the esptool package is the
minimum environment check; it is not hardware validation.

For a local release artifact downloaded from GitHub:

```bash
cd firmware/recovery
sha256sum -c /path/to/candis_s31_factory_merged.bin.sha256
./flash_factory.sh /dev/ttyACM0 /path/to/candis_s31_factory_merged.bin 4000000 0 1
```

The final two arguments explicitly attest the sampled GPIO61 levels: `0` for
the ROM-download reset and `1` for the post-write SPI-boot reset. The script
refuses to write without both values. It performs the checksum and `chip-id`
preflight again, writes the merged image at offset `0x0` with DIO/40 MHz/16 MB
settings while keeping the ROM session alive, verifies that entire image
against flash, then hard-resets only after the run-level assertion.

To create a local image, build `firmware/factory` and run
`idf.py --preview merge-bin --output candis_s31_factory_merged.bin --format raw`
from that project, then generate its checksum before invoking the script.

See [`enter_download_mode.md`](enter_download_mode.md) for automatic DTR/RTS
and manual BOOT-key entry.

The raw merged image spans the `nvs` and `phy_init` partition ranges; writing
it at `0x0` resets those partitions to the packaged `0xFF` state. This clears
all Factory results persisted in NVS, including any partially completed EVT
history and console-failure counter. Before reflashing a board already under
test, save the latest `host_tools/run_evt.py` JSON and complete console log;
after reflashing, treat every result as `NOT_RUN` and start a new run.

For browser flashing, use the generated `launchpad.toml` packaged beside the
matching Factory image; never publish `launchpad.template.toml` directly.
`tools/release/pack_factory_release.sh` substitutes the exact release tag and
image SHA-256 and refuses to leave those placeholders unresolved. The TOML
uses an explicit `0x0` address for the merged image. The self-hosted ESP
Launchpad build and its bundled esptool-js must recognize `esp32s31`; verify
both device detection and the configuration in the browser before publishing
the link. Verify the packaged checksum first because Launchpad TOML v1.0 does
not itself define a portable SHA-256 enforcement field.

`erase-flash` is not part of the normal recovery flow: it erases storage beyond
the ranges already reset by the merged image and can destroy provisioning or
user data. Run it only when the recovery instructions explicitly require it.

## Information to include in a recovery report

- board revision;
- USB connector and cable used;
- operating system and serial port;
- esptool and ESP-IDF versions;
- command line and complete output;
- ROM boot log;
- factory release name and checksum.

Recovery is complete only after the restored image boots and its documented checks pass.
