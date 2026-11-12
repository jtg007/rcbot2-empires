#!/usr/bin/env bash
# Regression check, ca. 3 Min: runs the short tests one after another and
# prints PASS/FAIL per test. Run after every change.
# Usage: scripts/dev_check.sh [map]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
MAP="${1:-emp_streetsoffire}"
TMP="$(mktemp -d)"
fail=0
row() { printf '%-22s %-5s %s\n' "$1" "$2" "$3"; [[ "$2" == PASS ]] || fail=1; }

# 1) Commander seat -> barracks placed -> Engineer builds it
scripts/dev_chain.sh "$MAP" 4 >"$TMP/chain.txt" 2>&1
if grep -q "build-assign result=COMPLETE" "$TMP/chain.txt" && ! grep -aq "Crashes: YES" "$TMP/chain.txt"; then
  row chain PASS "$(grep -a 'build-assign result=' "$TMP/chain.txt" | grep -o 'elapsed=[0-9.]*' | head -1)"
else
  row chain FAIL "$(grep -a 'commander-entry result\|commander-place result\|build-assign result\|Crashes\|error:' "$TMP/chain.txt" | tr '\n' '|' | cut -c1-200)"
fi

# 2) Long move order across the map (1 bot)
DEV_NO_RECORDER=1 DEV_TIMESCALE=4 DEV_ROSTER="2:1" DEV_PRE="rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0" \
DEV_CMDS="wait 10;rcbotd empires_test_commander_move bot 2 888 3085 -142" \
DEV_UNTIL="commander-move retry=6|commander-move result=COMPLETE" \
DEV_SHOW="commander-move (result|retry)|error:" \
scripts/dev.sh "$MAP" 1 8 >"$TMP/move.txt" 2>&1
retries=$(grep -a -c "commander-move retry=" "$TMP/move.txt")
if grep -aq "commander-move result=COMPLETE" "$TMP/move.txt" && ! grep -aq "Crashes: YES" "$TMP/move.txt"; then
  row long-move PASS "retries=$retries"
else
  row long-move FAIL "retries=$retries $(grep -a 'Crashes\|error:' "$TMP/move.txt" | tr '\n' '|' | cut -c1-160)"
fi

echo "Rohausgaben: $TMP"
exit $fail
