# Player skills (perks): when a choice becomes active

Static analysis of `server_srv.so` (32 bit, symbols), 2026-11-15. Not yet confirmed by a run.

## Fields (CEmpPlayer)

- `+0x12c0` rank points. Slot gates (`DesiresSkill` 0x65a590, `UpdateSkills` 0x65b220,
  `ImplementSkills` 0x664320): slot 1 at points >= 0 (`jns`/`not;shr 31`), slot 2 > 9,
  slot 3 > 29, slot 4 > 59. Slot 1 is therefore free at 0 points, as in the game UI.
- `+0x1200..+0x120c` wanted skill per slot. `changeskill <slot> <code>`
  (`Command_changeskill` 0x64aa30 -> `ChangeSkill` 0x65a530) only stores here. No alive check.
- `+0x11f0..+0x11fc` active skill per slot (networked; `UpdateSkills` copies wanted -> active
  with a change accessor).
- Effect bytes, server only (not SendProps; the name strings are used only by
  `emp_debug_skills` 0x64aeb0). `AddSkill` 0x662b90 sets them: SKILL_HEALINGUPGRADE `+0x121c`,
  SKILL_REPAIRUPGRADE `+0x121d`, SKILL_TURRETUPGRADE `+0x121e`, SKILL_REVIVE `+0x121f`.
  Consequence: reading `m_bSkillIncreasedRepair` as a SendProp always fails (reads false).

## Order of calls

- `CEmpPlayer::Spawn` 0x66a8d0: when `+0x131c == 0`, branch 0x66a990 -> block 0x66b460
  (copy wanted loadout, `UpdateSkills` at 0x66b58a), jump back to 0x66a9a9, later
  `ReEquip(true)` at 0x66af8b -> `ImplementSkills` (clears all effect bytes, then `AddSkill`
  for each unlocked active slot). So a wish stored before a spawn is effective in that life.
- `ChangeEquipment` 0x665370 (console `changeequipment`): if loadout valid and in barracks
  (`IsInBarracks` 0x65f9f0, or vfunc 0x71c) -> `UpdateSkills` then tail-jump to `ReEquip(false)`.
- No other caller of `UpdateSkills`/`ReEquip`. A `changeskill` sent while alive therefore takes
  effect only at the next spawn.

## Effect on the engineer kit

`CWeaponNFEngineerKit::PrimaryAttack` 0x3cf110 (shared by the BE kit): at 0x3cf5cd, if
`+0x121d` is set, the amount from kit field `+0x8f4` is multiplied by kit field `+0x8fc`
(0x3cfc90). `+0x121c` multiplies the `+0x8f0` amount by `+0x8f8`. Only the effect byte is read,
not the active slot. Not yet determined: whether `+0x8f4` applies to construction or only to repair.
