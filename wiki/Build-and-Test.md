# Build and test

Sources: [AGENTS.md](../AGENTS.md), [EMPIRES_ANALYSIS_RECORDER.md](../EMPIRES_ANALYSIS_RECORDER.md), header comments of
`scripts/*.sh` and `scripts/dev_run.py`, [README.md](../README.md) (Development), `.claude/agents/`. Refresh from the script headers when scripts change.

## Setup

1. Copy `scripts/dev.local.example` to `scripts/dev.local` (not in git) and set `EMPIRES_TEST_SERVER` to an **isolated** Empires dedicated server
   (Linux, SteamCMD) with Metamod:Source and this plugin installed once. Optional: `RCBOT_DEPS_DIR` (default sibling checkout `../rcbot2-clean`
   containing `alliedmodders/{hl2sdk-sdk2013,metamod-source,sourcemod}` and `hl2sdk-manifests/`).
2. Every map needs `empires/maps/<map>.nav`; see [Maps](Maps.md) and `scripts/dev_navgen.sh`.
3. Never build, deploy or test in a production server directory. Never kill processes by name or pattern (`pkill`, `killall srcds`); stop only PIDs
   you started. A game client and other runs may be active.
4. Test runs need write access to the test server and `~/.cache/rcbot2-dev-build`, a PTY, a netlink socket and local UDP ports from 49925
   (sandbox note). Without that, only edit and build.

## Build

| Command | Use |
|---|---|
| `scripts/dev_build.sh` | Incremental build (about 5 s) in a persistent mirror (`~/.cache/rcbot2-dev-build`); prints the `.so` path. Filter errors with `2>&1 \| grep error:`. |
| `SDK_ROOT=../rcbot2-clean/alliedmodders MANIFEST_DIR=../rcbot2-clean/hl2sdk-manifests scripts/build_empires.sh` | Clean from-scratch build of a disposable snapshot (SDK2013 x86, GCC, optimized). Applies tracked diff plus untracked sources, never touches the real index; writes `rcbot.2.sdk2013.so` and a `.build.json` provenance manifest (commit, plugin hash, compilers, ELF Build ID). Overrides: `AMBUILD_PYTHON`, `AMBUILD_BIN`. Linker warnings about `.rodata` relocations / `DT_TEXTREL` are expected. |
| `scripts/build_provenance.py` | Provenance helper of the clean build (own test `build_provenance_test.py`). |

CI (GitHub Actions `.github/workflows/build.yml`) builds Linux and Windows (MSVC x86) on every push; a push to `main` refreshes the `latest` release.
A new source file must be added to `AMBuilder`.
`cpp` unit tests (`*_test.cpp`) are not in `AMBuilder`; they are compiled by `scripts/run_*_tests.sh`.

A run folder's `build-info.txt` records the checkout SHA, whether tracked files were modified and the first 16 hex digits of the plugin SHA-256.
With a dirty tree the commit alone does not identify the binary; `DEV_BUILD_HASH` (16 or 64 hex digits) makes a repeat run verify it.

## `scripts/dev.sh <map> <bots> <minutes>`

Builds, deploys into the test server, starts a run through a PTY (no RCON needed), summarizes. Test run folders: `<test server>/dev-runs/`, recorder
sessions `<test server>/empires/analysis/session-*`. Run folder names look like `20261123-122645-emp_crossroads-10b-15m`.

| Variable | Meaning |
|---|---|
| `DEV_TIMESCALE=4` | `host_timescale`; minutes and waits count game time, log `engine->Time()` stays real time. Default 1. |
| `DEV_ROSTER="2:1 2:3"` | Team:class per bot (team 2 NF, 3 BE; class 0 Scout, 1 Rifleman, 2 Grenadier, 3 Engineer) instead of the alternating default; sets the bot count. Bots are named `NF_01`, `BE_01` ... |
| `DEV_PRE="cmd;cmd"` | Console commands before the bots join (`rcbot_empires_auto_commander 0` to switch the auto commander off). |
| `DEV_CMDS="wait 10;cmd;waitfor 120 regex;mark;cmd"` | Commands after the bots joined. `waitfor` searches up to N game seconds from run start; `mark` resets that point for repeated results (for example `build-assign result=`); `NOT found` stops the sequence. |
| `DEV_UNTIL` / `DEV_EXPECT` / `DEV_FAIL` / `DEV_SHOW` | Regex: end the run on a match (always set it) / require a matching line after the latest mark / abort on a failure line / print matching lines at the end (repeats collapsed, keep it small). Check the exit status or the `Run result:` line. |
| `DEV_PORT` | Server port, default 49925. Parallel runs need their own port each. |
| `DEV_SKIP_BUILD=1`, `DEV_BUILD_HASH` | Reuse the last build; verify its hash. |
| `DEV_NO_RECORDER=1` | Faster; only server-side evidence remains. Use the recorder when the question needs its data. |
| `DEV_NO_CHECK=1` | Skip the 1-minute health check (runs with 6+ bots). |
| `DEV_SRCDS_ARGS` | Extra `srcds_run` arguments (for example `-nowatchdog` for navmesh generation). |
| `DEV_SOURCETV=1`, `DEV_MANUAL=1`, `DEV_MANUAL_JOIN_MIN`, `DEV_MANUAL_MAX_MIN`, `DEV_SERVER_IP` | Human watch test: SourceTV demo, run ends after the person disconnected. Join with `connect <server-ip>:49925`, not 127.0.0.1. Example: `DEV_SERVER_IP=<server-ip> DEV_MANUAL=1 DEV_SKIP_BUILD=1 DEV_NO_RECORDER=1 DEV_PRE="rcbot_empires_auto_commander 0" scripts/dev.sh emp_crossroads 6 240`. |
| `RCBOT_CPUS` (default `4,5,10,11`, empty = all), `RCBOT_NICE` (10) | Builds and test servers run on a few CPUs with low priority (`scripts/dev_cpu.sh`) so a game client stays smooth. |
| `RCBOT_DEV_MIRROR` | Location of the incremental build mirror. |

Default test server economy: resource x10, research complete, debug vehicle purchase (`dev.sh` sets `emp_sv_research_complete 1`).
For a realistic match pass production values:
`DEV_PRE="emp_sv_resource_multiplier_be 1;emp_sv_resource_multiplier_nf 1;emp_sv_refinery_multiplier 1;emp_sv_research_complete 0;emp_sv_research_multiplier 1;emp_sv_debug_build_vehicles 0"`.

## Scenario scripts (`scripts/`)

Each takes `[map] [timescale]` unless noted and ends at the observable result. Prefer the smallest scenario that triggers the mechanic.

| Script | Checks | Approx. time |
|---|---|---|
| `dev_check.sh [map]` | **Regression**: unit tests plus 5 parallel scenarios (chain, move, pad, turret, revive); PASS/FAIL per test | 1-1.5 min |
| `dev_chain.sh` | Commander seat, barracks placement, engineer builds it | 1-2 min |
| `dev_quick.sh [map] [min]` | Short targeted run: few bots (`QUICK_ROSTER`), instant purchase, 1x; `QUICK_PRE/CMDS/UNTIL/SHOW`, `QUICK_FAIR=1` fair tank start | 1-4 min |
| `dev_econ.sh [map] [min]` | Production economy until the first driven vehicle, 6 bots, timescale 4 | about 5 min |
| `dev_team_match.sh [map] [min]` | 10-bot match at 1x with recorder, then director numbers (role swaps, alerts, kill-spawns, building stalls, recorder table). `--summarize RUN_DIR` reads saved numbers without a server | 12-15 min |
| `dev_buy.sh` | Vehicle purchase only (instant VF, extra money) | about 30-60 s |
| `dev_vf_drive.sh [0\|1\|place] [map] [nf\|be] ["x y z"]` | Regular purchase, VF exit, drive; `place` stops at first factory placement; `DEV_VF_APPROACH` | - |
| `dev_turn.sh <1-4> [map] [nf\|be]` | Steering, handbrake pulse, coast, brake at 1x | - |
| `dev_vehicle_obstacle.sh [0\|1] [map] [nf\|be] [ahead] [lateral] [model]` | Prop placed on the driver's route; ends at `vehicle obstacle passed\|timeout\|abort` | - |
| `dev_pad.sh`, `dev_mixed.sh` | Damaged tank to repair pad and refit; mixed loadout refit | 40-60 s |
| `dev_ammo.sh`, `dev_crate.sh`, `dev_ammocall.sh` | Empty cannon to pad refit; to nearer ammo crate; ammo call and engineer crate (`DEV_AMMO_TANKS=2`) | 1-3 min |
| `dev_orders.sh`, `dev_cmdorders.sh` | Commander orders like the game UI (move, attack target, guard); bot commander's own orders (`DEV_CMDORDERS_OFF=1` compares) | 3-4 min |
| `dev_engibot.sh` | `/buildengibot` purchase/limit/money; `DEV_ENGIBOT_MODE=build\|level` | 1-1.5 min |
| `dev_turret.sh` | Refinery MG turret; `DEV_TURRET_UPGRADE=1` upgrade 1 to 3 | - |
| `dev_revive.sh`, `dev_heal.sh`, `dev_seismic.sh` | Revive, heal teammate, seismic throws at a barracks (`DEV_SEISMIC_POS`) | 1 min each |
| `dev_lawliet.sh [map] [min]` | Forced NF APC rush on the BE CV at 1x; success ends at `lawliet end team=2 reason=cv-destroyed` | about 4 min |
| `dev_navgen.sh <map>` | Generate a navmesh on the test server (`DEV_NAVGEN_PRE`) | up to minutes (20 min timeout hit on `emp_mvalley`) |
| `dev_maps.sh [out] ["map:min:ts ..."]` | Map sweep, 10 bots per map one after another | - |
| `manual_match.sh [map] [bots]` | Watch match with a person; records server.log, recorder and a SourceTV demo, writes `fehler.txt` error counts | - |
| `run_unit_tests.sh` | All offline unit tests in parallel (needs only a C++17 compiler and python3); 34 tests in `dev_check` as of 23.11 | about 5 s |
| `run_*_tests.sh` | One header's unit tests (ai_decision, capture_retry, combat_relevance, lawliet, squad_*, team_director, vehicle_loadout ...) | - |
| `publish_tree.sh ["subject"] ["extra"]` | Builds the public branch commit (internal notes excluded); pushing is a separate manual step | - |

`dev_check.sh` uses five consecutive ports from `DEV_CHECK_PORT` (default 49925); choose a free range when another run is active.
`DEV_SKIP_UNIT_TESTS=1` skips unit tests, `DEV_CHECK_SERIAL=1` runs serially. Run it after every change to bot or test scripts.

Infantry combat changes use short tests with bots dropped by `setpos` (stop with `DEV_UNTIL`, at most 3 minutes), then the 15-minute
match `scripts/dev_team_match.sh emp_crossroads 15` with `emp_sv_research_complete 0`. Compare new vs. old by cvar value `2`/`3` (NF only / BE only)
and with swapped sides, because the dropping side decides short duels (duel `131000`/`131009`: BE won both).

## How to evaluate a run

1. **Reuse existing runs first** when build, map, speed and trigger fit the question. For before/after comparisons use the same conditions and the same measure.
2. **One main question per 10-bot match at 1x**; evaluate other open points from the same run only when their trigger occurred; leave unobserved cases open.
   A single match comparison is a hint; repeat it when keep-or-revert depends on it.
3. Rare mechanics: trigger in the smallest scenario and end at the observable outcome (for example grenade reserve rises); judge natural activation in a regular match separately.
4. Read-only summaries:
   - `scripts/dev_team_match.sh --summarize RUN_DIR`: director numbers plus the recorder table if `summary.txt` exists.
   - `scripts/dev_run.py --summarize RUN_DIR SESSION_DIR`: rebuilds `RUN_DIR/summary.txt` from raw recorder data (preserve the original first if you need its live crash assessment).
5. Always filter logs (`grep`), never read a whole `server.log` (thousands of lines). Useful lines: `stall-escalate who=...`, `navmesh-stuck` / `navmesh-stuck-trace`,
   `navmesh-plan-failed`, `vehicle give-up`, `vehicle stuck-front`, `detour no-exit`, `building stalls` (`navmesh-building-blocked`), `commander-sector`,
   `commander-move result=`, `build-assign result=`, `vehicle fire ... buttons= why= weapons=id:button:magazine/reserve`, `vehicle econ`, `lawliet ...`, `Run result:`, `Stuck share of lifetime`.
6. Spatial problems: look at a picture first (`tools/nav_overview.py`, see [Maps](Maps.md#offline-navmesh-tools)); `empires_nav_scan` uses the player hull, not the vehicle hull.
7. Record which revision (and build hash) an observation covers. Distinguish "run folder exists" from "verified successful map test". Distinguish fire requests in logs from shots and damage.
8. A change counts as working only with numbers from a real run; otherwise write "built, not measured". Record when a passing run did not reproduce the original failure.

Reference matches (all `emp_crossroads`, 10 bots, 1x, 15 min, `emp_sv_research_complete 0`): `20261123-122645` (current reference: 0 crashes, 0.24 % stuck, 17 kills,
0 TK, 6 vehicles) versus `20261122-213339` (1.0 % stuck, 12 kills, 9 vehicles). Acceptance rule for new infantry steps: kills >= reference, stuck <= 0.5 %, no new crashes/TK, idle not higher.

## Recorder and run evaluation

Passive and opt-in; it never selects goals, moves bots or changes cooldowns. A filesystem or write error disables it with a clear log line.

```text
rcbot_analysis_start | rcbot_analysis_status | rcbot_analysis_mark <text> | rcbot_analysis_stop
rcbotd analysis_start | analysis_status | analysis_mark <text> | analysis_stop      (same operations)
```

Output under the game directory: `analysis/session-YYYYMMDD-HHMMSS/{metadata.json, events.jsonl, snapshots.jsonl, summary.json}`. Events are sampled or
deduplicated transitions plus production events (session/game time, bot identity, team/class, position, goal/task, enemy context). Snapshots at most every
1.5 s per bot; writes buffered to 32 KiB. `mark` records a `USER_MARK`. Hang rule in the summary: alive at least 20 s and moved less than 100 units;
"Stuck" has a task, "Idle" has none.

## Claude Code agents (optional)

`.claude/agents/tester.md` (small model: starts exactly the given runs, filters, reports only requested lines) and `expert.md` (strongest model:
root cause after two failed attempts or binary/engine interpretation; it does not know the conversation, so the task must include symptom, numbers, run folder,
attempts and the concrete question). Expert findings about the game binary go to `reverse-engineering/notes/` in English.
