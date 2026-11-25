#!/usr/bin/env python3
"""Check the [RCBOT2] match-flow lines of a server.log against MATCH_FLOW.md.

Usage: scripts/match_flow.py SERVER_LOG [--flow MATCH_FLOW.md]
Measurement only: prints OK or the deviation per team and rule, exit status 0 unless the log
or the limits cannot be read. Limits come from the table in MATCH_FLOW.md (keys in backticks).
"""
import argparse
import os
import re
import sys

LINE = re.compile(r"\[RCBOT2\] match-flow (?:(state)|event=(\S+)) (.*)")
PAIR = re.compile(r"(\w+)=(\S+)")
LIMIT = re.compile(r"^\|\s*`(\w+)`\s*\|\s*([0-9.]+)\s*\|")
TEAMS = {2: "NF", 3: "BE"}
CORE = ("refinery", "radar", "factory", "barracks")
KEYS = ("start_econ_placed_by", "start_econ_build", "vf_place_after_research", "vf_build",
        "first_vehicle_after_vf", "min_vehicle_res", "pad_barracks_after_vf", "core_idle_max",
        "order_accept_max", "no_vehicle_max", "hoard_res", "hoard_max", "vehicle_front_max")


def parse_limits(text):
    limits = {}
    for line in text.splitlines():
        m = LIMIT.match(line.strip())
        if m:
            limits[m.group(1)] = float(m.group(2))
    missing = [k for k in KEYS if k not in limits]
    if missing:
        raise ValueError("MATCH_FLOW.md lacks limits: " + ", ".join(missing))
    return limits


def num(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def parse_log(lines):
    """Returns (events, states); each a dict of fields with float 't' and int 'team'."""
    events, states = [], []
    for line in lines:
        m = LINE.search(line)
        if not m:
            continue
        fields = dict(PAIR.findall(m.group(3)))
        fields["t"] = num(fields.get("t"))
        fields["team"] = int(num(fields.get("team"), -1))
        if m.group(1):
            for group in ("built", "raw"):
                fields[group] = {k: int(v) for k, v in
                                 (item.split(":") for item in fields.get(group, "").split(",") if ":" in item)}
            raw = []
            for item in fields.get("rawlist", "-").split(","):
                im = re.match(r"(\w+)#(\d+):([0-9.]+)/([0-9.]+)(?:/([01]))?$", item)
                if im:
                    raw.append({"kind": im.group(1), "ent": int(im.group(2)), "age": float(im.group(3)),
                                "idle": float(im.group(4)),
                                "accepted": im.group(5) == "1"})
            fields["rawlist"] = raw
            fields["res"] = num(fields.get("res"), -1.0)
            fields["vehicles"] = int(num(fields.get("vehicles")))
            states.append(fields)
        else:
            fields["event"] = m.group(2)
            events.append(fields)
    return events, states


def fmt(t):
    return "-" if t is None else "%d:%02d" % (int(t) // 60, int(t) % 60)


def first(items, pred):
    return next((x for x in items if pred(x)), None)


def runs(states, cond):
    """Longest run (start, end) of consecutive state lines where cond holds, by span."""
    best, start, last = None, None, None
    for s in states:
        if cond(s):
            start = s["t"] if start is None else start
            last = s["t"]
            if best is None or last - start > best[1] - best[0]:
                best = (start, last)
        else:
            start = None
    return best


def check_team(team, events, states, lim, end):
    ev = [e for e in events if e["team"] == team]
    st = [s for s in states if s["team"] == team]
    out = []

    def add(name, status, detail):
        out.append((name, status, detail))

    def placed(kind, after=-1.0):
        return first(ev, lambda e: e["event"] == "placed" and e.get("kind") == kind and e.get("initial") != "1"
                     and e["t"] >= after)

    def finished_of(p):
        return None if p is None else first(ev, lambda e: e["event"] == "finished" and e.get("ent") == p.get("ent")
                                            and e["t"] >= p["t"])

    # M1 start economy
    parts, bad = [], False
    for kind in ("refinery", "radar"):
        p = placed(kind)
        if p is None:
            initial = first(ev, lambda e: e["event"] == "placed" and e.get("kind") == kind and e.get("initial") == "1")
            if initial is None:
                parts.append("%s never placed" % kind)
                bad = True
            continue
        f = finished_of(p)
        text = "%s placed %s" % (kind, fmt(p["t"]))
        if p["t"] > lim["start_econ_placed_by"]:
            text += " (late +%ds)" % (p["t"] - lim["start_econ_placed_by"])
            bad = True
        if f is None:
            text += ", unfinished"
            bad = bad or end - p["t"] > lim["start_econ_build"]
        else:
            text += ", built in %ds" % (f["t"] - p["t"])
            if f["t"] - p["t"] > lim["start_econ_build"]:
                text += " (+%ds)" % (f["t"] - p["t"] - lim["start_econ_build"])
                bad = True
        parts.append(text)
    add("M1 start economy", "LATE" if bad else "OK", "; ".join(parts))

    # M2 factory placed after research
    research = first(ev, lambda e: e["event"] == "research-vf")
    anchor, anchor_text = None, "no VF research"
    if research is not None and research.get("instant") != "1":
        anchor, anchor_text = research["t"], "research %s" % fmt(research["t"])
    elif research is not None:
        radar = first(ev, lambda e: e["event"] == "finished" and e.get("kind") == "radar")
        radar_state = first(st, lambda s: s["built"].get("radar", 0) > 0)
        if radar is not None or radar_state is not None:
            anchor = radar["t"] if radar is not None else radar_state["t"]
            anchor_text = "instant research, radar %s" % fmt(anchor)
        else:
            anchor_text = "instant research, no radar"
    factory = placed("factory")
    if anchor is None:
        add("M2 factory placed", "n/a" if factory is None else "OK",
            anchor_text + ("" if factory is None else ", placed %s" % fmt(factory["t"])))
    elif factory is None:
        add("M2 factory placed", "MISSING", "%s, none placed by %s (+%ds)" % (anchor_text, fmt(end), end - anchor))
    else:
        delay = factory["t"] - anchor
        add("M2 factory placed", "OK" if delay <= lim["vf_place_after_research"] else "LATE",
            "%s, placed %s (%+ds)" % (anchor_text, fmt(factory["t"]), delay))

    # M3 factory finished
    vf_done = first(ev, lambda e: e["event"] == "finished" and e.get("kind") == "factory")
    if vf_done is None:
        if factory is None:
            add("M3 factory built", "n/a", "no factory placed")
        else:
            age = end - factory["t"]
            add("M3 factory built", "MISSING" if age > lim["vf_build"] else "open",
                "placed %s, unfinished after %ds" % (fmt(factory["t"]), age))
    else:
        age = num(vf_done.get("age"))
        add("M3 factory built", "OK" if age <= lim["vf_build"] else "LATE",
            "finished %s, %ds after placement (max idle %ss)" % (fmt(vf_done["t"]), age, vf_done.get("max_idle", "?")))

    # M4 first vehicle out
    if vf_done is None:
        add("M4 first vehicle", "n/a", "no finished factory")
    else:
        t0 = vf_done["t"]
        start = first(st, lambda s: s["t"] >= t0 - 30 and s["res"] >= lim["min_vehicle_res"])
        if start is not None and start["t"] > t0:
            t0, why = start["t"], " (res ok %s)" % fmt(start["t"])
        else:
            why = ""
        out_ev = first(ev, lambda e: e["event"] == "vehicle-out" and e["t"] >= vf_done["t"])
        if start is None:
            add("M4 first vehicle", "n/a", "factory %s, res never >= %d" % (fmt(vf_done["t"]), lim["min_vehicle_res"]))
        elif out_ev is None:
            add("M4 first vehicle", "MISSING" if end - t0 > lim["first_vehicle_after_vf"] else "open",
                "factory %s%s, no vehicle out by %s" % (fmt(vf_done["t"]), why, fmt(end)))
        else:
            delay = out_ev["t"] - t0
            add("M4 first vehicle", "OK" if delay <= lim["first_vehicle_after_vf"] else "LATE",
                "factory %s%s, vehicle out %s (+%ds)" % (fmt(vf_done["t"]), why, fmt(out_ev["t"]), delay))

    # M5 repair pad and forward barracks
    if vf_done is None:
        add("M5 pad + fwd barracks", "n/a", "no finished factory")
    else:
        t0, parts, status = vf_done["t"], [], "OK"
        limit = lim["pad_barracks_after_vf"]
        pad = first(ev, lambda e: e["event"] == "finished" and e.get("kind") == "repair" and e["t"] >= t0)
        if pad is None:
            pad = first(ev, lambda e: e["event"] == "finished" and e.get("kind") == "repair")
        fwd = finished_of(placed("barracks", t0))
        for name, f in (("pad", pad), ("fwd barracks", fwd)):
            if f is None:
                parts.append("%s missing" % name)
                if end - t0 > limit:
                    status = "MISSING"
            else:
                delay = max(0.0, f["t"] - t0)
                parts.append("%s %s (+%ds)" % (name, fmt(f["t"]), delay))
                if delay > limit and status == "OK":
                    status = "LATE"
        if status == "OK" and any("missing" in p for p in parts):
            status = "open"
        add("M5 pad + fwd barracks", status, ", ".join(parts))

    # R1 core idle
    idle = []
    for e in ev:
        # A finished building later lost repeats its max_idle: count it once (at "finished").
        counted = e["event"] == "finished" or (e["event"] == "lost" and e.get("built") == "0")
        if counted and e.get("kind") in CORE and num(e.get("max_idle")) > lim["core_idle_max"]:
            idle.append("%s#%s %ss" % (e["kind"], e.get("ent"), e.get("max_idle")))
    if st:
        last = [s for s in st if s["t"] == st[-1]["t"]][-1]
        for r in last["rawlist"]:
            if r["kind"] in CORE and r["idle"] > lim["core_idle_max"]:
                idle.append("%s#%d %ds (still raw)" % (r["kind"], r["ent"], r["idle"]))
    add("R1 core idle", "VIOLATION" if idle else "OK", ", ".join(idle) if idle else "max %ds" % max(
        [num(e.get("max_idle")) for e in ev if e["event"] in ("finished", "lost") and e.get("kind") in CORE] or [0]))

    # R2 order accepted: "accepted" (an engineer took the order); older logs only have "build-start".
    waits, all_waits = [], []
    for p in [e for e in ev if e["event"] == "placed" and e.get("initial") != "1"]:
        reuse = first(ev, lambda e: e["event"] == "placed" and e.get("ent") == p.get("ent") and e["t"] > p["t"])
        until = reuse["t"] if reuse is not None else float("inf")

        def mine(e, name):
            return e["event"] == name and e.get("ent") == p.get("ent") and p["t"] <= e["t"] < until

        took = first(ev, lambda e: mine(e, "accepted")) or first(ev, lambda e: mine(e, "build-start"))
        label = "%s#%s" % (p.get("kind"), p.get("ent"))
        if took is not None:
            wait = took["t"] - p["t"]
            all_waits.append(wait)
            if wait > lim["order_accept_max"]:
                waits.append("%s %ds" % (label, wait))
            continue
        lost = first(ev, lambda e: mine(e, "lost"))
        if lost is not None:
            if lost.get("started") == "0" and lost.get("accepted", "0") == "0" and num(lost.get("age")) > lim["order_accept_max"]:
                waits.append("%s never (lost after %ss)" % (label, lost.get("age")))
        elif end - p["t"] > lim["order_accept_max"]:
            waits.append("%s never (%ds at end)" % (label, end - p["t"]))
    add("R2 order accepted", "VIOLATION" if waits else "OK",
        ", ".join(waits) if waits else "max %ds" % max(all_waits or [0]))

    # R3 no living vehicle / R4 hoarding
    def no_vehicle(s):
        return s["built"].get("vf", 0) > 0 and s["res"] >= lim["min_vehicle_res"] and s["vehicles"] == 0

    def hoarding(s):
        return s["res"] > lim["hoard_res"] and (s.get("research") in ("idle", "paused", "none") or
                                                (s["built"].get("vf", 0) > 0 and s["vehicles"] == 0))

    for name, cond, key in (("R3 vehicle present", no_vehicle, "no_vehicle_max"), ("R4 no hoarding", hoarding, "hoard_max")):
        run = runs(st, cond)
        if run is None:
            add(name, "OK", "never")
        else:
            span = run[1] - run[0]
            add(name, "VIOLATION" if span > lim[key] else "OK", "longest %ds (%s-%s)" % (span, fmt(run[0]), fmt(run[1])))

    # R5 vehicles reach the front
    bought = [e for e in ev if e["event"] == "vehicle-bought"]
    ok = late = lost = pending = 0
    lates = []
    for b in bought:
        front = first(ev, lambda e: e["event"] == "vehicle-front" and e.get("ent") == b.get("ent") and e["t"] >= b["t"])
        gone = first(ev, lambda e: e["event"] == "vehicle-lost" and e.get("ent") == b.get("ent") and e["t"] >= b["t"])
        if front is not None and (gone is None or front["t"] <= gone["t"]):
            after = num(front.get("after"))
            if after <= lim["vehicle_front_max"]:
                ok += 1
            else:
                late += 1
                lates.append("#%s %ds" % (b.get("ent"), after))
        elif gone is not None and num(gone.get("age")) <= lim["vehicle_front_max"]:
            lost += 1
        elif (gone["t"] if gone is not None else end) - b["t"] > lim["vehicle_front_max"]:
            late += 1
            lates.append("#%s never" % b.get("ent"))
        else:
            pending += 1
    text = "%d bought: %d in time, %d late, %d lost before front, %d open" % (len(bought), ok, late, lost, pending)
    if lates:
        text += " (" + ", ".join(lates[:6]) + (", ..." if len(lates) > 6 else "") + ")"
    add("R5 vehicles to front", "VIOLATION" if late else ("OK" if bought else "n/a"), text)
    return out


def report(events, states, limits):
    lines = []
    if not events and not states:
        return ["== Match flow: no match-flow lines (build without telemetry or rcbot_empires_debug_log 0)"]
    end = max([e["t"] for e in events] + [s["t"] for s in states])
    lines.append("== Match flow (MATCH_FLOW.md, measurement only, match end t=%s)" % fmt(end))
    if any(e["event"] == "research-vf" and e.get("instant") == "1" for e in events):
        lines.append("note: research instant (emp_sv_research_complete 1): M2 counts from the first finished radar")
    for team, name in TEAMS.items():
        rows = check_team(team, events, states, limits, end)
        bad = sum(1 for r in rows if r[1] in ("LATE", "MISSING", "VIOLATION"))
        last = [s for s in states if s["team"] == team]
        tail = ""
        if last:
            s = last[-1]
            tail = " | end: tickets %s res %d vehicles %d phase %s" % (s.get("tickets"), s["res"], s["vehicles"],
                                                                       s.get("phase"))
        lines.append("%s (team %d): %s%s" % (name, team, "OK" if bad == 0 else "%d deviation(s)" % bad, tail))
        for rule, status, detail in rows:
            lines.append("  %-22s %-9s %s" % (rule, status, detail))
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log")
    parser.add_argument("--flow", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "MATCH_FLOW.md"))
    args = parser.parse_args(argv)
    try:
        with open(args.flow, encoding="utf-8") as f:
            limits = parse_limits(f.read())
        with open(args.log, encoding="utf-8", errors="replace") as f:
            levels = split_levels(f)
    except (OSError, ValueError) as error:
        print("match_flow: %s" % error, file=sys.stderr)
        return 1
    if len(levels) > 1:
        print("note: %d map loads in this log (event=start), each checked on its own" % len(levels))
    for number, lines in enumerate(levels, 1):
        events, states = parse_log(lines)
        if len(levels) > 1:
            print("-- map load %d of %d" % (number, len(levels)))
        for line in game_end_notes(lines):
            print(line)
        print("\n".join(report(events, states, limits)))
    return 0


def game_end_notes(lines):
    """Names why a map load ended early: a lost CV (game over) or a server map change."""
    notes = []
    for line in lines:
        if "[RCBOT2] match-flow event=cv-lost " in line:
            fields = dict(f.split("=", 1) for f in line.split("event=cv-lost ", 1)[1].split() if "=" in f)
            team = {"2": "NF", "3": "BE"}.get(fields.get("team"), fields.get("team"))
            t = int(float(fields.get("t", "0")))
            notes.append("note: %s CV lost at %d:%02d (water=%s) -> game over" % (team, t // 60, t % 60, fields.get("water", "?")))
    stats = any(line.startswith("Unable to update stats") for line in lines)
    change = any(line.startswith("CHANGE LEVEL:") for line in lines)
    if change and not notes:
        notes.append("note: map changed (%s)" % ("game over: stats upload seen" if stats else "no game-over marker"))
    return notes


def split_levels(lines):
    """Splits a log at each match-flow event=start (a map change restarts the clock)."""
    levels = [[]]
    for line in lines:
        if "[RCBOT2] match-flow event=start " in line and any("match-flow" in x for x in levels[-1]):
            levels.append([])
        levels[-1].append(line)
    return levels


if __name__ == "__main__":
    sys.exit(main())
