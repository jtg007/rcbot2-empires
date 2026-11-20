#!/usr/bin/env bash
# Map sweep: 10 bots per map, one after another, prints one summary block per map.
# Usage: scripts/dev_maps.sh [out-file] ["map:minutes:timescale ..."]
# Default list: baseline at normal speed, the rest at time lapse 2 (10 game minutes).
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT="${1:-/tmp/dev_maps_$(date +%H%M%S).txt}"
LIST="${2:-emp_streetsoffire:10:1 emp_crossroads:10:2 emp_cyclopean:10:2 emp_palmbay:10:2 emp_coast:10:2}"
: >"$OUT"
if [[ "${DEV_SKIP_BUILD:-0}" != 1 ]]; then
  if ! built="$(scripts/dev_build.sh 2>"$OUT.build")"; then
    echo "Build failed: $OUT.build" | tee -a "$OUT" >&2
    exit 1
  fi
  hash="$(sha256sum < "$built")" || exit 1
  export DEV_BUILD_HASH="${DEV_BUILD_HASH:-${hash%% *}}"
fi
export DEV_SKIP_BUILD=1
read -r -d '' -a entries < <(printf '%s\0' "$LIST")
failed=0
for entry in "${entries[@]}"; do
  IFS=: read -r map minutes ts <<<"$entry"
  echo "=== $map (${minutes} min, timescale $ts)" >>"$OUT"
  status=0
  DEV_TIMESCALE="$ts" scripts/dev.sh "$map" 10 "$minutes" >"$OUT.$map.full" 2>&1 || status=$?
  [[ "$status" == 0 ]] || failed=1
  echo "exit=$status" >>"$OUT"
  grep -a "== Run:\|Run result:\|TOTAL\|Stuck share\|Path failures\|Navigator\|Captures\|Crashes\|Longest stuck\|^  *[0-9]* s  #\|Build failed\|Build hash mismatch\|not found\|did not become ready" "$OUT.$map.full" >>"$OUT"
  echo >>"$OUT"
done
echo DONE >>"$OUT"
exit "$failed"
