// Engineer heals hurt teammates with the kit.
//
// CWeaponNFEngineerKit::PrimaryAttack (server_srv.so 0x3cf110) traces 100 units from the eye; a
// same-team player hit there gets CEmpPlayer::SetHealth and the kit spends one round. The kit
// places the selected deployable instead while m_iEngineerBuildItem != 0, and ItemPostFrame skips
// PrimaryAttack with an empty clip. The bot walks up to a nearby hurt teammate, aims at the
// chest and holds attack until the teammate is full, moves away or nothing changes.
#include "bot_empires.h"
#include "bot_empires_binary.h"
#include "bot_empires_lawliet.h"
#include "bot_globals.h"
#include "bot_cvars.h"
#include "bot_getprop.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include "in_buttons.h"
#include "bot_buttons.h"
#include "iplayerinfo.h"
#include <cmath>
#include <cstring>

namespace
{
constexpr float kHealSearchRange = 700.0f;
constexpr float kHealReach = 88.0f;      // eye to chest centre; the kit traces 100 to the hull
constexpr float kHealCloseIn = 220.0f;   // from here walk straight at the chest like the repair action
constexpr float kHealNoHealWindow = 4.0f; // in reach, no HP gained
constexpr float kHealApproachWindow = 8.0f;
constexpr float kHealTimeout = 25.0f;
constexpr float kHealSkip = 30.0f;
constexpr int kReviveReserve = 50; // Command_emp_revive needs 50 clip

struct HealClaim
{
	int engineerUserId = -1;
	float until = 0.0f;
	float since = 0.0f; // first claim of this heal, caps how long a bot patient waits
};
HealClaim s_healClaim[MAX_PLAYERS + 1];

Vector Chest(edict_t* player)
{
	return CBotGlobals::entityOrigin(player) + Vector(0.0f, 0.0f, 40.0f);
}

const char* KitName(int team)
{
	return team == EMP_TEAM_NF ? "weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
}

edict_t* CarriedKit(edict_t* self, int team)
{
	const CBaseHandle* weapons = CClassInterface::getWeaponList(self);
	if (weapons == nullptr)
		return nullptr;
	for (int slot = 0; slot < MAX_WEAPONS; ++slot)
	{
		edict_t* weapon = EmpiresCarriedWeapon(weapons[slot]);
		if (weapon != nullptr && std::strcmp(weapon->GetClassName(), KitName(team)) == 0)
			return weapon;
	}
	return nullptr;
}
} // namespace

// stopMoving() is priority-aware: from getTasks/modThink it lost against the running move task
// (the patient kept walking, run 20261122-233430). Stop with the override priority, as the
// engineer resupply fixture does.
void CBotEmpires::holdEmpiresHealPosition()
{
	const int saved = m_iMoveLookPriority;
	setMoveLookPriority(MOVELOOK_OVERRIDE);
	stopMoving();
	setMoveLookPriority(saved);
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
}

void CBotEmpires::lookEmpiresHealTarget(const Vector& at)
{
	const int saved = m_iMoveLookPriority;
	setMoveLookPriority(MOVELOOK_OVERRIDE);
	setLookVector(at);
	setLookAtTask(LOOK_VECTOR, 0.25f);
	setMoveLookPriority(saved);
}

bool EmpiresIsHuman(edict_t* player)
{
	if (player == nullptr || player->IsFree() || playerinfomanager == nullptr)
		return false;
	IPlayerInfo* info = playerinfomanager->GetPlayerInfo(player);
	if (info == nullptr || !info->IsConnected() || info->IsHLTV())
		return false;
	const int testUid = rcbot_empires_test_human_userid.GetInt();
	return !info->IsFakeClient() || (testUid > 0 && engine != nullptr && engine->GetPlayerUserId(player) == testUid);
}

bool EmpiresIsHumanUserId(int userId)
{
	return userId > 0 && EmpiresIsHuman(CBotGlobals::playerByUserId(userId));
}

void CBotEmpires::endEmpiresHeal(const char* reason, const float skipFor)
{
	const float now = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	edict_t* target = m_iHealTarget > 0 && engine != nullptr ? engine->PEntityOfEntIndex(m_iHealTarget) : nullptr;
	IPlayerInfo* info = target != nullptr && playerinfomanager != nullptr ? playerinfomanager->GetPlayerInfo(target) : nullptr;
	RCBOT_DIAG("[RCBOT2] heal end userid=%d target_userid=%d reason=%s hp=%d->%d secs=%.1f\n",
		engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1, m_iHealTargetUserId, reason, m_iHealStartHealth,
		info != nullptr ? info->GetHealth() : -1, now - m_flHealStarted);
	if (m_iHealTarget > 0 && m_iHealTarget <= MAX_PLAYERS &&
		s_healClaim[m_iHealTarget].engineerUserId == (engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1))
		s_healClaim[m_iHealTarget] = HealClaim();
	if (skipFor > 0.0f)
	{
		m_iHealSkipUserId = m_iHealTargetUserId;
		m_flHealSkipUntil = now + skipFor;
	}
	if (m_bHealAttacking && m_pButtons != nullptr)
		m_pButtons->letGo(IN_ATTACK);
	m_bHealAttacking = false;
	m_iHealTarget = 0;
	m_iHealTargetUserId = -1;
	m_flHealNextSearch = now + 1.0f;
	cancelNavigationAttempt("heal-end");
}

bool CBotEmpires::handleEmpiresHeal()
{
	if (gpGlobals == nullptr || engine == nullptr || playerinfomanager == nullptr || m_pPlayerInfo == nullptr ||
		m_pSchedules == nullptr)
		return false;
	const float now = gpGlobals->curtime;
	const int team = getTeam();
	const int userId = engine->GetPlayerUserId(m_pEdict);
	const bool enemy = hasSomeConditions(CONDITION_SEE_CUR_ENEMY) && m_pEnemy.get() != nullptr;
	const char* busy = m_pPlayerInfo->IsDead() || !isEmpiresEngineerClass() ? "not-engineer" :
		empiresVehicleOwnsBot() || m_pPlayerInfo->IsInAVehicle() ? "vehicle" :
		enemy ? "enemy" :
		m_iReviveTarget != 0 ? "revive" :
		m_iAmmoCrateStage != 0 ? "ammo-crate" :
		m_iBuildAssignStage > 0 ? "build-assign" :
		m_bTurretBuildArmed || m_iTurretBuilding > 0 ? "turret" :
		isEmpiresEngineerRepairActionActive() || isEmpiresEngineerBuildActionActive() ||
			isEmpiresEngineerBuildSelectionActive() || isEmpiresEngineerResupplyActionActive() ? "kit-action" :
		commanderDutyBlocksSideTasks() ? "commander" : nullptr;
	if (busy != nullptr)
	{
		if (m_iHealTarget > 0)
			endEmpiresHeal(busy, 0.0f);
		return false;
	}

	edict_t* kit = CarriedKit(m_pEdict, team);
	int clip = -1;
	if (kit == nullptr || !readEmpiresEntityIntProperty(kit, "m_iClip1", clip))
		clip = -1;
	const CBaseEntity* self = m_pEdict->GetUnknown() != nullptr ? m_pEdict->GetUnknown()->GetBaseEntity() : nullptr;
	// Keep a revive charge when this engineer has the Revive skill (+0x121f, see bot_empires_revive.cpp).
	const bool reviveSkill = self != nullptr && EmpiresBinaryOffsetsTrusted() &&
		reinterpret_cast<const char*>(self)[0x121f] != 0;
	const int reserve = reviveSkill ? kReviveReserve : 0;

	edict_t* target = m_iHealTarget > 0 ? engine->PEntityOfEntIndex(m_iHealTarget) : nullptr;
	if (m_iHealTarget == 0)
	{
		if (now < m_flHealNextSearch)
			return false;
		m_flHealNextSearch = now + 0.75f;
		if (clip <= reserve)
			return false;
		const Vector eye = getEyePosition();
		float bestScore = 1.0e9f;
		for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_PLAYERS; ++i)
		{
			edict_t* player = engine->PEntityOfEntIndex(i);
			if (player == nullptr || player->IsFree() || player == m_pEdict)
				continue;
			IPlayerInfo* info = playerinfomanager->GetPlayerInfo(player);
			if (info == nullptr || !info->IsConnected() || info->IsDead() || info->IsInAVehicle() ||
				info->GetTeamIndex() != team || info->GetHealth() <= 0)
				continue;
			const int health = info->GetHealth(), maxHealth = info->GetMaxHealth();
			if (maxHealth <= 0 || health >= maxHealth - 15 || health * 100 >= maxHealth * 85)
				continue;
			const int targetUserId = engine->GetPlayerUserId(player);
			if (targetUserId == m_iHealSkipUserId && now < m_flHealSkipUntil)
				continue;
			if (CBot* targetBot = CBots::getBotPointer(player); targetBot != nullptr)
			{
				CBotEmpires* tb = static_cast<CBotEmpires*>(targetBot);
				if (tb->commanderDutyBlocksSideTasks() || EmpiresLawlietBlocksSideTasks(team, i) ||
					tb->m_iReviveTarget != 0 || tb->m_iBuildAssignStage > 0 ||
					tb->m_iAmmoCrateStage != 0 || tb->m_iEmpiresBuildingActionMode != 0)
					continue;
			}
			const HealClaim& claim = s_healClaim[i];
			if (claim.engineerUserId >= 0 && claim.engineerUserId != userId && claim.until > now)
				continue;
			const Vector chest = Chest(player);
			const float distance = (chest - eye).Length();
			if (distance > kHealSearchRange * 2.0f)
				continue;
			const char* reject = distance > kHealSearchRange ? "far" : std::fabs(chest.z - eye.z) > 150.0f ? "height" :
				!CBotGlobals::isVisible(m_pEdict, eye, chest) ? "not-visible" : nullptr;
			if (reject != nullptr)
			{
				if (now >= m_flHealNextLog)
				{
					m_flHealNextLog = now + 5.0f;
					RCBOT_DIAG("[RCBOT2] heal skip userid=%d target_userid=%d reason=%s hp=%d dist=%.0f\n", userId,
						targetUserId, reject, health, distance);
				}
				continue;
			}
			float score = distance - static_cast<float>(maxHealth - health) * 4.0f;
			// TODO 8.3: humans first (a human 700 away beats a bot next to us).
			if (rcbot_empires_humans_first.GetBool() && EmpiresIsHuman(player))
				score -= 1000.0f;
			if (score < bestScore)
			{
				bestScore = score;
				target = player;
			}
		}
		if (target == nullptr)
			return false;
		IPlayerInfo* info = playerinfomanager->GetPlayerInfo(target);
		m_iHealTarget = engine->IndexOfEdict(target);
		m_iHealTargetUserId = engine->GetPlayerUserId(target);
		m_iHealStartHealth = m_iHealLastHealth = info->GetHealth();
		m_flHealStarted = m_flHealProgressAt = now;
		m_flHealBestDistance = (Chest(target) - eye).Length();
		m_flHealNextNav = 0.0f;
		m_bHealAttacking = false;
		m_bHealInReach = false;
		m_vHealNavGoal = vec3_origin;
		RCBOT_DIAG("[RCBOT2] heal start userid=%d target_userid=%d hp=%d/%d dist=%.0f clip=%d\n", userId,
			m_iHealTargetUserId, m_iHealStartHealth, info->GetMaxHealth(), m_flHealBestDistance, clip);
	}

	IPlayerInfo* info = target != nullptr && !target->IsFree() ? playerinfomanager->GetPlayerInfo(target) : nullptr;
	if (info == nullptr || engine->GetPlayerUserId(target) != m_iHealTargetUserId || !info->IsConnected() ||
		info->IsDead() || info->GetTeamIndex() != team)
	{
		endEmpiresHeal("target-gone", 0.0f);
		return false;
	}
	if (info->IsInAVehicle())
	{
		endEmpiresHeal("target-in-vehicle", 0.0f);
		return false;
	}
	const int health = info->GetHealth();
	if (health >= info->GetMaxHealth())
	{
		endEmpiresHeal("healed", 0.0f);
		return false;
	}
	if (clip <= reserve)
	{
		endEmpiresHeal(clip <= 0 ? "clip-empty" : "revive-reserve", kHealSkip);
		return false;
	}
	if (now - m_flHealStarted > kHealTimeout)
	{
		RCBOT_DIAG("[RCBOT2] stall-escalate who=healer stage=3 reason=timeout userid=%d target_userid=%d\n", userId,
			m_iHealTargetUserId);
		endEmpiresHeal("timeout", kHealSkip);
		return false;
	}
	// TODO 8.3: healing a bot while a hurt human waits in reach -> the human first.
	if (rcbot_empires_humans_first.GetBool() && now >= m_flHealHumanCheck && !EmpiresIsHuman(target))
	{
		m_flHealHumanCheck = now + 1.0f;
		const Vector eyeNow = getEyePosition();
		for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_PLAYERS; ++i)
		{
			edict_t* player = engine->PEntityOfEntIndex(i);
			if (player == nullptr || player == target || player == m_pEdict || !EmpiresIsHuman(player))
				continue;
			IPlayerInfo* hi = playerinfomanager->GetPlayerInfo(player);
			if (hi == nullptr || hi->IsDead() || hi->IsInAVehicle() || hi->GetTeamIndex() != team ||
				hi->GetMaxHealth() <= 0 || hi->GetHealth() <= 0 || hi->GetHealth() * 100 >= hi->GetMaxHealth() * 85)
				continue;
			const HealClaim& other = s_healClaim[i];
			if (other.engineerUserId >= 0 && other.engineerUserId != userId && other.until > now)
				continue;
			const Vector hc = Chest(player);
			if ((hc - eyeNow).Length() > kHealSearchRange || std::fabs(hc.z - eyeNow.z) > 150.0f ||
				!CBotGlobals::isVisible(m_pEdict, eyeNow, hc))
				continue;
			RCBOT_DIAG("[RCBOT2] heal human-first userid=%d bot_userid=%d human_userid=%d hp=%d\n", userId,
				m_iHealTargetUserId, engine->GetPlayerUserId(player), hi->GetHealth());
			endEmpiresHeal("human-first", 0.0f);
			m_flHealNextSearch = 0.0f;
			return false;
		}
	}
	HealClaim& claim = s_healClaim[m_iHealTarget];
	if (claim.engineerUserId != userId)
		claim.since = now;
	claim.engineerUserId = userId;
	claim.until = now + 0.5f;

	const Vector eye = getEyePosition();
	const Vector chest = Chest(target);
	const float distance = (chest - eye).Length();
	if (health > m_iHealLastHealth && distance <= kHealReach)
		m_flHealProgressAt = now;
	m_iHealLastHealth = health;
	if (now >= m_flHealNextLog)
	{
		m_flHealNextLog = now + 2.0f;
		RCBOT_DIAG("[RCBOT2] heal tick userid=%d target_userid=%d hp=%d dist=%.0f attacking=%d clip=%d\n", userId,
			m_iHealTargetUserId, health, distance, m_bHealAttacking ? 1 : 0, clip);
	}

	if (distance <= kHealReach && !m_bHealInReach)
		m_flHealProgressAt = now; // the no-heal window starts in reach
	m_bHealInReach = distance <= kHealReach;
	if (distance > kHealReach)
	{
		if (m_bHealAttacking)
			m_pButtons->letGo(IN_ATTACK);
		m_bHealAttacking = false;
		if (distance < m_flHealBestDistance - 48.0f)
		{
			m_flHealBestDistance = distance;
			m_flHealProgressAt = now;
		}
		else if (now - m_flHealProgressAt > kHealApproachWindow)
		{
			RCBOT_DIAG("[RCBOT2] stall-escalate who=healer stage=2 reason=no-approach userid=%d target_userid=%d dist=%.0f\n",
				userId, m_iHealTargetUserId, distance);
			endEmpiresHeal("no-approach", kHealSkip);
			return false;
		}
		if (distance > kHealSearchRange + 300.0f)
		{
			endEmpiresHeal("target-left", 10.0f);
			return false;
		}
		const bool visible = CBotGlobals::isVisible(m_pEdict, eye, chest);
		if (distance < kHealCloseIn && visible)
		{
			// setMoveTo stopped about 100 short of a standing teammate (run 20261122-232050);
			// face the chest and walk forward, as the building repair does.
			if (!m_pSchedules->isEmpty() || m_navigationIntent.active)
			{
				cancelNavigationAttempt("heal-close");
				m_pSchedules->freeMemory();
			}
			holdEmpiresHealPosition();
			lookEmpiresHealTarget(chest);
			Vector forward;
			AngleVectors(m_vViewAngles, &forward);
			Vector want = chest - eye;
			want.z = 0.0f;
			forward.z = 0.0f;
			if (want.NormalizeInPlace() > 1.0f && forward.NormalizeInPlace() > 0.1f && forward.Dot(want) > 0.9f)
				m_fForwardSpeed = 220.0f;
			return true;
		}
		// Route like the revive walk: replan when idle or when the teammate moved off the goal,
		// not every frame (cancelling every 1.5 s kept the engineer in place, run 20261122-232840).
		const Vector goal = CBotGlobals::entityOrigin(target);
		const bool idle = m_pSchedules->isEmpty();
		if (now >= m_flHealNextNav && (idle || (goal - m_vHealNavGoal).Length2D() > 150.0f))
		{
			m_flHealNextNav = now + 1.5f;
			m_vHealNavGoal = goal;
			if (m_bEmpiresNavGoalActive)
				endEmpiresNavmeshGoal("drop", "heal", 0.0f);
			cancelNavigationAttempt("heal");
			if (!m_pSchedules->isEmpty())
				m_pSchedules->freeMemory();
			if (!requestNavigation(goal, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
			{
				endEmpiresHeal("no-route", kHealSkip);
				return false;
			}
		}
		return true;
	}

	// In reach: stand, kit out, no deployable selected, aim at the chest, hold attack once aimed.
	if (!m_pSchedules->isEmpty() || m_navigationIntent.active)
	{
		cancelNavigationAttempt("heal-reach");
		m_pSchedules->freeMemory();
	}
	holdEmpiresHealPosition();
	if (now - m_flHealProgressAt > kHealNoHealWindow)
	{
		RCBOT_DIAG("[RCBOT2] stall-escalate who=healer stage=1 reason=no-heal userid=%d target_userid=%d hp=%d dist=%.0f\n",
			userId, m_iHealTargetUserId, health, distance);
		endEmpiresHeal("no-heal", kHealSkip);
		return false;
	}
	lookEmpiresHealTarget(chest);
	const edict_t* weapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool kitActive = weapon != nullptr && !weapon->IsFree() && weapon->GetClassName() != nullptr &&
		std::strcmp(weapon->GetClassName(), KitName(team)) == 0;
	if (!kitActive)
	{
		selectWeaponName(KitName(team));
		return true;
	}
	int item = 0;
	if (readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerBuildItem", item) && item != 0)
	{
		if (now >= m_flHealNextNav && helpers != nullptr)
		{
			m_flHealNextNav = now + 0.5f;
			helpers->ClientCommand(m_pEdict, "engSelect 0\n");
		}
		return true;
	}
	Vector forward;
	AngleVectors(m_vViewAngles, &forward);
	Vector want = chest - eye;
	want.NormalizeInPlace();
	if (forward.Dot(want) < 0.985f) // about 10 degrees
	{
		if (m_bHealAttacking)
			m_pButtons->letGo(IN_ATTACK);
		m_bHealAttacking = false;
		return true;
	}
	m_bHealAttacking = true;
	primaryAttack(true, 0.3f);
	return true;
}

// A bot being healed waits for its engineer like a player would: no enemy in sight, the engineer
// within 450 units, for at most 8 s per heal (a teammate walking on at the same speed kept the
// engineer 220 behind, run 20261122-232536).
bool CBotEmpires::empiresHealPatientWaiting()
{
	if (gpGlobals == nullptr || engine == nullptr || m_pEdict == nullptr || m_pPlayerInfo == nullptr ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->IsInAVehicle())
		return false;
	const int index = engine->IndexOfEdict(m_pEdict);
	if (index <= 0 || index > MAX_PLAYERS)
		return false;
	const float now = gpGlobals->curtime;
	const HealClaim& claim = s_healClaim[index];
	if (claim.engineerUserId < 0 || claim.until <= now || now - claim.since > 8.0f)
		return false;
	if (hasSomeConditions(CONDITION_SEE_CUR_ENEMY) && m_pEnemy.get() != nullptr)
		return false;
	if (commanderDutyBlocksSideTasks() || EmpiresLawlietBlocksSideTasks(getTeam(), index) ||
		m_iReviveTarget != 0 || m_iBuildAssignStage > 0 || m_iAmmoCrateStage != 0 ||
		m_iEmpiresBuildingActionMode != 0)
		return false;
	edict_t* engineer = CBotGlobals::playerByUserId(claim.engineerUserId);
	return engineer != nullptr && CBotGlobals::entityIsAlive(engineer) &&
		(CBotGlobals::entityOrigin(engineer) - getOrigin()).Length() <= 450.0f;
}

// Runs in modThink. The navmesh follower writes the move target every server frame
// (RCBotNavMeshProvider::Frame), so a stop alone did not hold (run 20261122-234053): give the
// route up once; getTasks stays out while waiting and the goal is picked again afterwards.
void CBotEmpires::tickEmpiresHealPatient()
{
	if (!empiresHealPatientWaiting())
	{
		if (m_bHealPatientHeld)
			RCBOT_DIAG("[RCBOT2] heal patient-release userid=%d\n", engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1);
		m_bHealPatientHeld = false;
		return;
	}
	if (!m_bHealPatientHeld)
	{
		m_bHealPatientHeld = true;
		if (m_bEmpiresNavGoalActive)
			endEmpiresNavmeshGoal("drop", "heal-wait", 0.0f);
		cancelNavigationAttempt("heal-wait");
		if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
			m_pSchedules->freeMemory();
		RCBOT_DIAG("[RCBOT2] heal patient-wait userid=%d\n", engine->GetPlayerUserId(m_pEdict));
	}
	holdEmpiresHealPosition();
}
