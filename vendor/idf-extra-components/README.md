# Vendored idf-extra-components driver snapshot

This directory is a **vendored mirror** of the four reusable Candis-S31
device drivers staged for submission to
[`espressif/idf-extra-components`](https://github.com/espressif/idf-extra-components):

- `esp_lcd_touch_cst820` — CST820 capacitive touch controller
- `fusb303b` — FUSB303B USB Type-C port controller
- `rx8130ce` — RX8130CE real-time clock
- `tg28_sw` — TG28 software fuel gauge / battery model

Source of truth during upstream preparation:
`LeenixP/idf-extra-components`, branch `feat/candis-s31-drivers`.
The submission pull requests are intentionally not opened yet; refresh this
snapshot after driver changes land there:

```bash
tools/sync_upstream.sh            # or: IEC_ROOT=/path/to/idf-extra-components tools/sync_upstream.sh
```

Each driver keeps its upstream layout, including `examples/` (per-driver
get-started evidence) and `test_apps/` (the acceptance gate). The snapshot
metadata lives in `vendor/idf-extra-components/SOURCE_COMMIT`.

The repository-owned board runtime at `components/candis_s31/` resolves these
drivers through relative `override_path` entries. There is no second copy in
an external BSP checkout; this snapshot is the only driver source consumed by the
repository's firmware projects.
