# ESP-IDF buttons

Prints every Candis-S31 physical input event on the serial console. The BOOT
key (GPIO61, external 10k pull-up) is polled with debounce; the PWR key and
the RX8130CE alarm flag arrive on the TG28 shared IRQ line (GPIO2).

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, DIO 40 MHz |
| Managed components | None direct; public registry dependencies via the repository board component |
| Compile status | Verified |
| Hardware status | **Partial** — EVT1 (2026-09-10): UART-bridge transitions of the real BOOT GPIO produced SHORT and LONG events; mechanical BOOT/PWR presses remain unverified |

## Behavior

- BOOT released within 800 ms prints `event: BOOT_SHORT`.
- BOOT held for 800 ms prints `event: BOOT_LONG` at the threshold moment; the
  following release is swallowed.
- PWR short press prints `event: PWR_SHORT`. A PWR long press powers the
  board off in TG28 hardware and never reaches software.
- An RX8130CE alarm flag drained from the shared IRQ line prints
  `event: RTC_ALARM` (no alarm is programmed by this example).
- With no key activity the console is quiet; the input task continues running.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/buttons -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/buttons -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/buttons -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) example_board: board init done (BSP rev repository-board-component)
I (…) example_input: input service started (BOOT=GPIO61, PWR=shared IRQ)
I (…) buttons: buttons example ready: short/long press BOOT, short press PWR
I (…) buttons: event: BOOT_SHORT
I (…) buttons: event: BOOT_LONG
I (…) buttons: event: PWR_SHORT
```

After `buttons example ready`, perform BOOT short, BOOT long (hold at least
800 ms), and PWR short presses; allow **15 s** for the three actions. Require
exactly one matching event for each, and no BOOT_SHORT after the long-press
release. There is no all-done marker for this continuous input example.
Readiness or silence is not button acceptance: a person or physical button
fixture is required. `RTC_ALARM` needs an independently armed RTC alarm; this
example does not manufacture one to claim that path passed.

## Constraints

- The example uses the IDF default partition table; no TF card, display, or
  PSRAM is required.
- Do not register a GPIO ISR on GPIO2 or GPIO61 from application code; both
  lines are owned by the input path described above.
