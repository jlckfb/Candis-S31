#!/usr/bin/env bash
# build-all.sh — Candis-S31 build baseline: compile ESP-IDF targets serially.
#
# Baseline ESP-IDF v6.1-rc1; esp32s31 is a *preview* target -> --preview.
#
# Default set (25 targets, every one buildable from a plain clone). The board
# implementation is first-party under components/candis_s31; reusable drivers
# are mirrored under vendor/idf-extra-components.
#   factory          firmware/factory
#   getting-started  examples/esp-idf/getting-started
#   low-power       examples/esp-idf/low-power (HP light-sleep state machine
#                    plus an LP core companion)
#   player           examples/esp-idf/player
#   display-hello    examples/esp-idf/display-hello (Board Manager path)
#   camera-test      examples/esp-idf/camera-test
#   power-cycle      examples/esp-idf/power-cycle
#   usb-cdc-device   examples/esp-idf/usb-cdc-device
#   buttons          examples/esp-idf/buttons (BOOT/PWR/RTC-alarm events)
#   led              examples/esp-idf/led
#   rtc              examples/esp-idf/rtc
#   pmic             examples/esp-idf/pmic
#   storage          examples/esp-idf/storage
#   display-touch    examples/esp-idf/display-touch
#   display-benchmark examples/esp-idf/display-benchmark
#   audio-recorder   examples/esp-idf/audio-recorder
#   audio-player     examples/esp-idf/audio-player
#   wifi             examples/esp-idf/wifi
#   ble              examples/esp-idf/ble
#   usb-host-msc     examples/esp-idf/usb-host-msc
#   testapp:<name>   board and reusable-driver test_apps
#
# Firmware projects resolve their remaining public dependencies from the ESP
# Component Registry on first build; committed dependencies.lock files pin the
# selected versions and hashes.
#
# There are no hidden maintainer-only targets. A target listed by --list is
# available from this checkout and is safe to run after a plain clone.
#
# Why not `set-target`: idf.py global options (-B/-D/--preview) must precede the
# action verb. This script uses `-D IDF_TARGET=esp32s31`, which is idempotent and
# avoids set-target renaming an existing sdkconfig as a side effect.
#
# Isolated mode (--isolated [targets...]): every selected target is built into
# its own build dir and sdkconfig under this run's log dir,
#   <RUN_DIR>/iso/<target>/build      and      <RUN_DIR>/iso/<target>/sdkconfig,
# generated from the project's sdkconfig.defaults as usual (-D SDKCONFIG=...).
# Project-local sdkconfigs are never read, copied, edited or deleted, so a
# stale or foreign configuration cannot leak into the result either. This is
# configuration isolation only, NOT a clean-clone check: dependency locks, the
# component registry cache and the shared IDF/tools installation are reused
# exactly as in default mode.
#
# Properties:
#   * Serial: one target at a time; a failure does not stop later targets.
#   * Idempotent: default-mode re-runs reuse the project build dirs
#     (incremental). A project-local sdkconfig configured for a different SoC
#     fails that target with a hint to use --isolated; the script never edits
#     or deletes it.
#   * Callable from any cwd; paths derive from the script location.
#     Project-local build dirs are covered by the `**/build/` ignore rule;
#     isolated outputs land under BUILD_LOG_ROOT (also git-ignored).
#   * --help and --list work without idf.py on PATH (no IDF activation).
#
# Usage:
#   tools/build-all.sh                          # all default targets
#   tools/build-all.sh getting-started factory  # subset (names from --list)
#   tools/build-all.sh --isolated               # all targets, isolated config
#   tools/build-all.sh --isolated wifi camera-test   # subset, isolated config
#   tools/build-all.sh --list                   # print target names
#
# Env overrides: CANDIS_WS, BUILD_LOG_ROOT, IDF_PATH, IDF_TOOLS_PATH.
# If idf.py is not in PATH, the script activates $CANDIS_WS/.tools/esp-idf itself.

set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CANDIS_REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)
CANDIS_WS=${CANDIS_WS:-$(cd -- "$CANDIS_REPO/../.." && pwd)}
BUILD_LOG_ROOT=${BUILD_LOG_ROOT:-$CANDIS_REPO/build-logs}
IDF_TARGET=esp32s31

die() { echo "build-all: ERROR: $*" >&2; exit 2; }

usage() {
    # Keep --help in sync with this header: print the leading comment block.
    awk 'NR == 1 { next } /^#/ { print; next } { exit }' "${BASH_SOURCE[0]}"
}

# --- target table ------------------------------------------------------------
FIRMWARE_TARGETS=(factory getting-started low-power player display-hello camera-test power-cycle usb-cdc-device buttons led rtc pmic storage display-touch display-benchmark audio-recorder audio-player wifi ble usb-host-msc)
TEST_APPS=(candis_s31 tg28_sw rx8130ce fusb303b cst820)

ALL_TARGETS=("${FIRMWARE_TARGETS[@]}")
for ta in "${TEST_APPS[@]}"; do ALL_TARGETS+=("testapp:$ta"); done

testapp_dir() {
    case "$1" in
        candis_s31) printf '%s\n' "$CANDIS_REPO/components/candis_s31/test_apps" ;;
        cst820) printf '%s\n' "$CANDIS_REPO/vendor/idf-extra-components/esp_lcd_touch_cst820/test_apps" ;;
        *)      printf '%s\n' "$CANDIS_REPO/vendor/idf-extra-components/$1/test_apps" ;;
    esac
}

firmware_dir() {
    case "$1" in
        factory) printf '%s\n' "$CANDIS_REPO/firmware/factory" ;;
        *)       printf '%s\n' "$CANDIS_REPO/examples/esp-idf/$1" ;;
    esac
}

# --- argument parsing (before IDF activation: --help/--list need no idf.py) ---
if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    usage
    exit 0
fi
if [ "${1:-}" = "--list" ]; then
    printf '%s\n' "${ALL_TARGETS[@]}"
    exit 0
fi

ISOLATED=0
SELECTED=()
for want in "$@"; do
    if [ "$want" = "--isolated" ]; then
        ISOLATED=1
        continue
    fi
    found=""
    for t in "${ALL_TARGETS[@]}"; do [ "$t" = "$want" ] && found=$t; done
    [ -n "$found" ] || die "unknown target '$want' (valid: ${ALL_TARGETS[*]})"
    SELECTED+=("$found")
done
if [ "${#SELECTED[@]}" -eq 0 ]; then
    SELECTED=("${ALL_TARGETS[@]}")
fi

# --- logging / accounting (RUN_DIR is created just before the run) -----------
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
    # Never copy, edit or delete a project-local sdkconfig. If one was
    # generated for a different SoC, `-D IDF_TARGET` cannot take effect over it
    # and the build would silently use the wrong configuration: fail this
    # target and point at --isolated instead of healing behind the user's back.
    if [ "$ISOLATED" -eq 0 ] && [ -f "$dir/sdkconfig" ] \
            && ! grep -q "^CONFIG_IDF_TARGET=\"$IDF_TARGET\"\$" "$dir/sdkconfig"; then
        echo "[$name] FAIL ($dir/sdkconfig was not configured for $IDF_TARGET and has been left untouched; re-run with --isolated for a throwaway per-target config, or fix the file yourself)" | tee -a "$SUMMARY"
        R_NAME+=("$name"); R_RESULT+=("FAIL"); R_DUR+=("-")
        FAIL=$((FAIL + 1)); FAILED_NAMES="$FAILED_NAMES $name"
        return
    fi
    local t0 t1 rc
    t0=$(date +%s)
    ( cd "$dir" && "$@" ) 2>&1 | tee "$log"
    local pipeline_status=("${PIPESTATUS[@]}")
    rc=${pipeline_status[0]}
    if [ "$rc" -eq 0 ] && [ "${pipeline_status[1]}" -ne 0 ]; then
        rc=${pipeline_status[1]}
    fi
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
    local name=$1 dir
    case "$name" in
        testapp:*) dir=$(testapp_dir "${name#testapp:}") ;;
        *)         dir=$(firmware_dir "$name") ;;
    esac

    local cmd=(idf.py --preview)
    if [ "$ISOLATED" -eq 1 ]; then
        # Per-target build dir + sdkconfig under this run's log dir, both
        # absolute so they do not depend on the cwd the script was called
        # from. CMake bakes SDKCONFIG into its build dir, so each target gets
        # one iso dir holding the pair.
        local iso="$RUN_DIR/iso/${name//:/_}"
        mkdir -p "$iso/build" || die "cannot create $iso/build"
        cmd+=(-B "$iso/build" -D "SDKCONFIG=$iso/sdkconfig")
    else
        # Default mode keeps the historical layout: testapps build into a
        # target-suffixed dir inside the project, firmware into ./build.
        case "$name" in testapp:*) cmd+=(-B build_esp32s31) ;; esac
    fi
    cmd+=(-D IDF_TARGET=$IDF_TARGET build)

    run_target "$name" "$dir" "${cmd[@]}"
}

# --- IDF environment ---------------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    export IDF_PATH=${IDF_PATH:-$CANDIS_WS/.tools/esp-idf}
    export IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-$CANDIS_WS/.tools/idf-tools}
    [ -f "$IDF_PATH/export.sh" ] || die "idf.py not in PATH and $IDF_PATH/export.sh missing"
    # shellcheck disable=SC1091
    . "$IDF_PATH/export.sh" >/dev/null 2>&1 || die "sourcing $IDF_PATH/export.sh failed"
fi
command -v idf.py >/dev/null 2>&1 || die "idf.py still not available after activation"

# --- run -----------------------------------------------------------------------
RUN_DIR=$BUILD_LOG_ROOT/build-all-$(date +%Y%m%d-%H%M%S)-$$
mkdir -p "$RUN_DIR" || die "cannot create $RUN_DIR"
RUN_DIR=$(cd -- "$RUN_DIR" && pwd) || die "cannot resolve $RUN_DIR"
ln -sfn "$RUN_DIR" "$BUILD_LOG_ROOT/latest"
SUMMARY=$RUN_DIR/summary.txt
: > "$SUMMARY"

[ "$ISOLATED" -eq 1 ] && echo "build-all: isolated mode: per-target build dir + sdkconfig under $RUN_DIR/iso"
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
