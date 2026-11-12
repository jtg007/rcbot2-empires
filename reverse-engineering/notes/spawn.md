# Spawn and Respawn

Static analysis of `server_srv.so` plus earlier in-game observations.

## Current Model

Prior project runtime evidence reports:

    jointeam → changeclass → emp_spawn_select
    → FPlayerCanRespawn
    → LIFE_DEAD / LIFE_RESPAWNABLE transition
    → later PlayerDeathThink with held IN_ATTACK
    → respawn
    → CEmpPlayer::Spawn
    → EntSelectSpawnPoint
    → player alive

The important observation is that emp_spawn_select does not itself perform the final spawn.

Static analysis supports the main function path and the command’s selection-only role. It does not establish that a particular runtime call sequence was reached, nor does it explain held-versus-edge input behavior.

## Static Symbols

Exact symbols and static ELF link-time values from the fingerprinted binary:

| Symbol | Address | Size |
|---|---:|---:|
| CBasePlayer::PlayerDeathThink() | 0x007dd050 | 0x741 |
| CEmpGameRules::FPlayerCanRespawn(CBasePlayer*) | 0x0039a4f0 | 0xd47 |
| respawn(CBaseEntity*, bool) | 0x00624e30 | 0x7e |
| Command_emp_spawn_select(CCommand const&) | 0x0064e2c0 | 0x1d2 |
| CEmpPlayer::EntSelectSpawnPoint() | 0x006636e0 | 0x319 |
| CEmpPlayer::Spawn() | 0x0066a8d0 | 0x130a |

Addresses are ELF link-time values, not runtime absolute addresses.

## Static Call Relationships

- Command_emp_spawn_select parses a decimal ID and stores it in CEmpPlayer +0x1300, named m_iDesiredSpawn. It has no direct Spawn or respawn call. FPlayerCanRespawn and EntSelectSpawnPoint consume desired-spawn state; CEmpPlayer::Spawn clears it to -1 after CBasePlayer::Spawn returns.
- PlayerDeathThink dispatches through game-rules vtable slot +0x130 to CEmpGameRules::FPlayerCanRespawn. It writes life-state values 2 and 3 along the death/eligible paths, then later calls respawn(CBaseEntity*, bool).
- respawn dispatches through entity vtable slot +0x5c to CEmpPlayer::Spawn.
- CEmpPlayer::Spawn directly calls CBasePlayer::Spawn. That base method dispatches through game-rules slot +0x138 to CEmpGameRules::GetPlayerSpawnSpot. The rules wrapper delegates to CGameRules::GetPlayerSpawnSpot, which dispatches through player vtable slot +0x654 to CEmpPlayer::EntSelectSpawnPoint.
- The +0x654 player slot resolves to CEmpPlayer::EntSelectSpawnPoint for CEmpPlayer and CBasePlayer::EntSelectSpawnPoint for CBasePlayer.

FPlayerCanRespawn includes loadout validation, a current-time gate, desired/team spawn checks, and ticket-related team state.

## Relevant Fields

| Location | Current interpretation |
|---|---|
| Player +0xfc | Life state: 1 DYING, 2 DEAD, 3 RESPAWNABLE; public SDK const.h confirms enum values |
| CEmpPlayer +0xa68 | Input/button-like word; exact field name and edge semantics unknown |
| CEmpPlayer +0x1300 | Networked desired-spawn ID |
| CEmpPlayer +0x131c and +0x1e34 | Gate and entity handle for an early spawn-point branch; names/semantics unknown |
| CEmpPlayer +0x1428 and +0x1424 | Time gate and retry/countdown counter, inferred from reads/writes |
| CEmpPlayer +0x1430 and +0x1434 | Spawn-status-related state; exact names unknown |

EntSelectSpawnPoint searches team spawn classes emp_info_player_nf and emp_info_player_imp, compares candidate +0x358 against m_iDesiredSpawn, and selects among eligible matching candidates. It returns an entity; the game-rules spawn-spot path applies origin/angle setup.

## Open questions

The exact +0xa68 field and pulse-versus-held behavior, several unnamed fields, all FPlayerCanRespawn rejection branches, and runtime reachability remain open.
