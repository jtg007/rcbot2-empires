# Commander order types (m_iCommandType)

Source: `empires/bin/server_srv.so` (32 bit, with symbols), static analysis 2026-11-10.

## CEmpPlayer fields (offsets)

| Field | Offset | Evidence |
|---|---|---|
| m_iCommandType | +0x1458 | `NetworkVar_m_iCommandType::Set` 0x674b80, calls in PostThink |
| m_vCommandLocation | +0x145c..0x1464 | PostThink 0x669561 (distance to the position) |
| m_iCommandTarget | +0x1468, **array of 32 int** up to 0x14e8 (read element 0; a SendProp reader that expects a single value gets nothing) | `NetworkVar_m_iCommandTarget::Set(int index, int const&)` 0x673f60; loop 0x64f0b9..0x64f150 clears all 32, then element 0 = target entindex |
| m_flOrderCancelTime | +0x1e40 | PostThink 0x669557 |
| order from squad leader (otherwise commander) | +0x152d (byte) | GiveSquadOrder 0x65c502 sets 1, Command_emp_unit_order 0x64ed56 sets 0 |

## Type values (confirmed)

| Value | Order | Evidence |
|---|---|---|
| 0 | no order / abort | emp_unit_order arg 0 -> "From Commander: Abort your current command order." (0x64ee19) |
| 1 | Move to location | PostThink 0x668ef5 -> "Move order completed" (0x9d76c4/0x9d76e8); rejection 0x670855 "move order (move to waypoint)" |
| 2 | Attack location (force attack ground) | emp_unit_order 0x64f5fa: arg 2 -> "Attack this location" (0x9d502c); rejection 0x671bba "(force attack ground)"; GiveSquadOrder 0x65c1db |
| 3 | Attack target | emp_unit_order 0x64f01d -> "Attack this target" (0x9d5054); rejection 0x671b4c; GiveSquadOrder 0x65bf9b |
| 4 | Guard unit | PostThink 0x668eec -> "Guard order failed. Unit was lost." (0x9d7710/0x9d7744); emp_unit_order 0x64f02a -> "Guard this unit" (0x9d507c); rejection 0x671b83 |

Other orders have **no type of their own**: `GiveSquadOrder(order, targetEnt, pos, name)` 0x65bac0 takes
input codes 1/2/3/4 directly, 14 = resupply, 15 = heal, 30 = repair (all set type **4** + target,
0x65c4ca/0x65c59a/0x65c6e2), 34 = abort ("Attack order cancelled").

## Setting the fields

- `emp_unit_order` (0x64e9a0) has two forms (corrected 2026-11-15, the earlier `x y z [entindex]` was wrong):
  - `emp_unit_order <type> <x> <y> <z>` (argc >= 5): location; the target entindex stays -1 (0x64e9ac).
  - `emp_unit_order <type> <entindex>` (argc 3..4, 0x650114: argv[1] type, argv[2] entindex at 0x65014b).
    Measured: the location form with a 5th argument left m_iCommandTarget[0] = -1 (run 20261115-112940).
  A type > 100 is reduced by 100 (0x64ec0a, meaning unknown).
- Unit selection: `emp_unit_order_list <entindex> ...` (0x64cdf0) sets the commander's selection (+0x153c count = argc-1,
  max 32; +0x1540 handles). A bot commander can give orders like a human with these two commands
  (`rcbotd empires_test_unit_order`).
  Applied to every unit the commander has selected (+0x153c count, +0x1540 handles).
  - Type 1/2: m_vCommandLocation = (x,y,z).
  - Type 3/4: m_iCommandTarget[0] = entindex; guard additionally sets m_vCommandLocation = the target's AbsOrigin at the time of the order (0x64f079..0x64f0ac).
- Map entity `CEmpCommanderAutoTarget::SetCommand(CBasePlayer*, bool)` 0x625310 sets, without a commander:
  entity target -> 3 (bool false) / 4 (true); position -> 2 (false) / 1 (true).

## End of an order (PostThink)

- Type 1: ends when the player is within **100 units (3D)** of m_vCommandLocation (PostThink 0x669561, squared threshold 10000.0 at 0x957f98). Dispatch on the type at 0x668eb4; flag `+0x119 & 8` jumps to 0x6694b7 first. A type that changes from 1 to 0 while the player is farther away is therefore an abort, 2/3/4 a new order.
- Type 3: target dead/invalid, out of range or m_flOrderCancelTime expired -> type 0.
- Type 4: no distance or time limit. Ends only when the target is missing or dead/destroyed ("Unit was lost", 0x6696d0..0x66977b), on abort or team change (ChangeTeam 0x667f75).

## Accepting / rejecting

The game does not reject orders itself. "Negative, I won't accept ..." is the **recipient's** reply via
`emp_menu_command 33` (jump table 0x9d8db0, index 33 -> 0x66dfd8): only if m_iCommandType != 0;
sets type 0 and SetMinimapOrderActive(true). `emp_menu_command 35` (-> 0x66df00) = "Roger, order acknowledged!"
(only if type != 0 and byte +0x152c is set).

Exception (2026-11-15): the **entity form on an enemy target** is dropped silently, before any unit is touched
(0x64eb15..0x64ebe3, `ret` at 0x64eba9; nothing is set, so m_iCommandType stays 0):
- target is a CEmpPlayer (dynamic_cast 0x64eb93) and byte `+0x14ec` is 0, or it is set but the target is
  `InCond(1)` (0x6507b6) -> dropped. `+0x14ec` is written only in `CEmpPlayer::ShouldTransmit` (0x673173):
  1 when the receiving client `IsCommander()` (vtable +0x72c) and the result is not FL_EDICT_DONTSEND
  (`!(flags & 0x10)`); 0 only in the constructor (0x666aae). Meaning: "this enemy is sent to a commander client".
- target is another class: dynamic_cast to 0x9df7a0 (probably the building base) needs byte `+0x9a4` != 0.
- A friendly target (InSameTeam, 0x64eb6f) skips these checks.
Consequence: a **bot** commander cannot give type 3/4 on enemy players, because a fake client receives no
snapshots and ShouldTransmit never runs for it (engine behaviour, assumed, not checked in the engine binary).
Measured: run 20261115-115258, `unit_command_type_now=0`. Test aid: write 1 to the target's `+0x14ec` before
`emp_unit_order 3 <entindex>`.

## Assumed / open

- Type 2 events with commander_userid -1 (all players, early in the round) probably come from the map's
  `CEmpCommanderAutoTarget`, not from a human.
- Guard and heal/repair/resupply from a squad leader cannot be told apart by type (all 4); whether +0x152d is networked is unknown.
- Meaning of type+100 in emp_unit_order and of byte +0x14e8 on the commander.
