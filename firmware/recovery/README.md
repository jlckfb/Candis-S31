# Recovery

Candis-S31 recovery starts with the ESP32-S31 ROM download mode. It does not depend on a working application, display, touch controller, or filesystem.

> EVT1 has not been fabricated. The physical button sequence, USB connector, serial device, and recovery image have not been validated.

## Recovery order

1. Disconnect the battery and external peripherals if the hardware condition is unknown.
2. Connect the documented programming USB port with a data cable.
3. Enter ROM download mode using the procedure for the released board revision.
4. Confirm that the ROM loader is visible before erasing or writing flash.
5. Download the factory image and checksum from the matching GitHub Release.
6. Run the exact flash command included in that release.
7. Reset the board and save the complete serial log.

Do not guess flash offsets. They come from the build that produced the release and may change with the partition table or bootloader.

## Using an ESP-IDF environment

Activate an official ESP-IDF installation that supports ESP32-S31. The release procedure will use its packaged esptool. Check communication first:

```bash
esptool.py --chip esp32s31 -p PORT chip-id
```

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
