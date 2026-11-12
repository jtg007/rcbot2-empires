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

- Observed build-item values: `0` ammo crate, `1` MG turret, `2` ML turret, `4` camera, `5` radar, `7` walls. Other values remain unresolved.
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
