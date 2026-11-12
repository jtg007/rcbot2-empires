#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
TEMP_DIR="$(mktemp -d /tmp/rcbot2-waypoint-anchor-tests.XXXXXX)"
trap 'rm -rf "$TEMP_DIR"' EXIT

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
	-I"$ROOT_DIR/utils/RCBot2_meta" \
	"$ROOT_DIR/utils/RCBot2_meta/bot_waypoint_anchor_exclusion_test.cpp" \
	-o "$TEMP_DIR/waypoint_anchor_exclusion_test"
"$TEMP_DIR/waypoint_anchor_exclusion_test"
