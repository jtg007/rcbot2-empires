// Engineer revives dead teammates (emp_revive).
//
// Command_emp_revive (server_srv.so 0x64d670): "emp_revive <player index> <x> <y> <z>"
// (ints). The target must be a dead same-team player with a CEmpRagdoll
// (handle at player+0x1e34) and flags +0x1e3c/+0x1e45 clear; the position must
// be within 512 of the reviver (squared 262144); the active weapon must be the
// NF/IMP engineer kit with clip >= 50, which is spent. The command itself does
// not check the Revive skill; the bot only revives with m_bSkillRevive active
// (user rule 2026-11-12); the command checks byte +0x121f on the reviver. Skills are applied in CEmpPlayer::Spawn
// (UpdateSkills), so a newly chosen skill counts after the next respawn.
#include "bot_empires.h"
#include "bot_empires_binary.h"
#include "bot_globals.h"
#include "bot_getprop.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include "iplayerinfo.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "bot_cvars.h"

namespace
{
constexpr float kReviveSearchRange = 3000.0f;
constexpr float kReviveSendRange = 120.0f;
constexpr float kReviveProgressWindow = 6.0f;
constexpr float kReviveProgressDistance = 64.0f;
constexpr int kReviveClip = 50;

struct ReviveClaim
{
	int engineerUserId = -1;
	float until = 0.0f;
};
ReviveClaim s_reviveClaim[MAX_PLAYERS + 1];

CBaseEntity* BaseOf(edict_t* e)
{
	return e != nullptr && !e->IsFree() && e->GetUnknown() != nullptr ? e->GetUnknown()->GetBaseEntity() : nullptr;
}

// Server CEmpRagdoll of a dead player (handle at +0x1e34), nullptr if none.
edict_t* PlayerRagdoll(edict_t* player)
{
	CBaseEntity* base = BaseOf(player);
	if (base == nullptr || engine == nullptr || !EmpiresBinaryOffsetsTrusted())
		return nullptr;
	const char* p = reinterpret_cast<const char*>(base);
	const unsigned int handle = *reinterpret_cast<const unsigned int*>(p + 0x1e34);
	if (handle == 0xffffffffu || (handle & 0xfff) == 0xfff)
		return nullptr;
	if (p[0x1e3c] != 0 || p[0x1e45] != 0)
		return nullptr; // Command_emp_revive refuses these
	edict_t* ragdoll = engine->PEntityOfEntIndex(static_cast<int>(handle & 0xfff));
	return BaseOf(ragdoll) != nullptr ? ragdoll : nullptr;
}

// Ragdoll position. CBotGlobals::entityOrigin() returns (0 0 0) for the
// sdk_ragdoll (no collision); read CBaseEntity::m_vecAbsOrigin (+0x294, the
// field Command_emp_revive 0x64d8bd reads) and fall back to the dead body.
Vector RagdollOrigin(edict_t* ragdoll, edict_t* player)
{
	const CBaseEntity* base = BaseOf(ragdoll);
	if (base != nullptr && EmpiresBinaryOffsetsTrusted())
	{
		const Vector v = *reinterpret_cast<const Vector*>(reinterpret_cast<const char*>(base) + 0x294);
		if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f)
			return v;
	}
	return CBotGlobals::entityOrigin(player);
}
} // namespace

bool CBotEmpires::handleEmpiresRevive()
{
	if (gpGlobals == nullptr || engine == nullptr || playerinfomanager == nullptr || helpers == nullptr ||
		!isEmpiresEngineerClass())
		return false;
	// Revive skill active: the byte Command_emp_revive checks on the reviver.
	const CBaseEntity* self = BaseOf(m_pEdict);
	if (self == nullptr || !EmpiresBinaryOffsetsTrusted() || reinterpret_cast<const char*>(self)[0x121f] == 0)
		return false;
	const float now = gpGlobals->curtime;
	const int team = getTeam();
	const int userId = engine->GetPlayerUserId(m_pEdict);

	// Keep the current target while it stays revivable.
	edict_t* target = m_iReviveTarget > 0 ? engine->PEntityOfEntIndex(m_iReviveTarget) : nullptr;
	edict_t* ragdoll = nullptr;
	if (m_iReviveTarget > 0)
	{
		const bool sameTeammate = target != nullptr && !target->IsFree() &&
			engine->GetPlayerUserId(target) == m_iReviveTargetUserId && CClassInterface::getTeam(target) == team;
		IPlayerInfo* info = sameTeammate ? playerinfomanager->GetPlayerInfo(target) : nullptr;
		ragdoll = info != nullptr && info->IsConnected() && !CBotGlobals::entityIsAlive(target) ? PlayerRagdoll(target) : nullptr;
		const ReviveClaim& activeClaim = s_reviveClaim[m_iReviveTarget];
		const bool taken = activeClaim.engineerUserId != userId && activeClaim.until > now;
		if (ragdoll != nullptr && !taken)
		{
			const float distance = (RagdollOrigin(ragdoll, target) - getOrigin()).Length();
			if (distance <= m_flReviveBestDistance - kReviveProgressDistance ||
				(distance <= kReviveSendRange && m_flReviveBestDistance > kReviveSendRange))
			{
				m_flReviveBestDistance = distance;
				m_flReviveUntil = now + kReviveProgressWindow;
				m_bReviveReplanned = false;
			}
			else if (now >= m_flReviveUntil && distance > kReviveSendRange && !m_bReviveReplanned)
			{
				m_bReviveReplanned = true;
				m_flReviveNextNav = now;
				m_flReviveUntil = now + kReviveProgressWindow;
				RCBOT_DIAG("[RCBOT2] stall-escalate who=reviver stage=1 reason=no-approach userid=%d target=%d dist=%.0f\n",
					userId, m_iReviveTarget, distance);
			}
		}
		const bool stalled = ragdoll != nullptr && !taken && now >= m_flReviveUntil;
		if (ragdoll == nullptr || taken || stalled)
		{
			const bool alive = info != nullptr && info->IsConnected() && CBotGlobals::entityIsAlive(target);
			RCBOT_DIAG("[RCBOT2] revive end userid=%d target=%d reason=%s sent=%d\n", userId, m_iReviveTarget,
				taken ? "claimed" : !sameTeammate ? "target-gone" : alive ? (m_flReviveSent > 0.0f ? "revived" : "alive") : (ragdoll == nullptr ? "no-ragdoll" : "timeout"),
				m_flReviveSent > 0.0f ? 1 : 0);
			ReviveClaim& claim = s_reviveClaim[m_iReviveTarget];
			if (claim.engineerUserId == userId)
				claim.until = 0.0f;
			if (stalled)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=reviver stage=2 reason=no-revive-progress userid=%d target=%d\n",
					userId, m_iReviveTarget);
				m_reviveRetries.push_back({m_iReviveTargetUserId, now + 15.0f});
			}
			m_iReviveTarget = 0;
			m_flReviveSent = 0.0f;
			cancelNavigationAttempt("revive-end");
			return false;
		}
	}
	if (m_iReviveTarget == 0)
	{
		if (now < m_flReviveNextSearch)
			return false;
		m_flReviveNextSearch = now + 0.5f;
		m_reviveRetries.erase(std::remove_if(m_reviveRetries.begin(), m_reviveRetries.end(),
			[now](const ReviveRetry& retry) { return now >= retry.until; }), m_reviveRetries.end());
		float best = kReviveSearchRange;
		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			edict_t* player = engine->PEntityOfEntIndex(i);
			if (player == nullptr || player == m_pEdict || player->IsFree() || CClassInterface::getTeam(player) != team)
				continue;
			IPlayerInfo* info = playerinfomanager->GetPlayerInfo(player);
			if (info == nullptr || !info->IsConnected() || CBotGlobals::entityIsAlive(player))
				continue;
			if (now < s_reviveClaim[i].until)
				continue;
			const int targetUserId = engine->GetPlayerUserId(player);
			if (std::any_of(m_reviveRetries.begin(), m_reviveRetries.end(),
				[targetUserId](const ReviveRetry& retry) { return retry.targetUserId == targetUserId; }))
				continue;
			edict_t* r = PlayerRagdoll(player);
			if (r == nullptr)
			{
				if (now >= m_flReviveNextLog)
				{
					m_flReviveNextLog = now + 5.0f;
					const char* p = EmpiresBinaryOffsetsTrusted() ? reinterpret_cast<const char*>(BaseOf(player)) : nullptr;
					RCBOT_DIAG("[RCBOT2] revive skip userid=%d target=%d handle=%08x f1e3c=%d f1e45=%d\n", userId, i,
						p != nullptr ? *reinterpret_cast<const unsigned int*>(p + 0x1e34) : 0u,
						p != nullptr ? p[0x1e3c] : -1, p != nullptr ? p[0x1e45] : -1);
				}
				continue;
			}
			const Vector body = RagdollOrigin(r, player);
			const float d = (body - getOrigin()).Length();
			if (d < best)
			{
				if (d > kReviveSendRange)
				{
					RCBotNavMesh::Path path;
					if (RCBotNavMesh::BuildPath(getOrigin(), body, path, this) != RCBotNavMesh::PathResult::SUCCESS ||
						path.points.empty() || (path.points.back() - body).Length() > kReviveSendRange)
					{
						m_reviveRetries.push_back({targetUserId, now + 3.0f});
						continue;
					}
				}
				best = d;
				target = player;
				ragdoll = r;
			}
		}
		if (target == nullptr)
		{
			if (now >= m_flReviveNextLog)
			{
				m_flReviveNextLog = now + 10.0f;
				RCBOT_DIAG("[RCBOT2] revive scan userid=%d none\n", userId);
			}
			return false;
		}
		m_iReviveTarget = engine->IndexOfEdict(target);
		m_iReviveTargetUserId = engine->GetPlayerUserId(target);
		m_flReviveUntil = now + kReviveProgressWindow;
		m_flReviveBestDistance = best;
		m_bReviveReplanned = false;
		m_flReviveNextNav = 0.0f;
		m_flReviveSent = 0.0f;
		const Vector at = RagdollOrigin(ragdoll, target);
		RCBOT_DIAG("[RCBOT2] revive start userid=%d target=%d target_userid=%d at=(%.0f %.0f %.0f) dist=%.0f\n", userId,
			m_iReviveTarget, engine->GetPlayerUserId(target), at.x, at.y, at.z, best);
	}
	s_reviveClaim[m_iReviveTarget] = {userId, now + 3.0f};

	const Vector at = RagdollOrigin(ragdoll, target);
	const float dist = (at - getOrigin()).Length();
	if (dist > kReviveSendRange)
	{
		const bool idle = m_pSchedules == nullptr || m_pSchedules->isEmpty();
		if (now >= m_flReviveNextNav && (idle || m_bReviveReplanned || now >= m_flReviveNextNav + 10.0f))
		{
			m_flReviveNextNav = now + 3.0f;
			if (m_bEmpiresNavGoalActive)
				endEmpiresNavmeshGoal("drop", "revive", 0.0f);
			cancelNavigationAttempt("revive");
			if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
				m_pSchedules->freeMemory();
			if (!requestNavigation(at, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=reviver stage=2 reason=no-route userid=%d target=%d\n",
					userId, m_iReviveTarget);
				s_reviveClaim[m_iReviveTarget].until = 0.0f;
				m_reviveRetries.push_back({m_iReviveTargetUserId, now + 3.0f});
				m_iReviveTarget = 0;
				return false;
			}
			RCBOT_DIAG("[RCBOT2] revive walk userid=%d target=%d dist=%.0f\n", userId, m_iReviveTarget, dist);
		}
		return true;
	}

	// Close: kit out, then send once per 1.5 s while the target stays dead.
	stopMoving();
	const char* kit = team == 2 ? "weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
	edict_t* weapon = CClassInterface::getCurrentWeapon(m_pEdict);
	if (weapon == nullptr || weapon->GetClassName() == nullptr || std::strcmp(weapon->GetClassName(), kit) != 0)
	{
		selectWeaponName(kit);
		return true;
	}
	int clip = -1;
	readEmpiresEntityIntProperty(weapon, "m_iClip1", clip);
	if (clip < kReviveClip)
	{
		RCBOT_DIAG("[RCBOT2] revive end userid=%d target=%d reason=low-clip clip=%d sent=0\n", userId, m_iReviveTarget, clip);
		s_reviveClaim[m_iReviveTarget].until = 0.0f;
		m_iReviveTarget = 0;
		m_flReviveNextSearch = now + 15.0f;
		return false;
	}
	if (now >= m_flReviveSent + 1.5f)
	{
		char command[96];
		snprintf(command, sizeof(command), "emp_revive %d %d %d %d\n", m_iReviveTarget, static_cast<int>(at.x),
			static_cast<int>(at.y), static_cast<int>(at.z + 8.0f));
		helpers->ClientCommand(m_pEdict, command);
		m_flReviveSent = now;
		RCBOT_DIAG("[RCBOT2] revive send userid=%d target=%d clip=%d dist=%.0f\n", userId, m_iReviveTarget, clip, dist);
	}
	return true;
}

// Dead bot: wait with the respawn while an Engineer with Revive targets us
// (up to 10 s), and 3 s after death so a nearby one can pick us.
bool CBotEmpires::holdRespawnForRevive()
{
	if (gpGlobals == nullptr || engine == nullptr || m_pEdict == nullptr)
		return false;
	const float now = gpGlobals->curtime;
	const float dead = now - m_flReviveDeathTime;
	if (dead >= 10.0f)
		return false;
	const int me = engine->IndexOfEdict(m_pEdict);
	const int team = CClassInterface::getTeam(m_pEdict);
	bool targeted = false, candidate = false;
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* bot = CBots::getBot(slot);
		if (bot == nullptr || bot == this || !bot->inUse() || bot->getEdict() == nullptr || !bot->isAlive() ||
			CClassInterface::getTeam(bot->getEdict()) != team)
			continue;
		CBotEmpires* eng = static_cast<CBotEmpires*>(bot);
		const CBaseEntity* base = BaseOf(bot->getEdict());
		if (base == nullptr || !EmpiresBinaryOffsetsTrusted() || reinterpret_cast<const char*>(base)[0x121f] == 0)
			continue;
		if (eng->m_iReviveTarget == me && eng->m_iReviveTargetUserId == engine->GetPlayerUserId(m_pEdict) &&
			s_reviveClaim[me].until > now && s_reviveClaim[me].engineerUserId == engine->GetPlayerUserId(bot->getEdict()))
			targeted = true;
		else if (eng->m_iReviveTarget == 0 && dead < 3.0f &&
			eng->m_flReviveNextSearch < m_flReviveDeathTime + 3.0f &&
			(eng->getOrigin() - getOrigin()).Length() < kReviveSearchRange)
			candidate = true;
	}
	const bool hold = targeted || (candidate && dead < 3.0f);
	if (hold && targeted && !m_bReviveHoldLogged)
	{
		m_bReviveHoldLogged = true;
		RCBOT_DIAG("[RCBOT2] revive hold-respawn userid=%d dead=%.1f\n", engine->GetPlayerUserId(m_pEdict), dead);
	}
	if (!hold)
		m_bReviveHoldLogged = false;
	return hold;
}

void EmpiresReviveLevelReset()
{
	for (ReviveClaim& c : s_reviveClaim)
		c = ReviveClaim();
}
