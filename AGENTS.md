# RCBot2 for Empires — working rules for AI agents and contributors

Read by Codex (`AGENTS.md`) and by Claude Code (through `CLAUDE.md`). Humans: the
same rules apply to you.

Goal: improve the existing RCBot2 Empires bots step by step into good, playable bots.
No rewrite, no parallel "AI v2", no new framework without a measured gap.

## Rules

- Before a diagnostic run, state which observation would change the next step. Reuse a saved baseline when the build, map, speed and trigger fit the question. Compare the same outcome under the same conditions after the smallest fix.
- Trigger rare mechanics in a short scenario that ends at the observable outcome (for example, grenade reserves increase). Assess natural activation during a regular match.
- Give each 10-bot match at 1× one primary question. Use the same run for other checks when their triggers occur. Leave unobserved cases open. Treat a single match comparison as an indication and repeat it when the result determines whether to keep or revert the change.
- Use the shortest test that answers the question: unit tests, then a scenario script or `dev_quick.sh`
  (time scale only where allowed), then a 10-bot match. Run a 10-bot match at 1× once the short test passes
  and match impact is the question.
- Do not assume game mechanics (valid targets of a weapon, turret levels per map). Check the game binary
  or game data before implementing them.
- Revert a fix that makes the measured outcome worse instead of committing it.
- Never kill processes by name or pattern (`pkill`, `killall srcds`, …); only stop PIDs you started. Parallel test runs, a real server and the game client may be running alongside.
- A change counts as working only with numbers from a real test run (log lines,
  counts, times). Write "built, not measured" when that is the state.
- Build, deploy and test only on an **isolated** test server, never on a public one.
- Bots should act like players, not repeat a failing action: check progress locally and escalate in steps
  (try differently, other goal, give up), logging `stall-escalate who=... stage=... reason=...` so real bugs stay visible.
  No central framework.
- No speculative refactors. Read the existing logs, code and game binary before
  starting a new run.
- Documentation, static review and analysis of saved logs can be completed offline. For bot or test-script changes run `scripts/dev_check.sh`. Record which revision a gameplay observation covers.
- Follow the priority order in [`docs/OPEN_WORK.md`](docs/OPEN_WORK.md). Reuse
  measured runs for related checks. Close each item when its remaining case
  has measured results, and record the numbers and run reference.
- Mark historical plans clearly and link to the current work list.
- Check available space before recording long test sessions.
- Check map geometry and building access before adding expansion placement rules.
- Verify straight factory departure after a regular purchase at normal game speed. Record the
  factory placement and whether the vehicle cleared the bay before route following resumed.
  When diagnosing a blocked exit, check both sides of the vehicle's path.
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
- For vehicle handbrake tests, compare the same chassis and entry speed and track sideways
  motion as well as heading change.
- Confirm vehicle handling changes at normal game speed. Record arrivals, planned reverse
  turns, stalls and exits.

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
scripts/run_unit_tests.sh                  # all offline unit tests (~5 s, no SDK or server)
scripts/dev_run.py --summarize RUN_DIR SESSION_DIR  # evaluate an existing recording
# Select one scripts/run_*_tests.sh for a single header
```

GitHub Actions builds Linux and Windows (MSVC, x86) on every push; a push to `main` refreshes the
`latest` release. Bundle commits before pushing. For MSVC: the SDK defines `min`/`max`/`snprintf` as
macros, so write `(std::min)(…)`, `(std::numeric_limits<T>::max)()` and plain `snprintf(`; keep Linux-only
APIs (`dlfcn.h`, ELF) under `#ifdef __linux__`.

Purchase only (~30 s, instant factory, extra money): `scripts/dev_buy.sh [map] [timescale]`.
Lawliet rush (forced NF crewed APC on the BE CV, 1x): `scripts/dev_lawliet.sh [map] [minutes]`; success ends the run at `lawliet end team=2 reason=cv-destroyed`.
Team match check (10 bots, 1x, recorder, then role swaps, alerts, kill-spawns, building stalls):
`scripts/dev_team_match.sh [map] [minutes]`. To read the same metrics from a saved match, use
`scripts/dev_team_match.sh --summarize RUN_DIR`. It reads the log and any saved recorder table.
Short targeted run instead of a full match (3 bots, instant purchase, 1x, 1-4 min; adapt with
`QUICK_ROSTER`, `QUICK_PRE`, `QUICK_CMDS`, `QUICK_UNTIL`, `QUICK_SHOW`; `QUICK_FAIR=1` = both teams start together): `scripts/dev_quick.sh [map] [minutes]`.
`rcbot_empires_vehicle_shell_trace 1` logs shell flight and impact (`vehicle shell-end`) and turret aim.
For the factory exit and subsequent route, use `scripts/dev_vf_drive.sh [0|1|place] [map] [nf|be] ["x y z"]`.
The factory is the first building. The NF buyer joins when its terminal is ready.
BE starts a rifleman early to give it time to leave the barracks.
The test uses a normal purchase and runs at normal game speed.
Choose 0 or 1 for the experimental curve speed setting. A completed run
confirms the purchase, exit, and start of route driving. Assess corner clearance
with a separate fixed-start comparison.
Use `place` to stop at the first automatic factory placement result. Supply
build-command coordinates in driving mode to place a factory through the game
at yaw 0. For the Crossroads NF exit, use
`DEV_VF_APPROACH="-300 -12760 -220" scripts/dev_vf_drive.sh 0 emp_crossroads nf "-1500 -12900 -223.96875"`.
The factory's resulting origin is `(-1500 -12900 -234)`. The optional
approach position starts the buyer near the ramp for the normal purchase.
`vehicle factory-exit drive` includes speed, and `vehicle factory-exit hull`
reports the forward hull sweep with the hit class and entity position.
For a fixed build site with automatic heading selection, use
`rcbotd empires_commander_build 5 x y z auto` in the dedicated server console.
`factory-yaw` reports the selected heading and the first building that blocked a candidate lane.
To compare local maneuver selection independently, use VF mode 0 and set
`DEV_PRE="rcbot_empires_vehicle_maneuvers 1"` for the enabled run.
The preview checks the regular steering arc first and searches for alternatives
when that arc hits a blocking entity or recovery from a maneuver stall is in
progress. Regular steering handles world geometry and ground clearance. In the drive logs,
`local=1` marks eligible route driving and `maneuver_apply=1` marks an applied
maneuver. Compare obstacle stops and route progress before enabling it by default.
The default remains 0. Focused checks passed: after a retry-release code change,
the Crossroads NF obstacle was passed in 1.86 seconds without needing a retry;
Crossroads VF speed stayed within 6% of selection-off results, and the Palmbay
BE exit was reached with selection on. In 10-bot Crossroads matches, the straight
NF exit timed out before maneuver selection in all three purchases from the same
factory. Both enabled drivers later gave up. At the same fixed NF site, the empty
bay cleared in 2.6 s. Adding the radar reproduced the stall and the sweep hit its
vehicle collision proxy. The updated heading selector chose 90° and the NF Light
cleared the exit in 1.8 s. The BE control kept its −96° heading and cleared in 2.6 s.
Both drove their routes and `dev_check` passed 5/5. On Palmbay, stop the placement
diagnostic on the first `NO_SPOT` for type 5 and inspect native candidate rejections
before running another match. See [open work](docs/OPEN_WORK.md) for these cases
and the separate Crossroads infantry route failures.
For a fixed obstacle on the route, run
`scripts/dev_vehicle_obstacle.sh [0|1] [map] [nf|be] [ahead] [lateral] [model]` (debug purchase, 1x,
0/1 = local maneuver selection off/on). Once the driver rolls at 450 HU/s or more,
`rcbotd empires_vehicle_obstacle` places a solid `prop_dynamic` (default
`models/props/concretebarrier.mdl`) `ahead` units along its route, offset `lateral` to the left.
The log reports `vehicle obstacle placed|passed|timeout|abort` with time, minimum distance, time
stopped near the obstacle, stall events and maneuver time. `vehicle maneuver-perf` reports planner
calls, traces and microseconds every 10 seconds; `vehicle maneuver-block` shows what blocks the regular arc (class, distance, `normal_z`). `rcbot_empires_vehicle_maneuver_budget` (default 2)
caps planner evaluations per server tick across all vehicles.
For a clear-road turn diagnostic after an in-place debug purchase, run
`scripts/dev_turn.sh <1|2|3|4> [map] [nf|be]` with an NF Light (default) or a BE AFV.
Mode 1 uses steering alone, and mode 2
adds a 0.2-second handbrake pulse. Modes 3 and 4 measure straight-line coasting
and braking from at least 500 HU/s. Filter the server log for `vehicle turn-test`.
Damaged tank to pad and refit (~40 s): `scripts/dev_pad.sh [map] [timescale]` disables
refinery turret jobs for this run. `dev_check.sh` covers turrets in a separate test.
Turret upgrade 1 -> 3 (~1.5 min): `DEV_TURRET_UPGRADE=1 scripts/dev_turret.sh [map]` (ranks the engineer up with cheats).
Refinery wall, then a camera behind it (~3 min): `scripts/dev_deploy.sh [map] [timescale]` (`DEPLOY_MODE=1` camera/radar only, `2` walls only; `DEPLOY_RANK=1` ranks the engineer up: two turrets, camera and radar).
Engineer heals a hurt teammate (~1 min): `scripts/dev_heal.sh [map] [timescale]`. Engineer throws seismics at the NF barracks (~1 min, 1x): `[DEV_SEISMIC_POS="x y z"] scripts/dev_seismic.sh [map] [timescale]`.
Mixed loadout refit on the pad (~1 min): `scripts/dev_mixed.sh [map] [timescale]`.
Empty cannon -> pad -> refit (~1.5 min): `scripts/dev_ammo.sh [map] [timescale]`. Empty cannon -> nearer team
ammo crate (~1 min): `[DEV_CRATE_WAIT=s] scripts/dev_crate.sh [map] [timescale] [distance]`. Ammo call ->
engineer places a crate (~3 min): `[DEV_AMMO_WAIT=s] [DEV_AMMO_TANKS=2] scripts/dev_ammocall.sh [map] [timescale]`
(`DEV_AMMO_TANKS=2`: two empty tanks at once).
EngiBots (`/buildengibot`, ~1 min): `[DEV_ENGIBOT_MODE=build|level] scripts/dev_engibot.sh [map] [timescale]`.
Commander orders like the game UI (move, attack target on an enemy, guard on a refinery, ~3 min): `scripts/dev_orders.sh [map] [timescale]`. Bot commander's own orders (~4 min, map with CV and flags, default emp_palmbay): `[DEV_CMDORDERS_OFF=1] scripts/dev_cmdorders.sh [map] [timescale] [minutes]`. Generate a navmesh (`-nowatchdog`, map files from `maps/<map>.res` first): `scripts/dev_navgen.sh <map>`. `DEV_SRCDS_ARGS` passes extra `srcds_run` arguments.
Builds and test servers run on CPUs `RCBOT_CPUS` (default `4,5,10,11`, empty = all) with nice
`RCBOT_NICE` (10), so a game client on the other cores stays smooth (`scripts/dev_cpu.sh`); `dev_check` then takes
about 1.5 min instead of 1 (all PASS). Set `RCBOT_CPUS=` to use every core.
Production economy quickly: `scripts/dev_econ.sh [map] [game-minutes]` (6 bots, fast-forward 4,
ends when a bot drives a vehicle it bought, ~5 min). Use the matching short
scenario for iteration and run all five checks with `scripts/dev_check.sh` for a
testable set of changes. For changes that affect match play, run the full
10-bot match at normal speed for acceptance after the short scenarios succeed.

Environment variables for `dev.sh`:
- `DEV_UNTIL="regex"`: end the run when the line appears. Always set it.
- Set `DEV_EXPECT="regex"` to require a matching server log line at or after the
  latest mark. `DEV_FAIL="regex"` ends the scenario on a failure line, including
  during command waits. Check the exit status or the `Run result:` line.
- `DEV_SHOW="regex"`: print matching log lines at the end. Keep output small.
- `DEV_ROSTER="2:1 2:3"`: team:class per bot (team 2 = NF, 3 = BE; class 0 Scout,
  1 Rifleman, 2 Grenadier, 3 Engineer).
- `DEV_CMDS="wait 10;cmd;waitfor 120 regex;cmd"`, `DEV_PRE="cmd;cmd"` (before bots join). `waitfor` searches from the
  start of the run; `mark` resets that point (for repeated results such as `build-assign result=`). A `NOT found` stops the sequence.
- `DEV_TIMESCALE=4`: fast-forward (breaks mortars, see below). `DEV_NO_RECORDER=1`: faster.
- `DEV_PORT=49926`: own server port so several runs can go at once (default 49925).
  `DEV_SKIP_BUILD=1`: reuse the last build, including in `dev_check.sh` and
  `dev_maps.sh`. Set `DEV_BUILD_HASH` to verify its SHA-256 (16 or 64 characters).
  The build and server startup use `flock` to protect the selected binary.

A shared library links even with missing symbols; the run then only says "RCBot2 not loaded"
(`undefined symbol` in `server.log`). Seen causes: a function defined in an anonymous namespace
but declared globally elsewhere, and `edict->StateChanged()` (needs engine-only symbols; set
`m_fStateFlags` instead). Check with `nm -uC <.so>`.

Detailed diagnostics default to off to keep normal server logs readable. The harness
turns them on (`rcbot_empires_debug_log 1`); use that cvar for manual diagnosis too.
A run writes thousands of log lines: always filter, never read a whole log.
Use the dedicated server console for Empires test and diagnostic commands.
`rcbotd empires_test_botcmd` accepts repeat counts from 1 to 32.

**Test server economy:** a typical isolated test config uses resource ×10, research
complete and debug vehicle purchase. For a realistic match pass production values,
e.g. `DEV_PRE="emp_sv_resource_multiplier_be 1;emp_sv_resource_multiplier_nf 1;emp_sv_refinery_multiplier 1;emp_sv_research_complete 0;emp_sv_research_multiplier 1;emp_sv_debug_build_vehicles 0"`.

## Tools for root-cause work

| What | How |
|---|---|
| Navmesh at a spot | console `rcbotd empires_nav_scan x y z x2 y2 z2`: route, areas, hull grid (`.` free, `#` world, `E` entity); needs at least one bot on the server, otherwise `navmesh-not-loaded` |
| Move order test | `rcbotd empires_test_commander_move bot <userid> x y z` (first bot has userid 2; test hook, bypasses the game's order path) |
| Order like a human | `rcbotd empires_test_unit_order <commander_userid> <type> <unit_userid> x y z [target_userid\|b\|bd]`: the seated commander bot selects the unit (`emp_unit_order_list`) and sends `emp_unit_order` (0 abort, 1 move, 2 attack location, 3 attack target, 4 guard); `b` = nearest own building below full health, `bd` = nearest full one, damaged to 100 |
| Kill zone test | `rcbotd empires_test_kill_zone x y z radius [team]`: every bot (of that team) on foot entering the circle is killed once per life and counted for hot zones (`director death`, `director hot-zone`); radius 0 disarms |
| Place a building | `rcbotd empires_commander_build <type> spot` (valid spot near the CV, no engineer assigned) |
| Game binary | `nm -C --defined-only`, `objdump -d -C --start-address=…`; float constants from `.rodata` with Python `struct.unpack('<f', …)` |
| Notes on game internals | `reverse-engineering/` |
| Hang analysis | `navmesh-stuck` + `navmesh-stuck-trace` lines show route point, position and what the hull hits (`worldspawn` = map geometry) |

## Known facts (measured, do not re-derive)

- Mortars, tank cannons and fast-forward: both fire `CEmpShell`, which stores `gpGlobals->realtime`
  at spawn and explodes once `curtime - spawn > 30`. With `host_timescale` > 1 every shell explodes
  at the muzzle. Mortar and cannon tests at timescale 1; fast-forward only for driving/economy.
- Tank turrets read the command view angles relative to the hull (see `reverse-engineering/notes/vehicle-driving.md`).
- Engineer kit traces only 100 units from the eye; building: 1 clip round = 1 HP, ~4.7 HP/s.
- A MOVE order ends in the game only within 100 units (3D) of its location.
- Navmesh limits in `bot_navmesh_api.cpp`: climb max 40, drop max 200. The generated mesh
  does not know buildings; `ApplyBuildingDetours` routes around them.
- After a hang, `kFailureCooldown` (15 s) blocks the same goal+segment.
- `emp_streetsoffire` has no flags (captures are always 0).
- The game binary is checked by size/CRC (`bot_empires_binary.cpp`); after a game update add
  the new value to `kKnownBinaries` or fixed-offset features stay off.

## Working with an AI agent

- One agent runs the whole loop: change, build, test (in the background, output to a file,
  filter only the needed lines), evaluate. No mandatory hand-offs between agents.
- Prioritize by what a player notices in a 10-bot match at 1× (captures, vehicles bought
  and arriving, no hangs) over technical fine-tuning.
- Timebox: if a topic shows no measured gain in the default setting after about one
  session, park it in the open-work list with a one-line finding and move on.
- After two substantive failed attempts, ask an expert model once (symptom, measurements,
  runs, both attempts, the concrete question). Without a viable approach, park the problem.
- Keep `HANDOFF.md` to the current state (about 80 lines); history lives in git.
- Sandboxes: test runs need write access to the test server and `~/.cache/rcbot2-dev-build`,
  a PTY, a netlink socket and a local UDP port. Without that, only edit and build.

For spatial problems, look at a picture first: `python3 -I tools/nav_overview.py <map>.nav --bsp <map>.bsp --terrain --props --vehicle-width 284 --log <run>/server.log --userid <uid> --crop x0,y0,x1,y1 --output out.png` draws navmesh, terrain slope, static props, the driven lane and stall marks (vehicle bottlenecks highlighted); `tools/check_nav.py` checks components. Maps like Palmbay use displacement terrain, so cliffs are steep triangles, not brushes. Confirm with in-game scans and hull traces (`empires_nav_scan` uses the player hull, not the vehicle). An overview does not prove vehicle clearance or account for dynamic building obstacles. No external tool is a bot build dependency.
