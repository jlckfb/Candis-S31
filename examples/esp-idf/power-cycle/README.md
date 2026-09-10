# Candis-S31 Power-Cycle Measurement Aid

A power-state cycle harness for board-level current measurements — a
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

The final-source 75-second EVT1 run (2026-09-10) recorded four ACTIVE
entries and three complete safe-state / timer deep-sleep transitions.
All three safe-state calls returned `ESP_OK`; timer wake restarted the next
cycle. White-screen appearance and meter-backed current values remain separate
physical acceptance items.

For a console-only cycle check, capture **75 s** from reset without touching
the board. A cycle takes 10 s ACTIVE + 10 s deep sleep **plus** reboot,
display initialization and PMIC/teardown overhead; it is not exactly 20 s.
Require at least three `ACTIVE` entries with two intervening
`safe-state returned ESP_OK` → `deep sleep: timer wake in 10000 ms` sequences
and timer wake causes on the next boots. There is no finite "all cycles done"
marker: the image intentionally repeats forever. A display/teardown error
is not a passing cycle, and console evidence does not replace the white-screen
visual check or a meter reading.

Earlier revisions disabled charging and the fuel gauge through persistent
TG28 registers. Reflashing alone does not restore those enables. If an older
measurement image has been run, verify the PMIC state before testing charging;
this example deliberately does not override an existing charger policy.

## Build and flash

```bash
idf.py --preview -C examples/esp-idf/power-cycle -D IDF_TARGET=esp32s31 build
idf.py --preview -C examples/esp-idf/power-cycle -D IDF_TARGET=esp32s31 flash monitor
```

工程通过仓库自有 `components/candis_s31/` 构建，通用驱动映射在
`vendor/idf-extra-components/`；不需要外部 BSP 检出。
