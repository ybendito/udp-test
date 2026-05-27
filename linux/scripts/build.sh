#!/usr/bin/env bash
# Build boostudp_server only; binary stays under linux/build/ (no install).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT}/linux/build"

if [[ ! -d "${ROOT}/deps/boost" ]]; then
    echo "Vendored Boost not found. Run: ${ROOT}/linux/scripts/fetch-deps.sh" >&2
    exit 1
fi

command -v cmake >/dev/null 2>&1 || {
    echo "cmake is required (e.g. dnf install cmake gcc-c++)." >&2
    exit 1
}

JOBS="$(nproc 2>/dev/null || echo 2)"

cmake -S "${ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --target boostudp_server -j"${JOBS}"

echo ""
echo "Server (run from build tree, no install):"
echo "  ${BUILD_DIR}/boostudp_server --port 9000 -v"
