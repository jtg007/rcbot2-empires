# RCBot2 for Empires

**Bots for the Source mod [Empires][empires]**, the mix of first-person shooter and
real-time strategy. Built on [RCBot2][rcbot2-fork] for Source SDK 2013 (x86), tested on Linux
dedicated servers.

The bots play whole matches: a bot commander builds the base and gives orders, engineers build
and repair, infantry capture and attack, drivers buy, drive and fight with vehicles. No
hand-made waypoints are needed, the bots use generated navmeshes.

> [!WARNING]
> **Work in progress.** The bots still make mistakes. See [what works](#what-the-bots-can-do),
> [known issues](#known-issues) and the [open-work list](docs/OPEN_WORK.md).

**Contents:** [Quick start](#quick-start) · [What the bots can do](#what-the-bots-can-do) ·
[EngiBots](#engibots-for-human-commanders) · [Settings](#settings) · [Known issues](#known-issues) ·
[Build](#build) · [Development](#development) · [License](#license) · [Credits](#credits)

## Quick start

1. Install [Metamod:Source][mms] on the Empires dedicated server.
2. Download `rcbot2-empires-linux-x86.zip` (or `-windows-x86.zip`) from
   [Releases](../../releases) (`latest` = newest build of `main`) and unzip it into the server's
   `empires/` folder. Or [build it yourself](#build).
3. Check that `empires/addons/metamod/rcbot2.vdf` exists:

   ```
   "Metamod Plugin"
   {
   	"alias"		"rcbot2"
   	"file"		"addons/rcbot2/bin/RCBot2Meta"
   	"rcbot2path"	"addons/rcbot2/"
   }
   ```

4. Every map needs a navmesh (`empires/maps/<map>.nav`). If it is missing, load the map and run
   `sv_cheats 1; p_nav_generate` once (up to a minute).
5. Add bots from the server console: `rcbotd addbot <class> <team> <name>`
   (team 2 = Northern Faction, 3 = Brenodi Empire; class 0 Scout, 1 Rifleman, 2 Grenadier, 3 Engineer).
   To fill the server automatically and remove bots as humans join, edit
   `addons/rcbot2/config/bot_quota.ini` (e.g. `Humans "12" => Bots "0"`).

Notes:
- The Windows build is untested in game; features that read fixed offsets of the Linux game binary
  use fallbacks there ([docs/WINDOWS.md](docs/WINDOWS.md)).
- A plugin built against Metamod 2.0 headers is refused by Metamod 1.12; see [Build](#build).

## What the bots can do

✅ works in matches · 🟡 partly / not reliable yet · ⬜ planned

### Movement

| | Feature | Notes |
|:-:|---|---|
| ✅ | Navigation without waypoints | Generated navmesh; routes around buildings placed during the match |
| ✅ | Get unstuck | Notices a stall, backs off, tries another route; a blocked link next to a building is avoided for 60 s |
| ✅ | Move orders | Ends within the game's 100 units; for targets beside or above the mesh the bot stops at the nearest reachable point on that level |
| 🟡 | Doors and gates | Some doors and buttons work; Crossroads base gates and ramps remain a problem |
| 🟡 | Maps | Runs on 7 maps; more installed maps have navmeshes but no recorded run |
| 🟡 | Water | Surfaces for air, avoids deep water when there is a land route; flags reachable only by ladders from the water are skipped |

### Commander

| | Feature | Notes |
|:-:|---|---|
| ✅ | Take the command vehicle | When no human commands |
| 🟡 | Hand over to a human | A human who gets in or uses the CV gets the seat (not yet tested with a real player) |
| ✅ | Build the base | Refineries, barracks, vehicle factory, armory, repair pad; the commander is an engineer and gets out to build |
| ✅ | Assign engineers | Sends free engineers to unfinished or damaged buildings; a stalled engineer tries another side, then gives the job back |
| ✅ | Place the vehicle factory | Reserves a free spot near the CV with the exit facing the enemy; otherwise drives to a remote site |
| ✅ | Keep the CV alive | Pulls back from nearby enemy vehicles or heavy fire |
| 🟡 | Drive the CV | Drives to distant building sites; gives up after 25 s without progress and builds from where it stands |
| 🟡 | Research | Radar, Structural Engineering and Vehicle Factory first, then turrets, APC spawn, chassis, armour, engines and heavy tanks |
| 🟡 | Expand and defend | Forward barracks near a remote factory and at the front (own half only), then up to two MG and two ML base turrets |
| 🟡 | Give orders to bots | A *team director* per team splits free infantry into push / hold / defense groups (~50/30/20) and sends them by *attack location* order to open flags, enemy refineries or threatened own buildings. Human orders are never touched |
| ⬜ | Adapt to the game | Choose resource points by value, react to lost buildings |

### Engineers

| | Feature | Notes |
|:-:|---|---|
| ✅ | Build and repair | Buildings and vehicles (the damaged armour side) |
| ✅ | Revive | Dead bots wait a moment for a nearby engineer |
| ✅ | Skills | Repair and revive are split across the team's engineers |
| 🟡 | Turret upgrades | Rank 30+ engineers upgrade their own turrets up to the level the map allows (works in a test, not yet seen in a full match) |
| 🟡 | Turrets at refineries | MG turrets work; rare placement failures remain |
| 🟡 | EngiBots | Engineer bots a human commander can buy, see [below](#engibots-for-human-commanders) |
| ⬜ | Walls, cameras, radar | |

### Infantry

| | Feature | Notes |
|:-:|---|---|
| ✅ | Classes | All four classes with several loadouts and class skills |
| ✅ | Fight | Picks targets, uses each weapon at its range (pistols ~1000 up to snipers 4500); cover and class-typical distance are open |
| ✅ | Capture flags | On maps with capture points |
| ✅ | Turrets | Retreat out of an enemy turret's range |
| 🟡 | Pick a spawn | By situation: near a threatened building on a base alert, at the factory for vehicle buyers, at a friendly APC or forward barracks near the front; gets out of a spawned-in APC, also when the exit is blocked |
| 🟡 | Tactical respawn | A defender far from an alarm or a buyer far from the factory may respawn closer (`rcbot_empires_kill_spawn`) |
| 🟡 | Attack buildings | Grenadiers use RPG and mortar, riflemen sticky grenades (confirmed on an enemy refinery), scouts sabotage (confirmed) |
| 🟡 | Fight vehicles | Riflemen stick grenades on vehicles, scouts walk up to enemy vehicles with the stun grenade (both not yet confirmed in a match) |
| 🟡 | Guard and attack-target orders | Infantry follow the guarded unit; engineers build or repair a guarded building (not yet tested with a human) |
| 🟡 | Squads, mines, binoculars | Implemented, rarely used |
| ⬜ | Seismic grenades | Engineers against turrets |

### Vehicles

| | Feature | Notes |
|:-:|---|---|
| ✅ | Buy | At the vehicle factory, also on map-built factories; empty vehicles blocking the bay are driven out |
| ✅ | Better parts after research | Chassis, weapons, armour and engine by research and budget; with APC Spawn researched each team keeps one APC at the front as a mobile spawn |
| ✅ | Drive | Follows the navmesh, three-point turns, avoids edges and water too deep for the vehicle |
| ✅ | Retreat and repair | Back to the repair pad when damaged, otherwise next to the own CV |
| ✅ | Run over infantry | |
| 🟡 | Fight | Cannon leads moving targets, the MG aims straight at infantry (it is hitscan, up to 1200); stands and fires at buildings from 550–1500; with a weakened side it turns its strongest side towards armed targets |
| 🟡 | Pick goals | Defend own base → hunt spotted tanks → support the front → push enemy buildings → enemy CV |
| 🟡 | Rearm | Drives to an ammo crate or the repair pad, or calls an engineer for a crate |
| 🟡 | Bill Nye mode | A scout jeep rams enemy vehicles off the road on `emp_rainbowroad`; sometimes falls off itself |

## EngiBots for human commanders

As commander, type `/buildengibot` or `!buildengibot` in chat (or `buildengibot` in the console) to buy
an engineer bot for `rcbot_empires_engibot_cost` team resources (default 100, limit 2 per team). The
screen shows the result (`EngiBot ready (1/2)`, or why not: not commander, limit, resources, server full).

- It builds and repairs the team's damaged or unfinished buildings within 2500 units of the CV,
  including the ones you place, and repairs own vehicles standing there.
- With nothing to do it waits about 900 units from the CV, out of the way of building spots.
- An *attack location* order makes it build an MG turret there; a *guard* order on a building makes it
  build or repair that building.
- It never captures, roams, enters vehicles, takes the commander seat or chases enemies.
- All EngiBots leave on map change; the bot quota does not count or kick them.

Not yet tested with a human commander (only with the bot commander, `scripts/dev_engibot.sh`).

## Settings

| Cvar | Default | Meaning |
|---|---|---|
| `rcbot_empires_auto_commander` | 1 | A bot takes the command vehicle if no human does |
| `rcbot_empires_commander_engineer` | 1 | The bot commander is an engineer and builds near the CV |
| `rcbot_empires_commander_orders` | 1 | The bot commander gives its free infantry team-director orders |
| `rcbot_empires_commander_drive_from` | 1000 | The commander drives the CV to buildings farther away than this |
| `rcbot_empires_commander_boomtank` | 1 | Commander runs over enemies in the base: 0 off, 1 on except on rainbowroad maps, 2 everywhere |
| `rcbot_empires_kill_spawn` | 1 | Tactical respawn closer to the goal (≥ 3800 units closer, 6 s out of combat, tickets > 20 and > 25 %, at most every 90 s per bot) |
| `rcbot_empires_engineer_turrets` | 1 | Engineers build MG turrets at the team's refineries |
| `rcbot_empires_engineer_help_human` | 2 | With a human commander, this many engineer bots per team build and repair near the CV on their own |
| `rcbot_empires_engibot` | 1 | Commanders can buy EngiBots |
| `rcbot_empires_engibot_cost` | 100 | Team resources per EngiBot |
| `rcbot_empires_engibot_limit` | 2 | EngiBots per team (0–8) |
| `rcbot_empires_engibot_leash` | 2500 | EngiBots work within this distance of the team CV |
| `rcbot_empires_vehicles` | 1 | Bots buy, drive and fight with vehicles |
| `rcbot_empires_vehicle_drivers` | -1 | Max bots per team in vehicles (-1 = 40 % of team bots, at least 2) |
| `rcbot_empires_billnye` | -1 | Bill Nye mode: -1 = NF on rainbowroad maps, 0 off, 2 NF, 3 BE |
| `rcbot_empires_debug_log` | 0 | 1 = detailed bot diagnostics in the server log |
| `rcbot_empires_force_offsets` | 0 | On an unknown `server_srv.so` build, fixed-offset reads are disabled; 1 = use them anyway (never on Windows) |

Bots hand their wages to a teammate who writes "wages" in chat.

## Known issues

- Infantry can get stuck inside barracks or vehicles; some Rainbowroad routes fail.
- Vehicles can get stuck near the repair pad, in the BE base, at Crossroads gates and
  ramps and at a wall near (3800 5000) on Crossroads; on `emp_palmbay` NF tanks sometimes get stuck in
  the palm cluster.
- `emp_palmbay`: both flags sit on boats without navmesh, so bots play it like a map without flags.
  NF finds no factory spot near its CV and builds a remote one; NF commander entry can stall at spawn.
- `emp_coast`: engineers find no stand point at the refineries, so they stay unfinished.
- Refinery turret placement on `emp_rainbowroad` is still intermittent.
- The Bill Nye jeep sometimes drives off edges on `emp_rainbowroad`.
- A bot whose vehicle exit is blocked on all sides stays seated and retries every ~15 s.
- Only Linux servers are tested.

## Build

Needs Git, Python 3 with [AMBuild][], GCC with 32-bit support, and SDK 2013, Metamod:Source,
SourceMod and the HL2SDK manifests (e.g. in a sibling `../rcbot2-clean` checkout):

```sh
SDK_ROOT="../rcbot2-clean/alliedmodders" \
MANIFEST_DIR="../rcbot2-clean/hl2sdk-manifests" scripts/build_empires.sh
```

The script prints the resulting `.so` path. The `metamod-source` checkout decides which Metamod the
plugin needs: for a Metamod 1.12 server, build from its `1.12-dev` branch
(otherwise: `Plugin requires newer Metamod version (17 > 16)`).
GitHub Actions builds Linux and Windows on every push ([Actions](../../actions/workflows/build.yml)).

## Development

Setup: copy `scripts/dev.local.example` to `scripts/dev.local` and set `EMPIRES_TEST_SERVER` to an
isolated test server with the addon installed. Working with Claude Code or Codex: see
[AGENTS.md](AGENTS.md) and the [open-work priorities](docs/OPEN_WORK.md).

| Script | What it does |
|---|---|
| `scripts/dev_build.sh` | Incremental build (cached) |
| `scripts/dev.sh <map> <bots> <minutes>` | Build, deploy, run a match, summarize; `DEV_UNTIL` / `DEV_EXPECT` / `DEV_FAIL` end on log lines, `DEV_TIMESCALE`, `DEV_ROSTER`, `DEV_PRE`, `DEV_CMDS`, `DEV_PORT` |
| `scripts/dev_check.sh` | Regression: commander chain, long move, repair pad, turret, revive (parallel, ~1 min) |
| `scripts/dev_quick.sh [map] [min]` | Short targeted run with few bots and instant purchase (`QUICK_*` variables, `QUICK_FAIR=1`) |
| `scripts/dev_team_match.sh [map] [min]` | 10-bot match at 1× with the team-director numbers |
| `scripts/dev_econ.sh`, `dev_buy.sh` | Economy until the first driven vehicle / purchase only |
| `scripts/dev_vf_drive.sh`, `dev_turn.sh`, `dev_vehicle_obstacle.sh` | Factory exit and driving, steering and braking, obstacles on the route |
| `scripts/dev_orders.sh`, `dev_cmdorders.sh` | Orders as from the game UI / the bot commander's own orders |
| `scripts/dev_engibot.sh`, `dev_turret.sh`, `dev_revive.sh` | EngiBots, turrets (`DEV_TURRET_UPGRADE=1`), revive |
| `scripts/dev_navgen.sh <map>` | Generate a map's navmesh on the test server |
| `scripts/run_*_tests.sh` | Unit tests of single headers |

Tank shells and mortars only work at timescale 1 (with fast-forward they explode at the muzzle).

Navmesh tools in `tools/` (Python 3 with Pillow, read only `.nav`/`.bsp` files):

- `tools/nav_overview.py <map>.nav --bsp <map>.bsp --output out.png` draws the navmesh from above;
  `--crop x0,y0,x1,y1`, `--terrain` (slopes), `--props` (static props), `--vehicle-width 284`
  (too narrow for vehicles), `--log server.log [--userid N]` (stuck marks and driven track).
- `tools/check_nav.py` checks that spawns, buildings and resource points are on the main mesh component.

Test and diagnostic console commands (`rcbotd …`), useful for diagnosis:

- `empires_nav_scan x y z x2 y2 z2`: route and hull checks between two points (needs at least one bot
  on the server, otherwise `navmesh-not-loaded`).
- `empires_test_commander_move bot <userid> x y z`: a move order to one bot.
- `empires_test_exit_block <userid> [passenger]`: boxes in a vehicle to test the blocked exit.
- `empires_vehicle_obstacle [ahead] [lateral] [speed] [model]`: puts a prop on a driver's route.

Test-only cvars: `rcbot_empires_vehicle_debug_team 2|3` (only that team buys vehicles),
`rcbot_empires_vehicle_maneuvers`, `rcbot_empires_vehicle_curve_speed`, `rcbot_empires_vehicle_turn_test`
(experimental driving, default 0; measurements in [open work](docs/OPEN_WORK.md)).
`rcbot_analysis_start` records what the bots do ([EMPIRES_ANALYSIS_RECORDER.md](EMPIRES_ANALYSIS_RECORDER.md));
game-server internals: [reverse-engineering/](reverse-engineering/).

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
