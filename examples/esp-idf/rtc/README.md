# ESP-IDF RTC

Exercises the RX8130CE real-time clock: calendar read (optional fixed-time
write), then a minute-compare alarm reported over the TG28/RX8130CE shared
IRQ line (GPIO2).

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, DIO 40 MHz |
| Managed components | None direct; public registry dependencies via the repository board component |
| Compile status | Verified |

## Behavior

- On boot the calendar is read and printed with the retained status flags.
- With `CONFIG_EXAMPLE_RTC_SET_ON_BOOT=y` (default `n`) the clock is first
  written to 2026-01-01 00:00:00; keep it disabled to preserve the
  battery-backed time.
- The alarm is armed for the next minute boundary (the RX8130CE compares
  minute/hour/day fields only, so this is the fastest self-contained target)
  and reported both through the shared-IRQ input path and a status-flag poll
  fallback.
- After fire or timeout, disables the alarm IRQ, disarms the compare, clears
  AF, then prints `RTC alarm fired` or the timeout error and ends.
  Capture **75 s** from reset (wait budget is 60 − current seconds + 8 s).
  No operator action is needed with a valid running RTC.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/rtc -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/rtc -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/rtc -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
I (…) rtc: time: 2026-09-05 05:12:34 flags=0x00
I (…) rtc: alarm armed for minute 13; waiting up to 34 s
I (…) rtc: RTC alarm fired
```

## Constraints

- The example uses the IDF default partition table; no TF card, display, or
  PSRAM is required.
- The RX8130CE runs from its internal oscillator; this board carries no external 32.768 kHz crystal.
- The alarm compares minute/hour/day fields; sub-minute alarms are not expressible.
