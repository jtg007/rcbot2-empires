#!/usr/bin/env bash
# Factory-first driving test at normal simulation speed.
# Usage: scripts/dev_vf_drive.sh [0|1|place] [map] [nf|be] ["x y z"]
# Fixed coordinates are sent to the native build command at yaw 0. The place mode
# stops at the first automatic factory placement result, before buying.
# DEV_VF_APPROACH="x y z" places the fixed-position run's buyer near the entrance.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

mode="${1:-0}"
map="${2:-emp_crossroads}"
team="${3:-nf}"
position="${4:-}"
approach="${DEV_VF_APPROACH:-}"
coordinate='-?[0-9]+(\.[0-9]+)?'
coordinates="^$coordinate[[:blank:]]+$coordinate[[:blank:]]+$coordinate$"
if [[ -n "$approach" && ( -z "$position" || ! "$approach" =~ $coordinates ) ]]; then
  echo "DEV_VF_APPROACH requires a fixed factory and three coordinates." >&2
  exit 2
fi
case "$mode:$team" in
  0:nf|1:nf|place:nf) side=2; crew="2:3 2:3"; chassis=2; driver="4"; grant="ModifyNFResources" ;;
  # Give the Engineer the first chance at the CV and the Rifleman time to leave the barracks.
  0:be|1:be|place:be) side=3; crew="3:3 3:1"; chassis=10; driver="[34]"; grant="ModifyBEResources" ;;
  *) echo "Usage: $0 [0|1|place] [map] [nf|be] [\"x y z\"]" >&2; exit 2 ;;
esac

pre="ent_fire emp_params $grant 5000;rcbot_empires_engineer_turrets 0;rcbot_empires_vehicle_drivers 1"
created='^\[RCBOT2\] commander-econ result=CREATED type=5 '
placement_failed='^\[RCBOT2\] commander-econ result=(NO_SPOT|REJECTED) type=5 '
panel='^\[RCBOT2\] NavMesh building-registry .*classname=emp_building_vehiclefactory_panel '
show="commander-build|commander-econ (factory-yaw|result=(CREATED|NO_SPOT|REJECTED) type=5)|vehicle (bought|factory-exit|stuck|give-up)|^VF_DRIVE_DONE"
done_pattern='^VF_DRIVE_DONE\s*$'
if [[ "$mode" == place ]]; then
  [[ -z "$position" ]] || { echo "The place mode uses automatic placement without coordinates." >&2; exit 2; }
  DEV_ROSTER="$crew" DEV_PRE="$pre${DEV_PRE:+;$DEV_PRE};rcbot_empires_vehicles 0" \
  DEV_CMDS="waitfor 75 $created" DEV_UNTIL="$created" DEV_EXPECT="$created" \
  DEV_FAIL="$placement_failed" DEV_SHOW="${DEV_SHOW:-$show}" \
  exec scripts/dev_buy.sh "$map" 1
fi

pre+=";rcbot_empires_vehicle_curve_speed $mode${DEV_PRE:+;$DEV_PRE}"
if [[ -n "$position" ]]; then
  if [[ ! "$position" =~ $coordinates ]]; then
    echo "Expected three coordinates for the factory's build command." >&2
    exit 2
  fi
  read -r x y z <<<"$position"
  position="$x $y $z"
  LC_NUMERIC=C printf -v placed_xy '%.0f %.0f' "$x" "$y"
  crew="$side:3 $side:3"
  driver="4"
  show+="|test-unit-order|build-assign result"
  pre+=";rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0"
  commands='wait 2;rcbotd empires_commander_entry_once;waitfor 75 commander-entry result=COMPLETE;mark;'
  commands+="rcbotd empires_commander_build 5 $position;waitfor 20 ^\[RCBOT2\] NavMesh building-registry .*classname=emp_building_vehiclefactory ;"
  # The engine adjusts Z during placement. The list records the actual origin.
  commands+="mark;rcbotd empires_commander_build list;waitfor 5 ^\[RCBOT2\] commander-build list building=[0-9]+ class=emp_building_vehiclefactory team=$side pos=\($placed_xy -?[0-9]+\)"
  # The instant-build setting still needs an engineer to use the kit once.
  commands+=";rcbotd empires_test_unit_order 2 4 3 0 0 0 b;waitfor 45 $panel"
else
  commands="waitfor 45 $created;waitfor 45 $panel"
fi

commands+=";rcbotd addbot 1 $side VF_Driver"
if [[ -n "$approach" ]]; then
  pre+=";sv_cheats 1"
  commands+=";waitfor 15 ^\[RCBOT2\] goal-roam pick userid=$driver ;rcbotd empires_test_botcmd $driver 1 setpos $approach"
  show+="|botcmd userid=$driver|vehicle factory-(route|enter)"
fi
[[ -z "$position" ]] || commands+=";rcbot_empires_vehicles 1"
commands+=";waitfor 75 ^\[RCBOT2\] vehicle bought userid=$driver vehicle=[0-9]+ chassis=$chassis ;"
commands+="waitfor 35 ^\[RCBOT2\] vehicle factory-exit end userid=$driver reason=reached ;"
commands+="waitfor 20 ^\[RCBOT2\] vehicle drive userid=$driver veh=[0-9]+ chassis=$chassis .* route=[1-9][0-9]*/;wait 25;echo VF_DRIVE_DONE;waitfor 5 $done_pattern"
DEV_ROSTER="$crew" \
DEV_PRE="$pre" \
DEV_CMDS="$commands" \
DEV_UNTIL="$done_pattern" DEV_EXPECT="$done_pattern" \
DEV_FAIL="^\[RCBOT2\] vehicle (factory-exit end userid=$driver reason=timeout|give-up userid=$driver) |^\[RCBOT2\] commander-build created=[0-9]+ class=emp_building_vehiclefactory rejected=1" \
DEV_SHOW="${DEV_SHOW:-$show}" \
exec scripts/dev_buy.sh "$map" 1
