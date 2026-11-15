# Open work

Work top to bottom inside each section. One item per step: baseline run, smallest fix,
same run again, compare. Tick an item only with measured numbers and the run folder.
Historical plans do not supersede this list.
Check free space before long recordings and remove only recordings you created.

## 0. Priority order (natural game flow first, enablers before what builds on them)

1. Shared team economy (commander reserve, bots buy from wages first); its production-economy runs
   also measure research pauses. Research pause/resume is measured (see below).
2. Done (measured): bots take an empty own vehicle only within 1500 of their CV or a building and after
   30 s without a driver and without moving (`vehicle empty-skip` logs the rest). Found on the way: every
   `m_hPassengers` read failed (Empires sends handle arrays as a DataTable "000", "001", ...), so empty
   vehicles were never taken over before; fixed in the handle reader.
3. Refinery MG turrets failing silently; commander vehicle drive getting stuck on the way to the factory.
4. Mixed weapon loadouts: done for light tank/AFV (refit on the own pad, `scripts/dev_mixed.sh`); other
   chassis need their own slot, engine and armour values.
5. Ammunition: step 1 done for light tank/AFV (empty cannon/missiles -> own pad, the refit refills every
   weapon; `scripts/dev_ammo.sh`, test command `rcbotd empires_vehicle_setammo <magazine> <reserve>`).
   Step 2 done: a team ammo crate (engineer crate or the team's map ammo crate) nearer than the finished
   pad is used instead; the tank stops within 850 and waits until the crate gives nothing for 8 s
   (`scripts/dev_crate.sh`, test command `rcbotd empires_vehicle_crate <distance>`; measured: empty
   cannon and MG back to 40/0 and 60/4 after 21-22 s). Step 3 done: without a nearer crate and with
   the pad over 2500 away (or none) the tank calls "I need ammo" (`emp_menu_command 29`) and waits
   while an engineer holds the call; the engineer walks over and places a supply crate (`engSelect 7`,
   `scripts/dev_ammocall.sh`; measured: crate on the first try, tank full again after 22 s). Each vehicle has its own call (up to four per team); an engineer takes
   the nearest call no other engineer holds (measured with two empty tanks: one engineer each; `DEV_AMMO_TANKS=2`). A call holds only while its
   driver sits alive in the vehicle. Engineers often get stuck on the way (`walk-stuck`). Next:
   other chassis, then infantry resupply for every class.
6. Human-like tank combat instead of circling the target (cover, stop to fire, relocate, keep
   range, strongest armour forward, advance with infantry); measured A/B by damage dealt/taken.
7. Then team and commander behaviour (orders to bots, base defence, adapting to losses, better parts
   after research, fire-group switching with missiles, base expansion, squad passengers, APC as a
   forward spawn point after its research).
8. Sporadic navigation/robustness cases only when they recur; items needing a human player; maps,
   game modes and long runs; cleanup.

## 1. Being measured

- [x] **Match with production server values** (`emp_crossroads`, 10 bots, 20 min,
  production economy cvars, see `AGENTS.md`). Baseline: no vehicle in 20 minutes. Causes
  found: the commander could not get back into the CV when wedged against its hull (Use
  only fired within 200 of the CV origin); the CV drive goal lay inside a barracks
  footprint, the commander got out into a niche it could not leave; the commander left
  the CV to repair scratches (94–97 % HP) and the economy only runs in the seat. Fixes
  built and measured: hangs 232 → 10, research 61 at 324 s instead of 1034 s, both factories
  built. Vehicle purchases then failed silently: the game's `IsInFactory` traces up first, the
  buyer must stand under the factory roof (found with gdb). With that fix a bot bought and drove
  a light tank (quick run, 6 bots). Full match (10 bots, timescale 1, 20 min): BE bought and
  drove 3 vehicles, but NF built only one refinery: the bot chosen as commander kept starting
  refinery turrets (all failed) and lost its seat attempts. Fix built and measured in two
  15-min matches (in the second, both teams build radar and armory, NF also pad and factory); the
  five regression checks passed after the seat-reservation clock was corrected; that
  particular correction has not been tested with a human. A later pad test exposed an
  entry retry that could stall; the revised path passed the pad test, but the exact
  stalled situation has not recurred, so the fix is not yet demonstrated.
  Another match left the NF factory unfinished: the CV's building-avoidance search
  pulled its destination back to its starting point, outside the commander's walking
  range. The search now checks lateral destinations too. In a six-bot accelerated run
  NF completed its factory and a BE bot drove a purchased vehicle after 796 game seconds;
  the precise original configuration has not been repeated. An earlier regression run
  passed four checks; revive failed because the target was already alive, then passed
  on a separate retry.
  The revive harness now waits for the successful result rather than ending on an
  earlier unsuccessful attempt; a standalone run and all five regression checks passed,
  without reproducing the earlier premature-end situation.
  Final normal-speed match `20261114-141126-emp_crossroads-10b-20m`: both teams completed factories and each drove three purchased
  vehicles; one NF commander drive got stuck, then the next attempt reached the factory.
- [ ] **Commander vehicle drive can get stuck:** in the production match an NF commander
  recovered on its second attempt. Identify the obstacle with a verified demo if it recurs.
- [ ] **Refinery MG turrets fail silently** (BE 5/5, NF 1/2 in one match). The same spots worked
  in earlier runs, so it is a state, not the geometry; likely the turret allotment or the
  engineer restriction state. The game's `TextMsg` rejections use a different engine
  interface from the existing hook. Failure-state logging is in place but awaits a
  reproduced failure; a 10-bot accelerated run built two turrets successfully.
- [x] **Diagnostic logs off by default** (`rcbot_empires_debug_log`): two-bot runs
  `20261114-143708-emp_crossroads-2b-2m` and `20261114-143815-emp_crossroads-2b-2m`
  produced 23 and 90 `[RCBOT2]` lines with diagnostics explicitly off and on respectively;
  five regression checks passed with diagnostics on. Vehicle logging was not exercised.
  The user chose to keep diagnostics off by default for readable normal server logs;
  the test harness enables them, and manual runs can set the cvar to 1.
- [ ] **MOVE completion distance:** the bot reports `COMPLETE` at the end of its route,
  which can be up to 600 units off the order location; the game ends a MOVE only within
  100. The end distance is now logged (`commander-move result=… dist=`); measure with
  targets beside the mesh and on a roof. A long-move test ended 61 units from its on-mesh target.

## 2. Needs a human player

- [ ] EngiBots (`/buildengibot`, community request): purchase, limit, leash, building help and map-change cleanup
  are measured with a bot commander (`scripts/dev_engibot.sh`). Turret on an "attack location" order: placed on
  the second try and built 100/100 in 3.9 s (bot commander). Console command `buildengibot` as a fallback for the
  `/` chat under SourceMod (built, not measured). Open: repairing vehicles on the pad and a test with a human
  commander. A community server crashed on `!buildengibot` with SourceMod and humans;
  the purchase now runs in the next server frame instead of inside the chat event (not yet confirmed there).

- [ ] Orders from a human commander (community test: guard/stop ignored, bots ran off). Now testable without a
  human: `rcbotd empires_test_unit_order` makes the commander bot send `emp_unit_order_list` + `emp_unit_order`
  like the game UI. Measured: move, abort, attack location and guard (infantry follow the guarded unit, engineers
  build or repair the guarded building: barracks, radar, armory, repair pad, factory). New
  `rcbot_empires_engineer_help_human`: with a human commander up to 2 engineers per team build and repair near the
  CV on their own. Open: attack target (type 3) on an enemy cannot be given by a bot commander (the game only accepts targets
  that were transmitted to the commander's client); the bot side shares the guard code. Also open: refinery not tested, a navmesh
  trap on emp_streetsoffire near (-4245 -5424), confirmation with a real human commander.
  `Bad SetLocalAngles(..., -nan)` about twice a minute: a NaN view angle is now zeroed before the user command
  and logged (`viewangles non-finite`), the source is still open.

- [ ] Hand the CV seat to a human commander; "Roger"/"Negative" replies.
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

- Commander research pause/resume (measured): the research tick used to sit behind the "wait for
  unfinished buildings" and "check last emp_build" early returns, so it ran only once in a while
  (one start, then never again for minutes). It now runs before them. With the pool emptied during
  Structural Engineering the bot saw the pause (remaining time frozen), sent
  `emp_comm_research_resume` once money returned, finished item 60 and started 61. Open: real
  research duration without a speed-up, a destroyed radar.
- Commander: more research (turret levels, chassis, armour) and better vehicle parts;
  give orders to bots; rebuild lost buildings, choose resource points by value.
- Shared team economy: commander and bots should share one view of team resources, income
  and planned spending so money is used for a natural game flow. Static findings: vehicle
  purchases pay from the buyer's own wages first and only the remainder from the team pool,
  with no game-side reservation; wages come only from rank points (kill 5, vehicle kill 10,
  revive 5, repair 3 per 100 HP, squad bonus about +2 per 3 squad events), not over time;
  research costs cost-per-second × time. Plan: log wages/resources/cost per purchase, add a
  per-team state with a reserve for the commander's next build and remaining research, then
  let bots buy from the team pool only above that reserve, preferring the buyer with the most
  wages. Measure time to factory/first vehicle, saving stalls, research pauses and idle money
  with production values.
  Built: the commander publishes a reserve (next building, plus 240 while the
  factory research is missing) and keeps it while waiting for engineers; vehicle purchases that
  the buyer's wages do not cover only happen if the pool keeps that reserve
  (`vehicle skip reason=team-reserve`). A pad refit with the same loadout is free (the game
  charges only the difference to the vehicle's total cost; measured: pool unchanged, armor
  refilled), so it needs no reserve check. Wages are read from the player and
  the price is learned from the pool/wage difference (the game's `vehicle_purchase` event does not
  reach the plugin's hook). Measured: price 192–245 for a light tank, wages are spent first
  (15 → 0, pool −207); on crossroads early game the pool stays at ~3900–4900, so the reserve never
  blocked there; with the pool forced to 100 the bot waited and bought after money arrived.
  Buyer with the most wages first (built): a bot whose wages do not cover the price waits up to
  30 s (`vehicle skip reason=richer-buyer`) while a free teammate with at least 10 more wages that
  reached the purchase decision itself buys; measured in short buy runs (rich bot buys from wages
  120 → 0; a richer bot stuck in a commander stage no longer blocks the purchase). Open: refit
  price, a late or poor match.
  Wages on request (built): when a human writes something containing "wage" in chat, every bot of
  his team hands over its wages, through the server's give plugin if `sm_give` exists (`!give
  #<userid>`), otherwise directly. Detected through the game event `player_say` (`say` is a
  game-DLL command and never reaches the plugin's client-command hook). Tested with
  `rcbotd empires_test_wages <userid> <wages>` (a bot really says "wages"); the `!give` path and
  the human's HUD are untested.
- Engineer: turret upgrade, seismic grenades against turrets (measure throw range first),
  walls, cameras.
- Team: defend own refineries, react to base attacks, squad roles; squad passengers in
  vehicles (gunner seats), jeep raids on refineries, a turret line at the refinery.
- Keep the vehicle factory's build spot clear: a vehicle there blocks every purchase. Enemy
  vehicle → a grenadier is sent to destroy it; wreck → an engineer dismantles it.
- Base layout on `emp_rainbowroad`: a forward barracks in front of the refinery (seen from the
  spawn, towards the front), later at least two repair pads there, and more vehicle factories
  before the first ramp from the spawn as the match goes on. First inspect route geometry and
  available space; ramp detection is not yet part of the planner.
- Maps: `emp_crossroads` base gates/ramps for vehicles, `as_escort` pillar tunnel.
- Vehicle weapons: done for the active fire group. The bot reads the turret's weapon IDs,
  modes, groups, magazine and reserve every 0.25 s; MG only against infantry, cannon there only
  after every MG of the group has an empty magazine and reserve, no fire while the MG reloads;
  cannon/missiles against vehicles and buildings. Run `20261114-150322-emp_crossroads-10b-12m`
  (10 bots, real time): MG magazine 60→0 and reserve 4→0 under IN_ATTACK, then IN_ATTACK2 and
  cannon 40→36; one armour target used the cannon. Still open: switching fire groups,
  adding a missile launcher to the bots' loadout, and measured damage. Weapons must match slot
  type and size per chassis (light tank/AFV take only size-1 weapons; slot 2 is the missile slot).
  If the MG shares its button with another weapon (a loadout the bot did not buy), the bot holds
  fire at infantry (`why=mg-mixed`). Built for light tank/AFV: the loadout is read every 3 s; a
  mixed one sends the tank to the own finished repair pad (`vehicle retreat reason=loadout`), which
  refits the bot loadout and logs `vehicle loadout-check` (measured: `12:a2,0:a2` before, MG alone
  on IN_ATTACK after). Without a pad the trip gives up after 30 s and waits 120 s. Test cvar
  `rcbot_empires_vehicle_debug_mixed 1` buys such a loadout. Other chassis still open.
- Taking empty vehicles: bots currently take any empty own vehicle within 2500 units of
  themselves. Planned: only vehicles near the own base that have stood without a driver for a while,
  so a player who just stepped out keeps their vehicle.
  Vehicles have multiple weapon slots assignable to primary or secondary fire; account
  for the actual loadout and groups instead of assuming a fixed slot or button. Requested
  setup: group 1 cannon primary and missile secondary for buildings and vehicles; group 2
  cannon primary and MG secondary for infantry, with cannon fallback only after the MG and
  its reserve are empty, where those weapons are fitted.
  Run `20261114-144912-emp_crossroads-6b-25m` read one BE vehicle's weapon slots, group,
  magazines and reserve; actual shots and group/button behavior remain to be compared.
  The diagnostic build passed all five regression checks.
- Vehicle ammunition: detect depletion, seek a repair pad or engineer ammo crate when
  appropriate, and request ammo. The user confirms engineer crates refill vehicles;
  verify the bot interaction and repair-pad resupply in game.
- Infantry ammunition: verify and complete ammo requests, engineer crate placement and
  resupply for every class; the current normal resupply path covers engineers only and
  searches team map crates, not engineer-built crates.
- Test-server inventory: 24 BSP maps, 10 with navmeshes; three of those have no recorded
  run yet: `con_district402`, `con_glycencity`, `con_urbanchaos`.
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
