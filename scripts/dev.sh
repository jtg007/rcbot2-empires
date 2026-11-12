#!/usr/bin/env bash
# Build, deploy into the isolated test server, run a bot match, print the numbers.
# Usage: scripts/dev.sh <map> <bot count> <minutes>
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 <map> <bot count> <minutes>" >&2
  exit 2
fi
MAP="$1" BOTS="$2" MINUTES="$3"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
CLEAN="${RCBOT_DEPS_DIR:-$(dirname "$ROOT")/rcbot2-clean}"
# Local settings (not in git): scripts/dev.local can set EMPIRES_TEST_SERVER etc.
[[ -f "$ROOT/scripts/dev.local" ]] && source "$ROOT/scripts/dev.local"
SERVER_ROOT="${EMPIRES_TEST_SERVER:?set EMPIRES_TEST_SERVER (or put it in scripts/dev.local) to an isolated Empires dedicated server}"
SERVER_PORT=49925
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

echo "== Build"
BUILD_LOG="$(mktemp)"
if ! BUILT="$("$ROOT/scripts/dev_build.sh" 2>"$BUILD_LOG")"; then
  grep -B2 -A12 -m5 "error" "$BUILD_LOG" || tail -40 "$BUILD_LOG"
  echo "Build failed, log: $BUILD_LOG" >&2
  exit 1
fi
rm -f "$BUILD_LOG"

echo "== Deploy"
[[ -e "$PLUGIN.pre-legacy-dev" ]] || cp -p "$PLUGIN" "$PLUGIN.pre-legacy-dev"
cp "$BUILT" "$PLUGIN"
sha256sum "$PLUGIN" | cut -c1-16

STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$SERVER_ROOT/dev-runs/$STAMP-$MAP-${BOTS}b-${MINUTES}m"
mkdir -p "$RUN_DIR"
# Loopback is enough: the bots are internal clients. Override with DEV_SERVER_IP.
SERVER_IP="${DEV_SERVER_IP:-127.0.0.1}"

echo "== Run: $MAP, $BOTS bots, $MINUTES minutes ($RUN_DIR)"
exec python3 "$ROOT/scripts/dev_run.py" --map "$MAP" --bots "$BOTS" --minutes "$MINUTES" \
  --server-root "$SERVER_ROOT" --ip "$SERVER_IP" --run-dir "$RUN_DIR"
