#!/usr/bin/env bash
# Fast check: an Engineer with the revive skill (rank raised by cheat) revives a
# killed team mate. Ends when a revive succeeds or the time limit is reached.
# Usage: scripts/dev_revive.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:3 2:1" \
DEV_PRE="sv_cheats 1;rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0" \
DEV_CMDS="wait 10;rcbotd empires_test_botcmd 2 30 emp_increase_rank;wait 8;rcbotd empires_test_botcmd 2 1 kill;rcbotd empires_test_botcmd 3 1 kill;waitfor 90 engineer-skills userid=2.*gate_revive=1;wait 4;rcbotd empires_test_botcmd 3 1 kill" \
DEV_UNTIL="revive end .*reason=revived" \
DEV_EXPECT='^\[RCBOT2\] revive end .*reason=revived' \
DEV_SHOW="revive " \
exec scripts/dev.sh "${1:-emp_crossroads}" 2 3
