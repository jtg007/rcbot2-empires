#!/usr/bin/env bash
# Fast check: an Engineer walls its own refinery's enemy side with a three-part wall (kit item 6) and
# places a camera behind it (item 4; rcbot_empires_engineer_deployables 3, default). Ends when the camera is
# done (or the job escalates).
# Usage: scripts/dev_deploy.sh [map] [timescale]   (DEPLOY_MODE=1 camera/radar only, 2 walls only)
# DEPLOY_RANK=1: rank the engineer up first (emp_increase_rank 5x = 50 points, sv_cheats) and respawn it, so it
# has two detection and two turret slots: expects two turrets, then wall, camera and radar. No auto commander
# (the engineer would take the seat after its respawn); uses the map's start refinery.
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ "${DEPLOY_RANK:-0}" == 1 ]]; then
  DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:3" \
  DEV_PRE="sv_cheats 1;emp_sv_resource_multiplier_be 10;emp_sv_resource_multiplier_nf 10;emp_sv_refinery_multiplier 30;emp_sv_research_complete 1;rcbot_empires_vehicles 0;rcbot_empires_auto_commander 0;rcbot_empires_engineer_deployables ${DEPLOY_MODE:-3}" \
  DEV_CMDS="wait 8;rcbotd empires_test_botcmd 3 5 emp_increase_rank;wait 4;rcbotd empires_test_botcmd 3 1 kill" \
  DEV_UNTIL="${DEV_UNTIL:-deploy done .*item=radar|stall-escalate who=deploy stage=[14]}" \
  DEV_EXPECT='^\[RCBOT2\] deploy done .*item=radar' \
  DEV_FAIL='stall-escalate who=deploy stage=[14]' \
  DEV_SHOW="deploy (start|done|placed|end)|refinery-turret (start|complete|failed|limit)|turret-upgrade (start|done)" \
  exec scripts/dev.sh "${1:-emp_crossroads}" 2 15
fi
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="${DEV_ROSTER:-2:1 2:3}" \
DEV_PRE="emp_sv_resource_multiplier_be 10;emp_sv_resource_multiplier_nf 10;emp_sv_refinery_multiplier 30;emp_sv_research_complete 1;rcbot_empires_vehicles 0;rcbot_empires_engineer_deployables ${DEPLOY_MODE:-3}" \
DEV_CMDS="" \
DEV_UNTIL="${DEV_UNTIL:-deploy done .*item=(radar|camera)|stall-escalate who=deploy}" \
DEV_EXPECT='^\[RCBOT2\] deploy done .*item=(radar|camera)' \
DEV_FAIL='stall-escalate who=deploy' \
DEV_SHOW="deploy |refinery-turret (complete|failed)" \
exec scripts/dev.sh "${1:-emp_crossroads}" 2 12
