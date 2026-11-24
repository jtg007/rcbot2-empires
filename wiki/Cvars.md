# Cvars

Sources: `utils/RCBot2_meta/bot_cvars.cpp` (authoritative: definitions, defaults, help text), `utils/RCBot2_meta/bot_empires_vehicle.cpp`
(six more cvars defined there), the readers found with `grep`, [README.md](../README.md) (Settings), [AGENTS.md](../AGENTS.md). Refresh: grep `ConVar rcbot_empires_`.

34 `rcbot_empires_*` cvars exist: 28 in `bot_cvars.cpp` and 6 in `bot_empires_vehicle.cpp`. The README documents the player-facing ones;
this page lists all of them, including test and development switches. Set them on the server console, in `server.cfg`, or with `DEV_PRE="cvar value;..."` in the dev harness.

## Commander

| Cvar | Default | Meaning | Read in |
|---|---:|---|---|
| `rcbot_empires_auto_commander` | 1 | One bot per team takes the Commander seat, builds the base and assigns engineers; yields the seat to a human who sits in or uses the CV. | `bot_empires_commander_economy.cpp`, `bot_empires.h` |
| `rcbot_empires_commander_engineer` | 1 | The auto-commander seat goes to the team's first Engineer bot (Repair role); it leaves the CV to finish nearby buildings when no other engineer is close and idle. | `bot_empires_commander_economy.cpp` |
| `rcbot_empires_commander_orders` | 1 | Seated bot commander orders its free attackers (attack location) per team-director role. | `bot_empires_commander_orders.cpp` |
| `rcbot_empires_commander_drive_from` | 1000 | Distance beyond which the commander drives the CV towards a building before leaving it to build. | `bot_empires_commander_economy.cpp` |
| `rcbot_empires_commander_boomtank` | 1 | Commander drives the CV over enemy infantry inside the base zone: 0 off, 1 on except rainbowroad maps, 2 everywhere. | `bot_empires_commander_economy.cpp` |
| `rcbot_empires_commander_first` | -1 | Test: building type built first (0 refinery, 1 barracks, 2 radar, 3 armory, 4 repair, 5 vehicle factory), -1 = normal order. | `bot_empires_commander_economy.cpp/.h` |

## Engineers and EngiBots

| Cvar | Default | Meaning | Read in |
|---|---:|---|---|
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
| `rcbot_empires_skirmish` | 1 | Infantry in a fight keeps an effective distance for its weapon, side-steps away from the enemy aim and backs off when too close, reloading or hurt, instead of running straight at the enemy. `2` = NF only, `3` = BE only (for A/B comparisons). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_fight_odds` | 1 | Compares visible enemies with teammates nearby: outnumbered it gives ground, ahead it presses in. | `bot_empires_skirmish.cpp` |
| `rcbot_empires_fight_cover` | 1 | Reloading, badly hurt or outnumbered infantry steps out of sight to a close safe spot and comes back; also suppresses the generic hurt-hide schedule while a skirmish is active. | `bot_empires_skirmish.cpp`, `bot_empires.cpp` |
| `rcbot_empires_join_fight` | 1 | Free infantry walks to a teammate's fight within 2000 units (max 2 helpers per fight). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_callouts` | 1 | Infantry calls out (spots) enemies it starts fighting (only a group of enemies or one within 800 units; once per team per minute, per bot every 3 minutes). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_scout_flank` | 1 | A scout whose teammate fights the same enemy circles to the other side of that enemy (up to 1200 units). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_scout_binoculars` | 1 | A scout that sees an unspotted enemy 1000-4000 units away stops and spots him with the binoculars (per bot every 45 s, per team every 20 s). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_grenadier_rpg` | 1 | A grenadier with rockets keeps 550-1300 units from an enemy vehicle and fires the RPG; keeps at least 400 units from soldiers (splash). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_engineer_rear` | 1 | An engineer in a fight stays behind the teammate closest to the enemy (up to 1600 units). | `bot_empires_skirmish.cpp` |
| `rcbot_empires_calm_look` | 1 | A bot standing without a route looks toward the last enemy it saw (20 s) or calmly around its facing (+-30 degrees, every 3-5 s) instead of at random points. | `bot_empires_skirmish.cpp` |

## Team behaviour and vehicles

| Cvar | Default | Meaning | Read in |
|---|---:|---|---|
| `rcbot_empires_vehicles` | 1 | Bots buy vehicles at their team's VF (or anywhere with `emp_sv_debug_build_vehicles 1`), drive, fight and run over enemies. | `bot_empires_vehicle.cpp`, `bot_empires_lawliet.cpp` |
| `rcbot_empires_vehicle_drivers` | -1 | Maximum bots per team using a vehicle at once (Bill Nye not counted); -1 = 40 % of the team bots, at least 2. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_billnye` | -1 | Bill Nye mode: one Scout takes a jeep, rams enemy vehicles off the map, stuns them when stuck, runs over infantry. -1 = NF on maps named `*rainbowroad*`, 0 off, 2 NF, 3 BE. | `bot_empires_vehicle.cpp` |
| `rcbot_empires_lawliet` | 1 | Lawliet mode (desperation APC rush on the enemy CV): 0 off, 1 auto when behind in research and map control (or tickets), 2 force NF, 3 force BE. | `bot_empires_lawliet.cpp/.h` |
| `rcbot_empires_lean_defense` | 1 | The team director keeps a standing defender at the base (no alert) only from 5 free infantry bots (floor 20 %; minima from 8/12 team bots stay). 0 = round 20 %, so 3 free bots become push/hold/defense. | `bot_empires_team_director.h` |
| `rcbot_empires_defend_patrol` | 1 | A bot holding a defend or sector spot moves on to another spot around it after 12 s instead of standing 30 s, and must walk to within 100 of the spot (0 = 30 s hold, 250 arrive radius). | `bot_empires_objective.cpp` |
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
