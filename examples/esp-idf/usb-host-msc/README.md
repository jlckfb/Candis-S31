# ESP-IDF USB host MSC (Type-C2)

Starts the BSP's validated 500 mA host mode, mounts the first USB mass storage
device on the Type-C2 data port at `/usb0`, and lists its root directory.
The example does not format the drive, write raw sectors, or create files.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, QIO 80 MHz |
| Partition table | `partitions.csv` (single 8 MB app) |
| Managed components | `espressif/usb_host_msc` from the public Component Registry; additional dependencies via the board component |
| Compile status | Verified |
| Hardware status | **Partial** — EVT1 (2026-09-10): host startup and drive enumeration work; the attached 62 GB stick reports partition type 0x07 and did not mount with FATFS |

## Behavior

- Starts the host in validated 500 mA mode (`BSP_USB_HOST_POWER_MODE_USB_DEV`),
  installs the MSC driver, and waits up to 15 s for enumeration. Normal startup
  no longer deliberately requests an unsupported power mode.
- With a compatible FAT12/16/32 drive: mounts `/usb0` and prints the root
  directory once. `dir: N root entries` is the end marker (zero is valid).
- Capture **25 s** from reset with the drive already inserted. Allow five
  existing mount attempts spaced by 500 ms. Require enumeration/capacity,
  successful mounting, then root listing, without read/mount errors.
- With no drive, installation failure, or failed mount: reports the precise
  stage and ends the probe; insert a compatible drive and **reset** to retry.
  There is no hot-plug remount loop disguised as a waiting heartbeat.
- Failed-mount diagnosis only reads sector 0. It reports the signature,
  partition-0 type and little-endian first LBA from offset `0x1BE + 8`.
  These fields describe a partition only when sector 0 is an MBR. Type `0x07`
  alone does not distinguish NTFS, exFAT or another filesystem; corruption
  and I/O failures are also possible. `FAT mount failed` is a failure marker,
  not a filesystem identification or PASS.
- **Do not reformat the existing 0x07 stick for this check.** A separate,
  already compatible FAT medium is required to close the mount/list gap.
  File read/write throughput is not a feature of this directory-listing example.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/usb-host-msc -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/usb-host-msc -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/usb-host-msc -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) usb_host_msc: MSC host up; insert a USB drive on Type-C2
I (…) usb_host_msc: USB drive mounted at /usb0
I (…) usb_host_msc: dir: DATA.TXT
I (…) usb_host_msc: dir: 12 root entries
```

## Constraints

- **Use the C2 port** for the drive; C1 is the flash/serial console.
- The example accepts the first enumerated drive only; hubs and multiple
  LUNs are not exercised here.
- The 500 mA limit is a board policy (EVT1 USB rail); drives that draw more
  than 500 mA without negotiation fail by design.
