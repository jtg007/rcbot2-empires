#!/usr/bin/env bash
# Local obstacle avoidance at 1x: debug purchase, drive toward the enemy CV,
# a solid prop is placed on the driver's route once it rolls at entry speed.
# Usage: scripts/dev_vehicle_obstacle.sh [0|1] [map] [nf|be] [ahead] [lateral] [model]
#   0/1      rcbot_empires_vehicle_maneuvers off/on (curve speed stays off)
#   ahead    distance along the route from the vehicle (default 700)
#   lateral  offset to the left of the route line (default 0 = centred)
#   model    prop model (default models/props/concretebarrier.mdl)
# Ends at "vehicle obstacle (passed|timeout|abort)"; timeout is 45 s after placement.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
mode="${1:-0}"
map="${2:-emp_crossroads}"
team="${3:-nf}"
ahead="${4:-700}"
lateral="${5:-0}"
model="${6:-}"
case "$mode:$team" in
  0:nf|1:nf) roster="2:1"; resources="ModifyNFResources" ;;
  0:be|1:be) roster="3:1"; resources="ModifyBEResources" ;;
  *) echo "Usage: $0 [0|1] [map] [nf|be] [ahead] [lateral] [model]" >&2; exit 2 ;;
esac
DEV_NO_RECORDER="${DEV_NO_RECORDER:-1}" DEV_TIMESCALE=1 DEV_ROSTER="$roster" \
DEV_PRE="rcbot_empires_vehicle_vf_grace 0;rcbot_empires_auto_commander 0;rcbot_empires_engineer_turrets 0;emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_vehicle_curve_speed 0;rcbot_empires_vehicle_maneuvers $mode${DEV_PRE:+;$DEV_PRE}" \
DEV_CMDS="ent_fire emp_params $resources 3000;rcbotd empires_vehicle_obstacle $ahead $lateral 450 $model;waitfor 90 ^\[RCBOT2\] vehicle obstacle placed" \
DEV_UNTIL='^\[RCBOT2\] vehicle obstacle passed ' DEV_EXPECT='^\[RCBOT2\] vehicle obstacle passed ' \
DEV_FAIL='vehicle obstacle (timeout|abort)|empires_vehicle_obstacle armed=0' \
DEV_SHOW="${DEV_SHOW:-vehicle (bought|buy-spot|obstacle|maneuver-retry|maneuver-perf|stuck-reverse|stuck-back-turn|route-block|give-up)}" \
exec scripts/dev.sh "$map" 1 3
