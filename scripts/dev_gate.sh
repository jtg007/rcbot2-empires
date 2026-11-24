#!/usr/bin/env bash
# Fast gate test on emp_crossroads: two BE vehicles (debug buy) spawn north of a BE base exit at the
# same time and drive to the enemy through it, timescale 4, 3 game minutes (~1.5 min real per batch).
# Runs are started in parallel; half of them with an extra cvar set (A/B), e.g. the 3.16b lane logic off.
# Usage: scripts/dev_gate.sh [east|west] [runs] ["cvars for the B half"]
#   scripts/dev_gate.sh west 6 "rcbot_empires_vehicle_wall_clearance 0;rcbot_empires_vehicle_narrow_cost 0"
# Prints per run: vehicles north of the gate, passes, reversals and give-ups in the gate zone.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
side="${1:-east}"
runs="${2:-4}"
alt="${3:-}"
case "$side" in
  east) a="4250 5950 -180"; b="4250 6500 -180"; box="3300 4700 4500 4950" ;;   # be_gate1, b queues behind a
  west) a="-4100 6100 -180"; b="-4450 6350 -180"; box="-4800 -3500 4900 5500" ;; # west exit
  *) echo "Usage: $0 [east|west] [runs] [\"cvars for the B half\"]" >&2; exit 2 ;;
esac
out="$(mktemp -d)"
base="sv_cheats 1;rcbot_empires_auto_commander 0;rcbot_empires_vehicle_debug_team 3;rcbot_empires_vehicles 0"
cmds="wait 6;rcbotd empires_test_botcmd 3 1 setpos $a;rcbotd empires_test_botcmd 4 1 setpos $b;wait 1;rcbot_empires_vehicles 1"
port0="${DEV_PORT:-49925}"
scripts/dev_build.sh >/dev/null
for ((i = 1; i <= runs; i++)); do
  pre="$base"
  [[ -n "$alt" && $((i % 2)) == 0 ]] && pre+=";$alt"
  (DEV_PORT=$((port0 + i * 10)) DEV_SKIP_BUILD=1 DEV_TIMESCALE=4 QUICK_ROSTER="2:1 3:1 3:1" \
    QUICK_PRE="$pre" QUICK_CMDS="$cmds" QUICK_SHOW='vehicle (bought|give-up )' \
    scripts/dev_quick.sh emp_crossroads 3 > "$out/run$i.log" 2>&1 || true) &
  sleep 3
done
wait
for ((i = 1; i <= runs; i++)); do
  run="$(sed -nE 's/^== Run: .*\((.*)\)$/\1/p' "$out/run$i.log")"
  label=A; [[ -n "$alt" && $((i % 2)) == 0 ]] && label=B
  printf '%s %s ' "$label" "$(basename "$run")"
  # x0 x1 = gate corridor, south = passed below, north = started above
  python3 -I - "$run/server.log" $box <<'EOF'
import re, sys
path, x0, x1, south, north = sys.argv[1], *map(float, sys.argv[2:])
drive = re.compile(r'vehicle drive userid=(\d+) veh=(\d+) .*?pos=\((-?\d+) (-?\d+) ')
pos = re.compile(r'pos=\((-?\d+) (-?\d+) ')
above, passed, rev, gu = set(), set(), 0, 0
for line in open(path, errors='replace'):
    m = drive.search(line)
    if m:
        k, x, y = m.group(1, 2), float(m.group(3)), float(m.group(4))
        if x0 < x < x1:
            if y > north: above.add(k)
            elif y < south and k in above: passed.add(k)
        continue
    p = pos.search(line)
    if p and x0 < float(p.group(1)) < x1 and south - 100 < float(p.group(2)) < north + 800:
        rev += 'stuck-reverse' in line
        gu += 'vehicle give-up ' in line
print(f"north={len(above)} passed={len(passed)} reversals={rev} give-ups={gu}")
EOF
  grep -h "Crashes: YES" "$out/run$i.log" || true
done
echo "Raw output: $out"
