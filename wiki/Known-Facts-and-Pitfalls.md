# Known facts and pitfalls

Sources: [AGENTS.md](../AGENTS.md) ("Known facts"), [docs/OPEN_WORK.md](../docs/OPEN_WORK.md), `reverse-engineering/notes/*.md`, `README.md` (known issues), code comments. These are **measured or
statically verified**; do not re-derive them, but re-check after a game update (binary size/CRC changes).

## Engine and game facts

### Time and projectiles
- **Mortars and tank cannons only work at `DEV_TIMESCALE=1`.** Both are `CEmpShell`: it stores `gpGlobals->realtime` at spawn and explodes when
  `curtime - realtime-spawn > 30` (0x3abe90); with `host_timescale` > 1 every shell detonates at the muzzle (0 hits). Fast-forward only for driving/economy.
- Seismic grenade projectile gravity is about 320 (measured apex/descent, runs `20261123-015951`), not `sv_gravity` 800; fuse about 4.7 s; 20 damage per hit on a barracks;
  ammo does not decrease. A standing throw reaches about 1750 HU flat. The building's `emp_building_vehicle_physbox` bounces grenades back, so the landing point
  must be in front of it.
- Vehicle MG is **hitscan** (`Speed` in `vehicle_weapons.txt` applies to projectile types 1-4 only); no lead for moving infantry, cannon leads moving targets.
- Tank turrets read the command **view angles relative to the hull**, not world angles (shots flew rotated by the hull yaw before the fix).

### Engineer and buildings
- The engineer kit traces only **100 units from the eye** (second trace 150), `CWeaponNFEngineerKit::PrimaryAttack` 0x3cf110. Building: 1 clip round = 1 HP,
  about 4.7 HP/s with clip, 2 HP/s without.
- The kit places the selected deployable instead of healing while `m_iEngineerBuildItem != 0`; `ItemPostFrame` skips `PrimaryAttack` with an empty clip.
- Revive: dead same-team player with ragdoll, target within 512 HU of the reviver, active kit with clip >= 50 (spent). The command itself does not check the Revive
- A revived player lies on the ground: the collision box is still 72 at the revive and drops to 16 right after it; pressing duck stands up (0.6 s in `dev_revive`), jump does not.
  skill byte; the bot only revives with the skill active.
- Skill slots by rank points: slot 1 at >= 0, slot 2 > 9, slot 3 > 29, slot 4 > 59. `changeskill` only stores the wanted skill; it becomes active at spawn.
- Commander building rules: `emp_build` needs commander status; refinery is `emp_build 0 <resIndex> <yaw>`; placement allowed only if a living non-commander teammate or the own
- `emp_build <type> x y z` uses the given position as is; requested at the ground, commander buildings land 10 (turrets 14) units sunk in, while a human commander's turret stands on the ground. A turret is folded (`deploy` sequence) until full health, then switches to `idle`. Scoped rifles toggle iron sights on secondary attack (`m_bInIronSights`).
  CV is within 4000 (with CV distance > 1000, also no enemy within 1000 / enemy building). Costs: Refinery 100, Armory 100, Repair 150, Barracks 300, Radar 400, VF 500, Turrets 75,
  Wall 15. Rejected buildings are removed with `UTIL_Remove`: the edict lingers until frame end (`EFL_KILLME`), so check `list` later. Submission is not success.
- Research costs are **per second**; running out of money pauses it, and the game never resumes by itself: the commander must send `emp_comm_research_resume`.
- `emp_sv_research_complete` is 1 in `dev.sh` by default; set 0 for natural research.

### Vehicles
- Purchase pays from the buyer's own **wages first**, only the remainder from the team pool; no reservation in the game. Wages come only from rank points (kill 5, vehicle kill 10,
  revive 5, repair 3 per 100 HP, squad bonus), not over time. A pad refit with the same loadout is free. Light tank price about 192-245.
- The game's `IsInFactory` traces upward: the buyer must stand **under the factory roof**. Wrong argument counts for `emp_vehicle_*` commands are silently ignored.
- Water: a vehicle's engine stops after 1 s and takes 20 HP/s once its centre is wet; vehicles ford only as deep as the centre stays dry.
- A vehicle in the factory's `build` attachment area (+-150) blocks every purchase (5.8a).
- Spawn at an APC seats the player; APC and Commander-built barracks spawns are ordinary `emp_info_player_*` entities (no new offsets needed).
- A vehicle loss can show up as `committed suicide with "world"`: read vehicle state and position before the death.
- Vehicle weapons must fit the chassis slot type and size (Light/AFV take only size-1 weapons; slot 2 is the missile slot).

### Orders, spawn and squads
- A MOVE order ends in the game only within **100 units (3D)** of its location. Move to an off-mesh target stops at the nearest reachable point on the same level.
- Type 3 (attack target) orders from a bot commander are dropped by the game unless the target was transmitted to the commander's client (`+0x14ec`); a test hook marks it.
- `m_iCommandTarget` is an array of 32 ints; read element 0.
- Bots joining via `jointeam` die on a team change in Unassigned unless prepared (fake-client suicide, RE `spawn.md`); `FinishClientPutInServer` skips the dead-spawn block for bots.
- Squads: IDs 1-26, at most five players per squad; leader position is 0 (position 0 is not a generic member location). `emp_make_lead` can be rate-limited.
- Ladders (`CGameMovement::LadderMove`): only `IN_FORWARD/BACK/MOVELEFT/MOVERIGHT` climb; with none set velocity is zeroed; a held `IN_JUMP` kicks the bot off (270 units).
- Without any player in the opposing team, a destroyed barracks ends the round and freezes everyone (flag 0x40) -- looks like a bot hang. `dev_seismic.sh` parks a scout at the far end.
- `emp_streetsoffire` has **no flags** (captures are always 0).

## Navigation facts
- Navmesh limits (`bot_navmesh_api.cpp`): climb at most 40, drop at most 200. A route point counts as reached at <= 18 below. `jump()` every frame.
- The generated mesh does not know buildings; `ApplyBuildingDetours` handles them (goal inside a building: nearest free cell outside; `PointNearBuilding` ring scan).
- `kFailureCooldown` 15 s blocks the same goal+segment after a hang; retry with `clearNavigationAttemptCooldownForStartAnchorRetry()`.
- `navmesh-stuck(-trace)` lines show the route point and what the hull hits; `worldspawn` = map geometry.
- Hang rule in the evaluator: alive >= 20 s and moved < 100 units. "Stuck" has a task, "Idle" has none. Idle includes commanders sitting in the CV (about 470 s of 1365 s in the reference run).
- `empires_nav_scan` needs at least one bot (else `navmesh-not-loaded`) and uses the player hull; an overview image does not prove vehicle clearance or dynamic building obstacles.
- Maps like Palmbay use displacement terrain; cliffs are steep triangles, not brushes.
- The Navmesh path follower overwrites the move target every server frame (see [Code map](Code-Map.md#documented-pitfalls-in-code-and-notes)).
- Game binary: 32-bit, symbols present (`nm -C`, `objdump -d -C`); the checked build is size 14,863,947 bytes (guarded in `bot_empires_binary.cpp`).
- Missing symbols: plugin loads as "RCBot2 not loaded" (`undefined symbol` in `server.log`); check `nm -uC <so> | grep Empires`.

## Evaluation pitfalls
- Build, map, speed and trigger must match when comparing runs; one match comparison is only a hint.
- The side that bots are dropped on decides short duels; swap sides (duel `131000`/`131009`: BE won both).
- `dev_check.sh` uses five consecutive ports; pick a free range for a parallel run. Parallel real servers/clients may be running: never kill by name.
- `dev_run.py` counts a premature server exit until the recorder finishes and ignores dumps of Steam WebHelper or another server port.
- Distinguish "run folder exists" from "verified success", fire requests from shots, a successful run from reproduction of the original failure.
- `server.log` has thousands of lines; filter, never read it whole. Log `engine->Time()` is real time even with timescale.
- A single refinery turret placement failure is silent in the game (`TextMsg` rejections use another interface); the bot cannot see the reason (1.5a).
- Coast refineries (5.8e): historically only the entity number of the blocking trace was logged; the effect of the physbox filter is unmeasured.
- Verify straight factory departure after a regular purchase at normal game speed; check both sides of the vehicle's path when diagnosing a blocked exit.
- Do not call a resupply "working" without verified ammo gain; the sticky-grenade reserve showed the same value (2) before and after crate placement in `021732`.

## Behaviour traps
- Release the navigation route (`cancelNavigationAttempt`) before holding a bot still; `stopMoving()` is not enough.
- Navmesh-less building interiors: bots can end up inside barracks hulls (`start_solid=1 hit=emp_building_nf_barracks`, item 3.2).
- Tanks stall in the Palmbay palm cluster (2800-3300, 800-1550) on the way to the front (about one give-up per 14-minute run).
- Blocked vehicle exits: a bot boxed in on all sides stays seated and retries exiting every about 15 s.
- Local vehicle maneuvers sped up obstacle passing but lowered open-road speed (BE median 244 to 153 HU/s); they stay off (parked).
- `Bad SetLocalAngles(..., -nan)` about twice a minute: NaN view angle is zeroed before the user command and logged (`viewangles non-finite`); source open.
- Navmesh trap on `emp_streetsoffire` near (-4245 -5424) (5.3).
