#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
TEMP_DIR="$(mktemp -d /tmp/rcbot2-squad-join-tests.XXXXXX)"
trap 'rm -rf "$TEMP_DIR"' EXIT

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
	-I"$ROOT_DIR/utils/RCBot2_meta" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_empires_squad_join_test.cpp" \
	-o "$TEMP_DIR/squad_join_test"
"$TEMP_DIR/squad_join_test"
