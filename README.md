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

Front support (pending match verification): spawn APCs look for dry flank parking with vehicle access and a short infantry route to a contested flag. Engineers can answer a human infantry ammo call. Under fire, infantry can take nearby navmesh cover and resume their objective. A seated bot commander can redirect one attacking grenadier to an enemy APC seen by a teammate.

> [!WARNING]
> **Work in progress.** The bots play complete matches, but still make mistakes. Check the [feature breakdown](#detailed-status-by-role), [known issues](#known-issues), and [open-work list](docs/OPEN_WORK.md) for current limits. The [project wiki](wiki/README.md) gives a page-by-page overview (features, cvars, commands, code map, build and test).

---

## Features

| Role | Highlights |
|---|---|
| **Navigation** | Waypoint-free navmesh routing that dynamically detours around match-built structures and recovers from stalls. |
| **Commander** | Takes the CV when vacant, builds the base & vehicle factory, assigns engineers, researches tech, and directs infantry squads. |
| **Engineers** | Build and repair structures & damaged vehicle armor, revive teammates, deploy refinery turrets, wall refineries off and put a radar/camera behind the wall, or serve as buyable **EngiBots**. |
| **Infantry** | All four classes with loadouts & skills; capture flags, pick tactical spawns, counter turrets, and attack buildings or vehicles. |
| **Vehicles** | Purchase upgraded chassis/weapons/armor as tech unlocks, drive with three-point turns, retreat to repair/rearm, and field mobile APC spawns. |

### Detailed Status by Role

> **Legend:** `✅` works in matches · `🟡` partial / not yet reliable · `⬜` planned

<details>
<summary><strong>🧭 Movement & Navigation</strong> — <code>6 ✅</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **Waypoint-free navigation** | Generated navmesh; routes around buildings placed during the match and ends at the nearest walkable spot outside solid building walls when a goal lies inside a structure |
| ✅ | **Stuck recovery** | Detects stalls (including flush building contacts), backs off, and tries another route; avoids blocked links near buildings for 60 s and applies a soft 15 s infantry-only edge penalty for vehicles without blocking vehicle routing |
| ✅ | **Move orders** | Finishes within the game's 100-unit tolerance; stops at the nearest reachable point on the same level for off-mesh targets |
| ✅ | **Doors and gates** | Blocks closed `func_door*`, `prop_door*`, and `func_movelinear` gates; reserves door buttons only when a front-side line-of-sight stand is reachable within 72 HU, releases claims on death, disambiguates duplicate button names by position (`emp_mvalley`), and skips timed auto-closing buttons (e.g. Crossroads 15 s gates); verified all three `as_escort` doors (including the `func_movelinear` pillar tunnel). Live built-state detection for `emp_eng_map_model` walls and `emp_eng_map_brush` ramps (Crossroads built ramps detoured for vehicles and infantry, unbuilt holograms ignored by obstacle traces, vehicle waypoint sightline check at gate center pillars) |
| ✅ | **Navmesh coverage** | All 22 multiplayer maps in the test inventory have a generated navmesh. `emp_mvalley` loads its 65,621-area mesh; unbuilt `emp_eng_map_brush` bridges/ramps whose mesh areas lack solid ground underneath are blocked until built, so both NF and BE reach the dam resource point without stalling in the ramp well (routing after engineer construction remains open). A route scan rejects the clipped dam crossing for vehicles. District completed a map change after the NF ticket depletion test. Sudden death remains unverified |
| ✅ | **Water** | Surfaces for air and avoids deep water when a land route exists. Swims to ladders that end in water, climbs onto boat decks to capture (`emp_palmbay`), and jumps back into the water afterwards |

</details>

<details>
<summary><strong>🎖️ Commander</strong> — <code>11 ✅</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **Auto-command** | Takes the command vehicle when no human commands |
| ✅ | **Base building** | Places refineries, barracks, vehicle factory, armory, and repair pad (the pad right after the factory is placed); gets out as an engineer to help build. A damaged finished building no longer blocks new placements; resource points nearer the enemy CV rank lower |
| ✅ | **Engineer dispatch** | Sends engineers to unfinished or damaged buildings by priority tier (core: refinery, radar, vehicle factory, first barracks; front: forward barracks, repair pad, armory, damaged core; comfort: turrets, walls, cameras). A core or front order pulls an engineer off a lower-tier job and is never given up silently (retried every 20 s with a `stall-escalate` line); stalled engineers try another side, then release the job *(10-bot Crossroads match at 1×: longest unaccepted order 922 s → 32 s, longest core build without progress 385 s → 60 s)* |
| ✅ | **Vehicle factory placement** | Reserves a clear spot near the CV with the exit facing the enemy, or drives to a remote site |
| ✅ | **CV survival** | Pulls back from nearby enemy vehicles or heavy fire |
| ✅ | **Research** | Radar, Structural Engineering, and Vehicle Factory first, then turrets, APC spawn, chassis, armor, engines, and heavy tanks; resumes paused research and resets on radar loss |
| ✅ | **Expansion & defense** | Forward barracks near remote factories and the front (own half only; with a single engineer the factory goes first and the barracks follows beside it), plus a forward repair pad beside a finished barracks or factory well ahead of every own pad, and up to two MG and two ML base turrets |
| ✅ | **Human handover** | Yields the seat when a human enters or uses the CV *(confirmed in an 11v11 human test)* |
| ✅ | **Driving the CV** | Drives to distant build sites and slows to about 250 u/s in the last 800 units, so it no longer overshoots the site onto slopes; gives up after 25 s without progress and builds from its current spot. A retreat from enemy vehicles only picks a spot the CV has a real vehicle route to (no long detour), so it no longer stalls against the Crossroads NF base edge *(build-site drives arrived in all checked runs since the slowdown; forced BE APC rush on Crossroads: NF CV retreated 1700 units and arrived in 6 s, where it stalled in 4 of 5 earlier retreats)* |
| ✅ | **Team-director orders** | Splits free infantry into push / hold / defense (~50 / 30 / 20) via *attack location* orders. All three roles received orders in 10-bot Crossroads matches. Human orders take priority. A flag with assigned living attackers is avoided for 120 seconds after 180 seconds without a 128-unit advance or a new friendly entry into its capture zone. Losing a core building keeps the team stabilizing for 60 seconds even if the CV recovers sooner |
| ✅ | **Strategic adaptation** | Weighs refinery income against distance, accounts for the engineer’s walk when sending an escort, and replans construction when buildings are lost |

</details>

<details>
<summary><strong>🔧 Engineers</strong> — <code>6 ✅</code> · <code>2 🟡</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **Build and repair** | Constructs and repairs buildings and vehicles with the repair upgrade active from the first life (one engineer builds a vehicle factory in about 45 s instead of about 120 s) (targeting the damaged armor side), and deconstructs destroyed vehicle carcasses blocking the vehicle factory bay or sitting in base to recover resources (while grenadiers move to clear live enemy vehicles parked in a friendly factory bay) |
| ✅ | **Revive** | Beyond immediate revive range, engineers require a route ending within 120 units of the body before claiming it. After six game seconds without progress they replan, then release the target after another six stalled seconds. Downed bots wait up to three game seconds for a nearby engineer who can search before that window closes. An active revive can delay normal respawn until ten game seconds after death. A short test confirmed the respawn hold followed by a successful revive. The full route retry and timeout sequence still needs gameplay verification |
| ✅ | **Skill distribution** | Repair and revive skills are split across the team's engineers |
| ✅ | **Heal teammates** | Engineers out of combat heal a visible hurt teammate within 700 units with the kit (clears the deployable selection, keeps a revive charge with the Revive skill); a hurt bot waits up to 8 seconds for the engineer unless busy with commander duty, Lawliet, revive, building, or ammo crates. Seen in 10-bot matches (25 → 100 HP in 6 s; 37 → 100 HP in 3.5 s) |
| ✅ | **Turret upgrades** | Rank 30+ engineers upgrade their own turrets up to the map limit. Natural rank gain (`0 → 30+`) followed by level `1 → 2 → 3` upgrades was observed in three full 21-/22-bot Crossroads matches (alongside the scripted rank-up test) |
| 🟡 | **Refinery turrets** | Engineers check that the route ends within building range before claiming a refinery turret stand; each refinery gets an MG turret first and an ML turret (kit item 2) second (on `emp_rainbowroad` up to 1 MG + 4 ML in a defensive line across the lane), with both MG and ML turrets upgraded 1 → 2 → 3 once rank 30+ allows it. Occasional placement failures remain |
| 🟡 | **EngiBots** | Buyable engineer bots for human commanders — see [EngiBots](#engibots-for-human-commanders) |
| ✅ | **Walls, radar, cameras** | At each own refinery (nearest first, a second engineer takes the next one) an engineer builds a three-part wall on the side facing the enemy CV — a middle piece across the line to the enemy and two wings bent back — then places a camera behind it — and a radar too once its rank gives a second detection slot — and builds everything up to full health. Walls only at refineries (engineer walls cost no resources); points the game rejects are retried shorter, and the standing pieces are kept (`rcbot_empires_engineer_deployables`) |

</details>

<details>
<summary><strong>🪖 Infantry</strong> — <code>14 ✅ · 7 🟡</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **All four classes** | Scout, Rifleman, Grenadier, and Engineer with multiple loadouts and class skills |
| ✅ | **Combat** | Picks targets and engages at weapon range (pistols ~1000 up to snipers 4500); navmesh cover under fire awaits match verification, and class spacing remains open |
| ✅ | **Skirmish movement** | Inside its class and weapon band (shotgun 110–380, SMG 180–650, rifle/HMG 300–1050, scoped sniper 550–1600 units; grenadiers at most 880, RPG against vehicles 550–1300) a bot side-steps away from the enemy's aim in short irregular bursts, backs off when too close, reloading, badly hurt or outnumbered, presses in when its side is ahead, and breaks line of sight to a close safe spot to reload. Every step is snapped to the navmesh and hull-traced; blocked steps escalate to standing, then to the old approach (`rcbot_empires_skirmish`, `_fight_odds`, `_fight_cover`) *(39 movement steps in one 10-bot Crossroads match at 1×, with no skirmish stall escalation. Later matches also logged cover selection while reloading or hurt)* |
| ✅ | **Joining fights & callouts** | Free infantry walks to a teammate's fight within 2000 units (max 2 helpers per fight); bots spot a group of enemies or a close enemy for the team, at most once per team per minute (`rcbot_empires_join_fight`, `rcbot_empires_callouts`). In two 10-bot Crossroads matches at 1×, helpers reached the reported fight once and twice, and bots issued four and eight enemy callouts, respectively |
| ✅ | **Classes in combat & calm look** | Scouts circle to the other side of an enemy a teammate fights and spot far enemies (1000–4000 units) with the binoculars; Grenadiers with rockets keep 550–1300 units from an enemy vehicle and at least 400 units from soldiers, and break off any task, commander orders included, for an RPG shot at an enemy vehicle in sight within 300–2600 units (only an enemy within 500 units interrupts the shot; the order resumes afterwards); Engineers stay behind the teammate closest to the enemy. A bot standing without a route looks toward the last enemy or calmly around its facing instead of at random points (`rcbot_empires_scout_flank`, `_scout_binoculars`, `_grenadier_rpg`, `_grenadier_rpg_vehicle_priority`, `_engineer_rear`, `_calm_look`) *(Forced APC rush, 5 vs 5, 1x: before 0 RPG shots at vehicles in two runs, after 14 and 32 shots with 13 and 30 hits. 10-bot Crossroads match: 7 RPG shots at vehicles, 7 hits, vs 0 in the previous match. Calm look: about 600 idle picks per run, level turns at most 30°)* |
| ✅ | **Flag capture** | Captures objectives on maps with capture points |
| ✅ | **Turret avoidance** | Retreats out of enemy turret firing range |
| ✅ | **Smart spawning** | Spawns near threatened buildings on base alerts, at the VF for drivers, or at friendly APCs / forward barracks; exits blocked APCs |
| ✅ | **Anti-structure** | Grenadiers use RPG/mortar, scouts sabotage *(confirmed)*; rifleman sticky grenades are kept for vehicles and the enemy CV |
| ✅ | **Fewer idle guards** | Without an alert no free infantry bot stands guard at the CV or holds a building on the own side; all go forward, and a base alert (enemy seen or damage) still pulls defenders back at once. A bot holding a defend or sector spot walks to the next spot around it every 12 s instead of standing 30 s (`rcbot_empires_lean_defense`, `rcbot_empires_defend_patrol`) Two 15-minute Crossroads pairs at 1× recorded 1,154 vs 1,422 bot-seconds idle and 859 vs 2,860 with the settings on vs off. The second pair also recorded 48 vs 0 bot-seconds stuck |
| 🟡 | **Learns from losses** | Three deaths of a team on foot within 30 s and 700 units mark the spot as hot for 90 s; the next wave's routes avoid it (`rcbot_empires_hot_zones`) *(natural creation, extension, and expiry occurred across 33 multi-bot matches, up to 23–26 times in 21-bot matches; in-match route detouring is not separately logged, detouring verified in a 5-pair kill-zone A/B test: 41 vs 84 deaths)* |
| ✅ | **Self-preservation** | When two teammates are killed by the enemy within 8 s and 600 units of a bot, it falls back 300-450 units from the killer (preferably out of his sight) or backs off out of the fight for 5 s instead of walking on (`rcbot_empires_self_preservation`) *(scripted test: 316 units back; observed naturally in 21 multi-bot matches including 10-bot Palmbay and 21-/22-bot Crossroads/Rainbowroad runs, with hidden fallback spots, completed 5 s retreats, and in-combat `shaken` cover)* |
| ✅ | **Tank escort** | One free engineer per team walks with an own driven tank about 350 units behind it; the tank waits up to 15 s on its way to the front while its engineer is more than 1500 units behind, and holds still (up to 30 s) while that engineer repairs it (`rcbot_empires_tank_escort`) *(confirmed in full matches: 48 multi-bot runs with escort starts, 12 with escort repair calls, and `repair-hold` + hull/armor repair to `repair-done` in 22-bot Crossroads matches, e.g. 117 → 250 HP)* |
| 🟡 | **APC cover park** | On maps without a contested flag (e.g. `emp_crossroads`), the spawn APC parks out of sight 1000–2300 units before the nearest enemy building instead of pushing into the base, else waits at a staging point 2500 units before it (`rcbot_empires_apc_cover_park`). The spawn APC never takes a push or enemy-CV goal and does not close in on a target like a tank; it leaves a park spot where it lost 15 % of hull and armor within 15 s and retreats to a repair pad below 60 % hull (`rcbot_empires_apc_retreat_hull`) *(hidden parking reached and held in 10-bot Crossroads runs with `fair_intel 0`; arrival/parking at hidden spots under the current `fair_intel 1` default and `apc-park reason=damaged` remain open)* |
| 🟡 | **Helps human tanks** | A free engineer bot repairs a damaged, standing own vehicle within 1200 units unasked when a human drives it or it is parked empty (`rcbot_empires_buddy_repair`) *(scripted test: empty damaged tank repaired to full; no trigger in a bot-only match yet)* |
| 🟡 | **Seeks a medic** | A bot below 25 % health with no enemy in sight walks to the nearest team engineer within 3000 units and waits there to be healed (`rcbot_empires_seek_medic`) *(scripted test: rifleman at 20 HP walked 1474 units, healed to 90 in 9.7 s)* |
| 🟡 | **Humans first** | Engineers heal hurt humans before bots (and leave a bot patient for a hurt human in reach), serve human ammo calls first *(scripted test: the first free engineer took the marked human tank 7846 units away before a nearer bot tank)*, and a damaged human-driven vehicle nearby takes over a bot's repair call (`rcbot_empires_humans_first`) *(scripted tests with a bot marked as human: switched from the bot, healed the human 42 -> 100, then went back; the human tank took over a standing bot repair call, the bot call waited and was repaired afterwards)* |
| ✅ | **Tactical respawn** | Defenders far from an alert or drivers far from the factory may respawn closer (`rcbot_empires_kill_spawn`). In two Palmbay matches, factory buyers selected respawn locations calculated to save about 4,900 and 6,400 units. In a Crossroads base-alert test, the defender respawned about 670 units from the CV after dying about 11,360 units away. The tactical search recognizes barracks spawn points regardless of classname capitalization |
| 🟡 | **Anti-vehicle** | Grenadiers track APC hull health when deciding whether to continue an RPG attack. RPG and sticky-grenade selection skips stale inventory handles after a weapon slot is reused. Riflemen sticky-grenade vehicles and the enemy CV only *(scripted test: two sticky projectiles stuck on the BE CV and went off after the 6 s fuse)*. Bots switch to owned weapons like a client (`weaponselect`); before, every weapon selection created a new weapon with one free grenade, so bots never ran out (`rcbot_empires_select_owned_weapon`); scouts approach slow vehicles with stun grenades (up to 150 HU/s using the vehicle’s horizontal velocity) and can throw at faster vehicles already within 250 HU *(throw triggers logged in full matches, but hits/stun effect and grenade ammo consumption are not yet confirmed)* |
| 🟡 | **Guard & attack orders** | Infantry follow guarded units and accept attack orders; engineers restore guarded buildings. Human commander Move, Guard, and Attack orders were confirmed in an 11v11 test (order abort, attack-target, and "Roger"/"Negative" replies remain open) |
| 🟡 | **Squads, mines, binoculars** | Implemented, rarely used |
| ✅ | **Seismic grenades** | Engineers lob seismics at enemy buildings, turrets, cameras/radars, walls, and built destructible map obstacles (`emp_eng_map_model`, skipping `GrenadierOnly`), aiming in front of the vehicle collision box; they use a standing throw up to 1550 HU and a running throw (`run=1`) up to 2250 HU (past an MG turret's 2000-HU firing range), give up after five throws without damage, and holster afterwards. Confirmed against barracks (`250 → 150 HP`), long-range running throws (`1989 HU`, `250 → 230 HP`), and enemy cameras (`1861 HU`, `21 → 7 HP`), plus refinery damage in 10-bot Crossroads matches |

</details>

<details>
<summary><strong>🚜 Vehicles</strong> — <code>6 ✅</code> · <code>4 🟡</code></summary>

<br>

| Status | Feature | Notes |
|:---:|---|---|
| ✅ | **Purchasing** | Buys at built or map-placed vehicle factories; drives blocking empty vehicles out of the bay after observing an empty driver seat for five seconds. Movement over 100 units from the start of that interval restarts the timer. A nearby human teammate within 250 units keeps priority in the bay even after 30 seconds. Engine-reported bots from other plugins do not reserve the vehicle. The updated timer and human priority await a handover check |
| ✅ | **Tech-scaled loadouts** | Upgrades chassis, weapons, armor, and engines as research & budget allow; keeps one APC at the front when APC Spawn is researched |
| ✅ | **Driving** | Follows the navmesh, executes three-point turns, routes around buildings, building vehicle physboxes, and solid map props (trees, roadblocks) for all vehicle classes, and avoids drop-offs or water too deep for the chassis. Regulates speed ahead of corners using arc-length Menger curvature (`rcbot_empires_vehicle_curve_speed`), scans ahead for tight passages including static props and gate pillars, lines up onto the passage axis and brakes actively when arriving off-axis (`rcbot_empires_vehicle_context_steer`). Vehicle routes check active map vehicle clips and reject partial routes whose continuation is blocked by a clip |
| ✅ | **Retreat & repair** | Returns to the repair pad when damaged. A tank that moves less than 96 units from its last progress mark in eight seconds on the approach tries to free itself, even when rocking registers as speed. If repairs finish during a refit, it finishes sending the commands while it remains on the pad. Where the route ends at the navmesh edge in front of the pad ramp, it drives straight onto the pad instead of stopping there. Without a pad, checks for a full vehicle route to a parking spot near the friendly CV. If all eight spots fail, waits locally and uses the existing retreat retry limit (pending gameplay verification) |
| ✅ | **Roadkills** | Runs over enemy infantry |
| ✅ | **Vehicle combat** | Cannon leads moving targets; hitscan MG tracks infantry up to 1200; shells buildings from 550–1500; angles strongest armor toward threats. Tanks carry a missile launcher in fire group 1 (with cannon and MG) and switch to group 2 (cannon and MG only) against infantry, back to group 1 against vehicles and buildings (`rcbot_empires_vehicle_missiles`). All turret weapons are aimed from the cannon pivot, because the game converges them on the cannon axis (`rcbot_empires_vehicle_cannon_eye`; NF heavy MG in a hold test: 0 → 7 HP/s per fire second) |
| 🟡 | **Target priority** | Defend base (immediate CV threats first) → hunt spotted tanks → support front → push enemy buildings → enemy CV *(CV-threat override over APCs awaits match verification)* |
| 🟡 | **Rearming** | Drives to an ammo crate or repair pad, or calls an engineer for ammo; Light/AFV, Medium, and Heavy tanks can retreat to refit and rearm. A heavy tank refilled its cannon from 0 to 40 rounds at a crate in a 10-bot Crossroads match. In a two-tank test, both heavy tanks called for ammo and one resupplied. A medium tank recovered cannon ammo after a pad refit in a separate 10-bot match. The retreat hit its timeout. Two callers refilling in the same run remains open |
| 🟡 | **Lawliet mode** | When a team falls clearly behind, a crewed APC (Scout, Rifleman, Engineer, plus a driver who stays in the APC and holds near the enemy CV) rushes the enemy CV: Scouts re-stun it about every 10 s, Riflemen throw sticky grenades, the Engineer drops an ammo crate next to it and the crew resupplies grenades there (`rcbot_empires_lawliet_driver_stays`) *(forced Crossroads run at 1×: crate 25 s after arrival, rifleman sticky 0 -> 1 at the crate; crew engineers are no longer pulled away by commander build jobs; auto trigger awaits match verification)* |
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
   - **Auto-fill / Bot quota:** edit `addons/rcbot2/config/bot_quota.ini` (e.g. `Humans "12" => Bots "0"`) to automatically adjust bot count as players join. With `rcbot_wait_for_human_team 1` (default) automatic filling (quota and `min_bots`/`max_bots`) starts only once a human has joined a team, so bots do not spawn while the first player is still in the team menu; `rcbotd addbot` is never held back.

> [!NOTE]
> - **Windows:** Untested in game; features that read fixed offsets in the Linux server binary fall back automatically ([docs/WINDOWS.md](docs/WINDOWS.md)).
> - **Metamod version:** A plugin built against Metamod 2.0 headers is rejected by Metamod 1.12 — see [Build](#build).

---

## EngiBots for Human Commanders

Human commanders can buy dedicated base-building engineer bots in chat with `/buildengibot` or `!buildengibot` (or `buildengibot` in console).

- **Cost & limit:** `rcbot_empires_engibot_cost` team resources (default `100`), up to `rcbot_empires_engibot_limit` per team (default `2`). Status feedback appears on screen (e.g. `EngiBot ready (1/2)`, or why a purchase failed).
- **Names:** the two EngiBots of a team are called *Builder Bob* and *Fix It Felix*; with a higher limit further ones are numbered.
- **Base duty:** Builds and repairs unfinished/damaged buildings and friendly vehicles within 2500 units of the CV (including human-placed structures).
- **Idle behavior:** Waits ~900 units from the CV so building spots stay clear.
- **Orders:** *Attack location* builds an MG turret at the target; *Guard* on a building builds or repairs it.
- **Strict scope:** Never captures flags, roams, enters vehicles, takes command, or chases enemies. Automatically leaves on map change and is ignored by both `bot_quota` and `min_bots`/`max_bots`. Chat `/buildengibot` and `!buildengibot` are also handled before chat-trigger plugins can swallow them, with properly formatted Empires `HintText`/`SayText` replies.

> [!NOTE]
> EngiBots have so far only been tested with the bot commander (`scripts/dev_engibot.sh`), not yet with a human commander.

---

## Settings

> [!TIP]
> Bots will hand over their wages to any teammate who types `wages` in chat.

### Bot names

Bots get a fixed name in Battlefield style for the whole match: a tag, an initial and a plain surname from their faction's list (NF e.g. `[AI] M. Berg`, BE e.g. `[AI] J. Morales`). The class is not part of the name because roles change. A bot without a team is `Bot <n>` until it joins; a team switch gives it a name from the new faction's list. Names passed to `addbot` or set in bot profiles stay.

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_bot_name_tag` | `[AI]` | Tag in front of generated names; empty = no tag. Applies to bots named after the change |

### Commander

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_auto_commander` | `1` | Bot takes the command vehicle when no human commands |
| `rcbot_empires_commander_engineer` | `1` | Bot commander spawns as an engineer and builds near the CV |
| `rcbot_empires_commander_orders` | `1` | Bot commander issues team-director orders to free infantry |
| `rcbot_empires_fair_intel` | `1` | Fair team knowledge: team director, commander, infantry goals, tank/APC goals and the Lawliet rush use only enemies the team could know (game spotting, own radar 1800 / camera 1200 with line of sight, a living teammate's sight up to 3500, bot sight reports; memory 10 s infantry, 20 s vehicles, 180 s buildings) plus the enemy base's start position (tanks and the Lawliet rush drive to the last known CV position, else that start position). Base alerts on building damage stay immediate. `0` off (bots read the full entity list), `1` both teams, `2` NF only, `3` BE only |
| `rcbot_empires_threat_map` | `1` | Lean threat map per team (512-unit grid, rebuilt every 1.5 s from fair intel only: known infantry, tanks/APCs, finished enemy MG turrets with their level range, hot zones, own building damage; needs `rcbot_empires_fair_intel`). On: hurt and fight cover prefer low-threat spots and, without a hidden spot, back off to a clearly lower threat; an engineer stalled against a known enemy vehicle gives the job up and the team holds that building for 30–120 s instead of sending the next engineer into the same spot. Its consumers trigger rarely with 10 bots (one hurt cover in a 12-minute match); `scripts/dev_threat_ab.sh` compares off/on. Logs `threat-map`, `cover start … route=threat-retreat`, `cover end`, `stall-escalate who=engineer … reason=enemy-vehicle`, `commander-econ defer` |
| `rcbot_empires_commander_drive_from` | `1000` | Distance beyond which the commander drives the CV to build sites |
| `rcbot_empires_commander_boomtank` | `1` | Commander runs over base intruders (`0` off, `1` on except rainbowroad, `2` everywhere) |
| `rcbot_empires_commander_build_lift` | `1` | The commander's `emp_build` request is lifted above the ground (turrets 14, other buildings 10 units) so bot buildings stand where a human commander's do; at the ground they had ended up that far sunk in |

### Engineers & EngiBots

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_engineer_turrets` | `1` | Engineers build MG and ML turrets at friendly refineries |
| `rcbot_empires_engineer_deployables` | `3` | Refinery fortification (bits): `1` camera behind the wall (plus radar with a second detection slot), `2` three-part wall on the enemy side |
| `rcbot_empires_engineer_help_human` | `2` | Engineer bots per team that autonomously build/repair near a human commander's CV |
| `rcbot_empires_engibot` | `1` | Allow commanders to purchase EngiBots |
| `rcbot_empires_engibot_cost` | `100` | Team resources per EngiBot |
| `rcbot_empires_engibot_limit` | `2` | Maximum EngiBots per team (`0`–`8`) |
| `rcbot_empires_engibot_leash` | `2500` | Operating radius around the team CV |
| `rcbot_empires_vacancy_revive` | `1` | Allow bots to run the death-vacancy Engineer Mass Revive transaction (promote/revive/restore) once per vacancy epoch |

### Vehicles & Tactics

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_vehicles` | `1` | Bots buy, drive, and fight with vehicles |
| `rcbot_empires_vehicle_drivers` | `-1` | Max vehicle drivers per team (`-1` = 40% of team bots, minimum 2) |
| `rcbot_empires_billnye` | `-1` | Bill Nye ramming mode (`-1` NF on rainbowroad, `0` off, `2` NF, `3` BE) |
| `rcbot_empires_lawliet` | `1` | Lawliet mode, a desperation APC rush on the enemy CV (`0` off, `1` auto when behind in research & map control or tickets, `2` force NF, `3` force BE) |
| `rcbot_empires_vehicle_mg_no_damage` | `1` | A vehicle MG that fires 1.2 s at a soldier without damage aims at the body on the ground (e.g. a soldier revived lying); after 3 s without damage it takes another target for 10 s |
| `rcbot_empires_vehicle_runover` | `1` | An armed vehicle runs a soldier over when its MG did no damage in 2 s or he is below the gun, if the straight way (< 1500 units, level, ground ahead) is safe; at most 6 s, then it leaves him for 10 s |
| `rcbot_empires_lean_defense` | `2` | `2` = no standing base defender and no hold goal on the own side without an alert; `1` = standing defender only from 5 free infantry bots; `0` = from 3 |
| `rcbot_empires_squad_flanks` | `0` | `1` = from 3 free infantry bots per team the director picks one push and one hold goal left and one right of the axis between both bases (each at least 1000 units off it, else rank order as before); odd squads (Alpha, Charlie) take the left goal, even squads (Bravo, Delta) the right one; bots without a squad or in a human-led squad are spread as before |
| `rcbot_empires_defend_patrol` | `1` | Defend/sector spot held 12 s, then the next spot around it (arrive within 100); 0 = 30 s hold, 250 radius |
| `rcbot_empires_hot_zones` | `1` | Hot zones: 3 deaths on foot within 30 s / 700 units make the spot hot for 90 s (renewed by each death there); routes pay to enter and per unit inside |
| `rcbot_empires_self_preservation` | `1` | Two teammates killed by the enemy within 8 s / 600 units of a bot: it falls back from the killer or backs off out of the fight for 5 s (20 s cooldown) |
| `rcbot_empires_vehicle_missiles` | `1` | Tanks buy the Standard Missile Launcher (fire group 1 only; the NF light tank drops one rear armor plate for the weight limit); the driver selects group 1 against vehicles and buildings, group 2 against infantry |
| `rcbot_empires_vehicle_cannon_eye` | `1` | Aim turret weapons from the cannon pivot (the game fires every turret weapon at the point the cannon axis hits), resolved once per tick and also used for target sight and the fight movement's fire line; `0` = old aim from hull origin + 90 |
| `rcbot_empires_vehicle_turret_align` | `1` | Fire once the turret yaw is within 8° (MG on infantry) or 12° (cannon) of the target and the aim is older than 0.4 s, instead of a blind 1.2 s; after 2.5 s fire anyway with a `stall-escalate who=vehicle-aim` line. Chassis without a rotatable turret keep the 1.2 s delay. A fire-group switch holds fire in its tick and the 0.35 s after it; ammo counts span all groups, and a group with only missiles loaded leads at 2300 u/s without drop |
| `rcbot_empires_vehicle_pitch_limit` | `1` | Hold fire and back off while the target is below the cannon's pitch limits (every chassis has its own minimum range, e.g. NF heavy about 570 units against infantry) |
| `rcbot_empires_vehicle_keep_route` | `1` | When replanning finds no path to the same goal (e.g. after sliding off the route), keep driving the last route instead of heading straight at the goal |
| `rcbot_empires_vehicle_deep_water` | `1` | Steering edge checks treat water deeper than the vehicle can ford as a drop (the game drowns a vehicle whose body centre is under water) |
| `rcbot_empires_nav_lip_escape` | `1` | A bot stuck on a building edge (feet blocked, hip height free) or inside a building box walks out along the box axis (checking foot and step height, ducking when airborne) or away from the hit face; after 5 consecutive immobile airborne stucks at the exact same spot, escalates to a clean self-respawn (`geometry-wedged-respawn`, 60 s cooldown) |
| `rcbot_empires_nav_enemy_vehicle_block` | `1` | Infantry stuck against an enemy vehicle blocks the mesh links through its box for 60 s for every bot (own vehicles keep the soft 15 s edge penalty) |
| `rcbot_empires_nav_detour_fast` | `1` | On-demand building-detour grid shared per building for 15 s (`0` = old full-grid planner, `2` = both with `nav-detour-compare` log) |
| `rcbot_empires_vehicle_wall_clearance` | `1` | Vehicle route points keep half a width off walls running along the route, centre in narrow lanes, and add a straight approach point before them |
| `rcbot_empires_vehicle_narrow_cost` | `1500` | Fixed navmesh route cost per area narrower than the vehicle, so short pillar gaps do not beat wide lanes |
| `rcbot_empires_vehicle_context_steer` | `1` | Context steering at tight passages: scans ahead up to 1100 units along the route (world, static props, solid buildings and gate pillars), caps lookahead before the passage, aligns onto its axis, brakes actively when arriving off-axis, and scores 16 directions against front-corner traces |
| `rcbot_empires_vehicle_curve_speed` | `1` | Anticipatory curve speed along the route for all chassis using arc-length Menger curvature (catches multi-segment bends and brakes before corner entry; `0` = off) |
| `rcbot_empires_engineer_first_perk_now` | `1` | An engineer's slot-1 perk (repair or revive) is active from its first life, as if picked before spawning; the repair upgrade doubles build speed |
| `rcbot_empires_vehicle_vf_grace` | `90` | Infantry opening: seconds after a team's first finished vehicle factory in a map before its bots start a vehicle purchase; base building, research and infantry go on. Once per map (a rebuilt factory starts no new wait); a known living enemy vehicle or APC ends it at once; `0` = off. The vehicle test scripts set `0` |
| `rcbot_empires_tank_escort` | `1` | One free engineer per team escorts an own driven tank (≤ 3000 units to start), repairs it when it stands damaged; the tank waits for it on the way to the front |
| `rcbot_empires_apc_cover_park` | `1` | Without a contested flag, the spawn APC parks out of sight of the nearest enemy building (else waits 2500 units before it) instead of pushing in; park and waiting spots keep 800 units from own buildings and 400 from other own vehicles |
| `rcbot_empires_apc_retreat_hull` | `0.6` | Hull share below which the spawn APC retreats to a repair pad (tanks keep 0.4) |
| `rcbot_empires_buddy_repair` | `1` | A free engineer bot repairs a damaged, standing own vehicle within 1200 units when a human drives it or it is empty (hull < 90 % or an armor side < 85 %); `2` also bot-driven (test) |
| `rcbot_empires_seek_medic` | `0.25` | Health fraction below which a bot out of combat walks to the nearest team engineer and waits to be healed (`0` = off) |
| `rcbot_empires_pad_stall_escalate` | `1` | A vehicle stalled on the way to a repair pad tries the next pad entrance after 2 stalls and gives the retreat up after 4 (60 s retreat cooldown) |
| `rcbot_empires_humans_first` | `1` | Humans come first for heal, ammo and repair (see above) |
| `rcbot_empires_test_human_userid` | `0` | Test only: the bot with this userid counts as a human for `humans_first` and `buddy_repair` |
| `rcbot_empires_test_human_hold` | `0` | Test only: that bot holds still in its vehicle and makes no calls, like a parked player |
| `rcbot_empires_select_owned_weapon` | `1` | Switch to owned weapons by `weaponselect`; `0` = old `SetActiveWeapon`, which creates a new weapon with free ammo on every call |
| `rcbot_empires_lawliet_driver_stays` | `1` | Lawliet: the APC driver stays seated near the enemy CV while the crew fights on foot |
| `rcbot_empires_kill_spawn` | `1` | Tactical respawn closer to goal (≥ 3800 units closer, 6 s out of combat, tickets > 20 & > 25%, max once per 90 s) |

### Infantry Combat

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_skirmish` | `1` | Side-step and keep an effective distance in a fight instead of running at the enemy (`2` NF only, `3` BE only, for comparisons) |
| `rcbot_empires_fight_odds` | `1` | Weigh visible enemies against nearby teammates: give ground when outnumbered, press in when ahead |
| `rcbot_empires_fight_cover` | `1` | Reloading, badly hurt or outnumbered: break line of sight to a close safe spot while still fighting |
| `rcbot_empires_navmesh_hurt_cover` | `1` | With a navmesh, losing 35 % of max health within 2 s outside a skirmish takes a short navmesh cover (at most 6 s, then 9 s cooldown; spots sideways, diagonal or back, hidden from the shooter or the damage origin) instead of the old waypoint hide spot, and the bot keeps firing at a visible attacker unless reloading; a director push/hold/defense order pauses for the cover, build, revive, vehicle and commander work suppress it |
| `rcbot_empires_health_poll_hurt` | `1` | Empires sends no `player_hurt` event, so the bot compares its own health every think and treats a drop of at least 3 HP (not fall/world damage, not in a vehicle) as a hit; the current enemy counts as attacker only if seen in the last 1.5 s. Wakes up hurt cover, look-at-damage and the engineer damage interrupt (an unseen graze under 20 % keeps the engineer working; a build assignment is never interrupted). `0` = no hit reaction, as before |
| `rcbot_empires_join_fight` | `1` | Free infantry joins a teammate's fight within 2000 units (max 2 helpers) |
| `rcbot_empires_callouts` | `1` | Spot a group or a close enemy for the team (once per team per minute, per bot every 3 minutes) |
| `rcbot_empires_scout_flank` | `1` | A scout circles to the other side of an enemy a teammate fights (up to 1200 units) |
| `rcbot_empires_scout_binoculars` | `1` | A scout spots an unspotted enemy 1000–4000 units away with the binoculars (per bot every 45 s, per team every 20 s) |
| `rcbot_empires_grenadier_rpg` | `1` | A grenadier with rockets keeps 550–1300 units from an enemy vehicle and fires the RPG; at least 400 units from soldiers |
| `rcbot_empires_grenadier_rpg_vehicle_priority` | `1` | A grenadier with rockets breaks off any task (commander orders included) for an RPG shot at an enemy vehicle in sight within 300–2600 units |
| `rcbot_empires_engineer_rear` | `1` | An engineer in a fight stays behind the teammate closest to the enemy (up to 1600 units) |
| `rcbot_empires_calm_look` | `1` | A bot standing without a route looks toward the last enemy (20 s) or calmly around its facing, not at random points |
| `rcbot_empires_move_look_own_sight` | `1` | A walking bot turns its view toward the last enemy only if it saw him itself; a teammate's report still sends it there but it keeps looking ahead |
| `rcbot_empires_smooth_look` | `1` | Decoupled critically-damped pitch/yaw spring-damper turning and content-based look priorities for infantry (fixes shared pitch/yaw momentum wobble) |
| `rcbot_empires_nav_lookahead` | `1` | Walking navmesh infantry looks smoothly ahead along its route kept level on slopes, holds lost-LOS corners at chest height for 2.5 s, and bounds last-enemy extrapolation |
| `rcbot_empires_smart_idle_look` | `1` | Standing infantry scans open sightlines instead of staring into walls (< 140 units), adds subtle idle drift, and lifts noise look to chest height |
| `rcbot_empires_combat_polish` | `1` | Proactive tactical reload outside combat (< 60% clip after 2.5 s without an enemy), asymmetric fire-alignment gate so bots do not fire while turning away, and long-range automatic bursts (> 750 units) |
| `rcbot_empires_target_keep` | `600` | Units a newly seen enemy must be closer than the current target before the bot switches (no flicking between enemies firing in bursts) |
| `rcbot_empires_target_switch_reaction` | `1` | A target switch during a fight waits 0.05–0.15 s instead of the first-sight reaction of 0.20–0.55 s |
| `rcbot_empires_hurt_look` | `1` | A bot hit by a shooter it does not see turns toward the damage origin for 1–1.5 s; seeing an enemy ends the turn at once |
| `rcbot_empires_revive_getup` | `2` | A revived bot (revived lying) presses duck until it stands (`1` jump, `2` duck, `3` both; jump does not get it up) |
| `rcbot_empires_sniper_aim` | `1` | A bot with a scoped rifle scopes in from about 700 units, crouches, lets its aim settle 0.6–1.1 s on a new target and fires at most every 1.7–2.2 s; the aim itself is unchanged |

### Diagnostics & Offsets

| Cvar | Default | Description |
|---|---:|---|
| `rcbot_empires_debug_log` | `0` | `1` = verbose bot diagnostics in the server log, including the `match-flow` team status every 30 game seconds and its events (see `MATCH_FLOW.md`) |
| `rcbot_empires_force_offsets` | `0` | Force fixed-offset reads on unknown `server_srv.so` builds (`1` = enable; ignored on Windows) |

---

## Known Issues

- **Infantry stuck spots:** Bots can get stuck inside barracks or vehicles; some `emp_rainbowroad` routes fail.
- On Crossroads, vehicles have timed out both on the way to the southwest repair-pad entrance and after switching to the final approach.
- Tanks defending a friendly asset and grenadiers mortaring an enemy APC now require a driver/grenadier or teammate sightline *(pending match verification)*.
- **Vehicle jam at Crossroads BE gates:** with many vehicles (22-bot match) BE vehicles can still queue at `be_gate1`; lane entry is improved (`scripts/dev_gate.sh`), the match effect is unmeasured. A single BE heavy still enters `be_gate1` 45–55° skewed from the north-west and scrapes the wall; earlier stuck detection raised give-ups and was not kept.
- **Commander vehicle drowning on `emp_palmbay`:** On the drive to a remote factory site the NF commander vehicle can slide down a slope into the sea near `(12000 6800)` and drown (2 of 4 recent 1× matches).
- **Vehicle stuck spots:** Near repair pads, inside the BE base, at a wall near `(3800 5000)`, and in the palm cluster for NF tanks on `emp_palmbay`.
- **`emp_palmbay`:** Bots capture the boat flags by swimming to a boat ladder and climbing it; in a 10-bot 1x match they climbed onto the deck and started a capture, and no ladder blockage lasted over 20 s, with no APCs lost in water. In 6 single-bot runs one bot stayed at the hull beside the ladder. NF finds no factory spot near its CV and builds remotely; NF commander entry can stall at spawn. Vehicles no longer chain reverse turns while still rolling backwards (the NF CV used to back off a cliff that way); the NF CV once wedged its nose under a static structure beside its refinery and could not back out.
- **`emp_coast`:** Engineers have left refineries unfinished at 50/150 HP. The work-spot search now ignores vehicle-only building collision proxies, and engineers now deselect a leftover turret build item before repairing (a rejected turret placement used to block all later construction until death); completion on Coast remains unverified.
- **Engineer build action past its deadline:** the turret placement action can miss its own 20 s deadline; a watchdog now ends it 10 s later (`stall-escalate who=engineer-build`), the underlying cause is still open.
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

Copy `scripts/dev.local.example` to `scripts/dev.local` and point `EMPIRES_TEST_SERVER` at an isolated test server with the addon installed. For contributor and agent workflows, see [AGENTS.md](AGENTS.md), [docs/OPEN_WORK.md](docs/OPEN_WORK.md) and the [wiki](wiki/README.md).

> [!IMPORTANT]
> Tank shells and mortars only work at `DEV_TIMESCALE=1` — with fast-forward enabled they detonate at the muzzle.

<details>
<summary><strong>Test & benchmark scripts</strong> (<code>scripts/</code>)</summary>

<br>

| Script | Purpose |
|---|---|
| `scripts/dev_build.sh` | Incremental cached build |
| `scripts/dev.sh <map> <bots> <minutes>` | Build, deploy, run match, and summarize. A nonempty `DEV_ROSTER` sets the bot count for the run and its health check. Names are numbered per team (`NF_01`, `NF_02`, …). A server exit while the recorder finishes counts as a failed run. Crash checks ignore Linux minidumps from Steam WebHelper or a Source server launched on a different port. Run options (`DEV_UNTIL`, `DEV_EXPECT`, `DEV_FAIL`, `DEV_TIMESCALE`, `DEV_ROSTER`, `DEV_PRE`, `DEV_CMDS`, `DEV_PORT`) |
| `scripts/dev_check.sh` | Parallel regression suite (~1 min): offline unit tests, commander chain, long move, repair pad (repair and refit command sent on the same vehicle), turret, revive (`DEV_SKIP_UNIT_TESTS=1` skips the unit tests). Stops before building or starting servers if its temporary log directory cannot be created. |
| `scripts/dev_quick.sh [map] [min]` | Short targeted run with few bots and instant purchase (`QUICK_*`, `QUICK_FAIR=1`) |
| `scripts/dev_team_match.sh [map] [min]` | 10-bot match at 1× with team-director metrics (`DEV_REAL_ECON=1` forces production economy rules instead of the test server's fast economy). A failed match returns a failure status even when its log is available. Use `scripts/dev_team_match.sh --summarize RUN_DIR` to read the same metrics from a saved run and display its recorder table if available. Both end with the match-flow check against `MATCH_FLOW.md` (milestones and standing rules per team, measurement only). |
| `scripts/dev_econ.sh`, `dev_buy.sh` | Economy to first driven vehicle / purchase-only check |
| `scripts/dev_threat_ab.sh [map] [min] [pairs]` | A/B for `rcbot_empires_threat_map`: 10-bot matches at 1× under real economy rules, map off/on alternating (default emp_crossroads, 18 min, 1 pair), then one table: map cost (µs per update), hurt cover (start, retreat, none, died in cover), engineer stalls at enemy vehicles, kills, STUCK, idle, MATCH_FLOW. `--summarize RUN_DIR…` reads saved runs |
| `scripts/dev_vf_drive.sh`, `dev_turn.sh`, `dev_vehicle_obstacle.sh` | Factory exit & driving, steering/braking, obstacle handling |
| `scripts/dev_orders.sh`, `dev_cmdorders.sh` | UI-style orders / bot commander's own orders |
| `scripts/dev_engibot.sh`, `dev_turret.sh`, `dev_revive.sh`, `dev_heal.sh`, `dev_seismic.sh` | EngiBots, turrets (`DEV_TURRET_UPGRADE=1`), revive, teammate healing, seismic throws at a barracks |
| `scripts/dev_deploy.sh [map] [timescale]` | Three-part refinery wall, then a camera behind it (`DEPLOY_MODE=1` camera/radar only, `2` walls only; `DEPLOY_RANK=1` ranks the engineer up: two turrets, camera and radar) |
| `scripts/dev_gate.sh [east\|west] [runs] ["B cvars"]` | Two BE vehicles drive through a Crossroads BE base exit at once, 4×, ~1.5 min per batch; A/B with cvars for every second run; `GATE_VEHICLES=1` drives a single vehicle (skewed gate entry of the BE heavy) |
| `scripts/dev_lawliet.sh [map] [min]` | Forced NF Lawliet rush on the BE CV at 1× (`LAWLIET_ROSTER`, `LAWLIET_REAL_VF=1`) |
| `scripts/dev_repaircall.sh [map] [timescale]` | Humans first for repair: a bot marked as human holds still in its damaged tank, a bot tank calls for repair; the human tank is repaired first |
| `scripts/dev_navgen.sh <map>` | Generate a map's navmesh on the test server with all available CPUs eligible by default (set `RCBOT_CPUS` to restrict affinity). Each sample shares its vertical clearance trace across four corners. Area merges use indexed incoming ground links and report elapsed time and candidate counts. |
| `scripts/run_unit_tests.sh` | All offline unit tests in parallel (~5 s, needs only a C++17 compiler and python3; also run by CI) |
| `scripts/run_*_tests.sh` | Unit tests for individual headers |

</details>

<details>
<summary><strong>Navmesh tools, diagnostic commands & test cvars</strong></summary>

<br>

**Navmesh tools (`tools/`, Python 3 + Pillow, reads `.nav`/`.bsp` only):**
- `tools/nav_overview.py <map>.nav --bsp <map>.bsp --output out.png` — top-down navmesh render; supports `--crop x0,y0,x1,y1`, `--terrain` (slopes), `--props` (static props), `--vehicle-width 284` (narrow bottlenecks), and `--log server.log [--userid N]` (stuck marks & driven track).
- `tools/check_nav.py` — verifies that spawns, buildings, and resource points sit on the main mesh component.

**Diagnostic console commands (`rcbotd …`):**
- `empires_nav_scan x y z x2 y2 z2 [vehicle-width]` — route and hull checks between two points. Supply a width in map units to check vehicle routing and whether an active vehicle clip blocks the direct segment. Requires at least one bot on the server to load the mesh.
- `empires_test_commander_move bot <userid> x y z` — send a move order to a single bot.
- `empires_test_kill_zone <x> <y> <z> <radius> [team]` — kill every bot (of that team) on foot that walks into the circle, to test hot zones; radius 0 disarms.
- `empires_test_near_death <bot_userid> [deaths=2] [threat_dist=800]` — record teammate deaths beside a bot, killed from ahead of its facing, to test self-preservation.
- `empires_test_exit_block <userid> [passenger]` — box in a vehicle to test blocked exits.
- `empires_test_bring <bot_userid> <target_userid> [dist]` — put a bot beside another player or its vehicle (nearest navmesh point).
- `empires_vehicle_obstacle [ahead] [lateral] [speed] [model]` — place a prop on a driver's route.

**Internals, test-only & experimental cvars:**
- `rcbot_empires_vehicle_debug_team 2|3` (restrict vehicle purchases to one team).
- `rcbot_empires_nav_detour_fast` (default `1`, listed above): building detour planning evaluates grid cells only when the search reaches them, shares them per building for 15 s across all bots, looks for the start cell only near the entry and stops A* at the first clear exit (same exit rule as before). Rainbowroad, 22 bots, 8 minutes: plans over 50 ms 96 → 0, longest 115 → 27 ms, planning time 22.2 → 2.2 s. `0` = old full-grid planner, `2` = plan both and log `nav-detour-compare`.
- `rcbot_empires_vehicle_wall_clearance` / `rcbot_empires_vehicle_narrow_cost` (defaults `1` / `1500`, listed above): route points keep half a width off walls running along the route (angled ahead walls do not count), centre in narrow lanes, and add a straight approach point (within 90 HU, 8 s fallback).
- `rcbot_empires_vehicle_context_steer` / `rcbot_empires_vehicle_curve_speed` (defaults `1`, listed above): arc-length Menger curvature speed regulation for all chassis plus lookahead passage detection (up to 1100 units, including static props and gate pillars) and active approach braking. Crossroads 1× Lawliet APC: 5 → 0 stuck reversals, NF gate 113.0 → 30.3 s, BE base 155.4 → 72.7 s; east gate two-vehicle test: 6/6 passes, 6 reversals, 0 give-ups (down from 51 → 28 → 9/6). Logs `vehicle ctx-steer` and `vehicle tight`.
- `rcbot_empires_vehicle_maneuvers`, `rcbot_empires_vehicle_turn_test` (experimental driving, default `0`; measurements in [docs/OPEN_WORK.md](docs/OPEN_WORK.md)).
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
