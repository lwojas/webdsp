#!/usr/bin/env bash
# Compiles native/src/api.cpp (and everything it includes) to a single WASM module + ES6
# JS glue, loadable both inside an AudioWorkletGlobalScope and under plain Node (the latter
# is what native/test/smoke.mjs uses to verify the compiled module without a browser).
#
# Single translation unit, no CMake: the engine is a handful of headers plus one .cpp, and a
# direct emcc invocation is easier to read end-to-end than a build-system indirection for
# something this size (see ARCHITECTURE.md, "small, understandable core").
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

EMSDK_DIR="$(pwd)/.emsdk"
if [ -f "$EMSDK_DIR/emsdk_env.sh" ]; then
  # shellcheck disable=SC1091
  source "$EMSDK_DIR/emsdk_env.sh" >/dev/null
fi

if ! command -v emcc >/dev/null 2>&1; then
  echo "emcc not found. Run: git clone https://github.com/emscripten-core/emsdk.git .emsdk && (cd .emsdk && ./emsdk install latest && ./emsdk activate latest)" >&2
  exit 1
fi

OUT_DIR="src/worklet/generated"
TEST_OUT_DIR="native/build"
mkdir -p "$OUT_DIR" "$TEST_OUT_DIR"

COMMON_FLAGS=(
  -std=c++17 -O3 -flto
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createEngineModule
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=33554432
  -sSINGLE_FILE=1
  -sFILESYSTEM=0
  -sASSERTIONS=0
  -sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAP32,HEAPU8
  -sNO_EXIT_RUNTIME=1
  --no-entry
)

# The artifact actually shipped to the browser: ENVIRONMENT=worker keeps it free of any
# Node-specific code path, which otherwise shows up as a spurious "node:module externalized"
# warning in the Vite build (harmless at runtime — dead code behind an environment check —
# but worth not shipping/explaining away). See ARCHITECTURE.md, "Where WASM is used".
em++ native/src/api.cpp "${COMMON_FLAGS[@]}" -sENVIRONMENT=worker -o "$OUT_DIR/engine.js"
echo "Built $OUT_DIR/engine.js (browser/worklet)"

# A second build, ENVIRONMENT=node, used only by native/test/smoke.mjs so the ABI can be
# exercised without a browser. Never referenced from src/ — kept out of the app's module
# graph entirely.
em++ native/src/api.cpp "${COMMON_FLAGS[@]}" -sENVIRONMENT=node -o "$TEST_OUT_DIR/engine.node.mjs"
echo "Built $TEST_OUT_DIR/engine.node.mjs (node, test-only)"
