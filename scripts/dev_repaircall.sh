#!/usr/bin/env bash
# Short test (TODO K9, 8.3 humans first for repair): two riflemen buy a tank in place (debug buy, no
# commander, so no repair pad: a damaged bot tank goes home and calls engineers). Driver 3 counts as
# human (rcbot_empires_test_human_userid 3) and holds still in its tank like a parked player
# (rcbot_empires_test_human_hold 1); engineer 5 is put beside it (empires_test_bring, engineers roam
# off to resource points otherwise). Both tanks are damaged; bot driver 2 retreats and calls for repair.
# Expected: an engineer repairs the human tank first (buddy-repair start ... driver=3). Once the bot
# call stands, both tanks are damaged again: the human tank takes the request over
# (buddy-repair human-first), the bot call waits (repair-call-wait), the bot tank is repaired afterwards.
# Usage: [REPAIR_PRE="cvar v;..."] scripts/dev_repaircall.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="${DEV_ROSTER:-2:1 2:1 2:3 2:3}" \
DEV_PRE="rcbot_empires_auto_commander 0;rcbot_empires_vehicle_vf_grace 0;emp_sv_resource_multiplier_be 10;emp_sv_resource_multiplier_nf 10;emp_sv_refinery_multiplier 30;emp_sv_research_complete 1;rcbot_empires_engineer_turrets 0;rcbot_empires_engineer_deployables 0;rcbot_empires_tank_escort 0;emp_sv_debug_build_vehicles 1;rcbot_empires_vehicle_debug_buy 1;rcbot_empires_test_human_userid 3;rcbot_empires_test_human_hold 1${REPAIR_PRE:+;$REPAIR_PRE}" \
DEV_CMDS="wait 5;waitfor 200 vehicle bought userid=2;waitfor 200 vehicle bought userid=3;wait 15;mark;rcbotd empires_vehicle_scalehp 0.35 0.25 0.25 0.25 0.25;rcbotd empires_test_bring 5 3 300;waitfor 60 ^\[RCBOT2\] buddy-repair start .*driver=3 ;waitfor 240 ^\[RCBOT2\] vehicle repair-call userid=2 ;mark;rcbotd empires_vehicle_scalehp 0.5 0.6 0.6 0.6 0.6;rcbotd empires_test_bring 5 3 300;waitfor 60 ^\[RCBOT2\] buddy-repair human-first ;waitfor 300 ^\[RCBOT2\] vehicle repaired userid=2 ;echo REPAIR_CALL_DONE;waitfor 5 ^REPAIR_CALL_DONE\s*$" \
DEV_UNTIL='^REPAIR_CALL_DONE\s*$' DEV_EXPECT='^REPAIR_CALL_DONE\s*$' \
DEV_FAIL="${REPAIR_FAIL:-^\[RCBOT2\] vehicle (retreat-timeout|retreat-abort|give-up )}" \
DEV_SHOW="\[RCBOT2\] (vehicle (bought|test-human-hold|retreat|repair-call|repair-walk|repair-assist|repaired|retreat-timeout|retreat-abort|reset .*repair)|buddy-repair (start|human-first)|repair-call-wait)|test-bring" \
exec scripts/dev.sh "${1:-emp_crossroads}" 4 10
