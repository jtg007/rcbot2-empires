<div align="center">

# RCBot2 for Empires

**Autonomous bots for the Source mod [Empires][empires]** — blending FPS combat and RTS base-building with zero hand-made waypoints.

[![Platform](https://img.shields.io/badge/platform-Linux%20x86%20%7C%20Windows%20x86-blue)](#quick-start)
[![Engine](https://img.shields.io/badge/engine-Source%20SDK%202013%20%2F%20Metamod%3ASource-informational)](#quick-start)
[![Status](https://img.shields.io/badge/status-Work%20in%20Progress-yellow)](#features)
[![License](https://img.shields.io/badge/license-AGPL--3.0-green)](LICENSE)

[Quick Start](#quick-start) · [Features](#features) · [EngiBots](#engibots-for-human-commanders) · [Settings](#settings) · [Known Issues](#known-issues) · [Build](#build) · [Development](#development)

</div>

---

Built on [RCBot2][rcbot2-fork] for Source SDK 2013 (x86) and tested on Linux dedicated servers, these bots play full matches from start to finish: a bot commander builds the base and coordinates the team, engineers construct and repair, infantry push and capture flags, and drivers buy, maneuver, and fight in vehicles — all navigating via generated navmeshes.

> [!WARNING]
> **Work in progress.** The bots play complete matches, but still make mistakes. Check the [feature breakdown](#detailed-status-by-role), [known issues](#known-issues), and [open-work list](docs/OPEN_WORK.md) for current limits.

---

## Features

| Role | Highlights |
|---|---|
| **Navigation** | Waypoint-free navmesh routing that dynamically detours around match-built structures and recovers from stalls. |
| **Commander** | Takes the CV when vacant, builds the base & vehicle factory, assigns engineers, researches tech, and directs infantry squads. |
| **Engineers** | Build and repair structures & damaged vehicle armor, revive teammates, deploy refinery turrets, or serve as buyable **EngiBots**. |
| **Infantry** | All four classes with loadouts & skills; capture flags, pick tactical spawns, counter turrets, and attack buildings or vehicles. |
| **Vehicles** | Purchase upgraded chassis/weapons/armor as tech unlocks, drive with three-point turns, retreat to repair/rearm, and field mobile APC spawns. |

### Detailed Status by Role

> **Legend:** `✅` works in matches · `🟡` partial / not yet reliable · `⬜` planned

<details>
<summary><strong>🧭 Movement & Navigation</strong> — <code>3 ✅</code> · <code>3 🟡</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **Waypoint-free navigation** | Generated navmesh; routes around buildings placed during the match |
| ✅ | **Stuck recovery** | Detects stalls, backs off, tries another route; avoids blocked links near buildings for 60 s |
| ✅ | **Move orders** | Finishes within the game's 100-unit tolerance; stops at the nearest reachable point on the same level for off-mesh targets |
| 🟡 | **Doors and gates** | Some doors and buttons work; Crossroads base gates and ramps remain problematic |
| 🟡 | **Map coverage** | Runs on 7 maps; more installed maps have navmeshes without a recorded run yet |
| 🟡 | **Water** | Surfaces for air and avoids deep water when a land route exists; flags reachable only via water ladders are skipped |

</details>

<details>
<summary><strong>🎖️ Commander</strong> — <code>5 ✅</code> · <code>5 🟡</code> · <code>1 ⬜</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **Auto-command** | Takes the command vehicle when no human commands |
| ✅ | **Base building** | Places refineries, barracks, vehicle factory, armory, and repair pad; gets out as an engineer to help build |
| ✅ | **Engineer dispatch** | Sends free engineers to unfinished or damaged buildings; stalled engineers try another side, then release the job |
| ✅ | **Vehicle factory placement** | Reserves a clear spot near the CV with the exit facing the enemy, or drives to a remote site |
| ✅ | **CV survival** | Pulls back from nearby enemy vehicles or heavy fire |
| 🟡 | **Human handover** | Yields the seat when a human enters or uses the CV *(not yet tested with a real player)* |
| 🟡 | **Driving the CV** | Drives to distant build sites; gives up after 25 s without progress and builds from its current spot |
| 🟡 | **Research** | Radar, Structural Engineering, and Vehicle Factory first, then turrets, APC spawn, chassis, armor, engines, and heavy tanks |
| 🟡 | **Expansion & defense** | Forward barracks near remote factories and the front (own half only), plus up to two MG and two ML base turrets |
| 🟡 | **Team-director orders** | Splits free infantry into push / hold / defense (~50 / 30 / 20) via *attack location* orders; never overrides human orders |
| ⬜ | **Strategic adaptation** | Prioritize resource points by value and react dynamically to lost buildings |

</details>

<details>
<summary><strong>🔧 Engineers</strong> — <code>3 ✅</code> · <code>3 🟡</code> · <code>1 ⬜</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **Build and repair** | Constructs and repairs buildings and vehicles (targeting the damaged armor side) |
| ✅ | **Revive** | Downed bots wait briefly for a nearby engineer |
| ✅ | **Skill distribution** | Repair and revive skills are split across the team's engineers |
| 🟡 | **Turret upgrades** | Rank 30+ engineers upgrade their own turrets up to the map limit *(verified in tests, not yet observed in a full match)* |
| 🟡 | **Refinery turrets** | MG turrets at refineries work; occasional placement failures remain |
| 🟡 | **EngiBots** | Buyable engineer bots for human commanders — see [EngiBots](#engibots-for-human-commanders) |
| ⬜ | **Walls, cameras, radar** | Not implemented yet |

</details>

<details>
<summary><strong>🪖 Infantry</strong> — <code>4 ✅</code> · <code>6 🟡</code> · <code>1 ⬜</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **All four classes** | Scout, Rifleman, Grenadier, and Engineer with multiple loadouts and class skills |
| ✅ | **Combat** | Picks targets and engages at weapon range (pistols ~1000 up to snipers 4500); cover usage and class spacing are still open |
| ✅ | **Flag capture** | Captures objectives on maps with capture points |
| ✅ | **Turret avoidance** | Retreats out of enemy turret firing range |
| 🟡 | **Smart spawning** | Spawns near threatened buildings on base alerts, at the VF for drivers, or at friendly APCs / forward barracks; exits blocked APCs |
| 🟡 | **Tactical respawn** | Defenders far from an alert or drivers far from the factory may respawn closer (`rcbot_empires_kill_spawn`) |
| 🟡 | **Anti-structure** | Grenadiers use RPG/mortar, riflemen throw sticky grenades *(confirmed on enemy refinery)*, scouts sabotage *(confirmed)* |
| 🟡 | **Anti-vehicle** | Riflemen sticky-grenade vehicles; scouts approach with stun grenades *(not yet confirmed in a full match)* |
| 🟡 | **Guard & attack orders** | Infantry follow guarded units; engineers build or repair guarded buildings *(not yet tested with a human)* |
| 🟡 | **Squads, mines, binoculars** | Implemented, rarely used |
| ⬜ | **Seismic grenades** | Engineers clearing enemy turrets |

</details>

<details>
<summary><strong>🚜 Vehicles</strong> — <code>5 ✅</code> · <code>5 🟡</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **Purchasing** | Buys at built or map-placed vehicle factories; drives blocking empty vehicles out of the bay |
| ✅ | **Tech-scaled loadouts** | Upgrades chassis, weapons, armor, and engines as research & budget allow; keeps one APC at the front when APC Spawn is researched |
| ✅ | **Driving** | Follows the navmesh, executes three-point turns, routes around buildings and solid map props (trees, roadblocks), and avoids drop-offs or water too deep for the chassis |
| ✅ | **Retreat & repair** | Returns to the repair pad when damaged, or parks beside the own CV |
| ✅ | **Roadkills** | Runs over enemy infantry |
| 🟡 | **Vehicle combat** | Cannon leads moving targets; hitscan MG tracks infantry up to 1200; shells buildings from 550–1500; angles strongest armor toward threats |
| 🟡 | **Target priority** | Defend base → hunt spotted tanks → support front → push enemy buildings → enemy CV |
| 🟡 | **Rearming** | Drives to an ammo crate or repair pad, or calls an engineer for ammo |
| 🟡 | **Lawliet mode** | When a team falls clearly behind, a crewed APC (Scout, Rifleman, Engineer) rushes the enemy CV: Scouts re-stun it about every 10 s, Riflemen throw sticky grenades, the Engineer drops an ammo crate *(far crew members respawn at base instead of walking; forced mode at 1×: CV destroyed in about 60 s; auto trigger not yet seen in a match)* |
| 🟡 | **Bill Nye mode** | Scout jeeps ram enemy vehicles off the road on `emp_rainbowroad` *(occasionally falls off itself)* |

</details>

---

## Quick Start

1. **Install [Metamod:Source][mms]** on your Empires dedicated server.
2. **Download the plugin**: grab `rcbot2-empires-linux-x86.zip` (or `-windows-x86.zip`) from [Releases](../../releases) (`latest` tracks `main`) and extract it into `empires/`. Or [build from source](#build).
3. **Verify `empires/addons/metamod/rcbot2.vdf`**:

   ```vdf
   "Metamod Plugin"
   {
   	"alias"		"rcbot2"
   	"file"		"addons/rcbot2/bin/RCBot2Meta"
   	"rcbot2path"	"addons/rcbot2/"
   }
   ```

4. **Generate navmeshes** (`empires/maps/<map>.nav`): if a map lacks one, load it and run:

   ```console
   sv_cheats 1; p_nav_generate
   ```

   *(Takes up to a minute per map.)*

5. **Add bots** from the server console:

   ```console
   rcbotd addbot <class> <team> <name>
   ```

   - **Teams:** `2` = Northern Faction, `3` = Brenodi Empire
   - **Classes:** `0` = Scout, `1` = Rifleman, `2` = Grenadier, `3` = Engineer
   - **Auto-fill / Bot quota:** edit `addons/rcbot2/config/bot_quota.ini` (e.g. `Humans "12" => Bots "0"`) to automatically adjust bot count as players join.

> [!NOTE]
> - **Windows:** Untested in game; features that read fixed offsets in the Linux server binary fall back automatically ([docs/WINDOWS.md](docs/WINDOWS.md)).
> - **Metamod version:** A plugin built against Metamod 2.0 headers is rejected by Metamod 1.12 — see [Build](#build).

---

## EngiBots for Human Commanders

Human commanders can buy dedicated base-building engineer bots in chat with `/buildengibot` or `!buildengibot` (or `buildengibot` in console).

- **Cost & limit:** `rcbot_empires_engibot_cost` team resources (default `100`), up to `rcbot_empires_engibot_limit` per team (default `2`). Status feedback appears on screen (e.g. `EngiBot ready (1/2)`, or why a purchase failed).
- **Base duty:** Builds and repairs unfinished/damaged buildings and friendly vehicles within 2500 units of the CV (including human-placed structures).
- **Idle behavior:** Waits ~900 units from the CV so building spots stay clear.
- **Orders:** *Attack location* builds an MG turret at the target; *Guard* on a building builds or repairs it.
- **Strict scope:** Never captures flags, roams, enters vehicles, takes command, or chases enemies. Automatically leaves on map change and is ignored by bot quota.

> [!NOTE]
> EngiBots have so far only been tested with the bot commander (`scripts/dev_engibot.sh`), not yet with a human commander.

---

## Settings

> [!TIP]
> Bots will hand over their wages to any teammate who types `wages` in chat.

### Commander

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_auto_commander` | `1` | Bot takes the command vehicle when no human commands |
| `rcbot_empires_commander_engineer` | `1` | Bot commander spawns as an engineer and builds near the CV |
| `rcbot_empires_commander_orders` | `1` | Bot commander issues team-director orders to free infantry |
| `rcbot_empires_commander_drive_from` | `1000` | Distance beyond which the commander drives the CV to build sites |
| `rcbot_empires_commander_boomtank` | `1` | Commander runs over base intruders (`0` off, `1` on except rainbowroad, `2` everywhere) |

### Engineers & EngiBots

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_engineer_turrets` | `1` | Engineers build MG turrets at friendly refineries |
| `rcbot_empires_engineer_help_human` | `2` | Engineer bots per team that autonomously build/repair near a human commander's CV |
| `rcbot_empires_engibot` | `1` | Allow commanders to purchase EngiBots |
| `rcbot_empires_engibot_cost` | `100` | Team resources per EngiBot |
| `rcbot_empires_engibot_limit` | `2` | Maximum EngiBots per team (`0`–`8`) |
| `rcbot_empires_engibot_leash` | `2500` | Operating radius around the team CV |

### Vehicles & Tactics

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_vehicles` | `1` | Bots buy, drive, and fight with vehicles |
| `rcbot_empires_vehicle_drivers` | `-1` | Max vehicle drivers per team (`-1` = 40% of team bots, minimum 2) |
| `rcbot_empires_billnye` | `-1` | Bill Nye ramming mode (`-1` NF on rainbowroad, `0` off, `2` NF, `3` BE) |
| `rcbot_empires_lawliet` | `1` | Lawliet mode, a desperation APC rush on the enemy CV (`0` off, `1` auto when behind in research & map control or tickets, `2` force NF, `3` force BE) |
| `rcbot_empires_kill_spawn` | `1` | Tactical respawn closer to goal (≥ 3800 units closer, 6 s out of combat, tickets > 20 & > 25%, max once per 90 s) |

### Diagnostics & Offsets

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_debug_log` | `0` | `1` = verbose bot diagnostics in the server log |
| `rcbot_empires_force_offsets` | `0` | Force fixed-offset reads on unknown `server_srv.so` builds (`1` = enable; ignored on Windows) |

---

## Known Issues

- **Infantry stuck spots:** Bots can get stuck inside barracks or vehicles; some `emp_rainbowroad` routes fail.
- **Vehicle stuck spots:** Near repair pads, inside the BE base, at Crossroads gates/ramps and a wall near `(3800 5000)`, and in the palm cluster for NF tanks on `emp_palmbay`.
- **`emp_palmbay`:** Both flags sit on boats without navmesh (played like a no-flag map); NF finds no factory spot near its CV and builds remotely; NF commander entry can stall at spawn.
- **`emp_coast`:** Engineers find no valid stand point at refineries, leaving them unfinished.
- **`emp_rainbowroad`:** Refinery turret placement is intermittent, and the Bill Nye jeep occasionally drives off edges.
- **Blocked vehicle exits:** A bot boxed in on all sides stays seated and retries exiting every ~15 s.
- **Platform coverage:** Only Linux dedicated servers have been tested.

---

## Build

**Requirements:** Git, Python 3 with [AMBuild][], GCC with 32-bit support, plus Source SDK 2013, Metamod:Source, SourceMod, and the HL2SDK manifests (e.g. in a sibling `../rcbot2-clean` checkout).

```sh
SDK_ROOT="../rcbot2-clean/alliedmodders" \
MANIFEST_DIR="../rcbot2-clean/hl2sdk-manifests" \
scripts/build_empires.sh
```

The script prints the resulting `.so` path.

- **Metamod compatibility:** Your `metamod-source` checkout determines the target Metamod version. For a Metamod 1.12 server, use its `1.12-dev` branch (otherwise loading fails with `Plugin requires newer Metamod version (17 > 16)`).
- **CI:** GitHub Actions builds Linux and Windows on every push ([Actions](../../actions/workflows/build.yml)).

---

## Development

Copy `scripts/dev.local.example` to `scripts/dev.local` and point `EMPIRES_TEST_SERVER` at an isolated test server with the addon installed. For contributor and agent workflows, see [AGENTS.md](AGENTS.md) and [docs/OPEN_WORK.md](docs/OPEN_WORK.md).

> [!IMPORTANT]
> Tank shells and mortars only work at `DEV_TIMESCALE=1` — with fast-forward enabled they detonate at the muzzle.

<details>
<summary><strong>Test & benchmark scripts</strong> (<code>scripts/</code>)</summary>

<br>

| Script | Purpose |
|---|---|
| `scripts/dev_build.sh` | Incremental cached build |
| `scripts/dev.sh <map> <bots> <minutes>` | Build, deploy, run match, and summarize (`DEV_UNTIL`, `DEV_EXPECT`, `DEV_FAIL`, `DEV_TIMESCALE`, `DEV_ROSTER`, `DEV_PRE`, `DEV_CMDS`, `DEV_PORT`) |
| `scripts/dev_check.sh` | Parallel regression suite (~1 min): commander chain, long move, repair pad, turret, revive |
| `scripts/dev_quick.sh [map] [min]` | Short targeted run with few bots and instant purchase (`QUICK_*`, `QUICK_FAIR=1`) |
| `scripts/dev_team_match.sh [map] [min]` | 10-bot match at 1× with team-director metrics |
| `scripts/dev_econ.sh`, `dev_buy.sh` | Economy to first driven vehicle / purchase-only check |
| `scripts/dev_vf_drive.sh`, `dev_turn.sh`, `dev_vehicle_obstacle.sh` | Factory exit & driving, steering/braking, obstacle handling |
| `scripts/dev_orders.sh`, `dev_cmdorders.sh` | UI-style orders / bot commander's own orders |
| `scripts/dev_engibot.sh`, `dev_turret.sh`, `dev_revive.sh` | EngiBots, turrets (`DEV_TURRET_UPGRADE=1`), revive |
| `scripts/dev_lawliet.sh [map] [min]` | Forced NF Lawliet rush on the BE CV at 1× (`LAWLIET_ROSTER`, `LAWLIET_REAL_VF=1`) |
| `scripts/dev_navgen.sh <map>` | Generate a map's navmesh on the test server |
| `scripts/run_*_tests.sh` | Unit tests for individual headers |

</details>

<details>
<summary><strong>Navmesh tools, diagnostic commands & test cvars</strong></summary>

<br>

**Navmesh tools (`tools/`, Python 3 + Pillow, reads `.nav`/`.bsp` only):**
- `tools/nav_overview.py <map>.nav --bsp <map>.bsp --output out.png` — top-down navmesh render; supports `--crop x0,y0,x1,y1`, `--terrain` (slopes), `--props` (static props), `--vehicle-width 284` (narrow bottlenecks), and `--log server.log [--userid N]` (stuck marks & driven track).
- `tools/check_nav.py` — verifies that spawns, buildings, and resource points sit on the main mesh component.

**Diagnostic console commands (`rcbotd …`):**
- `empires_nav_scan x y z x2 y2 z2` — route and hull checks between two points *(requires at least one bot on the server, otherwise `navmesh-not-loaded`)*.
- `empires_test_commander_move bot <userid> x y z` — send a move order to a single bot.
- `empires_test_exit_block <userid> [passenger]` — box in a vehicle to test blocked exits.
- `empires_vehicle_obstacle [ahead] [lateral] [speed] [model]` — place a prop on a driver's route.

**Test-only cvars & internals:**
- `rcbot_empires_vehicle_debug_team 2|3` (restrict vehicle purchases to one team).
- `rcbot_empires_vehicle_maneuvers`, `rcbot_empires_vehicle_curve_speed`, `rcbot_empires_vehicle_turn_test` (experimental driving, default `0`; measurements in [docs/OPEN_WORK.md](docs/OPEN_WORK.md)).
- `rcbot_analysis_start` records bot telemetry ([EMPIRES_ANALYSIS_RECORDER.md](EMPIRES_ANALYSIS_RECORDER.md)).
- Game-server internals: [reverse-engineering/](reverse-engineering/).

</details>

---

## License & Credits

Licensed under [AGPL-3.0](LICENSE), like RCBot2: if you run a modified version on a server, its source code must be available to players. `rcbot/logging.{h,cpp}` is released separately under the [BSD Zero Clause License](https://spdx.org/licenses/0BSD.html).

Based on [RCBot2][rcbot2] by **Cheeseh** and its [fork by APGRoboCop][rcbot2-fork].

<details>
<summary>RCBot2 upstream contributors</summary>

- Founder — Cheeseh
- Bot base code — Botman's HPB Template
- Linux Conversion and Waypointing — [APG]RoboCop[CL]
- TF2 support and enhancements — Ducky1231/Pongo
- SourceMod and AMBuild support — nosoop
- Synergy, TF2, MvM, CSS and AMBuild support — Anonymous Player/caxanga334
- TF2 Classified support — Technochips
- TF2 additional gamemodes support — RussiaTails
- Linux Black Mesa and SDK2013 mathlib fix — Sappho
- Dystopia support — Soft As Hell
- DoDS and main config support fixes — DNA.styx
- Major waypointer for TF2 — LK777, RussiaTails, Witch, Francis, RevilleAJ, Waffle033
- Major waypointer for DoDS — INsane, Gamelarg05, genmac
- Miniz compression — Rich Geldreich and Tenacious Software LLC

</details>

[empires]: https://empiresmod.com/
[rcbot2]: http://rcbot.bots-united.com/
[rcbot2-fork]: https://github.com/APGRoboCop/rcbot2
[mms]: https://www.sourcemm.net/
[AMBuild]: https://wiki.alliedmods.net/AMBuild
