#!/usr/bin/env bash
# Fast check: a BE engineer is put in front of the NF barracks on emp_crossroads and throws its
# seismic grenades at it (5.31.2). Logs arm/throw, the detonation point and the building health
# before/after. An NF scout is parked at the far end of the map: with no NF player left, the
# destroyed barracks ends the round and freezes everyone (flags 0x40). Ends after three results.
# Usage: scripts/dev_seismic.sh [map] [timescale]; DEV_SEISMIC_POS="x y z" moves the start point.
cd "$(dirname "${BASH_SOURCE[0]}")/.."
POS="${DEV_SEISMIC_POS:-400 -12500 -220}"
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-1}" DEV_ROSTER="${DEV_ROSTER:-3:3 2:0}" \
DEV_PRE="sv_cheats 1;rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0;rcbot_empires_engineer_turrets 0" \
DEV_CMDS="${DEV_SEISMIC_CMDS:-waitfor 60 spawn-select userid=3;wait 2;rcbotd empires_test_botcmd 3 1 setpos 0 12500 -220;rcbotd empires_test_botcmd 2 1 setpos $POS;waitfor 30 seismic (result|lost);mark;waitfor 30 seismic (result|lost);mark;waitfor 30 seismic (result|lost);echo SEISMIC_DONE}" \
DEV_UNTIL="SEISMIC_DONE|seismic lost" \
DEV_EXPECT='^\[RCBOT2\] seismic result .*health=([0-9]+)->(?!\1\b)[0-9]+' \
DEV_FAIL='seismic lost' \
DEV_SHOW="seismic (armed|thrown|detonated|result|lost)|spawn-select" \
exec scripts/dev.sh "${1:-emp_crossroads}" 2 4
