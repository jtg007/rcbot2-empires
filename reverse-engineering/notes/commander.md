# Commander seat

Order types are in [commander-order-types.md](commander-order-types.md).

## Commander seat: enter and leave (server_srv.so, measured 2026-11-11)

- Seat roles of the CV (`CEmpTank`, flag `+0xfe0` = command vehicle): role 0 = driver, role 1 = commander seat.
- `CEmpTank::EnterVehicle` 0x695020: a non-commander entering any seat gets `CEmpPlayer::ChangeCommanderState(true, role==1)` 0x6620f0 (so the CV driver is already commander). A commander entering role != 1 gets `CommanderInterface(false)`.
- `CEmpTank::Think` 0x6a34e0 (0x6a38c8): a passenger in role 1 is thrown out via `HandlePassengerExit` and gets `ChangeCommanderState(true, true)`. In the commander view the player is **outside** the CV, so `m_hVehicle` reads 4095.
- `Command_emp_comm_exit` 0x64bc20: needs `IsCommander` (vfunc 0x72c, byte `player+0x1438` = `m_bCommander`) and `player+0x144c` (CV handle). Moves the player to the CV origin and, if `GetPassengerRole` (vfunc 0x3cc) != 0, calls `GetInVehicle(sv, 0)` (vfunc 0x658): back into the **driver seat**, still commander. It does not resign.
- `CEmpTank::ExitVehicle` 0x6956e0 (0x695a10): leaving a command vehicle from role != 1 calls `ChangeCommanderState(false,false)` -> `CEmpTeam::SetCommander(NULL)`, `m_bCommander=0`, `+0x144c=-1`.
- Use key in a vehicle: `CBaseServerVehicle::ItemPostFrame` 0x8baef0 (`m_afButtonPressed & IN_USE`), `CEmpTank::CanExitVehicle` 0x6a4900 (curtime >= `tank+0x8fc` = entry time + 1.0 s, speed `+0x810` <= cvar).
- `emp_change_seat` 0x64c340 requires `IsInAVehicle` (vfunc 0x4cc): no effect in the commander view.

Leave sequence for a bot: `emp_comm_exit`, wait until `m_hVehicle == CV` (>= 1 s), then tap IN_USE.
Measured (emp_streetsoffire): tick 1 comm_exit (vehicle=4095), tick 2 use (vehicle=74), then `LEFT_SEAT` (`IsCommander` = 0).

Who is commander: player netprop `m_bCommander` (`+0x1438`), view `m_bInCommanderInterface` (probably `+0x1449`), team pointer `CEmpTeam+0x474`. `m_hVehicle` does not show a commander in the commander view.

## Exit point (2026-11-14)

`CEmpTank::GetExitPoint` 0x69c620 uses the attachments `exit_pos_%i_%i` (string 0x9ded55) and sweeps
a player hull there with `TraceRay`, ignoring the CV and its 8 passengers (loop 0x69c780). A point is
rejected only for fraction < 1.0 or start/allsolid (0x69c9e7–0x69ca08). Building footprints are not
checked: a free spot in a niche of a building counts as valid. Run 20261114-093847: the commander
drove the CV onto the BE barracks (drive goal inside its footprint), got out into a niche and could
not leave for ~10 minutes. The bot now keeps the CV drive goal 150 away from buildings.
