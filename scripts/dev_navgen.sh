#!/usr/bin/env bash
# Generate the navmesh for a map (empires/maps/<map>.nav) on the test server: 2 bots as seeds,
# p_nav_generate, ends when the file is saved. -nowatchdog: CreateNavAreasFromNodes runs in one
# frame and takes longer than the watchdog allows on big maps (emp_dmz_b1).
# The map's own files must be on the test server (copy what maps/<map>.res lists).
# Doors and sliding gates (func_door*, prop_door*, func_movelinear) count as passable while
# generating, so ways behind gates that open later (emp_dmz_b1) are in the mesh; routing blocks
# them while shut. DEV_NAVGEN_PRE="cmd;cmd": extra console commands before generating.
# Usage: [DEV_NAVGEN_PRE="..."] scripts/dev_navgen.sh <map>
cd "$(dirname "${BASH_SOURCE[0]}")/.."
[[ -n "${1:-}" ]] || { echo "usage: scripts/dev_navgen.sh <map>"; exit 1; }
PRE="${DEV_NAVGEN_PRE:-}"
DEV_SRCDS_ARGS="${DEV_SRCDS_ARGS:--nowatchdog}" DEV_NO_RECORDER=1 DEV_TIMESCALE=1 \
DEV_PRE="rcbot_empires_auto_commander 0;rcbot_empires_vehicles 0" \
DEV_CMDS="wait 5;sv_cheats 1${PRE:+;$PRE};p_nav_generate;waitfor 1200 Navigation map .* saved" \
DEV_UNTIL="Navigation map .* saved" DEV_SHOW="Generation complete|Navigation map|NOT found" \
exec scripts/dev.sh "$1" 2 30
