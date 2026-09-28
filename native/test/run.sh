#!/usr/bin/env bash
# Runs the native (non-WASM) pure-logic unit tests, compiled natively with the system
# compiler for fast iteration, plus the compiled-WASM ABI smoke test under Node if a build
# exists. See ARCHITECTURE.md, "Automated tests for the non-realtime parts".
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

BUILD_DIR="native/build"
mkdir -p "$BUILD_DIR"

CXX="${CXX:-clang++}"
STATUS=0

for test_file in native/test/*_test.cpp; do
  name="$(basename "$test_file" .cpp)"
  "$CXX" -std=c++17 -Wall -Wextra -O1 -Inative/src -o "$BUILD_DIR/$name" "$test_file"
  echo "--- running $name ---"
  "$BUILD_DIR/$name" || STATUS=1
done

if [ -f "native/build/engine.node.mjs" ]; then
  echo "--- running wasm ABI smoke test ---"
  node native/test/smoke.mjs || STATUS=1
else
  echo "skipping wasm ABI smoke test (run npm run build:wasm first)"
fi

exit $STATUS
