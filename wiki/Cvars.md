# Cvars

Sources: `utils/RCBot2_meta/bot_cvars.cpp` (authoritative: definitions, defaults, help text), `utils/RCBot2_meta/bot_empires_vehicle.cpp`
(14 more cvars defined there), `bot_empires.cpp` and `bot_navmesh_api.cpp`, the readers found with `grep`, [README.md](../README.md) (Settings), [AGENTS.md](../AGENTS.md). Refresh: grep `ConVar rcbot_empires_`.

63 `rcbot_empires_*` cvars exist: 46 in `bot_cvars.cpp`, 14 in `bot_empires_vehicle.cpp`, 2 in `bot_navmesh_api.cpp` and 1 in `bot_empires.cpp`. The README documents the player-facing ones;
this page lists all of them, including test and development switches. Set them on the server console, in `server.cfg`, or with `DEV_PRE="cvar value;..."` in the dev harness.

## Commander

| Cvar | Default | Meaning | Read in |
|---|---:|---|---|
| `rcbot_empires_auto_commander` | 1 | One bot per team takes the Commander seat, builds the base and assigns engineers; yields the seat to a human who sits in or uses the CV. | `bot_empires_commander_economy.cpp`, `bot_empires.h` |
| `rcbot_empires_commander_engineer` | 1 | The auto-commander seat goes to the team's first Engineer bot (Repair role); it leaves the CV to finish nearby buildings when no other engineer is close and idle. | `bot_empires_commander_economy.cpp` |
| `rcbot_empires_apc_retreat_hull` | 0.6 | Hull share below which the spawn APC retreats to a repair pad (tanks 0.4); it also leaves a park spot where it lost 15 % of hull and armor within 15 s. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_commander_orders` | 1 | Seated bot commander orders its free attackers (attack location) per team-director role. | `bot_empires_commander_orders.cpp` |
| `rcbot_empires_commander_drive_from` | 1000 | Distance beyond which the commander drives the CV towards a building before leaving it to build. | `bot_empires_commander_economy.cpp` |
| `rcbot_empires_commander_boomtank` | 1 | Commander drives the CV over enemy infantry inside the base zone: 0 off, 1 on except rainbowroad maps, 2 everywhere. | `bot_empires_commander_economy.cpp` |
| `rcbot_empires_commander_first` | -1 | Test: building type built first (0 refinery, 1 barracks, 2 radar, 3 armory, 4 repair, 5 vehicle factory), -1 = normal order. | `bot_empires_commander_economy.cpp/.h` |

## Engineers and EngiBots

| Cvar | Default | Meaning | Read in |
|---|---:|---|---|
| `rcbot_empires_engineer_first_perk_now` | 1 | An engineer's slot-1 perk (repair/revive) is active from its first life, as if picked before spawning. | `bot_empires.cpp` |
| `rcbot_empires_tank_escort` | 1 | One free engineer per team follows an own driven tank within 3000 HU about 350 HU behind it and repairs it when it stands damaged. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_engineer_turrets` | 1 | Repair-role engineers build an MG turret on the front side of own refineries. | `bot_empires_turret_build.cpp`, `bot_empires_commander_economy.cpp` |
| `rcbot_empires_engineer_deployables` | 3 | Bits: 1 camera (plus radar with a second detection slot) behind the refinery wall, 2 three-part wall on the refinery's enemy side. | `bot_empires_deployables.cpp` |
| `rcbot_empires_engineer_help_human` | 2 | With a human commander, up to this many engineer bots per team build/repair unfinished or damaged buildings near the CV on their own (0 = only on the commander's guard order). | `bot_empires_engibot.cpp`, `bot_commands.cpp`, `bot_empires.h` |
| `rcbot_empires_engibot` | 1 | Commanders can buy base engineer bots with `/buildengibot`. | `bot_empires_engibot.cpp` |
| `rcbot_empires_engibot_cost` | 100 | Team resources per EngiBot. | `bot_empires_engibot.cpp` |
| `rcbot_empires_engibot_limit` | 2 | Maximum EngiBots per team (0-8). | `bot_empires_engibot.cpp` |
| `rcbot_empires_engibot_leash` | 2500 | EngiBots work only within this distance of their team CV (else their first spawn). | `bot_empires_engibot.cpp` |
| `rcbot_empires_vacancy_revive` | 1 | Bots may run the death-vacancy Engineer Mass Revive transaction (promote/revive/restore) once per vacancy epoch. | `bot_empires.cpp` |

## Infantry combat

Added 2026-11-23 in `bot_empires_skirmish.cpp` (item 3.25). Measured in two paired 15-minute 10-bot `emp_crossroads` matches
(all on vs. all off): no new stalls, no crashes, kills 28 vs. 24.

| Cvar | Default | Meaning | Read in |
|---|---:|---|---|
| `rcbot_empires_health_poll_hurt` | 1 | Empires sends no `player_hurt`; the bot compares its health each think and reacts to a drop of at least 3 HP (not world/fall damage, not in a vehicle). The attacker is the current enemy if seen in the last 1.5 s. | `bot_empires.cpp` |
| `rcbot_empires_navmesh_hurt_cover` | 1 | With a navmesh, damage outside a skirmish takes a short navmesh cover (at most 6 s) instead of the legacy hide spot, and the bot keeps firing at a visible attacker unless reloading. | `bot_empires.cpp` |
| `rcbot_empires_skirmish` | 1 | Infantry in a fight keeps an effective distance for its weapon, side-steps away from the enemy aim and backs off when too close, reloading or hurt, instead of running straight at the enemy. `2` = NF only, `3` = BE only (for A/B comparisons). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_fight_odds` | 1 | Compares visible enemies with teammates nearby: outnumbered it gives ground, ahead it presses in. | `bot_empires_skirmish.cpp` |
| `rcbot_empires_fight_cover` | 1 | Reloading, badly hurt or outnumbered infantry steps out of sight to a close safe spot and comes back; also suppresses the generic hurt-hide schedule while a skirmish is active. | `bot_empires_skirmish.cpp`, `bot_empires.cpp` |
| `rcbot_empires_join_fight` | 1 | Free infantry walks to a teammate's fight within 2000 units (max 2 helpers per fight). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_callouts` | 1 | Infantry calls out (spots) enemies it starts fighting (only a group of enemies or one within 800 units; once per team per minute, per bot every 3 minutes). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_scout_flank` | 1 | A scout whose teammate fights the same enemy circles to the other side of that enemy (up to 1200 units). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_scout_binoculars` | 1 | A scout that sees an unspotted enemy 1000-4000 units away stops and spots him with the binoculars (per bot every 45 s, per team every 20 s). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_grenadier_rpg` | 1 | A grenadier with rockets keeps 550-1300 units from an enemy vehicle and fires the RPG; keeps at least 400 units from soldiers (splash). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_grenadier_rpg_vehicle_priority` | 1 | A grenadier with rockets breaks off any task (commander orders included) for an RPG shot at an enemy vehicle in sight within 300-2600 units; only an enemy within 500 units interrupts the shot. 0 = orders first, any visible enemy interrupts. | `bot_empires_objective.cpp` |
| `rcbot_empires_engineer_rear` | 1 | An engineer in a fight stays behind the teammate closest to the enemy (up to 1600 units). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_calm_look` | 1 | A bot standing without a route looks toward the last enemy it saw (20 s) or calmly around its facing (+-30 degrees, every 3-5 s) instead of at random points. | `bot_empires_skirmish.cpp` |

## Team behaviour and vehicles

| Cvar | Default | Meaning | Read in |
|---|---:|---|---|
| `rcbot_empires_fair_intel` | 0 | Fair team knowledge (being tested): director, commander, infantry goals, tank/APC goals and the Lawliet rush use only enemies the team could know (spotting, own radar/camera, teammate sight, bot sight reports; memory 10/20/180 s for infantry/vehicles/buildings) plus the enemy start base. `1` both teams, `2` NF, `3` BE. Default 0: in Crossroads A/B runs abandoned own vehicles at the single known front building raised stuck counts. | `bot_empires_team_director.cpp` and consumers |
| `rcbot_empires_apc_cover_park` | 1 | Without a contested flag the spawn APC parks out of sight 1000-1800 HU before the nearest enemy building (else waits 2500 HU before it); park and waiting spots keep 800 HU from own buildings and 400 HU from other own vehicles. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_missiles` | 1 | Tanks buy the Standard Missile Launcher (fire group 1); the driver selects group 1 against vehicles and buildings and group 2 otherwise. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_cannon_eye` | 1 | Turret weapons aim from the cannon pivot; 0 = hull origin + 90. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_turret_align` | 1 | Fire once the turret yaw is within 8° (MG on infantry) or 12° (cannon) and the aim is older than 0.4 s, else after 2.5 s; fixed-turret chassis keep 1.2 s. 0 = always 1.2 s. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_pitch_limit` | 1 | Hold fire while the game clamps the cannon pitch (target below the minimum range) and back off. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_keep_route` | 1 | When replanning finds no path to the same goal, keep driving the last route instead of straight at the goal. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_deep_water` | 1 | Steering edge checks treat water deeper than the vehicle can ford as a drop. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicles` | 1 | Bots buy vehicles at their team's VF (or anywhere with `emp_sv_debug_build_vehicles 1`), drive, fight and run over enemies. | `bot_empires_vehicle.cpp`, `bot_empires_lawliet.cpp` |
| `rcbot_empires_vehicle_drivers` | -1 | Maximum bots per team using a vehicle at once (Bill Nye not counted); -1 = 40 % of the team bots, at least 2. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_wall_clearance` | 1 | Vehicle route points keep half a width off walls running along the route, are centred in lanes barely wider than the vehicle and get a straight approach point (axis over one vehicle width) before such a lane; the driver reaches these within 90 HU (8 s fallback). | `bot_navmesh_api.cpp`, `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_narrow_cost` | 1500 | Fixed route cost per navmesh area too narrow for the vehicle, so short pillar gaps are not cheaper than the wide lane. | `bot_navmesh_api.cpp` |
| `rcbot_empires_billnye` | -1 | Bill Nye mode: one Scout takes a jeep, rams enemy vehicles off the map, stuns them when stuck, runs over infantry. -1 = NF on maps named `*rainbowroad*`, 0 off, 2 NF, 3 BE. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_lawliet` | 1 | Lawliet mode (desperation APC rush on the enemy CV): 0 off, 1 auto when behind in research and map control (or tickets), 2 force NF, 3 force BE. | `bot_empires_lawliet.cpp/.h` |
| `rcbot_empires_lean_defense` | 1 | The team director keeps a standing defender at the base (no alert) only from 5 free infantry bots (floor 20 %; minima from 8/12 team bots stay). 0 = round 20 %, so 3 free bots become push/hold/defense. | `bot_empires_team_director.h` |
| `rcbot_empires_defend_patrol` | 1 | A bot holding a defend or sector spot moves on to another spot around it after 12 s instead of standing 30 s, and must walk to within 100 of the spot (0 = 30 s hold, 250 arrive radius). | `bot_empires_objective.cpp` |
| `rcbot_empires_hot_zones` | 1 | Three deaths of a team on foot within 30 s and 700 HU make the spot hot for 90 s (renewed by each death there); that team's routes pay 1500 to enter and 3x per unit inside. | `bot_empires_team_director.cpp`, `bot_navmesh_api.cpp` |
| `rcbot_empires_self_preservation` | 1 | Two teammates killed by the enemy within 8 s and 600 HU of a bot make it fall back 300-450 HU from the killer (preferably out of sight) or back off out of the fight for 5 s; 20 s cooldown. |
| `rcbot_empires_buddy_repair` | 1 | A free engineer bot repairs a damaged, standing own vehicle within 1200 HU unasked when a human drives it or it is parked empty (hull below 90 % or an armor side below 85 %); 2 also includes bot-driven vehicles (test). |
| `rcbot_empires_humans_first` | 1 | Engineers heal hurt humans before bots (a bot patient is left for a hurt human in reach), serve human ammo calls first; a bot's repair call does not take over the request of a human-driven vehicle, and a damaged human-driven vehicle nearby replaces a bot call. |
| `rcbot_empires_test_human_userid` | 0 | Test only: the bot with this userid counts as a human for `rcbot_empires_humans_first` and `rcbot_empires_buddy_repair`. |
| `rcbot_empires_kill_spawn` | 1 | A defender on a base alert or a far vehicle buyer may respawn (via `kill`) at a spawn at least 3800 HU closer to its goal (no combat for 6 s, tickets > 20 and > 25 %, 90 s per bot). | `bot_empires_team_director.cpp` |

## Diagnostics and platform

| Cvar | Default | Meaning | Read in |
|---|---:|---|---|
| `rcbot_empires_debug_log` | 0 | Print the bots' diagnostic `[RCBOT2]` lines to the server console. Off by default for readable logs; the dev harness turns it on. | `bot_empires_vehicle.cpp` (log helper) |
| `rcbot_empires_force_offsets` | 0 | Use the fixed game-object offsets even when `server_srv.so` is not a known build. Never set on Windows. | `bot_empires_binary.cpp/.h` |

## Test-only and experimental (vehicles)

| Cvar | Default | Meaning | Read in |
|---|---:|---|---|
| `rcbot_empires_vehicle_debug_team` | 0 | Tests only: 2 or 3 = only that team's bots buy or adopt vehicles. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_debug_buy` | 0 | Tests only: with `emp_sv_debug_build_vehicles 1` buy in place even when a VF exists. | `bot_empires_vehicle.cpp`, `bot_empires_lawliet.cpp` |
| `rcbot_empires_vehicle_debug_mixed` | 0 | Tests only: bought tanks get the MG on the cannon's fire button (mixed loadout, item 3.10d). | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_hold` | 0 | Test only: bought vehicles wait in place (firing at targets) until set to 0, for a fair start. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_turn_test` | 0 | Debug purchases only: 1 normal turn, 2 handbrake pulse, 3 coast straight, 4 brake straight (`scripts/dev_turn.sh`). | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_shell_trace` | 0 | Diagnosis: log where cannon shells/missiles end while a driver has a target (`vehicle shell-end`). | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_curve_speed` | 0 | Experimental anticipatory curve speed for NF Light and BE AFV. Parked: no measured gain. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_maneuvers` | 0 | Experimental local maneuver selection for NF Light and BE AFV. Parked: obstacle passed in 1.6-1.9 s but BE open-road median 244 to 153 HU/s. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_vehicle_maneuver_budget` | 2 | Local maneuver evaluations per server tick across all vehicles. | `bot_empires_vehicle.cpp` |

## Related cvars (not `rcbot_empires_*`)

| Cvar | Default | Meaning |
|---|---:|---|
| `rcbot_bot_quota_interval` | 10 | Seconds between bot-quota checks; 0 or lower disables the quota (`config/bot_quota.ini` maps human count to bot count). `min_bots` / `max_bots` (config.ini) clamp the target; EngiBots do not count. |
| `rcbot_bot_squads_percent` | 50 | Percentage of time bots form squads with other bots (set in `config.ini`: `rcbot_bots_form_squads 1`). |
| `eta_nav_learning`, `eta_recovery_active`, `eta_local_yielding` | 0 | Experimental kill switches: learned navigation edge costs, bot-local active recovery for captures, deterministic WAIT-only ally yielding. Passive telemetry stays on. |
| `rcbot_analysis_*` | - | Console commands (not cvars): recorder start/stop/mark/status, see [Console commands](Console-Commands.md). |
| `emp_sv_research_complete` | 1 in `dev.sh` | Game cvar. `dev.sh` defaults to 1 (all research done); use `0` for natural research. |
| `emp_sv_resource_multiplier_be/nf`, `emp_sv_refinery_multiplier`, `emp_sv_research_multiplier`, `emp_sv_debug_build_vehicles` | - | Game cvars for economy values; production values: `1`, `1`, `1`, `1`, `0`. The test server defaults to resource x10, research complete, debug purchase. |
| `host_timescale` | 1 | Set by `DEV_TIMESCALE`; mortars and tank shells only work at 1 (see [Known facts](Known-Facts-and-Pitfalls.md)). |

Inherited RCBot2 cvars (many others in `bot_cvars.cpp`, e.g. `rcbot_speed_boost`, `rcbot_ffa`, `rcbot_notarget`) come from the
upstream bot framework and are mostly irrelevant to Empires.
