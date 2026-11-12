# Squad System — Static Reference

All findings below are from static analysis unless labelled otherwise.

## Ownership and state

Empires player squads are built from `CEmpPlayer` fields, the team roster, and `CEmpParams`; generic Source `CAI_Squad` is NPC infrastructure. Membership and authoritative squad points are keyed by `(team, squad ID)`. Squad IDs are 1–26; normal join admits at most five players, although the manager has eight backing slots.

- Player fields: `m_iSquad` `+0x1508`, leader byte `+0x150c`, position `+0x1530`, mirrored points `m_iEmpSquadXP` `+0x12c4`.
- `CEmpParams` owns membership and authoritative points. `m_iEmpSquadXP` is a per-player mirror.
- Forming resets that squad ID's balance; joining copies its current balance. Leaving or changing leader does not reset it.
- Leader position is zero. Position zero is therefore not a generic member location.

## Membership and leadership

Normal entry points are menu action 8 to form, action 12 or `emp_squad_leave` to leave, `emp_squad_join <id>` to join, `emp_squad_kick <userid>` to kick, and `emp_make_lead <userid>` to transfer. Command registration/ACLs and invite/lock field meanings remain unknown.

`LeaveSquad()` promotes the first roster-order same-team/squad nonleader when the departing player is marked leader. This path covers leave, kick, reassignment, and team change. `Event_Killed` does not call leave or leader transfer, so the static death path retains membership/leader state. The destructor removes manager membership directly without visible succession. A prior runtime disconnect experiment recorded position compaction and native authority at position zero, but this static pass did not locate the teardown step behind that outcome. Do not generalize the leave-path mechanism to disconnect, or native succession to death.

## Points

`CEmpParams::AddSquadPoints(team,squad)` adds one point per call with no recovered cap. Direct callers found:

- `IncrementRank(..., true, ...)`: +1 for a nonleader squad member with a leader within 3,000 HU squared-distance threshold. The gameplay events that pass `true` are not fully classified.
- `Command_emp_increase_squadpoints`: five additions for the command client's current team/squad; normal registration/authorization is unknown.

`UseSquadPoints(team,squad,cost,false)` is an affordability check; `true` subtracts exactly the cost and mirrors it to current members. No kill/assist/objective/heal earning rule was established by this static pass.

## Power dispatch

The normal route is `CEmpPlayer::ClientCommand("emp_menu_command", ...)`. Menu actions 48 and 49 require the leader marker and use an opaque player selector at `+0x11c0` (0–3). They are menu actions, not direct power IDs. A per-player shared debounce is set to **2.0 s** before later validation; this conflicts with an earlier approximate ~1 s runtime observation and needs a targeted live check.

| Action | Selector | Power | Cost | Static effect / time |
|---:|---:|---|---:|---|
| 48 | 0 | Recon | 2 | Enemy spotting scan to squared radius 10,240,000 (3,200 HU); 3 s state |
| 49 | 0 | Hide | 5 | Squad stealth deadline +30 s; exact modifier unknown |
| 48 | 1 | Charge | 2 | Squad sprint bonus deadline +30 s; exact multiplier unknown |
| 49 | 1 | Armor | 4 | Incoming damage ×0.8 infantry / ×0.9 vehicle; 30 s |
| 48 | 2 | Damage | 4 | Bullet damage ×1.2 and vehicle-target damage ×1.1; 30 s |
| 49 | 2 | Artillery | 5 | Validated aim trace creates `SquadArt`; 3 s display state, strike details unknown |
| 48 | 3 | Revive | 4/6/8/10 | Revives 1–4 dead same-squad players using safe placement; native path has no recovered Engineer requirement |
| 49 | 3 | Instant Heal | 6 | Health/condition path; exact targets and amount unknown |

Timed effects use separate per-player deadlines. Activation writes active ID/time for members present then; join copies points but not active power state. Leave, death, and spawn did not visibly clear the recovered power fields, but persistence is **INFERRED STATIC** and needs runtime verification. No separate power cooldown was found beyond the shared debounce. “Mass Heal” remains unresolved; the central dispatch labels selector 3/action 49 “Instant Heal.”

## Bot inputs and policy boundary

Current checkout reads squad ID, leader marker, position, and mirrored points; members can be grouped from the team roster. Active power ID/time and timed bonus deadlines have NetworkVar wrapper evidence, but a current RCBot reader was not found. The checkout contains an autonomous vacancy-revive transaction that applies bot policy (candidate/Engineer/vacancy/balance checks) and invokes the normal `emp_menu_command 48 1` route. Native Mass Revive itself did not show an Engineer requirement. Do not conflate that policy with native mechanics or interpret trailing `1` as a recovered power ID.

## Unresolved / runtime backlog

- Leader succession on disconnect; death and disconnect must be tested separately from leave/team-change.
- All squad IDs occupied; invite/lock and command ACLs; `+0x11c0` selector and trailing argument.
- Which rank events award squad points; whether the point-injection command is normally reachable.
- Recon center/visibility gates; Hide/Charge exact modifiers; artillery range/delay/strike damage.
- Revive ordering/placement and actual health restoration; Instant Heal amount/eligibility; whether a distinct Mass Heal exists.
- Active-state network exposure and persistence through join/leave/death/respawn; 2.0 s debounce vs prior runtime observation.


