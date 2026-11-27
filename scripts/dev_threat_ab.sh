#!/usr/bin/env bash
# A/B for rcbot_empires_threat_map: 10-bot matches at 1x under real economy rules (DEV_PRE from
# dev_econ.sh: resources x1, research from 0, plus rcbot_empires_vehicle_vf_grace 0 for immediate
# vehicle comparisons), alternating map off (A) / on (B), then the A/B table
# (scripts/threat_ab_summary.py: map cost, hurt cover, engineer stalls at enemy vehicles, kills,
# STUCK, idle, MATCH_FLOW). Runs one after another on DEV_PORT (default 49925).
# Usage: scripts/dev_threat_ab.sh [map] [minutes] [pairs]   (default emp_crossroads 18 1)
#        scripts/dev_threat_ab.sh --summarize RUN_DIR [RUN_DIR ...]   (read-only, no server start)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
if [[ "${1:-}" == --summarize ]]; then
  shift
  exec python3 -I "$ROOT/scripts/threat_ab_summary.py" "$@"
fi
MAP="${1:-emp_crossroads}"
MINUTES="${2:-18}"
PAIRS="${3:-1}"
ECON="rcbot_empires_vehicle_vf_grace 0;emp_sv_resource_multiplier_be 1;emp_sv_resource_multiplier_nf 1;emp_sv_refinery_multiplier 1;emp_sv_research_complete 0;emp_sv_research_multiplier 1;emp_sv_debug_build_vehicles 0;emp_sv_respawn_penalty 10;emp_sv_commander_freeze_time 60"
runs=()
built=0
for ((p = 1; p <= PAIRS; ++p)); do
  for side in 0 1; do
    OUT="$(mktemp)"
    # Build once, then the same .so for every run of the comparison.
    skip=$(( built ? 1 : ${DEV_SKIP_BUILD:-0} ))
    echo "== pair $p side $( ((side)) && echo B || echo A ) (rcbot_empires_threat_map $side)"
    DEV_SKIP_BUILD="$skip" DEV_PRE="$ECON;rcbot_empires_threat_map $side${DEV_PRE:+;$DEV_PRE}" \
      "$ROOT/scripts/dev_team_match.sh" "$MAP" "$MINUTES" >"$OUT" 2>&1 || true
    built=1
    run="$(sed -nE 's/^== Team director \((.*)\)$/\1/p' "$OUT" | head -n 1)"
    rm -f "$OUT"
    if [[ -z "$run" ]]; then
      echo "run failed (no run folder)" >&2
      continue
    fi
    echo "   $run"
    runs+=("$run")
  done
done
((${#runs[@]})) || exit 1
python3 -I "$ROOT/scripts/threat_ab_summary.py" "${runs[@]}"
