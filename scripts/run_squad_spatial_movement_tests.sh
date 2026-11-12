#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
CXX_BIN="${CXX:-c++}"
TEMP_DIR="$(mktemp -d /tmp/rcbot2-squad-spatial-movement-tests.XXXXXX)"
TEST_BINARY="$TEMP_DIR/squad_spatial_movement_test"

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
	"$ROOT_DIR/utils/RCBot2_meta/bot_order_protocol.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_order_protocol_telemetry.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_squad_order_recipient.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_squad_decision.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_squad_claims.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_squad_spatial.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_squad_spatial_movement.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_empires_squad_runtime.cpp" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_squad_spatial_movement_test.cpp" \
	-o "$TEST_BINARY"

"$TEST_BINARY"
