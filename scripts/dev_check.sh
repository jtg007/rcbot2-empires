#!/usr/bin/env bash
# Regression check: builds once, then runs the short tests in parallel (one
# server per test on its own port) and prints PASS/FAIL per test. Wall time is
# the slowest test (ca. 1-1.5 min) instead of the sum. DEV_CHECK_SERIAL=1 runs
# them one after another like before.
# Usage: scripts/dev_check.sh [map]   (pad/turret/revive use their own default map)
cd "$(dirname "${BASH_SOURCE[0]}")/.."
source scripts/dev_cpu.sh
MAP="${1:-emp_streetsoffire}"
TMP="$(mktemp -d)" || exit 1
fail=0
row() { printf '%-22s %-5s %s\n' "$1" "$2" "$3"; [[ "$2" == PASS ]] || fail=1; }

BUILD_LOG="$TMP/build.txt"
if [[ "${DEV_SKIP_BUILD:-0}" != 1 ]]; then
  if ! built="$(scripts/dev_build.sh 2>"$BUILD_LOG")"; then
    grep -a -m5 "error" "$BUILD_LOG"; echo "Build failed: $BUILD_LOG"; exit 1
  fi
  hash="$(sha256sum < "$built")" || exit 1
  export DEV_BUILD_HASH="${DEV_BUILD_HASH:-${hash%% *}}"
fi
export DEV_SKIP_BUILD=1
port="${DEV_CHECK_PORT:-49925}" # first port; DEV_CHECK_PORT=49940 next to a running test server
job() {
  local name="$1"; shift
  (
    t0=$SECONDS
    status=0
    DEV_PORT=$port "$@" >"$TMP/$name.txt" 2>&1 || status=$?
    echo "$status" >"$TMP/$name.status"
    echo $((SECONDS - t0)) >"$TMP/$name.sec"
  ) &
  port=$((port + 1))
  [[ "${DEV_CHECK_SERIAL:-0}" == 1 ]] && wait
  sleep 2  # stagger server starts
}
move() {
  DEV_NO_RECORDER=1 DEV_TIMESCALE=4 DEV_ROSTER="2:1" DEV_PRE="rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0" \
  DEV_CMDS="wait 10;rcbotd empires_test_commander_move bot 2 888 3085 -142" \
  DEV_UNTIL="commander-move retry=6|commander-move result=COMPLETE" \
  DEV_EXPECT='^\[RCBOT2\] commander-move result=COMPLETE' \
  DEV_SHOW="commander-move (result|retry)|error:" \
  scripts/dev.sh "$MAP" 1 8
}
job chain scripts/dev_chain.sh "$MAP" 4
job move move
job pad scripts/dev_pad.sh
job turret scripts/dev_turret.sh
job revive scripts/dev_revive.sh
wait
sec() { cat "$TMP/$1.sec" 2>/dev/null; }
ok() { [[ -f "$TMP/$1.status" && "$(<"$TMP/$1.status")" == 0 ]]; }
detail() {
  local reason
  reason="$(grep -a 'Run result: FAIL\|did not become ready\|not loaded\|^Missing build\|^Build hash mismatch\|^Map not found\|^Port ' "$TMP/$1.txt" | tail -1)"
  printf '%s\n' "${reason:-see $TMP/$1.txt}"
}

# 1) Commander seat -> barracks placed -> Engineer builds it
if ok chain; then
  row chain PASS "$(grep -a 'build-assign result=' "$TMP/chain.txt" | grep -o 'elapsed=[0-9.]*' | head -1)"
else
  row chain FAIL "$(detail chain)"
fi

# 2) Long move order across the map (1 bot)
retries=$(grep -a -c "^\[RCBOT2\] commander-move retry=" "$TMP/move.txt")  # not the harness echo "Ende: ..."
if ok move; then
  # COMPLETE after retries means the bot hung on the way (TODO 1.3b): not a PASS.
  if [[ "$retries" -eq 0 ]]; then
    row long-move PASS "retries=0"
  else
    row long-move FAIL "COMPLETE only after retries=$retries (hang on the route)"
  fi
else
  row long-move FAIL "retries=$retries $(detail move)"
fi

# 3) Damaged tank drives onto the repair pad and refits (default map of dev_pad.sh)
if ok pad; then
  row pad PASS "$(sec pad)s $(grep -am1 '^\[RCBOT2\] vehicle repaired' "$TMP/pad.txt" | cut -c10-89)"
else
  row pad FAIL "$(detail pad)"
fi

# 4) Engineer builds an MG turret at its own refinery (default map of dev_turret.sh)
if ok turret; then
  row turret PASS "$(sec turret)s $(grep -a 'refinery-turret complete' "$TMP/turret.txt" | head -1 | grep -o 'health=[0-9/]*')"
else
  row turret FAIL "$(detail turret)"
fi

# 5) Revive engineer revives a killed team mate (default map of dev_revive.sh)
if ok revive; then
  row revive PASS "$(sec revive)s"
else
  row revive FAIL "$(detail revive)"
fi

echo "Rohausgaben: $TMP"
exit $fail
