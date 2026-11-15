# Driving, buying and fighting with vehicles (RE note)

Source: `empires/bin/server_srv.so` (32 bit, with symbols), scripts from `scripts_dir.vpk`,
models from `models_dir.vpk`, `resource/modevents.res`. Addresses = ELF link addresses.
Read statically only, no run. **Assumption** = not confirmed in the binary.

CUserCmd offsets (SDK 2013): viewangles +0xc, forwardmove +0x18, sidemove +0x1c, buttons +0x24.

## 1. Driver input

Flow: `CEmpTank::SetupMove` 0x6a3fb0 → `CPropVehicleDriveable::SetupMove` 0x8b9a80 →
`CEmpTank::DriveVehicle` 0x6a43d0 (turret, weapons, neutral turn) →
`CFourWheelVehiclePhysics::UpdateDriverControls` 0x6fb130.

| Function | Input | Evidence |
|---|---|---|
| Digital steering | `IN_MOVELEFT` 0x200 / `IN_MOVERIGHT` 0x400; takes precedence over sidemove | `test bh,0x6` 0x6fb240 → `SteeringTurn` 0x6fac20 (left=(b>>9)&1, brake=IN_BACK, throttle=IN_FORWARD) |
| Analog steering | `sidemove` (only without MOVELEFT/RIGHT keys): steer = sidemove/368, clamped to [-1,1] → **intermediate values possible** | 0x6fb500 → `SteeringTurnAnalog` 0x6faf50, factor 0x9f1b2c = 1/368 |
| No steering | neither keys nor sidemove → wheels return to centre | `SteeringRest` 0x6fab60 |
| Analog throttle | `forwardmove` > 0: throttle = clamp(fwd/368, 0.25, 1) | 0x6fb2e3–0x6fb328 (0x9f1b40 = 0.25) |
| Analog reverse | `forwardmove` < 0: |fwd|/368, [0.25,1] × maxReverseThrottle; brakes first while still moving forward | 0x6fba50, state `+0xae8` |
| Digital throttle/reverse | `IN_FORWARD` 0x8 / `IN_BACK` 0x10 (only if forwardmove == 0) | `test bl,0x8` 0x6fb666, `test bl,0x10` 0x6fb900 |
| Boost | `IN_SPEED` 0x20000 (script `boost`, e.g. jeep 1 s, 12 s cooldown) | `test ebx,0x20000` 0x6fb395 |
| Handbrake | **bit 0x08000000** (not IN_JUMP!); one-sided at |sidemove|>100 | `and ebx,0x8000000` 0x6fb3a1 → 0x6fb790 |
| Reverse blocked | `IN_BACK` is cleared while `+0x1054` > curtime (set by `OverrideVelocity` 0x694790, e.g. after a spawner) | 0x6a4445 |
| Overturned | `IsOverturned` → IN_FORWARD/IN_BACK/IN_SPEED are removed | 0x6a40f5, mask 0xfffdffe7 |

**Neutral turn** (tank turns on the spot), `HandleNeutralTurn` 0x694700 / `HandleNeutralTurnPhysics` 0x6943c0:
- Condition: chassis `"Neutral Turn Factor"` ≠ 0, `sidemove` ≠ 0, `forwardmove` == 0 (exactly), `m_nSpeed` (+0x4a4, mph) ≤ 20 (0x94f6d8).
- Afterwards `sidemove` is set to 0 (no wheel steering). MOVELEFT/RIGHT keys are NOT evaluated → for a neutral turn set only sidemove, no keys.
- Direction: target = 1 − 2·clamp((sidemove+368)/736) → sidemove −368 = +1 (left/counter-clockwise), +368 = −1 (right). Analog. Angular impulse around the vehicle's up axis, only below 90°/s (|ω|² < 8100).
- Factor per script: NF Light 0 (cannot!), NF Med 1.25, NF Heavy 2.5, NF Arty 0.85, BE Med 1.15, BE Heavy 1, BE Arty 0.65. Jeep/APC/AFV: no entry.

## 2. Aiming and firing

- `DriveVehicle` calls `GetControllable()` 0x6a48a0 and then `CEmpTankTurret::SetTargetRotation(&ucmd->viewangles)` 0x3e6940 (vtable +0x37c).
- `SetTargetRotation`: target turret angle (local) = AngleNormalize(clamp(Normalize(viewYaw − 90), min +0x540, max +0x544) − turretAbsYaw (+0x32c)).
  **viewangles are world angles.** Evidence: `SetupMove` 0x6a436e sets `SnapEyeAngles(pitch=turret.absPitch, yaw=turret.absYaw+90)` on entry → turret target 0 (straight ahead).
  Vehicle models face local +Y: driving direction (world yaw) = entity yaw + 90.
- Bot: viewangles = normal world aim angles to the target (as on foot). The turret follows at `"Turret Turn Rate"`; shots leave from the weapon attachment (`GetWeaponVectors` 0x69b380), so only fire once the turret is aligned. Pitch handling not read (**assumption**: viewangles.x directly).
- `HandleWeaponButtons` 0x6942d0: `IN_ATTACK` → `FireWeapons(0, true, group, -1)`, `IN_ATTACK2` → `FireWeapons(0, false, group, -1)`, `IN_RELOAD` 0x2000 → `ReloadWeapons(true)`. `group` = player +0x534 (selected fire group).
- **Observation in game (2026-11-12, rainbowroad):** a bot pressing only `IN_ATTACK` visibly fired the MG (bought as MG mode 0, cannon mode 1). In practice `IN_ATTACK` → mode 0, `IN_ATTACK2` → mode 1; the mode direction in the next line was probably misread. The bot now uses `IN_ATTACK2` for the cannon.
- `FireWeapons` 0x69bc60 fires weapon i if: group flag[i][group] is set, **mode byte (+0xbb8+i) == bool** (static reading: IN_ATTACK fires weapons with mode=1, IN_ATTACK2 mode=0; `AddWeapon` 0x69a118 stores the mode unchanged), reload time elapsed, not `IsWeaponOverheated`, `+0x1050` (under water) == 0.
- Heat: `m_flVehicleHeat` (+0xbc0), capacity = chassis `"Heat Capacity"` (all 100). Each shot adds the weapon's `"Heat"` (MG 0.8). `IsInHeatPenalty` 0x6a4880: `+0x954` > 0. `IsWeaponOverheated` 0x3e6550: in penalty → blocked, otherwise heat + shot heat against capacity. Heat above the 3rd zone (chassis +0x8a0) lowers the maximum engine power (`CalculateEngineStall` 0x693298).

## 3. Buying

Player fields (CEmpPlayer): chassis +0x1670, weapons +0x1674[32][3] (int), groups +0x17f4[32][3][6], mode +0x1a34[32][3], armor +0x1a94[3], armor levels +0x1aa0[4], engine +0x1ab0[3], factory handle +0x1454.

| Command | argc | Meaning | Address |
|---|---|---|---|
| `emp_vehicle_factory <entindex>` | 2 | select factory (must be `IsBuilding`, same team) | 0x64d090 |
| `emp_vehicle_chassis <idx>` | 2 | set chassis and **reset everything** (weapons/armor/engine = −1, levels 0) | 0x64cbf0 |
| `emp_vehicle_weapon <slot> <sub> <weapon> <g0> <g1> <g2> <g3> <g4> <g5> <mode>` | **11** (10 arguments) | slot < 32, sub < 3; g0..g5 fire groups (bool); mode bool | 0x64a230 |
| `emp_vehicle_armor <slot> <armor>` | 3 | | 0x64a520 |
| `emp_vehicle_armor_levels <front> <rear> <left> <right>` | 5 | inches | 0x64cd40 |
| `emp_vehicle_engine <slot> <engine>` | 3 | | 0x64a5d0 |
| `emp_vehicle_build` | 1 | build | 0x652d70 |

Wrong argument count → the command is silently ignored (e.g. `emp_vehicle_weapon` with only 9 arguments has no effect).

Chassis index = order in `scripts/vehicle_chassis.txt` (`ParseChassis` 0x3b3970 counts up, stores +0xf68). `GetChassis` 0x3b0710 checks team and research.

| Idx | Chassis | Team | Cost | Seats | Max weight | HP | Research | Without research |
|---:|---|---|---:|---:|---:|---:|---|---|
| 0 | NF_Jeep | NF | 50 | 4 | 28 | 150 | Jeep_Chassis | yes |
| 1 | NF_APC | NF | 200 | 8 | 265 | 150 | APC_Chassis | yes |
| 2 | NF_Light_Tank | NF | 110 | 2 | 260 | 150 | Light_Tank_Chassis | yes |
| 3 | NF_Med_Tank | NF | 170 | 2 | 435 | 200 | Medium Tank Chassis | no |
| 4 | NF_Arty_Tank | NF | 650 | 1 | 270 | 150 | Artillery Tank Chassis | no |
| 5 | NF_Heavy_Tank | NF | 820 | 2 | 580 | 300 | Heavy Tank Chassis | no |
| 6/7 | NF_Fighter / NF_Falcon | NF | – | – | – | – | – | unused |
| 8 | Imperial_Jeep | BE | 40 | 2 | 28 | 150 | Jeep_Chassis | yes |
| 9 | Imperial_APC | BE | 240 | 8 | 265 | 150 | APC_Chassis | yes |
| 10 | Imperial_AFV (light tank) | BE | 60 | 4 | 320 | 150 | Light_Tank_Chassis | yes |
| 11 | Imperial_Med_Tank | BE | 220 | 2 | 380 | 200 | Medium Tank Chassis | no |
| 12 | Imperial_Arty_Tank | BE | 650 | 1 | 270 | 150 | Artillery Tank Chassis | no |
| 13 | Imperial_Heavy_Tank | BE | 850 | 2 | 550 | 250 | Heavy Tank Chassis | no |
| 14–16 | Imp_Fighter, Imp/NF_Commander | | | | | | | do not buy |

("Without research" = `Start_Researched 1` in `research_items.txt`.) Jeeps have no weapon slots.

Weapon index = order in `vehicle_weapons.txt`; without research: 0 Standard Machine Gun (type 0, 30), 6 Chain Gun (45), 7 Medium Chain Gun (60), 12 Standard Cannon (type 1, 50), 23/24 artillery, 27 Standard Missile Launcher (type 3, 40), 40 Grenade Launcher (type 4). The slot type must match (chassis script `weapon_slot` `"Type"`).
Engine index = order in `vehicle_engines.txt`, must match chassis type and team (`GetEngine` 0x3b0630: a non-zero team field must be equal): **0 = IMP Jeep Engine, 1 = NF Jeep Engine**, 7 APC Standard, 13 Light/AFV Standard, 19 Medium Standard, 25 Heavy Standard, 31 Arty Standard. Armor 0 = Plain Armor.

Minimal NF jeep: `emp_vehicle_chassis 0; emp_vehicle_armor 0 0; emp_vehicle_armor_levels 0 0 0 0; emp_vehicle_engine 0 1; emp_vehicle_build` (engine 0 = BE engine → for NF **assumption** `#emp_engine_required`).
Minimal NF light tank with cannon (slot 0 = cannon, 1 = MG, 2 = missiles per script/preset):
`emp_vehicle_chassis 2; emp_vehicle_weapon 0 0 12 1 0 0 0 0 0 1; emp_vehicle_armor 0 0; emp_vehicle_armor_levels 0 0 0 0; emp_vehicle_engine 0 13; emp_vehicle_build` (cost 110+50+20).

Rejections in `emp_vehicle_build` (all via `ClientPrint`, invisible to bots):

| Reason | Text |
|---|---|
| wrong chassis/team/research | server console `Chassis not found` / `Chassis unavailable` |
| vehicle limit (`emp_sv_max_vehicles`) | `#emp_vehicle_limit_reached` |
| weapon without group, wrong slot type, too many, too large | `Warning: Weapon ...` |
| armor/engine missing or mismatched | `#emp_armor_required`, `#emp_engine_required`, `Warning: Engine/Armor Type ...` |
| too heavy | `Total vehicle weight ... exceeds` |
| too expensive | `... exceeds available personal resources` / `... available resources` |
| no factory selected (and `emp_sv_debug_build_vehicles 0`) or player dead | `#emp_vehicle_not_in_vf` |

### Factory path without debug purchase (2026-11-14)

`emp_sv_debug_build_vehicles 1` skips the whole factory path (0x652e5d → 0x652fe0). Without it,
`emp_vehicle_build` aborts **silently** (no `ClientPrint`) when:
- `IsInFactory` 0x6540ee → 0x652fc0 fails: needs the player's factory handle `player+0x1454` valid
  (0x65fe95). `emp_vehicle_factory <index>` sets it; `CVehicleFactory::EndTouch` 0x6a9ec2 sets it to -1.
- the factory is not alive (0x65308b) or not built (`factory+0x492`, 0x6530b3).
- `#emp_vehicle_blocked` (0x6543e6): hull ±150 around the factory attachment `build`, mask 0x4600400b.
  The hull trace in `IsInFactory` uses `CTraceFilterNoNPCsOrPlayer` (vtable 0x983c18); the vehicle
  physbox is ignored (`CGameRules::ShouldCollide(0,12)` = 0, 0x3fd829).
- `CanBuild` 0x6aa2f4 writes the current time to `factory+0x60c` only when all checks up to the
  vehicle creation (`CreateEntityByName` 0x654556) passed.

Research, money (~4900 vs ~274), weight (230 of 260) and the limit (32) were ruled out for run
20261114-103144 (20 purchases, no vehicle). **Assumption:** the factory handle is cleared between
`emp_vehicle_factory` and `emp_vehicle_build` (sent 0.15 s apart, bots pushing each other at the
factory). The bot now sends both in the same tick and logs `vehicle factory-probe`
(handle, built, `+0x60c`).
**Measured, run 20261114-110533** (production cvars, both commands in one tick): handle valid and
pointing at the factory (`0x0397f232` = index 562), `built=1`, `+0x60c` stays 0 → the handle
assumption is **refuted**; the build is rejected after the built check (0x6530b3) and before
`CanBuild` writes `+0x60c`. No TextMsg reached the bot's message hook.

Checks that run only with a factory (0x654250–0x654ac7): player alive (0x654263, else
`not_in_vf`), factory attachment `build` (0x6542aa), then a hull trace ±150 at that point
(`CTraceFilterSimple(passEnt=factory, COLLISION_GROUP_NONE)`, mask 0x4600400b, 0x6543c6–0x6543fb).
A non-world hit passes only if it `IsPlayer` (vfunc 0x144, 0x654aac) or has collision group 14
(0x654ac0); otherwise `ClientPrint #emp_vehicle_blocked` (0x654aed) before `CanBuild` (0x654443).
Players (the buyer too), the factory and its physbox (`ShouldCollide(0,12)` = 0) do not block.
Part/weight/cost checks (0x6531e0–0x6544b0) are the same with and without the debug cvar; parts
without a `"Research"` key skip the research check; cost = team resources + salary (0x65448d).
**Assumption:** another solid object (turret, wall, crate, building, vehicle) stands in the build
spot. The bot logs `vehicle factory-near` (solid entities within the factory bounds + 150).

`IsInFactory` 0x65fe80 (called at 0x6540ee, failure ends silently 0x6540f5 → 0x652fc0): handle
`player+0x1454` valid, player alive (0x65fed4), not in a vehicle (0x65fee3), factory `IsBuilding` and
alive; then two player hull traces (`GetPlayerMins/Maxs`, mask 0x200400b, `CTraceFilterNoNPCsOrPlayer`,
physbox group 12 ignored): first `TraceRay` **up** from the origin to origin + (0,0,1024) (0x6601d2, end
built with `addss` 1024 at 0x660055), then `UTIL_TraceHull` **down** to origin − (0,0,1024) (0x6602de).
Both must hit the factory first (0x660298, 0x6602e3): the buyer must stand on the factory floor
**under its roof**; ramp or open apron fails silently. **Verified with gdb** (2026-11-14, crossroads,
production cvars): handle/alive/IsBuilding passed, up-trace frac=1 hit=null, IsInFactory=0, no vehicle.

Attachments from the models (v48, one bone `static_prop`, identity): BE `imp_vehiclefactory.mdl`
`build` (0, 0, 200), `panel` (0, 270, 150), `ramp` (750, 0, 20); NF `nf_vehiclefactory.mdl` `build`
(172.5, 0, 172.5), `ramp` (517.5, 0, 23). The surrounding bounds `obb` in the bot log equal the model hull.
Run 20261114-111731: removing the factory panel did not help; CV and props are outside the build box.
| spawn box blocked | `#emp_vehicle_blocked` (hull ±150 at attachment `build`, hits a non-world entity that is not a player) |
| factory cooldown 1 s (`CVehicleFactory::CanBuild` 0x6aa2c0) | `#emp_vehicle_recharging` |
| anti-griefing | `#emp_vehicle_antigrief_triggered` |

Factory: no distance test in the command; `CVehicleFactory::Touch` 0x6a9fe0 sets +0x1454 on entering, `EndTouch` 0x6a9e70 clears it on leaving → send `emp_vehicle_factory` right before `emp_vehicle_build`.
Without the debug cvar, `emp_vehicle_build` calls `CEmpPlayer::IsInFactory` 0x65fe80; if it fails, the command aborts **silently** (return 0x652fc0, no message). IsInFactory: handle +0x1454 valid, player alive and not in a vehicle, then a hull trace 1024 units down from the player, `MASK_SOLID` (0x200400b); the first hit must be the factory. The filter is a `CTraceFilterSimple` with `COLLISION_GROUP_NONE` (0x65ff5e); `CGameRules::ShouldCollide` (0x3fd760) lets group 0 pass through 10/12/15. The `emp_building_vehicle_physbox` has group 12 (`SetCollisionGroup(0xc)` 0x61971b) → the game trace passes through it, **standing on the factory floor is enough to buy**. The bot mirrors this with `CTraceFilterGameNone`. The hit is compared with handle +0x1454 (set by `Touch` and by `emp_vehicle_factory <idx>`, which checks vfunc 0x150 and `InSameTeam`). `ClientPrint` messages do not reach bots (recipient filter), not even through the UserMessage hook.
Payment comes from wages if they suffice (0x6545bd), otherwise from team resources.
**Detecting success:** game event `vehicle_purchase` (userid, recustomized, cost, chassis, builtfromwages; logged by the plugin in `bot_plugin_meta.cpp`). No event ≈ rejected.

## 4. Test shortcuts

- **Simplest:** `emp_sv_debug_build_vehicles 1` (cvar 0xc35de0). Then `emp_vehicle_build` works without a factory (and without the limit test). Spawn at player position + (0,0,200), angles (pitch, yaw−90, roll) from the player's angles (0x654a09) → the vehicle faces the player's view direction. Costs still apply.
- Spawner: `emp_vehicle_spawner` (keyvalues `enabled`, `autorespawn`, `autorespawndelay`, `CostsResources`; inputs `Enable`, `Disable`, `SpawnVehicle <preset-targetname>`, `SpawnVehicleByPtr`) and `emp_vehicle_preset` (keyvalues `teamNumber`, `spawnPoint`, `cost`, `chassisType`, `engineType`, `armorType`, `armorFront/Rear/Left/Right`, `weaponType1..8`, `weaponMode1..8`, `weaponGroup1..8` = bitmask of groups 0..5, `CEmpVehiclePreset::Spawn` 0x6aadc0). The spawner builds at its own origin/angles (`BuildVehicle` 0x6ac050), checks the limit and costs if `CostsResources`, and also fires `vehicle_purchase`. Errors go to the server console (`Spawner failed: ...`).
  From the plugin: `CreateEntityByName` + `DispatchKeyValue` + `DispatchSpawn` + `AcceptInput("SpawnVehicle", "preset1")`. `ent_create` from the server console needs a client (**assumption**, SDK default); `ent_fire sp1 SpawnVehicle preset1` works from the console.
  Example (keyvalues): preset `targetname preset1 teamNumber 2 chassisType 2 engineType 13 armorType 0 weaponType1 12 weaponMode1 1 weaponGroup1 1`; spawner `targetname sp1 enabled 1 CostsResources 0` at the desired position.

## 5. Entering and exiting

- `CEmpTank::Use` 0x68b9d0: player not in the vehicle → `HandlePassengerEntry`; in the vehicle and `CanExitVehicle` → `HandlePassengerExit`. IN_USE inside the vehicle = exit.
- `CanEnterVehicle` 0x68b1a0: not overturned, same team (`InSameTeam`), commander vehicle with lock time.
- Seat: `GetAvailableSeat` 0x695bd0 = first free seat 0..n−1 (+0x958 seat count), 0 = driver.
- `CanExitVehicle` 0x6a4900: curtime ≥ `+0x8fc` (entry time + 1 s, set at 0x6951a8) **and** speed `+0x810` ≤ cvar `g_vehicleexitspeed`.
- Use range: standard `CBasePlayer::FindUseEntity`, not Empires-specific (**assumption** about 80–100 units).
- Events: `vehicle_enter` (userid, vehicleid, seat), `vehicle_exit`, `vehicle_death`.

## 6. Running over players

`CheckWheelCollisions` 0x6969b0 (wheel traces) → `HandlePlayerInWheel` 0x6968d0:
- Victim alive, **not on the same team** and the vehicle moves horizontally **> 5 mph** (|v_xy|·0.0568 > 5, i.e. > 88 u/s; velocity +0x1044/+0x1048) → `KillPlayerInWheel` 0x695e20: 1000 damage, type 0x80 (DMG_CLUB), force 85 in driving direction, attacker = driver.
- Otherwise (teammate or slow) → `ForcePlayerOutOfWheels` (push away).

## 7. Scout sticky stun grenade

- Weapon: `weapon_emp_nf_sticky_stun_grenade` / `weapon_emp_imp_sticky_stun_grenade` (classes `CNFStickyStunGrenade`, `CIMPStickyStunGrenade`), Scout BOX_2, code `GRENADE_NF_STICKYSTUN` (17). Script: damage 100, radius 100, **timer 6 s**, ammo `AMMO_STICKY_GRENADE`.
- Sticks on touch (`StartTouch` 0x3e3070: MOVETYPE/parent, vehicle in +0x508).
- `Detonate` 0x3e2770: if stuck to an **enemy** vehicle → `DisableVehicle(10.0)`; then a normal explosion.
- `DisableVehicle` 0x696ee0: heat = capacity (max) and penalty end `+0x954` = curtime + 10 s → no weapons for 10 s, throttle reduced by heat. Driving still possible.
- Throwing: primary fire, thrown like `CBaseSDKGrenade::ThrowGrenade` 0x445240. Detonates only after 6 s → lead moving targets.

## 8. Readable state (netprops, by name)

| What | Netprop / field |
|---|---|
| Driver | `m_hPassengers` [0] (used by the plugin), `m_hPlayer` (DT_PropVehicleDriveable) |
| Speed | `m_nSpeed` (mph, int; server +0x4a4), `m_nRPM`, `m_flThrottle`, `m_nBoostTimeLeft`, `m_nHasBoost` |
| Heat | `m_flVehicleHeat` (+0xbc0), penalty end +0x954 (no netprop found) |
| Health | `m_flTotalHealth` (CEmpTank NetworkVar), wreck `m_iCarcassHealth` |
| Team / chassis | `m_iTeamNum`, `m_iChassis` |
| Turret | `m_hTurret`, turret `m_iWeaponAmmo`, `m_iGroup` |
| Player | `m_hVehicle` (used by the plugin) |
| Steering | no netprop found; **assumption** pose parameter `vehicle_steer` |

## 9. Orienting the vehicle factory

- `Command_emp_build` 0x651890 with 5 arguments: `emp_build <type> <x> <y> <z> <yaw>`. Yaw = `strtod`, used unchanged in degrees as QAngle(0, yaw, 0) (`[ebp-0x114]`, 0x651d99) → `SetAbsAngles` 0x651ebc. No rounding or snapping in the command. World yaw.
- Exit (attachment `build` via `LookupAttachment`/`GetAttachment` 0x6542aa, read from the `.mdl` files):

| Model | `build` (local) | `build` axis | `ramp` (local) | Model hull x / y |
|---|---|---|---|---|
| NF `nf_vehiclefactory.mdl` | (172.5, 0, 172.5) | −Y (yaw −90) | (517.5, 0, 23) | −414..920 / ±828 |
| BE `imp_vehiclefactory.mdl` | (0, 0, 200) | −Y (yaw −90) | (750, 0, 20) | −748..1200 / −552..677 |

  Vehicle angle = building yaw − 90, vehicle front = entity yaw + 90 → **the vehicle faces local +X (towards the ramp) and drives out there.** At yaw 0 that is world +X. Towards the enemy: `yaw = atan2(enemy.y − vf.y, enemy.x − vf.x)` in degrees.
- Placement: `CanBuildAt(origin, type)` 0x397af0 and `GetAllObstructions` 0x387ed0 (signature without angles) → `GetBuildingHullTrace` 0x387b30 with an axis-aligned box from `buildings.txt`: `CollisionRadius` 600 (± in x/y), `CollisionHeight` 300 (BE) / 500 (NF), offset X/Y 0. **Rotation is not taken into account.** The model hull extends beyond 600 in +X (NF 920, BE 1200): the ramp side can stick into walls despite passing the check; keep the area in front of the ramp clear.

## 10. Repairs

- Repair station `emp_building_{nf,imp}_repairstation` (`CRepairStation::RepairTanksInRadius` 0x677900): every friendly `CEmpTank` with health > 0 and distance² < 360000 (**600 units** from the station origin, `ShouldRepairTank` 0x677870) gets `GiveAmmo(1)` and `DoRepairs(0.5)` per think, with the upgrade flag (+0x5c0) 4 and 2. Only the station and `emp_vehicle_repair_brush` call `DoRepairs`.
- Radio call "I need repairs!": client command `emp_menu_command 30 <location text>` (`CEmpPlayer::ClientCommand` 0x66d200, jump table 0x9d8db0, 50 entries; 31 = medic, 32 = engineer). 2 s lock per player (+0x1534). Sets `SetRequest` and the minimap marker.
- Vehicle health: `m_flTotalHealth` on `emp_vehicle`. Note: `CBotEmpires::readEmpiresFloatProperty` only searches `CEmpPlayer` → always read vehicle values through the vehicle's own ServerClass (`EntityFloatProp` in bot_empires_vehicle.cpp).
- Re-customizing at the repair station (in game: on the pad press V → Customize → Buy, then the armor rebuilds while staying on the pad): same command `emp_vehicle_build` (`Command_emp_vehicle_build` 0x652d70). If the player drives their own `CEmpTank` (vfunc +0x3a0 == player), it sets the re-customize flag (0x653432) and takes its own branch at 0x654750: trace from the vehicle, the hit must be a built station (+0x492), whose upgrade flag (+0x5c0) is taken over. Command sequence as for buying (chassis/weapon/armor/armor_levels/engine/build), without `emp_vehicle_factory`. Untested.
  - Price of a re-customize (read 2026-11-14): `diff = newCost - tank.m_iTotalCost(+0x1040)` (0x6550a7-0x6550ad).
    Check before: `max(diff,0) <= wages + teamRes(-0x390)`, else "Warning: Vehicle recustomization cost ( %i ) exceeds
    available" (0x9d580c). Then `CEmpTank::RecustomizeVehicle` (0x655af6); only on success `m_iTotalCost = newCost`
    (0x655b30) and payment immediately in the command, not per Think: wages >= diff -> `wages -= diff` (0x655db9-0x655ddd);
    else team resources (+0x3b4, +0x30) get `-(diff - wages)` (0x655d2c-0x655d69) and wages = 0 (0x655d6e). Then event
    `vehicle_recustomize` (userid, cost = new total, vehicleid). Same loadout => diff 0 => armor refill is free
    (measured run 20261114-214852: wages 0->0, pool +31 income, armor 14/28/35/35 -> 140x4). Cheaper loadout =>
    negative diff => wages increase (refund). `m_iTotalCost` is only written at 0x6545b8 (buy) and 0x655b30.
    Game quirk: the >200-from-pool commander-limit check (0x655b58) runs after RecustomizeVehicle, so a blocked
    refit is not charged.
- Health fields: `m_iHealth` (+0xf8) = hull, repaired by `DoRepairs` and the kit (`GetHealth`/`GetMaxHealth` vfunc 0x1e8/0x1e0; when full `DoRepairs` calls `ClearRequest`). Armor per side is server-side only: `float[4]` +0xfe4, `int[4]` +0x1004 (order front, rear, left, right; left = local −X, right = +X, front = +Y, confirmed in a run: an engineer on +X heals index 3). `m_iMaxArmorHealth` (+0x1024) = 70 **per inch** (2 inches → 140). The kit heals the side that is hit (`CEmpTank::GetSide` 0x3e61c0 by angle limits from the chassis, then `HealArmorSide`), range 100 → aim at the nearest hull point, not the centre of a face. Only `m_flTotalHealth`/`m_flMaxTotalHealth`, `m_iArmor`, `m_iMaxArmorHealth` are networked.
- Max armor (inches per side, scripts): NF Light Tank 2, BE AFV 3, jeeps 1, NF Medium 5, BE Medium 4, heavy 6.

### Vehicle ammo (read 2026-11-14, static)

- Tank ammo is authoritative on the tank: magazine `+0x90c+4*i`, reserve clips `+0x92c+4*i` (8 weapon slots,
  weapon def `shared_ptr` at `+0xb28+8*i`). The turret netprops `m_iWeaponAmmo` (turret `+0x4f4`) and
  `m_iWeaponClips` (`+0x514`) are mirrors written after each change. A test write must set both.
- Weapon def fields: `+0xcbc` bool (slot skipped by GiveAmmo), `+0xcc0` max magazine, `+0xcc4` max clips,
  `+0xcc8` magazine refill per step (weapons with max clips 0), `+0xcdc` reload time.
- `CEmpTank::AddWeapon` 0x69a090 (vtable +0x3f4) takes the first free slot and fills clips = max, magazine = max.
  `RecustomizeVehicle` calls it for every chosen weapon (0x69f2eb) -> a refit (`emp_vehicle_build` on the pad)
  should refill ammo fully (static; not measured).
- `CEmpTank::GiveAmmo(float mult, bool silent)` 0x697ce0: returns if not alive or `curtime < +0x950`. Per slot:
  max clips 0 and magazine < max -> magazine += `+0xcc8` (clamped); else clips < max -> clips += 1. The magazine of
  clip weapons is not filled here (reload `CEmpTank::ReloadWeapon` 0x68c550 moves clip -> magazine). Each slot that
  needs ammo sets `+0x950 = curtime + reload * mult * 0.5`. Flag `+0x94c` (reset on build/refit/ctor): the first
  call after it only arms the timer, gives nothing. End: `GetPassenger(0)` -> `ClearRequest(4)`; sound
  `Vehicle.AmmoPickup` unless `silent`. No driver needed.
- Repair station: `GiveAmmo(mult, false)` with mult 1.0, 4.0 if the building is sabotaged (`+0x5c0`); station think
  0.25 s, same 600-unit radius as repairs.
- Crates `CEngineerCrate::CrateThink` 0x62af30: think 1.0 s; if `+0x596` set, every same-team `CEmpTank` with
  `WorldSpaceCenter` within **1000 units** (dist² 1e6, 0x957f84) of the crate origin gets `GiveAmmo(1.0, false)`.
  `CEngineerCrate::Spawn` 0x62c5b0: `+0x596` = 1 for `emp_eng_crate` (also `+0x595` = expires: removed after
  **120 s**, 0x976224), otherwise `+0x596` = classname contains "ammo" -> map crates `emp_{nf,imp}_ammo_crate` feed
  vehicles too; health crates do not.
- Measured 2026-11-14 (runs 234818, 234908, 234949, crossroads): an `emp_eng_crate` created by the plugin
  (`IServerTools::CreateEntityByName` + `DispatchSpawn`) ignores the `TeamNum` keyvalue (team read 0); with
  `m_iTeamNum` written directly it feeds a same-team tank at 650-810 units: cannon 0/0 -> 40/0 and MG 0/0 -> 60/4
  within about 13 s (no further rise for 8 s afterwards).
- Engineer kit: `m_iEngineerBuildItem` (player `+0x128c`) dispatch in `PrimaryAttack` (0x3cfb6e..): 1 MG turret,
  2 ML turret, 4 camera, 5 radar, 6 walls, **7 supply crate** (`BuildAmmoCrate` 0x3ce830 -> `emp_eng_crate` at
  `GetBuildLocation`, no construction). Cooldown `m_flEngineerNextSupply` (`+0x12a8`), message "You cannot build a
  supply crate for %f more seconds"; after placing, build item -> 0 and next supply = value at `+0x12ac`.
  (engineer.md listed 0 crate / 7 walls; the dispatch says 6 walls / 7 crate.)
- Radio "I need ammo!": `emp_menu_command 29` (jump table 0x9d8db0 -> 0x66e31a): SayGlobal, minimap order active,
  `SetRequest(GetVehicleRole() == 0 ? 4 : 2)` -> driver = request 4 (vehicle ammo, cleared by GiveAmmo), otherwise
  2. For comparison: 30 "I need repairs!" sets request 3. `GetVehicleRole` 0x65cce0 is -1 without a vehicle.

## Rainbowroad base expansion: offline map inspection (2026-11-14)

- BSP entities: NF starting barracks `(13680,-1136,256)`, CV `(12953,0,339.51)`,
  refinery `(14080,1408,256)`; BE barracks `(-13648,1152,256)`, CV
  `(-13034,0,288.239)`, refinery `(-14080,-1408,256)`. Barracks locations are
  references, not individual spawn points.
- The nav file contains 726 areas. Examples of sloped front areas: NF area 92
  `x=7100..7650 y=275..700 z=260..382`, area 120 `x=6950..7575 y=-850..-600
  z=256..444`; BE area 93 `x=-8050..-7500 y=-875..-450 z=257..379`, area 133
  `x=-7950..-7325 y=425..650 z=259..447`. Approximate areas to inspect on the
  start side are NF `x=8500..11500`, BE `x=-11500..-8500`; these are hypotheses,
  not verified building footprints or proof of the first driven ramp.

## Vehicle weapon ammunition (expert static inspection, 2026-11-14)

- `m_hTurret` leads to `DT_EmpTankTurret`; its `tankturretdriverdata` contains
  eight-element `m_iWeapon`, `m_bWeaponGroup`, `m_bWeaponMode`, `m_iWeaponAmmo`,
  `m_iWeaponSlot` and `m_iWeaponClips` tables. `SendPropArray3` exposes numbered
  child properties in data tables, not a `DPT_Array`. Validate parent and child offsets.
- `AddWeapon` 0x69a21d–0x69a420 writes the weapon ID and ammo/clip values at the
  same array index, with chassis slot separately. For the usual bot loadouts find
  standard MG ID 0 and cannon ID 12 by ID rather than assuming an NF/BE array index.
- `ReloadWeapon` 0x68c550 uses ammo as rounds in the magazine and clips as reserve
  magazines; it fills the magazine and decrements reserve unless the weapon's infinite
  flag is set. State and offsets remain to be checked in actual game runs.
- Run `20261114-144912-emp_crossroads-6b-25m`: BE userid 6 turret 571, raw active
  group 0, both weapons in group 0: cannon ID 12, chassis slot 0, mode 0, ammo 40,
  clips 0; MG ID 0, slot 1, mode 1, ammo 60, clips 4. One fire request; no measured
  before/after shot or button/group crossover yet.
- Static inspection: `m_bWeaponMode` is eight bytes, `m_bWeaponGroup` 48 bytes
  (`group[6*i+g]`) and `m_iGroup` the active group 0..5. A button can fire several
  weapons in one group. `IN_ATTACK` passes runtime mode 1, `IN_ATTACK2` mode 0;
  independently verify the relationship to buy commands and UI groups in game.
- `scripts/vehicle_weapons.txt` (from `scripts_dir.vpk`, 2026-11-14), index = weapon ID:
  0-11 machine guns (type 0; ammo type 1/2 = armour piercing/explosive for IDs 1, 4, 5,
  8-11), 12-22 tank cannons (type 1, clip 40 or 30, `Total Ammo Clips` 0 = reloads one
  round at a time), 23-26 artillery (type 2), 27-39 missile launchers (type 3), 40-42
  grenade launchers (type 4); array value 255 = empty entry. Standard MG clip 60 x 4
  and Standard Cannon 40 x 0 match run 144912 (60/4 and 40/0), so the arrays read
  magazine and reserve magazines as expected.
- Weapon slots per chassis (`scripts/vehicle_*.txt` `weapon_slot`, in slot index order,
  T = accepted weapon type, S = slot size): NF light tank T1/S1, T0/S1, T3/S1; BE AFV
  T0/S1, T1/S1, T3/S1; NF/BE medium T1/S2, T0/S2, T3/S2; NF heavy T1/S3, T0/S3, T3/S3,
  T3/S3; BE heavy T1/S3, T1/S3, T0/S3, T3/S3; APCs T0/S3, T4/S2; artillery T2/S2; jeeps
  and commander vehicles none. Weapon `Size` (vehicle_weapons.txt): size 1 only for
  Standard MG 0, Chain Gun 6, Standard Cannon 12, Standard Missile Launcher 27 and
  Grenade Launcher 40; researched weapons are mostly size 2-3. A weapon must match the slot
  type and fit its size (build otherwise rejects with `Warning: Weapon ...`), so the bot's
  light tank / AFV can only take size-1 weapons; slot 2 is the missile slot on both.
  Weight: light tank max 260, AFV max 320; cannon 50, MG 30, missile launcher 40 (armour and
  engine weights not yet added up; whether cannon+MG+missile fits is unverified in game).

## Wages and team pool (expert static inspection, 2026-11-14, untested)

Measured 2026-11-14 (plugin `vehicle econ` lines): reading +0x12c8 gives plausible wages (15, 20
after kills); a BE purchase took wages 15 -> 0 and the pool -207 (price 222), so wages are spent
first as described. Light tank cannon+MG, armour 2/2/2/2, engine 13: 192-245 from the pool
(includes a few seconds of income). The game event `vehicle_purchase` never reached the
plugin's FireEvent hook (also with `rcbot_loglevel 3`); cause unknown.

Chat (expert static, 2026-11-14; measured with a bot's say): `say`/`say_team` are game-DLL
ConCommands (`say_command` 0x00c2e200, `Host_Say` 0x5e5d40) and never reach the plugin's
`Hook_ClientCommand`. `Host_Say` fires `player_say` (0x5e610f, keys `userid`, `text`), which the
plugin's event list receives. No game command transfers wages; there is only `m_flWages`.
Writing it from the plugin: `edict_t::StateChanged()` / `StateChanged(offset)` need engine-only
symbols (`GetChangeAccessor`, `g_pSharedChangeInfo`) and stop the plugin from loading; set
`m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED` instead.

- `CEmpPlayer::m_flWages` float at player +0x12c8 (NetworkVar Set 0x3d1670; the name string
  exists, whether it is a SendProp is unverified). Kept across the round via
  Store/RetrievePlayerRoundStats.
- Income: `CEmpPlayer::IncrementRank` 0x663b46-0x663b7e adds the rank-point entry's value
  (+0xc) on score events (kills etc.); engineer-kit recycling adds 0.25 x recycled amount
  (0x3d0f32, constant at 0x950328). No time-based payout found; cvar `emp_sv_wages`
  (0xc36f40) has no reference besides its constructor (assumed unused). User: wages rise
  with fighting / squad points.
- Purchase payment is automatic: check 0x65448d `(int)team + wages >= cost`. If wages >= cost,
  wages -= cost (0x6545bd-0x6545f5), `builtfromwages=1`, team pool untouched. Otherwise
  (0x65595f) rest = cost - wages is taken from ledger +0x30 (0x655a16, also logged in the
  ledger float list +0x44, count +0x50) and wages set to 0 (0x655a28), `builtfromwages=0`.
  No game-side reservation: anyone can drain the team pool.
- Research cost is drained over time (`CEmpTeam::ResearchStartDrain` 0x6818d0,
  `CEmpTeam::Think` 0x680c40); research pauses when money runs out.
- Engineer kit build path (0x3cf110) has no team-ledger access (assumed free); commander
  walls use the ledger (`CEmpGameRules::CompleteWalls` 0x397fc0).
- Wage sources (expert, static): only `CEmpPlayer::IncrementRank` 0x663a00 raises wages, by the
  "wages" value of the rank point from `scripts/rankpoints.txt` (`ParseRankPoints` 0x3a7ca0,
  in scripts_000.vpk); XP at player +0x12c0. Values: kill 5, vehicle kill 10, building destroyed
  5, 200 building damage 5, sabotage 5, revive 5, heal 5 / 100 HP, repair 3 / 100 HP, recycle
  5 / 100, flag capture 3, defuse 3, spotting 8 targets 5, assists 1 per 2 (critical) or 3,
  squadbonus 2, squadleaderbonus 2, commanderbonus 0.
- Squad bonus: `CEmpParams::AddSquadPoints` 0x641c80 (callers IncrementRank and
  `emp_increase_squadpoints`); every 3rd squad point (counter +0xac8, reset 0x641e48) each team
  player in that squad (player +0x1508) gets squadbonus / squadleaderbonus (+0x150c), the
  commander commanderbonus 0: about +2 wages per member per 3 scoring events. Untraced whether
  every member's score event calls it.
- Research cost is per second: `"Cost"` x `"Time"` (Structural_Engineering and Vehicle_Factory
  each 4/s x 60 s = 240).
