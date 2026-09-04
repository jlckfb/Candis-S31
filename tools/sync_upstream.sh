#!/usr/bin/env bash
# Refresh vendor/idf-extra-components and vendor/esp-board-manager from the
# local upstream-preparation checkouts.
#
# Usage:
#   tools/sync_upstream.sh                 # uses $CANDIS_WS/espressif-Prj/{idf-extra-components,esp-board-manager}
#   IEC_ROOT=/path/to/idf-extra-components EBM_ROOT=/path/to/esp-board-manager tools/sync_upstream.sh
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$HERE/.." && pwd)"
CANDIS_WS=${CANDIS_WS:-$(cd -- "$REPO_ROOT/../.." && pwd)}
IEC_ROOT=${IEC_ROOT:-$CANDIS_WS/espressif-Prj/idf-extra-components}
EBM_ROOT=${EBM_ROOT:-$CANDIS_WS/espressif-Prj/esp-board-manager}
VENDOR_IEC="$REPO_ROOT/vendor/idf-extra-components"
VENDOR_EBM="$REPO_ROOT/vendor/esp-board-manager"

DRIVERS="esp_lcd_touch_cst820 fusb303b rx8130ce tg28_sw"

for d in $DRIVERS; do
    if [ ! -f "$IEC_ROOT/$d/idf_component.yml" ]; then
        echo "sync_upstream: no $d component at $IEC_ROOT" >&2
        exit 1
    fi
done
if [ ! -f "$EBM_ROOT/esp_board_manager/idf_component.yml" ] ||
   [ ! -f "$EBM_ROOT/esp_friends_boards/candis_s31/board_info.yaml" ]; then
    echo "sync_upstream: no Board Manager checkout at $EBM_ROOT" >&2
    exit 1
fi

# `--delete-excluded` drops files excluded by these rules that earlier syncs
# left behind. Stale lock files can record checkout-local override paths and
# break builds from the repository snapshot.
RSYNC=(rsync -a --delete --delete-excluded
       --exclude='build*/' --exclude='sdkconfig' --exclude='sdkconfig.old'
       --exclude='dependencies.lock' --exclude='.git' --exclude='.github'
       --exclude='managed_components/' --exclude='__pycache__/'
       --exclude='.pytest_cache/' --exclude='node_modules/')

# Reusable Candis-S31 drivers staged for espressif/idf-extra-components.
# Keep examples/ and test_apps/: the examples are the get-started evidence
# attached to each driver, and the test apps are the acceptance gate.
mkdir -p "$VENDOR_IEC"
for d in $DRIVERS; do
    "${RSYNC[@]}" "$IEC_ROOT/$d" "$VENDOR_IEC/"
done

# ESP Board Manager mirror: the core component (code generator included) and
# the esp_friends_boards package that carries candis_s31. test_apps/, docs/
# and examples/ of the core component stay upstream (multi-MB build
# artifacts); esp_boards and m5stack_boards are official registry packages
# unrelated to Candis-S31 and are not mirrored.
mkdir -p "$VENDOR_EBM"
"${RSYNC[@]}" --exclude='test_apps/' --exclude='docs/' --exclude='examples/' \
    "$EBM_ROOT/esp_board_manager" "$VENDOR_EBM/"
"${RSYNC[@]}" "$EBM_ROOT/esp_friends_boards" "$VENDOR_EBM/"

stamp() { # stamp <source-label> <checkout> <out-file>
    local label=$1 root=$2 out=$3 commit dirty
    commit=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)
    dirty=$(git -C "$root" status --porcelain --untracked-files=no 2>/dev/null | wc -l)
    {
        echo "source: $label"
        echo "commit: $commit"
        echo "dirty_files: $dirty"
        echo "synced_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$out"
}
stamp "LeenixP/idf-extra-components (local worktree)" "$IEC_ROOT" "$VENDOR_IEC/SOURCE_COMMIT"
stamp "LeenixP/esp-board-manager (local worktree)" "$EBM_ROOT" "$VENDOR_EBM/SOURCE_COMMIT"

echo "sync_upstream: vendored $(git -C "$IEC_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown) into $VENDOR_IEC"
echo "sync_upstream: vendored $(git -C "$EBM_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown) into $VENDOR_EBM"
