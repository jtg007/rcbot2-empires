#!/usr/bin/env bash
# Fast check: damaged tank drives onto the own repair pad and refits.
# Commander builds the repair station first (rcbot_empires_commander_first 4,
# instant build). Disable refinery turret sorties to isolate the pad order.
# Use a debug purchase in place and damage the tank right after purchase.
# The real purchase at the factory and the retreat from the front are covered
# by longer runs and dev_check.
# Usage: scripts/dev_pad.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:1 2:3" \
DEV_PRE="emp_sv_debug_instant_build 1;rcbot_empires_commander_first 4;rcbot_empires_engineer_turrets 0;emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_vehicles 0" \
DEV_CMDS="wait 5;ent_fire emp_params ModifyNFResources 3000;waitfor 200 pad-yaw;wait 5;ent_fire emp_params ModifyNFResources 3000;rcbot_empires_vehicles 1;waitfor 200 vehicle bought;wait 1;rcbotd empires_vehicle_scalehp 0.5 0.1 0.2 0.25 0.25" \
DEV_UNTIL="vehicle (repaired|retreat-timeout|retreat-abort|give-up)" \
DEV_EXPECT='^\[RCBOT2\] vehicle repaired ' \
DEV_SHOW="\[RCBOT2\] (vehicle (bought|buy-spot|retreat|pad-|park-retry|refit-sent|repaired|give-up)|commander-econ (pad-yaw|factory-yaw))" \
exec scripts/dev.sh "${1:-emp_crossroads}" 3 12
