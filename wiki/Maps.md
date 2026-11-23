# Maps

Sources: [README.md](../README.md) (map coverage, known issues), [docs/OPEN_WORK.md](../docs/OPEN_WORK.md) (items 5.11, 5.12, 5.16, 5.17, 3.9, 3.9a, 3.15, 5.8c, 5.8e), `reverse-engineering/notes/maps.md`, `scripts/dev_navgen.sh`, `scripts/dev_maps.sh`, `tools/`.
Snapshot 2026-11-23. Run IDs name test runs (folders under `dev-runs/` of the test server).

## Coverage

21 of 22 multiplayer maps have a generated navmesh (`empires/maps/<map>.nav`) and completed a short run without a crash (5.11). Maps without flags cannot
test capturing. Sources name 21 maps; the 22nd is not named anywhere (open).

| Map | Navmesh / run state | Notes |
|---|---|---|
| `emp_crossroads` | yes; main benchmark | 10-bot 1x 15 min `20261123-122645`; 32-min 18-bot run + mapchange `20261122-131813`. Base gates/ramps for vehicles and infantry still problematic (3.9, `nf_gate*`/`be_gate*`, built ramps `emp_eng_map_brush`); pad entrance timeouts (3.12, parked); NF VF at `(-1500 -12900 -234)`; Lawliet tests run here. |
| `emp_palmbay` | yes; benchmark | Displacement terrain. Boat flags (`cp_boat`) captured by swimming to a ladder and climbing (verified `20261122-124651`, spawn-APC fix `131210`). NF finds no VF spot near its CV and builds remotely; NF commander entry can stall at spawn; palm cluster (2800-3300, 800-1550) stalls NF tanks (about one give-up per 14 min); a CV once wedged under a static structure beside the NF refinery; BE APC water losses at fords of depth 134-207 HU (before parking fixes); BE VF site reserved (5.8c, remaining CV cliff fall `111840`). Sporadic refinery rejection (5.8d). |
| `emp_coast` | yes | `20261122-124709`: 0.5 % stuck, two bots pushed flag 168 together. Centre flag inside a bunker: vehicles need a reachable support spot outside. Two refineries without an engineer stand spot (5.8e, parked: trace starts solid / no kit aim; physbox filter effect unmeasured). Mapchange target of the long run (14,334 areas). |
| `emp_rainbowroad` (also `emp_rainbowroad_b4`, manual test map) | yes | Bill Nye jeep map (NF); some infantry routes fail, `start-area-not-found`/`goal-roam fail` leftovers (3.4); refinery turret placement intermittent (3.5); corner at (-7461, -911) parked (3.15); base expansion and jeep-start plans (5.8b, 3.11a, 3.11b); ramp detection not part of the planner. |
| `emp_streetsoffire` | yes | **No flags.** MOVE test target `(888 3085 -142)` used by `dev_check`; navmesh trap near (-4245 -5424) open (5.3); first-map quota test (`max_bots 4`). |
| `emp_cyclopean` | yes | Part of the default sweep list; fords not measured. |
| `emp_dmz_b1` | yes (18,101 areas) | Gates open by player count (thresholds 16/20/28/32); closed-gate mesh has 44 components. Turret danger circle used to cut the only corridor (now very expensive instead of forbidden). Flag rock `northflag` has no upward link (needs a crouch-jump link of about 52-56 HU; 5.16). Under 20 players only one flag free. |
| `emp_slaughtered` | yes | Flag 130 has no ground point; nearest nav area is 743 below the origin (skipped; access unclear, 5.17). CV stalled at the radar in `20261115-175843` (fixed detour). |
| `emp_mvalley` | **no** | Generation hit the 20-minute timeout; retry with a larger timeout (`dev_navgen.sh`, `-nowatchdog`). |
| `con_district402`, `con_glycencity` | yes | Conquest: PASS 0 crashes (`20261122-130618`, `20261122-130917`). Round end (tickets 0, sudden death) untested. |
| `con_eastborough`, `emp_arid`, `emp_bush`, `emp_canyon`, `emp_chain`, `emp_duststorm`, `emp_isle`, `emp_midbridge`, `emp_money` | generated 22.11. | Started cleanly without crash/plan flood; not otherwise evaluated. |
| `as_escort` | yes | PASS `20261122-130632`; pillar tunnel (`func_movelinear` plus button) unsupported (3.9a). |
| Training maps (`emp_tutorial_*`, `emp_vehicletraining_*`, `mm_crossroads`) | open | Not generated/run yet. |
| `tg_*` generated test maps | - | Exported test maps (for example `tg_nfcv1_20261116`); NF CV entry failure was fixed in the map export by raising the vehicle origin. |

## Per-map issue checklist when starting a run

1. Does a navmesh exist and do `tools/check_nav.py` components cover spawns, buildings and resource points?
2. Does the map have flags (not `emp_streetsoffire`)? Is it Conquest or escort (different end-of-round rules)?
3. Is there a team refinery at start (needed by `dev_orders.sh`, `dev_turret.sh` default maps)?
4. Are there gates/doors that depend on player count (`emp_dmz_b1`) or build-time barricades (`emp_eng_map_model`)?
5. Water: depth along vehicle routes (Palmbay, Cyclopean); boats reachable only by ladder.

## Generating a navmesh

- In game: load the map, `sv_cheats 1; p_nav_generate` (up to a minute), then `sv_cheats 0`; the file goes to `empires/maps/<map>.nav`.
- On the test server: `scripts/dev_navgen.sh <map>` (2 bots as seeds, ends when the file is saved, `-nowatchdog` because `CreateNavAreasFromNodes` runs in one
  frame and exceeds the watchdog on big maps; copy the map's own files first, as listed in `maps/<map>.res`). `DEV_NAVGEN_PRE="cmd;cmd"` runs commands first.
- Generator specifics (`navmesh/nav_mesh/nav_generate.cpp`): floods from players, or without `info_player_*` from commanders/barracks/resource/cap points; seeds above every `emp_cap_point` (boat decks);
  water-bottom ladders search 400 HU and are not dropped as "dangling"; doors and sliding gates (`func_door*`, `prop_door*`, `func_movelinear`) count as passable (routing blocks them while shut, `navmesh-gate`).
  Improving unreachable areas and jump/climb edges is open (5.21).
- Map sweep: `scripts/dev_maps.sh [out] ["map:min:timescale ..."]` (default list: streetsoffire 1x, crossroads, cyclopean, palmbay, coast at timescale 2).

## Offline navmesh tools

Python 3 (+ Pillow); read `.nav`/`.bsp` only, never need the game; library in `tools/navlib/` (`navmesh.py`, `bsp.py`, `bsp_entities.py`, `static_props.py`, `terrain.py`).

| Tool | Use |
|---|---|
| `python3 -I tools/nav_overview.py <map>.nav --bsp <map>.bsp --terrain --props --vehicle-width 284 --log <run>/server.log --userid <uid> --crop x0,y0,x1,y1 --output <png>` | Top-down render: navmesh components, terrain slope (amber 30-45 deg, red > 45), static props, vehicle lane/bottlenecks (orange-pink), stuck marks and the driven track of one vehicle. Also `--size`, `--json`, `--marks`. **Look at the image first for spatial bugs.** |
| `python3 tools/check_nav.py <map>.nav [--bsp <map>.bsp]` | Area/component counts; spawn, commander, barracks, refinery, resource entities located on the mesh; exit 1 if a point is off the main component (default `--reach-hu 384`). |
| `python3 -I tools/test_nav_tools.py [maps_dir]` | Smoke test that displacement heights match the navmesh (skips without `emp_palmbay`). |

Screenshots of the game scene: `../rcbot2-clean/scripts/capture_empires_demo.py` (delete own demos afterwards). `empires_nav_scan` (in game) uses the player hull; an overview
does not prove vehicle clearance. Further map-specific measurements: `reverse-engineering/notes/maps.md` ([Reverse engineering](Reverse-Engineering.md)).
