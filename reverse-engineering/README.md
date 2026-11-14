# Empires server internals

Notes on how the Empires server (`empires/bin/server_srv.so`, 32-bit Linux, with
symbols) handles the things the bots need: commands, entity fields, rules and
limits. They come from static analysis of the binary (symbols, disassembly,
`.rodata` constants), the game's script files, and measurements in test matches.

Addresses are ELF link-time values of the binary that was analysed; another
Empires version will have different addresses. No game files are included here.

**Assumption** marks a point that is not confirmed in the binary.

| Note | Contents |
|---|---|
| [Buildings](notes/buildings.md) | `emp_build`, placement rules, costs, team resources |
| [Commander seat](notes/commander.md) | Entering and leaving the command vehicle, who is commander |
| [Commander orders](notes/commander-order-types.md) | Order types, fields, when an order ends |
| [Engineer](notes/engineer.md) | Engineer kit, repair, revive, deployables, turret upgrade |
| [Vehicles](notes/vehicle-driving.md) | Driver input, aiming and firing, buying, repair, factory orientation |
| [Research](notes/research.md) | Starting/resuming research, team research fields, item indices |
| [Squads](notes/squads.md) | Squad membership, points, powers |
| [Spawn](notes/spawn.md) | Spawn selection and respawn |
