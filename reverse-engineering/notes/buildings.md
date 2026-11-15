# Buildings, Resources, and Research

This note groups shared native anchors.

## Static anchors

| Symbol or string | Evidence |
|---|---|
| CEmpBuilding | CONFIRMED RTTI/typeinfo and vtable symbols |
| CEmpParams::AddBuilding(CEmpBuilding*, int), 0x00641750 | CONFIRMED exact symbol identity |
| CEmpParams::RemoveWalls(int, int), 0x00641910 | CONFIRMED exact symbol identity |
| CEmpParams::SetWallLimit(int, int), 0x006455d0 | CONFIRMED exact symbol identity |
| CRefinery DataMap/ServerClass initialization | CONFIRMED generated symbol names |
| emp_building_refinery | CONFIRMED embedded string |
| resource/maps/%s.txt | CONFIRMED embedded string |
| emp_sv_research_item, emp_sv_unresearch_item, emp_sv_research_complete | CONFIRMED embedded command strings |
| CEmpResearch | CONFIRMED class/type symbol evidence |

## Separate the layers

- **Building class/API presence:** Symbols establish named types and methods in the binary.
- **Refinery/resource data:** Class and resource strings identify useful search areas, not autonomous economy logic.
- **Placement rules:** Placement-rule names are static search anchors. They do not establish the complete validation algorithm or legal runtime placement.
- **Autonomous bot economy/placement policy:** The binary’s native features do not show that RCBot2 chooses or places refineries.
- **Commander research policy:** Native research commands and CEmpResearch symbols do not establish autonomous commander research by a bot.

Binary strings and symbols do not establish that RCBot2 has refinery placement or autonomous research behavior. Continue with a narrow call path only when a bot task needs that mechanism.

## Commander building via `emp_build` (confirmed 2026-11-10)

`Command_emp_build` 0x651890 (only if player vfunc 0x72c = commander), argument forms by `argc`:

| Form | Meaning |
|---|---|
| `emp_build <type> x y z yaw` (argc 6–7) | Building at a position. **Type 0 (refinery) does nothing here**: jump table 0x9d59a0, entry 0 = 0x65201d. |
| `emp_build 0 <resIndex> <yaw>` (argc 3–5) | Refinery. argv[1] is ignored (type fixed to 0), argv[2] = `m_iResIndex` (CResourcePoint +0x4b0, netprop; running number from `CEmpParams`+0xbc0 in `Spawn`), argv[3] = yaw. Looks up the `emp_resource_point` with this index and `IsEnabled` (+0x4a8) and places the refinery at its origin. |
| argc ≥ 8 | type argv[1], second vector argv[2..4], position argv[5..7]. |

Checks (in order): research (`ResearchIsItemUnlocked`, message "not researched"), building limit (`#emp_building_limit_reached`), then the entity is created and possibly rejected. Rejection = `CBaseEntity::Remove` → `UTIL_Remove`: only `EFL_KILLME` (m_iEFlags +0x118, bit 1), the edict stays **occupied until the end of the frame**. A before/after comparison in the same frame therefore reports rejected buildings as "created"; check with `EmpiresEntityMarkedForDeletion` (bot_commands.cpp) or one frame later.
- Normal buildings: `CanBuildAt` (only type ≠ 0), cost > team resources (CEmpTeam+0x3b4 → +0x30 float) → Remove 0x652b82, `GetAllObstructions` ≠ 0 → Remove 0x652015.
- Refinery: cost > resources, `CanBuildAt(point origin)`, hull trace 50 down at the point: only a `worldspawn` hit is allowed; `emp_eng_crate`/`prop_physics_multiplayer` are removed; any other entity (including a player inside the hull) → Remove 0x652d12. No `DistanceToCV` check in the refinery path except through `CanBuildAt`.
- `CEmpGameRules::CanBuildAt(pos, team)` 0x397af0: true only if a **living non-commander teammate** is within 4000 (dist² < 16e6) **or the own CV** is within 4000; with CV distance > 1000 additionally false if a living enemy is < 1000 away or an enemy building is within a 1000 radius. The commander in the CV does not count as a teammate.
- `GetAllObstructions` 0x387ed0 flags: 0x8 = > 4000 from the CV without a teammate in range, 0x40 = refinery without a resource point, 0x4/0x2 = resource point disabled/hull.
- Costs (scripts_000.vpk buildings): Refinery 100, Armory 100, Repair 150, Barracks 300, Radar 400, VehicleFactory 500 (research "Vehicle Factory"), Turrets 75, Wall 15.

Measurements (emp_streetsoffire, NF CV at (-7548 -14179 -192), probe `empires_commander_build`, runs 2026-11-10):
- Refinery via the coordinate form at RP 0 (-34 -12542): no effect; RP 0 is also occupied by the map refinery 153 (`enabled=0`).
- `emp_build 0 6 0` (RP 6 at (-8369 -3974), 10239 from the CV), teammate far away: `rejected=1`. Teammate 200 next to the point (inside the hull): rejected. Teammate 1000 next to the point: refinery 520 stays, RP 6 `enabled=0`.
- Factory (type 5) at the CV origin: `rejected=1`. Index 520 was only reused afterwards; the factory never existed.
- Team resources: `CEmpTeam` (ServerClass name, entities 33–36 = team 0–3) +0x3b4 → resource object, +0x30 float (confirmed via `emp_give_resources` 0x647610: NF +2000 shows up there). Team number `CTeam` +0x3a8 (`CTeam::GetTeamNumber` 0x8775d0), not +0x208. Start on emp_streetsoffire: NF 5031.
- The factory rejection is **hull, not cost**: still rejected at 5031 and 7033 resources. Direct call to `GetAllObstructions` (probe `obstruct <type> x y z`): type 5 at the CV `0x22`, at the barracks site (-7588 -13662, 524 from the CV) `0x2`, barracks there `0x0`; type 5 at (-7548 -13179, 1003 from the CV) `0x0`. Built there: factory 523 stays, resources 5031 → 4533 (cost 500). Flag 0x2 = building hull hits something, 0x20 comes from the collision checks (`GetCollisionObstructions`/`GetGeneralCollisions`).
- `empires_commander_place_barracks` at (-7588 -13662): barracks stays.

## Changing team resources (expert 2026-11-15, static)

- `CEmpParams::InputModifyNFResources` 0x647530 and `emp_give_resources` 0x647610 both do: ledger = `CEmpTeam+0x3b4`,
  `ledger+0x30 += x` (float), then `AddToTail` on the ledger's history vector `+0x44`. The ledger is not an entity, so
  no network change flags are needed. A plugin can subtract cost by writing `+0x30` directly (history entry skipped). The
  history is summed into ledger `+0x3c` each period by `CResourceManager::CalculateAdjustments` 0x6785f0 and
  networked as `CEmpTeam+0x3c0` (`CEmpTeam::Think` 0x680cf9); skipping it only hides the change from that
  "adjustments" value, the pool `+0x30` is correct. `CUtlVector<float>::InsertBefore` 0x39fad0 would add it. `emp_give_resources` needs a server admin (`UTIL_IsCommandIssuedByServerAdmin`),
  i.e. works from the server console.
