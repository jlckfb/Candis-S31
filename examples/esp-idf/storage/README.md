# ESP-IDF TF card storage

Mounts the TF card through the BSP SDMMC path, prints FAT capacity, lists
the root directory, and runs a write → read-back → delete probe on
`/sdcard/example_storage.txt`.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, QIO 80 MHz |
| Partition table | `partitions.csv` (single 8 MB app) |
| Managed components | None direct |
| Compile status | Verified |
| Hardware status | **Verified** — EVT1 (2026-09-10): mount, capacity, root listing and the write/read-back/delete probe all passed |

## Behavior

- With no card inserted, reports the prerequisite once and ends. Insert a
  compatible TF card and reset; there is no hot-plug retry.
- With a card present, mounts it, prints total/free capacity, lists every
  root entry, and probes a new file using exclusive creation. An existing
  `example_storage.txt` is preserved and reported as a blocked write probe.
- Capture **15 s** from reset and require the complete sequence:
  `write ok` → `read ok (…, content match)` → `delete ok`, without errors.
  Short writes, close/read errors and mismatched content are not success.
  Deletion after an error only cleans the file created by this run; `delete ok`
  alone is not acceptance. A full card may validly have zero free capacity.
- A mount failure is reported once; it is not followed by a completion heartbeat.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/storage -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/storage -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/storage -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) storage: capacity: 30277632 KB total, 30277500 KB free
I (…) storage: dir: media
I (…) storage: dir: 3 entries
I (…) storage: write ok (27 bytes)
I (…) storage: read ok (27 bytes, content match)
I (…) storage: delete ok
```

Without a card:

```text
W (…) storage: insert TF card, then reset to run the storage probe
```

## Constraints

- GPIO36 doubles as the VDD_SPI strap and the TF power control; the BSP owns
  the power sequencing. Do not toggle it from application code.
- Card detection is polled once at boot in this example (no hot-plug
  service); reflash or reset after inserting a card.
