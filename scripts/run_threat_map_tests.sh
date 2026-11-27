#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Iutils/RCBot2_meta \
  utils/RCBot2_meta/bot_empires_threat_map_test.cpp -o "$out"
"$out"
