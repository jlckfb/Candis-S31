# Vendored Candis-S31 BSP snapshot

This directory is a **vendored copy** of the Candis-S31 board support package
and its local component dependencies, so every firmware project in this
repository builds after a plain clone with only an ESP-IDF environment
installed — no external checkouts, no environment variables.

Source of truth during upstream development:
`LeenixP/esp-bsp`, branch `feat/candis-s31`.

Refresh this snapshot after BSP or component changes land there:

```bash
tools/sync_bsp.sh            # or: ESP_BSP_ROOT=/path/to/esp-bsp tools/sync_bsp.sh
```

The script copies `bsp/candis_s31` and `components/{tg28_sw, rx8130ce,
fusb303b, lcd_touch/esp_lcd_touch_cst820, esp_lvgl_port}` from the checkout,
excluding build artifacts, and records the source commit in
`vendor/esp-bsp/SOURCE_COMMIT`.

The BSP does not redistribute a battery-model blob. It verifies the TG28
factory ROM model at initialization; applications must provide a compatible
licensed model explicitly if they replace it.
