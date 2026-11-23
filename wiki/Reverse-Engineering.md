# Reverse engineering

Sources: [reverse-engineering/README.md](../reverse-engineering/README.md), `reverse-engineering/notes/*.md`, [AGENTS.md](../AGENTS.md). Notes are English. Addresses are ELF
link-time values of the analysed build; another Empires version has other addresses. Labels used in the notes: CONFIRMED, STRONGLY SUPPORTED,
INFERRED, UNKNOWN, and **Assumption** (not confirmed in the binary). No game files are stored in the repo.

## Target

| Item | Value |
|---|---|
| Module | `empires/bin/server_srv.so` (32-bit ELF, i386, not stripped, symbols present, no DWARF) |
| Size / SHA-256 | 14,863,947 bytes / `f7b7e884...a580` (`kKnownBinaries` in `bot_empires_binary.cpp`) |
| Tools | `nm -C --defined-only`, `objdump -d -C --start-address=...`, `readelf`, `strings`, Python `struct.unpack('<f', ...)` for `.rodata` floats |
| Loader chain | `srcds_run` -> `srcds_linux` -> `dedicated_srv.so` -> `engine_srv.so` -> `server_srv.so` |
| Plugin guard | `bot_empires_binary.cpp` compares size/CRC with `kKnownBinaries` before fixed offsets are used |

Workflow rule: expert findings about the binary go to `reverse-engineering/notes/` right away (English, short); ask an expert after two failed attempts.

## Notes (`reverse-engineering/notes/`) -- current, bot-relevant

| Note | Covers |
|---|---|
| `buildings.md` | `emp_build` argument forms and checks, refinery form, `CanBuildAt`/`GetAllObstructions` flags, costs, team resources (`CEmpTeam +0x3b4 -> +0x30` float), factory placement flags, `m_bFullyBuilt` lifecycle |
| `commander.md` | Entering/leaving the command vehicle, seat roles, `ChangeCommanderState`, `emp_comm_exit`, exit point |
| `commander-order-types.md` | `m_iCommandType` values (0 abort, 1 move, 2 attack location, 3 attack target, 4 guard), field offsets, setting fields, order end in `PostThink`, accept/reject rules |
| `engineer.md` | Engineer kit (`PrimaryAttack`, deployables, walls), repair/heal, revive, turret upgrade (`emp_eng_upgrade`), silent MG turret placement rejections (`GetBuildLocation`), supply crate (item 7), seismic grenade measurements |
| `vehicle-driving.md` | Driver input flow, turret aiming/firing, buying commands and chassis/part tables, factory path without debug purchase, enter/exit, running over players, scout stun grenade, readable state, factory orientation, repairs, vehicle ammo, weapon ammunition, wages and team pool, static props and traces, seat change |
| `research.md` | Starting/resuming research (`emp_comm_research`), team research fields, research item index list |
| `skills.md` | Player skills: fields, order of calls, when a choice becomes active, effect on the engineer kit |
| `spawn.md` | Spawn model, `jointeam` suicide of fake clients, Commander-built barracks spawns, APC mobile spawns, spawning at an APC |
| `squads.md` | Squad ownership/state, membership and leadership, points, power dispatch, bot-input boundary, runtime backlog |
| `maps.md` | Map findings: `emp_palmbay` boat capture points and ladders, BE APC approaches, `emp_dmz_b1` no-path analysis, `emp_coast` bunker flag |
| `plugin-callbacks.md` | Why work from chat/event handlers must be queued (EngiBot crash on SourceMod servers); client creation runs in `GameFrame` |

## Useful anchors (most reused)

| Mechanism | Anchor |
|---|---|
| Engineer kit primary attack | `CWeaponNFEngineerKit::PrimaryAttack` 0x3cf110; `BuildAmmoCrate` 0x3ce830; `Command_engSelect` 0x64e0e0; `GetBuildLocation` 0x3cd800 |
| Revive | `Command_emp_revive` 0x64d670 |
| Commander build / research | `Command_emp_build` 0x651890; `Command_emp_comm_research` 0x64bf90; `CEmpTeam::ResearchStart` 0x682280 |
| Vehicle buying | `emp_vehicle_build` 0x652d70; `emp_vehicle_chassis` 0x64cbf0; `emp_vehicle_weapon` 0x64a230 |
| Command vehicle seat | `CEmpTank::EnterVehicle` 0x695020; `CEmpTank::ExitVehicle` 0x6956e0; `Command_emp_comm_exit` 0x64bc20 |
| Shell timeout (timescale trap) | `CEmpShell` detonation check 0x3abe90 |
| Spawn | `CEmpPlayer::JoinTeam` 0x659610; `CEmpPlayer::ChangeTeam` 0x667d00; `CBarracks::SetHealth` 0x6148a0; `CEmpTank::AddSpawnPoint` 0x69d910 |
| Ladder movement | `CGameMovement::LadderMove` 0x3f2020 |
| Skills | `Command_changeskill` 0x64aa30; `UpdateSkills` 0x65b220; `ImplementSkills` 0x664320 |

Windows has no symbols in `server.dll`; port by finding the same strings/commands, see [Windows](Windows.md).
