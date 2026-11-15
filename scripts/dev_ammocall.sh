#!/usr/bin/env bash
# Fast check (TODO 3.10b step 3): a bought tank far from the pad runs out of cannon and MG ammo
# (rcbotd empires_vehicle_setammo 0 0 after DEV_AMMO_WAIT s, default 40), calls for ammo
# (emp_menu_command 29), a free team engineer (two in the roster, one is often busy building) walks over and places a supply crate (engSelect 7),
# the tank refills there (expected ammo-crate placed, crate-done, resupplied). Ca. 2-3 min.
# Usage: [DEV_AMMO_WAIT=s] scripts/dev_ammocall.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="${DEV_ROSTER:-2:1 2:1 2:3 2:3}" \
DEV_PRE="emp_sv_debug_instant_build 1;rcbot_empires_commander_first 4;emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_vehicles 0" \
DEV_CMDS="wait 5;ent_fire emp_params ModifyNFResources 3000;waitfor 200 pad-yaw;wait 5;ent_fire emp_params ModifyNFResources 3000;rcbot_empires_vehicles 1;waitfor 200 vehicle bought;wait ${DEV_AMMO_WAIT:-40};rcbotd empires_vehicle_setammo 0 0" \
DEV_UNTIL="vehicle (loadout-check|retreat-timeout|retreat-abort)" \
DEV_SHOW="\[RCBOT2\] (vehicle (bought|debug-ammo|retreat|ammo-call|crate-|resupplied|refit-sent|repaired|retreat-timeout|retreat-abort|pad-enter|loadout-check)|ammo-crate)" \
exec scripts/dev.sh "${1:-emp_crossroads}" 4 15
