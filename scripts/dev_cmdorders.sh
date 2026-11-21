#!/usr/bin/env bash
# TODO 5.4 / 5.8: the auto commander bot sits and orders its free infantry by team-director role
# (attack location, type 2): push goals, hold goal, defense. Expected: commander-sector lines with
# role=push/hold/defense, commander-order start type=2, then the first flag gained (run ends there).
# Compare with rcbot_empires_commander_orders 0 (DEV_CMDORDERS_OFF=1): time to the first flag.
# Usage: [DEV_CMDORDERS_OFF=1] scripts/dev_cmdorders.sh [map] [timescale] [game minutes]   (~4 min; map needs a CV and capture points: emp_palmbay)
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ON=1; [[ -n "${DEV_CMDORDERS_OFF:-}" ]] && ON=0
UNTIL="team-director team=2 flag-gained"
DEV_NO_RECORDER="${DEV_NO_RECORDER-}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:3 2:1 2:1 2:1 3:1" \
DEV_PRE="emp_sv_research_complete 1;rcbot_empires_vehicles 0;rcbot_empires_commander_orders $ON" \
DEV_UNTIL="$UNTIL" \
DEV_SHOW="commander-entry result|commander-orders|commander-sector|team-director|commander-order (start|rearm) type=2|kill-spawn|NOT found" \
exec scripts/dev.sh "${1:-emp_palmbay}" 5 "${3:-15}"
