#!/usr/bin/env python3
"""Offline test for scripts/match_flow.py (run by scripts/run_unit_tests.sh)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import match_flow  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def state(t, team, res, research="done", vf=0, vehicles=0, rawlist="-"):
    return ("L 11/25/2026 - 12:00:00: [RCBOT2] match-flow state t=%d team=%d tickets=300 res=%d research=%s "
            "built=ref:1,radar:1,vf:%d,bar:1,arm:0,pad:0 raw=ref:0,radar:0,vf:0,bar:0,arm:0,pad:0 rawlist=%s "
            "vehicles=%d phase=PUSH" % (t, team, res, research, vf, rawlist, vehicles))


def ev(text):
    return "[RCBOT2] match-flow event=" + text


GOOD = [
    ev("start t=0 curtime=3"),
    ev("research-vf t=0 team=2 instant=1"),
    ev("placed t=5 team=2 kind=barracks ent=50 hp=1000/1000 initial=1 built=1"),
    ev("placed t=30 team=2 kind=refinery ent=60 hp=50/500 initial=0 built=0"),
    ev("build-start t=40 team=2 kind=refinery ent=60 wait=10"),
    ev("finished t=80 team=2 kind=refinery ent=60 age=50 max_idle=5"),
    ev("placed t=60 team=2 kind=radar ent=61 hp=50/500 initial=0 built=0"),
    ev("build-start t=65 team=2 kind=radar ent=61 wait=5"),
    ev("finished t=110 team=2 kind=radar ent=61 age=50 max_idle=4"),
    ev("placed t=150 team=2 kind=factory ent=62 hp=50/350 initial=0 built=0"),
    ev("build-start t=160 team=2 kind=factory ent=62 wait=10"),
    ev("finished t=230 team=2 kind=factory ent=62 age=80 max_idle=10"),
    ev("vehicle-bought t=260 team=2 ent=70 chassis=3"),
    ev("vehicle-out t=280 team=2 ent=70 after=20"),
    ev("vehicle-front t=330 team=2 ent=70 after=70 reason=near-enemy"),
    ev("placed t=250 team=2 kind=repair ent=63 hp=50/300 initial=0 built=0"),
    ev("build-start t=255 team=2 kind=repair ent=63 wait=5"),
    ev("finished t=300 team=2 kind=repair ent=63 age=50 max_idle=3"),
    ev("placed t=260 team=2 kind=barracks ent=64 hp=50/1000 initial=0 built=0"),
    ev("build-start t=270 team=2 kind=barracks ent=64 wait=10"),
    ev("finished t=380 team=2 kind=barracks ent=64 age=120 max_idle=20"),
    state(240, 2, 400, vf=1),
    state(270, 2, 300, vf=1, vehicles=1),
    state(300, 2, 900, vf=1, vehicles=1),
    state(330, 2, 900, vf=1, vehicles=1),
]

BAD = [
    ev("research-vf t=0 team=3 instant=1"),
    ev("placed t=200 team=3 kind=refinery ent=80 hp=50/500 initial=0 built=0"),
    ev("build-start t=250 team=3 kind=refinery ent=80 wait=50"),
    ev("finished t=400 team=3 kind=refinery ent=80 age=200 max_idle=120"),
    ev("lost t=420 team=3 kind=refinery ent=80 built=1 started=1 age=220 max_idle=120"),
    ev("placed t=20 team=3 kind=radar ent=81 hp=50/500 initial=0 built=0"),
    ev("lost t=100 team=3 kind=radar ent=81 built=0 started=0 age=80 max_idle=80"),
    ev("vehicle-bought t=300 team=3 ent=90 chassis=3"),
    ev("placed t=300 team=3 kind=barracks ent=85 hp=50/250 initial=0 built=0"),
    ev("placed t=310 team=3 kind=repair ent=86 hp=50/200 initial=0 built=0"),
    ev("accepted t=320 team=3 kind=repair ent=86 wait=10"),
    state(300, 3, 1200, research="none"),
    state(330, 3, 1200, research="none"),
    state(360, 3, 1200, research="none"),
    state(390, 3, 1200, research="none"),
    state(420, 3, 1200, research="none"),
    state(450, 3, 1200, research="none", rawlist="barracks#85:150/150/0,repair#86:140/140/1"),
]


class MatchFlowTest(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(ROOT, "MATCH_FLOW.md"), encoding="utf-8") as f:
            self.limits = match_flow.parse_limits(f.read())
        events, states = match_flow.parse_log(GOOD + BAD)
        self.end = max([e["t"] for e in events] + [s["t"] for s in states])
        self.nf = {r[0]: r for r in match_flow.check_team(2, events, states, self.limits, self.end)}
        self.be = {r[0]: r for r in match_flow.check_team(3, events, states, self.limits, self.end)}

    def test_limits_from_doc(self):
        self.assertEqual(self.limits["vf_build"], 90)
        self.assertEqual(self.limits["hoard_res"], 800)

    def test_good_team_all_ok(self):
        for name, status, detail in self.nf.values():
            self.assertEqual(status, "OK", "%s: %s" % (name, detail))
        self.assertIn("radar 1:50", self.nf["M2 factory placed"][2])  # anchored to the radar with instant research

    def test_bad_team(self):
        self.assertEqual(self.be["M1 start economy"][1], "LATE")
        self.assertEqual(self.be["M2 factory placed"][1], "MISSING")  # radar from the state lines
        self.assertEqual(self.be["R1 core idle"][1], "VIOLATION")
        self.assertEqual(self.be["R1 core idle"][2].count("refinery#80"), 1)  # finished, then lost: once
        self.assertIn("barracks#85 150s (still raw)", self.be["R1 core idle"][2])
        self.assertEqual(self.be["R2 order accepted"][1], "VIOLATION")
        self.assertIn("refinery#80 50s", self.be["R2 order accepted"][2])
        self.assertIn("radar#81 never", self.be["R2 order accepted"][2])
        self.assertIn("barracks#85 never", self.be["R2 order accepted"][2])
        self.assertNotIn("repair#86", self.be["R2 order accepted"][2])  # accepted after 10 s, walk not counted
        self.assertEqual(self.be["R4 no hoarding"][1], "VIOLATION")
        self.assertEqual(self.be["R3 vehicle present"][1], "OK")
        self.assertEqual(self.be["R5 vehicles to front"][1], "VIOLATION")

    def test_split_levels(self):
        levels = match_flow.split_levels(GOOD + [ev("start t=0 curtime=12")] + BAD)
        self.assertEqual(len(levels), 2)
        self.assertEqual(len(levels[0]), len(GOOD))

    def test_report_without_lines(self):
        self.assertIn("no match-flow lines", match_flow.report([], [], self.limits)[0])

    def test_r4_saving_before_vf_and_separate_spans(self):
        # Saving with active research before VF is finished is normal (not hoarding).
        saving = [state(t, 2, 1500, research="active", vf=0, vehicles=0) for t in range(30, 300, 30)]
        events, states = match_flow.parse_log(saving)
        rows = {r[0]: r for r in match_flow.check_team(2, events, states, self.limits, 270)}
        self.assertEqual(rows["R4 no hoarding"][1], "OK")

        # Stopped research before VF (90 s) + missing vehicles right after VF (60 s) must not stitch into >120 s.
        split = [
            state(180, 2, 1500, research="idle", vf=0, vehicles=0),
            state(210, 2, 1500, research="idle", vf=0, vehicles=0),
            state(240, 2, 1500, research="idle", vf=0, vehicles=0),
            state(270, 2, 1500, research="idle", vf=0, vehicles=0),
            state(300, 2, 1500, research="active", vf=1, vehicles=0),
            state(330, 2, 1500, research="active", vf=1, vehicles=0),
            state(360, 2, 1500, research="active", vf=1, vehicles=0),
        ]
        events, states = match_flow.parse_log(split)
        rows = {r[0]: r for r in match_flow.check_team(2, events, states, self.limits, 360)}
        self.assertEqual(rows["R4 no hoarding"][1], "OK")

        # Missing vehicles after VF for >120 s IS a violation.
        after_vf = [state(t, 2, 1500, research="active", vf=1, vehicles=0) for t in range(300, 480, 30)]
        events, states = match_flow.parse_log(after_vf)
        rows = {r[0]: r for r in match_flow.check_team(2, events, states, self.limits, 450)}
        self.assertEqual(rows["R4 no hoarding"][1], "VIOLATION")
        self.assertIn("no vehicles after VF", rows["R4 no hoarding"][2])


if __name__ == "__main__":
    unittest.main()
