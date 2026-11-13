#!/usr/bin/env bash
# Fast check: an Engineer with the repair role builds an MG turret at its own
# refinery (rcbot_empires_engineer_turrets 1, default). Ends at the result line.
# Usage: scripts/dev_turret.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:3" \
DEV_PRE="rcbot_empires_vehicles 0" \
DEV_CMDS="wait 5;ent_fire emp_params ModifyNFResources 3000" \
DEV_UNTIL="refinery-turret (complete|unfinished|failed)" \
DEV_SHOW="refinery-turret|build-assign result" \
exec scripts/dev.sh "${1:-emp_crossroads}" 2 10
