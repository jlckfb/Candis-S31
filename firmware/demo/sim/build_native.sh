#!/usr/bin/env bash
set -euo pipefail

SIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(cd "${SIM_DIR}/.." && pwd)"
BUILD_DIR="${1:-${DEMO_DIR}/build-sim}"

"${SIM_DIR}/prepare_lvgl.sh"
cmake -S "${SIM_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"
cmake --build "${BUILD_DIR}" \
    --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"

echo "Native simulator: ${BUILD_DIR}/candis_s31_sim"
