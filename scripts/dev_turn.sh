#!/usr/bin/env bash
# Native vehicle response at 1x. Use the same map for both modes.
# Usage: scripts/dev_turn.sh <1|2|3|4> [map] [nf|be]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
mode="${1:-}"
team="${3:-nf}"
if [[ ! "$mode" =~ ^[1-4]$ ]] || [[ "$team" != nf && "$team" != be ]]; then
  echo "Usage: $0 <1|2|3|4> [map] [nf|be]" >&2
  exit 2
fi
if [[ "$team" == nf ]]; then
  roster="2:1"
else
  roster="3:1"
fi
DEV_NO_RECORDER="${DEV_NO_RECORDER:-1}" DEV_TIMESCALE=1 DEV_ROSTER="$roster" \
DEV_PRE="rcbot_empires_vehicle_vf_grace 0;emp_sv_resource_multiplier_be 10;emp_sv_resource_multiplier_nf 10;emp_sv_refinery_multiplier 30;emp_sv_research_complete 1;rcbot_empires_auto_commander 0;rcbot_empires_engineer_turrets 0;emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_vehicle_turn_test $mode" \
DEV_CMDS="" \
DEV_UNTIL="vehicle turn-test (end|abort)" DEV_EXPECT='^\[RCBOT2\] vehicle turn-test end ' \
DEV_SHOW="${DEV_SHOW:-vehicle (bought|buy-spot|turn-test)}" \
exec scripts/dev.sh "${2:-emp_crossroads}" 1 1
