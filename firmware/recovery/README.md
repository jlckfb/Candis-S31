# Recovery

Candis-S31 recovery starts with the ESP32-S31 ROM download mode. It does not depend on a working application, display, touch controller, or filesystem.

> EVT1 has not been fabricated. The physical button sequence, USB connector, serial device, and recovery image have not been validated.

## Recovery order

1. Disconnect the battery and external peripherals if the hardware condition is unknown.
2. Connect the Type-C1 programming port (CH343P) with a data cable. Type-C2 is
   the OTG connector and is not the programming port.
3. Enter ROM download mode using the procedure for the released board revision.
4. Confirm that the ROM loader is visible before erasing or writing flash.
5. Obtain `candis_s31_factory_merged.bin` and its adjacent `.sha256` file from
   the matching Factory release directory or GitHub Release.
6. Verify the SHA-256, then run `flash_factory.sh` from the configured tmux
   `idf` session.
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

For a local release artifact:

```bash
cd firmware/factory/release
sha256sum -c candis_s31_factory_merged.bin.sha256
cd ../../recovery
./flash_factory.sh PORT
```

The script performs the checksum and `chip-id` preflight again, then writes the
merged image at offset `0x0` with DIO/40 MHz/16 MB settings. See
[`enter_download_mode.md`](enter_download_mode.md) for automatic DTR/RTS and
manual BOOT-key entry.

For browser flashing, self-host this directory and the Factory release files,
then pass the public `launchpad.toml` URL to ESP Launchpad. The TOML uses an
explicit `0x0` address for the merged image. The self-hosted Launchpad build
and its bundled esptool-js must recognize `esp32s31`; verify both device
detection and the configuration in the browser before publishing the link.
Verify the published checksum first because Launchpad TOML v1.0 does not
itself define a portable SHA-256 enforcement field.

The final write command will be published with the factory image. Do not run `erase-flash` unless the recovery instructions explicitly require it; an erase can destroy calibration, provisioning, or user data.

## Information to include in a recovery report

- board revision;
- USB connector and cable used;
- operating system and serial port;
- esptool and ESP-IDF versions;
- command line and complete output;
- ROM boot log;
- factory release name and checksum.

Recovery is complete only after the restored image boots and its documented checks pass.
