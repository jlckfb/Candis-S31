# Candis-S31 Power-Cycle Measurement Aid

A power-state cycle example for board-level current measurements — a
measurement aid, not a product app (`main/powercycle_main.c`).

Each cycle:

1. **ACTIVE (10 s)** — display on, white full-screen frame, maximum
   brightness. Measures the screen-on running state.
2. **Low-power window** — PMIC state dumped over serial before and after
   `bsp_display_stop()` and `bsp_power_safe_state()`; charger and fuel-gauge enables are preserved;
   then the SoC enters deep sleep with a 10 s timer wake. Measurements include
   those PMIC blocks and any battery charging current, not just the SoC.
3. Wake is a fresh reset, which starts the next cycle. The cycle repeats
   indefinitely.

A series meter (for example across the lifted C1 pad) reads the steady-state
current of each phase directly. C1 is the USB-serial debug connector. The C2
OTG boost switch stays off throughout the example; it is not the debug bridge
rail. Deep-sleep measurements are meter-only while the SoC is asleep.

For a console-only cycle check, capture **75 s** from reset without touching
the board. A cycle takes 10 s ACTIVE + 10 s deep sleep **plus** reboot,
display initialization and PMIC/teardown overhead; it is not exactly 20 s.
Require at least three `ACTIVE` entries with two intervening
`safe-state returned ESP_OK` → `deep sleep: timer wake in 10000 ms` sequences
and timer wake causes on the next boots. There is no finite "all cycles done"
marker: the image intentionally repeats forever. A display/teardown error
is not a passing cycle, and console evidence does not replace the white-screen
visual check or a meter reading.

The charger and fuel-gauge policy lives in persistent TG28 registers that
survive reflashing, and this example deliberately does not override an
existing policy: verify the PMIC state before testing charging if an earlier
image left those blocks disabled.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/power-cycle -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/power-cycle -D IDF_TARGET=esp32s31 flash monitor
```

The project builds through the repository-owned `components/candis_s31/`; the
reusable drivers are mapped under `vendor/idf-extra-components/`, so no
external BSP checkout is needed.
