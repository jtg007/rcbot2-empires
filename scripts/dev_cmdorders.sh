#!/usr/bin/env bash
# TODO 5.4: the auto commander bot sits and orders its free attackers (attack location, type 2)
# to the next capture point. Expected: commander-orders issue units>=3, each rifleman
# commander-order start type=2, the point is taken, then retarget (or release) without a rearm loop.
# Compare with rcbot_empires_commander_orders 0 (DEV_CMDORDERS_OFF=1): time to first capture.
# Usage: [DEV_CMDORDERS_OFF=1] scripts/dev_cmdorders.sh [map] [timescale] [game minutes]   (~4 min; map needs a CV and capture points: emp_palmbay)
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ON=1; [[ -n "${DEV_CMDORDERS_OFF:-}" ]] && ON=0
UNTIL="commander-orders (retarget|release)"; [[ $ON == 0 ]] && UNTIL="no-such-line"
DEV_NO_RECORDER="${DEV_NO_RECORDER-}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:3 2:1 2:1 2:1 3:1" \
DEV_PRE="emp_sv_research_complete 1;rcbot_empires_vehicles 0;rcbot_empires_commander_orders $ON" \
DEV_UNTIL="$UNTIL" \
DEV_SHOW="commander-entry result|commander-orders|commander-order (start|rearm) type=2|NOT found" \
exec scripts/dev.sh "${1:-emp_palmbay}" 5 "${3:-15}"
