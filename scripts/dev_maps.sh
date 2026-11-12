#!/usr/bin/env bash
# Map sweep: 10 bots per map, one after another, prints one summary block per map.
# Usage: scripts/dev_maps.sh [out-file] ["map:minutes:timescale ..."]
# Default list: baseline at normal speed, the rest at time lapse 2 (10 game minutes).
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT="${1:-/tmp/dev_maps_$(date +%H%M%S).txt}"
LIST="${2:-emp_streetsoffire:10:1 emp_crossroads:10:2 emp_cyclopean:10:2 emp_palmbay:10:2 emp_coast:10:2}"
: >"$OUT"
for entry in $LIST; do
  IFS=: read -r map minutes ts <<<"$entry"
  echo "=== $map (${minutes} min, timescale $ts)" >>"$OUT"
  DEV_TIMESCALE="$ts" scripts/dev.sh "$map" 10 "$minutes" >"$OUT.$map.full" 2>&1
  echo "exit=$?" >>"$OUT"
  grep -a "TOTAL\|Stuck share\|Path failures\|Navigator\|Captures\|Crashes\|Longest stuck\|^  *[0-9]* s  #\|Build failed\|not found\|did not become ready" "$OUT.$map.full" >>"$OUT"
  echo >>"$OUT"
done
echo DONE >>"$OUT"
