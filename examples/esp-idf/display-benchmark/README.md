# ESP-IDF display benchmark

Repeats four AMOLED refresh phases: fullscreen then partial with TE disabled,
followed by fullscreen then partial with TE enabled. Each phase lasts about
five seconds and reports its average frame rate:

- `fullscreen`: scrolling high-contrast stripes repaint the whole 460×460
  panel every frame;
- `partial`: a bouncing 48 px ball repaints a bounded region only.

Frame counting: a refresh cycle only counts at `LV_EVENT_REFR_READY` when it
contains a flush. FPS is
`(frames - 1) * 1000000 / interval_us`, using the first and last counted
refresh timestamps. This measures LVGL refresh throughput, not timer calls,
SPI chunks, or physical panel scans.

| Item | Current value |
|---|---|
| Target | ESP32-S31 preview target |
| Tested ESP-IDF | `v6.1-rc1` |
| Flash configuration | 16 MB, QIO 80 MHz, 32 MB PSRAM octal 250 MHz |
| Partition table | `partitions.csv` (single 8 MB app) |
| Managed components | None direct (LVGL via the board component) |
| Compile status | Verified |

## Behavior

- Runs each phase for about 5 s after a 500 ms warm-up, reporting the measured
  frame count, first-to-last interval, TE state, FPS, and `PASS` or `BELOW_TARGET`.
- Runs its animation and LVGL refresh timers at 1 ms with a 1 ms FreeRTOS tick.
- The on-screen label shows the number of refreshes in the latest stats tick.
- The pass thresholds are fullscreen >= 30 fps and partial >= 60 fps, compared
  on the unrounded FPS.

## Measured throughput

Two four-phase cycles on EVT1 with 48 MHz QSPI and two 460 × 48 internal-DMA
draw buffers:

| TE synchronization | Fullscreen refreshes/s | Partial refreshes/s |
|---|---|---|
| Off | 47.571 | 498.998–499.037 |
| On | 29.978–29.993 | 59.976–59.9996 |

The panel refreshes at about 59.9 Hz (TE period 16695 us), so with TE enabled
one frame is submitted per panel refresh and the TE-on fullscreen rate follows
the panel cadence.

Approximately 499 partial refreshes/s describes host-side LVGL updates to a
small region, which is a region refresh rate and not the panel scan rate.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/display-benchmark -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/display-benchmark -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -C examples/esp-idf/display-benchmark -p /dev/ttyACM0 monitor
```

## Expected serial output

```text
display_benchmark: fullscreen te=off fps=<measured> frames=<count> interval_us=<elapsed> target=30 result=<PASS|BELOW_TARGET>
display_benchmark: partial te=off fps=<measured> frames=<count> interval_us=<elapsed> target=60 result=<PASS|BELOW_TARGET>
display_benchmark: fullscreen te=on fps=<measured> frames=<count> interval_us=<elapsed> target=30 result=<PASS|BELOW_TARGET>
display_benchmark: partial te=on fps=<measured> frames=<count> interval_us=<elapsed> target=60 result=<PASS|BELOW_TARGET>
```

## Constraints

- Requires 32 MB PSRAM (the LVGL draw pipeline baseline refuses to boot
  without it).
- GPIO16 (TE) interrupt ownership belongs to the display stack; the
  benchmark changes synchronization through the display API and never
  registers its own ISR on that pin.
- Results depend on the 48 MHz QSPI, 48-line buffers, and LVGL settings in
  `sdkconfig.defaults`; changing them requires a new measurement.
