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

Consequence: cp_boat cannot be captured by navmesh bots until deck areas plus a ladder link
exist, or a swim-to-ladder/climb behaviour is added. Until then a goal whose nearest nav
area is far below it (no area within ~120 units) should be skipped.

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
