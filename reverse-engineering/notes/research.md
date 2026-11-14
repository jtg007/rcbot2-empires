# Research

Static analysis of `server_srv.so` (2026-11-14) plus measured runs. Bot code:
`ReadTeamResearch` and the research block in `bot_empires_commander_economy.cpp`.

## Starting and resuming

- Client command `emp_comm_research <index>` (`Command_emp_comm_research` 0x64bf90), commander
  only (seat check `+0x72c`), like `emp_build`. A bot sends it with `helpers->ClientCommand`.
- `CEmpTeam::ResearchStart` 0x682280 checks silently (no message on failure):
  the research building (radar) is finished (Team+0x3d8; building type 2 = radar, 400 resources),
  no other research is active (Team+0x3d0 == 0), parents researched (`ResearchPrerequisitesMet` 0x3ae190).
- Cost is **per second** (`Cost` in the script); duration `Time / emp_sv_research_multiplier`.
- Not enough money pauses the research (Team+0x3d9). The game never resumes it by itself
  (`ResearchResume` 0x682d16 only when the radar comes back); the commander must send
  `emp_comm_research_resume`.

## Team state (CEmpTeam, size 0x480)

| Field | Offset |
|---|---|
| active item (0 = none) | +0x3d0 |
| remaining time | +0x3d4 |
| research building present | +0x3d8 |
| paused | +0x3d9 |
| item researched, byte per item | +0x3e4 + index |

`ResearchIsItemUnlocked` 0x3ae140 reads `Team+0x3e4+id`.

## Items (`scripts/research_items.txt`)

- Index = order in the file, counting from `Research_Index` 0 (**confirmed** 2026-11-14).
- `Start_Researched 1` items are set in `CEmpTeam::Init` 0x6835e2 (research_t+0x220), e.g.
  36 Jeep_Chassis, 37 Light_Tank_Chassis, 38 APC chassis.
- 60 Structural_Engineering (4/s, 60 s), 61 Vehicle_Factory (4/s, 60 s, needs 60, unlocks
  `emp_build 5`), 62/63 MG turret level 2/3, 64/65 ML turret level 2/3, 28 Upgraded_Chassis
  (300 s), 29 Medium_Tank_Chassis.
- Vehicle parts name their item in a `"Research"` key (`CEmpTank::CheckResearch` 0x69dbe0);
  an empty key means available from the start (standard MG 0, cannon 12, armour 0, engine 13).

## Measured

- Commander bot: radar → `emp_comm_research 60` → `61` → vehicle factory (run 20261114-005653,
  crossroads, instant build, research ×20). With production values: run 20261114-103144.
- Research only runs while the commander bot sits in the CV (the economy tick runs in the seat).

## Open

- Which items are worth researching after 60/61, and in which order (needs player knowledge).
