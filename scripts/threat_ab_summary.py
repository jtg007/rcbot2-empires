#!/usr/bin/env python3
"""A/B numbers for rcbot_empires_threat_map from finished run folders (read-only, no server).

Usage: scripts/threat_ab_summary.py RUN_DIR [RUN_DIR ...]
Per run: threat-map cost, hurt cover (start/none/retreat, died in cover), engineer stalls at enemy
vehicles, recorder totals (kills, STUCK, idle, engineer idle/stuck) and the MATCH_FLOW verdicts.
A run counts as B (map on) when its server.log has threat-map lines.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def recorder(run):
    path = os.path.join(run, "summary.txt")
    rows, total = [], None
    if not os.path.isfile(path):
        return rows, total
    inside = False
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("Bot "):
                inside = True
                continue
            if not inside:
                continue
            parts = line.split()
            if line.startswith("TOTAL") and len(parts) >= 11:
                # TOTAL alive moved STUCK Stuck Stuck_s Idle_s Kills Deaths Suic TK
                total = dict(zip(["alive", "moved", "STUCK", "stuck", "stuck_s", "idle_s", "kills", "deaths"],
                                 map(float, parts[1:9])))
                break
            if line.startswith("#") and len(parts) >= 13:
                rows.append({"userid": parts[0][1:], "class": parts[2], "STUCK": float(parts[5]),
                             "stuck_s": float(parts[7]), "idle_s": float(parts[8]), "deaths": float(parts[10])})
    return rows, total


def summarize(run):
    log = os.path.join(run, "server.log")
    with open(log, errors="replace") as f:
        text = f.read()
    lines = text.splitlines()

    def count(pattern):
        rx = re.compile(pattern)
        return sum(1 for line in lines if rx.search(line))

    out = {"run": os.path.basename(run.rstrip("/"))}
    tm = [(float(a), float(b)) for a, b in re.findall(r"threat-map team=\d .*?us_avg=([\d.]+) us_max=([\d.]+)", text)]
    out["side"] = "B(map=1)" if tm else "A(map=0)"
    out["map_us_avg"] = sum(a for a, _ in tm) / len(tm) if tm else 0.0
    out["map_us_max"] = max((b for _, b in tm), default=0.0)
    out["cover_start"] = count(r"\] cover start ")
    out["cover_retreat"] = count(r"\] cover start .*route=threat-retreat")
    out["cover_none"] = count(r"\] cover none ")
    out["cover_none_low_hp"] = sum(1 for hp in re.findall(r"\] cover none .* hp=(-?\d+)", text) if int(hp) <= 25)
    out["cover_died"] = count(r"\] cover end .*result=died")
    out["cover_alive"] = count(r"\] cover end .*result=alive")
    ended = out["cover_died"] + out["cover_alive"]
    out["cover_died_pct"] = 100.0 * out["cover_died"] / ended if ended else 0.0
    out["deaths"] = count(r"director death team=")

    rows, total = recorder(run)
    engineers = {r["userid"] for r in rows if r["class"].lower().startswith("engineer")}
    stuck_vehicle = re.findall(r"navmesh-stuck-vehicle userid=(\d+)", text)
    out["eng_stuck_vehicle"] = sum(1 for uid in stuck_vehicle if uid in engineers)
    out["eng_escalate"] = count(r"stall-escalate who=engineer ")
    out["eng_escalate_vehicle"] = count(r"stall-escalate who=engineer .*reason=enemy-vehicle")
    out["eng_fail_threat"] = count(r"build-assign result=FAIL_THREAT")
    out["eng_fail_stall"] = count(r"build-assign result=FAIL_STALL")
    out["econ_defer"] = count(r"commander-econ defer .*reason=enemy-vehicle")
    out["eng_idle_s"] = sum(r["idle_s"] for r in rows if r["userid"] in engineers)
    out["eng_stuck_s"] = sum(r["stuck_s"] for r in rows if r["userid"] in engineers)
    if total:
        out["kills"] = total["kills"]
        out["STUCK"] = total["STUCK"]
        out["idle_s"] = total["idle_s"]

    flow = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scripts", "match_flow.py"), log,
                           "--flow", os.path.join(ROOT, "MATCH_FLOW.md")], capture_output=True, text=True)
    verdicts, team = [], "?"
    for line in flow.stdout.splitlines():
        head = re.match(r"^(NF|BE) \(team", line)
        if head:
            team = head.group(1)
            continue
        m = re.match(r"^\s+([MR]\d+)\s.*?\s{2,}([A-Z]+)\b", line)
        if m:
            verdicts.append((team + ":" + m.group(1), m.group(2)))
    out["flow_ok"] = sum(1 for _, v in verdicts if v == "OK")
    out["flow_bad"] = ",".join(k for k, v in verdicts if v != "OK") or "-"
    return out


KEYS = ["side", "map_us_avg", "map_us_max", "cover_start", "cover_retreat", "cover_none", "cover_none_low_hp",
        "cover_died", "cover_alive", "cover_died_pct", "deaths", "eng_stuck_vehicle", "eng_escalate",
        "eng_escalate_vehicle", "eng_fail_threat", "eng_fail_stall", "econ_defer", "eng_idle_s", "eng_stuck_s",
        "kills", "STUCK", "idle_s", "flow_ok", "flow_bad"]


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    results = [summarize(run) for run in argv[1:]]
    width = max(len(k) for k in KEYS)
    print("%-*s  %s" % (width, "run", "  ".join("%-22s" % r["run"][:22] for r in results)))
    for key in KEYS:
        cells = []
        for r in results:
            v = r.get(key, "-")
            cells.append("%-22s" % (("%.1f" % v) if isinstance(v, float) else v))
        if key == "flow_bad":
            for r in results:
                print("%-*s  %s %s" % (width, key, r["run"][:22], r.get(key, "-")))
            continue
        print("%-*s  %s" % (width, key, "  ".join(cells)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
