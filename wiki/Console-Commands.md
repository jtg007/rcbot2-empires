# Console commands

Sources: `utils/RCBot2_meta/bot_commands.cpp` (all `CBotCommandInline` registrations and their `usage` strings, grep
`CBotCommandInline`), `utils/RCBot2_meta/bot_navmesh_poc*.cpp` (`rcbot_navmesh_*`), [AGENTS.md](../AGENTS.md), [README.md](../README.md),
`reverse-engineering/notes/*.md` (game commands). Test/fixture commands change over time; `rcbotd printcommands` lists the live set.

Use the **dedicated server console**. Commands marked "console only" (`CMD_ACCESS_CONSOLE_ONLY`) are server-console-only; add `rcbotd` in front
(for example `rcbotd empires_nav_scan ...`). "Test-only" commands start bounded fixtures and are meant for scenario scripts; never use them on a live server.
User IDs: the first bot normally has userid 2 in a fresh test server.

## Running bots

| Command | Purpose |
|---|---|
| `rcbotd addbot <class> <team> <name>` | Add a bot. Classes: 0 Scout, 1 Rifleman, 2 Grenadier, 3 Engineer. Teams: 2 NF, 3 BE. |
| `rcbotd kickbot [team]` | Kick a random bot or one of that team. |
| `rcbotd control <name> ...` | Turn an existing client into a controlled bot (`CBots::controlBot`; inherited framework command). |
| `rcbotd printcommands` | List all registered bot commands. |
| `rcbotd analysis_start`, `analysis_stop`, `analysis_mark <text>`, `analysis_status` | Recorder (also as `rcbot_analysis_*` console commands). See [Build and test](Build-and-Test.md#recorder-and-run-evaluation). |
| `sv_cheats 1; p_nav_generate` | Generate a navmesh for the loaded map (engine/library command; takes up to a minute; use `scripts/dev_navgen.sh` on big maps). |
| `buildengibot` (client console) / `/buildengibot`, `!buildengibot` (chat) | EngiBot purchase by a commander. |

Bot quota: `rcbot_bot_quota_interval`, `config/bot_quota.ini`, `min_bots`/`max_bots` ([Cvars](Cvars.md#related-cvars-not-rcbot_empires_)).

## Diagnosis (most used)

| Command | Arguments | Purpose |
|---|---|---|
| `empires_nav_scan` | `x y z x2 y2 z2` | Route, areas and a hull grid (`.` free, `#` world, `E` entity) between two points. Uses the **player** hull, not the vehicle hull. **Needs at least one bot** on the server, otherwise `navmesh-not-loaded`. |
| `empires_commander_build` | `<type> [x y z [auto] \| spot] \| list` | Seated commander bot submits `emp_build` (probe). Type 5 + `auto` uses the automatic factory yaw; `spot` = valid spot near the CV without engineer assignment; `list` shows buildings and resource points. `obstruct` diagnoses placement (Linux only). |
| `empires_test_commander_move` | `bot <userid> x y z` or `squad <team> <squad_id> x y z` | Send a MOVE order to one bot (bypasses the game's order path) or one squad parent/child order. |
| `empires_test_unit_order` | `<cmd_userid> <type> <unit_userid> x y z [target_userid\|b\|bd\|r]` | The seated commander bot sends `emp_unit_order_list` + `emp_unit_order` like the game UI. Types: 0 abort, 1 move, 2 attack location, 3 attack target, 4 guard. `b` = nearest own building below full health, `bd` = nearest full building damaged to 100 HP. Type 3 on a real enemy is dropped by the game for a bot commander. |
| `empires_test_guard` | `<bot_userid> <building_entindex\|0>` | Make the bot act as if a commander gave Guard on that building. |
| `empires_test_botcmd` | `<bot_userid> <repeat> <command> [args]` | The bot runs a client command, repeat 1-32 times. |
| `empires_test_commander_yield` | `<team>` | The team's auto commander acts as if a human wanted the seat. |
| `empires_test_help_human` | `<team\|0>` | The seated bot commander counts as a human commander for `rcbot_empires_engineer_help_human`. |
| `empires_test_engibot` | `<speaker_userid> [!]` | The speaker says `/buildengibot` (or `!buildengibot`) in chat. |
| `empires_test_wages` | `<speaker_userid> <wages>` | Set teammates' wages and run the wages chat request. |
| `empires_squad_observation` | `[userid]` | Read-only squad membership/leader/class/points observation. |
| `empires_squad_make_lead` / `_status` | `<issuer_userid> <target_userid>` / - | Native squad leader change via `emp_make_lead`; confirm with the status command. |
| `empires_vehicle_inventory` | - | Read-only inventory of live vehicles, spawners and presets. |

## Vehicle test hooks

| Command | Arguments | Purpose |
|---|---|---|
| `empires_vehicle_obstacle` | `[ahead] [lateral] [entry_speed] [model]` | The next NF Light / BE AFV driver at entry speed gets a solid prop (default `models/props/concretebarrier.mdl`) on its route; logs `vehicle obstacle placed/passed/timeout/abort`. |
| `empires_test_kill_zone` | `<x> <y> <z> <radius> [team]` | Kills every bot (of that team) on foot that enters the circle, once per life, and counts it for hot zones; radius 0 disarms. |
| `empires_test_near_death` | `<bot_userid> [deaths=2] [threat_dist=800]` | Records teammate deaths 150 units beside the bot, killed from `threat_dist` ahead of its facing, to test self-preservation. |
| `empires_test_exit_block` | `<bot_userid\|0> [passenger]` | Boxes a driving vehicle in with barriers to test blocked exits (`stall-escalate who=vehicle-exit`). |
| `empires_vehicle_setammo` | `<magazine> <reserve>` | Set all vehicles' ammo (ammo scenarios). |
| `empires_vehicle_crate` | `<distance>` | Put a team supply crate beside the first vehicle. |
| `empires_vehicle_scalehp` | `[factor] [front rear left right]` | Scale vehicle health/armor (default 0.3). |
| `empires_vehicle_vf_drive_once` | - | Bounded tutorial completion, VF purchase, enter, drive, exit. |
| `empires_commander_entry_once`, `empires_commander_place_barracks` | - | Commander seat entry / one barracks `emp_build`. |

## Fixtures and probes (development only)

Each ends at one observable result and is used by `scripts/dev_*.sh` or historical acceptance work; most reference the old waypoint graph.

| Group | Commands |
|---|---|
| Engineer | `empires_engineer_repair_once [building_index] [full]`, `empires_engineer_repair_fixture_prepare`, `..._fixture_damage [class] [damage]`, `empires_engineer_repair_visibility_scan [building_index]`, `..._scan_all`, `empires_engineer_resupply_once`, `empires_engineer_resupply_goal_once`, `empires_engineer_build_select_once`, `empires_engineer_build_once [target_waypoint] [full]`, `empires_engineer_build_at <x> <y> <z> [full]` |
| Grenadier | `empires_mortar_smoke_once`, `empires_mortar_fire_once`, `empires_mortar_aim_once [target_waypoint]`, `empires_mine_fire_once`, `empires_mine_trigger_once <target_userid> [hold_s]` (mortar tests need timescale 1) |
| Scout | `empires_scout_binocular_once <scout> <target>`, `empires_scout_binocular_building_once <scout> <building>`, `empires_scout_skill_observation <scout>`, `empires_scout_priority_once ...`, `empires_scout_priority_cleanup`, `empires_scout_building_visibility_scan <building_index>` |
| Combat/LOS | `empires_enemy_los_once <observer> <opponent> [expire]`, `empires_enemy_los_cleanup`, `empires_enemy_combat_once <observer> <opponent>`, `empires_enemy_approach_once <observer> <opponent>` |
| Recovery | `empires_recovery_acceptance_capture <prepare\|progress\|select> [userid]`, `empires_recovery_acceptance_squad [userid]` |
| Respawn | `empires_normal_respawn_probe_kill` |
| Bootstrap client (dev bot used to bring up a map) | `empires_bootstrap_create`, `_remove`, `_countcheck`, `_jointeam <nf\|be>`, `_joinclass <class>`, `_spawn`, `_spawnconfirm` |
| Navigation providers (test-only) | `empires_navmesh_provider_once <x> <y> <z> [userid]`, `..._status`, `..._cancel`; `empires_navigation_request <x> <y> <z> [AUTO\|WAYPOINT_ONLY\|NAVMESH_ONLY] [userid]`, `empires_navigation_intent_once/_status/_cancel`, `..._navmesh_fixture_once`, `..._barracks_once`, `..._intent_writer_probe`, `..._stale_writer_probe`; `empires_local_explorer_request/_barracks_once/_budget_once/_unreachable/_status/_cancel`; `empires_normal_path_once [start dest]`, `empires_anchor_resolver_probe ...` |
| Navmesh spike (`bot_navmesh_poc*.cpp`) | `rcbot_navmesh_poc x1 y1 z1 x2 y2 z2`, `rcbot_navmesh_barracks_bot`, `rcbot_navmesh_building_bot`, `rcbot_nav_record_start/_stop`, `rcbot_navmesh_entity_sweep`, `rcbot_navmesh_building_collision_probe/_mapper/_3d_topology/_obstacle_poc/_detour_poc` |

## Game commands the bots use (for orientation)

These are the game's own client commands, sent by bots through the engine helper; details in [Reverse engineering](Reverse-Engineering.md).

| Command | Used for |
|---|---|
| `emp_build <type> x y z [yaw]` | Commander building placement. |
| `emp_comm_research <index>` / `emp_comm_research_resume` | Start / resume research. |
| `emp_unit_order_list`, `emp_unit_order` | Commander orders (types 0-4). |
| `emp_comm_exit`, `emp_change_seat` | Leave the CV / change vehicle seat. |
| `emp_vehicle_chassis`, `emp_vehicle_armor`, `emp_vehicle_weapon`, `emp_vehicle_build` | Choose parts and buy a vehicle at the VF (`reverse-engineering/notes/vehicle-driving.md` section 3). |
| `emp_revive <index> x y z`, `engSelect <n>`, `emp_eng_upgrade` | Engineer revive, build item select (7 = supply crate), turret upgrade. |
| `emp_menu_command 29` | "I need ammo" call. |
| `changeskill <slot> <code>` | Personal skill choice (stored as wanted skill). |
| `emp_make_lead`, `emp_spawn_select`, `jointeam`, `changeclass` | Squad leader change, spawn choice, team/class. |
