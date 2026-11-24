# Map notes

## emp_palmbay: boat capture points (cp_boat)

Measured 2026-11-15 (BSP lumps, `emp_palmbay.nav` brute-force search, `empires_nav_scan`,
run `dev-runs/20261115-162015-emp_palmbay-1b-3m-p49975`).

Geometry
- Two `emp_cap_point` brushes named `cp_boat`: `*27` origin (225.5 4846 -2836), bounds
  x 1..450, y 4601..5091, z -2936..-2736 ("Jekotian Battleship"); `*28` origin
  (-1207.5 905.5 -2836), half-extent 158 x 238 x 100 ("Brenodi Luxury Liner").
- Water surface (all `CONTENTS_WATER` brushes): z = -3024. Sea floor under boat 1: about -3260.
- Deck of boat 1: z about -2935 (ammo crates `nf_ac_4` (204 4694 -2934.7), `imp_ac_4`
  (294 4965 -2934.4); the capture brush bottom is the deck). Hull bottom about -3066
  (a bot swimming straight up under the boat stops at origin z -3138).
- The flagpole `emp_cap_model` origin (341 5022 -3055) is below the deck; do not use it as a goal.
- Access is by ladder only: `CONTENTS_LADDER` brushes (contents 0x38000008), z -3059..-2932:
  boat 1 at x 372..385 y 4781..4803 and x 105..120 y 4929..4949; boat 2 at
  x -1034..-1033 y 843..867 and x -1333..-1327 y 800..823. No `func_ladder` entity.
  Ladder bottom is 35 below the water surface, about 200 above the sea floor.

Navmesh
- No nav area exists on either deck: no area in the .nav file with z above -3080 in
  x -1500..1400 / y 4300..5400 or around boat 2; only sea-floor areas (boat 1: area 692,
  x -250..575, y 4625..5100, z -3271..-3238). The generator floods by walking; the ladder
  hangs in open water, so the deck is never reached and never generated. No nav ladders.
- `groundApproachOrigin` (226 4846 -2934) is correct (deck). `BuildPath`
  (`bot_navmesh_api.cpp` ~1286) picks `GetNearestNavArea` = sea-floor area 692, so the route
  ends at (226 4846 -3246), 312 below the goal, under the hull.
- Arrival: for capture goals a drained route counts as arrived with a 2D check only
  (`bot_empires_objective.cpp` ~1815, `closeInHeight` is only required for repair), so bots
  "arrive" under water and hold 30 s -> drown (Recorder session-20261115-161202, NF_02).

Consequence (2026-11-15): cp_boat could not be captured until deck areas plus a ladder link
existed. Resolved 2026-11-22, see "Boat ladder climbing" below.

Navmesh side (2026-11-22):
- A mesh regenerated with the current generator has deck areas (seeds above every
  `emp_cap_point`, `nav_generate.cpp` AddWalkableSeeds): boat 1 51 areas, boat 2 60 areas.
- `ConnectGeneratedLadder` found no bottom area: `GetNearestNavArea` measures from the ladder
  (z -3034), the sea floor is about 216 below (limit 200). A water bottom now searches 400.
- It then disconnected the bottom again as "dangling" (bottom more than HumanHeight above its
  area) while keeping `m_bottomArea`, so logs showed an id but A* had no ladder (no-path with
  equal components; components also link the deck-to-sea-floor drop). Water bottoms skip that.
- The route adds a point at floating height (bottom - 16) in front of the ladder; going down,
  the foot becomes a floating point 48 out (the sea-floor foot lay 7 sideways from the top and
  the bot stood still on the deck).

### Boat ladder climbing (2026-11-22)

- `CGameMovement::LadderMove` (0x3f2020) is the stock SDK 2013 code: mount trace =
  `TracePlayerBBox` along wishdir for `LadderDistance()` = 2.0 (0x395090, float 0x950348) with
  `CEmpGameMovement::LadderMask` (0x3950c0, player solid mask); `OnLadder` (0x3fd620) accepts
  `contents & CONTENTS_LADDER` or a climbable surfaceprop. On the ladder it climbs only from the
  buttons IN_FORWARD/IN_BACK/IN_MOVELEFT/IN_MOVERIGHT (mv+0x24 tests at 0x3f2390..0x3f23bd,
  `ClimbSpeed()` 200, 0x3950a0); with none set the velocity is zeroed. IN_JUMP (test 0x3f23c3)
  sets MOVETYPE_WALK (0x3f26e0) and velocity = plane normal * 270 (0x9773f0) in the same call
  that mounted (SetMoveType 9 at 0x3f224c): a held jump (surfacing) kicks the bot off on contact.
- Boat 1 ladder brush 225 (contents 0x38000008) is a 24 x 1.2 slab, face normal
  (0.887 -0.462 0), plus axial bevel planes (x 372.3..384.7, y 4780.7..4802.7). A box touching
  the slab's ends mounts on a bevel plane (thrown along +x or -y on jump), which still climbs.
- The `metalladder001` prop (collision: rails +-3 deep, 24 wide, 128 tall, from the .phy) sits
  0.5 behind the brush face and does not block mounting.
- Cause of "hangs on the ladder, then drops off": during a Commander MOVE
  `CBotEmpires::alterOutgoingUserCmd` stripped IN_FORWARD/... after `runPlayerMove` had added
  them for MOVETYPE_LADDER; the bot hung still (origin constant 2 s, buttons 0x0), then
  `CBot::checkStuck` pressed IN_JUMP and LadderMove threw it 270 off the ladder
  (run `dev-runs/20261122-044528`). Keeping the buttons on a ladder: climbed on the first grab
  (`20261122-044646`).
- Second failure mode (`20261122-044747`, same build): the hull (`boat_fishing01a` .phy piece with
  face normal (0.93 -0.37), z -3066..-2935) bulges out past the south end of the ladder slab. A
  bot that drifts south of the ladder front slides along the hull into the corner at about
  (385 4760); the mount trace toward the deck hits the hull first, so it never mounts
  (navmesh-stuck-trace normal (0.85 -0.52), frac 0). Mounting works from in front of the slab
  centre (pressing along -normal) or from the east end (x-max bevel, run 044646).

### BE APC approaches to the southern boat

In Palmbay run `20261121-212957` (checkout `8171745`, dirty, plugin hash prefix `a03bdce6b57c84ae`), six BE APCs logged a fording depth of 160 HU. Each retreated at 58/150 hull HP with full armour before its driver died to world damage.

| Support point (approximate XY) | Ground Z | Depth below surface | Vehicles | Distance from support point at retreat (2D) |
|---|---:|---:|---|---:|
| (-1766, -157) | -3231 | 207 HU | 235, 236, 216, 224 | 318–340 HU |
| (-1160, -294) | -3158 | 134 HU | 239, 222 | 807–814 HU |

The first logged vehicle origin below the surface was 229–989 HU in horizontal distance from the assigned spot across the six approaches. Positions are sampled. They bound the entry interval between consecutive samples and cannot identify the first engine-cutoff frame. At the shallower goal the two APCs retreated with their origins at Z -3240 and -3242. Checking the destination depth alone cannot establish a safe driven approach or stopping position. This run predates the parking changes.

## emp_dmz_b1: no-path to almost every goal

Measured 2026-11-15 offline (`emp_dmz_b1.nav` and `emp_dmz_b1.closed-gates.nav`: same 18101 areas
and geometry, only area ids renumbered; BSP entity lump; run
`dev-runs/20261115-161624-emp_dmz_b1-5b-20m-p49970`, 660 `navmesh-plan-failed`).

Map
- Cap points: `northflag` (0 13248 1456) +-128 xy, z 1264..1648 (on the rock); `midflag`
  (0 -2624 1560) +-664/1208; `southflag` (0 -11776 1416) +-1128/768; start flags (+-12768/-12032 6912).
- Gates open by player count (`players_unlock_*`, thresholds 16/20/28/32): `door_tunnel2`
  (+-3872, -3200..-2048, z 1440..1920, 28 thick), `door_center_lower/upper` (+-1016 -5728),
  `door_tunnel1` (+-11240/11800 -3616), `tunnel1_lock_*` func_brush (+-11520 -3516). With
  fewer than 16 players they stay closed; `trigger_hurt` crushers sit at the gates.

Navmesh components (all generator links, undirected): 44; main 11110 areas (north field,
both bases, west/east flanks), centre 1090 (`midflag`), south 5854 (`southflag`). The gaps are
exactly the closed gates: centre<->main 50 units at x -3900..-3850 (door_tunnel2), south<->main
50 units at y -3650..-3600 (door_tunnel1). Same z on both sides, no climb involved.
- Edge classes over the whole mesh: 91089 links, climb 40..58: 199, climb > 58: 23, drop > 200:
  35. Directed reach from the NF side: all links 11028 areas, bot limits (40/200) 10977, climb 58
  11026. None of the goals changes class: the 40..58 links do not separate anything relevant.
- `northflag` rock: 45 areas x -200..175, y 13050..13375, z 1241..1357. It has 20 one-way
  links down (drops 82..113) and no link up, in both nav files. Nearest ground areas are 25..50
  units away across the steep rock face: west area 14221 (-300..-225, 13175..13225, z 1172..1189)
  to rock area 35411 (-200..-175, 13250..13300, z 1241..1259): climb about 52; east area 21149
  to 43673 climb 22..35 but 50 units of slope in between (corner contact only). No climb limit
  can open it; it needs an added jump-up link (crouch jump about 56) and a check in game.
- Because the drop links join the rock to main, `EnsureNavComponents` (undirected,
  `WalkableEitherWay`) puts the goal in the start component, so the island snap in `BuildPath`
  never runs and every plan to the flag returns no-path (80 of 660 failures).

Main cause of the mass failures: turret danger circle. At log line 545 NF bots note BE MG
turret 85 (lvl3, `empiresMgTurretRange` 4000) at (-4160 7296). `LearnedNavMeshCost` forbids
entering the circle (hard -1). With the gates closed the only east-west corridor is the north
field (x = 0 crossings only at y 6000..13750), and the circle cuts it: offline, with the circle,
NF reach drops from 10977 to 6538 areas and reaches none of the west goals. Failures before the
turret: 22 (only rock, midflag, southflag); after: 638 to every west goal. The circle stays until
the turret dies.

Consequence: generating the gates as passable is only correct when the runtime blocks those
links while the gates are closed (otherwise routes run into closed gates and crushers).

## emp_coast: centre flag inside a bunker

- User (2026-11-20): the centre capture point sits inside a bunker. Vehicles cannot reach it; a tank's flag
  goal there must be a vehicle-reachable support spot outside the bunker, not the flag origin or the
  infantry ground approach.


## District ticket test (2026-11-24)

The map BSP contains an `emp_info_params` entity. The game FGD lists `SetNFTickets`, and the Linux binary's `CEmpParams::InputSetNFTickets` writes the team ticket balance at +0x3cc. The Linux `ent_fire` handler returns early if `UTIL_GetCommandClient()` returns null. The dedicated console attempts did not change District's ticket count. Sending the command through an RCBot test client and passing the reinforcement multiplier before `+map` also left the starting count unchanged in these runs. The built-in kill-zone fixture consumed tickets through ordinary respawns.
