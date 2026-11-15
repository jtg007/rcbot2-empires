# Engineer Mechanisms

Static analysis of `server_srv.so`.

## Confirmed Entry Points

- `Command_emp_revive` (`0x0064d670`) is the individual revive command. It requires a dead same-team player, an active NF/IMP Engineer kit with at least 50 clip units, and a requested position within 512 HU. Its success path deducts 50 clip units, calls `CEmpPlayer::Revive`, emits `player_revive`, and the revive function emits `NF_engineerkit.revive`.
- `CWeaponNFEngineerKit::PrimaryAttack` (`0x003cf110`) selects targets through a trace. It supports same-team player healing/biological-agent removal, building repair, vehicle armor-side repair, and the selected Engineer deployable.
- `CWeaponNFEngineerKit::SecondaryAttack` selects a build item. PrimaryAttack dispatches placement. `BuildWalls` collects two endpoints and calls `CEmpGameRules::CompleteWalls`.
- `CEngineerCrate::Use` (`0x0062bc60`) is the teammate resupply interaction; it requires an alive same-team user and gives eligible ammo from the crate.

## Repair and Heal

- Player healing calls the target `CEmpPlayer::SetHealth`, removes biological-agent state, and consumes one kit round on success. The amount uses an unresolved player float at `+0x1e20`; range and cadence remain data-driven.
- Building repair calls `CEmpBuilding::Repair(float)`, which clamps health to the maximum and may reject an opaque state byte at `+0x498`. The kit consumes one round on success.
- Vehicle repair calls `CEmpTank::HealArmorSide` for the side selected from the hit point. The kit path establishes armor-side repair; no chassis-health repair call was found.
- No separate Engineer blueprint-progress action was identified. Building health is advanced through the repair path (measured: 1 clip round = 1 HP, about 4.7 HP/s; without clip 2 HP/s).

## Walls and Deployables

- Observed build-item values: `1` MG turret, `2` ML turret, `4` camera, `5` radar, `6` walls, `7` ammo crate (`BuildAmmoCrate` 0x3ce830, placed at once, cooldown `m_flEngineerNextSupply`; corrected 2026-11-14, earlier note had 0 crate / 7 walls). Other values remain unresolved.
- `GetBuildLocation` performs aim/attachment and geometry traces. Turret and detection allotment checks exist. Numeric range and small-deployable costs remain unresolved.
- Wall completion uses team/building properties, wall limits, `CanBuildAt`, collision/surface traces, and resource checks. `CEngineerWalls::Destroy` updates wall-count accounting.

## Skills and Bot Observations

- `SKILL_REVIVE`, `m_bSkillRevive`, `m_bSkillIncreasedHeal`, and `m_bSkillIncreasedRepair` are present, but no exact Engineer class ID or class-to-skill mapping was established. No direct revive-skill gate was mapped.
- Network-variable names include `m_iEngineerBuildItem`, Engineer wall/turret/detection allotments, and `m_flEngineerNextSupply`.
- Bot decisions need player life/team/position/health, active Engineer kit and clip, target health/team/type, build selection/allotments, and placement visibility. Heal amount, some target-state gates, and placement acceptance remain server-internal or unresolved.

## Normal Invocation Paths

- Individual revive: `COMMAND` (`emp_revive`, after confirming normal dispatch).
- Heal, building repair, vehicle armor repair, and deployable placement: Engineer-kit `WEAPON PRIMARY ATTACK`.
- Build-item selection: Engineer-kit `WEAPON SECONDARY ATTACK`.
- Resupply: crate `ENTITY INTERACTION` (`IN_USE`-style use).
- Do not invoke final-effect helpers directly when a normal command, weapon action, or entity interaction exists.

## Open Questions and Runtime Backlog

Resolve the heal float/skill mapping, numeric PrimaryAttack range and cadence, revive health restoration and command registration, deployable costs/geometry, wall recycle dispatch, and crate timers/refill values.

## Turret upgrade (emp_eng_upgrade)

Source: `server_srv.so`, `Command_emp_eng_upgrade(CCommand const&)` 0x650830 (static disassembly, not yet run-tested).

- Caller: `UTIL_GetCommandClient()` dynamic_cast to `CEmpPlayer`; requires byte `+0x121e` != 0. `+0x121e` = `m_bSkillTurretUpgrade`, `+0x121f` = `m_bSkillRevive` (labels from `Command_emp_debug_skills` 0x64b4c6/0x64b4f3; not a SendProp). No active-weapon check, no class check beyond the skill.
- Trace: start = eye position (vfunc 0x440), dir = AngleVectors(EyeAngles + PunchAngle), length **115.0** (`.rodata` 0x9d5a48), mask 0x4600400b (MASK_SHOT), `CTraceFilterSimple(player, 0)`.
- Hit entity: `GetServerClass()->m_pNetworkName == "CTurret"` (0x650b1c, string 0x9d4639; MG and ML turrets of both teams), then dynamic_cast `CTurret`.
- Checks on the turret (DT_EmpBuilding SendProps, `ServerClassInit<DT_EmpBuilding>` 0x61bf50):
  - `m_iOwner` (+0x494) == player entindex -> only the builder can upgrade (0x650b8f).
  - max level = `3 - player.m_iTurretRestrict` (+0x1cfc; set by brush `emp_eng_restrict`, keyvalue `turretRestrict`). If `m_iUpgradeLevel >= max` -> ClientPrint HUD_PRINTTALK `#emp_turretlevel_max` and abort.
  - `m_iUpgradeLevel` (+0x4ac) > 2 -> abort (hard cap 3).
  - `m_bFullyBuilt` (+0x492) must be true, else silent abort.
  - No resource cost, no clip use, no cooldown, no research check in the command itself (the building script lists `"Research" "Upgraded ... Turrets Lvl 2/3"` for `*_lvl2/3`; where that is enforced is not checked).
- Effect: `m_bFullyBuilt = false`, `CEmpBuilding::SetBuildingLevel(level+1)` (0x61dbd0; no-op while `m_bRecycling` +0x498), `CTurret::UpdateModel()`, `CTurret::SetHealth(GetBuildingData()->starting health)` (data +0x64, `"starting health"`; lvl1 turrets 25/100). So the turret drops to starting health and must be built up again with the Engineer kit.
- Level values: constructor sets `m_iUpgradeLevel = 1`; valid 1..3. Bot reads it as netprop `m_iUpgradeLevel` (DT_EmpBuilding, int 8 bit unsigned, offset 0x4ac) on the turret entity; `m_bFullyBuilt` and `m_iOwner` likewise.

## MG turret placement: silent rejections (expert, 2026-11-14, static only, not yet measured)

- No research check on the MG path: `CWeaponNFEngineerKit::BuildMGTurret` 0x3ce8e0, `GetBuildLocation` 0x3cd800;
  `CEmpBuilding::Spawn` 0x61ca00 uses `GetBuildingLevel` 0x38d0e0 only for the level (>= 1). Turrets were placed
  with `emp_sv_research_complete 0` (run 110533), so research is not the cause of run 121241's 15 failures.
- Allotment: `ReEquip` sets `m_iEngineerTurretsAllotted` = 1 (0x6650a9), 2 above rank 3. Limit reached → TextMsg
  "You've reached your maximum allotted number of turrets" (0x96b08c, call 0x3d020d). Restriction brushes also send
  `#eng_restriction_*` text (`ClientPrint`, 0x3d009b). CORRECTION: the bot's textmsg log never shows game
  `TextMsg` (all dev-runs contain only RCBot's own HintText/SayText), so neither rejection is excluded.
- `PrimaryAttack` returns silently while `IsCommander` (+0x1438) is set (0x3cf152).
- Further silent aborts: kit clip 0 → `ItemPostFrame` 0x323dd7 calls `HandleFireOnEmpty` instead of `PrimaryAttack`;
  player in a vehicle (0x3cf20a); `GetBuildLocation` rejects: vehicle-factory ramp closer than 500 (0x3cda12), swept
  box ±50 hits something (0x3ce21f), ground hit farther than 150 or normal z < 0.7 (0x3cde5d/6f), any entity in the
  turret box, players included (0x3ce1c1).
- `m_iEngineerBuildItem` is reset to 0 only after a successful build (0x3d01f7); still 1 after the attempt = rejected.
- The bot presses attack once for 0.35 s (`bot_empires_engineer.cpp` ~1789) and then waits for the deadline, so one
  short rejection costs the whole attempt.

### GetBuildLocation 0x3cd800 in order (expert, 2026-11-14, static)

1. Vehicle factory (GetBuildingType 5) with attachment "ramp" closer than 500 to the eye → reject.
2. Box ±50 along eye → eye + 150·view, mask 1, `CTraceFilterEngRestrict` (entities only): hits only
   CEmpEngRestrict, CEngineerMapBrush, CEngineerMapObject (none within ±1500 of the crossroads bases).
3. Water trace (mask 0x20).
4. `UTIL_TraceLine` MASK_SHOT, group 0, length 150 (0x3cde46): must hit, normal.z ≥ 0.7, `DidHitWorld`, team-0 BSP model;
   0x3cdf2d also rejects normal.z < 0.9 with normal.x < -0.2.
5. Box ±25 × 0..25 at the spot, `CTraceFilterOnlyPlayersAndVehicles` team 0: any player or vehicle → reject.
- The game filter ignores `emp_resource_point` (ShouldCollide); the bot's `CEmpiresEngineerShotTraceFilter` does not,
  so the bot's `build=trace` line is no evidence (NF turret 541 stood on the floor although the bot trace hit point 135).
- MG branch 0x3d0193 builds only while `m_iEngineerTurretsBuilt` (0x129c) < `m_iEngineerTurretsAllotted` (0x1298).
  0x1298 is written only in the constructor (0) and ReEquip (1); 0x129c in AddTurret/RemoveTurret/InitialSpawn/constructor.
- Same BE stand spots at refinery 136 worked in runs 110533/111419/114814 and failed in 125445: a state, not geometry.
  Most likely (unproven): allotment (built ≥ allotted) after commander seat in/out, or the restriction state 0x1cfc==3.
  Next: print built/allotted/kit_clip/build_item/m_bCommander at `no-new-mg-turret-observed`
  (`bot_empires_engineer.cpp` ~2111) and check why `Hook_UserMessageBegin` (`bot_plugin_meta.cpp` ~395) never sees TextMsg.

### TextMsg hook (expert, 2026-11-14, static)

- The plugin installs its message hooks on `VEngineServer021`; the Empires game binary uses
  `VEngineServer023` for `UTIL_ClientPrintFilter` (`UserMessageBegin` slot 43, `MessageEnd` slot 44).
  The engine factory returns different interface subobjects for 021 and 023, so the existing
  hook does not intercept the game's calls. `AddRecipient` does not exclude fake clients.
- A separate hook on 023 could capture these messages; this has not been implemented or tested.
  Direct failure-state logging is the smaller diagnostic for the turret case.

## Supply crate (item 7): why the bot placed none (expert, 2026-11-15, static)

- `PrimaryAttack` item-7 branch (0x3cfb9b..0x3cfbbb, 0x3d0be0): only `m_flEngineerNextSupply` (+0x12a8) > curtime
  (ClientPrint "You cannot build a supply crate...") and `BuildAmmoCrate` 0x3ce830. No research, allotment, clip cost
  or build state. Common gates before it: not commander (0x3cf152), kit clip > 0 (ItemPostFrame), not in a vehicle
  (0x3cfb35), `+0x1cf4 == 2` (`#eng_restriction_all`, 0x3d033e). On success item -> 0 (0x3d0c0d) and next supply =
  curtime + `+0x12ac`.
- `BuildAmmoCrate` calls ChangeTeam and Spawn first, then `GetBuildLocation` (turrets: the reverse); on failure the
  crate is removed. `GetBuildLocation` rejects for every item except walls when a vehicle factory "ramp" attachment is
  within **625** of the eye (500 for walls; 0x96b814/0x95a89c). Player/vehicle box (±25, 0..25) uses a filter without
  pass entity: an aim spot closer than about 41 to the bot's centre hits the bot itself.
- Cause in run 20261115-002134 (crossroads, `ammo-crate miss item=7`): the attack never reached the game.
  `CBot::think` calls `modThink`/`getTasks` (where `handleEmpiresAmmoRequest` calls `primaryAttack(true, 0.35f)`)
  and afterwards `handleWeapons`; `CBotEmpires::handleWeapons` without a visible enemy calls
  `m_pButtons->letGo(IN_ATTACK)` before `doButtons`. The MG-turret action works because `handleWeapons` returns early
  while `isEmpiresEngineerBuildActionActive()`. Fix: the same early return while the crate action aims or observes.
