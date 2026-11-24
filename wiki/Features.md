# Features

Sources: [README.md](../README.md) (feature tables), [docs/OPEN_WORK.md](../docs/OPEN_WORK.md), file headers in
`utils/RCBot2_meta/bot_empires*.cpp`. Snapshot 2026-11-23.

Status legend:
- **Verified**: seen working in a real run (run IDs such as `20261123-122645` name the test run the numbers come from).
- **Built**: implemented and passing a scenario or unit test, but no natural occurrence in a 10-bot match yet.
- **Validation**: implemented and on by default, measured in 10-bot matches, but not yet observed with human players.
- **Open**: not implemented or known unreliable.

Numbers like `3.25` are stable item IDs used in commits, logs and [docs/OPEN_WORK.md](../docs/OPEN_WORK.md) (see [Roadmap](Roadmap.md)). Switches are listed on [Cvars](Cvars.md).

## Navigation (all roles)

| Feature | Status | Notes |
|---|---|---|
| Waypoint-free navmesh routing | Verified | Generated `.nav` per map; routes detour around buildings built during the match (`ApplyBuildingDetours`); goals inside solid building walls end at the nearest free cell outside. |
| Stuck recovery | Verified | Detects stalls (also flush building contacts), backs off, replans. A 15 s cooldown (`kFailureCooldown`) blocks the same goal+segment; vehicles get a soft infantry-only edge penalty (3.16) instead of a 60 s full block. |
| Move orders | Verified | Finish within the game's 100-unit tolerance; stop at the nearest reachable point for off-mesh targets (3.18). |
| Water | Verified | Surface for air, avoid deep water, swim to ladders ending in water, climb boat decks on `emp_palmbay`, jump back into water (5.15). |
| Doors and gates | Verified | Blocks closed `func_door*`, `prop_door*`, and `func_movelinear` gates; reserves door buttons only when a front-side line-of-sight stand is reachable within 72 HU, releases claims on death, disambiguates duplicate names by position (`emp_mvalley`), and skips timed auto-closing buttons (Crossroads 15 s gates); live built-state detection for `emp_eng_map_model` walls and `emp_eng_map_brush` ramps (`as_escort` 3/3 `20261124-181431`, Crossroads built ramp `20261124-180923`, gate drive `20261124-181243`). |
| Map coverage | Built | 21 of 22 multiplayer maps have a navmesh and ran without a crash (5.11); see [Maps](Maps.md). |

## Commander (bot commander in the CV)

| Feature | Status | Notes |
|---|---|---|
| Auto-command | Verified | Takes the CV when no human commands; yields the seat when a human enters/uses it (handover not tested with a real player). |
| Base building | Verified | Refineries, barracks, vehicle factory (VF), armory, repair pad via `emp_build`; commander leaves the CV as an engineer to help build (`rcbot_empires_commander_engineer`). Policy in `bot_empires_commander_economy.h`. |
| Engineer dispatch | Verified | Free engineers go to unfinished/damaged buildings; stalled engineers try another side, then release the job. |
| VF placement | Verified | Reserves a clear spot near the CV with the exit facing the enemy, or drives to a remote site; exit lane checked (`factory-yaw`). On `emp_palmbay` NF finds no near spot and builds remotely. |
| CV survival (5.23) | Verified | Pulls the CV back from enemy vehicles within 2200 HU or under heavy fire; also drives the CV over base intruders (`rcbot_empires_commander_boomtank`). |
| Research (5.1, 5.2) | Verified | Radar, Structural Engineering, Vehicle Factory first, then turret levels, Mechanical Engineering, APC spawn, chassis/armor/engine tiers, heavy tanks; resumes paused research and resets after radar loss. Natural-research run `20261120-233436`. |
| Expansion and defense | Built | Forward barracks near remote factories/the front (own half only), up to two MG and two ML base turrets. Walls open (5.9). |
| Driving the CV | Verified | Drives to distant sites, slows to about 250 u/s in the last 800 units; gives up after 25 s without progress (`stall-escalate`). Sporadic stalls remain (1.5b). |
| Team-director orders (5.4, 5.8, 5.28) | Validation | Free infantry split about 50/30/20 push/hold/defense via *attack location* orders; never overrides human orders; role locks (40 s), alert logic, flag avoidance. Shared flag attack seen (Coast `20261122-124709`); feel with humans open. |
| Shared team economy (5.5a) | Built | Commander publishes a reserve; vehicle purchases keep it; richest-wages buyer goes first; bots hand over wages when a teammate types `wages`. |
| Commander target call (5.30) | Built | A seated commander can redirect one attacking grenadier to an APC a teammate sees. |
| Strategic adaptation (5.5) | Verified | The commander weighs map income against distance when choosing a refinery site. Escort choices also account for the engineer’s walk. A controlled Arid run (`20261124-112144`) selected the higher-yield site. In a ten-bot Palmbay match at normal speed (`20261124-112020`), NF chose between two sites and placed the refinery. Palmbay run `20261121-170243` showed recovery after a refinery loss. |

## Engineers

| Feature | Status | Notes |
|---|---|---|
| Build and repair | Verified | Buildings and damaged vehicle armor (targets the damaged side). Kit trace is only 100 units from the eye. |
| Skill distribution | Verified | Repair and revive skills split across the team's engineers (`updateEmpiresEngineerSkills`). |
| Revive | Built | Individual `emp_revive` (route must end within 120 HU of the body; 6 s replan/release; wounded bot waits up to 3 s). Timing changes await match verification (3.4d). A "vacancy" Mass Revive path exists behind `rcbot_empires_vacancy_revive`. |
| Heal teammates (5.31.1) | Verified | Out of combat, heals a visible hurt teammate (<85 %, within 700 HU) with the kit; hurt bot waits up to 8 s. `dev_heal.sh`: 42 to 100 HP in about 5 s; natural heals in match `20261123-020726`. |
| Seismic grenades (5.31.2, 5.7) | Built | Throws at enemy buildings up to 1400 HU (turrets, then camera/radar, large buildings, walls). 20 damage per hit on a barracks; aborts after 5 throws without damage. Never triggered naturally yet; run-up throws at firing turrets open. |
| Refinery turrets (1.5a, 3.5, 5.8d) | Built | Repair-role engineer places an MG turret on the refinery front side; with two turret slots (rank) a second one on the side away from the enemy. Occasional silent placement failures (rainbowroad, Palmbay). |
| Turret upgrade (5.6) | Built | Rank 30+ engineers raise own turrets 1 to 3 (`DEV_TURRET_UPGRADE=1 scripts/dev_turret.sh`); not seen in a full match. |
| Ammo crates (3.10b, 3.10c) | Built | Answers a vehicle "I need ammo" call and a human infantry ammo call by placing a supply crate (`engSelect 7`). Two simultaneous calls unreliable; autonomous infantry requests open. |
| EngiBots (7.1) | Built | Buyable base-engineer bots for human commanders (`/buildengibot`); only tested with the bot commander. See below. |
| Refinery walls, radar, cameras (5.9, 5.31.4) | Built | Three-part wall on the refinery side facing the enemy CV (kit item 6, one chained line of four clicks), then a camera behind it (item 4) and, with a second detection slot from rank, a radar (item 5), each built up to full health; walls are free for engineers; one refinery per engineer (`bot_empires_deployables.cpp`, `scripts/dev_deploy.sh`). |
| ML turret (5.31.3) | Open | Not implemented. |

### EngiBots

`/buildengibot` or `!buildengibot` in chat (or console `buildengibot`). Cost `rcbot_empires_engibot_cost` (100), limit
`rcbot_empires_engibot_limit` (2 per team). They build/repair unfinished or damaged buildings and friendly vehicles within
`rcbot_empires_engibot_leash` (2500 HU) of the CV, idle about 900 HU from the CV, build an MG turret on an *attack location*
order, build/repair a guarded building, and never capture, roam, drive, command or chase. They are ignored by bot quota and
`min_bots`/`max_bots`. Chat is intercepted in `Hook_ClientCommand` before SourceMod chat triggers; HintText/SayText formats match
`server_srv.so`. See `bot_empires_engibot.cpp`, `reverse-engineering/notes/plugin-callbacks.md`.

## Infantry (Scout, Rifleman, Grenadier, Engineer)

| Feature | Status | Notes |
|---|---|---|
| Four classes, loadouts, personal skills (5.25) | Verified | `changeskill` for all classes; loadout variants in `kEmpiresNormalLoadouts` (`bot_empires.cpp`), incl. rifleman sticky, scout sniper/shotgun. Scout hide and sniper positions, grenadier anti-tank positions and road mines open. |
| Flag capture | Verified | `bot_empires_objective.cpp`; bounded retries, combat-aware resume, start-anchor recovery. |
| Combat target and range | Verified | Engages at weapon range (pistol about 1000 up to snipers 4500); weapon ranges in `bot_weapons.cpp`. |
| Turret avoidance | Verified | Retreats out of enemy turret firing range (danger circle; very expensive instead of forbidden since the DMZ fix). |
| Smart spawning / tactical respawn (5.28) | Verified / Built | Spawns near threatened buildings on base alert, at the VF for drivers, at friendly APCs/forward barracks. Kill-spawn proven for VF buyers; a redeployed defender not yet observed. |
| Anti-structure (3.23) | Verified | Grenadier RPG and mortar (mortar kill of a turret seen), rifleman sticky grenades, scout sabotage (`IN_USE`). |
| Anti-vehicle (3.22) | Built | Grenadier tracks APC hull HP; scouts approach slow vehicles with stun grenades (at most 150 HU/s) and throw faster ones within 250 HU; sticky hits and stun effect still to confirm at 1x. |
| Target knowledge (3.20) | Built | Tank hunting/defense, commander target calls and grenadier mortar vs. APCs require a sightline of the bot or a living teammate (up to 3500 HU). |
| Guard and attack orders from a human commander (5.3) | Built | Infantry follow guarded units, engineers build/repair guarded buildings (tested via `empires_test_unit_order`; real human open). |
| Squads, mines, binoculars (5.26) | Built | Implemented, rarely used; squad runtime observation modules (`bot_empires_squad_runtime.*`). |
| Close-quarters movement `rcbot_empires_skirmish` (3.25) | Validation | Keeps an effective distance for the weapon (at most 900 HU, beyond that it advances), side-steps away from the enemy aim, backs off when too close/reloading/hurt, short crouched bursts from 500 HU. Short tests: 36 fights, 82 steps, 0 escalations; duel new:old 13:12. |
| Fight odds `rcbot_empires_fight_odds` | Validation | Compares visible enemies with nearby teammates: outnumbered gives ground, ahead presses in. |
| Fight cover `rcbot_empires_fight_cover` | Validation | Reloading, badly hurt or outnumbered bots step out of sight to a close safe spot and return; keeps shooting (replaces the old "hurt hide schedule" in `hurt()`). |
| Join fight `rcbot_empires_join_fight` | Validation | Free infantry within 2000 HU walks to a teammate's fight (max 2 helpers per fight; a fight report stays valid for 15 s). |
| Callouts `rcbot_empires_callouts` | Validation | Bots spot enemies they start fighting (groups or within 800 HU; once per team per minute, per bot every 3 min). |
| Classes in combat (`_scout_flank`, `_scout_binoculars`, `_grenadier_rpg`, `_engineer_rear`) | Validation | Scout flanks a teammate's enemy and spots far enemies with the binoculars (game marks the target spotted, `m_VisibilityState` 3); Grenadier RPG stand-off against vehicles (550-1300 HU) and at least 400 HU from soldiers; Engineer stays behind the front teammate. |
| Calm idle look `rcbot_empires_calm_look` | Validation | Standing bots look toward the last enemy or around their facing instead of random points around their head. |

The five infantry switches default to 1. Two paired 15-minute 10-bot Crossroads matches (all on vs. all off, run in parallel) showed
no new stalls and no crashes; kills 28 vs. 24. Each switch can be set to 0 on a live server without a rebuild.

## Vehicles

| Feature | Status | Notes |
|---|---|---|
| Purchase | Verified | At built or map-placed VFs; buyer must stand under the VF roof; adopts empty blocking vehicles after 5 s (`bay-occupied`) and retries abandoned field vehicles once after 10 s; a human teammate within 250 HU keeps priority (3.8 awaits a human check). |
| Tech-scaled loadouts (5.2) | Verified | `bot_empires_vehicle_loadout.h`: chassis/weapons/armor/engine by research and budget; keeps one APC at the front when APC spawn is researched. |
| Driving | Verified | Three-point turns, detours around buildings, physboxes and solid props for all vehicle classes (5.x audit 22.11.), avoids drop-offs and too-deep water. Local maneuver/curve-speed experiments are parked (default 0). |
| Retreat and repair (3.12) | Verified | Returns to the repair pad when damaged; without a pad parks near the CV (needs a full vehicle route). Crossroads pad entrance timeouts parked. |
| Roadkills | Verified | Runs over enemy infantry. |
| Vehicle combat (3.21) | Verified | Turret aims in hull frame; cannon leads targets; hitscan MG up to 1200 vs. infantry; shells buildings from 550-1500 HU; strongest armor side toward threats. |
| Target priority (3.24) | Built | Defend base (CV threat first) then hunt tanks, support front, push buildings, CV. CV-threat override over APCs awaits a match. |
| Rearming (3.10b, 3.10d) | Built | Pad refit, nearer ammo crate, or engineer call; Light/AFV measured, Medium/Heavy pending. Mixed loadout on pad refits (`vehicle retreat reason=loadout`). |
| Spawn APCs (5.14a) | Verified (Palmbay) | Flank parking with vehicle access; no APC parked in front of its own VF without a support spot. |
| Lawliet mode (5.14b) | Built | Desperation APC rush on the enemy CV (crew 2 Scouts + Rifleman + Engineer). Forced Crossroads runs destroy the CV; natural trigger seen once (boarding timeout, fixed). Open: boarding under the natural trigger, real grenade resupply, scout re-throw after APC respawn. |
| Bill Nye mode (3.6b) | Built | One scout jeep rams enemy vehicles off the road on `emp_rainbowroad`; occasionally falls off itself. |
| Squad passengers (3.11) | Open | Squad bots should ride as gunners instead of leaving at `not-driver-seat`. |

## Team director and squads

The team director (`bot_empires_team_director.*`, 5.4/5.8/5.28) rebuilds one picture per team every about 1.5 s: own CV and buildings,
capture points, enemies near own buildings, tickets. It names push/hold/defense goals, raises base alerts (an alert lasts 40 s after its
last trigger), splits free infantry (no engineers, EngiBots, drivers or bots with a human order) about 50/30/20 and lets the seated
commander issue *attack location* orders (`bot_empires_commander_orders.cpp`). Without reachable flags push goals are the nearest enemy
refinery/forward barracks. Bots stalled at a building forbid that mesh link for 60 s. Match numbers: Palmbay `171039` to `174745`
push/defense swaps 11 to 8, longest barracks stall about 4 min to 68 s.

## Analysis recorder

Passive opt-in telemetry (JSONL events and snapshots plus `summary.json`) used for stuck share, kills, captures, engagements.
Commands and output: see [Build and test](Build-and-Test.md#recorder-and-run-evaluation).
