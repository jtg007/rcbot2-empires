#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -Iutils/RCBot2_meta \
  utils/RCBot2_meta/bot_empires_turret_plan_test.cpp -o "$out"
"$out"
