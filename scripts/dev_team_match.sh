#!/usr/bin/env bash
# Team match check (TODO 5.8 / 5.28): 10 bots, timescale 1, recorder on, then the team-director numbers:
# role swaps push<->defense (total, max per bot), early lock breaks, alerts, alert at a hold goal,
# kill-spawns, building stalls (navmesh-building-blocked), the recorder table (kills, stuck) and the
# match-flow check against MATCH_FLOW.md (scripts/match_flow.py; measurement only, never changes the exit status).
# Usage: scripts/dev_team_match.sh [map] [minutes]   (default emp_palmbay 12)
#        scripts/dev_team_match.sh --summarize RUN_DIR  (read-only, no server start)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
status=0
if [[ "${1:-}" == --summarize ]]; then
  if [[ $# != 2 || -z "$2" ]]; then
    echo "Usage: $0 --summarize RUN_DIR" >&2
    exit 2
  fi
  RUN="$2"
else
  MAP="${1:-emp_palmbay}"
  MINUTES="${2:-12}"
  export DEV_TIMESCALE="${DEV_TIMESCALE:-1}"
  OUT="$(mktemp)"
  trap 'rm -f "$OUT"' EXIT
  "$ROOT/scripts/dev.sh" "$MAP" 10 "$MINUTES" 2>&1 | tee "$OUT" |
    awk '/^== Run|Health check|Crashes|Run result/ {print; fflush()}' || status=$?
  RUN="$(sed -nE 's/^== Run: .*\((.*)\)$/\1/p' "$OUT" | head -n 1)"
  if [[ $status != 0 && -z "$RUN" ]]; then
    tail -n 8 "$OUT" >&2
    exit "$status"
  fi
fi
LOG="$RUN/server.log"
if [[ -z "$RUN" || ! -f "$LOG" || ! -r "$LOG" ]]; then
  echo "Cannot read server log: $LOG" >&2
  if [[ $status != 0 ]]; then exit "$status"; fi
  exit 1
fi
count() { grep -cE "$1" "$LOG" || true; }
echo "== Team director ($RUN)"
echo "role changes:        $(count 'team-director role .*from=(push|hold|defense)')"
echo "push<->defense:      $(count 'role=(push from=defense|defense from=push)')"
echo "max per bot:         $(grep -oE 'team=. userid=[0-9]+ role=(push from=defense|defense from=push)' "$LOG" | cut -d' ' -f1,2 | sort | uniq -c | sort -rn | awk 'NR==1{print $1} END{if(NR==0)print 0}')"
echo "early (<40 s):       $(grep -E 'team-director role .*from=(push|hold|defense)' "$LOG" | grep -oE 'held=[0-9]+' | awk -F= '$2<40' | wc -l)"
echo "alert lines:         $(count 'phase=BASE_ALERT')"
echo "alert at hold goal:  $(count 'alert-hold=[0-9]')"
echo "alert switches:      $(count 'alert-switch ')"
echo "kill-spawn done:     $(count 'kill-spawn userid=')  skip: $(count 'kill-spawn skip')"
echo "building stalls:     $(count 'navmesh-building-blocked')  (goal at building: $(count 'navmesh-building-blocked .*goal_at_building=1'))"
echo "navmesh-stuck:       $(count 'navmesh-stuck ')  hit building: $(count 'navmesh-stuck-trace lift=2 .*hit=emp_building')"
echo "detour no-exit:      $(count 'nav-detour-fail reason=no-exit')"
echo "give-up / escalate:  $(count 'give-up')  / $(count 'stall-escalate')"
if [[ -f "$RUN/summary.txt" ]]; then
  echo "== Recorder"
  sed -nE '/^Bot +Team/,/^TOTAL/p' "$RUN/summary.txt"
else
  echo "Recorder table unavailable (no summary.txt)"
fi
python3 -I "$ROOT/scripts/match_flow.py" "$LOG" --flow "$ROOT/MATCH_FLOW.md" || true
exit "$status"
