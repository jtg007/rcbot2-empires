#!/usr/bin/env bash
# Fast check of the chain Commander seat -> place barracks -> Engineer builds it.
# Two NF bots, time lapse, ends as soon as the build result is known (~1-2 min).
# Usage: scripts/dev_chain.sh [map] [timescale]
cd "$(dirname "${BASH_SOURCE[0]}")/.."
DEV_NO_RECORDER="${DEV_NO_RECORDER-1}" DEV_TIMESCALE="${2:-4}" DEV_ROSTER="2:1 2:3" \
DEV_PRE="rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0;rcbot_empires_engineer_turrets 0" DEV_CMDS="wait 2;rcbotd empires_commander_entry_once;waitfor 180 commander-entry result=COMPLETE;wait 2;rcbotd empires_commander_place_barracks" \
DEV_UNTIL="build-assign result=|build-assign requested=0" \
DEV_SHOW="commander-entry (result|stage=approach)|commander-place|build-assign|repair=result" \
exec scripts/dev.sh "${1:-emp_streetsoffire}" 2 8
