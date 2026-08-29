#!/usr/bin/env bash
set -euo pipefail

DEMO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LVGL_ROOT="${DEMO_ROOT}/managed_components/lvgl__lvgl"
EXPECTED_HASH="184e532558c1c45fefed631f3e235423d22582aafb4630f3e8885c35281a49ae"

if [[ ! -f "${LVGL_ROOT}/lv_version.h" ]]; then
    if ! command -v idf.py >/dev/null 2>&1; then
        CANDIS_WS="${CANDIS_WS:-$(cd "${DEMO_ROOT}/../../../.." && pwd)}"
        IDF_ROOT="${CANDIS_WS}/.tools/esp-idf"
        IDF_TOOLS_ROOT="${CANDIS_WS}/.tools/idf-tools"
        if [[ -f "${IDF_ROOT}/export.sh" ]]; then
            export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-${IDF_TOOLS_ROOT}}"
            # shellcheck disable=SC1091
            source "${IDF_ROOT}/export.sh" >/dev/null 2>&1
        fi
    fi
    if ! command -v idf.py >/dev/null 2>&1; then
        echo "idf.py is required to restore managed_components/lvgl__lvgl" >&2
        exit 1
    fi
    if [[ -f "${DEMO_ROOT}/sdkconfig" ]]; then
        echo "LVGL tree missing; running ESP-IDF component reconfigure"
        (
            cd "${DEMO_ROOT}"
            idf.py --preview reconfigure
        )
    else
        echo "LVGL tree missing; initializing ESP32-S31 target and components"
        (
            cd "${DEMO_ROOT}"
            idf.py --preview set-target esp32s31
        )
    fi
fi

if [[ ! -f "${LVGL_ROOT}/lv_version.h" ]]; then
    echo "LVGL tree was not restored at ${LVGL_ROOT}" >&2
    exit 1
fi

major="$(sed -n 's/^[[:space:]]*#define LVGL_VERSION_MAJOR[[:space:]]*//p' "${LVGL_ROOT}/lv_version.h")"
minor="$(sed -n 's/^[[:space:]]*#define LVGL_VERSION_MINOR[[:space:]]*//p' "${LVGL_ROOT}/lv_version.h")"
patch="$(sed -n 's/^[[:space:]]*#define LVGL_VERSION_PATCH[[:space:]]*//p' "${LVGL_ROOT}/lv_version.h")"
if [[ "${major}.${minor}.${patch}" != "9.5.0" ]]; then
    echo "Expected firmware LVGL 9.5.0, found ${major}.${minor}.${patch}" >&2
    echo "Set CANDIS_LVGL_ROOT to an exact 9.5.0 snapshot instead of fetching another version." >&2
    exit 1
fi

if [[ ! -f "${LVGL_ROOT}/.component_hash" ]]; then
    echo "Missing .component_hash in ${LVGL_ROOT}" >&2
    exit 1
fi
hash="$(sed -n '1p' "${LVGL_ROOT}/.component_hash" | tr -d '[:space:]')"
if [[ "${hash}" != "${EXPECTED_HASH}" ]]; then
    echo "LVGL component hash differs from the firmware lock: ${hash}" >&2
    exit 1
fi

echo "Using firmware LVGL ${major}.${minor}.${patch} at ${LVGL_ROOT}"
echo "Using firmware LVGL component hash ${hash}"
