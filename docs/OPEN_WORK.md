# Open work

Use a matching saved run for the baseline. Choose a short scenario for a specific mechanic or a 10-bot match at 1x for its interaction with the team. Reuse runs for related checks and close each item when its
remaining case has measured results and a run reference.
Check free space before long recordings and remove only recordings you created.
Prioritize by match impact and timebox each topic; see
[the contributor instructions](../AGENTS.md#working-with-an-ai-agent).

**Parked** (no measured gain in the default setting): vehicle physics fine-tuning
(handbrake steering, curve speed control, local maneuver selection, default 0;
obstacles passed in 1.6–1.9 s instead of 9–20 s, but open-road speed dropped, match
BE median 244 → 153 HU/s) and a scripted spectator camera for build-site screenshots.

## 0. Priority order

1. **Palmbay vehicles (3.27, 5.14a).** In a 10-bot 1x match (`20261122-124651`) bots climbed the boat ladders and started a capture, no ladder blockage lasted over 20 s, and no APC was lost in water. A spawn APC without a known support spot no longer parks in front of its own factory; it falls through to a push goal. Whether flank parking produces usable spawns is still open.
2. **Front support (3.10c, 5.26, 3.25, 5.30).** Engineers can answer a human infantry ammo request, infantry can seek navmesh cover under fire, and a seated commander can assign one attacking grenadier to an APC seen by a teammate. Observe actual refills, target engagement and resumption of the original objective. These changes have a Linux x86 build; gameplay verification is pending.
3. **Team roles (5.4, 5.8, 5.28).** In a 10-bot Coast match at 1x (`20261122-124709`) two bots ordered to push flag 168 stayed together and reached it. Review pressure on flags and the role of the last free infantry bot with players. A defender redeploying on a base alert remains unobserved.
4. **Lawliet (5.14b).** Forced Crossroads runs at 1x destroyed the enemy CV on the first rush in six runs (45–63 s of assault), and after fixing boarding aim/distance (`20261122-223256`) a forced 4-minute Crossroads run boarded cleanly (`aboard=3/3`, `cv-stunned`, BE CV HP 1.00 → 0.55). Run `021732` also recorded a crate 388 units from the CV. Open cases are boarding under the automatic match trigger (which previously timed out once in `20261122-213339`), a crew member actually replenishing grenades, and a Scout throwing again after an APC respawn. The driver seat is checked before departure and disconnected crew members release their assignments.
5. **Target knowledge (3.20).** Tank hunting, tank defense against APCs, commander target calls, and grenadier mortar targeting of APCs all require a sightline from the bot or a living teammate. Check in a match that a hidden APC near a friendly building becomes a defense or mortar target only once a teammate sees it.
6. **Next human session.** Verify the EngiBot purchase with SourceMod, commander orders and vehicle handover. Keep the Coast refinery stand spot (5.8e) parked until evidence identifies the obstruction.

## 1. Being measured

- [x] **Progress instead of fixed timers (`stall-escalate`):** each action checks locally whether it gets
  anywhere and escalates in steps (try differently, other goal, give up) with a `stall-escalate who=... reason=...`
  log line. Built: commander CV drive (25 s without net progress), vehicle buyers (skip unreachable route
  corners), engineers (20 s without health gain or movement: other stand spot, third stall gives the job back),
  infantry plans around vehicles. No false triggers in 4x test runs; the original fault cases did not recur yet.
  Open: tanks on `emp_palmbay` still stall in the palm cluster (2800-3300, 800-1550) on the way to the front
  (palms probed, route points pushed off trunks, about one give-up per 14-minute run remains). Built: a stuck
  driver first backs clear for 3 s and replans (`clear-before-exit`) and holds out longer in the factory bay;
  empty own vehicles blocking the bay or ramp are adopted after 5 s (`bay-occupied`, `20261120-224310`: heavy
  tank driven out in 11 s), and a bot-abandoned field vehicle is retried once by another bot after 10 s
  (`adopt-abandoned`). Crossroads 10 bots (`20261120-223253`): 4 heavy tanks, 0 give-ups.
  Tank fights: the fire line ignores invisible `emp_eng_restrict` brushes (`20261121-105931`: 0 false
  blocks, direct hits at 750-2380 HU), and a persistent `fire-blocked` under 1500 HU escalates (6 s flank,
  then skip the target for 15 s).

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
- [ ] **Commander stalls on a refinery turret job (1.6):** in 677 saved logs, all 407
  `LEFT_SEAT` events followed a logged building sortie by the same bot. In failed
  pad runs `20261115-004044` and `20261115-130138`, the commander placed a turret
  that stayed at 25/100 HP until the run ended, before a pad order. The pad check now
  disables refinery turret jobs for its own run. Refit took 71.8 game seconds in
  the passing baseline `20261118-091337` and 70.2 with the change in `20261118-091441`.
  All five regression checks passed, including the separate turret check at 100/100 HP.
  The stalled turret job during normal play still needs investigation if it recurs.
- [x] **Resource goal reachability (5.20/5.18):** a generated-map run logged 185
  `no-path` results for resource targets on navmesh islands. `navmesh-plan-failed`
  now logs start/goal areas and components, goal candidates are filtered by
  component reachability before selection, and an unreachable goal is dropped at
  once. In the 10-bot Crossroads match `20261120-122757` there were 0
  `navmesh-plan-failed` and 0 `no-path` results (baseline `20261120-120044`: 168
  and 55). The remaining Rainbowroad resource failures are not re-measured.
- [ ] **Tank combat (3.21) and vehicle goals (3.24):** tanks hold position and fire
  at static buildings from 550–1500 HU, the fire-line check ignores physboxes, panels
  and vehicle clip brushes, and side-on facing is used only against armed targets in
  direct combat. Vehicles pick base defence, front flags, enemy refinery/barracks/
  factory/radar, then the CV. In `20261120-122757`: 6 vehicles bought (baseline 5),
  no facing stalls, a 2-vs-1 tank fight, two cannon refills at an ammo crate.
  **Turret aim fixed:** the tank turret reads the command view angles relative to the
  hull, not as world angles; cannon and MG shots flew rotated by the hull yaw and almost
  all missed. The bot now rotates its aim into the hull frame. The fire-line check also
  ignores resource points (shells pass through them in the game). Short test
  (`scripts/dev_quick.sh`, 1x): 0 hits before, 8/8 after (refinery 150 -> 92). 10-bot
  Crossroads match `20261120-132819`: tanks destroyed 4 refineries (before: 0), tank duels
  now deal real damage, 0 bail-outs (before: 4). Commander build assignments no longer
  pull an engineer off a running vehicle repair call. Buildings and the CV are targets only
  when in sight and when no vehicle or infantry target is in range.
  **Goals by game flow:** defend (enemy tank within 3000 of the own CV or a refinery), hunt
  (enemy tank within 6000 seen by the tank or a teammate), support the front from a point
  with room for the vehicle 1200–1600 before a contested flag (never into it), push on enemy
  buildings only without a contested front, then the CV. Retreating without a repair station
  parks at a free, reachable spot next to the own CV (no building within 200). Buyers get a
  walk limit by distance (90–300 s) and the commander's order leash no longer pulls them back.
  Commander buildings no engineer could take after 6 tries no longer stall the build order.
  10-bot Coast match, 10 min, 1x: 0 tanks (`20261120-154142`, 15 min, stalled economy) ->
  4 tanks (`20261120-165710`), all goal kinds used, 5 retreats, 3 repairs, 0 give-ups,
  0 bail-outs. MG lead fixed 21.11.: the vehicle MG is hitscan (`Speed` in `vehicle_weapons.txt`
  applies to projectile types 1-4 only), so the old range/1000 lead missed moving infantry.
  Without lead (log line `vehicle mg-engage`), moving infantry at 500-900 HU took 1.5 -> 23.2 HP/s
  in two Crossroads 10-bot 1x matches. Palmbay `160030` recorded hunt, front and CV goals. Priority when an APC and an immediate CV threat coexist is still open.
- [ ] **Coast refineries without an engineer stand spot (5.8e):** the ground trace next to both
  Coast refineries starts solid (64/64); starting it higher finds ground, but then no kit aim
  reaches the building (64/64). Both stayed at 50/150 in those runs. Engineer ground and hull probes now ignore vehicle-only building proxies. The effect is unmeasured.
- [x] **Infantry against buildings and the CV (3.23):** grenadiers use RPG and mortar
  from a stand point outside the building box; in `20261120-122757` a grenadier
  destroyed an NF MG turret by mortar (100 → 61 → 22 → destroyed). Rifleman sticky
  charges (`weapon_emp_*_sticky_grenade`, ≤ 420 HU) and scout sabotage (`IN_USE`) are
  implemented; scout sabotage was confirmed live (`20261120-231458` on an enemy refinery),
  rifleman stickies once pressed fire before the grenade was in hand (no throw); the throw now
  waits for the grenade and was confirmed on an enemy refinery (damage alert right after).
- [ ] **Commander vehicle drive can get stuck:** in the production match an NF commander
  recovered on its second attempt. Identify the obstacle with a verified demo if it recurs.
  On `emp_slaughtered`, the CV stopped in front of the radar on the same approach at
  normal speed (`20261115-175843`) and at 4× (`20261115-175413`). The driver now keeps
  the detour around the radar and starts a reverse turn at low speed if the next point is
  behind the vehicle. With the same start and goal (`20261115-180657`), arrival took
  about 4 seconds at normal speed versus 8 before. At 4× it took 287 game ticks versus
  591 with only the self-vehicle exclusion (`20261115-180116`). Both new runs arrived
  after one planned reverse turn with no stall. All five checks in `dev_check` passed.
- [ ] **Refinery MG turrets fail silently** (BE 5/5, NF 1/2 in one match). The same spots worked
  in earlier runs. Turret allotment and engineer restrictions are possible causes.
  The game's `TextMsg` rejections use a different engine interface from the existing
  hook. Crossroads and Palmbay each reproduced a failed placement, then built the turret
  from a different spot and angle. Capture the reason for the failed attempt.
- [x] **Diagnostic logs off by default** (`rcbot_empires_debug_log`): two-bot runs
  `20261114-143708-emp_crossroads-2b-2m` and `20261114-143815-emp_crossroads-2b-2m`
  produced 23 and 90 `[RCBOT2]` lines with diagnostics explicitly off and on respectively;
  five regression checks passed with diagnostics on. Vehicle logging was not exercised.
  The user chose to keep diagnostics off by default for readable normal server logs;
  the test harness enables them, and manual runs can set the cvar to 1.
- [x] **MOVE completion distance (3.18).** Streetsoffire off-mesh orders ended 63 HU from a nearby edge target, 308 HU from a blocked ground target and 196 HU from its roof counterpart on the upper level. The bot reported completion without stalling.
- [ ] **Team director and bot commander orders (built, measuring, 5.4/5.8/5.28):** per team a small picture every
  1.5 s (phase push / base alert / stabilize, up to two push and two hold goals, alert target, tickets). Free infantry
  (no engineers, EngiBots, drivers, bots with a human order) is split ~50/30/20 push/hold/defense, at least one
  defender on a base alert; the seated bot commander gives each group an *attack location* order on its goal
  (`commander-sector`), bots stay within 450 of it. Without reachable flags push goals are the nearest enemy
  refinery / forward barracks and the free resource point nearest the middle of the map. Defenders spawn and walk
  to the threatened building. Tactical kill-spawn (`rcbot_empires_kill_spawn`) re-deploys a defender or a far
  vehicle buyer under strict rules (no combat 6 s, spawn 3800 closer, tickets > 20 and 25 %, 90 s per bot).
  Measured with 10 bots at 4x: palmbay kills 13 vs 20 against the cvar off (the first version was passive: 1 vs 41),
  coast flag captured by an ordered bot; open: kill-spawn for defenders never triggered yet, contact rate on coast
  (kills 15 vs 33). Roles now last at least 40 s and an alert at a hold goal is answered by the holders
  (palmbay 4x push<->defense swaps 16/15 -> 10/9 per match); open: a team with only one free infantry bot still
  sent it back and forth when alerts at a forward refinery kept returning (Palmbay 1x run `171039`: nine push/defense switches for BE userid 11); now an
  alert lasts 40 s after its last trigger and moves to another building only for a clearly stronger threat, and a
  bot stalled at a building forbids that mesh link for 60 s (palmbay 1x: swaps 11 -> 8, max per bot 9 -> 3, a 4 min
  stall at the own barracks -> 68 s). How the roles feel needs a human test.
- [x] **Water:** bots surface after 5 s with the head under water and avoid deep water when land is possible;
  vehicles ford only as deep as their body centre stays dry (the game stops the engine after 1 s and deals
  20 HP/s once the centre is wet). Palmbay boat flags are captured by swimming to the boat ladder, climbing
  onto the deck, and jumping back into the water afterwards (verified in 10-bot 1x match `20261122-124651`).
  Fords on `emp_cyclopean` are not measured yet.
- [ ] **Gates:** doors and sliding gates (`func_movelinear`) count as passable when generating, and routing
  blocks the links a gate covers while shut (`navmesh-gate`). On emp_dmz_b1 (paths open by player count) the
  `no-path` everywhere came from an enemy turret's danger circle being forbidden while the only open way ran
  through it; entering the circle is now very expensive instead of forbidden. Open: the DMZ flag rock has no
  upward link in the mesh (a crouch-jump link of about 52 would be needed). On emp_slaughtered the flag sits
  743 above the nearest mesh area and is skipped; how to get up there is open.

## 2. Needs a human player

- [ ] EngiBots (`/buildengibot`, community request): purchase, limit, leash, building help,
  base/pad vehicle repair (without repair calls and even with `rcbot_empires_vehicles 0`,
  `20261120-190522` 75 → 150 HP), guard return, physbox filtering, commander-econ exclusion,
  and map-change cleanup are implemented and pass `dev_engibot.sh` and `dev_check.sh`. Turret
  on an "attack location" order: placed on the second try and built 100/100 in 3.9 s (bot
  commander). Console command `buildengibot` as a fallback for `/` chat under SourceMod.
  First human test (15.11.) issues are fixed and the SourceMod crash fix moves the purchase
  to the next server frame; retest with a human player is open.

- [ ] Orders from a human commander (community test: guard/stop ignored, bots ran off). Now testable without a
  human: `rcbotd empires_test_unit_order` makes the commander bot send `emp_unit_order_list` + `emp_unit_order`
  like the game UI. Measured: move, abort, attack location and guard (infantry follow the guarded unit, engineers
  build or repair the guarded building: barracks, radar, armory, repair pad, factory). New
  `rcbot_empires_engineer_help_human`: with a human commander up to 2 engineers per team build and repair near the
  CV on their own. Open: attack target (type 3) on an enemy cannot be given by a bot commander (the game only accepts targets
  that were transmitted to the commander's client); the bot side shares the guard code. With a test hook that marks
  the enemy like a commander view does, attack target is accepted and the bot follows the target; a guard on a
  damaged refinery was repaired in 10 s (`scripts/dev_orders.sh`). Open: a navmesh trap on emp_streetsoffire near
  (-4245 -5424), confirmation with a real human commander.
  `Bad SetLocalAngles(..., -nan)` about twice a minute: a NaN view angle is now zeroed before the user command
  and logged (`viewangles non-finite`), the source is still open.

- [ ] Hand the CV seat to a human commander; "Roger"/"Negative" replies.
- [ ] Vehicle MG effect, scout binocular spotting, engineer behaviour under a human commander.

## 3. Driving and intermittent bugs

- [x] **CV arrival and reverse turns (Palmbay):** the CV now slows to about 250 u/s in the last
  800 units before its goal (it used to arrive at 520-570 u/s and slide down the slope behind the NF
  build site), and reverse turns no longer restart while a vehicle still rolls backwards or keep
  throttling above 250 u/s backwards (a CV backed off a cliff near its base that way). Palmbay 4x:
  three of three site drives arrived on the plateau. Open: one CV wedged its nose under a static
  structure beside the NF refinery and could not back out (once in about 70 site drives); routes do
  not keep clearance from static props for a 284-wide vehicle. Confirm in a 10-bot 1x match.
- Compare normal steering and short handbrake inputs on open ground at matched entry
  speeds. Measure yaw change, lateral speed, space used and time to stable forward travel.
  The first two modes of `scripts/dev_turn.sh <1|2|3|4> [map] [nf|be]` sample the same steering input
  with and without a 0.2-second handbrake pulse after an in-place debug purchase.
  On Crossroads at normal speed, two pairs per chassis ran to completion on November 19.
  The NF Light used 875/833 HU and 852/859 HU of lateral space after two seconds.
  The BE AFV used 779/760 HU and 773/779 HU. Each pair lists steering alone first.
  Neither chassis consistently used less space with the pulse. Starting positions
  and entry speeds varied. Fixed starts and tighter speed matching remain open.
- Trial an anticipatory speed cap on regular NF Light and BE AFV routes.
  Straight braking from around 500 HU/s reduced speed below 80 HU/s in about
  1.0–1.1 seconds on Crossroads. The experimental `rcbot_empires_vehicle_curve_speed 1`
  setting uses up to three route corners to estimate the speed cap, with no
  added collision traces. Four corridor runs completed without a logged reverse
  recovery. A fixed-start comparison with measured clearance is still open.
  With six NF Light drivers on one server, two 12-second windows per setting
  averaged about 1.12 ms per tick off and 1.13 ms on, measured as server process
  CPU time. The driving workload varied between windows.
  Use `scripts/dev_vf_drive.sh [0|1] [map] [nf|be] ["x y z"]` for a short factory-first
  comparison with a regular purchase and 25 seconds of driving after the first route.
  All four Crossroads factory runs completed with no logged recoveries.
  Across 13 route samples per run, median speed with the cap off/on was
  783/642 HU/s for the NF Light and 784/567 HU/s for the BE AFV.
  In a separate match, BE AFVs were much slower and logged more recoveries
  during the two enabled windows. Combat and changing routes limit the
  comparison. The setting defaults to 0 pending a fixed-start comparison.

- Trial local maneuver selection for NF Light and BE AFV on regular routes
  with `rcbot_empires_vehicle_maneuvers 1` (default 0). The driver scores
  forward and reverse arcs, sweeps three body sections for obstacles and
  checks ground support. It caps speed using the available stopping distance
  and replans stalled approaches without blocking a navmesh connection.
  Early enabled runs stopped on the vehicle's own turret and on map/world
  geometry because every sampled arc was rejected. The trace now uses the
  vehicle collision filter, and the regular steering resumes when no sampled
  maneuver is valid. Both updated runs completed without recoveries or give-up,
  but median sampled speed fell from 774 to 534 HU/s for the NF Light and from
  772 to 478 HU/s for the BE AFV. Route progress measured between 2-second
  samples also fell. Starts and routes differed, so this is not a controlled
  A/B comparison. No driving gain is established, and the default remains off.
  The next revision checks the regular steering arc first and keeps the route
  controller's steering and throttle when it is clear. Alternative arcs are
  scored for blocked approaches and recovery. In enabled runs `20261119-122700`
  (NF) and `-122929` (BE), the driver still retried after stalls. Review found
  that the special recovery path also intercepted stalls when no maneuver had
  been applied. It now uses the existing reverse recovery in that case and
  suspends maneuver control until the reverse window ends.
  The confirmation runs `20261119-123633`/`-123746` (NF) and
  `-123858`/`-124012` (BE) completed without retries, give-up or falls.
  Median speed off/on was 794/498 HU/s for the NF Light and 779/689 HU/s for
  the BE AFV. The summed two-second sample segments were 15621/11975 HU and
  16017/14199 HU. Each run had 13 samples. The routes ended at different
  positions, so the comparison does not establish a driving gain. `dev_check`
  passed 5/5. The default remains 0 pending a fixed-start comparison.
  Verify progress from a fixed approach, turning response on different
  surfaces and server CPU cost before enabling it by default.
  The next revision used the turn-test samples, which show yaw rates of about
  85 deg/s at 680 HU/s (NF Light) and 67 deg/s at 725 HU/s (BE AFV), so the
  planner assumed radii about three times too large. It now uses a
  per-chassis lateral acceleration (800/680 HU/s², about 80 % of measured)
  with a 0.3-second steering lag. The curve speed cap uses the same values.
  Each step now sweeps one world-axis box sized to the exact body width at
  any heading, instead of three slice boxes that widened the body by up to
  40 units on diagonals. Arcs that leave the route line are penalized. A
  shared per-tick budget limits evaluations, and `vehicle maneuver-perf`
  reports the cost. `scripts/dev_vehicle_obstacle.sh` provides the fixed
  obstacle comparison.
  First obstacle round: the planner never saw the prop because the shared
  `StandardFilterRules` (`navmesh/util/UtilTrace.cpp`) rejects every entity
  when the trace mask contains `CONTENTS_MONSTER`, as `MASK_SOLID` does.
  Centred NF Light runs gave up in both modes. Planner cost was 11–41 µs per
  evaluation. The planner now has its own entity filter and 24 units of body
  clearance, and treats walkable-slope hits (normal z ≥ 0.7) as ground.
  `vehicle maneuver-block` logs what blocks the regular arc. The shared filter
  issue does not affect infantry navmesh following, which uses its own trace
  filters. It does affect the Local Explorer fallback, which Empires rarely
  reaches, and navmesh generation. It stays unchanged there to avoid baking
  props and brush entities into generated meshes.
  Later rounds: the planner now brakes when no arc is usable, penalizes
  blocked arcs, follows the route to its next corner, ignores kerbs up to
  48 units, treats `emp_resource_point` and entities it overlaps while moving
  as passable, and closes every link through a built engineer barricade
  for 60 seconds. With selection on, every obstacle variant was passed, offset
  ones in about 2 seconds without stopping, and NF factory drives matched the
  speed with selection off. Other maps and full matches remain to be tested; the default stays 0.
  A committed detour now picks one side of an entity on the route (smaller
  clear offset) and holds it, at a capped speed of about 520 HU/s. In the
  latest round, all four obstacle variants were passed in 1.6–1.9 seconds
  without stopping (9–20 seconds with selection off, two give-ups).
  Final check (10-bot matches at 1x, `dev_check` 5/5): on `emp_crossroads` the
  BE AFV median fell from 244 to 153 HU/s with selection on, stall recoveries
  dropped from 11 to 4, give-ups rose from 2 to 3, and planner cost peaked at
  379 µs. One detour around a map brush timed out and one NF Light gave up at
  the factory exit's vehicle physbox. On `emp_palmbay`, both BE obstacle runs
  passed, but the enabled BE factory drive missed its exit. NF factory
  placement failed (`NO_SPOT`) and the matches bought no vehicles. The default
  stays 0. Open: factory exit with selection on, the vehicle physbox at the
  exit, factory placement and vehicle purchases on `emp_palmbay`.
  Focused driving checks for the narrower selection passed on Crossroads and
  Palmbay. Crossroads VF medians with selection off/on were 768/728 HU/s for
  NF Light and 746/725 HU/s for the BE AFV (13 samples each). The Palmbay BE
  run with selection on reached the factory exit and averaged 561 HU/s; the
  off run timed out at the exit. Fixed-obstacle checks passed in 1.86 s for NF
  on Crossroads after the retry-release fix (the confirmation did not need a
  retry), 1.69 s for BE on Crossroads, and 2.05 s for BE on Palmbay. The
  confirmation runs had no stops. `dev_check` passed 5/5 with no crashes. A Sol 6.2 review
  identified a recovery risk that appeared in the 10-bot match. All three NF
  Light exits from the same Crossroads factory timed out, one with selection
  off and two with selection on. The straight exit runs before maneuver selection.
  The exit's ground and world traces reported clear while the vehicle stood still
  at full throttle. The two enabled drivers then gave up at the factory or its
  physbox. One committed a new detour at 71 HU during recovery, released it on
  the next retry, and later got stuck on foot at its abandoned tank nine times.
  The fixed Crossroads comparison reproduced the stall with the radar alone.
  At yaw 0°, the NF tank timed out in 20.6 s with 1812 HU remaining, and the
  sweep hit the radar's vehicle collision proxy. With the updated heading
  selector, the same site with the radar present used 90° and cleared in 1.8 s
  with 15 HU remaining. The BE control used −96° and cleared in 2.6 s with
  38 HU remaining. Both drove their routes and `dev_check` passed 5/5.
  An occupied bay (3.17) is now cleared by adopting the empty vehicle (see
  the stall-escalate item); recovering wrecks abandoned twice is still open. The default is 0.

- [ ] **Palmbay factory placement (5.8c):** `FindNearFactorySite` (100-unit ground grid,
  radius 3600, reachable + exit lane) now reserves a free base factory spot before the
  first building, and other buildings keep 1500 HU off it (Palmbay BE `(-7444 -9281)`,
  `20261120-235714`: BE factory built first in base, 6 tanks in 8 min, 0 give-ups, 7
  `navmesh-stuck`). As fallback (`near clear=0`, Palmbay NF), `FindRemoteFactorySite`
  scans 11500 HU radius / 2400 HU depth with cluster fallback ≥ 1 and navmesh-snapped CV
  goal (`20261120-225803`: both teams built VF + forward barracks, 7 heavy tanks, 0
  `walk-timeout`). Open: spawning at forward barracks (5.24) and sporadic NF commander
  CV entry stalls. A map-built factory with an unexpected team number is found by class
  name (5.22, confirmed on a test map with built-in factories).

- [x] **Crossroads navigation around large props (3.4g):** a stall at prop 185
  closed 398 edges for 60 s, and `prop_dynamic` 180 closed 2244 links twice in a
  10-bot match. Box blocking is now limited to plausible entity sizes (doors up to
  600 HU, `prop_dynamic` up to 256, others up to 450), skipped when the bot stands
  inside the box (only the one edge is blocked), and an escape route out of the
  block is kept. In `20261120-122757` the prop logged `links=0 box_skip=1` and
  there were no navmesh plan failures.

When the following cases recur, compare the relevant trace and route.

- Bot stuck inside barracks after leaving the CV (one cause fixed above; others possible).
- On Palmbay, ten hull traces hit the same NF barracks before the first
  commander timed out on foot. A replacement entered the same command vehicle.
- MG turret placement sometimes fails on Rainbowroad, Crossroads and Palmbay.
  In the latter two matches a retry from a different position built the turret
  to full health. Capture the rejection at placement.
- Three BE respawn attempts reported a timeout. All three bots later resumed
  tasks, consistent with the existing late-respawn recovery. Revisit if a bot
  remains idle after its next spawn opportunity.
- Vehicle bail-out before a fall: rare false triggers; the Bill Nye jeep still falls off alone.
- Repair-pad entry blocked when the CV stands in it.
- A bot hangs at a map wall on `emp_rainbowroad` (-7461 -911); a parked vehicle is not
  blocked for infantry routes; an abandoned vehicle blocks the NF base for minutes.
- Navmesh islands (3.4d): refinery turret jobs check the route endpoint before claiming a stand. Rainbowroad run `122756` recorded five path failures for the same stand; follow-up `150857` recorded none. Revive and door-button claims now check the route endpoint against their interaction range. Vehicles require a full route for parking beside the CV and wait locally if all parking spots fail. These three changes await gameplay verification. `GetNearestAreaId` supplies telemetry keys and helps select an edge on an existing route.

## 4. Missing features

- Commander research pause/resume (measured): the research tick used to sit behind the "wait for
  unfinished buildings" and "check last emp_build" early returns, so it ran only once in a while
  (one start, then never again for minutes). It now runs before them. With the pool emptied during
  Structural Engineering the bot saw the pause (remaining time frozen), sent
  `emp_comm_research_resume` once money returned, finished item 60 and started 61.
  With the research multiplier at 1, an existing Crossroads match built both factories
  and each team bought three vehicles. Observe a destroyed radar and new research
  choices during later economy work.
- Commander research & vehicle tiers (5.1, 5.2): post-VF research path (`NextResearch()`,
  unit-tested: MG/ML turret L2 → Mechanical Engineering (27) → APC spawn (39) →
  Upgraded/Medium/Composite/Gas Turbine → L3/Heavy) and radar-loss reset are implemented.
  Live-verified without cheat research (`20261120-233436`, `emp_sv_research_complete 0`:
  `60 → 61 → 62 → 64 → 27 → 39 → 28` all started and completed cleanly). Dynamic vehicle
  part selection by research and budget (`bot_empires_vehicle_loadout.h`, unit-tested, live
  check `20261120-193737`: NF/BE Heavy Tanks with Composite Armor, Gas Turbine and Medium
  Chain Gun) is wired into purchase and pad refit. Open: confirm in a full normal-speed
  match; rebuild lost buildings and choose resource points by value.
- Commander CV survival & base defense (5.23): the commander pulls the CV back when an
  alive enemy vehicle closes within 2200 HU or under heavy fire, preferring a finished own
  repair station or open navmesh away from the threat (live test `20261121-002633`: NF CV
  retreated 1800 HU sideways from a BE heavy tank at `threat_dist=1610`). Built: a general
  forward barracks beside an on-foot teammate far from every own barracks (`20261121-114259`)
  and up to two MG and two ML base turrets after the repair pad (`20261121-121836`). Open: walls.
- Infantry combat, classes & barracks spawn (3.25, 5.25, 5.24): weapon ranges in
  `bot_weapons.cpp` are tuned to infantry roles, `changeskill` and class loadout variants
  (including rifleman stickies and scout sniper/shotgun) are active for all four classes,
  and spawn selection breaks ties among non-flag spawns by 2D distance to the front/VF.
  Navmesh cover is implemented. Skirmish movement (`bot_empires_skirmish.cpp`): infantry side-steps
  away from the enemy's line of fire within an effective weapon band (max. 900 HU, advances when farther),
  backs off when too close, reloading, below 35 % HP or outnumbered, ducks into cover out of sight,
  free bots within 2000 HU join a teammate's fight (max. 2 helpers) and a throttled spot callout is sent
  (cvars `rcbot_empires_skirmish`, `_fight_odds`, `_fight_cover`, `_join_fight`, `_callouts`, all default 1).
  Two paired 10-bot Crossroads matches at 1x (all on vs. all off, `133220`/`133250`, `134952`/`135032`):
  no new stalls, no crashes, kills 28 vs. 24. Open: grenades against groups or cover (no frag grenade in
  the loadouts), class roles in a fight, effect with human players. Recovery of the previous objective,
  class spacing, Scout hide and Grenadier road mines remain open.
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
  120 → 0; a richer bot stuck in a commander stage no longer blocks the purchase).
  In a 25-minute match with both pools reduced at the start, 17 vehicles were driven
  and four research items started without pauses. The reserve blocked no purchases.
  Compare resource use with later research and more expensive vehicles.
  Wages on request (built): when a human writes something containing "wage" in chat, every bot of
  his team hands over its wages, through the server's give plugin if `sm_give` exists (`!give
  #<userid>`), otherwise directly. Detected through the game event `player_say` (`say` is a
  game-DLL command and never reaches the plugin's client-command hook). Tested with
  `rcbotd empires_test_wages <userid> <wages>` (a bot really says "wages"); the `!give` path and
  the human's HUD are untested.
- Engineer heal (built, seen in a match): an engineer out of combat heals a visible hurt teammate
  within 700 units with the kit; the hurt bot gives up its route and waits up to 8 s
  (`scripts/dev_heal.sh`). Pitfall: the navmesh follower rewrites the move target every server frame,
  so `stopMoving()` alone never holds a bot with an active route - release the route first.
- Engineer seismic grenades (built, not yet triggered in a match): lobbed at enemy buildings up to
  1400 units to a landing point in front of the building's `emp_building_vehicle_physbox` (it bounces
  grenades back); turrets > camera/radar > large buildings > walls; 20 damage per hit on a barracks;
  projectile gravity measured ~320 (not 800), so a standing throw reaches ~1750 flat
  (`scripts/dev_seismic.sh`; numbers in `reverse-engineering/notes/engineer.md`). Open: a run-up throw at
  a firing turret from outside its 2000-unit range (the turret retreat keeps engineers out today),
  map barricades, damage on turrets/cameras, walls and cameras as builds.
  Turret upgrade (5.6) is done: an engineer with the upgrade skill raises its own finished
  turrets 1 -> 2 -> 3 with `emp_eng_upgrade` (only up to the map's turret restriction) and rebuilds them
  (`DEV_TURRET_UPGRADE=1 scripts/dev_turret.sh`); whether engineers reach rank 30 in a match is open.
- Team: defend own refineries, react to base attacks, squad roles; squad passengers in
  vehicles (gunner seats), jeep raids on refineries, a turret line at the refinery.
- Keep the vehicle factory's build spot clear: a vehicle there blocks every purchase. Enemy
  vehicle → a grenadier is sent to destroy it; wreck → an engineer dismantles it.
- Base layout on `emp_rainbowroad`: a forward barracks in front of the refinery (seen from the
  spawn, towards the front), later at least two repair pads there, and more vehicle factories
  before the first ramp from the spawn as the match goes on. First inspect route geometry and
  available space; ramp detection is not yet part of the planner.
- Infantry against vehicles (user 15.11.): only Grenadiers target vehicle crews now (rifles did nothing).
  Rifleman sticky grenades at vehicles never left the hand (the throw step read the wrong health field) - fixed.
  Scouts walk up to a visible enemy vehicle (<= 1200) with the sticky stun grenade and throw from 250; in short 1x
  runs a driving tank outran the scout. Outside throw range, Scouts now drop chases above 150 HU/s. Verify stun and sticky hits at 1x.
- Maps: `emp_crossroads` base gates/ramps for vehicles, `as_escort` pillar tunnel.
- Vehicle weapons: done for the active fire group. The bot reads the turret's weapon IDs,
  modes, groups, magazine and reserve every 0.25 s; hitscan MG only against infantry (up to 1200;
  never the cannon, no infantry target without MG ammo), no fire while the MG reloads;
  cannon/missiles against vehicles and buildings. Run `20261114-150322-emp_crossroads-10b-12m`
  (10 bots, real time): MG magazine 60→0 and reserve 4→0 under IN_ATTACK, then IN_ATTACK2 and
  cannon 40→36; one armour target used the cannon. Still open: switching fire groups,
  adding a missile launcher to the bots' loadout, and measured damage. Weapons must match slot
  type and size per chassis (light tank/AFV take only size-1 weapons; slot 2 is the missile slot).
  If the MG shares its button with another weapon (a loadout the bot did not buy), the bot holds
  fire at infantry (`why=mg-mixed`). Built for all tank chassis (Light/AFV, Medium, Heavy): the loadout is read every 3 s; a
  mixed one sends the tank to the own finished repair pad (`vehicle retreat reason=loadout`), which
  refits the bot loadout and logs `vehicle loadout-check` (measured on Light/AFV: `12:a2,0:a2` before, MG alone
  on IN_ATTACK after). Without a pad the trip gives up after 30 s and waits 120 s. Test cvar
  `rcbot_empires_vehicle_debug_mixed 1` buys such a loadout. Medium/Heavy refit awaits match verification.
  Vehicles have multiple weapon slots assignable to primary or secondary fire; account
  for the actual loadout and groups instead of assuming a fixed slot or button. Requested
  setup: group 1 cannon primary and missile secondary for buildings and vehicles; group 2
  cannon primary and MG secondary for infantry, with cannon fallback only after the MG and
  its reserve are empty, where those weapons are fitted.
  Run `20261114-144912-emp_crossroads-6b-25m` read one BE vehicle's weapon slots, group,
  magazines and reserve; actual shots and group/button behavior remain to be compared.
  The diagnostic build passed all five regression checks.
- Taking empty vehicles (measured): an empty own vehicle is eligible within 1500 of the
  team's CV or a building after 30 s without a driver or movement. The passenger-handle
  reader recognizes the game's DataTable array. The empty timer starts on observation and restarts after movement over 100 units. In the factory bay, a human teammate within 250 units has priority even after 30 s. Other fake clients do not reserve the vehicle. A human handover and taking the vehicle
  again after leaving the passenger seat still need a player check.
- Vehicle ammunition: step 1 done for light tank/AFV (empty cannon/missiles -> own pad, the refit refills every
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
   other chassis and reliable simultaneous calls.
- Two simultaneous vehicle ammo calls can still fail. In Crossroads run
  `20261119-182605`, one engineer got stuck on the way and the other failed to
  place a crate after four attempts. One tank refilled at the pad. With an
  earlier call (`20261119-183356`), an engineer again failed to place a crate
  and one driver gave up after four reverses. The single-crate and pad checks passed.
- Infantry ammunition: verify and complete ammo requests, engineer crate placement and
  resupply for every class; an engineer can now respond to a human infantry caller. Actual ammunition gain and autonomous requests from ordinary infantry bots remain open.
- Test-server inventory: 21 of 22 multiplayer maps have a navmesh and ran briefly without a crash (22.11.). `con_district402`, `con_glycencity` and `as_escort` loaded and ran without a crash or stuck bot. Still open: `emp_mvalley` (navmesh generation hit the 20-minute timeout) and the training maps.
- Fairness: vehicle and objective targets are picked from the full entity list; limit to
  what a player could know.
- Full pad refit (5.10) is covered by the `dev_check` pad test; bot-bought loadouts never trigger
  it in matches (0 of 25), where the pad only repairs with the same loadout.

## 5. Coverage

- Generate the `emp_mvalley` navmesh with a longer timeout.
- End of round on Conquest and escort maps (tickets 0, sudden death).
- Long run done (22.11., `20261122-131813`): 18 bots, 32 minutes at 1x on Crossroads, then `changelevel emp_coast`, 0 crashes. Quota bots (`min_bots`/`max_bots`) that join after a map change now get a class and spawn.
- Generated maps: the NF command vehicle entry failure was fixed in the map export by
  raising the NF vehicle origin. With a 90-unit height, both commanders entered and
  the run recorded zero path errors (`tg_nfcv1_20261116`). Revisit bot-side entry
  recovery if a correctly exported map still reproduces the failure.

## 6. Cleanup

- Clarify file open warnings when touching file loading. The profile and waypoint
  directories exist, and the specific files were absent. Navmeshes loaded and all
  four health checks passed despite the map material warnings.
- Split the huge functions (`tickEmpiresVehicle`, `getTasks`, `tickCommanderEconomy`)
  only when touching them.

## Questions for experienced Empires players

Answers help where logs and the binary cannot decide:
- A sensible research order after Structural Engineering and Vehicle Factory (turret
  levels, chassis, armour, engine)? How much money should a commander keep in reserve?
- Commander priorities when buildings are lost; good turret lines at refineries.

Navigation diagnosis: compare spatial navmesh coverage and connected components with the matching map when a suitable external viewer is available; verify vehicle clearance and dynamic obstacles with in-game traces.
