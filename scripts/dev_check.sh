#!/usr/bin/env bash
# Regression check: builds once, then runs the short tests in parallel (one
# server per test on its own port) and prints PASS/FAIL per test. Wall time is
# the slowest test (ca. 1-1.5 min) instead of the sum. DEV_CHECK_SERIAL=1 runs
# them one after another like before.
# Usage: scripts/dev_check.sh [map]   (pad/turret/revive use their own default map)
cd "$(dirname "${BASH_SOURCE[0]}")/.."
source scripts/dev_cpu.sh
MAP="${1:-emp_streetsoffire}"
TMP="$(mktemp -d)"
fail=0
row() { printf '%-22s %-5s %s\n' "$1" "$2" "$3"; [[ "$2" == PASS ]] || fail=1; }

BUILD_LOG="$TMP/build.txt"
if ! scripts/dev_build.sh >/dev/null 2>"$BUILD_LOG"; then
  grep -a -m5 "error" "$BUILD_LOG"; echo "Build failed: $BUILD_LOG"; exit 1
fi
export DEV_SKIP_BUILD=1
port=49925
job() {  # job <name> <command...>: output to $TMP/<name>.txt, seconds to $TMP/<name>.sec
  local name="$1"; shift
  ( t0=$SECONDS; DEV_PORT=$port "$@" >"$TMP/$name.txt" 2>&1; echo $((SECONDS - t0)) >"$TMP/$name.sec" ) &
  port=$((port + 1))
  [[ "${DEV_CHECK_SERIAL:-0}" == 1 ]] && wait
  sleep 2  # stagger server starts
}
move() {
  DEV_NO_RECORDER=1 DEV_TIMESCALE=4 DEV_ROSTER="2:1" DEV_PRE="rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0" \
  DEV_CMDS="wait 10;rcbotd empires_test_commander_move bot 2 888 3085 -142" \
  DEV_UNTIL="commander-move retry=6|commander-move result=COMPLETE" \
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

# 1) Commander seat -> barracks placed -> Engineer builds it
if grep -q "build-assign result=COMPLETE" "$TMP/chain.txt" && ! grep -aq "Crashes: YES" "$TMP/chain.txt"; then
  row chain PASS "$(grep -a 'build-assign result=' "$TMP/chain.txt" | grep -o 'elapsed=[0-9.]*' | head -1)"
else
  row chain FAIL "$(grep -a 'commander-entry result\|commander-place result\|build-assign result\|Crashes\|error:' "$TMP/chain.txt" | tr '\n' '|' | cut -c1-200)"
fi

# 2) Long move order across the map (1 bot)
retries=$(grep -a -c "^\[RCBOT2\] commander-move retry=" "$TMP/move.txt")  # not the harness echo "Ende: ..."
if grep -aq "commander-move result=COMPLETE" "$TMP/move.txt" && ! grep -aq "Crashes: YES" "$TMP/move.txt"; then
  # COMPLETE after retries means the bot hung on the way (TODO 1.3b): not a PASS.
  if [[ "$retries" -eq 0 ]]; then
    row long-move PASS "retries=0"
  else
    row long-move FAIL "COMPLETE only after retries=$retries (hang on the route)"
  fi
else
  row long-move FAIL "retries=$retries $(grep -a 'Crashes\|error:' "$TMP/move.txt" | tr '\n' '|' | cut -c1-160)"
fi

# 3) Damaged tank drives onto the repair pad and refits (default map of dev_pad.sh)
if grep -aq "vehicle repaired" "$TMP/pad.txt" && ! grep -aq "Crashes: YES" "$TMP/pad.txt"; then
  row pad PASS "$(sec pad)s $(grep -a 'vehicle repaired' "$TMP/pad.txt" | head -1 | sed 's/.*\(vehicle repaired\)/\1/' | cut -c1-80)"
else
  row pad FAIL "$(grep -a 'retreat-timeout\|retreat-abort\|give-up\|Crashes\|error:' "$TMP/pad.txt" | tr '\n' '|' | cut -c1-200)"
fi

# 4) Engineer builds an MG turret at its own refinery (default map of dev_turret.sh)
if grep -aq "refinery-turret complete" "$TMP/turret.txt" && ! grep -aq "Crashes: YES" "$TMP/turret.txt"; then
  row turret PASS "$(sec turret)s $(grep -a 'refinery-turret complete' "$TMP/turret.txt" | head -1 | grep -o 'health=[0-9/]*')"
else
  row turret FAIL "$(grep -a 'refinery-turret\|Crashes\|error:' "$TMP/turret.txt" | tail -4 | tr '\n' '|' | cut -c1-200)"
fi

# 5) Revive engineer revives a killed team mate (default map of dev_revive.sh)
if grep -aq "revive end .*reason=revived" "$TMP/revive.txt" && ! grep -aq "Crashes: YES" "$TMP/revive.txt"; then
  row revive PASS "$(sec revive)s"
else
  row revive FAIL "$(grep -a 'revive \|Crashes\|error:' "$TMP/revive.txt" | tail -4 | tr '\n' '|' | cut -c1-200)"
fi

echo "Rohausgaben: $TMP"
exit $fail
