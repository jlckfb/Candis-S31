# ESP-IDF display + touch

AMOLED in the panel's physical 180° mounting orientation (applied by the
BSP), a cross marker follows the finger, and every touch move prints
`touch: x,y` on the serial console. The idle screen does not print a heartbeat.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, QIO 80 MHz, 32 MB PSRAM octal 250 MHz |
| Partition table | `partitions.csv` (single 8 MB app) |
| Managed components | None direct (LVGL via the board component) |
| Compile status | Verified |

## Behavior

- Screen shows a dark background, a cross marker centered on the panel, and
  a hint label.
- Pressing/dragging moves the cross marker to the finger and prints
  `touch: <x>,<y>` for every coordinate update.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/display-touch -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/display-touch -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/display-touch -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) display_touch: display-touch ready: 460x460, cross marker follows the finger
I (…) display_touch: touch: 230,230
I (…) display_touch: touch: 118,341
```

The marker must sit under the finger in every corner; coordinate x grows
right and y grows down in the 180°-rotated panel orientation.

## Constraints

- GPIO16 (TE) interrupt ownership belongs to the display stack; do not
  register an application ISR on that pin.
- Requires 32 MB PSRAM, the LVGL draw pipeline baseline.
