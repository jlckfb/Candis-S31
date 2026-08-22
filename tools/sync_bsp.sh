#!/usr/bin/env bash
# Refresh vendor/esp-bsp from the development esp-bsp checkout.
#
# Usage:
#   tools/sync_bsp.sh                 # uses $CANDIS_WS/espressif-Prj/esp-bsp
#   ESP_BSP_ROOT=/path/to/esp-bsp tools/sync_bsp.sh
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$HERE/.." && pwd)"
CANDIS_WS=${CANDIS_WS:-$(cd -- "$REPO_ROOT/../.." && pwd)}
ESP_BSP_ROOT=${ESP_BSP_ROOT:-$CANDIS_WS/espressif-Prj/esp-bsp}
VENDOR="$REPO_ROOT/vendor/esp-bsp"

if [ ! -f "$ESP_BSP_ROOT/bsp/candis_s31/include/bsp/candis_s31.h" ]; then
    echo "sync_bsp: no Candis-S31 BSP at $ESP_BSP_ROOT" >&2
    exit 1
fi

RSYNC=(rsync -a --delete
       --exclude='build*/' --exclude='sdkconfig' --exclude='dependencies.lock'
       --exclude='.git' --exclude='managed_components/')

mkdir -p "$VENDOR/bsp" "$VENDOR/components/lcd_touch"
"${RSYNC[@]}" "$ESP_BSP_ROOT/bsp/candis_s31" "$VENDOR/bsp/"
for comp in tg28_sw rx8130ce fusb303b esp_lvgl_port; do
    "${RSYNC[@]}" --exclude='examples/' "$ESP_BSP_ROOT/components/$comp" "$VENDOR/components/"
done
"${RSYNC[@]}" "$ESP_BSP_ROOT/components/lcd_touch/esp_lcd_touch_cst820" "$VENDOR/components/lcd_touch/"

{
    git -C "$ESP_BSP_ROOT" rev-parse HEAD 2>/dev/null || echo "unknown"
    git -C "$ESP_BSP_ROOT" status --porcelain --untracked-files=no >/dev/null 2>&1 || true
} > /dev/null
COMMIT=$(git -C "$ESP_BSP_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
DIRTY=$(git -C "$ESP_BSP_ROOT" status --porcelain --untracked-files=no 2>/dev/null | wc -l)
{
    echo "source: $ESP_BSP_ROOT"
    echo "commit: $COMMIT"
    echo "dirty_files: $DIRTY"
    echo "synced_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$VENDOR/SOURCE_COMMIT"

echo "sync_bsp: vendored $COMMIT (dirty files: $DIRTY) into $VENDOR"
