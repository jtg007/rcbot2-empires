# Match flow test bench (MATCH_FLOW)

A test bench for one team's match flow, not an AI script: it does not change what the bots do.
Reference match: 10 bots at 1× on `emp_crossroads` (no flags) and `emp_palmbay` (flags).
Later phases deliberately have no fixed minutes, only delays after the previous milestone and standing rules.

- Source: the `[RCBOT2] match-flow …` lines (`bot_empires_match_flow.cpp`, only with `rcbot_empires_debug_log 1`; the
  dev harness turns it on).
- Check: `scripts/match_flow.py <server.log>`, run at the end of `scripts/dev_team_match.sh` (also with
  `--summarize RUN_DIR`). Measurement only, no failing exit status, until the limits are calibrated on 1–2 real 1× runs.
- The limits live only in the table below; `match_flow.py` reads them from this file.

## Telemetry

`t` = game seconds since the first bot tick on the map (`event=start`), i.e. game time, not wall time.

| Line | When | Fields |
|---|---|---|
| `match-flow state` | every 30 s per team | `tickets`, `res` (team pool), `research` (`active`/`paused`/`idle`/`none` = no radar/`done`), `built=` and `raw=` per building kind, `rawlist=kind#ent:age/seconds_without_progress/accepted`, `vehicles` (alive, CV excluded), `phase` (team director) |
| `event=placed` | new building | `kind`, `ent`, `hp`, `initial=1` for buildings already standing at the start |
| `event=accepted` | an engineer (or the commander himself) took the build order | `wait` = seconds since placement |
| `event=build-start` | first health gain after placement | `wait` = seconds until construction started (includes the walk) |
| `event=finished` | `m_bFullyBuilt` or full health | `age` since placement, `max_idle` = longest time without health gain |
| `event=lost` | building gone | `built`, `started`, `accepted`, `age`, `max_idle` |
| `event=research-vf` | vehicle factory research done | `instant=1` with `emp_sv_research_complete 1` (dev default) |
| `event=vehicle-bought` / `vehicle-out` / `vehicle-front` / `vehicle-lost` | vehicle new / ≥ 1000 HU from where it appeared / at the front / destroyed | `after` or `age` in s since purchase; front = within 3000 HU of an enemy building or CV, or half way to the enemy CV |

Building kinds: `refinery`, `radar`, `factory` (vehicle factory, VF), `barracks`, `armory`, `repair` (repair pad).
Core buildings = refinery, radar, VF, barracks. Turrets and engineer deployables are not tracked.

## Milestones (per team)

| No | Milestone | Measured from |
|---|---|---|
| M1 | First refinery and radar placed by `start_econ_placed_by`, each finished ≤ `start_econ_build` after placement | match start |
| M2 | VF placed ≤ `vf_place_after_research` after the VF research is done | `research-vf`; with `instant=1` the first finished radar (research would be possible from then) |
| M3 | VF finished ≤ `vf_build` after placement | first finished VF |
| M4 | First vehicle bought and driven out of the VF (`vehicle-out`) ≤ `first_vehicle_after_vf` after the VF is finished | from the first 30 s line with `res ≥ min_vehicle_res` if the pool was smaller before |
| M5 | Repair pad and forward barracks (first barracks placed after the VF was finished) finished ≤ `pad_barracks_after_vf` after the VF | first finished VF |

## Standing rules (whole match)

| No | Rule |
|---|---|
| R1 | No core building stays unfinished longer than `core_idle_max` without build progress (`max_idle`; at match end the `rawlist`) |
| R2 | No build order stays unaccepted longer than `order_accept_max` (`accepted wait`; logs before the `accepted` event use `build-start`) |
| R3 | With a finished VF and `res ≥ min_vehicle_res`, no team is without a living vehicle longer than `no_vehicle_max` |
| R4 | No hoarding: `res > hoard_res` longer than `hoard_max` while research stands (`idle`/`paused`/`none`) or, with a finished VF, no vehicle is alive |
| R5 | Every bought vehicle reaches the front ≤ `vehicle_front_max` after purchase; vehicles destroyed before that are counted separately |

R3 and R4 come from the 30 s lines: duration = span from the first to the last line in which the condition held without a break.
Note: the dev harness default (`emp_sv_research_complete 1`, server resource multipliers) gives large pools quickly, so R4
mostly tests vehicle presence there; with natural research (`emp_sv_research_complete 0`) it also tests stalled research.

## Limits (seconds or resources)

| Key | Value | Note |
|---|---|---|
| `start_econ_placed_by` | 180 | minute 2–3, upper bound |
| `start_econ_build` | 60 | |
| `vf_place_after_research` | 60 | |
| `vf_build` | 90 | |
| `first_vehicle_after_vf` | 90 | |
| `min_vehicle_res` | 250 | light tank, `kDefaultVehicleCost` |
| `pad_barracks_after_vf` | 180 | 2–3 min, upper bound |
| `core_idle_max` | 90 | |
| `order_accept_max` | 30 | |
| `no_vehicle_max` | 120 | |
| `hoard_res` | 800 | |
| `hoard_max` | 120 | |
| `vehicle_front_max` | 90 | |

## Calibration

Limits unchanged after the first three runs (25.11.); the deviations below are real, not too tight limits.

| Run | Map, settings | What it showed |
|---|---|---|
| `20261125-133129` | Crossroads, dev default | before 9.2: NF radar never accepted (922 s), BE pad/armory never, R1/R2 violations in both teams |
| `20261125-135805` | Crossroads, dev default | after 9.2: R1 max 59/60 s, R2 max 24/32 s; VF 141/139 s; first vehicle out +34/+60 s; pad +394/+346 s |
| `20261125-135810` | Palmbay, production values (`emp_sv_research_complete 0`, multipliers 1) | VF research 3:23/3:31, no VF placed (CV drives to a remote site), R4 hoarding 211/180 s; ended at 7:14: the NF CV drove off-route into the sea and drowned (game over, then the server's next map). `match_flow.py` now names this (`event=cv-lost`) |
| `20261125-144235` | Palmbay, production values, with route-keep/deep-water fix | full 15:31; VF placed 4:33/7:07 (+70/+217 s), built in 157/126 s, 11/7 vehicles; BE VF waited for the forward barracks (one engineer, one unfinished building at a time); R4 hoarding 271/360 s; R5 3 of 11 / 3 of 7 lost before the front |
| `20261125-150814` | Palmbay, production values, with 9.3 (slot-1 perk from the first life) and 9.4b (factory before forward barracks with one engineer) | full 15:44; VF placed 4:46/4:03 (+111/+63 s), built 47/45 s after the engineer started (before 121/122 s), M3 90/103 s after placement (walk/idle 43/58 s); 9/11 vehicles; R4 240/211 s; R5 BE 8 of 11 lost before the front (open) |

- `vf_build` 90 stays: the repair upgrade doubles construction per clip unit and the clip-less rate (RE
  `engineer.md`). With it active from the first life one engineer builds the 300 HP of a vehicle factory in about 45 s
  (Crossroads A/B `20261125-150503` 45 s vs `150523` 119 s without). What is left over 90 s is the walk to the site.
  The engineer's own supply crate does not refill the kit clip.
- `pad_barracks_after_vf` 180: the repair pad came 346–394 s after the VF (commander order of placement), a bot topic.
