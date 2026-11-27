# Grenade Ammo (Sticky, Stun, Hand Grenades)

Static analysis of `server_srv.so` (2026-11-27). Trigger: Lawliet run
`dev-runs/20261127-141522-emp_crossroads-6b-8m`, NF rifleman threw 12+ stickies per life, logged
`m_iAmmo[17]` never dropped (7, 52, 34->45, 12->14, 28; stun scout 2, 23, 37, 31, 42).

## Where the count lives

- Count = `CBaseCombatCharacter::m_iAmmo[type]` at player `+0x6bc + 4*type` (`GetAmmoCount` 0x3174d0,
  networked in `DT_LocalPlayerSpectatorsExclusive`). This is what the HUD shows. No per-weapon count:
  grenade scripts have `clip_size -1`, `default_clip 1`.
- Ammo types (`GetAmmoDef` 0x397260, index from 1): 2 GRENADE, 12 SMOKE, 13 INCED, 14 SEISMIC,
  15 MINE, 17 STICKY_GRENADE (NF/BE sticky and sticky-stun share type 17). MaxCarry 999 for all.
- Only writers of `m_iAmmo`: `GiveAmmo` 0x5970d0 (cap = MaxCarry 999), `RemoveAmmo` 0x317300,
  `SetAmmoCount` 0x317470 (only from `CEmpPlayer::Spawn` restoring the death snapshot `+0x1c54`),
  `RemoveAllAmmo` 0x3173f0.

## Class limits (scripts/max_ammo.txt in scripts_000.vpk)

- Loaded by `CEmpParams::Spawn` 0x646f70 into `maxClassAmmo[class][type-1]` (0xc35480) and
  `maxClassAmmoAtSpawn` (0xc35280); class = player `+0x11c0` (0 scout, 1 rifleman, 2 grenadier, 3 engineer).
- Rifleman: GRENADE 3, SEISMIC 4, STICKY 2. Scout: GRENADE 6, STICKY (stun) 2. Grenadier: GRENADE 5
  (mines). Engineer: SEISMIC 5. Spawn amount = max. A skill byte at player `+0x1222` doubles both.
- Only the class-aware paths respect these limits: spawn `GiveWeaponAndAmmo` 0x659c60, crate, APC.

## Throw and decrement

- `CEmpBaseGrenade::Operator_HandleAnimEvent` 0x38b950: events 0xbbd/0xbc5/0xbc8 -> Throw/Roll/Lob
  (vtable 0x61c/0x620/0x624, all reach `EmitGrenade` 0x630) and then `DecrementAmmo` 0x38cf50 ->
  owner `RemoveAmmo(1, weapon+0x4c0)`. Holster with a primed grenade: `DropGrenade` + decrement.
  No projectile path without decrement; no bot exception.

## Bot-only refill bug (root cause)

- `CPlayerInfo::SetActiveWeapon(const char*)` 0x7d4d50 (what RCBot `selectWeaponName` calls via
  `IBotController`): if `IsBot()` (FL_FAKECLIENT), `Weapon_Create(name)` + `Weapon_Equip` (vtable 0x430)
  + `Weapon_Switch`. No ownership check: every call spawns a new weapon entity.
- `CBaseCombatCharacter::Weapon_Equip` 0x59e770: for `GetMaxClip1() == -1` it calls
  `GiveAmmo(GetDefaultClip1(), primaryType)` -> +1 grenade per call, capped only at 999.
  For clip weapons it fills the clip / gives default ammo. Extra weapon entities pile up in
  `m_hMyWeapons` (48 slots).
- Safe selection of an owned weapon: `CUserCmd::weaponselect = entindex(owned weapon)`
  (handled by `CBasePlayer::SelectItem`; RCBot `m_iSelectWeapon`, as in `bot_fortress.cpp`).

## Resupply paths

- Engineer crate: players only via +use, `CEngineerCrate::Use` 0x62bc60 (same team, per-player timer
  at crate `+0x488[idx]`). Active weapon first, then re-gives the class grenade weapon
  (`GiveNamedItem("weapon_"+WeaponIDToAlias(player+0x11d0))`) if slot `m_hMyWeapons[1]` is empty, then
  the other slots. Per weapon `TryGiveAmmo` 0x62bc20 -> `CanGiveAmmo` 0x62baf0 (cur < max) ->
  `GiveAmmo` 0x62bb50: give max/4, or 1 if max <= 3, or max-cur at the top. Sticky: +1 per use tick.
- `CEngineerCrate::CrateThink` 0x62af30 only resupplies vehicles (`CEmpTank::GiveAmmo` 0x697ce0).
- APC `CEmpTank::ResupplyPassengers` 0x693f30: every 1.0 s, seats 1..n (not the driver),
  same max/4 / 1 / max-cur rule, re-gives the grenade weapon if `m_hMyWeapons[1]` is empty.

## Test hint

- No stock console command lowers ammo. After the selection fix a rifleman empties naturally after 2
  stickies. Otherwise SourceMod `SetEntProp(client, Prop_Send, "m_iAmmo", 0, 4, 17)` or a test command
  writing `+0x6bc+4*type`.

## Crate use sequence (verified 27.11.)

- `CEngineerCrate::Use` returns early when crate `+0x47c` or `+0x597` is set (likely not ready right
  after placement), when the activator is no player, or not on the same team. The first use only starts
  the "Open" sequence (sound `AmmoCrate.Open`); ammo is given on a later use while open, gated per player
  by `+0x488[idx]` (next time = now + `+0x59c` + const).
- Bots: hold +use with a new press edge every second for up to 6 s, starting 2 s after placement.
  Lawliet run 20261127-154450: rifleman sticky 0 -> 1 at the crew engineer's crate. 2.5 s of held use
  right after placement gave nothing (20261127-153719).
