#!/usr/bin/env bash
# Lawliet mode (TODO 5.14b) end-to-end test: forced NF crewed APC rush on BE CV at 1x.
# Usage: scripts/dev_lawliet.sh [map] [game-minutes]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
MAP="${1:-emp_crossroads}"
MINUTES="${2:-5}"
ROSTER="${LAWLIET_ROSTER:-2:3 2:0 2:0 2:1 2:1 2:3 3:3}"
BOTS=$(wc -w <<<"$ROSTER")
PRE="rcbot_empires_vehicle_vf_grace 0;emp_sv_resource_multiplier_be 10;emp_sv_resource_multiplier_nf 10;emp_sv_refinery_multiplier 30;emp_sv_research_complete 1;rcbot_empires_lawliet 2;emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_engineer_turrets 0"
if [[ "${LAWLIET_REAL_VF:-0}" == 1 ]]; then
  PRE="rcbot_empires_vehicle_vf_grace 0;emp_sv_resource_multiplier_be 10;emp_sv_resource_multiplier_nf 10;emp_sv_refinery_multiplier 30;emp_sv_research_complete 1;rcbot_empires_lawliet 2;rcbot_empires_commander_first 5;rcbot_empires_engineer_turrets 0"
fi
DEV_NO_RECORDER="${DEV_NO_RECORDER:-1}" DEV_TIMESCALE="${DEV_TIMESCALE:-1}" DEV_ROSTER="$ROSTER" \
DEV_PRE="$PRE${LAWLIET_PRE:+;$LAWLIET_PRE}" \
DEV_CMDS="${LAWLIET_CMDS:-}" \
DEV_UNTIL="${LAWLIET_UNTIL:-lawliet end team=2 reason=cv-destroyed}" \
DEV_SHOW="${LAWLIET_SHOW:-lawliet |vehicle (apc-buy|bought|passenger-exit|give-up|factory-exit)|stall-escalate who=lawliet}" \
exec scripts/dev.sh "$MAP" "$BOTS" "$MINUTES"
