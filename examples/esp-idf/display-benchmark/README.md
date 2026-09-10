# ESP-IDF display benchmark

Repeats four AMOLED refresh phases: fullscreen then partial with TE disabled,
followed by fullscreen then partial with TE enabled. Each phase lasts about
five seconds and reports its average frame rate:

- `fullscreen`: scrolling high-contrast stripes repaint the whole 460×460
  panel every frame;
- `partial`: a bouncing 48 px ball repaints a bounded region only.

Frame counting follows the demo's display diagnostics: a refresh cycle only
counts at `LV_EVENT_REFR_READY` when it contains a flush. FPS is
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
| Hardware status | Run on EVT1 on 2026-09-07, 2026-09-08 and 2026-09-10; tear-free display acceptance remains open |

## Behavior

- Runs each phase for about 5 s after a 500 ms warm-up, reporting the measured
  frame count, first-to-last interval, TE state, FPS, and `PASS` or `BELOW_TARGET`.
- Uses 1 ms animation and LVGL refresh timers with a 1 ms FreeRTOS tick.
  Throughput is not capped by a 30/60 fps animation timer.
- The on-screen label shows the number of refreshes in the latest stats tick.
- Fullscreen >= 30 fps and partial >= 60 fps are minimum thresholds, not
  scheduling targets or upper limits. The comparison uses the unrounded FPS;
  a printed `60.000` can still be `BELOW_TARGET`.

## EVT1 measurements — 2026-09-07

The local bench record `display-unlimited.log` captured two four-phase cycles
with 48 MHz QSPI and two 460 × 48 internal-DMA draw buffers. The raw log is a
local validation artifact and is not shipped in the repository.

| TE synchronization | Fullscreen refreshes/s | Partial refreshes/s | Threshold result |
|---|---|---|---|
| Off | 47.571 | 498.998–499.037 | Both PASS |
| On | 29.978–29.993 | 59.976–59.9996 | Both BELOW_TARGET |

The TE-on partial sample with 300 frames over 4,983,366 us is approximately
59.9996 refreshes/s; the log prints `60.000` but correctly reports
`BELOW_TARGET`. Values below the minimum are not rounded up to PASS.

Approximately 499 partial refreshes/s describes host-side LVGL updates to a
small region. It does not mean the AMOLED scans at 499 Hz or displays every
update as a separate complete image. These measurements establish throughput;
neither TE setting has a tear-free visual acceptance claim.

The 2026-09-08 isolated-config rerun produced 47.170-47.229 fullscreen and
498.537-498.538 partial refreshes/s with TE off; with TE on it produced
29.973-29.987 fullscreen and 59.959-59.971 partial refreshes/s. The strict
TE-on thresholds remain unmet. The screen now labels each phase with
`FULL`/`PARTIAL` and `TE ON`/`TE OFF`; visual tearing acceptance is still open.

The 2026-09-10 rerun produced 47.122 fullscreen and 498.499 partial
refreshes/s with TE off, and 29.949 fullscreen and 59.854 partial with TE on.
A separate probe measured the panel TE period at 16695 us (59.897 Hz), so the
TE-on numbers are one submitted frame per panel refresh: the gap to the strict
30/60 thresholds is the panel refresh rate, not headroom left in the render
path. The thresholds are kept as written rather than rounded up.

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
