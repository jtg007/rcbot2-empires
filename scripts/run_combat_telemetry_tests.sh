#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
CXX_BIN="${CXX:-c++}"
TEMP_DIR="$(mktemp -d /tmp/rcbot2-combat-telemetry-tests.XXXXXX)"
TEST_BINARY="$TEMP_DIR/combat_telemetry_test"

cleanup() {
  rm -f "$TEST_BINARY"
  rmdir "$TEMP_DIR"
}
trap cleanup EXIT

"$CXX_BIN" \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Werror \
  -pedantic \
  -I"$ROOT_DIR/utils/RCBot2_meta" \
  "$ROOT_DIR/utils/RCBot2_meta/bot_empires_combat_telemetry_test.cpp" \
  -o "$TEST_BINARY"

"$TEST_BINARY"
