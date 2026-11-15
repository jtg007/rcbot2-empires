#!/usr/bin/env bash
# Commander orders in one run, given like the game UI (emp_unit_order_list + emp_unit_order):
# NF commander bot (userid 2) seated, NF rifleman (3), BE rifleman (4), NF engineer (5).
#  1. Move (type 1) for 3 to DEV_ORDER_MOVE -> commander-move result=COMPLETE dist= (under 100;
#     streetsoffire default, skipped on other maps unless DEV_ORDER_MOVE is set)
#  2. Attack target (type 3) for 3 on enemy 4 -> unit_command_type_now=3, commander-order start type=3
#  3. Guard (type 4) for engineer 5 on the team's nearest refinery (damaged to 100) -> repaired,
#     build-assign result=COMPLETE. Needs a map with a team refinery at start (emp_crossroads).
# Usage: [DEV_ORDER_MOVE="x y z"] scripts/dev_orders.sh [map] [timescale]   (~3 min)
cd "$(dirname "${BASH_SOURCE[0]}")/.."
MAP="${1:-emp_crossroads}"
O="rcbotd empires_test_unit_order 2"
MOVE="${DEV_ORDER_MOVE:-}"
[[ -z "$MOVE" && "$MAP" == emp_streetsoffire ]] && MOVE="888 3085 -142"
CMDS="wait 2;rcbotd empires_commander_entry_once;waitfor 180 commander-entry result=COMPLETE;wait 2"
[[ -n "$MOVE" ]] && CMDS+=";$O 1 3 $MOVE;waitfor 300 commander-move result=(COMPLETE|FAIL)"
CMDS+=";mark;$O 3 3 0 0 0 4;waitfor 60 commander-order start type=3;wait 20"
CMDS+=";mark;$O 4 5 0 0 0 r;waitfor 240 build-assign result="
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:1 3:1 2:3" \
DEV_PRE="rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0;rcbot_empires_engineer_turrets 0" \
DEV_CMDS="$CMDS" DEV_UNTIL="build-assign result=" \
DEV_SHOW="commander-entry result|test-unit-order|empires_test_unit_order|commander-move result|navmesh-path-extended|guard-order|build-assign (result|stage=work)|NOT found" \
exec scripts/dev.sh "$MAP" 4 12
