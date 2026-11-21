# RCBot2 for Empires

**Bots for the Source mod [Empires][empires]** – the mix of first-person shooter and
real-time strategy. Built on [RCBot2][rcbot2-fork], running on a Linux dedicated server
(Source SDK 2013, x86).

> [!WARNING]
> **Work in progress.** The bots already play whole matches – commander, engineers,
> infantry and vehicles – but they are not finished and still make mistakes.
> See [what works](#what-the-bots-can-do) and [known issues](#known-issues).
> Current work: factory sites on `emp_palmbay`, refinery
> stand points on Coast, and confirming the new research path, vehicle parts and infantry
> attacks on buildings in full matches. The [open-work list](docs/OPEN_WORK.md) shows the
> checks behind each item.

## The goal

Bots that play Empires the way a team does, not generic shooter bots:

- **Automatic navigation.** Use generated navmeshes; map coverage and some waypoint-based behavior still need testing.
- **A bot commander** builds the base, keeps the economy running and leads the team,
  or hands the seat to a human player.
- **Human commanders can command bots** like units in a real-time strategy game:
  move, abort, attack location and guard orders work (tested through the game's own order
  commands, not yet with a real human); attack target is still open.
- **Engineers** keep the base alive: build, repair, revive, place turrets.
- **Vehicles** are bought, driven, fought with and brought back for repairs.

## What the bots can do

✅ works in matches · 🟡 partly / not reliable yet · ⬜ planned

### Movement

| | Feature | Notes |
|:-:|---|---|
| ✅ | Navigation without waypoints | Generated navmesh (`p_nav_generate`) |
| ✅ | Walk around buildings | Also buildings placed during the match |
| ✅ | Get unstuck | Notice when stuck, back off, take another route |
| 🟡 | Doors and gates | Some doors and buttons work; crossroads base gates and ramps remain a problem |
| 🟡 | All maps | Runs exist on 7 maps; three additional installed maps have navmeshes but no recorded run yet. Doors and sliding gates (`func_movelinear`) are in the mesh and blocked for routing while shut |

### Commander

| | Feature | Notes |
|:-:|---|---|
| ✅ | Take the command vehicle | When no human commands; pulls the CV back to a repair pad or open ground away from danger when an enemy vehicle closes in or under heavy fire |
| 🟡 | Prioritize the commander candidate | Avoids starting refinery turrets while taking the seat; match testing continues |
| 🟡 | Hand over to a human | A human who gets in or uses the CV gets the seat; not tested with a real player yet |
| ✅ | Build the base | Refineries, barracks, vehicle factory, armory, repair station in a fixed order |
| ✅ | Assign engineers | Sends free engineers to unfinished or damaged buildings. An engineer that neither moves nor raises the building's health for 20 s tries another side; after the third stall it gives the job back |
| ✅ | Leave the CV to build | The commander is an engineer: gets out, builds, gets back in |
| 🟡 | Drive the CV | Drives to distant buildings and enemy infantry. Checks shortcuts for buildings and backs up to line up with a detour when needed. A drive that makes no headway for 25 s ends, and the commander builds from where the CV stands if in range |
| ✅ | Place the vehicle factory | Exit faces along the route towards the enemy. Before the first base building a fine ground scan reserves a free factory spot near the CV (single exact spots in cramped bases, e.g. Palmbay BE), other buildings keep 1500 units off it; without one the CV drives to a remote site |
| 🟡 | Research | Radar, then Structural Engineering and Vehicle Factory before the factory is built; after the factory (from 350 resources) turret level 2, Mechanical Engineering, APC spawn, chassis, armour and engine research, then level 3 and heavy tanks (live-verified through Upgraded Chassis without cheat research) |
| 🟡 | Expand the base forward | A factory far from the own barracks (more than 3500 units) gets a barracks beside it (seen on Palmbay); spawning there and further forward buildings are open |
| ⬜ | Adapt to the game | Choose resource points by value, react to lost buildings |
| 🟡 | Give orders to bots | The seated bot commander sends its free attackers (no engineers, defenders or bots with another order) to the next open capture point with an *attack location* order, re-targets when it is taken and releases them after 120 s without progress (`rcbot_empires_commander_orders`). Measured on palmbay and DMZ: orders arrive and bots walk there; no capture measured yet |

### Engineers

| | Feature | Notes |
|:-:|---|---|
| ✅ | Build and repair buildings | |
| ✅ | Repair vehicles | Heals the damaged armor side |
| ✅ | Revive teammates | Dead bots wait a moment for a nearby engineer |
| 🟡 | Turrets at refineries | MG turret construction works; unreachable Rainbowroad stand points are rejected and rotated, while rare placement failures remain |
| ✅ | Skills | Repair and revive split across the team's engineers |
| ✅ | Repair vehicles in the base | EngiBots repair standing damaged own vehicles near the CV without a repair call, preferring those at the repair pad |
| ✅ | Turret upgrades | An engineer with the turret-upgrade skill (rank 30+) upgrades its own finished turrets up to the level the map allows (level 3, or less where `emp_eng_restrict` brushes or the map's `emp_info_params` restrict turrets) and builds them up again; `DEV_TURRET_UPGRADE=1 scripts/dev_turret.sh`. Not yet seen in a full match |
| ⬜ | Walls, cameras, radar | |
| 🟡 | EngiBots (`/buildengibot`) | The commander types `/buildengibot` (or `!buildengibot`) in chat and gets an engineer bot for team resources. It stays near the team CV, builds and repairs the team's buildings there, never captures, roams or takes a vehicle, and builds no turrets of its own. An "attack location" order makes it build a turret there. Limit per team, removed on map change, not kicked by the bot quota |

#### How EngiBots work

1. **Buy one.** While you sit in the CV as commander, type `/buildengibot` or `!buildengibot` in chat, or
   `buildengibot` in the console (works even when a chat plugin hides the line; `bind <key> buildengibot`). The
   plugin checks the request, takes `rcbot_empires_engibot_cost` (default 100) from the team resources and adds
   an engineer bot named `EngiBot NF 1`, `EngiBot BE 2` and so on. A message on your screen gives the result:
   - `EngiBot ready (1/2), -100 resources`
   - `Only the commander can build an EngiBot`: you are not in commander mode.
   - `EngiBot limit reached (2/2)`: the team already has `rcbot_empires_engibot_limit` EngiBots.
   - `Not enough resources for an EngiBot (100 needed)`
   - `Server is full, no slot for an EngiBot`
   - `EngiBots are disabled on this server`: `rcbot_empires_engibot 0`.

   If paying fails after the bot was created, the bot is kicked again. You never get one for free.
2. **What it does.** Every 2 seconds an EngiBot looks for the nearest damaged or unfinished building of its team
   within `rcbot_empires_engibot_leash` (default 2500) of the team CV. It goes there and builds or repairs it.
   With two EngiBots, each one takes a different building. Buildings you place yourself count, so you can drop a
   barracks next to them and they finish it. With no CV, the base point is where the EngiBot first spawned.
3. **When there is nothing to do,** it walks back to about 900 units from the CV. Each EngiBot waits on its own
   side, so they do not stand on the commander's building spots.
4. **Vehicles in the base.** A damaged own vehicle that stands within the leash (preferably at the repair pad)
   gets repaired, even without a repair call and with `rcbot_empires_vehicles 0`.
5. **Turrets on your order.** Select an EngiBot and give it an *attack location* order. It walks there, places
   an MG turret facing the enemy (turns and retries if the spot is blocked) and builds it to full health. Your
   screen shows `EngiBot NF 1: building a turret`, then `turret ready`, or why not: `turret limit reached`,
   `no room for a turret here`, `cannot reach that spot for a turret`. While an order stands (attack location,
   guard, move) the EngiBot stays there instead of walking back to the CV. A *guard* order on a building makes
   it build or repair that building.
6. **What it never does.** It does not chase enemies (it shoots from where it is), answer revive calls, or answer
   repair and ammo calls of vehicles outside its base area (inside it, it repairs them and drops ammo crates). It
   does not capture flags, join squads, roam, build turrets without your order, enter vehicles or take the
   commander seat. Your orders go first: a *guard* on a building replaces the job it is on, and an engineer under
   your order only answers repair/ammo calls near the order spot or the guarded building. With two EngiBots, each
   takes a different building when there is more than one to build.
   EngiBots have the repair upgrade active from their first spawn (the first perk is free at 0 points).
7. **When it goes away.** On a map change all EngiBots are kicked. If one leaves or changes team (autobalance),
   its slot is free and the commander can buy a new one. The bot quota does not count or kick EngiBots, and bots
   that fill the quota later do not become EngiBots.

Not done yet: a test with a human commander (with SourceMod, if `/buildengibot` does nothing, use
`!buildengibot` or the console command `buildengibot`).
A server crash on `!buildengibot` with SourceMod (community report) is fixed: the bot is now added in the next
server frame instead of inside the chat event. So far everything was measured
with a bot commander (`scripts/dev_engibot.sh`).

### Infantry

| | Feature | Notes |
|:-:|---|---|
| ✅ | Classes | Scout, Rifleman, Grenadier, Engineer with several loadouts per class (e.g. rifleman stickies, scout sniper or shotgun) and class skills for all four |
| ✅ | Fight | Aim, shoot, pick targets; engagement ranges per weapon (pistols ~1000, shotguns 700, SMGs ~1700, rifles ~3000, snipers up to 4500). Keeping class-typical distance and using cover are open |
| 🟡 | Resupply | Engineer resupply exists; ammo requests and coverage for other classes remain open |
| ✅ | Turret fire | Retreat out of a turret's range |
| ✅ | Capture flags | On maps with capture points |
| ✅ | Follow move orders | Move, abort and attack location from the commander; tested through the game's own order commands, not yet with a real human. A move ends within the game's 100 units (route extended straight to the spot when the mesh ends beside it) |
| 🟡 | Destroy turrets and buildings | Grenadiers attack turrets, enemy buildings and the CV with RPG and mortar from outside the building; some map/state combinations still find no firing position. Scouts sabotage enemy buildings (confirmed live); riflemen use sticky grenades (up to 420 units, not yet confirmed in a full match) |
| 🟡 | Follow guard and attack-target orders | Infantry follow the guarded unit or the attack target; engineers build or repair the guarded building (also refineries). Attack target on an enemy is accepted by the game only from a human commander's view, so it was tested with a test hook; a test with a real human is open |
| 🟡 | Water | Bots come up for air after 5 s with the head under water and avoid deep water when there is a way over land. Flags reachable only by ladders from the water (palmbay boats) are skipped for now |
| ✅ | Team join | Bots join a team without the join suicide (marked dead before `jointeam`, as the game does for humans) |
| 🟡 | Squads | Bots join and follow squads; coordinated vehicle passengers and roles are planned |
| 🟡 | Mines, scout binoculars | Implemented, rarely used in matches |
| ⬜ | Seismic grenades | Engineers against turrets |

### Vehicles

| | Feature | Notes |
|:-:|---|---|
| ✅ | Buy at the vehicle factory | Also without debug purchase (the buyer stands under the factory roof). Buyers far from the factory get a walk limit by distance that extends while they move. Empty own vehicles blocking the factory bay or exit ramp are adopted and driven out instead of failing a purchase |
| 🟡 | Better parts after research | Chassis, weapons, armour and engine are chosen by the team's research and budget, at purchase and pad refit (heavy tanks with composite armour and gas turbine seen with all research unlocked); not yet seen after the commander's own research in a match |
| ✅ | Drive | Follows the navmesh, makes three-point turns and replans when a reverse turn stalls. Avoids edges and fords only as deep as the vehicle's body centre stays dry (the game stops the engine and deals 20 HP/s once it is wet) |
| 🟡 | Fight | Leads targets and circles enemies; holds position and fires at static buildings from 550–1500 units; turns the side armour only towards armed targets; MG only against infantry (up to 1200, only with MG ammo left), cannon against vehicles and buildings; buildings block their sight; a seated enemy commander is no target. Close range to buildings and group switching still need work |
| 🟡 | Pick goals | Defend own CV/refineries against tanks, hunt spotted tanks, support the front from outside the flag, push on enemy buildings, then the CV; confirmed in 10-bot Coast and Crossroads matches, Palmbay pending |
| ✅ | Run over infantry | |
| ✅ | Retreat and repair | Back to the repair station when damaged; without one, parks at a free spot next to the own CV |
| 🟡 | Rearm | Light tank/AFV with empty cannon/missiles drives to the nearest source: a team ammo crate (engineer or map crate) or the own repair pad, where it refits (refills every weapon); far from both it calls for ammo and an engineer places a supply crate |
| 🟡 | Re-customize on the repair pad | Seen in 22-bot matches after a purchase at the factory; not yet confirmed on a server without `emp_sv_debug_build_vehicles 1`. Same loadout is free (the game charges only the difference). A light tank/AFV whose MG shares a fire button with another weapon drives to the pad and refits the bot loadout |
| 🟡 | Bill Nye mode | One scout jeep rams enemy vehicles off the road (on `emp_rainbowroad`); still falls off itself sometimes |

## Download

Ready-made builds (32-bit, Source SDK 2013) are made by GitHub Actions:

- **[Releases](../../releases)**: `latest` always holds the newest build of `main`; tagged
  versions (`v*`) get their own release. Download `rcbot2-empires-linux-x86.zip` or
  `rcbot2-empires-windows-x86.zip` and unzip it into the server's `empires/` folder.
- **[Actions](../../actions/workflows/build.yml)**: every push and pull request; the build
  artifacts are attached to each run (GitHub login required).

The Windows build is untested in game. Features that read fixed offsets from the game binary only
know the Linux `server_srv.so` and use their fallbacks on Windows; do not set
`rcbot_empires_force_offsets 1` there. What is affected and how to port it:
[docs/WINDOWS.md](docs/WINDOWS.md).

## Install

1. Install [Metamod:Source][mms] on the Empires dedicated server.
2. Download a build (see [Download](#download)) or build the plugin yourself (see [Build](#build)).
3. Install the built plugin at `empires/addons/rcbot2/bin/rcbot.2.sdk2013.so` on the server, with
   `empires/addons/metamod/rcbot2.vdf`:

   ```
   "Metamod Plugin"
   {
   	"alias"		"rcbot2"
   	"file"		"addons/rcbot2/bin/RCBot2Meta"
   	"rcbot2path"	"addons/rcbot2/"
   }
   ```

   If you load the plugin by hand (`meta load`), a vdf with only `"rcbot2path"` (no `"file"`) still tells
   RCBot2 where its config lives, and Metamod ignores it. Without `rcbot2path` the plugin now falls back to
   `addons/rcbot2` and says so in the log.
4. Every map needs a navmesh (`empires/maps/<map>.nav`). If it is missing, load the
   map and run `sv_cheats 1; p_nav_generate` once (takes up to a minute).
5. Add bots from the server console:

   ```
   rcbotd addbot <class> <team> <name>
   ```

   Team 2 = Northern Faction, 3 = Brenodi Empire. Class 0 Scout, 1 Rifleman,
   2 Grenadier, 3 Engineer.

6. To fill the server automatically, and to switch the bots off once enough humans play, edit
   `addons/rcbot2/config/bot_quota.ini` (one line per human count, e.g. `Humans "12" => Bots "0"`)
   and leave `rcbot_bot_quota_interval` above 0 (default 10 s). EngiBots bought by a commander are not counted or kicked.

### Settings

| Cvar | Default | Meaning |
|---|---|---|
| `rcbot_empires_auto_commander` | 1 | A bot takes the command vehicle if no human does |
| `rcbot_empires_commander_engineer` | 1 | The bot commander is an engineer and builds near the CV itself |
| `rcbot_empires_commander_orders` | 1 | The seated bot commander sends its free attacker bots (no engineers, no defenders, none with another order) to the next capture point with an attack location order |
| `rcbot_empires_commander_drive_from` | 1000 | The commander drives the CV to buildings farther away than this |
| `rcbot_empires_commander_boomtank` | 1 | Commander runs over enemies in the base: 0 off, 1 on except on rainbowroad maps, 2 everywhere |
| `rcbot_empires_engineer_turrets` | 1 | Engineers build MG turrets at the team's refineries |
| `rcbot_empires_engibot` | 1 | The commander can buy EngiBots with `/buildengibot` in chat |
| `rcbot_empires_engibot_cost` | 100 | Team resources per EngiBot |
| `rcbot_empires_engibot_limit` | 2 | EngiBots per team (0–8) |
| `rcbot_empires_engibot_leash` | 2500 | EngiBots work within this distance of their team CV |
| `rcbot_empires_engineer_help_human` | 2 | With a human commander, up to this many engineer bots per team build and repair unfinished or damaged buildings near the CV on their own (0 = only on a guard order) |
| `rcbot_empires_vehicles` | 1 | Bots buy, drive and fight with vehicles |
| `rcbot_empires_vehicle_drivers` | -1 | Max bots per team in vehicles (-1 = 40 % of team bots, minimum 2; Bill Nye excluded) |
| `rcbot_empires_billnye` | -1 | Bill Nye mode: -1 = NF on rainbowroad maps, 0 off, 2 NF, 3 BE |
| `rcbot_empires_debug_log` | 0 | 1 = print detailed bot diagnostics; startup and registry messages remain visible |
| `rcbot_empires_force_offsets` | 0 | Checks `server_srv.so`; on an unknown build, fixed-offset reads are disabled and affected features use fallbacks. 1 = use the offsets anyway |

Detailed diagnostics default to off to keep ordinary server logs readable. The development
test harness enables them automatically; set `rcbot_empires_debug_log 1` for manual diagnosis.
Run Empires test and diagnostic commands such as `rcbotd empires_nav_scan` from the
dedicated server console. RCBot2 loads a map's navmesh only while at least one bot is on the server:
with zero bots `empires_nav_scan` prints `scan route=navmesh-not-loaded`. Add a bot first
(`rcbotd addbot 1 2 Scanner`) and wait a few seconds.

## Build

Needs Git, Python 3 with [AMBuild][], GCC with 32-bit support, and SDK2013,
Metamod, SourceMod and the HL2SDK manifests. For a local sibling dependency checkout, a clean build is:

```sh
SDK_ROOT="../rcbot2-clean/alliedmodders" \
MANIFEST_DIR="../rcbot2-clean/hl2sdk-manifests" scripts/build_empires.sh
```

Relative dependency paths are resolved from the directory where you invoke the script.
The script prints the resulting `.so` path (by default
`/tmp/rcbot2-build-output-v2/rcbot.2.sdk2013.so`). For iterative development,
`scripts/dev_build.sh` uses a cache and the sibling `rcbot2-clean` checkout;
`RCBOT_DEPS_DIR` overrides that dependency location. Install the plugin package
and Metamod configuration on the server before replacing the binary.

The `alliedmodders/metamod-source` checkout decides the Metamod version the plugin needs. Metamod 2.0 headers
build a plugin that Metamod 1.12 refuses (`Plugin requires newer Metamod version (17 > 16)`); for a 1.12
server, check out the `1.12-dev` branch there before building (reported working by a community server).

## Known issues


- The Bill Nye jeep sometimes drives off edges on `emp_rainbowroad`.
- Turret placement at map-owned refineries on `emp_rainbowroad` is still intermittent; unreachable stand points are skipped and retried at another angle.
- Infantry can get stuck inside barracks or vehicles; some rainbowroad routes fail.
- Vehicles can become stuck near the repair pad or in the BE base; crossroads gates and ramps need work.
- On `emp_palmbay` BE builds its vehicle factory in the base; NF (no free spot near its CV) drives to a remote site and builds a forward barracks there. NF commander entry can occasionally stall at spawn.
- On `emp_palmbay` NF tanks sometimes get stuck in the palm cluster on the way to the front and leave the tank; on `emp_crossroads` a concrete wall near (3800 5000) stops tanks.
- On `emp_coast` engineers find no stand point at the refineries, so they stay unfinished and income is lower.
- Only Linux servers are tested.

Planned work includes coordinated squad passengers, jeep attacks and refinery defenses.

## Development

- `scripts/dev.sh <map> <bots> <minutes>` builds, deploys to an isolated test server
  and prints a summary of the match. Set `EMPIRES_TEST_SERVER` (or put it in
  `scripts/dev.local`) to that server. The addon must already be installed there;
  the script replaces its binary. Use `DEV_UNTIL` to end a targeted run on a result.
  Use `DEV_EXPECT` to require a success line and `DEV_FAIL` to end on a failure
  line. Missing prerequisites and failed checks return a nonzero exit status.
  Each run keeps its own `server.log`.
  Each run folder gets a `build-info.txt` with the git SHA and whether the tree was dirty.
- `scripts/dev_check.sh` runs the commander chain, long-move, repair-pad, turret-build and
  revive checks in parallel, one server per test on its own port (about 1 minute;
  `DEV_CHECK_SERIAL=1` runs them one after another). The long-move check fails if the bot only arrives after retries.
  The repair-pad check disables refinery turret jobs so the commander can place the pad first.
  `scripts/dev_maps.sh` also builds once for the whole batch. Both wrappers
  accept `DEV_SKIP_BUILD=1` to reuse a build and `DEV_BUILD_HASH` to verify the
  selected binary. Build and server startup use `flock` (util-linux).
- `scripts/dev_orders.sh [map] [timescale]` gives commander orders like the game UI (move, attack
  target on an enemy, guard on a refinery); `scripts/dev_cmdorders.sh [map]` checks the bot
  commander's own orders (map with a CV and capture points, e.g. emp_palmbay).
- `scripts/dev_navgen.sh <map>` generates a map's navmesh on the test server (`-nowatchdog`;
  copy the map's files listed in `maps/<map>.res` first).
- `scripts/dev_econ.sh [map] [game-minutes]` plays with production economy values (no debug
  vehicle purchase) and ends when a bot drives a vehicle it bought.
- Bots hand their wages to a teammate who writes "wages" in chat (`!give #<userid>` when the
  server has a give plugin, otherwise directly).
- `scripts/dev_turn.sh <1|2|3|4> [map] [nf|be]` samples the driving response
  after an in-place debug purchase of an NF Light (default) or a BE AFV.
  Mode 1 uses steering alone. Mode 2 adds a
  0.2-second handbrake pulse. Modes 3 and 4 measure straight-line coasting and
  braking from at least 500 HU/s. The `vehicle turn-test` log records pose and
  velocity at roughly 0.1-second intervals. The underlying
  `rcbot_empires_vehicle_turn_test` cvar defaults to 0 (disabled).
- `rcbot_empires_vehicle_curve_speed 1` enables the experimental route-based
  speed limit for NF Light and BE AFV drivers. It previews up to three corners
  using the existing route. The default is 0 while fixed-start tests are pending.
- `scripts/dev_quick.sh [map] [minutes]` is a short targeted run instead of a full match: 3 bots,
  instant vehicle purchase, timescale 1 (tank shells and mortars explode at once with fast-forward);
  adapt it with `QUICK_ROSTER`, `QUICK_PRE`, `QUICK_CMDS`, `QUICK_UNTIL` and `QUICK_SHOW`.
  `QUICK_FAIR=1` starts both teams' tanks at the same time (auto commander off).
- `scripts/dev_buy.sh [map] [timescale]` is the short version (~30 s): instant factory, extra
  money, ends at the first real purchase; for purchase diagnostics, not for economy timing.
- `scripts/dev_vf_drive.sh [0|1|place] [map] [nf|be] ["x y z"]` uses an instantly built factory
  as the first building. A bot buys normally and drives for 25 seconds after
  the first successful route.
  The test runs at normal game speed. Use 0 or 1 to compare the experimental
  curve speed cap, which defaults to off during normal play.
  Supply build-command coordinates to place the factory at yaw 0, or use `place` to stop
  at the first automatic placement result.
  Set `DEV_VF_APPROACH="x y z"` with a fixed factory to start the buyer near
  the ramp and keep the normal purchase and exit sequence.
- For experimental local maneuver selection on ordinary NF Light and BE AFV
  routes, set `rcbot_empires_vehicle_maneuvers 1`. The default is 0.
  `rcbot_empires_vehicle_maneuver_budget` (default 2) caps planner evaluations
  per server tick across all vehicles; `vehicle maneuver-perf` logs the cost.
- `scripts/dev_vehicle_obstacle.sh [0|1] [map] [nf|be] [ahead] [lateral] [model]`
  places a solid prop on a debug-bought driver's route
  (`rcbotd empires_vehicle_obstacle [ahead] [lateral] [entry_speed] [model]`)
  and logs how the driver gets past it.
  The planner searches for alternatives when the regular arc hits a blocking
  entity or during recovery from a maneuver stall. Regular steering handles
  world geometry and ground clearance. After a maneuver retry, it releases the
  committed detour so it can replan. Focused Crossroads and Palmbay obstacle
  runs passed in 1.86–2.05 s without stopping; factory-drive speed on Crossroads
  stayed within 6% of selection-off results. In the Crossroads matches all three
  NF Light purchases at the same factory timed out at the straight exit before
  maneuver selection began. The initial short NF factory test used a different site.
  With selection enabled, both drivers later gave up at factory entities.
  Palmbay's native site checks found no valid factory candidate, so no vehicles
  were bought. At the same Crossroads factory pose, the empty bay cleared in
  2.6 seconds, and adding the NF radar reproduced the stall. `vehicle factory-exit hull`
  recorded contact with the radar and then its vehicle collision proxy. Automatic
  factory heading selection now checks for existing buildings with a tank-width
  sweep. With the radar present, the selected 90° heading cleared in 1.8 seconds
  and the NF Light continued on its route. The BE control cleared in 2.6 seconds
  at −96°, and `dev_check` passed 5/5. On Palmbay the first short placement
  attempt failed for NF and succeeded for BE. See
  [open work](docs/OPEN_WORK.md) for the existing baselines. The default is 0.
- `scripts/run_*_tests.sh` are focused tests; some use test-server binaries or dependencies.
- `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts -p dev_run_test.py`
  checks the runner's log matching and exit status offline.
- `rcbot_analysis_start` records what the bots do: [EMPIRES_ANALYSIS_RECORDER.md](EMPIRES_ANALYSIS_RECORDER.md).
- What we know about the game server's internals: [reverse-engineering/](reverse-engineering/).
- Setup: copy `scripts/dev.local.example` to `scripts/dev.local`. Build dependencies (SDK 2013,
  Metamod:Source, SourceMod, HL2SDK manifests) go in `../rcbot2-clean` or `RCBOT_DEPS_DIR`.
- Working with Claude Code or Codex: see [AGENTS.md](AGENTS.md).
  Claude Code reads it through `CLAUDE.md`. One agent changes, builds and tests itself;
  topics without measured gain are timeboxed and parked.
  See the [open-work priorities](docs/OPEN_WORK.md).

## License

[AGPL-3.0](LICENSE), like RCBot2: if you run a modified version on a server, its source
code must be available to the players. `rcbot/logging.{h,cpp}` is released separately
under the [BSD Zero Clause License](https://spdx.org/licenses/0BSD.html).

## Credits

Based on [RCBot2][rcbot2] by Cheeseh and its [fork by APGRoboCop][rcbot2-fork]:


- Founder - Cheeseh
- Bot base code - Botman's HPB Template
- Linux Conversion and Waypointing - [APG]RoboCop[CL]
- TF2 support and enhancements - Ducky1231/Pongo
- SourceMod and AMBuild support - nosoop
- Synergy, TF2, MvM, CSS and AMBuild support - Anonymous Player/caxanga334
- TF2 Classified support - Technochips
- TF2 additional gamemodes support - RussiaTails
- Linux Black Mesa and SDK2013 mathlib fix - Sappho
- Dystopia support - Soft As Hell
- DoDS and main config support fixes - DNA.styx
- Major waypointer for TF2 - LK777, RussiaTails, Witch, Francis, RevilleAJ, Waffle033
- Major waypointer for DoDS - INsane, Gamelarg05, genmac
- Miniz compression - Rich Geldreich and Tenacious Software LLC


[empires]: https://empiresmod.com/
[rcbot2]: http://rcbot.bots-united.com/
[rcbot2-fork]: https://github.com/APGRoboCop/rcbot2
[mms]: https://www.sourcemm.net/
[AMBuild]: https://wiki.alliedmods.net/AMBuild

For spatial navigation debugging, `tools/` holds offline helpers (Python 3 with Pillow, they only read the `.nav`/`.bsp` files):

- `tools/nav_overview.py <map>.nav --bsp <map>.bsp --output out.png` renders a top-down PNG of the navmesh: connected components, commanders, barracks, refineries and resource points.
  `--crop x0,y0,x1,y1` zooms to a world rectangle, `--terrain` shades the displacement terrain below the mesh (amber 30-45 deg and red above 45 deg slope), `--vehicle-width 284` tints mesh a vehicle of that width cannot pass, `--props` draws static props (palms, rocks; solid ones white-ringed, from the BSP game lump), and `--log server.log [--userid N]` marks stuck, reverse and route-block events and draws that vehicle's driven track.
- `tools/check_nav.py <map>.nav --bsp <map>.bsp` prints component sizes and whether every spawn, commander, building and resource point is on the main component.
- `tools/test_nav_tools.py` checks the terrain reader against the navmesh of `emp_palmbay`.

The picture does not include static props or dynamic building obstacles, and terrain is only shown for displacement maps (walls of brush maps are not drawn). It does not replace a clearance trace in the game.
