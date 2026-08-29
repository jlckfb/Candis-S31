#!/usr/bin/env bash
set -euo pipefail

SIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(cd "${SIM_DIR}/.." && pwd)"
BUILD_DIR="${1:-${DEMO_DIR}/build-web}"

if ! command -v emcmake >/dev/null 2>&1 || ! command -v emmake >/dev/null 2>&1; then
    echo "Emscripten is required: install the emscripten package first." >&2
    exit 1
fi

"${SIM_DIR}/prepare_lvgl.sh"

export EM_FROZEN_CACHE=0
export EM_CACHE="${EM_CACHE:-${XDG_CACHE_HOME:-${HOME}/.cache}/candis-s31-emscripten}"
mkdir -p "${EM_CACHE}"

emcmake cmake -S "${SIM_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
emmake cmake --build "${BUILD_DIR}" \
    --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"

echo "Browser preview: ${BUILD_DIR}/candis_s31_sim.html"
