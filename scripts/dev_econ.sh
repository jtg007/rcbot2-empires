#!/usr/bin/env bash
# Quick match with production economy (no debug purchase, research not complete, resources x1):
# 6 bots, fast-forward 4, ends as soon as a bot drives a vehicle it bought.
# Fast-forward breaks mortars (see AGENTS.md); for the final check use timescale 1 and 10 bots.
# Usage: scripts/dev_econ.sh [map] [game-minutes]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
MAP="${1:-emp_crossroads}"
MINUTES="${2:-20}"
export DEV_TIMESCALE="${DEV_TIMESCALE:-4}"
export DEV_NO_RECORDER=1
export DEV_ROSTER="${DEV_ROSTER:-2:3 3:3 2:3 3:3 2:1 3:1}"
export DEV_PRE="rcbot_empires_vehicle_vf_grace 0;emp_sv_resource_multiplier_be 1;emp_sv_resource_multiplier_nf 1;emp_sv_refinery_multiplier 1;emp_sv_research_complete 0;emp_sv_research_multiplier 1;emp_sv_debug_build_vehicles 0;emp_sv_respawn_penalty 10;emp_sv_commander_freeze_time 60${DEV_PRE:+;$DEV_PRE}"
export DEV_UNTIL="${DEV_UNTIL:-vehicle stage userid=[0-9]+ wait-spawn->driving}"
export DEV_EXPECT="${DEV_EXPECT:-$DEV_UNTIL}"
export DEV_SHOW="${DEV_SHOW:-commander-econ (result=CREATED|research start)|wait-spawn->driving|reason=stage-timeout}"
exec "$ROOT/scripts/dev.sh" "$MAP" 6 "$MINUTES"
