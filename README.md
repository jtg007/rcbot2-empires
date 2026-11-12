# RCBot2 for Empires

**Bots for the Source mod [Empires][empires]** – the mix of first-person shooter and
real-time strategy. Built on [RCBot2][rcbot2-fork], running on a Linux dedicated server
(Source SDK 2013, x86).

> [!WARNING]
> **Work in progress.** The bots already play whole matches – commander, engineers,
> infantry and vehicles – but they are not finished and still make mistakes.
> See [what works](#what-the-bots-can-do) and [known issues](#known-issues).

## The goal

Bots that play Empires the way a team does, not generic shooter bots:

- **No hand-made waypoints.** Every map works with an automatically generated navmesh.
- **A bot commander** builds the base, keeps the economy running and leads the team,
  or hands the seat to a human player.
- **Human commanders can command bots** like units in a real-time strategy game:
  move here, attack that, guard this building.
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
| ✅ | Doors | Open doors and press map buttons that open gates |
| 🟡 | All maps | Tested on 7 maps: streetsoffire, crossroads, as_escort, rainbowroad, palmbay, coast, cyclopean |

### Commander

| | Feature | Notes |
|:-:|---|---|
| ✅ | Take the command vehicle | When no human commands |
| 🟡 | Hand over to a human | A human who gets in or uses the CV gets the seat; not tested with a real player yet |
| ✅ | Build the base | Refineries, barracks, vehicle factory, armory, repair station in a fixed order |
| ✅ | Assign engineers | Sends free engineers to unfinished or damaged buildings |
| ✅ | Leave the CV to build | The commander is an engineer: gets out, builds, gets back in |
| ✅ | Drive the CV | To distant buildings, and over enemy infantry in the base |
| ✅ | Place the vehicle factory | Exit faces along the route towards the enemy |
| ⬜ | Research | |
| ⬜ | Adapt to the game | Choose resource points by value, react to lost buildings |
| ⬜ | Give orders to bots | Attack location, attack target, guard |

### Engineers

| | Feature | Notes |
|:-:|---|---|
| ✅ | Build and repair buildings | |
| ✅ | Repair vehicles | Heals the damaged armor side |
| ✅ | Revive teammates | Dead bots wait a moment for a nearby engineer |
| ✅ | Turrets at refineries | MG turret next to the team's refineries |
| ✅ | Skills | Repair and revive split across the team's engineers |
| ⬜ | Walls, cameras, radar | |

### Infantry

| | Feature | Notes |
|:-:|---|---|
| ✅ | Classes | Scout, Rifleman, Grenadier, Engineer with fitting loadouts |
| ✅ | Fight | Aim, shoot, pick targets |
| ✅ | Turret fire | Retreat out of a turret's range |
| ✅ | Capture flags | On maps with capture points |
| ✅ | Follow move orders | From a human commander |
| 🟡 | Destroy turrets | Grenadier with mortar or rockets; often finds no firing position yet |
| 🟡 | Follow guard orders | Engineers repair the ordered building; not tested with a human commander yet |
| 🟡 | Squads | Bots join squads; roles and teamwork inside a squad are planned |
| 🟡 | Mines, scout binoculars | Implemented, rarely used in matches |
| ⬜ | Seismic grenades | Engineers against turrets |

### Vehicles

| | Feature | Notes |
|:-:|---|---|
| ✅ | Buy at the vehicle factory | Standard loadout; better parts after research are planned |
| ✅ | Drive | On the navmesh, three-point turns, keep away from edges |
| ✅ | Fight | Cannon against vehicles, MG against infantry, lead moving targets, circle the enemy |
| ✅ | Run over infantry | |
| ✅ | Retreat and repair | Back to the repair station when damaged |
| 🟡 | Re-customize on the repair pad | Implemented, not confirmed in a match |
| 🟡 | Bill Nye mode | One scout jeep rams enemy vehicles off the road (on `emp_rainbowroad`); still falls off itself sometimes |

## Install

1. Install [Metamod:Source][mms] on the Empires dedicated server.
2. Build the plugin (see [Build](#build)).
3. Copy `build/package/addons` into the server's `empires/` folder.
4. Every map needs a navmesh (`empires/maps/<map>.nav`). If it is missing, load the
   map and run `sv_cheats 1; p_nav_generate` once (takes up to a minute).
5. Add bots from the server console:

   ```
   rcbotd addbot <class> <team> <name>
   ```

   Team 2 = Northern Faction, 3 = Brenodi Empire. Class 0 Scout, 1 Rifleman,
   2 Grenadier, 3 Engineer.

### Settings

| Cvar | Default | Meaning |
|---|---|---|
| `rcbot_empires_auto_commander` | 1 | A bot takes the command vehicle if no human does |
| `rcbot_empires_commander_engineer` | 1 | The bot commander is an engineer and builds near the CV itself |
| `rcbot_empires_commander_drive_from` | 1000 | The commander drives the CV to buildings farther away than this |
| `rcbot_empires_commander_boomtank` | 1 | Commander runs over enemies in the base: 0 off, 1 on except on rainbowroad maps, 2 everywhere |
| `rcbot_empires_engineer_turrets` | 1 | Engineers build MG turrets at the team's refineries |
| `rcbot_empires_vehicles` | 1 | Bots buy, drive and fight with vehicles |
| `rcbot_empires_vehicle_drivers` | -1 | Max bots per team in vehicles (-1 = 40 % of the team's bots) |
| `rcbot_empires_billnye` | -1 | Bill Nye mode: -1 = NF on rainbowroad maps, 0 off, 2 NF, 3 BE |

## Build

Needs Git, Python 3 with [AMBuild][], and GCC with 32-bit support (`gcc-multilib`).

```sh
git submodule update --init hl2sdk-manifests alliedmodders/hl2sdk-sdk2013 \
    alliedmodders/metamod-source alliedmodders/sourcemod
mkdir build && cd build
CC=gcc CXX=g++ python3 ../configure.py -s sdk2013 --targets x86 --enable-optimize \
    --hl2sdk-root ../alliedmodders \
    --mms-path ../alliedmodders/metamod-source \
    --sm-path ../alliedmodders/sourcemod
ambuild
```

The finished files are in `build/package/addons/`.

## Known issues

- The Bill Nye jeep sometimes drives off edges on `emp_rainbowroad`.
- Engineers cannot place turrets at the map's own refineries on `emp_rainbowroad`.
- Infantry can get stuck inside vehicles.
- Only Linux servers are tested.

## Development

- `scripts/dev.sh <map> <bots> <minutes>` builds, deploys to an isolated test server
  and prints a summary of the match. Set `EMPIRES_TEST_SERVER` (or put it in
  `scripts/dev.local`) to that server.
- `scripts/dev_check.sh` is the quick regression check (about 3 minutes).
- `scripts/run_*_tests.sh` are unit tests that run without a game server.
- `rcbot_analysis_start` records what the bots do: [EMPIRES_ANALYSIS_RECORDER.md](EMPIRES_ANALYSIS_RECORDER.md).
- What we know about the game server's internals: [reverse-engineering/](reverse-engineering/).

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
