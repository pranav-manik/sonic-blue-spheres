#!/usr/bin/env bash
# build_web.sh — build blue_spheres for WebAssembly via Emscripten.
#
# Prerequisites:
#   1. Install emsdk (once, anywhere on your machine):
#        git clone https://github.com/emscripten-core/emsdk
#        cd emsdk && ./emsdk install latest && ./emsdk activate latest
#
#   2. Either source emsdk_env.sh before running this script:
#        source /path/to/emsdk/emsdk_env.sh
#      OR set the EMSDK_ENV variable below to the path of that file.
#
# Usage:
#   ./build_web.sh          # Release build (default)
#   ./build_web.sh Debug    # Debug build
#
# Output:
#   build_web/blue_spheres.html   — open this in a browser (via a local server)
#   build_web/blue_spheres.js
#   build_web/blue_spheres.wasm
#   build_web/blue_spheres.data   — preloaded sprites

set -euo pipefail

BUILD_TYPE="${1:-Release}"
BUILD_DIR="build_web"

# ── optional: point to your emsdk_env.sh if not already sourced ─────────────
# EMSDK_ENV="/path/to/emsdk/emsdk_env.sh"
# if [[ -n "${EMSDK_ENV:-}" && -f "$EMSDK_ENV" ]]; then
#     source "$EMSDK_ENV"
# fi

# ── sanity check ────────────────────────────────────────────────────────────
if ! command -v emcmake &> /dev/null; then
    echo "ERROR: emcmake not found."
    echo "  Source the Emscripten environment first:"
    echo "    source /path/to/emsdk/emsdk_env.sh"
    exit 1
fi

echo "==> Configuring for WebAssembly (${BUILD_TYPE})…"
emcmake cmake \
    -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    .

echo "==> Building…"
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" -- -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"

echo ""
echo "✓ Build complete.  Output files are in: ${BUILD_DIR}/"
echo ""
echo "  To run in a browser start a local HTTP server, e.g.:"
echo "    python3 -m http.server 8080 --directory ${BUILD_DIR}/"
echo "  Then open:  http://localhost:8080/blue_spheres.html"
echo ""
echo "  (Direct file:// URLs won't work because browsers block"
echo "   SharedArrayBuffer / WASM loading from the local file system.)"
