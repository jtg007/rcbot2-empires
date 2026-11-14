# Open work

Work top to bottom inside each section. One item per step: baseline run, smallest fix,
same run again, compare. Tick an item only with measured numbers and the run folder.

## 1. Being measured

- [ ] **Match with production server values** (`emp_crossroads`, 10 bots, 20 min,
  production economy cvars, see `AGENTS.md`). Baseline: no vehicle in 20 minutes. Causes
  found: the commander could not get back into the CV when wedged against its hull (Use
  only fired within 200 of the CV origin); the CV drive goal lay inside a barracks
  footprint, the commander got out into a niche it could not leave; the commander left
  the CV to repair scratches (94–97 % HP) and the economy only runs in the seat. Fixes
  built and measured: hangs 232 → 10, research 61 at 324 s instead of 1034 s, both factories
  built. Vehicle purchases then failed silently: the game's `IsInFactory` traces up first, the
  buyer must stand under the factory roof (found with gdb). With that fix a bot bought and drove
  a light tank (quick run, 6 bots). Still open: one full match (10 bots, timescale 1).
- [ ] **Diagnostic logs off by default** (`rcbot_empires_debug_log`); check that
  `dev_check.sh` still passes and a run without the cvar stays quiet.
- [ ] **MOVE completion distance:** the bot reports `COMPLETE` at the end of its route,
  which can be up to 600 units off the order location; the game ends a MOVE only within
  100. The end distance is now logged (`commander-move result=… dist=`); measure with
  targets on the mesh, beside it and on a roof.

## 2. Needs a human player

- [ ] Hand the CV seat to a human commander; MOVE with cancel (cancel handling is built,
  untested); guard and attack orders from a human; "Roger"/"Negative" replies.
- [ ] Vehicle MG effect, scout binocular spotting, engineer behaviour under a human commander.

## 3. Known bugs waiting for a reproduction

- Bot stuck inside barracks after leaving the CV (one cause fixed above; others possible).
- MG turret placement at map refineries on `emp_rainbowroad` fails sometimes.
- Vehicle bail-out before a fall: rare false triggers; the Bill Nye jeep still falls off alone.
- Repair-pad entry blocked when the CV stands in it.
- A bot hangs at a map wall on `emp_rainbowroad` (-7461 -911); a parked vehicle is not
  blocked for infantry routes; an abandoned vehicle blocks the NF base for minutes.
- Navmesh islands: some callers of `ClosestPointOnMesh`/`GetNearestAreaId` can snap to an
  unreachable island.

## 4. Missing features

- Commander: more research (turret levels, chassis, armour) and better vehicle parts;
  give orders to bots; rebuild lost buildings, choose resource points by value.
- Engineer: turret upgrade, seismic grenades against turrets (measure throw range first),
  walls, cameras.
- Team: defend own refineries, react to base attacks, squad roles; squad passengers in
  vehicles (gunner seats), jeep raids on refineries, a turret line at the refinery.
- Keep the vehicle factory's build spot clear: a vehicle there blocks every purchase. Enemy
  vehicle → a grenadier is sent to destroy it; wreck → an engineer dismantles it.
- Maps: `emp_crossroads` base gates/ramps for vehicles, `as_escort` pillar tunnel.
- Fairness: vehicle and objective targets are picked from the full entity list; limit to
  what a player could know.
- Vehicle re-customizing on the repair pad on a server without debug purchase.

## 5. Coverage

- Generate navmeshes for the maps without one and run each map briefly.
- Conquest maps (`con_*`) and `as_escort` objectives for both teams; end of round
  (tickets 0, sudden death).
- A long run: 30+ minutes, 16–22 bots, with a map change.

## 6. Cleanup

- Remove old probe/fixture code from `bot_empires.cpp` in pieces (the current scripts use
  only `empires_test_commander_move`, `empires_commander_entry_once`,
  `empires_commander_place_barracks`, `empires_vehicle_scalehp`, `empires_test_botcmd`,
  `empires_nav_scan`), running `dev_check.sh` after each piece. Keep `m_commanderMoveShadow`.
- Split the huge functions (`tickEmpiresVehicle`, `getTasks`, `tickCommanderEconomy`)
  only when touching them.

## Questions for experienced Empires players

Answers help where logs and the binary cannot decide:
- A sensible research order after Structural Engineering and Vehicle Factory (turret
  levels, chassis, armour, engine)? How much money should a commander keep in reserve?
- Commander priorities when buildings are lost; good turret lines at refineries.
