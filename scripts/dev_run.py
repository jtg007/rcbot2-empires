#!/usr/bin/env python3
"""Run one bot match on the isolated test server and summarize it.

Drives the server console through a PTY (srcds reads stdin), so no RCON setup
is needed. `--summarize RUN_DIR SESSION_DIR` re-runs only the evaluation.
"""
import argparse
import collections
import glob
import json
import math
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import threading
import time

SERVER_PORT = os.environ.get("DEV_PORT", "49925")  # parallel runs: own port each
CLASSES = (1, 0, 2, 3)  # rifleman, scout, grenadier, engineer
TEAMS = ((2, "NF"), (3, "BE"))
ADD_BOT_GAP_S = 6.0
HANG_MIN_S = 20.0
HANG_RADIUS = 100.0
CRASH_RE = re.compile(
    r"segmentation fault|core dumped|double free|signal 11|\baborted\b|corrupted (size|double)|"
    r"malloc\(\)|free\(\): |watchdog|host_error", re.I)
NO_RECORDER = bool(os.environ.get("DEV_NO_RECORDER"))
# Fast iteration (all optional):
#   DEV_TIMESCALE=4      host_timescale; minutes and waits count game time
#   DEV_ROSTER="2:1 2:3" team:class per bot instead of the alternating default
#   DEV_SRCDS_ARGS="-nowatchdog"   extra srcds_run arguments (navmesh generation on big maps)
#   DEV_CMDS="cmd;wait 5;waitfor 120 some regex;cmd"   waitfor = up to 120 game seconds
#   "mark" in DEV_CMDS: later waitfor only match lines logged after it (repeated results)
#   DEV_UNTIL="regex"    end the run as soon as a log line matches
#   DEV_EXPECT="regex"   require a matching server log line for success
#   DEV_FAIL="regex"     abort on a matching server log line
#   DEV_SHOW="regex"     print matching log lines at the end (repeats collapsed)
TIMESCALE = max(float(os.environ.get("DEV_TIMESCALE", "1")), 0.1)
ROSTER = os.environ.get("DEV_ROSTER", "").split()
UNTIL = os.environ.get("DEV_UNTIL")
EXPECT = os.environ.get("DEV_EXPECT")
FAIL = os.environ.get("DEV_FAIL")
# One game minute after the bots are in, check that the run is worth waiting for
# (runs with 6+ bots; DEV_NO_CHECK=1 skips it).
CHECK_AFTER_S = 60.0
CHECK_MIN_BOTS = 6
NO_CHECK = bool(os.environ.get("DEV_NO_CHECK"))
SHOW = os.environ.get("DEV_SHOW")
# DEV_MANUAL=1: a person watches. SourceTV records a demo into the run folder,
# no health check, the run ends only after that person disconnected
# (waits DEV_MANUAL_JOIN_MIN minutes for the join, at most DEV_MANUAL_MAX_MIN in total).
MANUAL = os.environ.get("DEV_MANUAL") == "1"
MANUAL_JOIN_MIN = float(os.environ.get("DEV_MANUAL_JOIN_MIN", "30"))
MANUAL_MAX_MIN = float(os.environ.get("DEV_MANUAL_MAX_MIN", "240"))
HUMAN_CONNECT_RE = re.compile(r'"(.+?)<(\d+)><((?!BOT>|HLTV>)[^>]*)><[^>]*>" connected, address')
DUMP_DIR = "/tmp/dumps"


class ScenarioFailed(Exception):
    pass


class Server:
    def __init__(self, args, log_path):
        master, slave = pty.openpty()
        attrs = termios.tcgetattr(slave)
        attrs[3] &= ~termios.ECHO
        termios.tcsetattr(slave, termios.TCSANOW, attrs)
        self.master = master
        self.lines = []
        self.lock = threading.Lock()
        source_tv = os.environ.get("DEV_SOURCETV") == "1" or MANUAL
        self.source_tv = source_tv
        self.proc = subprocess.Popen(
            ["./srcds_run", "-norestart", "-console", "-game", "empires",
             "-insecure", "-nomaster", *([] if source_tv else ["-nohltv"]), "-nojoy",
             "-ip", args.ip, "-port", SERVER_PORT, *os.environ.get("DEV_SRCDS_ARGS", "").split(),
             "+sv_lan", "1", "+rcbot_empires_debug_log", "1", "+tv_enable", "1" if source_tv else "0", "+map", args.map],
            cwd=args.server_root, env=dict(os.environ, SteamAppId="17740", SteamGameId="17740"),
            stdin=slave, stdout=slave, stderr=slave, start_new_session=True, close_fds=True)
        os.close(slave)
        self.log = open(log_path, "w", errors="replace")
        self.reader_stop = threading.Event()
        self.reader = threading.Thread(target=self._reader, daemon=True)
        self.reader.start()

    def _reader(self):
        buffer = b""
        while True:
            try:
                if not select.select([self.master], [], [], 0.2)[0]:
                    if self.reader_stop.is_set():
                        return
                    continue
                chunk = os.read(self.master, 65536)
            except OSError:
                return
            if not chunk:
                return
            buffer += chunk
            *complete, buffer = buffer.split(b"\n")
            for raw in complete:
                line = raw.decode("utf-8", "replace").rstrip("\r")
                self.log.write(line + "\n")
                with self.lock:
                    self.lines.append(line)
            self.log.flush()

    def send(self, command):
        os.write(self.master, (command + "\n").encode())

    def alive(self):
        return self.proc.poll() is None

    def mark(self):
        with self.lock:
            return len(self.lines)

    def find(self, pattern, start=0):
        return self.scan(re.compile(pattern), start)[0]

    def scan(self, regex, start):
        """Return a match and the next unread offset, or the end if no line matched."""
        with self.lock:
            end = len(self.lines)
            for index in range(start, end):
                match = regex.search(self.lines[index])
                if match:
                    return match, index + 1
        return None, end

    def wait_for(self, pattern, timeout, start=0, resend=None, check_failure=None):
        regex = re.compile(pattern) if pattern is not None else None
        deadline = time.monotonic() + timeout
        next_send = 0.0
        while True:
            if check_failure:
                check_failure()
            if regex is not None:
                match, start = self.scan(regex, start)
                if match:
                    return match
            now = time.monotonic()
            if now >= deadline or not self.alive():
                return None
            if resend and now >= next_send:
                self.send(resend)
                next_send = now + 2.0
            time.sleep(min(0.2, deadline - now))


def shutdown(server):
    try:
        server.send("quit")
    except OSError:
        pass
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline and server.alive():
        time.sleep(0.5)
    if server.alive():
        os.killpg(server.proc.pid, signal.SIGTERM)
        try:
            server.proc.wait(15)
        except subprocess.TimeoutExpired:
            os.killpg(server.proc.pid, signal.SIGKILL)
            server.proc.wait()


def show_lines(lines, pattern):
    regex = re.compile(pattern)
    previous, repeats = None, 0

    def flush():
        if previous is not None:
            print(previous[:400] + (f"   (x{repeats})" if repeats > 1 else ""))

    for line in lines:
        if not regex.search(line):
            continue
        line = re.sub(r"\x1b\[[0-9;]*m", "", line)
        if line == previous:
            repeats += 1
            continue
        flush()
        previous, repeats = line, 1
    flush()


def health_check(server, args, session_dir):
    """Returns (problems, facts). Problems mean the run is not worth finishing."""
    problems, facts = [], []
    if not server.alive():
        return ["Server beendet"], facts
    with server.lock:
        lines = list(server.lines)
    crash = [line for line in lines if CRASH_RE.search(line)]
    if crash:
        problems.append("crash pattern in log: " + crash[0].strip()[:100])
    if os.path.exists(os.path.join(args.server_root, "empires", "maps", args.map + ".nav")):
        nav = [line for line in lines if "NavMesh load result=" in line]
        if not nav:
            problems.append("navmesh not loaded")
        elif "result=0" not in nav[-1]:
            problems.append("navmesh load error: " + nav[-1].strip()[:100])
    if not session_dir:
        return problems, facts

    snapshots = load_jsonl(os.path.join(session_dir, "snapshots.jsonl"))
    events = load_jsonl(os.path.join(session_dir, "events.jsonl"))
    if not snapshots:
        problems.append("recorder writes no snapshots")
        return problems, facts
    now = max(sample["session_time"] for sample in snapshots)
    window = collections.defaultdict(list)
    for sample in snapshots:
        if sample["session_time"] >= now - 45.0:
            window[sample["bot_id"]].append(sample)
    alive = [bot for bot, samples in window.items() if any(s.get("alive") for s in samples)]
    moved = 0
    for bot in alive:
        points = [s["position"] for s in window[bot] if s.get("alive")]
        walked = sum(math.dist(a[:2], b[:2]) for a, b in zip(points, points[1:]) if math.dist(a[:2], b[:2]) < 2000)
        moved += walked > 150.0
    counts = collections.Counter(event.get("event") for event in events)
    requests, failures = counts["PATH_REQUEST"], counts["PATH_FAILURE"]
    facts.append(f"alive {len(alive)}/{args.bots}, moving {moved}/{max(len(alive), 1)}, "
                 f"path requests {requests}, path failures {failures}")
    if len(alive) < max(1, int(args.bots * 0.7)):
        problems.append(f"only {len(alive)} of {args.bots} bots alive")
    elif moved < max(1, int(len(alive) * 0.4)):
        problems.append(f"only {moved} of {len(alive)} living bots are moving")
    if requests >= 10 and failures > 0.7 * requests:
        problems.append(f"{failures} of {requests} path requests fail")
    return problems, facts


def wait_for_human(server, args, demos):
    """Manual run: wait for a real player, then until that player disconnects.
    A new map (round end) stops the SourceTV demo: record the next one."""
    print(f"Waiting for a player (up to {MANUAL_JOIN_MIN:g} min): connect <server-ip>:{SERVER_PORT}", flush=True)
    end = time.monotonic() + MANUAL_MAX_MIN * 60
    match = server.wait_for(HUMAN_CONNECT_RE.pattern, MANUAL_JOIN_MIN * 60)
    if not match:
        print("No player connected, run ends.", flush=True)
        return
    name, userid, steam = match.group(1), match.group(2), match.group(3)
    print(f"Player connected: {name} (userid {userid}). Server runs until they disconnect.", flush=True)
    gone = re.escape(f"<{userid}><{steam}>") + r'<[^>]*>" disconnected'
    map_mark = server.mark()
    while server.alive() and time.monotonic() < end:
        if server.find(gone):
            print(f"Player {name} disconnected, run ends.", flush=True)
            return
        started = server.find(r'Started map "([^"]+)"', map_mark)
        if started and demos:
            map_mark = server.mark()
            print(f"New map: {started.group(1)}", flush=True)
            if server.wait_for(r"SourceTV broadcast active", 120, map_mark):
                demo = f"{demos[0]}-{len(demos) + 1}"
                server.send("tv_record " + demo)
                demos.append(demo)
        time.sleep(1.0)
    if server.alive():
        print(f"Maximum duration {MANUAL_MAX_MIN:g} min reached, run ends.", flush=True)


def stop_demo(server, args, demos):
    mark = server.mark()
    server.send("tv_stoprecord")
    server.wait_for(r"Completed SourceTV demo", 30, mark)
    for demo in demos:
        path = os.path.join(args.server_root, "empires", demo + ".dem")
        if os.path.exists(path):
            target = os.path.join(args.run_dir, demo + ".dem")
            os.replace(path, target)
            print(f"Demo: {target}", flush=True)


def dump_files():
    return set(glob.glob(os.path.join(DUMP_DIR, "*.dmp")))


def is_unrelated_dump(path):
    # Run 211230 picked up a desktop Steam crash from the shared dump directory.
    # Linux Breakpad stream 0x47670006 contains NUL-separated /proc/self/cmdline.
    try:
        with open(path, "rb") as dump:
            magic, _, count, directory = struct.unpack("<4sIII", dump.read(16))
            if magic != b"MDMP" or count > 256:
                return False
            dump.seek(directory)
            for _ in range(count):
                kind, size, offset = struct.unpack("<III", dump.read(12))
                if kind == 0x47670006:
                    dump.seek(offset)
                    data = dump.read(min(size, 65536))
                    command, end, arguments = data.partition(b"\0")
                    executable = os.path.basename(command)
                    if end and executable == b"steamwebhelper":
                        return True
                    if executable != b"srcds_linux" or len(data) != size or not data.endswith(b"\0"):
                        return False
                    args = arguments.split(b"\0")
                    if args.count(b"-port") != 1:
                        return False
                    port = args[args.index(b"-port") + 1]
                    own_port = SERVER_PORT.encode()
                    return (port.isdigit() and own_port.isdigit() and
                            0 < int(port) <= 65535 and 0 < int(own_port) <= 65535 and
                            int(port) != int(own_port))
    except (OSError, ValueError, struct.error):
        pass
    return False


def run_scenario(server, args, session_dir, mark, started):
    def stamp(text):
        print(f"  +{(time.monotonic() - started) * TIMESCALE:6.1f}s game time  {text}", flush=True)

    fail_pattern = re.compile(FAIL) if FAIL else None
    fail_cursor = mark

    def check_failure():
        nonlocal fail_cursor
        if fail_pattern:
            match, fail_cursor = server.scan(fail_pattern, fail_cursor)
            if match:
                raise ScenarioFailed(match.string)

    deadline = time.monotonic() + args.minutes * 60 / TIMESCALE
    for part in os.environ.get("DEV_CMDS", "").split(";"):
        # A waitfor pattern keeps its trailing blank: "userid=3 " must not match userid=31.
        item = part.lstrip()
        if not item.startswith("waitfor "):
            item = item.rstrip()
        if not item.strip():
            continue
        check_failure()
        if not server.alive():
            return mark
        if item.startswith("waitfor "):
            _, seconds, pattern = item.split(None, 2)
            if not server.wait_for(pattern, float(seconds) / TIMESCALE, mark, check_failure=check_failure):
                stamp("NOT found: " + pattern)
                raise ScenarioFailed("missing prerequisite: " + pattern)
            stamp("found: " + pattern)
        elif item == "mark":
            mark = server.mark()
        elif item.startswith("wait "):
            server.wait_for(None, float(item.split()[1]) / TIMESCALE, check_failure=check_failure)
        else:
            server.send(item)
            stamp(item)

    if MANUAL:
        return mark
    print(f"Bots added, running for up to {args.minutes:g} game minutes ...", flush=True)
    check_at = None if NO_CHECK or args.bots < CHECK_MIN_BOTS else time.monotonic() + CHECK_AFTER_S / TIMESCALE
    until_pattern = re.compile(UNTIL) if UNTIL else None
    until_cursor = mark
    while server.alive():
        check_failure()
        now = time.monotonic()
        if check_at is not None and now >= check_at:
            check_at = None
            problems, facts = health_check(server, args, session_dir)
            if problems:
                raise ScenarioFailed("health check after 60 s: " + "; ".join(problems))
            print("Health check after 60 s: OK (" + "; ".join(facts) + ")", flush=True)
        if until_pattern:
            match, until_cursor = server.scan(until_pattern, until_cursor)
            if match:
                stamp("Ende: " + UNTIL)
                break
        if now >= deadline:
            break
        time.sleep(0.5)
    return mark


def run_match(args):
    dumps_before = dump_files()
    server = Server(args, os.path.join(args.run_dir, "server.log"))
    crash_note = None
    session_dir = None
    failure = None
    try:
        if not server.wait_for(r"^DEVSH_READY\s*$", 300, resend="echo DEVSH_READY"):
            raise SystemExit("Server did not become ready, see server.log")
        mark = server.mark()
        server.send("meta list")
        if not server.wait_for(r"RCBot2", 15, mark):
            raise SystemExit("RCBot2 not loaded, see server.log")
        if args.deploy_lock_fd is not None:
            os.close(args.deploy_lock_fd)

        # Diagnostic [RCBOT2] lines are off by default (public servers); every check here reads them.
        server.send("rcbot_empires_debug_log 1")
        server.send("rcbotd config min_bots -1")
        server.send("rcbotd config max_bots -1")
        if not NO_RECORDER:
            mark = server.mark()
            server.send("rcbot_analysis_start")
            match = server.wait_for(r"path=(analysis/session-\S+)", 20, mark)
            if not match:
                raise SystemExit("Recorder start not confirmed, see server.log")
            session_dir = os.path.join(args.server_root, "empires", match.group(1))
            print(f"Recorder: {session_dir}", flush=True)

        demos = []
        if MANUAL:
            mark = server.mark()
            demo = "manual-" + os.path.basename(args.run_dir.rstrip("/"))[:15] + "-p" + SERVER_PORT
            server.send("tv_record " + demo)
            if server.wait_for(r"Recording SourceTV demo", 20, mark):
                demos.append(demo)
            else:
                print("WARNING: SourceTV recording not confirmed", flush=True)

        started = time.monotonic()
        if TIMESCALE != 1.0:
            server.send("sv_cheats 1")
            server.send(f"host_timescale {TIMESCALE:g}")
        if ROSTER:
            roster = [(int(team), int(cls)) for team, cls in (item.split(":") for item in ROSTER)]
        else:
            roster = [(TEAMS[i % 2][0], CLASSES[(i // 2) % len(CLASSES)]) for i in range(args.bots)]
        names = dict(TEAMS)
        # Mark before DEV_PRE/bots: waitfor in DEV_CMDS also sees lines logged
        # while the bots join (e.g. a Commander placing its first building).
        mark = server.mark()
        # DEV_PRE: console commands before the bots join (e.g. "rcbot_empires_auto_commander 0").
        for command in filter(None, (c.strip() for c in os.environ.get("DEV_PRE", "").split(";"))):
            server.send(command)
        for index, (team, bot_class) in enumerate(roster):
            server.send(f"rcbotd addbot {bot_class} {team} {names[team]}_{index // 2 + 1:02d}")
            time.sleep(max(ADD_BOT_GAP_S / TIMESCALE, 2.0))

        try:
            mark = run_scenario(server, args, session_dir, mark, started)
            if MANUAL and server.alive():
                wait_for_human(server, args, demos)
            if EXPECT and not server.find(EXPECT, mark):
                failure = "missing expected event: " + EXPECT
        except ScenarioFailed as exc:
            failure = str(exc)
        if demos and server.alive():
            stop_demo(server, args, demos)
        if session_dir and server.alive():
            server.send("rcbot_analysis_stop")
            summary = os.path.join(session_dir, "summary.json")
            end = time.monotonic() + 60
            while time.monotonic() < end and server.alive() and not os.path.exists(summary):
                time.sleep(0.5)
            time.sleep(1)
    finally:
        if server.alive():
            shutdown(server)
        else:
            crash_note = f"Server endete vorzeitig (Exit {server.proc.returncode})"
        server.reader_stop.set()
        server.reader.join()
        os.close(server.master)
        server.log.close()
    if SHOW:
        show_lines(server.lines, SHOW)

    crash_lines = []
    with open(os.path.join(args.run_dir, "server.log"), errors="replace") as log:
        for number, line in enumerate(log, 1):
            if CRASH_RE.search(line):
                crash_lines.append(f"{number}: {line.strip()[:160]}")
    new_dumps = sorted(path for path in dump_files() - dumps_before if not is_unrelated_dump(path))
    return session_dir, crash_note, crash_lines, new_dumps, failure


def load_jsonl(path):
    if not os.path.exists(path):
        return []
    records = []
    with open(path, errors="replace") as handle:
        for line in handle:
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return records


def hangs_for(samples):
    """Episodes of ≥HANG_MIN_S alive, within HANG_RADIUS, with no enemy targeted."""
    hangs = []
    anchor = None

    def close(end_time):
        nonlocal anchor
        if anchor and end_time - anchor["start"] >= HANG_MIN_S:
            wanted = anchor["wants"] / max(anchor["count"], 1) >= 0.5  # had a task, so it meant to act
            hangs.append({"seconds": end_time - anchor["start"], "wanted": wanted,
                          "position": anchor["pos"], "task": anchor["task"]})
        anchor = None

    for sample in samples:
        now = sample["session_time"]
        if not sample.get("alive") or sample.get("enemy_index", -1) >= 0:
            close(now)
            continue
        pos = sample["position"]
        if anchor is None:
            anchor = {"start": now, "pos": pos, "count": 0, "wants": 0, "task": sample.get("task", "")}
        elif math.dist(pos, anchor["pos"]) > HANG_RADIUS:
            close(now)
            anchor = {"start": now, "pos": pos, "count": 0, "wants": 0, "task": sample.get("task", "")}
        anchor["count"] += 1
        anchor["wants"] += 1 if sample.get("task") else 0
    if samples:
        close(samples[-1]["session_time"])
    return hangs


def summarize(session_dir, run_dir, crash_note=None, crash_lines=(), new_dumps=()):
    snapshots = load_jsonl(os.path.join(session_dir, "snapshots.jsonl"))
    events = load_jsonl(os.path.join(session_dir, "events.jsonl"))

    bots = collections.defaultdict(lambda: {
        "name": "?", "team": "?", "class": "?", "samples": [], "stuck": 0,
        "kills": 0, "suicides": 0, "teamkills": 0, "deaths": 0, "path_fail": 0})
    for sample in snapshots:
        bot = bots[sample["bot_id"]]
        bot["samples"].append(sample)
        bot["name"] = sample.get("name", bot["name"])
        if sample.get("team_name") not in (None, "unknown"):
            bot["team"] = sample["team_name"]
        bot["class"] = sample.get("class", bot["class"])

    counts = collections.Counter(event.get("event") for event in events)
    capture_points = set()
    providers = collections.Counter()
    for event in events:
        kind = event.get("event")
        if kind == "CAPTURE_POINT_STATE":
            capture_points.add(json.dumps(event.get("data", {}).get("entity_index", event.get("entity_index"))))
        if kind == "NAV_PROVIDER_SELECT":
            provider = re.search(r"provider=(\w+)", str((event.get("data") or {}).get("detail")))
            if provider:
                providers[provider.group(1)] += 1
        bot_id = event.get("bot_id")
        if bot_id is None:
            continue
        bot = bots[bot_id]
        if kind == "STUCK_DETECTED":
            bot["stuck"] += 1
        elif kind == "BOT_DEATH":
            bot["deaths"] += 1
        elif kind == "PATH_FAILURE":
            bot["path_fail"] += 1
        elif kind == "KILL":
            data = event.get("data") or {}
            target_team = (data.get("target") or {}).get("team")
            if data.get("detail") == "suicide":
                bot["suicides"] += 1
            elif target_team is not None and target_team == event.get("team") and target_team != 0:
                bot["teamkills"] += 1
            else:
                bot["kills"] += 1

    out = []
    header = (f'{"Bot":<9}{"Team":<5}{"Class":<11}{"alive s":>7}{"moved":>8}{"STUCK":>7}'
              f'{"Stuck":>8}{"Stuck s":>10}{"Idle s":>10}{"Kills":>7}{"Deaths":>7}{"Suic.":>6}{"TK":>4}')
    out.append(header)
    out.append("-" * len(header))
    totals = collections.Counter()
    worst = []
    for bot_id in sorted(bots):
        bot = bots[bot_id]
        samples = bot["samples"]
        if not samples and not (bot["kills"] or bot["deaths"] or bot["stuck"]):
            continue
        alive_time = 0.0
        travelled = 0.0
        for prev, cur in zip(samples, samples[1:]):
            if prev.get("alive") and cur.get("alive"):
                delta_t = cur["session_time"] - prev["session_time"]
                step = math.dist(prev["position"][:2], cur["position"][:2])
                if delta_t < 10:
                    alive_time += delta_t
                if step < 2000:
                    travelled += step
        hangs = hangs_for(samples)
        wanted = [h for h in hangs if h["wanted"]]
        hang_s = sum(h["seconds"] for h in wanted)
        idle_s = sum(h["seconds"] for h in hangs if not h["wanted"])
        for hang in wanted:
            worst.append((hang["seconds"], f"#{bot_id}", hang["position"], hang["task"]))
        out.append(f'{"#" + str(bot_id):<9}{bot["team"]:<5}{bot["class"][:10]:<11}{alive_time:>7.0f}{travelled:>8.0f}'
                   f'{bot["stuck"]:>7}{len(wanted):>8}{hang_s:>10.0f}{idle_s:>10.0f}'
                   f'{bot["kills"]:>7}{bot["deaths"]:>7}{bot["suicides"]:>6}{bot["teamkills"]:>4}')
        totals.update(alive=alive_time, travelled=travelled, stuck=bot["stuck"], hangs=len(wanted),
                      hang_s=hang_s, idle_s=idle_s, kills=bot["kills"], deaths=bot["deaths"],
                      suicides=bot["suicides"], teamkills=bot["teamkills"], path_fail=bot["path_fail"])
    out.append("-" * len(header))
    out.append(f'{"TOTAL":<25}{totals["alive"]:>7.0f}{totals["travelled"]:>8.0f}{totals["stuck"]:>7}'
               f'{totals["hangs"]:>8}{totals["hang_s"]:>10.0f}{totals["idle_s"]:>10.0f}'
               f'{totals["kills"]:>7}{totals["deaths"]:>7}{totals["suicides"]:>6}{totals["teamkills"]:>4}')
    out.append("")

    share = 100.0 * totals["hang_s"] / totals["alive"] if totals["alive"] else 0.0
    out.append(f"Stuck share of lifetime: {share:.1f} %")
    out.append(f'Path failures: {totals["path_fail"]}  (requests: {counts["PATH_REQUEST"]}, successes: {counts["PATH_SUCCESS"]})')
    out.append("Navigator: " + (", ".join(f"{name} {count}" for name, count in providers.most_common()) or "-"))
    out.append(f'Captures: {counts["CAPTURE_COMPLETED"]}  started: {counts["CAPTURE_STARTED"]}  '
               f'points in recorder: {len(capture_points)}')
    if not capture_points:
        out.append("  Note: the recorder saw no capture points, the map probably has no flags.")

    if worst:
        out.append("")
        out.append("Longest stuck periods:")
        for seconds, name, pos, task in sorted(worst, key=lambda item: -item[0])[:5]:
            task_name = (task or "").split("\n")[0] or "-"
            out.append(f'  {seconds:5.0f} s  {name:<9} at ({pos[0]:.0f} {pos[1]:.0f} {pos[2]:.0f})  Task: {task_name}')

    out.append("")
    crashed = bool(crash_note or crash_lines or new_dumps)
    out.append(f'Crashes: {"YES" if crashed else "none"}')
    if crash_note:
        out.append(f"  {crash_note}")
    for line in list(crash_lines)[:5]:
        out.append(f"  server.log {line}")
    for dump in new_dumps:
        out.append(f"  Dump: {dump}")
    out.append(f"Session: {session_dir}")

    text = "\n".join(out)
    print(text)
    with open(os.path.join(run_dir, "summary.txt"), "w") as handle:
        handle.write(text + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--map")
    parser.add_argument("--bots", type=int)
    parser.add_argument("--minutes", type=float)
    parser.add_argument("--server-root")
    parser.add_argument("--ip")
    parser.add_argument("--run-dir")
    parser.add_argument("--deploy-lock-fd", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--summarize", nargs=2, metavar=("RUN_DIR", "SESSION_DIR"))
    args = parser.parse_args()
    if args.summarize:
        summarize(args.summarize[1], args.summarize[0])
        return
    session_dir, crash_note, crash_lines, new_dumps, failure = run_match(args)
    if not session_dir:
        print("No recorder. Crashes:", "YES" if crash_note or crash_lines or new_dumps else "none")
        for detail in ([crash_note] if crash_note else []) + list(crash_lines)[:5] + list(new_dumps):
            print(" ", detail)
    else:
        summarize(session_dir, args.run_dir, crash_note, crash_lines, new_dumps)
    if failure or crash_note or crash_lines or new_dumps:
        print("Run result: FAIL (" + (failure or "server crash") + ")")
        return 1
    print("Run result: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
