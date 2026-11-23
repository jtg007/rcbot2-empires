#!/usr/bin/env bash
# Fast check: an Engineer heals a hurt team mate with the kit. The rifleman (userid 3)
# is placed 360 units from the engineer (crossroads NF base) and takes 60 damage by cheat;
# ends when the heal ends.
# Usage: scripts/dev_heal.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-1}" DEV_ROSTER="2:3 2:1" \
DEV_PRE="sv_cheats 1;rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0;rcbot_empires_engineer_turrets 0" \
DEV_CMDS="${DEV_HEAL_CMDS:-waitfor 60 spawn-select userid=3;wait 2;rcbotd empires_test_botcmd 2 1 setpos 403 -12307 -220;rcbotd empires_test_botcmd 3 1 setpos 760 -12234 -220;rcbotd empires_test_botcmd 3 1 hurtme 60;waitfor 40 heal end;wait 3;rcbotd empires_test_botcmd 3 1 hurtme 30}" \
DEV_UNTIL="heal end .*reason=(healed|no-heal|timeout)" \
DEV_EXPECT='^\[RCBOT2\] heal end .*reason=healed' \
DEV_SHOW="heal (start|end)|stall-escalate who=healer" \
exec scripts/dev.sh "${1:-emp_crossroads}" 2 2
