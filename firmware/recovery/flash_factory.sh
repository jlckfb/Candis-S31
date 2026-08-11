#!/usr/bin/env bash

set -euo pipefail

# Run this from the configured tmux `idf` session so the ESP-IDF esptool and
# Python environment are active. Arguments:
# PORT [MERGED_IMAGE] [BAUD] GPIO61_DOWNLOAD_LEVEL GPIO61_RUN_LEVEL.
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
port=${1:-PORT}
image=${2:-"${script_dir}/../factory/release/candis_s31_factory_merged.bin"}
# Default to the conservative baud: the UART0 series resistors (998 ohm TX /
# 499 ohm RX) are unverified on EVT1. Raise to 460800 only after the download
# path passes at the intended high baud.
baud=${3:-115200}
download_level=${4:-}
run_level=${5:-}

if [[ "${port}" == "PORT" ]]; then
    echo "usage: $0 PORT [MERGED_IMAGE] [BAUD] GPIO61_DOWNLOAD_LEVEL GPIO61_RUN_LEVEL" >&2
    echo "example: $0 /dev/ttyACM0 ./candis_s31_factory_merged.bin 115200 0 1" >&2
    exit 2
fi
if [[ "${download_level}" != "0" || "${run_level}" != "1" ]]; then
    echo "refusing to flash without explicit GPIO61 levels: 0 in ROM download, 1 for post-flash SPI boot" >&2
    exit 2
fi
if [[ ! -f "${image}" ]]; then
    echo "factory image not found: ${image}" >&2
    exit 2
fi

# The esptool.py shim is deprecated; prefer the esptool entry point and fall
# back to the module form.
if command -v esptool >/dev/null 2>&1; then
    esptool=(esptool)
elif python -m esptool version >/dev/null 2>&1; then
    esptool=(python -m esptool)
else
    echo "esptool is unavailable; activate the tmux idf environment first" >&2
    exit 2
fi

version_output=$("${esptool[@]}" version 2>&1)
version=$(sed -nE 's/[^0-9]*([0-9]+\.[0-9]+(\.[0-9]+)?).*/\1/p' <<<"${version_output}" | head -n 1)
if [[ -z "${version}" ]]; then
    echo "cannot parse esptool version: ${version_output}" >&2
    exit 2
fi
IFS=. read -r version_major version_minor version_patch <<<"${version}"
if (( version_major < 5 || (version_major == 5 && version_minor < 3) )); then
    echo "esptool ${version} is too old; Candis-S31 requires >= 5.3.0" >&2
    exit 2
fi

checksum_file="${image}.sha256"
if [[ ! -f "${checksum_file}" ]]; then
    echo "checksum file not found: ${checksum_file}" >&2
    exit 2
fi
(
    cd -- "$(dirname -- "${image}")"
    sha256sum -c "$(basename -- "${checksum_file}")"
)

echo "Preflight: ESP32-S31 ROM loader on ${port}"
"${esptool[@]}" --chip esp32s31 --port "${port}" \
    --after no-reset chip-id

echo "Flashing merged factory image at offset 0x0"
"${esptool[@]}" --chip esp32s31 --port "${port}" --baud "${baud}" \
    --before no-reset --after no-reset write-flash \
    --flash-mode dio --flash-freq 40m --flash-size 16MB \
    0x0 "${image}"

echo "Verifying merged factory image against flash before reset"
"${esptool[@]}" --chip esp32s31 --port "${port}" --baud "${baud}" \
    --before no-reset --after no-reset verify-flash \
    --flash-mode dio --flash-freq 40m --flash-size 16MB \
    0x0 "${image}"

echo "GPIO61 run level was explicitly confirmed as 1; releasing ROM download mode"
"${esptool[@]}" --chip esp32s31 --port "${port}" \
    --before no-reset --after hard-reset run
echo "Capture UART0 output and require an SPI boot log plus Factory startup before accepting recovery"
