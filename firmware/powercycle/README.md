# Candis-S31 Power-Cycle Measurement Aid

A power-state cycle harness for board-level current measurements — a
measurement aid, not a product app (`main/powercycle_main.c`).

Each cycle:

1. **ACTIVE (10 s)** — display on, white full-screen frame, maximum
   brightness. Measures the screen-on running state.
2. **Low-power window** — PMIC state dumped over serial before and after
   `bsp_power_safe_state()`; the charger and fuel gauge are disabled for the
   window (they survive safe-state and would otherwise burn their quiescent
   current); then the SoC enters deep sleep with a 10 s timer wake.
3. Wake is a fresh reset, which starts the next cycle. The cycle repeats
   indefinitely.

A series meter (for example across the lifted C1 pad) reads the steady-state
current of each phase directly. The USB-serial bridge rail is re-asserted at
the top of every cycle so the console returns for each ACTIVE phase
(`idf.py monitor` reconnects on its own) while the low-power window stays
meter-only.

## Build and flash

```bash
idf.py --preview -C firmware/powercycle -D IDF_TARGET=esp32s31 build
idf.py --preview -C firmware/powercycle -D IDF_TARGET=esp32s31 flash monitor
```

工程通过仓库自有 `components/candis_s31/` 构建，通用驱动映射在
`vendor/idf-extra-components/`；不需要外部 BSP 检出。
