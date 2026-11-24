#!/usr/bin/env bash
# Fast check: an Engineer walls its own refinery's enemy side with a three-part wall (kit item 6) and
# places a radar/camera behind it (items 4/5, rcbot_empires_engineer_deployables 1, default).
# Ends when a radar or camera is done (or the job escalates).
# Usage: scripts/dev_deploy.sh [map] [timescale]   (DEPLOY_MODE=1 radar/camera only, 2 walls only)
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="${DEV_ROSTER:-2:1 2:3}" \
DEV_PRE="rcbot_empires_vehicles 0;rcbot_empires_engineer_deployables ${DEPLOY_MODE:-3}" \
DEV_CMDS="wait 5;ent_fire emp_params ModifyNFResources 3000" \
DEV_UNTIL="${DEV_UNTIL:-deploy done .*item=(radar|camera)|stall-escalate who=deploy}" \
DEV_EXPECT='^\[RCBOT2\] deploy done .*item=(radar|camera)' \
DEV_FAIL='stall-escalate who=deploy' \
DEV_SHOW="deploy |refinery-turret (complete|failed)" \
exec scripts/dev.sh "${1:-emp_crossroads}" 2 12
