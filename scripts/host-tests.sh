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
    # A test can need extra sources / include paths (for example a filesystem stand-in): put them in <name>.args
    extra=""
    if [ -f "test/host/${name}.args" ]; then
        extra="$(cat "test/host/${name}.args")"
    fi
    # shellcheck disable=SC2086  # the args file holds several words on purpose
    g++ -std=c++17 -Wall -Wextra -Iinclude -Iinclude/glimmer -I"$ARDUINOJSON" $extra "$test" -o "$OUT/$name"
    "$OUT/$name"
done
