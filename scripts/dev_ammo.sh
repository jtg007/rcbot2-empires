#!/usr/bin/env bash
# Fast check (TODO 3.10b step 1): a bought tank whose cannon runs out of magazine and
# reserve (rcbotd empires_vehicle_setammo 0 0) drives to the own repair pad, refits
# (refills every weapon) and logs the loadout afterwards (expected heavy-left=1). Ca. 1 min.
# Usage: scripts/dev_ammo.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:1 2:3" \
DEV_PRE="emp_sv_debug_instant_build 1;rcbot_empires_commander_first 4;emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_vehicles 0" \
DEV_CMDS="wait 5;ent_fire emp_params ModifyNFResources 3000;waitfor 200 pad-yaw;wait 5;ent_fire emp_params ModifyNFResources 3000;rcbot_empires_vehicles 1;waitfor 200 vehicle bought;wait 4;rcbotd empires_vehicle_setammo 0 0" \
DEV_UNTIL="vehicle (loadout-check|retreat-timeout|retreat-abort)" \
DEV_EXPECT='^\[RCBOT2\] vehicle loadout-check .* known=1 .*heavy-left=1 ' \
DEV_SHOW="\[RCBOT2\] vehicle (bought|debug-ammo|retreat|refit-sent|repaired|retreat-timeout|retreat-abort|pad-enter|loadout-check)" \
exec scripts/dev.sh "${1:-emp_crossroads}" 3 12
