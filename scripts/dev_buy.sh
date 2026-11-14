#!/usr/bin/env bash
# Short purchase test (~1 min): Commander builds the vehicle factory first (instant build),
# the team gets money, a bot buys a vehicle at the factory (no debug in-place buy).
# Ends at the first purchase; "vehicle econ" lines before/after give price and payer. For purchase/economy diagnostics (TODO 5.5a);
# production-value comparisons stay with dev_econ.sh.
# Usage: scripts/dev_buy.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="${DEV_ROSTER:-2:1 2:1 2:3}" \
DEV_PRE="emp_sv_debug_instant_build 1;rcbot_empires_commander_first 5;emp_sv_debug_build_vehicles 0;rcbot_empires_vehicle_debug_buy 0${DEV_PRE:+;$DEV_PRE}" \
DEV_CMDS="${DEV_CMDS:-wait 5;ent_fire emp_params ModifyNFResources 3000}" \
DEV_UNTIL="${DEV_UNTIL:-gameplay vehicle_purchase|vehicle bought|reason=stage-timeout}" \
DEV_SHOW="${DEV_SHOW:-vehicle econ|vehicle_purchase|commander-econ result=CREATED|vehicle skip|stage-timeout}" \
exec scripts/dev.sh "${1:-emp_crossroads}" 3 10
