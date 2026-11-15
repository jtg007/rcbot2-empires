# RCBot2 for Empires — working rules for AI agents and contributors

Read by Codex (`AGENTS.md`) and by Claude Code (through `CLAUDE.md`). Humans: the
same rules apply to you.

Goal: improve the existing RCBot2 Empires bots step by step into good, playable bots.
No rewrite, no parallel "AI v2", no new framework without a measured gap.

## Rules

- One problem per step: baseline run, smallest fix, the same run again, compare.
- A change counts as working only with numbers from a real test run (log lines,
  counts, times). Write "built, not measured" when that is the state.
- Build, deploy and test only on an **isolated** test server, never on a public one.
- No speculative refactors. Read the existing logs, code and game binary before
  starting a new run.
- Open work, in order: [`docs/OPEN_WORK.md`](docs/OPEN_WORK.md). Keep it updated
  (tick items, add the run folder and the measured numbers).
- Mark historical plans clearly and link to the current work list.
- Check available space before recording long test sessions.
- Check map geometry and building access before adding expansion placement rules.
- Distinguish an existing run folder from a verified successful map test.
- Verify ammunition sources and depletion behavior in game before claiming resupply works.
- Distinguish fire requests in logs from actual shots and damage.
- Read each vehicle's weapon slots and primary/secondary assignments when testing fire behavior.
- Verify weapon-group switching and mixed loadouts in game.
- Record the raw active weapon group, slot, mode, magazine and reserve before changing fire selection.
- Vehicle fire logs (`vehicle fire … buttons= why= weapons=id:button:magazine/reserve`) show the chosen
  button and ammo; compare consecutive lines for shots. Weapons must fit each chassis slot's type and size.
- Compare diagnostic logging with the same short run when changing its default.
- Public text and code comments in English.
- Keep the README and public open-work notes in sync with changes to bot capabilities.
- Record when a successful run did not reproduce the exact original failure.
- Test harnesses should wait for the success condition they actually verify.
- Record completion distance when measuring movement orders.
- Retain recovery and failure counts even when a full-match outcome succeeds.

## Setup

| What | Where |
|---|---|
| Source | `utils/RCBot2_meta/` |
| Build dependencies | a folder with `alliedmodders/{hl2sdk-sdk2013,metamod-source,sourcemod}` and `hl2sdk-manifests/`; default `../rcbot2-clean`, override with `RCBOT_DEPS_DIR` |
| Test server | an Empires dedicated server (Linux, SteamCMD) with Metamod:Source and this plugin installed once; set `EMPIRES_TEST_SERVER` in `scripts/dev.local` (copy `scripts/dev.local.example`) |
| Navmeshes | `empires/maps/<map>.nav`; missing ones: load the map, `sv_cheats 1; p_nav_generate` |
| Test runs | `<test server>/dev-runs/` |
| Game binary | `<test server>/empires/bin/server_srv.so` (32 bit, with symbols) |

## Build and test

```sh
scripts/dev_build.sh                       # incremental build (~5 s), prints the .so path
scripts/dev.sh <map> <bots> <minutes>      # build, deploy, run, summary
scripts/dev_check.sh                       # regression checks in parallel, ~1 min, PASS/FAIL per test
scripts/run_*_tests.sh                     # unit tests of single headers
```

Purchase only (~30 s, instant factory, extra money): `scripts/dev_buy.sh [map] [timescale]`.
Mixed loadout refit on the pad (~1 min): `scripts/dev_mixed.sh [map] [timescale]`.
Empty cannon -> pad -> refit (~1.5 min): `scripts/dev_ammo.sh [map] [timescale]`. Empty cannon -> nearer team
ammo crate (~1 min): `[DEV_CRATE_WAIT=s] scripts/dev_crate.sh [map] [timescale] [distance]`. Ammo call ->
engineer places a crate (~3 min): `[DEV_AMMO_WAIT=s] scripts/dev_ammocall.sh [map] [timescale]`.
Builds and test servers run on CPUs `RCBOT_CPUS` (default `4,5,10,11`, empty = all) with nice
`RCBOT_NICE` (10), so a game client on the other cores stays smooth (`scripts/dev_cpu.sh`); `dev_check` then takes
about 1.5 min instead of 1 (all PASS). Set `RCBOT_CPUS=` to use every core.
Production economy quickly: `scripts/dev_econ.sh [map] [game-minutes]` (6 bots, fast-forward 4,
ends when a bot drives a vehicle it bought, ~5 min). While iterating run only the matching
single test; run `scripts/dev_check.sh` and one full match (10 bots, timescale 1) before a commit.

Environment variables for `dev.sh`:
- `DEV_UNTIL="regex"`: end the run when the line appears. Always set it.
- `DEV_SHOW="regex"`: print matching log lines at the end. Keep output small.
- `DEV_ROSTER="2:1 2:3"`: team:class per bot (team 2 = NF, 3 = BE; class 0 Scout,
  1 Rifleman, 2 Grenadier, 3 Engineer).
- `DEV_CMDS="wait 10;cmd;waitfor 120 regex;cmd"`, `DEV_PRE="cmd;cmd"` (before bots join).
- `DEV_TIMESCALE=4`: fast-forward (breaks mortars, see below). `DEV_NO_RECORDER=1`: faster.
- `DEV_PORT=49926`: own server port so several runs can go at once (default 49925).
  `DEV_SKIP_BUILD=1`: reuse the last build. The plugin is replaced atomically, so running servers survive.

A shared library links even with missing symbols; the run then only says "RCBot2 not loaded"
(`undefined symbol` in `server.log`). Seen causes: a function defined in an anonymous namespace
but declared globally elsewhere, and `edict->StateChanged()` (needs engine-only symbols; set
`m_fStateFlags` instead). Check with `nm -uC <.so>`.

Detailed diagnostics default to off to keep normal server logs readable. The harness
turns them on (`rcbot_empires_debug_log 1`); use that cvar for manual diagnosis too.
A run writes thousands of log lines: always filter, never read a whole log.

**Test server economy:** a typical isolated test config uses resource ×10, research
complete and debug vehicle purchase. For a realistic match pass production values,
e.g. `DEV_PRE="emp_sv_resource_multiplier_be 1;emp_sv_resource_multiplier_nf 1;emp_sv_refinery_multiplier 1;emp_sv_research_complete 0;emp_sv_research_multiplier 1;emp_sv_debug_build_vehicles 0"`.

## Tools for root-cause work

| What | How |
|---|---|
| Navmesh at a spot | console `rcbotd empires_nav_scan x y z x2 y2 z2`: route, areas, hull grid (`.` free, `#` world, `E` entity) |
| Move order test | `rcbotd empires_test_commander_move bot <userid> x y z` (first bot has userid 2) |
| Game binary | `nm -C --defined-only`, `objdump -d -C --start-address=…`; float constants from `.rodata` with Python `struct.unpack('<f', …)` |
| Notes on game internals | `reverse-engineering/` |
| Hang analysis | `navmesh-stuck` + `navmesh-stuck-trace` lines show route point, position and what the hull hits (`worldspawn` = map geometry) |

## Known facts (measured, do not re-derive)

- Mortar and fast-forward: `CEmpShell` stores `gpGlobals->realtime` at spawn and explodes
  once `curtime - spawn > 30`. With `host_timescale` > 1 every shell explodes at the bot.
  Mortar tests at timescale 1.
- Engineer kit traces only 100 units from the eye; building: 1 clip round = 1 HP, ~4.7 HP/s.
- A MOVE order ends in the game only within 100 units (3D) of its location.
- Navmesh limits in `bot_navmesh_api.cpp`: climb max 40, drop max 200. The generated mesh
  does not know buildings; `ApplyBuildingDetours` routes around them.
- After a hang, `kFailureCooldown` (15 s) blocks the same goal+segment.
- `emp_streetsoffire` has no flags (captures are always 0).
- The game binary is checked by size/CRC (`bot_empires_binary.cpp`); after a game update add
  the new value to `kKnownBinaries` or fixed-offset features stay off.

## Working with an AI agent

- Delegate long test runs to a cheap model/subagent that only runs the given command and
  reports the requested lines and numbers (`.claude/agents/tester.md`).
- Use the strongest model for root causes (game binary, navmesh geometry, engine behaviour)
  when a cause is unclear after 1–2 attempts (`.claude/agents/expert.md`). Give it: symptom,
  numbers, run folder and log lines, what was tried, the concrete question.
- Bundle proven fixes into one run; one diagnostic run, repeat only to confirm.
- Sandboxes: test runs need write access to the test server and `~/.cache/rcbot2-dev-build`,
  a PTY, a netlink socket and a local UDP port. Without that, only edit and build.
