#!/usr/bin/env bash
set -euo pipefail

SIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(cd "${SIM_DIR}/.." && pwd)"
BUILD_DIR="${1:-${DEMO_DIR}/build-web}"
PORT="${PORT:-8080}"

if [[ ! -f "${BUILD_DIR}/candis_s31_sim.html" ]]; then
    echo "Missing ${BUILD_DIR}/candis_s31_sim.html. Run sim/build_web.sh first." >&2
    exit 1
fi

# Serves with COOP/COEP headers (Emscripten pthreads requirement) and
# no-store caching so a rebuild is visible after a plain browser refresh.
exec python3 "${SIM_DIR}/web/serve.py" "${PORT}" "${BUILD_DIR}"
