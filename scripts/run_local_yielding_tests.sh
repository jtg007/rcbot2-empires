#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
CXX_BIN="${CXX:-c++}"
TEMP_DIR="$(mktemp -d /tmp/rcbot2-local-yielding-tests.XXXXXX)"
TEST_BINARY="$TEMP_DIR/local_yielding_test"

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
	-DRCBOT_LOCAL_CONFLICT_TEST_VECTOR \
	-I"$ROOT_DIR/utils/RCBot2_meta" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_local_conflict_observation.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_local_yielding.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_local_yielding_test.cpp" \
	-o "$TEST_BINARY"

"$TEST_BINARY"
