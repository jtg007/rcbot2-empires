#!/usr/bin/env bash
# Build, deploy into the isolated test server, run a bot match, print the numbers.
# Usage: scripts/dev.sh <map> <bot count> <minutes>
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
source "$(dirname "${BASH_SOURCE[0]}")/dev_cpu.sh"

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 <map> <bot count> <minutes>" >&2
  exit 2
fi
MAP="$1" BOTS="$2" MINUTES="$3"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# Local settings (not in git): scripts/dev.local can set EMPIRES_TEST_SERVER etc.
[[ -f "$ROOT/scripts/dev.local" ]] && source "$ROOT/scripts/dev.local"
MIRROR="${RCBOT_DEV_MIRROR:-$HOME/.cache/rcbot2-dev-build}"
SERVER_ROOT="${EMPIRES_TEST_SERVER:?set EMPIRES_TEST_SERVER (or put it in scripts/dev.local) to an isolated Empires dedicated server}"
SERVER_PORT="${DEV_PORT:-49925}"  # parallel runs need distinct ports (dev_check.sh)
PLUGIN="$SERVER_ROOT/empires/addons/rcbot2/bin/rcbot.2.sdk2013.so"

[[ -f "$SERVER_ROOT/empires/maps/$MAP.bsp" ]] || { echo "Map not found: $MAP" >&2; exit 2; }
# Port check without netlink (ss/ip are blocked in some agent sandboxes).
if python3 -c '
import errno, socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.bind(("0.0.0.0", int(sys.argv[1])))
except OSError as e:
    sys.exit(1 if e.errno == errno.EADDRINUSE else 0)
' "$SERVER_PORT"; then :; else
  echo "Port $SERVER_PORT is in use: is a test server already running?" >&2
  exit 1
fi

if [[ "${DEV_SKIP_BUILD:-0}" == 1 ]]; then
  # dev_check.sh builds once, then starts several runs in parallel.
  BUILT="$MIRROR/post/package/addons/rcbot2/bin/rcbot.2.sdk2013.so"
else
  echo "== Build"
  BUILD_LOG="$(mktemp)"
  if ! BUILT="$("$ROOT/scripts/dev_build.sh" 2>"$BUILD_LOG")"; then
    grep -B2 -A12 -m5 "error" "$BUILD_LOG" || tail -40 "$BUILD_LOG"
    echo "Build failed, log: $BUILD_LOG" >&2
    exit 1
  fi
  rm -f "$BUILD_LOG"
fi

# Keep a parallel launch on the selected binary until Metamod reports it
# loaded. Identical builds can launch together.
[[ -d "$MIRROR" ]] || { echo "Missing build directory: $MIRROR" >&2; exit 1; }
exec {build_lock}>"$MIRROR/.dev-build.lock"
flock -s "$build_lock"
[[ -f "$BUILT" ]] || { echo "Missing build: $BUILT" >&2; exit 1; }
build_hash="$(sha256sum < "$BUILT")"
build_hash="${build_hash%% *}"
if [[ -n "${DEV_BUILD_HASH:-}" && "$DEV_BUILD_HASH" != "$build_hash" && "$DEV_BUILD_HASH" != "${build_hash:0:16}" ]]; then
  echo "Build hash mismatch: expected $DEV_BUILD_HASH, found $build_hash" >&2
  exit 1
fi

echo "== Deploy"
exec {deploy_lock}>"$SERVER_ROOT/.rcbot-dev-deploy.lock"
flock -s "$deploy_lock"
if [[ ! -e "$PLUGIN.pre-legacy-dev" ]] || ! cmp -s "$BUILT" "$PLUGIN"; then
  flock -u "$deploy_lock"
  flock -x "$deploy_lock"
  [[ -e "$PLUGIN.pre-legacy-dev" ]] || cp -p "$PLUGIN" "$PLUGIN.pre-legacy-dev"
  # A live server still maps the old inode.
  if ! cmp -s "$BUILT" "$PLUGIN"; then
    cp "$BUILT" "$PLUGIN.tmp.$$"
    mv -f "$PLUGIN.tmp.$$" "$PLUGIN"
  fi
fi
exec {build_lock}>&-
echo "${build_hash:0:16}"

if [[ ${DEV_ROSTER:-} =~ [^[:space:]] ]]; then
  BOTS=$(wc -w <<<"$DEV_ROSTER")
fi
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$SERVER_ROOT/dev-runs/$STAMP-$MAP-${BOTS}b-${MINUTES}m${DEV_PORT:+-p$DEV_PORT}"
mkdir -p "$RUN_DIR"
# Record which source state produced this run (SHA, dirty flag, plugin hash).
{
  echo "sha=$(git -C "$ROOT" rev-parse --short HEAD)"
  if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]]; then echo "dirty=1"; else echo "dirty=0"; fi
  echo "plugin_sha256=${build_hash:0:16}"
} > "$RUN_DIR/build-info.txt"
# Loopback is enough: the bots are internal clients. Override with DEV_SERVER_IP.
SERVER_IP="${DEV_SERVER_IP:-127.0.0.1}"

echo "== Run: $MAP, $BOTS bots, $MINUTES minutes ($RUN_DIR)"
exec python3 "$ROOT/scripts/dev_run.py" --map "$MAP" --bots "$BOTS" --minutes "$MINUTES" \
  --server-root "$SERVER_ROOT" --ip "$SERVER_IP" --run-dir "$RUN_DIR" --deploy-lock-fd "$deploy_lock"
