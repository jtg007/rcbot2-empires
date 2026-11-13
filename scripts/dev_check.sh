#!/usr/bin/env bash
# Regression check, ca. 5 Min: runs the short tests one after another and
# prints PASS/FAIL per test. Run after every change.
# Usage: scripts/dev_check.sh [map]   (the pad test uses the default map of dev_pad.sh)
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
t0=$SECONDS
scripts/dev_pad.sh >"$TMP/pad.txt" 2>&1
if grep -aq "vehicle repaired" "$TMP/pad.txt" && ! grep -aq "Crashes: YES" "$TMP/pad.txt"; then
  row pad PASS "$((SECONDS - t0))s $(grep -a 'vehicle repaired' "$TMP/pad.txt" | head -1 | sed 's/.*\(vehicle repaired\)/\1/' | cut -c1-80)"
else
  row pad FAIL "$(grep -a 'retreat-timeout\|retreat-abort\|give-up\|Crashes\|error:' "$TMP/pad.txt" | tr '\n' '|' | cut -c1-200)"
fi

# 4) Engineer builds an MG turret at its own refinery (default map of dev_turret.sh)
t0=$SECONDS
scripts/dev_turret.sh >"$TMP/turret.txt" 2>&1
if grep -aq "refinery-turret complete" "$TMP/turret.txt" && ! grep -aq "Crashes: YES" "$TMP/turret.txt"; then
  row turret PASS "$((SECONDS - t0))s $(grep -a 'refinery-turret complete' "$TMP/turret.txt" | head -1 | grep -o 'health=[0-9/]*')"
else
  row turret FAIL "$(grep -a 'refinery-turret\|Crashes\|error:' "$TMP/turret.txt" | tail -4 | tr '\n' '|' | cut -c1-200)"
fi

# 5) Revive engineer revives a killed team mate (default map of dev_revive.sh)
t0=$SECONDS
scripts/dev_revive.sh >"$TMP/revive.txt" 2>&1
if grep -aq "revive end .*reason=revived" "$TMP/revive.txt" && ! grep -aq "Crashes: YES" "$TMP/revive.txt"; then
  row revive PASS "$((SECONDS - t0))s"
else
  row revive FAIL "$(grep -a 'revive \|Crashes\|error:' "$TMP/revive.txt" | tail -4 | tr '\n' '|' | cut -c1-200)"
fi

echo "Rohausgaben: $TMP"
exit $fail
