#!/usr/bin/env bash
# Short targeted run instead of a full match: few bots, instant vehicle purchase,
# resources granted. Adapt via variables; defaults = tank vs. enemy building (3.21).
# Usage: scripts/dev_quick.sh [map] [game-minutes]
#   QUICK_ROSTER   bots "team:class ..." (default "2:1 3:3 3:1": NF rifleman, BE engineer+rifleman;
#                  the BE side buys a tank and drives at the NF base and its refinery)
#   QUICK_PRE      extra cvars before the bots (";"-separated)
#   QUICK_CMDS     extra commands after the bots (DEV_CMDS syntax)
#   QUICK_UNTIL    end regex (default: none, runs the full game minutes)
#   QUICK_SHOW     output filter regex
#   QUICK_FAIR=1   fair start: bought vehicles hold (rcbot_empires_vehicle_hold 1) until both teams have
#                  one (NF light tank chassis 2, BE AFV chassis 10), then "mark": waitfor/UNTIL after that
#                  count from the common start. Turns the auto commander off; e.g. QUICK_ROSTER="2:1 3:1".
#   DEV_TIMESCALE  default 1: tank shells and mortars are CEmpShell and explode at the muzzle
#                  with time-lapse (curtime - realtime spawn > 30). Use 4 only for pure driving/economy.
# Examples:
#   scripts/dev_quick.sh                                   # tank vs. building, 3 minutes at 1x
#   QUICK_PRE='rcbot_empires_vehicle_shell_trace 1' scripts/dev_quick.sh   # + shell flight/impact log
#   QUICK_UNTIL='vehicle fire-blocked' scripts/dev_quick.sh emp_crossroads 5
#   QUICK_FAIR=1 QUICK_ROSTER="2:1 3:1" scripts/dev_quick.sh emp_coast 6   # both tanks start together
#   QUICK_ROSTER="2:3 3:3" QUICK_SHOW='repair-walk|commander-econ assign' scripts/dev_quick.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
map="${1:-emp_crossroads}"
minutes="${2:-3}"
roster="${QUICK_ROSTER:-2:1 3:3 3:1}"
bots=$(wc -w <<<"$roster")
pre="emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_engineer_turrets 0"
cmds="ent_fire emp_params ModifyNFResources 3000;ent_fire emp_params ModifyBEResources 3000"
if [[ "${QUICK_FAIR:-0}" == 1 ]]; then
  # No commanders: their orders (escort, move) cancel the purchase walk; debug buying needs no factory.
  pre+=";rcbot_empires_vehicle_hold 1;rcbot_empires_auto_commander 0"
  cmds+=";waitfor 240 ^\[RCBOT2\] vehicle bought .* chassis=(2|3|4|5) ;waitfor 240 ^\[RCBOT2\] vehicle bought .* chassis=(10|11|12|13) "
  cmds+=";echo FAIR_START;mark;rcbot_empires_vehicle_hold 0"
fi
DEV_NO_RECORDER="${DEV_NO_RECORDER:-1}" DEV_TIMESCALE="${DEV_TIMESCALE:-1}" DEV_ROSTER="$roster" \
DEV_PRE="$pre${QUICK_PRE:+;$QUICK_PRE}" \
DEV_CMDS="$cmds${QUICK_CMDS:+;$QUICK_CMDS}" \
DEV_UNTIL="${QUICK_UNTIL:-}" \
DEV_SHOW="${QUICK_SHOW:-vehicle (bought|fire |fire-blocked|stuck-reverse|bail-exit userid=[0-9]+ t=)|building-(rpg|mortar)}" \
exec scripts/dev.sh "$map" "$bots" "$minutes"
