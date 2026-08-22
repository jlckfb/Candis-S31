#!/usr/bin/env bash
# build-all.sh — Candis-S31 build baseline: compile all 15 ESP-IDF targets serially.
#
# Targets (baseline ESP-IDF v6.1-beta1; esp32s31 is a *preview* target -> --preview):
#   example:<name>   7 esp-bsp examples (display, display_camera_video,
#                    display_lvgl_demos, display_lvgl_benchmark, display_sdcard,
#                    audio, display_audio_photo), built with
#                    the esp-bsp convention
#                    `-D SDKCONFIG_DEFAULTS=sdkconfig.bsp.candis_s31`,
#                    build dir `build_candis_s31/` inside each example.
#   testapp:<name>   4 driver component test_apps (tg28_sw, rx8130ce, fusb303b,
#                    cst820), build dir `build_esp32s31/`.
#   factory          firmware/factory, needs CANDIS_S31_BSP_PATH pointing at the
#                    local esp-bsp worktree (bsp/candis_s31). Default build/ dir.
#   getting-started  examples/esp-idf/getting-started. Default build/ dir.
#   low-power        examples/esp-idf/low-power, also needs CANDIS_S31_BSP_PATH
#                    (HP light-sleep state machine plus an LP core companion).
#
# Why not `set-target`: idf.py global options (-B/-D/--preview) must precede the
# action verb. The 2026-07-31 ws1 script wrote `set-target esp32s31 -B dir build`,
# which idf.py rejected ("No such option '-B'") or forwarded to ninja, so 10/12
# targets never compiled — and set-target still renamed existing sdkconfig files
# to sdkconfig.old as a side effect. This script never calls set-target: the SoC
# comes from `-D IDF_TARGET=esp32s31` (plus the self-contained
# sdkconfig.bsp.candis_s31 for BSP examples), which is idempotent.
#
# Properties:
#   * Serial: one target at a time; a failure does not stop later targets.
#   * Idempotent: re-runs reuse build dirs (incremental). A stale git-ignored
#     sdkconfig configured for a different SoC is deleted so defaults re-apply.
#   * Callable from any cwd; paths derive from the script location.
#   * Logs stay outside every git worktree:
#     $BUILD_LOG_ROOT/build-all-<yyyymmdd-HHMMSS>/<target>.log + summary.txt,
#     with a `latest` symlink. Build dir names above are covered by the
#     respective .gitignore files (esp-bsp: `build*`; this repo: `**/build/`).
#
# Usage:
#   tools/build-all.sh                          # all 15 targets
#   tools/build-all.sh getting-started factory  # subset (names from --list)
#   tools/build-all.sh --list                   # print target names
#
# Env overrides: CANDIS_WS, ESP_BSP_ROOT, BUILD_LOG_ROOT, IDF_PATH, IDF_TOOLS_PATH.
# If idf.py is not in PATH, the script activates $CANDIS_WS/.tools/esp-idf itself.

set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CANDIS_REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)
CANDIS_WS=${CANDIS_WS:-$(cd -- "$CANDIS_REPO/../.." && pwd)}
ESP_BSP_ROOT=${ESP_BSP_ROOT:-$CANDIS_WS/espressif-Prj/esp-bsp}
BUILD_LOG_ROOT=${BUILD_LOG_ROOT:-$CANDIS_WS/build-logs}
IDF_TARGET=esp32s31

die() { echo "build-all: ERROR: $*" >&2; exit 2; }

# --- IDF environment -------------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    export IDF_PATH=${IDF_PATH:-$CANDIS_WS/.tools/esp-idf}
    export IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-$CANDIS_WS/.tools/idf-tools}
    [ -f "$IDF_PATH/export.sh" ] || die "idf.py not in PATH and $IDF_PATH/export.sh missing"
    # shellcheck disable=SC1091
    . "$IDF_PATH/export.sh" >/dev/null 2>&1 || die "sourcing $IDF_PATH/export.sh failed"
fi
command -v idf.py >/dev/null 2>&1 || die "idf.py still not available after activation"

# --- target table ------------------------------------------------------------
BSP_EXAMPLES=(display display_camera_video display_lvgl_demos display_lvgl_benchmark display_sdcard audio display_audio_photo)
TEST_APPS=(tg28_sw rx8130ce fusb303b cst820)

ALL_TARGETS=()
for ex in "${BSP_EXAMPLES[@]}"; do ALL_TARGETS+=("example:$ex"); done
for ta in "${TEST_APPS[@]}"; do ALL_TARGETS+=("testapp:$ta"); done
ALL_TARGETS+=(factory getting-started low-power)

testapp_dir() {
    case "$1" in
        cst820) printf '%s\n' "$ESP_BSP_ROOT/components/lcd_touch/esp_lcd_touch_cst820/test_apps" ;;
        *)      printf '%s\n' "$ESP_BSP_ROOT/components/$1/test_apps" ;;
    esac
}

# --- logging / accounting (RUN_DIR is created after argument parsing) ----------
PASS=0; FAIL=0; FAILED_NAMES=""
declare -a R_NAME R_RESULT R_DUR

fmt_dur() { local s=$1; printf '%dm%02ds' $((s / 60)) $((s % 60)); }

run_target() {
    local name=$1 dir=$2; shift 2
    local log="$RUN_DIR/${name//:/_}.log"
    echo "=== [$name] $dir" | tee -a "$SUMMARY"
    if [ ! -d "$dir" ]; then
        echo "[$name] FAIL (missing dir: $dir)" | tee -a "$SUMMARY"
        R_NAME+=("$name"); R_RESULT+=("FAIL"); R_DUR+=("-")
        FAIL=$((FAIL + 1)); FAILED_NAMES="$FAILED_NAMES $name"
        return
    fi
    # Self-heal: drop a git-ignored sdkconfig left over from a different SoC so
    # that SDKCONFIG_DEFAULTS / -D IDF_TARGET below actually take effect.
    if [ -f "$dir/sdkconfig" ] && ! grep -q "^CONFIG_IDF_TARGET=\"$IDF_TARGET\"\$" "$dir/sdkconfig"; then
        rm -f "$dir/sdkconfig"
        echo "[$name] note: removed stale sdkconfig (CONFIG_IDF_TARGET != $IDF_TARGET)" | tee -a "$SUMMARY"
    fi
    local t0 t1 rc
    t0=$(date +%s)
    ( cd "$dir" && "$@" ) >"$log" 2>&1
    rc=$?
    t1=$(date +%s)
    if [ "$rc" -eq 0 ]; then
        echo "[$name] PASS ($(fmt_dur $((t1 - t0)))) log: $log" | tee -a "$SUMMARY"
        R_NAME+=("$name"); R_RESULT+=("PASS"); R_DUR+=("$(fmt_dur $((t1 - t0)))")
        PASS=$((PASS + 1))
    else
        echo "[$name] FAIL (rc=$rc, $(fmt_dur $((t1 - t0)))) log: $log" | tee -a "$SUMMARY"
        R_NAME+=("$name"); R_RESULT+=("FAIL"); R_DUR+=("$(fmt_dur $((t1 - t0)))")
        FAIL=$((FAIL + 1)); FAILED_NAMES="$FAILED_NAMES $name"
    fi
}

build_one() {
    local name=$1
    case "$name" in
        example:*)
            local ex=${name#example:}
            run_target "$name" "$ESP_BSP_ROOT/examples/$ex" \
                idf.py --preview -B build_candis_s31 \
                       -D IDF_TARGET=$IDF_TARGET \
                       -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.candis_s31 build
            ;;
        testapp:*)
            local ta=${name#testapp:}
            run_target "$name" "$(testapp_dir "$ta")" \
                idf.py --preview -B build_esp32s31 -D IDF_TARGET=$IDF_TARGET build
            ;;
        factory)
            run_target "$name" "$CANDIS_REPO/firmware/factory" \
                env CANDIS_S31_BSP_PATH="$ESP_BSP_ROOT/bsp/candis_s31" \
                idf.py --preview -D IDF_TARGET=$IDF_TARGET build
            ;;
        getting-started)
            run_target "$name" "$CANDIS_REPO/examples/esp-idf/getting-started" \
                idf.py --preview -D IDF_TARGET=$IDF_TARGET build
            ;;
        low-power)
            run_target "$name" "$CANDIS_REPO/examples/esp-idf/low-power" \
                env CANDIS_S31_BSP_PATH="$ESP_BSP_ROOT/bsp/candis_s31" \
                idf.py --preview -D IDF_TARGET=$IDF_TARGET build
            ;;
        *) die "unknown target: $name" ;;
    esac
}

# --- argument parsing ----------------------------------------------------------
if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    sed -n '2,43p' "${BASH_SOURCE[0]}"
    exit 0
fi
if [ "${1:-}" = "--list" ]; then
    printf '%s\n' "${ALL_TARGETS[@]}"
    exit 0
fi

SELECTED=()
if [ "$#" -gt 0 ]; then
    for want in "$@"; do
        found=""
        for t in "${ALL_TARGETS[@]}"; do [ "$t" = "$want" ] && found=$t; done
        [ -n "$found" ] || die "unknown target '$want' (valid: ${ALL_TARGETS[*]})"
        SELECTED+=("$found")
    done
else
    SELECTED=("${ALL_TARGETS[@]}")
fi

# --- run -----------------------------------------------------------------------
RUN_DIR=$BUILD_LOG_ROOT/build-all-$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN_DIR" || die "cannot create $RUN_DIR"
ln -sfn "$RUN_DIR" "$BUILD_LOG_ROOT/latest"
SUMMARY=$RUN_DIR/summary.txt
: > "$SUMMARY"

echo "build-all: $(date '+%F %T')  idf=$(idf.py --version 2>/dev/null)  targets=${#SELECTED[@]}"
echo "build-all: logs -> $RUN_DIR"
for t in "${SELECTED[@]}"; do
    build_one "$t"
done

{
    echo "------------------------------------------------------------"
    printf '%-32s %-6s %s\n' TARGET RESULT TIME
    for i in "${!R_NAME[@]}"; do
        printf '%-32s %-6s %s\n' "${R_NAME[$i]}" "${R_RESULT[$i]}" "${R_DUR[$i]}"
    done
    echo "------------------------------------------------------------"
    echo "TOTAL pass=$PASS fail=$FAIL${FAILED_NAMES:+  failed:$FAILED_NAMES}"
} | tee -a "$SUMMARY"

[ "$FAIL" -eq 0 ]
