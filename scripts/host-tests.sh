#!/bin/bash
# Build and run the pure-logic unit tests on the host (no hardware, no PlatformIO env needed beyond the library cache).
# Usage: scripts/host-tests.sh      (run `pio run` once first so .pio/libdeps/esp12e/ArduinoJson exists)
set -euo pipefail
cd "$(dirname "$0")/.."

ARDUINOJSON=".pio/libdeps/esp12e/ArduinoJson/src"
if [ ! -d "$ARDUINOJSON" ]; then
    echo "ArduinoJson not found in $ARDUINOJSON: run 'pio run' (or scripts/build-with-docker.sh) once first" >&2
    exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

for test in test/host/*_test.cpp; do
    name="$(basename "$test" .cpp)"
    g++ -std=c++17 -Wall -Wextra -Iinclude -Iinclude/glimmer -I"$ARDUINOJSON" "$test" -o "$OUT/$name"
    "$OUT/$name"
done
