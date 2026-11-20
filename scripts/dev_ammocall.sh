#!/usr/bin/env bash
# Fast check (TODO 3.10b step 3): a bought tank far from the pad runs out of cannon and MG ammo
# (rcbotd empires_vehicle_setammo 0 0 after DEV_AMMO_WAIT s, default 40), calls for ammo
# (emp_menu_command 29), a free team engineer (two in the roster, one is often busy building) walks over and places a supply crate (engSelect 7),
# the tank refills there (expected ammo-crate placed, crate-done, resupplied). Ca. 2-3 min.
# DEV_AMMO_TANKS=2: a third rifleman, both drivers (userid 3 and 4) buy first, then both tanks run
# dry at once (one call per vehicle, each engineer takes its own).
# Usage: [DEV_AMMO_WAIT=s] [DEV_AMMO_TANKS=2] scripts/dev_ammocall.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ "${DEV_AMMO_TANKS:-1}" == 2 ]]; then
  ROSTER="2:1 2:1 2:1 2:3 2:3"; BOTS=5; BOUGHT="waitfor 200 vehicle bought userid=3;waitfor 200 vehicle bought userid=4"
  RESULT='^AMMO_CALL_DONE\s*$'; MINUTES=10
  DONE=""
  for driver in 3 4; do
    DONE+=";waitfor 180 ^\[RCBOT2\] vehicle resupplied userid=$driver ;waitfor 20 ^\[RCBOT2\] vehicle loadout-check userid=$driver .* known=1 .*heavy-left=1 "
  done
  DONE+=";echo AMMO_CALL_DONE;waitfor 5 $RESULT"
  # Earlier than 40 s: the first buyer reaches the enemy base by then and backs off damaged (a damage
  # retreat skips the ammo check, run 20261115-014540).
  WAIT="${DEV_AMMO_WAIT:-20}"
else
  ROSTER="2:1 2:1 2:3 2:3"; BOTS=4; BOUGHT="waitfor 200 vehicle bought"
  RESULT='^\[RCBOT2\] vehicle loadout-check .* known=1 .*heavy-left=1 '
  DONE=";waitfor 180 ^\[RCBOT2\] vehicle resupplied ;waitfor 20 $RESULT"
  MINUTES=15; WAIT="${DEV_AMMO_WAIT:-40}"
fi
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="${DEV_ROSTER:-$ROSTER}" \
DEV_PRE="emp_sv_debug_instant_build 1;rcbot_empires_commander_first 4;emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_vehicles 0" \
DEV_CMDS="wait 5;ent_fire emp_params ModifyNFResources 3000;waitfor 200 pad-yaw;wait 5;ent_fire emp_params ModifyNFResources 3000;rcbot_empires_vehicles 1;$BOUGHT;wait $WAIT;mark;rcbotd empires_vehicle_setammo 0 0$DONE" \
DEV_UNTIL="$RESULT" DEV_EXPECT="$RESULT" \
DEV_FAIL='^\[RCBOT2\] vehicle (retreat-timeout|retreat-abort|give-up) ' \
DEV_SHOW="\[RCBOT2\] (vehicle (bought|debug-ammo|retreat|ammo-call|crate-|resupplied|refit-sent|repaired|retreat-timeout|retreat-abort|pad-enter|loadout-check)|ammo-crate)" \
exec scripts/dev.sh "${1:-emp_crossroads}" "$BOTS" "$MINUTES"
