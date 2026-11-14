#!/usr/bin/env bash
# Fast check (TODO 3.10d): a tank bought with the MG on the cannon's fire button
# (rcbot_empires_vehicle_debug_mixed 1) drives to the own repair pad, refits the
# bot loadout and logs the loadout afterwards (expected mixed=0). Ca. 1 min.
# Usage: scripts/dev_mixed.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:1 2:3" \
DEV_PRE="emp_sv_debug_instant_build 1;rcbot_empires_commander_first 4;emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_vehicle_debug_mixed 1;rcbot_empires_vehicles 0" \
DEV_CMDS="wait 5;ent_fire emp_params ModifyNFResources 3000;waitfor 200 pad-yaw;wait 5;ent_fire emp_params ModifyNFResources 3000;rcbot_empires_vehicles 1;waitfor 200 vehicle bought" \
DEV_UNTIL="vehicle (loadout-check|retreat-timeout|retreat-abort)" \
DEV_SHOW="\[RCBOT2\] vehicle (bought|retreat|refit-sent|repaired|retreat-timeout|retreat-abort|pad-enter|loadout-check)" \
exec scripts/dev.sh "${1:-emp_crossroads}" 3 12
