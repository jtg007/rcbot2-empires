# Roadmap

Source: the project's work list (stable item IDs, also used in commits and logs); [docs/OPEN_WORK.md](../docs/OPEN_WORK.md) has the public
priority order and details for the main open items. Snapshot 2026-11-23. Prioritization rule: what a player notices in a 10-bot 1x match first; park a topic with a one-sentence
finding after about one session without measurable gain.

## Now / next

| Item | Task | Success looks like |
|---|---|---|
| Infantry with humans (3.25) | Watch the new skirmish, join-fight and callout behaviour with real players; next steps of the infantry plan below | Players see bots fight together; no complaints about constant callouts |
| Next player session (5.3 / 3.10c / 5.26 / 3.8 / 7.1) | Human commander (move/guard/attack/cancel/CV handover), ammo call to an engineer, vehicle takeover with a human next to the bay, EngiBots (`/buildengibot`) | Playability with real players confirmed |
| Repair pad jitter (3.12) | Tanks occasionally jitter in front of the pad for minutes without stuck detection (1 of 8 regression runs) | Jitter without progress is detected and escalated |
| Map coverage (5.11) | Generate `emp_mvalley` navmesh with a larger timeout (optional); training maps | 22 of 22 maps start cleanly |
| Side observations during runs | Revive/door button reach (3.4d), APC sight check incl. mortar (3.20), CV threat vs. APC (3.24), medium/heavy ammo (3.10b), target call (5.30), natural Lawliet boarding (5.14b) | Items closed with a run reference |

Last accepted state (2026-11-23): `dev_check` PASS (unit 34/34 + 5/5); 15-minute Crossroads match `20261123-122645`: 0 crashes, 0.24 % stuck
(vs. 1.0 %), 17 kills (vs. 12), 6 vehicles with 0 `vehicle give-up`, `detour no-exit` 48 to 7, building stalls 11 to 1. Infantry combat on top:
two paired matches (all on vs. all off) with no new stalls, kills 28 vs. 24.

## Gameplay flow (built, awaiting match verification)

| ID | Topic | Open part |
|---|---|---|
| 5.8 | Sector roles and base alert | Judge role split with only one free infantry bot and with players. |
| 5.28 | Tactical kill-spawn | Proven for VF buyers (`154513`, `161706`); watch for an actually redeployed defender on a base alert. |
| 3.24 | Vehicle goals (defend, hunt, front support) | CV-threat (< 1500 HU) override over APCs: match verification. |
| 5.14b | Lawliet | Natural auto trigger with clean boarding, real grenade resupply, scout re-throw after APC respawn. |
| 3.10c / 5.26 | Infantry ammunition | Crate, refill of all classes, return to task; autonomous bot requests. |
| 3.25 | Infantry combat | Return to objective, class spacing, penalties/ducking, grenades vs. groups/cover; see infantry plan below. |
| 5.30 | Commander target call | Target switch and return to task; snipers, ninjas, radar/camera reports (3.20). |
| 3.22 | Infantry vs. vehicles | Sticky hits and stun effect at 1x. |
| Engineer revive | Disconnect release, 6 s replan, 10 s wait limit (`e1c3db8`-`5a44372`) | Observe with the next fitting acceptance (island check 3.4d). |
| 3.20 | Target knowledge (sightline rules) | Hidden APC near own building becomes a target only once a teammate sees it. |

## Expansion by player impact

| ID | Topic |
|---|---|
| 5.31 | Complete the classes (user wish: mainly Engineer). Order: 1 heal (done, verified in match) -> 2 seismic (built; open: run-up throw at firing turrets from outside their ~2000 range, map barricades `emp_eng_map_model`, damage on turrets/cameras, match acceptance) -> 3 ML turret (kit item 2) at refineries against vehicles -> 4 cameras/radar (items 4/5), walls (item 6), finish-repair after attacks (5.9) -> 5 Scout hide skill and sniper positions, Grenadier anti-tank positions and road mines (5.25) -> 6 couple a human medic call to the healing engineer (5.26) |
| 5.5 / 5.5a | Front infrastructure (forward repair pad and an additional front barracks for redundancy); team economy (buyer choice, wages, late research) when a concrete purchase failed |
| 3.10d / 3.10b | Mixed loadout refit on the pad and vehicle ammo for Medium/Heavy; two simultaneous ammo calls (`DEV_AMMO_TANKS=2`) |
| 3.10f | Fire-group switching and missile in the loadout (user 14.11.): group 1 cannon/missile vs. buildings and vehicles, group 2 cannon/MG vs. infantry |
| 5.25 | Class depth: Scout hide + sniper positioning, Grenadier anti-tank + road mines |
| 5.8a / 5.8b | Clear the factory build spot (destroy enemy vehicle with a grenadier, dismantle wrecks); rainbowroad base expansion (forward barracks, pads, factories) |
| 3.11 / 3.11a / 3.11b | Squad passengers in vehicles; rainbowroad jeep start; defend the rainbowroad refinery with about four ML turrets plus an MG turret in a line |
| 5.7 / 5.9 / 5.26 | Seismic vs. turrets; cameras/radar/walls/hardpoint repair; squad heal/hide/artillery/charge and squad targets on the navmesh |

## Sporadic bugs (reopen on recurrence)

| ID | Symptom |
|---|---|
| 1.6 | Commander returns to the CV too late after a refinery turret (turret left at 25/100 HP) |
| 1.5a / 3.5 / 5.8d | Refinery turret fails silently / first rainbowroad attempt fails / sporadic refinery rejection on Palmbay (`REJECTED type=0`) |
| 1.5b | Commander CV stops on the way to the factory (`drive-stuck`, second attempt worked) |
| 3.2 / 3.2b | Bot inside barracks hull after leaving the CV (`start_solid=1`); turret job hangs after a map change |
| 3.4 / 3.4d | Rainbowroad `start-area-not-found` and `goal-roam fail`; navmesh islands (revive on separate area, door button with offset route end, retreat without reachable spot) |
| 3.16 | Vehicle as obstacle for infantry (soft 15 s penalty built; recheck in long match) |
| 3.17 | Abandoned vehicle stays as obstacle (adoption built; wedged wrecks open via 5.8a) |
| 3.12a / 3.6a / 3.6b | Bail-out false trigger (`143031`), bail-out timing per chassis, Bill Nye falls off alone (4 falls per run on rainbowroad) |
| 3.9 | Crossroads base ramps and gates |

## Human-session checks

| ID | Check |
|---|---|
| 7.1 | EngiBots with SourceMod: chat/console purchase, limit, leash, turrets on order, pad repair, enemy contact (base repair proven in `190522`) |
| 5.3 | Human commander orders: Guard (engineers and others), Attack Location/Target, MOVE cancel, "Roger"/"Negative" replies; navmesh trap `emp_streetsoffire (-4245 -5424)`; `Bad SetLocalAngles(-nan)` |
| 3.10 | MG effect of vehicles, seat handover to a human commander, ammo crate swap, scout binocular spotting, engineer under a human commander |
| 3.8 | Vehicle takeover: human within 250 HU keeps priority in the bay; timer starts at first empty sighting and restarts after movement > 100 HU |
| 3.4e | Re-entering after a passenger exit |

## Maps, modes, long runs

5.11 map sweep (21/22 done), 5.12 game modes (Conquest/escort PASS; round end untested), 5.21 better navmesh generation (unreachable areas, jump/climb edges),
5.16 `emp_dmz_b1` flag rock link, 5.17 `emp_slaughtered` flag 130, 3.9a `as_escort` pillar tunnel. Details on [Maps](Maps.md).

## Cleanup, portability, upkeep

| ID | Topic |
|---|---|
| 6.7 | Attribute test-server file warnings (missing profiles/waypoints/material) when file loading is touched |
| 1.1 | Check free disk space before long recordings |
| 6.8 | Remove legacy waypoint remnants and old probes/fixtures (`bot_empires.cpp`, `bot_navmesh_poc.cpp` 5982 lines); unify remaining waypoint paths (squad, resupply, cover, approach) on the navmesh |
| 6.9 | Windows/update stability: replace fixed Linux offsets (`bot_empires_binary.cpp`) and function addresses (`bot_empires_native_placement.cpp`) with SendProps/datamaps, signature scans or Windows equivalents ([Windows](Windows.md)) |
| 6.10 | Local avoidance in `PathFollower::Avoid()` is disabled; test whether enabling reduces corner/prop hangs |
| 6.3 | Split giant functions (`tickEmpiresVehicle`, `getTasks`, `tickCommanderEconomy`) only when touched |
| 6.6 | Community FAQ (last; template in `docs/OPEN_WORK.md`) |

## Parked (no measured gain, reopen on evidence)

3.12 repair pad (Crossroads entrance timeouts), 3.15 rainbowroad corner (-7461, -911), 5.8c Palmbay VF (remaining CV cliff fall), driving physics fine-tuning (local maneuvers, curve speed,
handbrake; default 0), build-site camera (SourceTV/spectator), 5.8e Coast refineries, commander walling/recycling/artillery (dynamic walls can block own vehicles; artillery needs spotting and ballistics).

## Infantry plan (2026-11-23)

Goal: players should notice bots fighting together and deliberately, not running straight at them or standing around at long range. Yardstick: `emp_crossroads` 10 bots, 1x,
`emp_sv_research_complete 0`, 15 min. Before: `APPROACH_ENEMY` walked straight at the enemy until almost inside it; cover only after being hit, with fire stopped.
Model for movement rules: YaPB `Bot::attackMovement` (strafe side from the enemy's facing, 30 % side swap, re-decide every 0.3-0.8 s, back off under about 100 HU and while reloading).

Rules: every step has its own cvar; short targeted tests first (bots placed with `setpos`, `DEV_UNTIL`, <= 3 min), then a 15-minute match against a parallel
match with the feature off; compare new vs. old with cvar values `2`/`3` (NF only / BE only) and swapped sides; every escalation logs `stall-escalate`.

| Step | Content | State |
|---|---|---|
| 0 Skirmish movement | `rcbot_empires_skirmish`: effective distance per weapon (<= 900 HU, advance beyond), side-steps off the enemy's aim, retreat below the band / reloading / < 35 % HP, short crouched bursts from 500 HU, navmesh-snapped and hull-traced steps, 1 s grace on lost sight | Done, default on |
| 1 Fight together | `rcbot_empires_join_fight`: free infantry within 2000 HU walks to a teammate's fight (max 2 helpers per report). Group push with a gather point about 1500 HU before the goal is not built yet | Join fight done; gather point open |
| 2 Accept or break off | `rcbot_empires_fight_odds`: outnumbered (enemies >= allies + 2) gives ground while shooting, ahead presses in | Done, default on |
| 3 Use cover actively | `rcbot_empires_fight_cover`: reloading, < 35 % HP or outnumbered -> short move to a spot out of the enemy's sight, back once ready; keeps firing (generic hurt-hide schedule suppressed in a skirmish) | Done, default on |
| 4 Targeted grenades | Throw at >= 2 enemies within 250 HU or one who vanished behind cover | Open: bot loadouts have no frag grenade against infantry |
| 5 Callouts | `rcbot_empires_callouts`: spot quick command only for a group or an enemy within 800 HU, once per team per minute and per bot every 3 minutes. Medic call open (location text format of `emp_menu_command 31` unknown) | Spot callout done |
| 6 Classes in combat | Scout flanks and spots with binoculars (`rcbot_empires_scout_flank`, `_scout_binoculars`); Grenadier keeps distance: mortar from 900 HU, at least 400 HU from soldiers, RPG stand-off 550-1300 HU against vehicles (`_grenadier_rpg`); Engineer stays behind teammates (`_engineer_rear`) and heals | Done, default on (10-bot match check pending) |

Measured (two paired matches, all on vs. all off): kills 13/17 and 15/7, together 28 vs. 24; no new stalls, 0 crashes. Fixed during the work: the hull trace started inside
`emp_resource_point` volumes, so every step was rejected exactly where bots fight most.
