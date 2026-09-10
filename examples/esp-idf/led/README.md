# ESP-IDF RGB LED

Drives the Candis-S31 WS2812B (GPIO4, powered through the TG28 DC1SW switch
opened by the BSP) with a steady white and cycles three effects, printing the
current effect name on every switch.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, DIO 40 MHz |
| Managed components | None direct (board component pulls `led_indicator`) |
| Compile status | Verified |

## Behavior

- Board bootstrap powers the switchable domains down, then
  `bsp_led_indicator_create()` opens the DC1SW switch and creates the
  indicator.
- The LED lights up white and cycles `on (steady)` → `breathe (slow)` →
  `blink (slow)` every 4 s, printing `effect: <name>` each time.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/led -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/led -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/led -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) example_board: board init done (BSP rev repository-board-component)
I (…) led: RGB LED ready (WS2812B on GPIO4, powered via DC1SW)
I (…) led: effect: on (steady)
I (…) led: effect: breathe (slow)
I (…) led: effect: blink (slow)
I (…) led: effect: on (steady)
```

Capture **15 s** from reset: `on (steady)` → `breathe (slow)` →
`blink (slow)` → the next `on (steady)` covers one 12 s cycle. The sequence
repeats indefinitely, so the repeated steady phase marks the cycle. The LED
visibly follows the printed effect names.

## Constraints

- The example uses the IDF default partition table; no TF card, display, or
  PSRAM is required.
- The DC1SW switch is owned by the BSP indicator path; do not toggle it from
  application code while the indicator exists.
