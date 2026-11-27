#!/usr/bin/env bash
# Fast check: an Engineer with the repair role builds an MG turret at its own
# refinery (rcbot_empires_engineer_turrets 1, default). Ends at the result line.
# Usage: scripts/dev_turret.sh [map] [timescale]
# DEV_TURRET_UPGRADE=1: afterwards rank the engineer up (emp_increase_rank, sv_cheats), respawn it so the
# turret-upgrade skill becomes active, and wait for emp_eng_upgrade (5.6) plus the rebuild. No auto
# commander: with two bots the engineer would take the seat after its respawn.
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ "${DEV_TURRET_UPGRADE:-0}" == 1 ]]; then
  DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:3" \
  DEV_PRE="sv_cheats 1;emp_sv_resource_multiplier_be 10;emp_sv_resource_multiplier_nf 10;emp_sv_refinery_multiplier 30;emp_sv_research_complete 1;rcbot_empires_vehicles 0;rcbot_empires_auto_commander 0" \
  DEV_CMDS="wait 5;waitfor 400 refinery-turret complete;rcbotd empires_test_botcmd 3 5 emp_increase_rank;wait 4;rcbotd empires_test_botcmd 3 1 kill;waitfor 300 turret-upgrade (done|end);mark;waitfor 400 refinery-turret (complete|unfinished|failed);echo UPGRADE_TEST_DONE" \
  DEV_UNTIL="turret-upgrade end|UPGRADE_TEST_DONE" \
  DEV_EXPECT='^\[RCBOT2\] turret-upgrade done ' \
  DEV_FAIL='turret-upgrade end ' \
  DEV_SHOW="refinery-turret (complete|finish)|turret-upgrade|engineer-skills" \
  exec scripts/dev.sh "${1:-emp_crossroads}" 2 12
fi
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:3" \
DEV_PRE="emp_sv_resource_multiplier_be 10;emp_sv_resource_multiplier_nf 10;emp_sv_refinery_multiplier 30;emp_sv_research_complete 1;rcbot_empires_vehicles 0" \
DEV_CMDS="" \
DEV_UNTIL="refinery-turret (complete|unfinished|failed)" \
DEV_EXPECT='^\[RCBOT2\] refinery-turret complete ' \
DEV_SHOW="refinery-turret|build-assign result" \
exec scripts/dev.sh "${1:-emp_crossroads}" 2 10
