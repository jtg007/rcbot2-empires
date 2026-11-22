#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/rcbot2-recovery-episode-tests"
binary="${build_dir}/bot_recovery_episode_test"
active_binary="${build_dir}/bot_recovery_episode_active_test"

mkdir -p "$build_dir"
g++ -std=c++17 -Wall -Wextra -Werror -I"$repo_dir/utils/RCBot2_meta" \
"$repo_dir/utils/RCBot2_meta/bot_recovery_episode_test.cpp" \
	-o "$binary"
"$binary"
g++ -std=c++17 -Wall -Wextra -Werror -I"$repo_dir/utils/RCBot2_meta" \
	"$repo_dir/utils/RCBot2_meta/bot_recovery_episode_active_test.cpp" \
	-o "$active_binary"
"$active_binary"
