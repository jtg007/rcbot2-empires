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

## Join-team suicide of fake clients (2026-11-15)

Static analysis, same binary. Symptom: every bot logs `joined team "..."` followed by
`committed suicide with "player"` while still `<Unassigned>`.

- `jointeam N` is handled in `CEmpGameRules::ClientCommand` 0x3955e0 (string compare at 0x39565f),
  which calls `CEmpPlayer::JoinTeam(int)` 0x659610. JoinTeam checks `+0x1449`, the
  `emp_teamswitch_delay` gate at `+0x142c`, and `CEmpTeam::CanJoin`, then calls vtable `+0x178`
  = `CEmpPlayer::ChangeTeam(int)` 0x667d00 and sets `+0x142c = curtime + emp_teamswitch_delay`.
- `CEmpPlayer::ChangeTeam` resets class/loadout/desired-spawn netvars, leaves vehicle/squad,
  removes engineer items and mines; for team 1 it calls `StartObserverMode(7)` (vtable +0x5fc),
  otherwise `StopObserverMode` (+0x600); then tail-jumps to `CBasePlayer::ChangeTeam` 0x7ee940.
- The kill: `CBasePlayer::ChangeTeam` 0x7eea75 `cmpb $0x2,0xfc(%ebx)`: if `m_lifeState != LIFE_DEAD`
  it calls vtable +0x714 = `CEmpPlayer::CommitSuicide(false,false)` 0x6729c0 →
  `CBasePlayer::CommitSuicide` 0x7d68b0, which kills when `IsAlive()` and
  `curtime >= m_fNextSuicideTime (+0xb64)`. This happens before `CBaseCombatCharacter::ChangeTeam`
  (0x7eeb08) stores the new team, so the death is logged on team Unassigned.
- Why humans survive: `FinishClientPutInServer` 0x624900 calls InitialSpawn and Spawn, then
  `IsBot()` (vtable +0x71c, 0x62494c). Only for non-bots on team 0 it sets `m_takedamage(+0xfd)`,
  `pl.deadflag(+0xa38)=1`, `m_lifeState(+0xfc)=LIFE_DEAD`, MOVETYPE_OBSERVER, `ChangeTeam(0)`,
  `StartObserverMode(3)` and opens the class/team/specgui panels. Bots skip this block.
- Why a fake client is alive in Unassigned: `CEmpPlayer::Spawn` takes the dead branch
  (0x66b992..0x66ba01, `m_lifeState=2`) only when `EntSelectSpawnPoint` returns worldspawn.
  For team 0/1, `EntSelectSpawnPoint` 0x663742 returns a random `emp_info_map_overview`; maps
  that have one (e.g. emp_crossroads) spawn the bot alive at the overview camera.
- Cost of the death: `CEmpGameRules::PlayerKilled` 0x395710 sets `+0x1428 = curtime +
  emp_sv_respawn_penalty` (ConVar 0xbf5ee0, default "10"; the test server sets 0). Score `+0x12c0`
  is only decremented for team kills (killer != victim, 0x395833). Base rules add a death and a
  -1 frag for the suicide. No ticket code runs on death (ticket writes are in FPlayerCanRespawn
  and cap points), and the team is still Unassigned.
- Smallest plugin fix: before `jointeam`, if team is 0 and the `m_lifeState` byte is 0, write
  `m_lifeState = 2` (byte, sendprop offset) and `pl.deadflag = true`
  (`IServerGameClients::GetPlayerState`), then set `FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED`.
  The `+0xfc` check then skips CommitSuicide; PreThink runs PlayerDeathThink for the spawn stage.
