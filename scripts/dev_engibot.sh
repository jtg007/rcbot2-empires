#!/usr/bin/env bash
# Fast check (community request /buildengibot): the NF commander bot (userid 2) enters the CV and
# says /buildengibot; a second NF bot (userid 3, not commander) is refused. Expected: refuse
# not-commander, two buys (pool -100 each), refuse limit, then refuse money (cost raised). ~1 min.
# DEV_ENGIBOT_MODE=build: two EngiBots are bought, then the commander places a barracks; the
# EngiBots (the only engineers) must find it inside the leash and build it (build-assign COMPLETE),
# and report their distance to the CV every 10 s. ~1.5 min.
# DEV_ENGIBOT_MODE=level: buy two, changelevel, expect engibot kick reason=level-reset and no
# EngiBot in status.
# Usage: [DEV_ENGIBOT_MODE=build|level] scripts/dev_engibot.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
T="rcbotd empires_test_engibot"
if [[ "${DEV_ENGIBOT_MODE:-}" == build ]]; then
  CMDS="wait 2;rcbotd empires_commander_entry_once;waitfor 180 commander-entry result=COMPLETE;wait 2;$T 2;wait 3;$T 2;wait 20;rcbotd empires_commander_place_barracks"
  UNTIL="build-assign result="
elif [[ "${DEV_ENGIBOT_MODE:-}" == level ]]; then
  # Map change: both EngiBots are kicked (level-reset) and gone from status afterwards.
  CMDS="wait 2;rcbotd empires_commander_entry_once;waitfor 180 commander-entry result=COMPLETE;wait 2;$T 2;wait 3;$T 2;wait 10;changelevel ${1:-emp_streetsoffire};wait 40;status;wait 5"
  UNTIL="no-such-line"
else
  CMDS="wait 2;rcbotd addbot 1;rcbotd addbot 3 3;rcbotd empires_commander_entry_once;waitfor 180 commander-entry result=COMPLETE;wait 2;$T 3;wait 2;$T 2;wait 3;$T 2;wait 3;$T 2;wait 40;rcbot_empires_engibot_limit 3;rcbot_empires_engibot_cost 999999;$T 2;wait 10"
  UNTIL="engibot refuse.*reason=money"
fi
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:1" \
DEV_PRE="rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0;rcbot_empires_engineer_turrets 0" \
DEV_CMDS="$CMDS" DEV_UNTIL="$UNTIL" \
DEV_SHOW="commander-entry result|commander-place|engibot |EngiBot|build-assign result" \
exec scripts/dev.sh "${1:-emp_streetsoffire}" 2 6
