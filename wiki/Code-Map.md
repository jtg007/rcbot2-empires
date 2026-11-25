# Code map

Sources: `AMBuilder` (compiled source list), [AGENTS.md](../AGENTS.md) (code layout), file headers and comments of
`utils/RCBot2_meta/bot_empires*.cpp/.h`, [docs/OPEN_WORK.md](../docs/OPEN_WORK.md) (6.x cleanup items), [docs/WINDOWS.md](../docs/WINDOWS.md). Line numbers are
approximate (2026-11-23) and drift; use `grep`.

All Empires code lives in `utils/RCBot2_meta/` next to the inherited multi-game RCBot2 framework (TF2, DoD:S, CS:S, HL2DM,
Synergy, FF ... files that Empires does not use). `scripts/` and `tools/` are covered on [Build and test](Build-and-Test.md) and
[Maps](Maps.md#offline-navmesh-tools).

## Entry points and call flow

| Where | What |
|---|---|
| `CEmpiresMod` (`bot_empires.cpp`: `initMod`, `mapInit`, `modFrame`, `updateCommanderOrders`, `updateCapturePoints`, `clientCommand`) | Mod hook per map/frame: capture-point table, commander order hand-off, client commands. `initMod` must not enable bootstrap test mode, otherwise `bot_quota`/`min_bots` fail on the first map (fixed 23.11.). |
| `CBotEmpires::setup` | Bot creation: class, loadout, skills. |
| `CBotEmpires::modThink` (~6590) | Per-frame brain tick: calls the module ticks (`tickTeamDirector`, `tickEmpiresVehicle`, `tickEmpiresLawliet`, `tickEmpiresKillSpawn`, `updateEmpiresEngineerSkills`, `handleEmpiresHeal/Revive/RefineryTurret/AmmoRequest`, `tickEmpiresSeismicTrack`, squad/recovery observers; `abortEmpiresBuildingAction("enemy")` on enemy sight). |
| `CBotEmpires::getTasks` (~5360, about 1100 lines) | Task selection (RCBot schedule system): captures, enemy approach (`APPROACH_ENEMY`), guard orders, build assignments, squad goals; modules hook in at fixed points (`handleEmpiresSkirmish()`, `handleEmpiresJoinFight()`, `handleEmpiresLawlietTask()`). |
| `CBotEmpires::handleWeapons` (~1860) | Weapon choice and firing for infantry; skirmish movement leaves firing to this path. Despite the name it also runs `tickEmpiresGuardOrder()`, `tickEmpiresBuildAssignment()` and `tickCommanderEconomy()` first (after an early return while a commander MOVE is active), then the vehicle/commander-entry actions. |
| `CBotEmpires::hurt` (~1760) | Damage reaction (turret threat notes, engineer interruptions, cover). |
| `CBotEmpires::alterOutgoingUserCmd` (~3260) | Last write to the outgoing `CBotCmd` (button overrides, ladder keys must survive this). Lawliet crew overrides are applied via `applyEmpiresLawlietCommand()` at the `RunPlayerMove` boundary. |
| `RCBotNavMesh::BuildPath` / `BuildVehiclePath` | Navmesh route construction (`bot_navmesh_api.cpp`); vehicle variant takes width and the vehicle (props, physboxes, building exits). |
| `RCBotNavMesh::Frame()` (`bot_navmesh_provider.cpp`) | Per-server-frame path follower update. See pitfall below. |

## Empires files by responsibility

| File | Responsibility |
|---|---|
| `bot_empires.cpp` / `.h` | Bot class `CBotEmpires`, mod class `CEmpiresMod`, task selection, weapons, spawn/class selection, bootstrap development client (`empires_bootstrap_*`), squads, fixtures; 16 000+ lines (split only when touching, item 6.3). |
| `bot_empires_internal.h` | Shared constants and stateless helpers (each translation unit gets its own copy). |
| `bot_empires_objective.cpp` | Capture approach and retries, start-anchor recovery, navmesh goals, turret rocket fallback and enemy-turret threat circles. |
| `bot_empires_commander_economy.cpp` / `.h` | Automatic commander: seat entry, `emp_build` placement, build order, research, engineer assignment, CV drive/survival. Policy header is engine-free and unit tested. |
| `bot_empires_commander_orders.cpp`, `bot_empires_commander_order_adapter.*` | Seated commander gives *attack location* orders like a human (`emp_unit_order_list` + `emp_unit_order`); MOVE order adapter and telemetry. |
| `bot_empires_team_director.cpp` / `.h` | Per-team situation board (phases push/alert/stabilize, goals, alerts), role split, kill-spawn. `.h` is SDK-free. |
| `bot_empires_priority.h` | Priority tiers 0–4 for engineer work (core, front, map play, comfort); a higher-tier build order preempts a lower-tier job. SDK-free, unit test `scripts/run_priority_tests.sh`. |
| `bot_empires_match_flow.cpp` | Match-flow telemetry (debug log only): team status every 30 game seconds and building/vehicle events, checked by `scripts/match_flow.py` against `MATCH_FLOW.md`. |
| `bot_empires_team_economy.h` | Shared money picture (reserve, buyer choice). Pure logic. |
| `bot_empires_vehicle.cpp` / `bot_empires_vehicle_loadout.h` | Everything vehicles: buy, enter, drive, fight, retreat/repair, ammo, adopt, Bill Nye, test hooks (6900 lines; `tickEmpiresVehicle`). Loadout choice by research/budget in the header. |
| `bot_empires_lawliet.cpp` / `.h` | Lawliet APC rush: team coordinator (about 0.5 s), crew tasks, UserCmd overrides. Policy in header (unit tested). |
| `bot_empires_engineer.cpp` | Engineer support targets, repair/build/resupply actions, interruption for combat. |
| `bot_empires_engibot.cpp` | EngiBot purchase via chat, limit/leash, cleanup; team resource ledger read/write. |
| `bot_empires_turret_build.cpp` | Refinery MG turret placement (front side heading). |
| `bot_empires_deployables.cpp` | Refinery three-part wall and radar/camera behind it (engineer kit items 4/5/6). |
| `bot_empires_ammo_crate.cpp` | Answers vehicle ammo calls with a supply crate (`engSelect 7`). |
| `bot_empires_heal.cpp` | Engineer heals hurt teammates with the kit (5.31.1). |
| `bot_empires_revive.cpp`, `bot_empires_vacancy_revive.h` | Individual revive (`emp_revive`) and vacancy Mass Revive policy. |
| `bot_empires_seismic.cpp` | Seismic grenade target choice, throw pitch solver (g about 320), flight/damage tracing (5.31.2). |
| `bot_empires_grenadier.cpp` | Mortar and mine actions (`handleEmpiresMortarAction`, `handleEmpiresMineAction`). |
| `bot_empires_skirmish.cpp` | Infantry fight movement: side steps, fight odds, cover, join fight, callouts (`handleEmpiresSkirmish`, `handleEmpiresJoinFight`, `calloutEmpiresEnemy`). Reads the five infantry cvars. |
| `bot_empires_squad_runtime.*`, `bot_empires_squad_join.h`, `bot_empires_squad_observation.h`, `bot_empires_leader_actuation.h` | Squad observation/shadow runtime, join policy, native leader change (`emp_make_lead`). |
| `bot_empires_spatial.*`, `bot_empires_native_placement.*`, `bot_empires_barracks_execution.*` | Building placement: spatial checks, calls of the game's own placement functions by address (Linux only), dispatcher for `emp_build`. |
| `bot_empires_binary.*` | Game-binary size/CRC check (`kKnownBinaries`); gates all fixed-offset reads (`rcbot_empires_force_offsets`). |
| `bot_empires_telemetry.*`, `bot_empires_combat_telemetry.h` | Analysis recorder (events, snapshots, summary). Passive. |
| `bot_empires_capture_retry.h`, `_capture_route_terminal.h`, `_start_anchor_recovery.h`, `_combat_relevance.h` | Engine-free policies for capture retries, route terminal causes, start-anchor recovery and combat-vs-objective relevance (unit tested). |
| `bot_empires_*_test.cpp` | Offline unit tests, run by `scripts/run_*_tests.sh` and `scripts/run_unit_tests.sh`. |

## Navigation stack

| File | Role |
|---|---|
| `bot_navmesh_api.*` | Production API: path construction on the Source nav mesh, climb/drop limits (40/200), building detours (`ApplyBuildingDetours`, `PlanDetour`), static prop detours, `DebugNavScan`. |
| `bot_navmesh_provider.*` | Provider that writes through the shared navigation ownership seam; `Frame()` follows routes each server frame; infantry-only blocked edges, door blocks. |
| `bot_navmesh_follower.*` | Valve-style path follower; local avoidance (`Avoid`) exists but is disabled at the call site (item 6.10). |
| `bot_navmesh_poc.cpp` (5900+ lines), `bot_navmesh_poc_bot.cpp` | Old spike and test-only probes (`rcbot_navmesh_*` commands); candidates for cleanup (item 6.8). |
| `navmesh/nav_mesh/*`, `navmesh/util/*` | Imported nav mesh library; `nav_generate.cpp` is the generator with Empires changes (seeds above cap points, water ladders, gates). |
| `bot_navigation_attempt.*`, `bot_navigation_arbitration.*`, `bot_local_explorer.*`, `bot_local_yielding.*`, `bot_local_conflict_observation.*`, `bot_recovery_episode.h`, `bot_navigation_experience.h` | Provider-neutral navigation intent/attempt model, provider arbitration, bounded local explorer, ally yielding, recovery telemetry. Experimental ones behind `eta_*` cvars. |
| `bot_order_protocol.*`, `bot_squad_*`, `bot_decision_core.*` | Order protocol, squad decision/claims/spatial movement, neutral decision value objects. |

Other relevant framework files: `bot_commands.cpp` (all `rcbotd` commands), `bot_cvars.cpp`, `bot_plugin_meta.cpp` (plugin hooks, chat
interception, config queue), `bot_mods.cpp` (mod registration, quota), `bot_weapons.cpp` (weapon ranges), `bot_configfile.*`,
`bot_events.cpp` (game events such as `player_say`), `bot_getprop.*` (SendProp reads by name).

## Documented pitfalls in code and notes

- **Navmesh follower overwrites the move target every frame** (`RCBotNavMesh::Frame()` -> `setMoveTo` each server frame). `stopMoving()` alone
  does not hold a bot that still has a route, not even with override priority. To hold or redirect a bot: release the route
  (`cancelNavigationAttempt`) and keep `getTasks` from re-planning (heal and seismic modules do this).
- **`alterOutgoingUserCmd` can strip buttons**: during a commander MOVE it stripped `IN_FORWARD/...` after `runPlayerMove` added them for
  `MOVETYPE_LADDER`, so bots hung on boat ladders and then `checkStuck` pressed `IN_JUMP` and threw them off. Keep movement buttons while on a ladder.
- **Shared library links with missing symbols**: a build can succeed and the plugin loads as "RCBot2 not loaded" (`undefined symbol` in
  `server.log`). Seen causes: a function in an anonymous namespace declared globally elsewhere; `edict->StateChanged()` (use `m_fStateFlags`).
  Check with `nm -uC <so> | grep Empires`.
- **Fixed offsets in game objects** are only trusted for known binaries (`bot_empires_binary.cpp`; after a game update add size/CRC to
  `kKnownBinaries`). Each offset has a comment with the Linux function address it was read from.
- **`CBotEmpires` is huge**: `getTasks`, `tickEmpiresVehicle`, `tickCommanderEconomy` should only be split when you are in there anyway (item 6.3).
- **MSVC**: write `(std::min)(...)`, `(std::numeric_limits<T>::max)()`, plain `snprintf(`; Linux-only APIs under `#ifdef __linux__`; no duplicate class
  names across files (MSVC LNK2005); `static constexpr` inside lambdas. See [Windows](Windows.md).
- **Do not guess**: a missing SendProp is a result, not a reason to substitute an offset (comment in `bot_empires_squad_observation.h`).
- **Submission is not success**: build/command submissions (`emp_build`, `emp_make_lead`) are confirmed only by later observed state.
- **Waypoint remnants**: some squad/resupply/cover/approach paths still use RCBot waypoints; item 6.8 plans to move them to the navmesh.
  `bot_navigation_attempt.h` still describes an AUTO order of "Waypoint first, then NavMesh, then Local Explorer".
- **New work always gets its own cvar** and a `stall-escalate who=... stage=... reason=...` log line on every escalation.
