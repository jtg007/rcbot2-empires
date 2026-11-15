#include "bot_empires.h"
#include "bot_empires_telemetry.h"
#include "bot_weapons.h"
#include "in_buttons.h"
#include "bot_buttons.h"

#include "bot_getprop.h"
#include "bot_globals.h"
#include "bot_waypoint.h"
#include "bot_kv.h"
#include "bot_navigator.h"
#include "bot_profile.h"
#include "bot_schedule.h"
#include "bot_waypoint_locations.h"
#include "rcbot/logging.h"

#include "dt_send.h"
#include "server_class.h"
#include "const.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreorder"
#endif
#include "ientityinfo.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern IServerGameEnts* servergameents;
extern IServerTools* servertools;

namespace
{
	// This bound comes from the current Engineer-kit interaction distance and
	// is only used to rank a normal support target. The kit's own trace remains
	// authoritative when the action is executed.
	constexpr float kEmpiresEngineerSupportTraceWindow = 400.0f;
	class CEmpiresEngineerShotTraceFilter : public CTraceFilter
	{
	public:
		explicit CEmpiresEngineerShotTraceFilter(const IHandleEntity* passEntity)
			: m_passEntity(passEntity)
		{
		}

		bool ShouldHitEntity(IHandleEntity* entity, int) override
		{
			if (entity == m_passEntity)
				return false;

			if (enginetrace != nullptr)
			{
				const ICollideable* collideable =
					enginetrace->GetCollideable(entity);
				if (collideable != nullptr &&
					collideable->GetCollisionGroup() ==
						COLLISION_GROUP_VEHICLE_CLIP)
				{
					return false;
				}
			}

			return true;
		}

		TraceType_t GetTraceType() const override
		{
			return TRACE_EVERYTHING;
		}

	private:
		const IHandleEntity* m_passEntity;
	};

	int countEmpiresEngineerMGTurrets(int* highestIndex = nullptr)
	{
		if (highestIndex != nullptr)
			*highestIndex = -1;
		if (servertools == nullptr || engine == nullptr)
			return 0;

		int count = 0;
		for (CBaseEntity* pEntity = servertools->FirstEntity();
			pEntity != nullptr; pEntity = servertools->NextEntity(pEntity))
		{
			IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
			IServerNetworkable* pNetworkable = pUnknown != nullptr ?
				pUnknown->GetNetworkable() : nullptr;
			const char* className = pNetworkable != nullptr ?
				pNetworkable->GetClassName() : nullptr;
			edict_t* pEdict = pNetworkable != nullptr ?
				pNetworkable->GetEdict() : nullptr;
			if (className == nullptr || pEdict == nullptr || pEdict->IsFree() ||
				std::strcmp(className, "emp_building_mgturret") != 0)
			{
				continue;
			}

			++count;
			if (highestIndex != nullptr)
				*highestIndex = (std::max)(*highestIndex, engine->IndexOfEdict(pEdict));
		}
		return count;
	}

}

bool CBotEmpires::isEmpiresRepairableBuildingClass(
	const char* className, const int team)
{
	if (className == nullptr || (team != EMP_TEAM_NF && team != EMP_TEAM_BE))
		return false;

	const char* const buildingPrefix = team == EMP_TEAM_NF ?
		"emp_building_nf_" : "emp_building_imp_";
	if (std::strncmp(className, buildingPrefix, std::strlen(buildingPrefix)) == 0)
		return true;
	// Buildings placed by emp_build use the team-neutral class names
	// (Command_emp_build 0x651890); callers check the entity team.
	static const char* const kGenericBuildings[] = {
		"emp_building_barracks", "emp_building_vehiclefactory", "emp_building_refinery",
		"emp_building_radar", "emp_building_armory", "emp_building_repairstation",
		"emp_building_mgturret", "emp_building_mlturret" };
	for (const char* name : kGenericBuildings)
		if (std::strcmp(className, name) == 0)
			return true;
	return false;
}

bool CBotEmpires::findEmpiresEngineerNearbySupportTarget(
	int& buildingIndex) const
{
	buildingIndex = -1;
	if (servertools == nullptr || engine == nullptr || gpGlobals == nullptr ||
		m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		m_bBootstrapOnly || m_bControllerProbeOnly)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_ENGINEER)
	{
		return false;
	}

	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	// The 400 HU interaction window is the already-observed Engineer-kit
	// PrimaryAttack trace boundary. The action still performs the authoritative
	// normal kit trace; this selector only decides whether a nearby candidate
	// is worth taking over from an idle/roam schedule.
	// Was 400: from 290-380 away the action had to walk in itself and stalled
	// (as_escort turret 292, health never rose). Goal-roam now brings the bot to a
	// stand point at the building; start only from there.
	constexpr float kNearbySupportTraceWindow = 200.0f;
	const Vector eye = getEyePosition();
	float bestDistance = kNearbySupportTraceWindow;

	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pBuilding = pNetworkable != nullptr ?
			pNetworkable->GetEdict() : nullptr;
		const char* className = pNetworkable != nullptr ?
			pNetworkable->GetClassName() : nullptr;
		if (pBuilding == nullptr || pBuilding->IsFree() ||
			!isEmpiresRepairableBuildingClass(className, team))
		{
			continue;
		}

		const int buildingTeam = CClassInterface::getTeam(pBuilding);
		if (buildingTeam > 0 && buildingTeam != team)
			continue;

		int health = -1;
		int maxHealth = -1;
		if (!readEmpiresEntityIntProperty(pBuilding, "m_iHealth", health) ||
			!readEmpiresEntityIntProperty(pBuilding, "m_iMaxHealth", maxHealth) ||
			health < 0 || maxHealth <= 0 || health >= maxHealth)
		{
			continue;
		}

		const ICollideable* pCollision = pBuilding->GetCollideable();
		if (pCollision == nullptr)
			continue;

		const Vector collisionOrigin = pCollision->GetCollisionOrigin();
		const Vector mins = pCollision->OBBMins();
		const Vector maxs = pCollision->OBBMaxs();
		const Vector mid = (mins + maxs) * 0.5f;
		// Only from the building's own floor: the repair walks straight in, which
		// fails from a storey above (as_escort turret 292 from z -24, base -255).
		const float floorDelta = getOrigin().z - (collisionOrigin.z + mins.z);
		if (floorDelta > 100.0f || floorDelta < -100.0f)
			continue;
		const Vector targetPoints[] =
		{
			collisionOrigin + mid,
			collisionOrigin + Vector(mid.x, mid.y, maxs.z)
		};
		for (const Vector& targetPoint : targetPoints)
		{
			const float distance = (targetPoint - eye).Length();
			if (distance >= bestDistance)
				continue;

			bestDistance = distance;
			buildingIndex = engine->IndexOfEdict(pBuilding);
		}
	}

	return buildingIndex >= 0;
}

bool CBotEmpires::findEmpiresEngineerSupportTarget(
	int& buildingIndex, int& waypointId, Vector& waypointOrigin) const
{
	buildingIndex = -1;
	waypointId = -1;
	waypointOrigin = vec3_origin;

	if (servertools == nullptr || engine == nullptr || gpGlobals == nullptr ||
		m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		m_bBootstrapOnly || m_bControllerProbeOnly)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_ENGINEER)
	{
		return false;
	}

	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;
	const bool rejectedRouteActive =
		m_iEmpiresEngineerSupportRejectedBuildingIndex >= 0 &&
		m_iEmpiresEngineerSupportRejectedWaypoint >= 0 &&
		engine->Time() < m_flEmpiresEngineerSupportRejectedUntil;

	float bestScore = 1.0e30f;

	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pBuilding = pNetworkable != nullptr ?
			pNetworkable->GetEdict() : nullptr;
		const char* className = pNetworkable != nullptr ?
			pNetworkable->GetClassName() : nullptr;
		if (pBuilding == nullptr || pBuilding->IsFree() ||
			!isEmpiresRepairableBuildingClass(className, team))
		{
			continue;
		}

		const int buildingTeam = CClassInterface::getTeam(pBuilding);
		if (buildingTeam > 0 && buildingTeam != team)
			continue;
		const int candidateBuildingIndex = engine->IndexOfEdict(pBuilding);

		int health = -1;
		int maxHealth = -1;
		if (!readEmpiresEntityIntProperty(pBuilding, "m_iHealth", health) ||
			!readEmpiresEntityIntProperty(pBuilding, "m_iMaxHealth", maxHealth) ||
			health < 0 || maxHealth <= 0 || health >= maxHealth)
		{
			continue;
		}

		const ICollideable* pCollision = pBuilding->GetCollideable();
		if (pCollision == nullptr)
			continue;

		const Vector collisionOrigin = pCollision->GetCollisionOrigin();
		const Vector mins = pCollision->OBBMins();
		const Vector maxs = pCollision->OBBMaxs();
		const Vector mid = (mins + maxs) * 0.5f;
		const Vector targetPoints[] =
		{
			collisionOrigin + mid,
			collisionOrigin + Vector(mid.x, mid.y, maxs.z)
		};

		for (int candidateWaypoint = 0;
			candidateWaypoint < CWaypoints::numWaypoints(); ++candidateWaypoint)
		{
			CWaypoint* pWaypoint = CWaypoints::getWaypoint(candidateWaypoint);
			if (pWaypoint == nullptr || !pWaypoint->isUsed() ||
				!pWaypoint->forTeam(team))
			{
				continue;
			}
			if (rejectedRouteActive &&
				candidateBuildingIndex == m_iEmpiresEngineerSupportRejectedBuildingIndex &&
				candidateWaypoint == m_iEmpiresEngineerSupportRejectedWaypoint)
			{
				continue;
			}

			const Vector eye = pWaypoint->getOrigin() + Vector(0.0f, 0.0f, 28.0f);
			for (const Vector& targetPoint : targetPoints)
			{
				const float traceDistance = (targetPoint - eye).Length();
				if (traceDistance > kEmpiresEngineerSupportTraceWindow)
					continue;

				// Do not pre-filter with generic isShotVisible(): the current
				// Engineer kit remains authoritative for the final logical hit.
				const float score =
					(getOrigin() - pWaypoint->getOrigin()).Length() + traceDistance;
				if (score >= bestScore)
					continue;

				bestScore = score;
				buildingIndex = candidateBuildingIndex;
				waypointId = candidateWaypoint;
				waypointOrigin = pWaypoint->getOrigin();
			}
		}
	}

	return buildingIndex >= 0 && waypointId >= 0;
}

bool CBotEmpires::isEmpiresEngineerRepairActionActive() const
{
	return m_eEmpiresEngineerRepairActionState !=
		EmpiresEngineerRepairActionState::IDLE &&
		m_eEmpiresEngineerRepairActionState !=
		EmpiresEngineerRepairActionState::COMPLETE &&
		m_eEmpiresEngineerRepairActionState !=
		EmpiresEngineerRepairActionState::FAILED;
}

bool CBotEmpires::isEmpiresEngineerBuildSelectionActive() const
{
	return m_eEmpiresEngineerBuildSelectionState !=
		EmpiresEngineerBuildSelectionState::IDLE &&
		m_eEmpiresEngineerBuildSelectionState !=
		EmpiresEngineerBuildSelectionState::COMPLETE &&
		m_eEmpiresEngineerBuildSelectionState !=
		EmpiresEngineerBuildSelectionState::FAILED;
}

bool CBotEmpires::isEmpiresEngineerBuildActionActive() const
{
	return m_eEmpiresEngineerBuildActionState !=
		EmpiresEngineerBuildActionState::IDLE &&
		m_eEmpiresEngineerBuildActionState !=
		EmpiresEngineerBuildActionState::COMPLETE &&
		m_eEmpiresEngineerBuildActionState !=
		EmpiresEngineerBuildActionState::FAILED;
}

bool CBotEmpires::isEmpiresEngineerResupplyActionActive() const
{
	return m_eEmpiresEngineerResupplyActionState !=
		EmpiresEngineerResupplyActionState::IDLE &&
		m_eEmpiresEngineerResupplyActionState !=
		EmpiresEngineerResupplyActionState::COMPLETE &&
		m_eEmpiresEngineerResupplyActionState !=
		EmpiresEngineerResupplyActionState::FAILED;
}

bool CBotEmpires::isEmpiresEngineerResupplyNeeded() const
{
	if (m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		!m_pPlayerInfo->IsConnected() || m_pPlayerInfo->IsObserver() ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		m_pPrimaryWeapon == nullptr || !m_pPrimaryWeapon->hasWeapon() ||
		m_pPrimaryWeapon->getWeaponInfo() == nullptr)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_ENGINEER)
	{
		return false;
	}

	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	// The generic CBotWeapon entity/clip pointer can lag behind an Empires
	// reload/depletion transition.  Resolve the current carried primary by its
	// classname before reading its dynamically resolved clip value; reserve ammo
	// is still read through the normal CBot ammo list.
	int primaryClip = -1;
	int primaryClip2 = -1;
	const char* primaryName =
		m_pPrimaryWeapon->getWeaponInfo() != nullptr ?
		m_pPrimaryWeapon->getWeaponInfo()->getWeaponName() : nullptr;
	edict_t* primaryEntity = primaryName != nullptr ?
		CWeapons::findWeapon(m_pEdict, primaryName) : nullptr;
	const edict_t* activeEntity = CClassInterface::getCurrentWeapon(m_pEdict);
	if (activeEntity != nullptr && !activeEntity->IsFree() && primaryName != nullptr &&
		activeEntity->GetClassName() != nullptr &&
		std::strcmp(activeEntity->GetClassName(), primaryName) == 0)
	{
		// Empires can leave the generic CBotWeapon entity handle stale after
		// normal depletion/reload processing.  The current active entity is the
		// authoritative instance when it has the expected primary classname.
		primaryEntity = const_cast<edict_t*>(activeEntity);
	}
	if (primaryEntity == nullptr || primaryEntity->IsFree())
		return false;
	CClassInterface::getWeaponClip(primaryEntity, &primaryClip, &primaryClip2);
	return primaryClip <= 0 && m_pPrimaryWeapon->getAmmo(this) == 0;
}

bool CBotEmpires::findEmpiresEngineerAmmoSource(const int team,
	edict_t*& source, Vector& sourceOrigin) const
{
	source = nullptr;
	sourceOrigin = vec3_origin;
	if (servertools == nullptr || engine == nullptr || m_pController == nullptr ||
		(team != EMP_TEAM_NF && team != EMP_TEAM_BE))
	{
		return false;
	}

	const char* sourceClassName = team == EMP_TEAM_NF ?
		"emp_nf_ammo_crate" : "emp_imp_ammo_crate";
	float bestDistance = 1.0e30f;
	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pCandidate = pNetworkable != nullptr ?
			pNetworkable->GetEdict() : nullptr;
		const char* className = pNetworkable != nullptr ?
			pNetworkable->GetClassName() : nullptr;
		if (pCandidate == nullptr || pCandidate->IsFree() || className == nullptr ||
			std::strcmp(className, sourceClassName) != 0)
		{
			continue;
		}

		const int sourceTeam = CClassInterface::getTeam(pCandidate);
		if (sourceTeam > 0 && sourceTeam != team)
			continue;

		const Vector candidateOrigin = CBotGlobals::entityOrigin(pCandidate);
		const float distance = (candidateOrigin - m_pController->GetLocalOrigin()).Length();
		if (distance >= bestDistance)
			continue;

		bestDistance = distance;
		source = pCandidate;
		sourceOrigin = candidateOrigin;
	}

	return source != nullptr && sourceOrigin.IsValid();
}

void CBotEmpires::interruptEmpiresEngineerSupportForCombat(
	const char* reason)
{
	const bool supportActive = m_bEmpiresEngineerSupportTaskIssued ||
		m_bEmpiresEngineerSupportNearby || isEmpiresEngineerRepairActionActive();
	if (!supportActive)
		return;
	const bool actionActive = isEmpiresEngineerRepairActionActive();

	if (actionActive)
		failEmpiresEngineerRepairAction(reason != nullptr ? reason :
			"combat-interrupt");

	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires gameplay engineer-support=interrupted index=%d userid=%d reason=%s building_index=%d nearby=%d action_active=%d next=normal-combat",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			reason != nullptr ? reason : "combat-interrupt",
			m_iEmpiresEngineerSupportBuildingIndex,
			m_bEmpiresEngineerSupportNearby ? 1 : 0,
			actionActive ? 1 : 0);
	}

	m_bEmpiresEngineerSupportTaskIssued = false;
	m_bEmpiresEngineerSupportNearby = false;
	m_iEmpiresEngineerSupportBuildingIndex = -1;
	m_iEmpiresEngineerSupportWaypoint = -1;
	m_vEmpiresEngineerSupportWaypointOrigin = vec3_origin;
	m_flEmpiresNextEngineerSupportTime = engine != nullptr ?
		engine->Time() + 15.0f : 0.0f;

	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		m_pNavigator->clear();
	m_bEmpiresCoverScheduleActive = false;
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	stopMoving();
}

void CBotEmpires::interruptEmpiresEngineerResupplyForCombat(
	const char* reason)
{
	const bool resupplyActive = m_bEmpiresEngineerResupplyTaskIssued ||
		isEmpiresEngineerResupplyActionActive();
	if (!resupplyActive)
		return;
	const bool actionActive = isEmpiresEngineerResupplyActionActive();

	if (actionActive)
		failEmpiresEngineerResupplyAction(reason != nullptr ? reason :
			"combat-interrupt");
	EmpiresAnalysis::RecordBotEvent(this, "ENGINEER_RESUPPLY_INTERRUPTED", nullptr,
		reason != nullptr ? reason : "combat-interrupt");

	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires gameplay engineer-resupply=interrupted index=%d userid=%d reason=%s waypoint=%d action_active=%d next=normal-combat",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			reason != nullptr ? reason : "combat-interrupt",
			m_iEmpiresEngineerResupplyWaypoint, actionActive ? 1 : 0);
	}

	m_bEmpiresEngineerResupplyTaskIssued = false;
	m_iEmpiresEngineerResupplyWaypoint = -1;
	m_vEmpiresEngineerResupplyWaypointOrigin = vec3_origin;
	m_flEmpiresNextEngineerResupplyTime = engine != nullptr ?
		engine->Time() + 15.0f : 0.0f;

	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		m_pNavigator->clear();
	m_bEmpiresCoverScheduleActive = false;
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	stopMoving();
}

void CBotEmpires::failEmpiresEngineerResupplyAction(const char* reason)
{
	if (!isEmpiresEngineerResupplyActionActive())
		return;

	if (m_pButtons != nullptr)
	{
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_FORWARD);
		m_pButtons->letGo(IN_BACK);
		m_pButtons->letGo(IN_MOVELEFT);
		m_pButtons->letGo(IN_MOVERIGHT);
		m_pButtons->letGo(IN_USE);
		m_pButtons->letGo(IN_DUCK);
	}
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	m_pLookEdict = nullptr;
	setLookAtTask(LOOK_NONE);

	if (logger != nullptr)
	{
		logger->Log(LogLevel::WARN,
			"Empires engineer resupply=result=FAIL index=%d userid=%d source_index=%d source_class=%s ammo_type=%d ammo_before=%d ammo_last=%d clip_before=%d clip_last=%d frames=%d use_submitted=%d reason=%s",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			m_iEmpiresEngineerResupplyActionSourceIndex,
			m_pEmpiresEngineerResupplyActionSource != nullptr &&
				m_pEmpiresEngineerResupplyActionSource->GetClassName() != nullptr ?
				m_pEmpiresEngineerResupplyActionSource->GetClassName() : "<unknown>",
			m_iEmpiresEngineerResupplyActionAmmoType,
			m_iEmpiresEngineerResupplyActionAmmoBefore,
			m_iEmpiresEngineerResupplyActionAmmoLast,
			m_iEmpiresEngineerResupplyActionClipBefore,
			m_iEmpiresEngineerResupplyActionClipLast,
			m_iEmpiresEngineerResupplyActionFrames,
			m_bEmpiresEngineerResupplyActionUseSubmitted ? 1 : 0,
			reason != nullptr ? reason : "unspecified");
	}
	m_eEmpiresEngineerResupplyActionState =
		EmpiresEngineerResupplyActionState::FAILED;
}

bool CBotEmpires::requestEmpiresEngineerResupplyOnce(const bool fixtureMode,
	const bool depletionOnly)
{
	if (isEmpiresEngineerRepairActionActive() ||
		isEmpiresEngineerBuildSelectionActive() ||
		isEmpiresEngineerBuildActionActive() ||
		isEmpiresEngineerResupplyActionActive() ||
		isEmpiresMortarProbeActive() || isEmpiresMortarActionActive() ||
		m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr || engine == nullptr || gpGlobals == nullptr ||
		servertools == nullptr || !m_pPlayerInfo->IsConnected() ||
		m_pPlayerInfo->IsObserver() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_ENGINEER)
	{
		return false;
	}

	const int team = m_pPlayerInfo->GetTeamIndex();
	edict_t* pSource = nullptr;
	Vector sourceOrigin;
	if ((team != EMP_TEAM_NF && team != EMP_TEAM_BE) ||
		!findEmpiresEngineerAmmoSource(team, pSource, sourceOrigin))
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires engineer resupply rejected: no current-team ammo source team=%d",
				team);
		return false;
	}

	// Fixture mode positions the actor for the focused proof. Normal goal mode
	// starts at the bot's existing origin and leaves all movement to the route
	// selected by CBotEmpires::getTasks(). The source interaction itself remains
	// the normal IN_USE trace/Use path; no ammo or player state is written here.
	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		m_pNavigator->clear();
	m_eNormalPathState = EmpiresNormalPathState::IDLE;
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	// stopMoving() is priority-aware.  The normal Think loop may have left
	// MOVELOOK_TASK/ATTACK as the current owner, so explicitly give this
	// bounded fixture the same highest movement priority used by existing
	// one-shot probes before clearing the stale move target.
	setMoveLookPriority(MOVELOOK_OVERRIDE);
	stopMoving();

	Vector fixtureOrigin = getOrigin();
	const Vector aimPoint = sourceOrigin + Vector(0.0f, 0.0f, 24.0f);
	float groundTraceFraction = -1.0f;
	Vector groundTraceEnd = fixtureOrigin;
	if (fixtureMode)
	{
		// Empires' current IsUseableEntity() distance gate is 40 units from the
		// entity origin. Keep this debug-only fixture inside that gate; the
		// interaction itself remains the normal IN_USE trace/Use path.
		fixtureOrigin = sourceOrigin + Vector(32.0f, 0.0f, 0.0f);
		if (enginetrace != nullptr && m_pPlayerInfo != nullptr)
		{
		// A crate's origin is not necessarily the legal player feet origin.
		// Resolve the fixture position with the current player hull against
		// world brushes so the one-time placement can settle into a real
		// FL_ONGROUND state without writing player flags or using a map offset.
		const Vector traceStart = fixtureOrigin + Vector(0.0f, 0.0f, 128.0f);
		const Vector traceEnd = fixtureOrigin + Vector(0.0f, 0.0f, -512.0f);
		Ray_t groundRay;
		groundRay.Init(traceStart, traceEnd, m_pPlayerInfo->GetPlayerMins(),
			m_pPlayerInfo->GetPlayerMaxs());
		CEmpiresEngineerShotTraceFilter groundTraceFilter(
			m_pEdict->GetIServerEntity());
		trace_t groundTrace{};
		enginetrace->TraceRay(groundRay, MASK_PLAYERSOLID_BRUSHONLY,
			&groundTraceFilter, &groundTrace);
		groundTraceFraction = groundTrace.fraction;
		groundTraceEnd = groundTrace.endpos;
			if (groundTrace.fraction < 1.0f && groundTrace.endpos.IsValid())
				fixtureOrigin = groundTrace.endpos;
		}
	}
	// Use the existing dynamic-property teleport helper for this fixture. A
	// bare IBotController::SetAbsOrigin() is observable immediately, but the
	// next normal player-physics step restores the pre-fixture position. The
	// helper also supplies the existing short fly/ground transition used by
	// RCBot2 teleports; it does not change any production Engineer behavior.
	if (fixtureMode)
	{
		if (CClient* pFixtureClient = CClients::get(m_pEdict); pFixtureClient != nullptr)
			pFixtureClient->teleportTo(fixtureOrigin);
	}
	// Keep the controller/collision representation synchronized with the
	// dynamic property teleport.  The helper updates the networked origin and
	// the short move-type transition; SetAbsOrigin updates the bot controller's
	// collision-side origin for this fixture-only placement.
	if (fixtureMode)
	{
		m_pController->SetAbsOrigin(fixtureOrigin);
		QAngle fixtureAngles(0.0f,
			RAD2DEG(std::atan2(aimPoint.y - fixtureOrigin.y,
								 aimPoint.x - fixtureOrigin.x)), 0.0f);
		m_pController->SetAbsAngles(fixtureAngles);
	}

	m_iEmpiresEngineerResupplyActionTeam = team;
	m_iEmpiresEngineerResupplyActionExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iEmpiresEngineerResupplyActionExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pEmpiresEngineerResupplyActionExpectedEdict = m_pEdict;
	m_pEmpiresEngineerResupplyActionSource = pSource;
	m_iEmpiresEngineerResupplyActionSourceIndex = engine->IndexOfEdict(pSource);
	m_vEmpiresEngineerResupplyActionSourceOrigin = sourceOrigin;
	m_vEmpiresEngineerResupplyActionFixtureOrigin = fixtureOrigin;
	m_bEmpiresEngineerResupplyActionFixtureMode = fixtureMode;
	m_bEmpiresEngineerResupplyActionDepletionOnly = depletionOnly;
	m_bEmpiresEngineerResupplyHandoffCheckLogged = false;
	m_bEmpiresEngineerResupplyActionFixturePositionCorrected = false;
	m_bEmpiresEngineerResupplyActionFixturePositionDriftLogged = false;
	m_iEmpiresEngineerResupplyActionAmmoType = -1;
	m_iEmpiresEngineerResupplyActionAmmoBefore = -1;
	m_iEmpiresEngineerResupplyActionAmmoLast = -1;
	m_iEmpiresEngineerResupplyActionClipBefore = -1;
	m_iEmpiresEngineerResupplyActionClipLast = -1;
	m_iEmpiresEngineerResupplyActionFrames = 0;
	m_bEmpiresEngineerResupplyActionUseSubmitted = false;
	m_bEmpiresEngineerResupplyActionUseInputLogged = false;
	m_bEmpiresEngineerResupplyActionUsePreRunLogged = false;
	m_bEmpiresEngineerResupplyActionUseProcessedLogged = false;
	m_bEmpiresEngineerResupplyActionBaselineLogged = false;
	m_bEmpiresEngineerResupplyActionDepletionAttackSubmitted = false;
	m_bEmpiresEngineerResupplyActionDepletionReadinessLogged = false;
	m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted = false;
	m_bEmpiresEngineerResupplyActionDepletionComplete =
		!fixtureMode && !depletionOnly;
	m_flEmpiresEngineerResupplyActionDeadline = gpGlobals->curtime +
		(depletionOnly ? 90.0f : 15.0f);
	m_eEmpiresEngineerResupplyActionState =
		EmpiresEngineerResupplyActionState::WAITING_FOR_ACTIVE;

	const char* pistolClassName = team == EMP_TEAM_NF ?
		"weapon_emp_nf_pistol" : "weapon_emp_imp_pistol1";
	const char* actionWeaponClassName = pistolClassName;
	if (!fixtureMode && m_pPrimaryWeapon != nullptr &&
		m_pPrimaryWeapon->getWeaponInfo() != nullptr &&
		m_pPrimaryWeapon->getWeaponInfo()->getWeaponName() != nullptr)
	{
		// Fixture mode deliberately selects the pistol to make the proven refill
		// effect observable. Normal goal mode keeps the currently depleted
		// primary weapon selected so the real Engineer request is not detached
		// from the ammo condition that triggered it.
		actionWeaponClassName =
			m_pPrimaryWeapon->getWeaponInfo()->getWeaponName();
	}
	selectWeaponName(actionWeaponClassName);
	setLookVector(aimPoint);
	setLookAtTask(LOOK_VECTOR);
	const Vector appliedOrigin = m_pController->GetLocalOrigin();
	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires engineer resupply=armed index=%d userid=%d team=%d class=%d class_property=%s source_index=%d source_class=%s source_origin=(%.1f %.1f %.1f) fixture_origin=(%.1f %.1f %.1f) applied_origin=(%.1f %.1f %.1f) ground_trace_fraction=%.3f ground_trace_end=(%.1f %.1f %.1f) mode=%s positioning=%s normal_input=IN_USE deadline=%.3f",
			m_iEmpiresEngineerResupplyActionExpectedIndex,
			m_iEmpiresEngineerResupplyActionExpectedUserId, team, classId,
			classPropertyName != nullptr ? classPropertyName : "<unknown>",
			m_iEmpiresEngineerResupplyActionSourceIndex, pSource->GetClassName(),
			sourceOrigin.x, sourceOrigin.y, sourceOrigin.z,
			fixtureOrigin.x, fixtureOrigin.y, fixtureOrigin.z,
			appliedOrigin.x, appliedOrigin.y, appliedOrigin.z,
			groundTraceFraction, groundTraceEnd.x, groundTraceEnd.y,
			groundTraceEnd.z, fixtureMode ? "fixture" : "normal-goal",
			fixtureMode ? "SetAbsOrigin/SetAbsAngles" : "existing-origin",
			m_flEmpiresEngineerResupplyActionDeadline);
	}
	return true;
}

void CBotEmpires::handleEmpiresEngineerResupplyAction()
{
	if (!isEmpiresEngineerResupplyActionActive())
		return;

	if (engine == nullptr || gpGlobals == nullptr || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr || m_pEmpiresEngineerResupplyActionSource == nullptr)
	{
		failEmpiresEngineerResupplyAction("runtime-prerequisite-invalid");
		return;
	}

	++m_iEmpiresEngineerResupplyActionFrames;
	const float now = gpGlobals->curtime;
	if (engine->IndexOfEdict(m_pEdict) !=
		m_iEmpiresEngineerResupplyActionExpectedIndex ||
		engine->GetPlayerUserId(m_pEdict) !=
		m_iEmpiresEngineerResupplyActionExpectedUserId ||
		m_pEdict != m_pEmpiresEngineerResupplyActionExpectedEdict)
	{
		failEmpiresEngineerResupplyAction("client-identity-changed");
		return;
	}
	if (m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		m_pPlayerInfo->GetTeamIndex() != m_iEmpiresEngineerResupplyActionTeam)
	{
		failEmpiresEngineerResupplyAction("bot-no-longer-alive-or-team-changed");
		return;
	}

	edict_t* pSource = const_cast<edict_t*>(
		m_pEmpiresEngineerResupplyActionSource);
	if (pSource->IsFree() || pSource->GetClassName() == nullptr ||
		engine->IndexOfEdict(pSource) !=
		m_iEmpiresEngineerResupplyActionSourceIndex)
	{
		failEmpiresEngineerResupplyAction("source-invalid");
		return;
	}

	// The console command can arm this action after the current Think has
	// already prepared a movement command.  Correct the one stale movement
	// boundary once, from inside the action owner, then let normal physics and
	// neutral RunPlayerMove settle the player.  This is fixture-only; it does
	// In fixture mode this corrects one stale movement boundary; normal goal
	// mode does not reposition the actor. Neither mode writes ammo or building
	// state.
	if (m_bEmpiresEngineerResupplyActionFixtureMode &&
		!m_bEmpiresEngineerResupplyActionFixturePositionCorrected)
	{
		if (CClient* pFixtureClient = CClients::get(m_pEdict); pFixtureClient != nullptr)
			pFixtureClient->teleportTo(m_vEmpiresEngineerResupplyActionFixtureOrigin);
		m_pController->SetAbsOrigin(m_vEmpiresEngineerResupplyActionFixtureOrigin);
		m_bEmpiresEngineerResupplyActionFixturePositionCorrected = true;
		if (logger != nullptr)
		{
			const Vector controllerOrigin = m_pController->GetLocalOrigin();
			const Vector entityOrigin = CBotGlobals::entityOrigin(m_pEdict);
			const Vector* sendPropOrigin = CClassInterface::getOrigin(m_pEdict);
			const Vector eyeOrigin = getEyePosition();
			logger->Log(LogLevel::INFO,
				"Empires engineer resupply=fixture-position-corrected index=%d userid=%d origin=(%.1f %.1f %.1f) reason=flush-stale-movement-boundary",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_vEmpiresEngineerResupplyActionFixtureOrigin.x,
				m_vEmpiresEngineerResupplyActionFixtureOrigin.y,
				m_vEmpiresEngineerResupplyActionFixtureOrigin.z);
			logger->Log(LogLevel::INFO,
				"Empires engineer resupply=fixture-position-observed index=%d userid=%d controller=(%.1f %.1f %.1f) entity_collision=(%.1f %.1f %.1f) sendprop=(%.1f %.1f %.1f) eye=(%.1f %.1f %.1f)",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				controllerOrigin.x, controllerOrigin.y, controllerOrigin.z,
				entityOrigin.x, entityOrigin.y, entityOrigin.z,
				sendPropOrigin != nullptr ? sendPropOrigin->x : 0.0f,
				sendPropOrigin != nullptr ? sendPropOrigin->y : 0.0f,
				sendPropOrigin != nullptr ? sendPropOrigin->z : 0.0f,
				eyeOrigin.x, eyeOrigin.y, eyeOrigin.z);
		}
	}
	else if (m_bEmpiresEngineerResupplyActionFixtureMode &&
		!m_bEmpiresEngineerResupplyActionUseSubmitted)
	{
		// The normal player-physics step can restore the pre-fixture collision
		// origin after RunPlayerMove.  Keep this debug-only actor fixture at the
		// already validated interaction point until the normal IN_USE command is
		// submitted; the crate trace and the game interaction remain untouched.
		m_pController->SetAbsOrigin(
			m_vEmpiresEngineerResupplyActionFixtureOrigin);
	}

	// The position comparison above is intentionally a one-frame snapshot.
	// Detect a later engine-side drift without changing the fixture or writing
	// any movement/entity state.  This distinguishes stale velocity/physics from
	// an eye-position accessor that disagrees with the entity after RunPlayerMove.
	if (m_bEmpiresEngineerResupplyActionFixtureMode &&
		m_bEmpiresEngineerResupplyActionFixturePositionCorrected &&
		!m_bEmpiresEngineerResupplyActionFixturePositionDriftLogged &&
		logger != nullptr)
	{
		const Vector controllerOrigin = m_pController->GetLocalOrigin();
		const Vector entityOrigin = CBotGlobals::entityOrigin(m_pEdict);
		const Vector eyeOrigin = getEyePosition();
		const float entityDistance =
			(entityOrigin - m_vEmpiresEngineerResupplyActionFixtureOrigin).Length();
		const float eyeDistance = (eyeOrigin -
			(m_vEmpiresEngineerResupplyActionFixtureOrigin + Vector(0.0f, 0.0f, 60.0f))).Length();
		if (entityDistance > 16.0f || eyeDistance > 32.0f)
		{
			Vector velocity = vec3_origin;
			const bool velocityKnown = CClassInterface::getVelocity(m_pEdict, &velocity);
			logger->Log(LogLevel::WARN,
				"Empires engineer resupply=fixture-position-drift index=%d userid=%d fixture=(%.1f %.1f %.1f) controller=(%.1f %.1f %.1f) entity_collision=(%.1f %.1f %.1f) eye=(%.1f %.1f %.1f) entity_distance=%.1f eye_distance=%.1f velocity_known=%d velocity=(%.1f %.1f %.1f)",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_vEmpiresEngineerResupplyActionFixtureOrigin.x,
				m_vEmpiresEngineerResupplyActionFixtureOrigin.y,
				m_vEmpiresEngineerResupplyActionFixtureOrigin.z,
				controllerOrigin.x, controllerOrigin.y, controllerOrigin.z,
				entityOrigin.x, entityOrigin.y, entityOrigin.z,
				eyeOrigin.x, eyeOrigin.y, eyeOrigin.z, entityDistance, eyeDistance,
				velocityKnown ? 1 : 0, velocity.x, velocity.y, velocity.z);
			m_bEmpiresEngineerResupplyActionFixturePositionDriftLogged = true;
		}
	}

	// Own no movement or unrelated input while the one-shot interaction is
	// active.  Once the depletion shot has been scheduled, preserve its normal
	// button window until the clip transition is observed; clearing it here
	// would cancel the attack before the next outgoing RunPlayerMove boundary.
	// An Empires ammo crate advertises both impulse and continuous use.  The
	// first IN_USE edge opens it; its normal CEngineerCrate::Use() ammo path is
	// reached while IN_USE remains held.  Keep that hold bounded by the action
	// deadline and release it immediately on an observed effect/failure.
	const bool preserveResupplyUse =
		m_bEmpiresEngineerResupplyActionUseSubmitted &&
		m_eEmpiresEngineerResupplyActionState ==
			EmpiresEngineerResupplyActionState::OBSERVING_RESULT;
	const bool preserveDepletionAttack =
		m_bEmpiresEngineerResupplyActionDepletionAttackSubmitted &&
		!m_bEmpiresEngineerResupplyActionDepletionComplete &&
		!m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted;
	if (!preserveDepletionAttack)
		m_pButtons->letGo(IN_ATTACK);
	m_pButtons->letGo(IN_ATTACK2);
	m_pButtons->letGo(IN_FORWARD);
	m_pButtons->letGo(IN_BACK);
	m_pButtons->letGo(IN_MOVELEFT);
	m_pButtons->letGo(IN_MOVERIGHT);
	if (!preserveResupplyUse)
		m_pButtons->letGo(IN_USE);
	m_pButtons->letGo(IN_DUCK);
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons &= ~(IN_ATTACK2 | IN_FORWARD | IN_BACK | IN_MOVELEFT |
		IN_MOVERIGHT | IN_DUCK);
	if (!preserveResupplyUse)
		m_iButtons &= ~IN_USE;
	if (!preserveDepletionAttack)
		m_iButtons &= ~IN_ATTACK;
	if (preserveResupplyUse)
		m_pButtons->holdButton(IN_USE, 0.0f, 0.25f, 0.0f);
	setLookVector(m_vEmpiresEngineerResupplyActionSourceOrigin +
		Vector(0.0f, 0.0f, 24.0f));
	setLookAtTask(LOOK_VECTOR);
	doLook();

	const char* pistolClassName =
		m_iEmpiresEngineerResupplyActionTeam == EMP_TEAM_NF ?
			"weapon_emp_nf_pistol" : "weapon_emp_imp_pistol1";
	const edict_t* pWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const char* actionWeaponClassName = pistolClassName;
	if (!m_bEmpiresEngineerResupplyActionFixtureMode &&
		m_pPrimaryWeapon != nullptr && m_pPrimaryWeapon->getWeaponInfo() != nullptr &&
		m_pPrimaryWeapon->getWeaponInfo()->getWeaponName() != nullptr)
	{
		actionWeaponClassName =
			m_pPrimaryWeapon->getWeaponInfo()->getWeaponName();
	}
	const bool actionWeaponActive = pWeapon != nullptr && !pWeapon->IsFree() &&
		pWeapon->GetClassName() != nullptr &&
		std::strcmp(pWeapon->GetClassName(), actionWeaponClassName) == 0;
	if (!actionWeaponActive)
	{
		selectWeaponName(actionWeaponClassName);
		if (now >= m_flEmpiresEngineerResupplyActionDeadline)
			failEmpiresEngineerResupplyAction("action-weapon-did-not-become-active");
		return;
	}

	int clip1 = -1;
	int clip2 = -1;
	int ammoType1 = -1;
	int ammoType2 = -1;
	CClassInterface::getWeaponClip(const_cast<edict_t*>(pWeapon), &clip1, &clip2);
	CClassInterface::getAmmoTypes(const_cast<edict_t*>(pWeapon), &ammoType1,
		&ammoType2);
	if (ammoType1 < 0)
	{
		failEmpiresEngineerResupplyAction("active-weapon-ammo-type-unavailable");
		return;
	}
	const int ammo = getAmmo(static_cast<std::size_t>(ammoType1));
	m_iEmpiresEngineerResupplyActionAmmoType = ammoType1;
	m_iEmpiresEngineerResupplyActionAmmoLast = ammo;
	m_iEmpiresEngineerResupplyActionClipLast = clip1;

	if (!m_bEmpiresEngineerResupplyActionBaselineLogged)
	{
		m_iEmpiresEngineerResupplyActionAmmoBefore = ammo;
		m_iEmpiresEngineerResupplyActionClipBefore = clip1;
		m_bEmpiresEngineerResupplyActionBaselineLogged = true;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer resupply=baseline index=%d userid=%d source_index=%d source_class=%s distance=%.1f active_weapon=%s ammo_type=%d ammo=%d clip=%d input=IN_USE",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresEngineerResupplyActionSourceIndex, pSource->GetClassName(),
				distanceFrom(pSource), pWeapon->GetClassName(), ammoType1, ammo, clip1);
		m_eEmpiresEngineerResupplyActionState =
			EmpiresEngineerResupplyActionState::USING_SOURCE;
		return;
	}

	// A fresh Empires player starts at the pistol's reserve-ammo cap, so a
	// normal crate interaction is correctly a no-op until the reserve is
	// below that cap.  Deplete one round and reload through the ordinary bot
	// input path; never write ammo, clip or weapon state here.
	if (!m_bEmpiresEngineerResupplyActionDepletionComplete)
	{
		if (m_bEmpiresEngineerResupplyActionDepletionOnly)
		{
			// This bounded harness action owns no game state. It holds the
			// selected primary through normal RunPlayerMove input and taps the
			// normal reload command when the current magazine is empty. Once
			// both reserves are empty, normal getTasks() sees outOfAmmo().
			if (clip1 <= 0 && ammo <= 0)
			{
				m_pButtons->letGo(IN_ATTACK);
				m_iButtons &= ~IN_ATTACK;
				m_bEmpiresEngineerResupplyActionDepletionComplete = true;
				m_iEmpiresEngineerResupplyActionAmmoLast = ammo;
				m_iEmpiresEngineerResupplyActionClipLast = clip1;
				if (logger != nullptr)
					logger->Log(LogLevel::INFO,
						"Empires engineer resupply=depletion-complete index=%d userid=%d ammo_before=%d ammo_after=%d clip=%d method=normal-primary-and-reload out_of_ammo=1 handoff=normal-goal",
						engine->IndexOfEdict(m_pEdict),
						engine->GetPlayerUserId(m_pEdict),
						m_iEmpiresEngineerResupplyActionAmmoBefore, ammo, clip1);
				m_iButtons = 0;
				m_fForwardSpeed = 0.0f;
				m_fSideSpeed = 0.0f;
				m_fUpSpeed = 0.0f;
				m_pLookEdict = nullptr;
				setLookAtTask(LOOK_NONE);
				m_eEmpiresEngineerResupplyActionState =
					EmpiresEngineerResupplyActionState::COMPLETE;
				return;
			}

			if (clip1 <= 0)
			{
				m_pButtons->letGo(IN_ATTACK);
				m_iButtons &= ~IN_ATTACK;
				if (ammo > 0 &&
					!m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted)
				{
					reload();
					m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted = true;
					if (logger != nullptr)
						logger->Log(LogLevel::INFO,
							"Empires engineer resupply=depletion-reload index=%d userid=%d ammo=%d clip=%d input=normal-reload",
							engine->IndexOfEdict(m_pEdict),
							engine->GetPlayerUserId(m_pEdict), ammo, clip1);
				}
				if (now >= m_flEmpiresEngineerResupplyActionDeadline)
					failEmpiresEngineerResupplyAction("normal-ammo-depletion-timeout");
				return;
			}

			if (m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted)
				m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted = false;

			float playerNextAttack = 0.0f;
			const bool playerNextKnown = readEmpiresFloatProperty(m_pEdict,
				"m_flNextAttack", playerNextAttack);
			float weaponNextPrimary = 0.0f;
			const bool weaponNextKnown = readEmpiresWeaponFloatProperty(pWeapon,
				"m_flNextPrimaryAttack", weaponNextPrimary);
			if (!playerNextKnown || !weaponNextKnown)
			{
				failEmpiresEngineerResupplyAction(
					"depletion-readiness-properties-unavailable");
				return;
			}
			if (playerNextAttack > now || weaponNextPrimary > now)
				return;

			primaryAttack(true, 0.25f);
			m_bEmpiresEngineerResupplyActionDepletionAttackSubmitted = true;
			return;
		}

		float playerNextAttack = 0.0f;
		const bool playerNextKnown = readEmpiresFloatProperty(m_pEdict,
			"m_flNextAttack", playerNextAttack);
		float weaponNextPrimary = 0.0f;
		const bool weaponNextKnown = readEmpiresWeaponFloatProperty(pWeapon,
			"m_flNextPrimaryAttack", weaponNextPrimary);
		if (!playerNextKnown || !weaponNextKnown)
		{
			failEmpiresEngineerResupplyAction(
				"depletion-readiness-properties-unavailable");
			return;
		}

		if (playerNextAttack > now || weaponNextPrimary > now)
		{
			if (!m_bEmpiresEngineerResupplyActionDepletionReadinessLogged &&
				logger != nullptr)
			{
				logger->Log(LogLevel::INFO,
					"Empires engineer resupply=depletion-wait-ready index=%d userid=%d curtime=%.3f player_next_attack=%.3f weapon_next_primary=%.3f ammo=%d clip=%d",
					engine->IndexOfEdict(m_pEdict),
					engine->GetPlayerUserId(m_pEdict), now, playerNextAttack,
					weaponNextPrimary, ammo, clip1);
				m_bEmpiresEngineerResupplyActionDepletionReadinessLogged = true;
			}
			return;
		}

		if (!m_bEmpiresEngineerResupplyActionDepletionAttackSubmitted)
		{
			primaryAttack();
			m_bEmpiresEngineerResupplyActionDepletionAttackSubmitted = true;
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires engineer resupply=depletion-attack index=%d userid=%d active_weapon=%s ammo=%d clip=%d input=normal-primary",
					engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
					pWeapon->GetClassName(), ammo, clip1);
			return;
		}

		if (ammo < m_iEmpiresEngineerResupplyActionAmmoBefore)
		{
			m_bEmpiresEngineerResupplyActionDepletionComplete = true;
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires engineer resupply=depletion-complete index=%d userid=%d ammo_before=%d ammo_after=%d clip=%d method=normal-primary-and-reload",
					engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
					m_iEmpiresEngineerResupplyActionAmmoBefore, ammo, clip1);
			// The depletion phase intentionally changes the baseline.  The
			// subsequent crate assertion must accept a refill back to the
			// pre-depletion cap, not require ammo to exceed that cap.
			m_iEmpiresEngineerResupplyActionAmmoBefore = ammo;
			m_iEmpiresEngineerResupplyActionClipBefore = clip1;
			return;
		}

		if (clip1 < m_iEmpiresEngineerResupplyActionClipBefore &&
			!m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted)
		{
			m_pButtons->letGo(IN_ATTACK);
			m_iButtons &= ~IN_ATTACK;
			reload();
			m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted = true;
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires engineer resupply=depletion-reload index=%d userid=%d ammo=%d clip=%d input=normal-reload",
					engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
					ammo, clip1);
			return;
		}

		if (now >= m_flEmpiresEngineerResupplyActionDeadline)
			failEmpiresEngineerResupplyAction("normal-ammo-depletion-not-observed");
		return;
	}

	if (m_eEmpiresEngineerResupplyActionState ==
		EmpiresEngineerResupplyActionState::USING_SOURCE &&
		!m_bEmpiresEngineerResupplyActionUseSubmitted)
	{
		// The normal Empires E interaction uses the player's current view ray.
		// Do not submit IN_USE until that ray actually reaches the selected crate;
		// this only gates the debug fixture and does not select or invoke the
		// target on the bot's behalf.
		if (enginetrace == nullptr || m_pEdict->GetIServerEntity() == nullptr ||
			pSource->GetIServerEntity() == nullptr || servergameents == nullptr)
		{
			if (now >= m_flEmpiresEngineerResupplyActionDeadline)
				failEmpiresEngineerResupplyAction("use-trace-unavailable");
			return;
		}

		const Vector eye = getEyePosition();
		QAngle desiredAngles;
		VectorAngles((m_vEmpiresEngineerResupplyActionSourceOrigin +
			Vector(0.0f, 0.0f, 24.0f)) - eye, desiredAngles);
		Vector viewForward;
		// The matching debug-only alterOutgoingUserCmd() supplies these exact
		// view angles at the normal RunPlayerMove boundary.  Trace that planned
		// command here so E is not submitted one frame before the real ray can
		// reach the crate.
		AngleVectors(desiredAngles, &viewForward);
		const Vector viewRayEnd = eye + viewForward * 96.0f;
		CEmpiresEngineerShotTraceFilter useTraceFilter(
			m_pEdict->GetIServerEntity());
		trace_t useTrace{};
		Ray_t useRay;
		useRay.Init(eye, viewRayEnd);
		enginetrace->TraceRay(useRay, MASK_SOLID, &useTraceFilter, &useTrace);
		edict_t* traceHit = servergameents->BaseEntityToEdict(useTrace.m_pEnt);
		const int traceHitIndex = traceHit != nullptr && !traceHit->IsFree() ?
			engine->IndexOfEdict(traceHit) : -1;
		const char* traceHitClass = traceHit != nullptr &&
			traceHit->GetClassName() != nullptr ? traceHit->GetClassName() :
			"<world/none>";
		const bool traceHitsSource = traceHit == pSource;
		if (!traceHitsSource)
		{
			if (!m_bEmpiresEngineerResupplyActionUseTraceBlockedLogged &&
				logger != nullptr)
			{
				QAngle desiredAngles;
				VectorAngles((m_vEmpiresEngineerResupplyActionSourceOrigin +
					Vector(0.0f, 0.0f, 24.0f)) - eye, desiredAngles);
				logger->Log(LogLevel::INFO,
					"Empires engineer resupply=use-trace-blocked index=%d userid=%d eye=(%.1f %.1f %.1f) view=(%.2f %.2f) desired=(%.2f %.2f) fraction=%.3f hit_index=%d hit_class=%s target_index=%d target_distance=%.1f",
					engine->IndexOfEdict(m_pEdict),
					engine->GetPlayerUserId(m_pEdict), eye.x, eye.y, eye.z,
					m_vViewAngles.x, m_vViewAngles.y, desiredAngles.x,
					desiredAngles.y, useTrace.fraction, traceHitIndex,
					traceHitClass, m_iEmpiresEngineerResupplyActionSourceIndex,
					distanceFrom(pSource));
				m_bEmpiresEngineerResupplyActionUseTraceBlockedLogged = true;
			}
			if (now >= m_flEmpiresEngineerResupplyActionDeadline)
				failEmpiresEngineerResupplyAction("use-trace-never-reached-source");
			return;
		}

		if (logger != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires engineer resupply=use-trace-ready index=%d userid=%d eye=(%.1f %.1f %.1f) view=(%.2f %.2f) fraction=%.3f hit_index=%d hit_class=%s target_index=%d target_distance=%.1f",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				eye.x, eye.y, eye.z, m_vViewAngles.x, m_vViewAngles.y,
				useTrace.fraction, traceHitIndex, traceHitClass,
				m_iEmpiresEngineerResupplyActionSourceIndex, distanceFrom(pSource));
		}
		// The initial edge opens the crate. Keep the same normal button path
		// held so Empires can execute the crate's continuous-use ammo branch.
		use();
		m_pButtons->holdButton(IN_USE, 0.0f, 0.25f, 0.0f);
		m_bEmpiresEngineerResupplyActionUseSubmitted = true;
		m_eEmpiresEngineerResupplyActionState =
			EmpiresEngineerResupplyActionState::OBSERVING_RESULT;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer resupply=use-submitted index=%d userid=%d source_index=%d source_class=%s distance=%.1f active_weapon=%s ammo_before=%d clip_before=%d input=normal-IN_USE-initial-edge-plus-continuous-hold",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresEngineerResupplyActionSourceIndex, pSource->GetClassName(),
				distanceFrom(pSource), pWeapon->GetClassName(),
				m_iEmpiresEngineerResupplyActionAmmoBefore,
				m_iEmpiresEngineerResupplyActionClipBefore);
		return;
	}

	if (m_eEmpiresEngineerResupplyActionState ==
		EmpiresEngineerResupplyActionState::OBSERVING_RESULT &&
		(ammo > m_iEmpiresEngineerResupplyActionAmmoBefore ||
			clip1 > m_iEmpiresEngineerResupplyActionClipBefore))
	{
		m_pButtons->letGo(IN_USE);
		m_iButtons &= ~IN_USE;
		m_iEmpiresEngineerResupplyActionAmmoLast = ammo;
		m_iEmpiresEngineerResupplyActionClipLast = clip1;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer resupply=result=PASS index=%d userid=%d source_index=%d source_class=%s source_team=%d distance=%.1f active_weapon=%s ammo_type=%d ammo_before=%d ammo_after=%d clip_before=%d clip_after=%d frames=%d input=normal-IN_USE effect=real-crate-ammo",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresEngineerResupplyActionSourceIndex, pSource->GetClassName(),
				CClassInterface::getTeam(pSource), distanceFrom(pSource),
				pWeapon->GetClassName(), ammoType1,
				m_iEmpiresEngineerResupplyActionAmmoBefore, ammo,
				m_iEmpiresEngineerResupplyActionClipBefore, clip1,
				m_iEmpiresEngineerResupplyActionFrames);
		m_iButtons = 0;
		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		m_pLookEdict = nullptr;
		setLookAtTask(LOOK_NONE);
		m_eEmpiresEngineerResupplyActionState =
			EmpiresEngineerResupplyActionState::COMPLETE;
		return;
	}

	if (now >= m_flEmpiresEngineerResupplyActionDeadline)
		failEmpiresEngineerResupplyAction("resupply-effect-not-observed");
}

void CBotEmpires::failEmpiresEngineerBuildSelection(const char* reason)
{
	if (!isEmpiresEngineerBuildSelectionActive())
		return;

	if (m_pButtons != nullptr)
	{
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_FORWARD);
		m_pButtons->letGo(IN_BACK);
		m_pButtons->letGo(IN_MOVELEFT);
		m_pButtons->letGo(IN_MOVERIGHT);
		m_pButtons->letGo(IN_USE);
		m_pButtons->letGo(IN_DUCK);
	}
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons &= ~(IN_ATTACK | IN_ATTACK2 | IN_FORWARD | IN_BACK |
		IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK |
		IN_DUCK);

	if (logger != nullptr)
	{
		logger->Log(LogLevel::WARN,
			"Empires engineer build-select=result=FAIL index=%d userid=%d state=%d reason=%s frames=%d item_known=%d item_before=%d item_last=%d build_state_known=%d build_state_before=%d build_state_last=%d input_submitted=%d",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			static_cast<int>(m_eEmpiresEngineerBuildSelectionState),
			reason != nullptr ? reason : "unspecified",
			m_iEmpiresEngineerBuildSelectionFrames,
			m_bEmpiresEngineerBuildSelectionItemKnown ? 1 : 0,
			m_iEmpiresEngineerBuildSelectionBaselineItem,
			m_iEmpiresEngineerBuildSelectionLastItem,
			m_bEmpiresEngineerBuildSelectionStateKnown ? 1 : 0,
			m_iEmpiresEngineerBuildSelectionBaselineState,
			m_iEmpiresEngineerBuildSelectionLastState,
			m_bEmpiresEngineerBuildSelectionInputSubmitted ? 1 : 0);
	}
	m_eEmpiresEngineerBuildSelectionState =
		EmpiresEngineerBuildSelectionState::FAILED;
}

void CBotEmpires::handleEmpiresEngineerBuildSelection()
{
	if (!isEmpiresEngineerBuildSelectionActive())
		return;

	if (engine == nullptr || gpGlobals == nullptr || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr)
	{
		failEmpiresEngineerBuildSelection("runtime-prerequisite-invalid");
		return;
	}

	++m_iEmpiresEngineerBuildSelectionFrames;
	const float now = gpGlobals->curtime;
	if (engine->IndexOfEdict(m_pEdict) !=
		m_iEmpiresEngineerBuildSelectionExpectedIndex ||
		engine->GetPlayerUserId(m_pEdict) !=
		m_iEmpiresEngineerBuildSelectionExpectedUserId ||
		m_pEdict != m_pEmpiresEngineerBuildSelectionExpectedEdict)
	{
		failEmpiresEngineerBuildSelection("client-identity-changed");
		return;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_ENGINEER ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		m_pPlayerInfo->GetTeamIndex() != m_iEmpiresEngineerBuildSelectionTeam)
	{
		failEmpiresEngineerBuildSelection("bot-no-longer-alive-engineer-or-team-changed");
		return;
	}

	const char* kitClassName = m_iEmpiresEngineerBuildSelectionTeam == EMP_TEAM_NF ?
		"weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
	const edict_t* pCurrentWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool kitActive = pCurrentWeapon != nullptr && !pCurrentWeapon->IsFree() &&
		pCurrentWeapon->GetClassName() != nullptr &&
		std::strcmp(pCurrentWeapon->GetClassName(), kitClassName) == 0;
	if (!kitActive)
	{
		selectWeaponName(kitClassName);
		if (now >= m_flEmpiresEngineerBuildSelectionDeadline)
			failEmpiresEngineerBuildSelection("engineer-kit-selection-timeout");
		return;
	}

	int buildItem = -1;
	int buildState = -1;
	const bool itemKnown = readEmpiresDebugIntProperty(
		m_pEdict, "m_iEngineerBuildItem", buildItem);
	const bool stateKnown = readEmpiresDebugIntProperty(
		m_pEdict, "m_iEngBuild", buildState);
	if (!itemKnown && !stateKnown)
	{
		failEmpiresEngineerBuildSelection("build-selection-properties-unavailable");
		return;
	}

	if (!m_bEmpiresEngineerBuildSelectionItemKnown && itemKnown)
	{
		m_bEmpiresEngineerBuildSelectionItemKnown = true;
		m_iEmpiresEngineerBuildSelectionBaselineItem = buildItem;
		m_iEmpiresEngineerBuildSelectionLastItem = buildItem;
	}
	if (!m_bEmpiresEngineerBuildSelectionStateKnown && stateKnown)
	{
		m_bEmpiresEngineerBuildSelectionStateKnown = true;
		m_iEmpiresEngineerBuildSelectionBaselineState = buildState;
		m_iEmpiresEngineerBuildSelectionLastState = buildState;
	}

	if (logger != nullptr && !m_bEmpiresEngineerBuildSelectionBaselineLogged)
	{
		logger->Log(LogLevel::INFO,
			"Empires engineer build-select=baseline index=%d userid=%d team=%d class=%d class_property=%s weapon=%s item_known=%d item=%d build_state_known=%d build_state=%d deadline=%.3f",
			engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
			m_iEmpiresEngineerBuildSelectionTeam, classId,
			classPropertyName != nullptr ? classPropertyName : "<unknown>",
			kitClassName, itemKnown ? 1 : 0, buildItem, stateKnown ? 1 : 0,
			buildState, m_flEmpiresEngineerBuildSelectionDeadline);
		m_bEmpiresEngineerBuildSelectionBaselineLogged = true;
	}

	if (!m_bEmpiresEngineerBuildSelectionInputRequested)
	{
		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		m_iButtons &= ~(IN_ATTACK | IN_ATTACK2 | IN_FORWARD | IN_BACK |
			IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK |
			IN_DUCK);
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_FORWARD);
		m_pButtons->letGo(IN_BACK);
		m_pButtons->letGo(IN_MOVELEFT);
		m_pButtons->letGo(IN_MOVERIGHT);
		m_pButtons->letGo(IN_USE);
		m_pButtons->letGo(IN_DUCK);
		// The installed Empires Engineer UI exposes the current player command
		// `engSelect 1` for its first build-selection action.  Dispatch it through
		// the public server-side ClientCommand path before retaining the original
		// bounded secondary-input probe.  This is debug-only evidence collection;
		// it does not write the selection properties or alter normal bot logic.
		if (helpers == nullptr)
		{
			failEmpiresEngineerBuildSelection("client-command-helper-unavailable");
			return;
		}
		helpers->ClientCommand(m_pEdict, "engSelect 1\n");
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer build-select=command-requested index=%d userid=%d command=engSelect 1",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict));

		// Retain the existing normal secondary-input hold as a paired diagnostic
		// so the harness still records that the regular UserCmd path is intact.
		secondaryAttack(true, 0.75f);
		m_bEmpiresEngineerBuildSelectionInputRequested = true;
		m_eEmpiresEngineerBuildSelectionState =
			EmpiresEngineerBuildSelectionState::OBSERVING;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer build-select=secondary-requested index=%d userid=%d input=normal-secondary-attack-hold",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict));
		return;
	}

	if (itemKnown && buildItem != m_iEmpiresEngineerBuildSelectionLastItem)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer build-select=item-transition index=%d userid=%d item_before=%d item_after=%d build_state=%d",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresEngineerBuildSelectionLastItem, buildItem, buildState);
		m_iEmpiresEngineerBuildSelectionLastItem = buildItem;
		m_bEmpiresEngineerBuildSelectionTransitionLogged = true;
	}
	if (stateKnown)
		m_iEmpiresEngineerBuildSelectionLastState = buildState;

	if (m_bEmpiresEngineerBuildSelectionTransitionLogged)
	{
		m_pButtons->letGo(IN_ATTACK2);
		m_iButtons &= ~IN_ATTACK2;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer build-select=result=PASS index=%d userid=%d item=%d build_state=%d input=normal-secondary-attack-hold",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				buildItem, buildState);
		m_eEmpiresEngineerBuildSelectionState =
			EmpiresEngineerBuildSelectionState::COMPLETE;
		return;
	}

	if (now >= m_flEmpiresEngineerBuildSelectionDeadline)
		failEmpiresEngineerBuildSelection("selection-state-did-not-change");
}

void CBotEmpires::failEmpiresEngineerBuildAction(const char* reason)
{
	if (!isEmpiresEngineerBuildActionActive())
		return;

	if (m_pButtons != nullptr)
	{
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_FORWARD);
		m_pButtons->letGo(IN_BACK);
		m_pButtons->letGo(IN_MOVELEFT);
		m_pButtons->letGo(IN_MOVERIGHT);
		m_pButtons->letGo(IN_USE);
		m_pButtons->letGo(IN_DUCK);
	}
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons &= ~(IN_ATTACK | IN_ATTACK2 | IN_FORWARD | IN_BACK |
		IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK |
		IN_DUCK);

	if (logger != nullptr)
	{
		int highestIndex = -1;
		const int turretCount = countEmpiresEngineerMGTurrets(&highestIndex);
		logger->Log(LogLevel::WARN,
			"Empires engineer build=result=FAIL index=%d userid=%d state=%d reason=%s frames=%d target_waypoint=%d baseline_turrets=%d current_turrets=%d baseline_max_index=%d current_max_index=%d attack_submitted=%d",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			static_cast<int>(m_eEmpiresEngineerBuildActionState),
			reason != nullptr ? reason : "unspecified", m_iEmpiresEngineerBuildActionFrames,
			m_iEmpiresEngineerBuildActionTargetWaypoint,
			m_iEmpiresEngineerBuildActionBaselineTurrets, turretCount,
			m_iEmpiresEngineerBuildActionBaselineMaxIndex, highestIndex,
			m_bEmpiresEngineerBuildActionAttackSubmitted ? 1 : 0);
	}
	m_eEmpiresEngineerBuildActionState =
		EmpiresEngineerBuildActionState::FAILED;
}

void CBotEmpires::handleEmpiresEngineerBuildAction()
{
	if (!isEmpiresEngineerBuildActionActive())
		return;

	if (engine == nullptr || gpGlobals == nullptr || servertools == nullptr ||
		m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		m_pController == nullptr || m_pButtons == nullptr)
	{
		failEmpiresEngineerBuildAction("runtime-prerequisite-invalid");
		return;
	}

	++m_iEmpiresEngineerBuildActionFrames;
	const float now = gpGlobals->curtime;
	if (engine->IndexOfEdict(m_pEdict) !=
		m_iEmpiresEngineerBuildActionExpectedIndex ||
		engine->GetPlayerUserId(m_pEdict) !=
		m_iEmpiresEngineerBuildActionExpectedUserId ||
		m_pEdict != m_pEmpiresEngineerBuildActionExpectedEdict)
	{
		failEmpiresEngineerBuildAction("client-identity-changed");
		return;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_ENGINEER ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		m_pPlayerInfo->GetTeamIndex() != m_iEmpiresEngineerBuildActionTeam)
	{
		failEmpiresEngineerBuildAction("bot-no-longer-alive-engineer-or-team-changed");
		return;
	}

	const char* kitClassName = m_iEmpiresEngineerBuildActionTeam == EMP_TEAM_NF ?
		"weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
	const edict_t* pCurrentWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool kitActive = pCurrentWeapon != nullptr && !pCurrentWeapon->IsFree() &&
		pCurrentWeapon->GetClassName() != nullptr &&
		std::strcmp(pCurrentWeapon->GetClassName(), kitClassName) == 0;
	if (!kitActive)
	{
		selectWeaponName(kitClassName);
		if (now >= m_flEmpiresEngineerBuildActionDeadline)
			failEmpiresEngineerBuildAction("engineer-kit-selection-timeout");
		return;
	}

	int buildItem = -1;
	int buildState = -1;
	const bool itemKnown = readEmpiresDebugIntProperty(
		m_pEdict, "m_iEngineerBuildItem", buildItem);
	const bool stateKnown = readEmpiresDebugIntProperty(
		m_pEdict, "m_iEngBuild", buildState);
	if (!itemKnown)
	{
		failEmpiresEngineerBuildAction("build-selection-property-unavailable");
		return;
	}

	if (!m_bEmpiresEngineerBuildActionBaselineLogged)
	{
		logger->Log(LogLevel::INFO,
			"Empires engineer build=baseline index=%d userid=%d team=%d class=%d class_property=%s weapon=%s item=%d build_state_known=%d build_state=%d target_waypoint=%d target=(%.1f %.1f %.1f) baseline_turrets=%d baseline_max_index=%d deadline=%.3f",
			engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
			m_iEmpiresEngineerBuildActionTeam, classId,
			classPropertyName != nullptr ? classPropertyName : "<unknown>",
			kitClassName, buildItem, stateKnown ? 1 : 0, buildState,
			m_iEmpiresEngineerBuildActionTargetWaypoint,
			m_vEmpiresEngineerBuildActionTarget.x,
			m_vEmpiresEngineerBuildActionTarget.y,
			m_vEmpiresEngineerBuildActionTarget.z,
			m_iEmpiresEngineerBuildActionBaselineTurrets,
			m_iEmpiresEngineerBuildActionBaselineMaxIndex,
			m_flEmpiresEngineerBuildActionDeadline);
		m_bEmpiresEngineerBuildActionBaselineLogged = true;
	}

	if (m_eEmpiresEngineerBuildActionState ==
		EmpiresEngineerBuildActionState::WAITING_FOR_ACTIVE)
	{
		if (buildItem != 1)
		{
			if (!m_bEmpiresEngineerBuildActionCommandRequested)
			{
				if (helpers == nullptr)
				{
					failEmpiresEngineerBuildAction("client-command-helper-unavailable");
					return;
				}
				helpers->ClientCommand(m_pEdict, "engSelect 1\n");
				m_bEmpiresEngineerBuildActionCommandRequested = true;
				m_eEmpiresEngineerBuildActionState =
					EmpiresEngineerBuildActionState::WAITING_FOR_SELECTION;
				if (logger != nullptr)
					logger->Log(LogLevel::INFO,
						"Empires engineer build=selection-requested index=%d userid=%d command=engSelect 1",
						engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict));
			}
			return;
		}
		m_eEmpiresEngineerBuildActionState =
			EmpiresEngineerBuildActionState::WAITING_FOR_SELECTION;
	}

	// Empires may clear the selected build item immediately after a
	// successful placement. During result observation, the newly created
	// turret is the authoritative success signal; do not turn that normal
	// post-build state back into a selection failure.
	if (buildItem != 1 &&
		m_eEmpiresEngineerBuildActionState !=
			EmpiresEngineerBuildActionState::OBSERVING_RESULT)
	{
		if (now >= m_flEmpiresEngineerBuildActionDeadline)
			failEmpiresEngineerBuildAction("build-item-did-not-apply");
		return;
	}

	if (!m_bEmpiresEngineerBuildActionSelectionLogged)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer build=selection-applied index=%d userid=%d item=%d build_state_known=%d build_state=%d",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				buildItem, stateKnown ? 1 : 0, buildState);
		m_bEmpiresEngineerBuildActionSelectionLogged = true;
	}

	if (m_eEmpiresEngineerBuildActionState ==
		EmpiresEngineerBuildActionState::WAITING_FOR_SELECTION)
	{
		// Aim at a current loaded waypoint. The Engineer kit's own trace and
		// placement checks remain authoritative; this action never writes origin,
		// angles or build state directly. The normal look code is smoothed, so
		// wait for convergence before submitting Primary; firing in the same
		// frame as the first look request can trace behind the bot.
		setLookVector(m_vEmpiresEngineerBuildActionTarget);
		setLookAtTask(LOOK_VECTOR);
		doLook();
		const Vector eye = getEyePosition();
		QAngle desiredAngles;
		VectorAngles(m_vEmpiresEngineerBuildActionTarget - eye, desiredAngles);
		CBotGlobals::fixFloatAngle(&desiredAngles.x);
		CBotGlobals::fixFloatAngle(&desiredAngles.y);
		auto angleDelta = [](float current, float desired) -> float
		{
			float delta = current - desired;
			while (delta > 180.0f)
				delta -= 360.0f;
			while (delta < -180.0f)
				delta += 360.0f;
			return delta;
		};
		const float pitchError = std::fabs(angleDelta(m_vViewAngles.x,
			desiredAngles.x));
		const float yawError = std::fabs(angleDelta(m_vViewAngles.y,
			desiredAngles.y));
		constexpr float kEngineerBuildAimTolerance = 2.0f;
		if (pitchError > kEngineerBuildAimTolerance ||
			yawError > kEngineerBuildAimTolerance)
		{
			if (m_pButtons != nullptr)
				m_pButtons->letGo(IN_ATTACK);
			m_iButtons &= ~IN_ATTACK;
			return;
		}
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer build=aim-ready index=%d userid=%d current=(%.2f %.2f) desired=(%.2f %.2f) error=(%.2f %.2f) tolerance=%.1f",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_vViewAngles.x, m_vViewAngles.y, desiredAngles.x,
				desiredAngles.y, pitchError, yawError,
				kEngineerBuildAimTolerance);
		// Bounded diagnostic for the first unresolved construction boundary:
		// record what the normal Engineer view ray actually hits immediately
		// before the real Primary input is submitted.  This is observation
		// only; the Empires kit remains authoritative for placement and no
		// origin, build state or placement result is written here.
		if (logger != nullptr)
		{
			Vector viewForward;
			AngleVectors(m_vViewAngles, &viewForward);
			const Vector viewRayEnd = eye + viewForward * 8192.0f;
			CEmpiresEngineerShotTraceFilter engineerShotFilter(
				m_pEdict->GetIServerEntity());
			CBotGlobals::traceLine(eye, viewRayEnd, MASK_SHOT,
				&engineerShotFilter);
			const trace_t* trace = CBotGlobals::getTraceResult();
			edict_t* hit = trace != nullptr && trace->m_pEnt != nullptr &&
				servergameents != nullptr ?
				servergameents->BaseEntityToEdict(trace->m_pEnt) : nullptr;
			logger->Log(LogLevel::INFO,
				"Empires engineer build=trace index=%d userid=%d eye=(%.1f %.1f %.1f) view=(%.2f %.2f %.2f) target=(%.1f %.1f %.1f) fraction=%.3f end=(%.1f %.1f %.1f) hit_index=%d hit_class=%s startsolid=%d allsolid=%d",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				eye.x, eye.y, eye.z, m_vViewAngles.x, m_vViewAngles.y,
				m_vViewAngles.z, m_vEmpiresEngineerBuildActionTarget.x,
				m_vEmpiresEngineerBuildActionTarget.y,
				m_vEmpiresEngineerBuildActionTarget.z,
				trace != nullptr ? trace->fraction : -1.0f,
				trace != nullptr ? trace->endpos.x : 0.0f,
				trace != nullptr ? trace->endpos.y : 0.0f,
				trace != nullptr ? trace->endpos.z : 0.0f,
				hit != nullptr ? engine->IndexOfEdict(hit) : -1,
				hit != nullptr && hit->GetClassName() != nullptr ?
					hit->GetClassName() : "<world/none>",
				trace != nullptr && trace->startsolid ? 1 : 0,
				trace != nullptr && trace->allsolid ? 1 : 0);
		}
		primaryAttack(true, 0.35f);
		m_eEmpiresEngineerBuildActionState =
			EmpiresEngineerBuildActionState::OBSERVING_RESULT;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer build=attack-requested index=%d userid=%d target_waypoint=%d target=(%.1f %.1f %.1f) input=normal-primary-attack-hold",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresEngineerBuildActionTargetWaypoint,
				m_vEmpiresEngineerBuildActionTarget.x,
				m_vEmpiresEngineerBuildActionTarget.y,
				m_vEmpiresEngineerBuildActionTarget.z);
		return;
	}

	int highestIndex = -1;
	const int turretCount = countEmpiresEngineerMGTurrets(&highestIndex);
	if (turretCount > m_iEmpiresEngineerBuildActionBaselineTurrets)
	{
		int resultIndex = -1;
		const char* resultClassName = "<unknown>";
		int resultTeam = -1;
		int resultOwner = -1;
		int resultHealth = -1;
		int resultMaxHealth = -1;
		edict_t* resultBuilding = nullptr;
		for (CBaseEntity* pEntity = servertools->FirstEntity();
			pEntity != nullptr; pEntity = servertools->NextEntity(pEntity))
		{
			IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
			IServerNetworkable* pNetworkable = pUnknown != nullptr ?
				pUnknown->GetNetworkable() : nullptr;
			edict_t* pBuilding = pNetworkable != nullptr ?
				pNetworkable->GetEdict() : nullptr;
			const char* className = pNetworkable != nullptr ?
				pNetworkable->GetClassName() : nullptr;
			if (pBuilding == nullptr || pBuilding->IsFree() || className == nullptr ||
				std::strcmp(className, "emp_building_mgturret") != 0)
				continue;
			const int index = engine->IndexOfEdict(pBuilding);
			if (index > m_iEmpiresEngineerBuildActionBaselineMaxIndex)
			{
				resultBuilding = pBuilding;
				resultIndex = index;
				resultClassName = className;
				resultTeam = CClassInterface::getTeam(pBuilding);
				readEmpiresWeaponHandleProperty(pBuilding, "m_hOwner", resultOwner);
				readEmpiresEntityIntProperty(pBuilding, "m_iHealth", resultHealth);
				readEmpiresEntityIntProperty(pBuilding, "m_iMaxHealth", resultMaxHealth);
			}
		}

		m_iEmpiresEngineerBuildActionResultIndex = resultIndex;
		if (resultBuilding != nullptr && logger != nullptr && engine != nullptr &&
			gpGlobals != nullptr)
		{
			// Observe the same view-ray target while the normal Engineer Primary
			// path is running after placement.  This is deliberately diagnostic:
			// it does not resolve a target for the game, call Repair(), or alter
			// the command.  The current kit remains the only authority.
			const Vector eye = getEyePosition();
			Vector viewForward;
			AngleVectors(m_vViewAngles, &viewForward);
			const Vector viewRayEnd = eye + viewForward * 8192.0f;
			CEmpiresEngineerShotTraceFilter engineerShotFilter(
				m_pEdict->GetIServerEntity());
			CBotGlobals::traceLine(eye, viewRayEnd, MASK_SHOT,
				&engineerShotFilter);
			const trace_t* trace = CBotGlobals::getTraceResult();
			edict_t* traceHit = trace != nullptr && trace->m_pEnt != nullptr &&
				servergameents != nullptr ?
				servergameents->BaseEntityToEdict(trace->m_pEnt) : nullptr;
			const int traceHitIndex = traceHit != nullptr && !traceHit->IsFree() ?
				engine->IndexOfEdict(traceHit) : -1;
			const char* traceHitClass = traceHit != nullptr &&
				traceHit->GetClassName() != nullptr ?
				traceHit->GetClassName() : "<world/none>";
			const int traceHitTeam = traceHit != nullptr && !traceHit->IsFree() ?
				CClassInterface::getTeam(traceHit) : -1;
			int traceHitHealth = -1;
			int traceHitMaxHealth = -1;
			bool traceHitFullyBuilt = false;
			const bool traceHitHealthKnown = traceHit != nullptr &&
				readEmpiresEntityIntProperty(traceHit, "m_iHealth",
					traceHitHealth);
			const bool traceHitMaxHealthKnown = traceHit != nullptr &&
				readEmpiresEntityIntProperty(traceHit, "m_iMaxHealth",
					traceHitMaxHealth);
			const bool traceHitFullyBuiltKnown = traceHit != nullptr &&
				readEmpiresEntityBoolProperty(traceHit, "m_bFullyBuilt",
					traceHitFullyBuilt);

			int directOwnerIndex = -1;
			if (traceHit != nullptr && !traceHit->IsFree())
			{
				edict_t* pOwner = CClassInterface::getOwner(traceHit);
				if (pOwner != nullptr && !pOwner->IsFree())
					directOwnerIndex = engine->IndexOfEdict(pOwner);
			}
			int ownerHandle = -1;
			int moveParent = -1;
			int parent = -1;
			const bool ownerHandleKnown = traceHit != nullptr &&
				readEmpiresWeaponHandleProperty(traceHit, "m_hOwner",
					ownerHandle);
			const bool moveParentKnown = traceHit != nullptr &&
				readEmpiresWeaponHandleProperty(traceHit, "m_hMoveParent",
					moveParent);
			const bool parentKnown = traceHit != nullptr &&
				readEmpiresWeaponHandleProperty(traceHit, "m_hParent", parent);

			int infoOwner = -1;
			int infoParent = -1;
			int infoMoveParent = -1;
			int infoRootMoveParent = -1;
			bool entityInfoKnown = false;
			if (entityinfomanager != nullptr && traceHit != nullptr &&
				!traceHit->IsFree())
			{
				IEntityInfo* pInfo = entityinfomanager->GetEntityInfo(traceHit);
				if (pInfo != nullptr)
				{
					auto relationIndex = [](edict_t* pEntity) -> int
					{
						if (pEntity == nullptr || pEntity->IsFree() ||
							engine == nullptr)
							return -1;
						return engine->IndexOfEdict(pEntity);
					};
					infoOwner = relationIndex(pInfo->GetOwner());
					infoParent = relationIndex(pInfo->GetParent());
					infoMoveParent = relationIndex(pInfo->GetMoveParent());
					infoRootMoveParent = relationIndex(pInfo->GetRootMoveParent());
					entityInfoKnown = true;
				}
			}

			bool buildingFullyBuilt = false;
			const bool buildingFullyBuiltKnown = readEmpiresEntityBoolProperty(
				resultBuilding, "m_bFullyBuilt", buildingFullyBuilt);
			int engineerBuildItem = -1;
			int engineerBuildState = -1;
			int turretsBuilt = -1;
			int turretsAllotted = -1;
			const bool engineerBuildItemKnown = readEmpiresDebugIntProperty(
				m_pEdict, "m_iEngineerBuildItem", engineerBuildItem);
			const bool engineerBuildStateKnown = readEmpiresDebugIntProperty(
				m_pEdict, "m_iEngBuild", engineerBuildState);
			const bool turretsBuiltKnown = readEmpiresDebugIntProperty(
				m_pEdict, "m_iEngineerTurretsBuilt", turretsBuilt);
			const bool turretsAllottedKnown = readEmpiresDebugIntProperty(
				m_pEdict, "m_iEngineerTurretsAllotted", turretsAllotted);
			const edict_t* pCurrentWeapon = CClassInterface::getCurrentWeapon(
				m_pEdict);
			int kitClip = -1;
			float playerNextAttack = 0.0f;
			float weaponNextPrimary = 0.0f;
			const bool kitClipKnown = pCurrentWeapon != nullptr &&
				!pCurrentWeapon->IsFree() && readEmpiresEntityIntProperty(
					pCurrentWeapon, "m_iClip1", kitClip);
			const bool playerNextAttackKnown = readEmpiresFloatProperty(
				m_pEdict, "m_flNextAttack", playerNextAttack);
			const bool weaponNextPrimaryKnown = pCurrentWeapon != nullptr &&
				!pCurrentWeapon->IsFree() && readEmpiresWeaponFloatProperty(
					pCurrentWeapon, "m_flNextPrimaryAttack", weaponNextPrimary);
			int playerButtons = -1;
			int playerButtonPressed = -1;
			const bool playerButtonsKnown = readEmpiresDebugIntProperty(
				m_pEdict, "m_nButtons", playerButtons);
			const bool playerButtonPressedKnown = readEmpiresDebugIntProperty(
				m_pEdict, "m_afButtonPressed", playerButtonPressed);

			const int resultBuildingTeam = CClassInterface::getTeam(resultBuilding);
			const bool directLogicalTarget = traceHitIndex == resultIndex;
			const bool relatedLogicalTarget = directOwnerIndex == resultIndex ||
				ownerHandle == resultIndex || moveParent == resultIndex ||
				parent == resultIndex || infoOwner == resultIndex ||
				infoParent == resultIndex || infoMoveParent == resultIndex ||
				infoRootMoveParent == resultIndex;
			const char* resolution = directLogicalTarget ? "direct" :
				relatedLogicalTarget ? "relationship" : "unresolved";
			const int effectiveHealth = traceHitIndex == resultIndex &&
				traceHitHealthKnown ? traceHitHealth : resultHealth;
			const int effectiveMaxHealth = traceHitIndex == resultIndex &&
				traceHitMaxHealthKnown ? traceHitMaxHealth : resultMaxHealth;
			const int effectiveFullyBuilt = traceHitIndex == resultIndex &&
				traceHitFullyBuiltKnown ? (traceHitFullyBuilt ? 1 : 0) :
				buildingFullyBuiltKnown ? buildingFullyBuilt : -1;
			const bool traceStateChanged =
				m_iEmpiresEngineerBuildActionLastTraceIndex != traceHitIndex ||
				m_iEmpiresEngineerBuildActionLastTraceOwner != ownerHandle ||
				m_iEmpiresEngineerBuildActionLastTraceMoveParent != moveParent ||
				m_iEmpiresEngineerBuildActionLastTraceParent != parent ||
				m_iEmpiresEngineerBuildActionLastTraceInfoOwner != infoOwner ||
				m_iEmpiresEngineerBuildActionLastTraceInfoParent != infoParent ||
				m_iEmpiresEngineerBuildActionLastTraceInfoMoveParent != infoMoveParent ||
				m_iEmpiresEngineerBuildActionLastTraceInfoRootMoveParent != infoRootMoveParent ||
				m_iEmpiresEngineerBuildActionLastTraceHealth != effectiveHealth ||
				m_iEmpiresEngineerBuildActionLastTraceMaxHealth != effectiveMaxHealth ||
				m_iEmpiresEngineerBuildActionLastTraceFullyBuilt != effectiveFullyBuilt;
			if (traceStateChanged)
			{
				logger->Log(LogLevel::INFO,
					"Empires engineer build=post-placement-target curtime=%.3f index=%d userid=%d building_index=%d building_team=%d building_health=%d building_max_health=%d building_fully_built_known=%d building_fully_built=%d trace_fraction=%.3f trace_distance=%.1f trace_hit_index=%d trace_hit_class=%s trace_hit_team=%d trace_hit_health_known=%d trace_hit_health=%d trace_hit_max_known=%d trace_hit_max=%d trace_hit_fully_built_known=%d trace_hit_fully_built=%d direct_owner_index=%d owner_handle_known=%d owner_handle=%d move_parent_known=%d move_parent=%d parent_known=%d parent=%d entityinfo_known=%d entityinfo_owner=%d entityinfo_parent=%d entityinfo_move_parent=%d entityinfo_root_move_parent=%d logical_target_index=%d resolution=%s build_item_known=%d build_item=%d build_state_known=%d build_state=%d turrets_built_known=%d turrets_built=%d turrets_allotted_known=%d turrets_allotted=%d kit_clip_known=%d kit_clip=%d player_next_attack_known=%d player_next_attack=%.3f weapon_next_primary_known=%d weapon_next_primary=%.3f player_buttons_known=%d player_buttons=0x%x player_pressed_known=%d player_pressed=0x%x desired_buttons=0x%x",
					now, engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
					resultIndex, resultBuildingTeam, resultHealth, resultMaxHealth,
					buildingFullyBuiltKnown ? 1 : 0, buildingFullyBuilt ? 1 : 0,
					trace != nullptr ? trace->fraction : -1.0f,
					traceHit != nullptr ? distanceFrom(traceHit) : -1.0f,
					traceHitIndex, traceHitClass, traceHitTeam,
					traceHitHealthKnown ? 1 : 0, traceHitHealth,
					traceHitMaxHealthKnown ? 1 : 0, traceHitMaxHealth,
					traceHitFullyBuiltKnown ? 1 : 0, traceHitFullyBuilt ? 1 : 0,
					directOwnerIndex, ownerHandleKnown ? 1 : 0, ownerHandle,
					moveParentKnown ? 1 : 0, moveParent, parentKnown ? 1 : 0, parent,
					entityInfoKnown ? 1 : 0, infoOwner, infoParent, infoMoveParent,
					infoRootMoveParent, resultIndex, resolution,
					engineerBuildItemKnown ? 1 : 0, engineerBuildItem,
					engineerBuildStateKnown ? 1 : 0, engineerBuildState,
					turretsBuiltKnown ? 1 : 0, turretsBuilt,
					turretsAllottedKnown ? 1 : 0, turretsAllotted,
					kitClipKnown ? 1 : 0, kitClip,
					playerNextAttackKnown ? 1 : 0, playerNextAttack,
					weaponNextPrimaryKnown ? 1 : 0, weaponNextPrimaryKnown ? weaponNextPrimary : 0.0f,
					playerButtonsKnown ? 1 : 0, playerButtons,
					playerButtonPressedKnown ? 1 : 0, playerButtonPressed, m_iButtons);
			}
			m_iEmpiresEngineerBuildActionLastTraceIndex = traceHitIndex;
			m_iEmpiresEngineerBuildActionLastTraceOwner = ownerHandle;
			m_iEmpiresEngineerBuildActionLastTraceMoveParent = moveParent;
			m_iEmpiresEngineerBuildActionLastTraceParent = parent;
			m_iEmpiresEngineerBuildActionLastTraceInfoOwner = infoOwner;
			m_iEmpiresEngineerBuildActionLastTraceInfoParent = infoParent;
			m_iEmpiresEngineerBuildActionLastTraceInfoMoveParent = infoMoveParent;
			m_iEmpiresEngineerBuildActionLastTraceInfoRootMoveParent = infoRootMoveParent;
			m_iEmpiresEngineerBuildActionLastTraceHealth = effectiveHealth;
			m_iEmpiresEngineerBuildActionLastTraceMaxHealth = effectiveMaxHealth;
			m_iEmpiresEngineerBuildActionLastTraceFullyBuilt = effectiveFullyBuilt;
		}
		if (m_bEmpiresEngineerBuildActionRequireFullHealth)
		{
			if (resultIndex < 0)
			{
				if (now >= m_flEmpiresEngineerBuildActionDeadline)
					failEmpiresEngineerBuildAction(
						"new-mg-turret-index-unavailable");
				return;
			}

			if (resultHealth >= 0 && resultMaxHealth > 0 &&
				resultHealth >= resultMaxHealth)
			{
				m_pButtons->letGo(IN_ATTACK);
				m_iButtons &= ~IN_ATTACK;
				if (logger != nullptr)
					logger->Log(LogLevel::INFO,
						"Empires engineer build=result=PASS index=%d userid=%d building_index=%d building_class=%s building_team=%d owner_index=%d health=%d max_health=%d target_waypoint=%d baseline_turrets=%d current_turrets=%d baseline_max_index=%d current_max_index=%d frames=%d input=normal-primary-attack full_mode=1",
						engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
						resultIndex, resultClassName, resultTeam, resultOwner,
						resultHealth, resultMaxHealth,
						m_iEmpiresEngineerBuildActionTargetWaypoint,
						m_iEmpiresEngineerBuildActionBaselineTurrets, turretCount,
						m_iEmpiresEngineerBuildActionBaselineMaxIndex, highestIndex,
						m_iEmpiresEngineerBuildActionFrames);
				m_eEmpiresEngineerBuildActionState =
					EmpiresEngineerBuildActionState::COMPLETE;
				return;
			}

			if (now >= m_flEmpiresEngineerBuildActionDeadline)
			{
				failEmpiresEngineerBuildAction(
					"building-health-did-not-reach-max");
				return;
			}

			if (!m_bEmpiresEngineerBuildActionResultObservedLogged &&
				logger != nullptr)
			{
				logger->Log(LogLevel::INFO,
					"Empires engineer build=placed index=%d userid=%d building_index=%d building_class=%s building_team=%d owner_index=%d health=%d max_health=%d full_mode=1",
					engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
					resultIndex, resultClassName, resultTeam, resultOwner,
					resultHealth, resultMaxHealth);
				m_bEmpiresEngineerBuildActionResultObservedLogged = true;
			}

			// Empires clears the selected build item after placement. Keep that
			// zero value: in the current Engineer-kit binary the nonzero item
			// path is the placement branch, while the zero-item path resolves a
			// friendly building and calls its normal Repair() path. Re-selecting
			// item 1 here would attempt another placement and prevent construction
			// from reaching CEmpBuilding::Repair().
			if (buildItem != 0)
			{
				m_pButtons->letGo(IN_ATTACK);
				m_iButtons &= ~IN_ATTACK;
				return;
			}

			if (!m_bEmpiresEngineerBuildActionCompletionCommandRequested &&
				logger != nullptr)
			{
				logger->Log(LogLevel::INFO,
					"Empires engineer build=completion-selection-suppressed index=%d userid=%d build_item=%d path=normal-repair",
					engine->IndexOfEdict(m_pEdict),
					engine->GetPlayerUserId(m_pEdict), buildItem);
			}
			m_bEmpiresEngineerBuildActionCompletionCommandRequested = true;
			setLookVector(m_vEmpiresEngineerBuildActionTarget);
			setLookAtTask(LOOK_VECTOR);
			doLook();
			primaryAttack(true, 0.35f);
			return;
		}
		m_pButtons->letGo(IN_ATTACK);
		m_iButtons &= ~IN_ATTACK;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer build=result=PASS index=%d userid=%d building_index=%d building_class=%s building_team=%d owner_index=%d health=%d max_health=%d target_waypoint=%d baseline_turrets=%d current_turrets=%d baseline_max_index=%d current_max_index=%d frames=%d input=normal-primary-attack",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				resultIndex, resultClassName, resultTeam, resultOwner, resultHealth,
				resultMaxHealth, m_iEmpiresEngineerBuildActionTargetWaypoint,
				m_iEmpiresEngineerBuildActionBaselineTurrets, turretCount,
				m_iEmpiresEngineerBuildActionBaselineMaxIndex, highestIndex,
				m_iEmpiresEngineerBuildActionFrames);
		m_eEmpiresEngineerBuildActionState =
			EmpiresEngineerBuildActionState::COMPLETE;
		return;
	}

	if (now >= m_flEmpiresEngineerBuildActionDeadline)
	{
		int built = -1, allotted = -1, item = -1, commander = -1, clip = -1;
		const bool builtKnown = readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerTurretsBuilt", built);
		const bool allottedKnown = readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerTurretsAllotted", allotted);
		const bool itemKnown = readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerBuildItem", item);
		const bool commanderKnown = readEmpiresEntityIntProperty(m_pEdict, "m_bCommander", commander);
		const bool clipKnown = readEmpiresEntityIntProperty(pCurrentWeapon, "m_iClip1", clip);
		RCBOT_DIAG("[RCBOT2] turret-build-failure userid=%d built=%d/%d allotted=%d/%d item=%d/%d commander=%d/%d clip=%d/%d\n",
			engine->GetPlayerUserId(m_pEdict), builtKnown ? 1 : 0, built,
			allottedKnown ? 1 : 0, allotted, itemKnown ? 1 : 0, item,
			commanderKnown ? 1 : 0, commander, clipKnown ? 1 : 0, clip);
		failEmpiresEngineerBuildAction("no-new-mg-turret-observed");
	}
}

void CBotEmpires::failEmpiresEngineerRepairAction(const char* reason)
{
	if (!isEmpiresEngineerRepairActionActive())
		return;

	if (m_pButtons != nullptr)
	{
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_FORWARD);
		m_pButtons->letGo(IN_BACK);
		m_pButtons->letGo(IN_MOVELEFT);
		m_pButtons->letGo(IN_MOVERIGHT);
		m_pButtons->letGo(IN_USE);
	}
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	wantToShoot(false);
	m_pLookEdict = nullptr;
	setLookAtTask(LOOK_NONE);

	if (logger != nullptr)
	{
		logger->Log(LogLevel::WARN,
			"Empires engineer repair=result=FAIL index=%d userid=%d building_index=%d health_before=%d health_last=%d max_health=%d frames=%d distance=%.1f reason=%s",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			m_iEmpiresEngineerRepairActionBuildingIndex,
			m_iEmpiresEngineerRepairActionHealthBefore,
			m_iEmpiresEngineerRepairActionHealthLast,
			m_iEmpiresEngineerRepairActionMaxHealth,
			m_iEmpiresEngineerRepairActionFrames,
			m_pEmpiresEngineerRepairActionBuilding != nullptr ?
				distanceFrom(const_cast<edict_t*>(m_pEmpiresEngineerRepairActionBuilding)) : -1.0f,
			reason != nullptr ? reason : "unspecified");
	}
	m_eEmpiresEngineerRepairActionState =
		EmpiresEngineerRepairActionState::FAILED;
	m_bEmpiresEngineerRepairActionRequireFullHealth = false;
}

void CBotEmpires::handleEmpiresEngineerRepairAction()
{
	if (!isEmpiresEngineerRepairActionActive())
		return;

	if (engine == nullptr || gpGlobals == nullptr || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr || m_pEmpiresEngineerRepairActionBuilding == nullptr)
	{
		failEmpiresEngineerRepairAction("runtime-prerequisite-invalid");
		return;
	}

	++m_iEmpiresEngineerRepairActionFrames;
	const float now = gpGlobals->curtime;
	if (engine->IndexOfEdict(m_pEdict) !=
		m_iEmpiresEngineerRepairActionExpectedIndex ||
		engine->GetPlayerUserId(m_pEdict) !=
		m_iEmpiresEngineerRepairActionExpectedUserId ||
		m_pEdict != m_pEmpiresEngineerRepairActionExpectedEdict)
	{
		failEmpiresEngineerRepairAction("client-identity-changed");
		return;
	}

	const int classId = [&]()
	{
		int id = -1;
		bool desired = false;
		bool desiredKnown = false;
		const char* propertyName = nullptr;
		return readEmpiresClass(m_pEdict, id, desired, desiredKnown,
			propertyName) ? id : -1;
	}();
	if (classId != EMP_CLASS_ENGINEER || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0 ||
		m_pPlayerInfo->GetTeamIndex() != m_iEmpiresEngineerRepairActionTeam)
	{
		failEmpiresEngineerRepairAction("bot-no-longer-alive-engineer-or-team-changed");
		return;
	}

	edict_t* pBuilding = const_cast<edict_t*>(
		m_pEmpiresEngineerRepairActionBuilding);
	if (pBuilding->IsFree() || pBuilding->GetClassName() == nullptr ||
		CClassInterface::getTeam(pBuilding) != m_iEmpiresEngineerRepairActionTeam)
	{
		failEmpiresEngineerRepairAction("building-invalid-or-team-changed");
		return;
	}

	int health = -1;
	int maxHealth = -1;
	bool fullyBuilt = false;
	bool disabled = false;
	bool tempDisabled = false;
	bool sabotaged = false;
	bool recycling = false;
	bool damaged = false;
	bool canBeSabotaged = false;
	bool hasOwner = false;
	bool hadOwner = false;
	int teamOwner = -1;
	int teamNumber = -1;
	int disabledValue = -1;
	const bool fullyBuiltKnown = readEmpiresEntityBoolProperty(
		pBuilding, "m_bFullyBuilt", fullyBuilt);
	const bool disabledKnown = readEmpiresEntityBoolProperty(
		pBuilding, "m_bDisabled", disabled);
	const bool tempDisabledKnown = readEmpiresEntityBoolProperty(
		pBuilding, "m_bTempDisabled", tempDisabled);
	const bool sabotagedKnown = readEmpiresEntityBoolProperty(
		pBuilding, "m_bSabotaged", sabotaged);
	const bool recyclingKnown = readEmpiresEntityBoolProperty(
		pBuilding, "m_bRecycling", recycling);
	const bool damagedKnown = readEmpiresEntityBoolProperty(
		pBuilding, "m_bDamaged", damaged);
	const bool canBeSabotagedKnown = readEmpiresEntityBoolProperty(
		pBuilding, "m_bCanBeSabotaged", canBeSabotaged);
	const bool hasOwnerKnown = readEmpiresEntityBoolProperty(
		pBuilding, "m_bHasOwner", hasOwner);
	const bool hadOwnerKnown = readEmpiresEntityBoolProperty(
		pBuilding, "m_bHadOwner", hadOwner);
	const bool teamOwnerKnown = readEmpiresEntityIntProperty(
		pBuilding, "m_iTeamOwner", teamOwner);
	const bool teamNumberKnown = readEmpiresEntityIntProperty(
		pBuilding, "m_iTeamNum", teamNumber);
	const bool disabledValueKnown = readEmpiresEntityIntProperty(
		pBuilding, "m_iDisabled", disabledValue);
	const bool healthKnown = readEmpiresEntityIntProperty(pBuilding,
		"m_iHealth", health);
	const bool maxHealthKnown = readEmpiresEntityIntProperty(pBuilding,
		"m_iMaxHealth", maxHealth);
	if (!healthKnown || !maxHealthKnown)
	{
		failEmpiresEngineerRepairAction("building-health-properties-unavailable");
		return;
	}
	if (health >= maxHealth && !m_bEmpiresEngineerRepairActionRequireFullHealth)
	{
		failEmpiresEngineerRepairAction("building-already-full-health");
		return;
	}

	const char* kitClassName = m_iEmpiresEngineerRepairActionTeam == EMP_TEAM_NF ?
		"weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
	const edict_t* pCurrentWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool kitActive = pCurrentWeapon != nullptr && !pCurrentWeapon->IsFree() &&
		pCurrentWeapon->GetClassName() != nullptr &&
		std::strcmp(pCurrentWeapon->GetClassName(), kitClassName) == 0;
	int kitClip = -1;
	const bool kitClipKnown = pCurrentWeapon != nullptr &&
		readEmpiresEntityIntProperty(pCurrentWeapon, "m_iClip1", kitClip);
	// SetActiveWeapon() for SDK2013 fake clients creates/equips/switches the
	// named weapon.  Repeating that call every think resets the weapon/player
	// attack timers and prevents the Engineer kit from ever reaching its normal
	// PrimaryAttack path.  Select only until the kit is actually active, just
	// like the other bounded Empires action probes.
	if (!kitActive)
		selectWeaponName(kitClassName);

	// The Empires kit uses a normal shot trace.  The generic RCBot2
	// isShotVisible() helper deliberately excludes its destination entity; that
	// is correct for player LOS, but makes every ray terminating on a building
	// collision surface look blocked.  For this explicit Engineer action, test
	// the world/prop line only and let the kit's own normal trace resolve the
	// building hit.  No production target selection or weapon scoring uses it.
	const Vector eye = getEyePosition();
	const Vector centerAim = getAimVector(pBuilding);
	const ICollideable* pBuildingCollision = pBuilding->GetCollideable();
	const Vector surfaceAim = pBuildingCollision != nullptr ?
		pBuildingCollision->GetCollisionOrigin() +
		Vector(0.0f, 0.0f, pBuildingCollision->OBBMaxs().z * 0.5f) :
		centerAim;
	const Vector topAim = pBuildingCollision != nullptr ?
		pBuildingCollision->GetCollisionOrigin() +
		Vector(0.0f, 0.0f, pBuildingCollision->OBBMaxs().z) :
		centerAim;
	const bool centerVisible = CBotGlobals::isVisible(
		m_pEdict, eye, centerAim);
	const bool surfaceVisible = pBuildingCollision != nullptr &&
		CBotGlobals::isVisible(m_pEdict, eye, surfaceAim);
	const bool topVisible = pBuildingCollision != nullptr &&
		CBotGlobals::isVisible(m_pEdict, eye, topAim);
	CEmpiresEngineerShotTraceFilter engineerShotFilter(
		m_pEdict->GetIServerEntity());
	auto traceEngineerShot = [&](const Vector& endpoint, float& fraction) -> edict_t*
	{
		CBotGlobals::traceLine(eye, endpoint, MASK_SHOT, &engineerShotFilter);
		const trace_t* trace = CBotGlobals::getTraceResult();
		fraction = trace != nullptr ? trace->fraction : -1.0f;
		if (trace == nullptr || trace->m_pEnt == nullptr || servergameents == nullptr)
			return nullptr;
		return servergameents->BaseEntityToEdict(trace->m_pEnt);
	};
	float centerShotFraction = -1.0f;
	float surfaceShotFraction = -1.0f;
	float topShotFraction = -1.0f;
	edict_t* centerShotHit = traceEngineerShot(centerAim, centerShotFraction);
	edict_t* surfaceShotHit = traceEngineerShot(surfaceAim, surfaceShotFraction);
	edict_t* topShotHit = traceEngineerShot(topAim, topShotFraction);
	auto ownerOfShotHit = [](edict_t* pHit) -> edict_t*
	{
		if (pHit == nullptr || pHit->IsFree())
			return nullptr;
		return CClassInterface::getOwner(pHit);
	};
	edict_t* centerShotOwner = ownerOfShotHit(centerShotHit);
	edict_t* surfaceShotOwner = ownerOfShotHit(surfaceShotHit);
	edict_t* topShotOwner = ownerOfShotHit(topShotHit);
	int centerShotMoveParent = -1;
	int surfaceShotMoveParent = -1;
	int topShotMoveParent = -1;
	int centerShotParent = -1;
	int surfaceShotParent = -1;
	int topShotParent = -1;
	int centerShotInfoOwner = -1;
	int centerShotInfoParent = -1;
	int centerShotInfoMoveParent = -1;
	int centerShotInfoRootMoveParent = -1;
	int surfaceShotInfoOwner = -1;
	int surfaceShotInfoParent = -1;
	int surfaceShotInfoMoveParent = -1;
	int surfaceShotInfoRootMoveParent = -1;
	int topShotInfoOwner = -1;
	int topShotInfoParent = -1;
	int topShotInfoMoveParent = -1;
	int topShotInfoRootMoveParent = -1;
	auto entityInfoIndex = [](edict_t* pEntity) -> int
	{
		if (pEntity == nullptr || pEntity->IsFree() || engine == nullptr)
			return -1;
		return engine->IndexOfEdict(pEntity);
	};
	auto readEntityInfoRelations = [&](edict_t* pHit, int& owner,
		int& parent, int& moveParent, int& rootMoveParent) -> bool
	{
		if (entityinfomanager == nullptr || pHit == nullptr || pHit->IsFree())
			return false;
		IEntityInfo* pInfo = entityinfomanager->GetEntityInfo(pHit);
		if (pInfo == nullptr)
			return false;
		owner = entityInfoIndex(pInfo->GetOwner());
		parent = entityInfoIndex(pInfo->GetParent());
		moveParent = entityInfoIndex(pInfo->GetMoveParent());
		rootMoveParent = entityInfoIndex(pInfo->GetRootMoveParent());
		return true;
	};
	const bool centerShotInfoKnown = readEntityInfoRelations(centerShotHit,
		centerShotInfoOwner, centerShotInfoParent, centerShotInfoMoveParent,
		centerShotInfoRootMoveParent);
	const bool surfaceShotInfoKnown = readEntityInfoRelations(surfaceShotHit,
		surfaceShotInfoOwner, surfaceShotInfoParent, surfaceShotInfoMoveParent,
		surfaceShotInfoRootMoveParent);
	const bool topShotInfoKnown = readEntityInfoRelations(topShotHit,
		topShotInfoOwner, topShotInfoParent, topShotInfoMoveParent,
		topShotInfoRootMoveParent);
	const bool centerShotMoveParentKnown = centerShotHit != nullptr &&
		readEmpiresWeaponHandleProperty(centerShotHit, "m_hMoveParent",
			centerShotMoveParent);
	const bool surfaceShotMoveParentKnown = surfaceShotHit != nullptr &&
		readEmpiresWeaponHandleProperty(surfaceShotHit, "m_hMoveParent",
			surfaceShotMoveParent);
	const bool topShotMoveParentKnown = topShotHit != nullptr &&
		readEmpiresWeaponHandleProperty(topShotHit, "m_hMoveParent",
			topShotMoveParent);
	const bool centerShotParentKnown = centerShotHit != nullptr &&
		readEmpiresWeaponHandleProperty(centerShotHit, "m_hParent",
			centerShotParent);
	const bool surfaceShotParentKnown = surfaceShotHit != nullptr &&
		readEmpiresWeaponHandleProperty(surfaceShotHit, "m_hParent",
			surfaceShotParent);
	const bool topShotParentKnown = topShotHit != nullptr &&
		readEmpiresWeaponHandleProperty(topShotHit, "m_hParent",
			topShotParent);
	Vector kitAim = m_vEmpiresKitAim;
	const bool kitAimFound = findEmpiresKitAim(pBuilding, eye, kitAim,
		m_iEmpiresEngineerRepairActionFrames > 1);
	m_vEmpiresKitAim = kitAim;
	if (kitAimFound)
	{
		setLookVector(kitAim);
		setLookAtTask(LOOK_VECTOR);
	}
	// A box-top aim can graze over a low building (as_escort turret 292: top shot
	// hit nothing, centre shot hit the turret); take a point whose shot hits it.
	else if (surfaceShotHit == pBuilding || centerShotHit == pBuilding)
	{
		setLookVector(surfaceShotHit == pBuilding ? surfaceAim : centerAim);
		setLookAtTask(LOOK_VECTOR);
	}
	else if (topVisible)
	{
		setLookVector(topAim);
		setLookAtTask(LOOK_VECTOR);
	}
	else if (surfaceVisible)
	{
		setLookVector(surfaceAim);
		setLookAtTask(LOOK_VECTOR);
	}
	else
	{
		lookAtEdict(pBuilding);
		setLookAtTask(LOOK_EDICT);
	}
	const Vector repairAim = kitAimFound ? kitAim :
		(surfaceShotHit == pBuilding ? surfaceAim : (centerShotHit == pBuilding ? centerAim :
		(topVisible ? topAim : (surfaceVisible ? surfaceAim : centerAim))));
	// handleWeapons() runs after the normal think-phase doLook().  Recompute
	// the existing look task here so the same outgoing RunPlayerMove command
	// that carries repair input is also aimed at the building.
	doLook();
	Vector viewForward;
	AngleVectors(m_vViewAngles, &viewForward);
	const Vector viewRayEnd = eye + viewForward * 8192.0f;
	CBotGlobals::traceLine(eye, viewRayEnd, MASK_SHOT, &engineerShotFilter);
	const trace_t* viewRayTrace = CBotGlobals::getTraceResult();
	edict_t* viewRayHit = viewRayTrace != nullptr &&
		viewRayTrace->m_pEnt != nullptr && servergameents != nullptr ?
		servergameents->BaseEntityToEdict(viewRayTrace->m_pEnt) : nullptr;
	const float viewRayFraction = viewRayTrace != nullptr ?
		viewRayTrace->fraction : -1.0f;

	// Debug-only evidence for the first unresolved Engineer boundary: the kit
	// receives a normal attack command, but it is not yet known whether the
	// command's view is actually aimed at the logical building. Sample this at
	// low frequency without changing the command or action state.
	if (logger != nullptr && (m_iEmpiresEngineerRepairActionFrames == 1 ||
		m_iEmpiresEngineerRepairActionFrames % 30 == 0))
	{
		QAngle desiredAngles;
		VectorAngles(repairAim - eye, desiredAngles);
		CBotGlobals::fixFloatAngle(&desiredAngles.x);
		CBotGlobals::fixFloatAngle(&desiredAngles.y);
		const CBotCmd lastCommand = m_pPlayerInfo->GetLastUserCommand();
		auto angleDelta = [](float current, float desired) -> float
		{
			float delta = current - desired;
			while (delta > 180.0f)
				delta -= 360.0f;
			while (delta < -180.0f)
				delta += 360.0f;
			return delta;
		};
		const int flags = CClassInterface::getPlayerFlags(m_pEdict);
		float amountToRepair = 0.0f;
		const bool amountToRepairKnown = readEmpiresDebugFloatProperty(
			m_pEdict, "m_fAmountToRepair", amountToRepair);
		int engineerBuildItem = -1;
		const bool engineerBuildItemKnown = readEmpiresDebugIntProperty(
			m_pEdict, "m_iEngineerBuildItem", engineerBuildItem);
		int engineerBuildState = -1;
		const bool engineerBuildStateKnown = readEmpiresDebugIntProperty(
			m_pEdict, "m_iEngBuild", engineerBuildState);
			int currentKitClip = -1;
			const bool currentKitClipKnown = pCurrentWeapon != nullptr &&
				readEmpiresEntityIntProperty(pCurrentWeapon, "m_iClip1", currentKitClip);
			int shotsFired = -1;
			const bool shotsFiredKnown = readEmpiresDebugIntProperty(
				m_pEdict, "m_iShotsFired", shotsFired);
			// The current Engineer-kit binary consumes a server-side player
			// bookkeeping value during PrimaryAttack, but its public property name
			// is not known yet.  Index only the live datamap once for this bounded
			// diagnostic; this is evidence collection, not a production property
			// lookup and does not read or write by guessed offset.
			if (m_iEmpiresEngineerRepairActionFrames == 1)
			{
				IServerUnknown* pPlayerUnknown = m_pEdict->GetUnknown();
				CBaseEntity* pPlayerEntity = pPlayerUnknown != nullptr ?
					pPlayerUnknown->GetBaseEntity() : nullptr;
				datamap_t* pPlayerMap = pPlayerEntity != nullptr ?
					CBaseEntity_GetDataDescMap(pPlayerEntity) : nullptr;
				int indexedFields = 0;
				for (datamap_t* pMap = pPlayerMap; pMap != nullptr &&
					indexedFields < 128; pMap = pMap->baseMap)
				{
					for (int field = 0; field < pMap->dataNumFields &&
						indexedFields < 128; ++field)
					{
						const typedescription_t& description = pMap->dataDesc[field];
						const int offset = description.fieldOffset[TD_OFFSET_NORMAL];
						if (description.fieldName == nullptr || offset < 0x1000 ||
							offset > 0x1300)
							continue;
						++indexedFields;
						logger->Log(LogLevel::INFO,
							"Empires engineer repair=player-datamap field=%s class=%s type=%d offset=0x%x size=%d flags=0x%x",
							description.fieldName,
							pMap->dataClassName != nullptr ? pMap->dataClassName : "<unknown>",
							description.fieldType, offset,
							description.fieldSizeInBytes, description.flags);
					}
				}
				logger->Log(LogLevel::INFO,
					"Empires engineer repair=player-datamap summary fields=%d map=%s",
					indexedFields,
					pPlayerMap != nullptr && pPlayerMap->dataClassName != nullptr ?
						pPlayerMap->dataClassName : "<unavailable>");

				IServerNetworkable* pPlayerNetworkable = pPlayerUnknown != nullptr ?
					pPlayerUnknown->GetNetworkable() : nullptr;
				const ServerClass* pPlayerClass = pPlayerNetworkable != nullptr ?
					pPlayerNetworkable->GetServerClass() : nullptr;
				int indexedSendProps = 0;
				auto indexSendTable = [&](auto&& self, SendTable* pTable,
					const int baseOffset) -> void
				{
					if (pTable == nullptr || indexedSendProps >= 128)
						return;
					for (int propIndex = 0; propIndex < pTable->GetNumProps() &&
						indexedSendProps < 128; ++propIndex)
					{
						SendProp* pProp = pTable->GetProp(propIndex);
						if (pProp == nullptr)
							continue;
						const int absoluteOffset = baseOffset +
							std::abs(pProp->GetOffset());
						if (pProp->GetDataTable() != nullptr)
						{
							self(self, pProp->GetDataTable(), absoluteOffset);
							continue;
						}
						if (pProp->GetName() == nullptr || absoluteOffset < 0x1000 ||
							absoluteOffset > 0x1300)
							continue;
						++indexedSendProps;
						logger->Log(LogLevel::INFO,
							"Empires engineer repair=player-sendtable field=%s table=%s type=%d offset=0x%x flags=0x%x",
							pProp->GetName(),
							pTable->GetName() != nullptr ? pTable->GetName() : "<unknown>",
							pProp->GetType(), absoluteOffset, pProp->GetFlags());
					}
				};
				if (pPlayerClass != nullptr)
					indexSendTable(indexSendTable, pPlayerClass->m_pTable, 0);
				logger->Log(LogLevel::INFO,
					"Empires engineer repair=player-sendtable summary fields=%d class=%s",
					indexedSendProps,
					pPlayerClass != nullptr && pPlayerClass->m_pNetworkName != nullptr ?
						pPlayerClass->m_pNetworkName : "<unavailable>");
			}
		const int buildingCollisionGroup = pBuildingCollision != nullptr ?
			pBuildingCollision->GetCollisionGroup() : -1;
		const int buildingSolid = pBuildingCollision != nullptr ?
			pBuildingCollision->GetSolid() : -1;
		const int buildingSolidFlags = pBuildingCollision != nullptr ?
			pBuildingCollision->GetSolidFlags() : -1;
		logger->Log(LogLevel::INFO,
			"Empires engineer repair=aim-telemetry frame=%d index=%d userid=%d target_index=%d target_class=%s scheduled_view=(%.2f %.2f %.2f) lastcmd_view=(%.2f %.2f %.2f) desired_view=(%.2f %.2f %.2f) view_error=(%.2f %.2f) lastcmd_buttons=0x%x flags=0x%x on_ground=%d ducking=%d fakeclient_identity=%d teamnum_known=%d teamnum=%d viewray_fraction=%.3f viewray_index=%d viewray_class=%s building_collision_group=%d building_solid=%d building_solid_flags=0x%x shots_fired_known=%d shots_fired=%d amount_to_repair_known=%d amount_to_repair=%.3f engineer_build_item_known=%d engineer_build_item=%d engineer_build_state_known=%d engineer_build_state=%d kit_clip_known=%d kit_clip=%d",
			m_iEmpiresEngineerRepairActionFrames, engine->IndexOfEdict(m_pEdict),
			engine->GetPlayerUserId(m_pEdict),
			m_iEmpiresEngineerRepairActionBuildingIndex,
			pBuilding->GetClassName() != nullptr ? pBuilding->GetClassName() : "<unknown>",
			m_vViewAngles.x, m_vViewAngles.y, m_vViewAngles.z,
			lastCommand.viewangles.x, lastCommand.viewangles.y,
			lastCommand.viewangles.z, desiredAngles.x, desiredAngles.y,
			desiredAngles.z, angleDelta(m_vViewAngles.x, desiredAngles.x),
			angleDelta(m_vViewAngles.y, desiredAngles.y), lastCommand.buttons,
			flags, (flags & FL_ONGROUND) != 0 ? 1 : 0,
			(flags & FL_DUCKING) != 0 ? 1 : 0,
			m_pPlayerInfo->IsFakeClient() ? 1 : 0,
			teamNumberKnown ? 1 : 0, teamNumber, viewRayFraction,
			viewRayHit != nullptr ? engine->IndexOfEdict(viewRayHit) : -1,
			viewRayHit != nullptr && viewRayHit->GetClassName() != nullptr ?
				viewRayHit->GetClassName() : "<world/none>",
			buildingCollisionGroup, buildingSolid, buildingSolidFlags,
			shotsFiredKnown ? 1 : 0, shotsFired,
			amountToRepairKnown ? 1 : 0, amountToRepair,
			engineerBuildItemKnown ? 1 : 0, engineerBuildItem,
			engineerBuildStateKnown ? 1 : 0, engineerBuildState,
			currentKitClipKnown ? 1 : 0, currentKitClip);
	}

	// The probe must not combine repair with navigation or an unrelated held
	// action.  The regular RunPlayerMove boundary still delivers the command.
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons &= ~(IN_ATTACK | IN_ATTACK2 | IN_FORWARD | IN_BACK |
		IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK);
	m_pButtons->letGo(IN_ATTACK2);
	m_pButtons->letGo(IN_FORWARD);
	m_pButtons->letGo(IN_BACK);
	m_pButtons->letGo(IN_MOVELEFT);
	m_pButtons->letGo(IN_MOVERIGHT);
	m_pButtons->letGo(IN_USE);
	// Clear button objects as well as m_iButtons.  The generic button
	// scheduler is the source of the final RunPlayerMove mask; leaving a
	// previously scheduled jump/duck/speed state alive can submit e.g.
	// IN_ATTACK|IN_JUMP or IN_ATTACK|IN_JUMP|IN_DUCK during this repair-only
	// action and changes the Empires engineer-kit posture/input path.
	m_pButtons->letGo(IN_DUCK);
	m_pButtons->letGo(IN_JUMP);
	m_pButtons->letGo(IN_SPEED);
	m_pButtons->letGo(IN_WALK);
	m_pButtons->letGo(IN_RUN);
	m_pButtons->letGo(IN_ALT1);
	m_pButtons->letGo(IN_RELOAD);

	// The kit reaches only 100 from the eye (CWeaponNFEngineerKit::PrimaryAttack);
	// support picks buildings up to 400 away and goal-roam arrives 300 from the
	// origin. Walk straight at the aim until the view ray hits within reach
	// (as_escort turret 292: kit used from 290 away, health never rose).
	const float viewHitDistance = viewRayFraction >= 0.0f ? viewRayFraction * 8192.0f : 0.0f;
	if (viewHitDistance > 85.0f && (repairAim - eye).Length2D() > 40.0f)
	{
		m_fForwardSpeed = 250.0f;
		// Keep the action alive while the distance shrinks.
		if (m_flEmpiresEngineerRepairApproachLast <= 0.0f ||
			viewHitDistance < m_flEmpiresEngineerRepairApproachLast - 1.0f)
			m_flEmpiresEngineerRepairActionDeadline =
				(std::max)(m_flEmpiresEngineerRepairActionDeadline, now + 3.0f);
		m_flEmpiresEngineerRepairApproachLast = viewHitDistance;
	}
	else
		m_flEmpiresEngineerRepairApproachLast = 0.0f;

	if (!kitActive)
	{
		if (now >= m_flEmpiresEngineerRepairActionDeadline)
			failEmpiresEngineerRepairAction("engineer-kit-selection-timeout");
		return;
	}

	if (m_eEmpiresEngineerRepairActionState ==
		EmpiresEngineerRepairActionState::WAITING_FOR_ACTIVE)
	{
		m_eEmpiresEngineerRepairActionState =
			EmpiresEngineerRepairActionState::REPAIRING;
		if (logger != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires engineer repair=aim index=%d userid=%d building_index=%d eye=(%.1f,%.1f,%.1f) center=(%.1f,%.1f,%.1f) surface=(%.1f,%.1f,%.1f) top=(%.1f,%.1f,%.1f) view=(%.1f,%.1f,%.1f) center_visible=%d surface_visible=%d top_visible=%d selected=%s center_shot_fraction=%.3f center_shot_index=%d center_shot_class=%s surface_shot_fraction=%.3f surface_shot_index=%d surface_shot_class=%s top_shot_fraction=%.3f top_shot_index=%d top_shot_class=%s",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresEngineerRepairActionBuildingIndex,
				eye.x, eye.y, eye.z, centerAim.x, centerAim.y, centerAim.z,
				surfaceAim.x, surfaceAim.y, surfaceAim.z,
				topAim.x, topAim.y, topAim.z,
				m_vViewAngles.x, m_vViewAngles.y, m_vViewAngles.z,
				centerVisible ? 1 : 0, surfaceVisible ? 1 : 0,
				topVisible ? 1 : 0,
				topVisible ? "top" : (surfaceVisible ? "surface" : "center"),
				centerShotFraction,
				centerShotHit != nullptr ? engine->IndexOfEdict(centerShotHit) : -1,
				centerShotHit != nullptr && centerShotHit->GetClassName() != nullptr ?
					centerShotHit->GetClassName() : "<world/none>",
				surfaceShotFraction,
				surfaceShotHit != nullptr ? engine->IndexOfEdict(surfaceShotHit) : -1,
				surfaceShotHit != nullptr && surfaceShotHit->GetClassName() != nullptr ?
					surfaceShotHit->GetClassName() : "<world/none>",
				topShotFraction,
				topShotHit != nullptr ? engine->IndexOfEdict(topShotHit) : -1,
				topShotHit != nullptr && topShotHit->GetClassName() != nullptr ?
					topShotHit->GetClassName() : "<world/none>");
			logger->Log(LogLevel::INFO,
				"Empires engineer repair=proxy center_hit=%d owner=%d moveparent_known=%d moveparent=%d parent_known=%d parent=%d entityinfo_known=%d entityinfo_owner=%d entityinfo_moveparent=%d entityinfo_parent=%d entityinfo_root=%d surface_hit=%d owner=%d moveparent_known=%d moveparent=%d parent_known=%d parent=%d entityinfo_known=%d entityinfo_owner=%d entityinfo_moveparent=%d entityinfo_parent=%d entityinfo_root=%d top_hit=%d owner=%d moveparent_known=%d moveparent=%d parent_known=%d parent=%d entityinfo_known=%d entityinfo_owner=%d entityinfo_moveparent=%d entityinfo_parent=%d entityinfo_root=%d",
				centerShotHit != nullptr ? engine->IndexOfEdict(centerShotHit) : -1,
				centerShotOwner != nullptr ? engine->IndexOfEdict(centerShotOwner) : -1,
				centerShotMoveParentKnown ? 1 : 0, centerShotMoveParent,
				centerShotParentKnown ? 1 : 0, centerShotParent,
				centerShotInfoKnown ? 1 : 0, centerShotInfoOwner,
				centerShotInfoMoveParent, centerShotInfoParent,
				centerShotInfoRootMoveParent,
				surfaceShotHit != nullptr ? engine->IndexOfEdict(surfaceShotHit) : -1,
				surfaceShotOwner != nullptr ? engine->IndexOfEdict(surfaceShotOwner) : -1,
				surfaceShotMoveParentKnown ? 1 : 0, surfaceShotMoveParent,
				surfaceShotParentKnown ? 1 : 0, surfaceShotParent,
				surfaceShotInfoKnown ? 1 : 0, surfaceShotInfoOwner,
				surfaceShotInfoMoveParent, surfaceShotInfoParent,
				surfaceShotInfoRootMoveParent,
				topShotHit != nullptr ? engine->IndexOfEdict(topShotHit) : -1,
				topShotOwner != nullptr ? engine->IndexOfEdict(topShotOwner) : -1,
				topShotMoveParentKnown ? 1 : 0, topShotMoveParent,
				topShotParentKnown ? 1 : 0, topShotParent,
				topShotInfoKnown ? 1 : 0, topShotInfoOwner,
				topShotInfoMoveParent, topShotInfoParent,
				topShotInfoRootMoveParent);
			logger->Log(LogLevel::INFO,
				"Empires engineer repair=started index=%d userid=%d building_index=%d building_class=%s distance=%.1f health=%d max_health=%d fully_built_known=%d fully_built=%d disabled_known=%d disabled=%d temp_disabled_known=%d temp_disabled=%d sabotaged_known=%d sabotaged=%d recycling_known=%d recycling=%d damaged_known=%d damaged=%d can_be_sabotaged_known=%d can_be_sabotaged=%d has_owner_known=%d has_owner=%d had_owner_known=%d had_owner=%d team_owner_known=%d team_owner=%d disabled_value_known=%d disabled_value=%d active_weapon=%s clip1_known=%d clip1=%d",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresEngineerRepairActionBuildingIndex,
				pBuilding->GetClassName(), distanceFrom(pBuilding), health, maxHealth,
				fullyBuiltKnown ? 1 : 0, fullyBuilt ? 1 : 0,
				disabledKnown ? 1 : 0, disabled ? 1 : 0,
				tempDisabledKnown ? 1 : 0, tempDisabled ? 1 : 0,
				sabotagedKnown ? 1 : 0, sabotaged ? 1 : 0,
				recyclingKnown ? 1 : 0, recycling ? 1 : 0,
				damagedKnown ? 1 : 0, damaged ? 1 : 0,
				canBeSabotagedKnown ? 1 : 0, canBeSabotaged ? 1 : 0,
				hasOwnerKnown ? 1 : 0, hasOwner ? 1 : 0,
				hadOwnerKnown ? 1 : 0, hadOwner ? 1 : 0,
				teamOwnerKnown ? 1 : 0, teamOwner,
				disabledValueKnown ? 1 : 0, disabledValue,
				kitClassName,
				kitClipKnown ? 1 : 0, kitClip);
		}
		return;
	}

	if (health > m_iEmpiresEngineerRepairActionHealthLast)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer repair=progress index=%d userid=%d building_index=%d health=%d max_health=%d delta=%d",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresEngineerRepairActionBuildingIndex, health, maxHealth,
				health - m_iEmpiresEngineerRepairActionHealthLast);
		m_iEmpiresEngineerRepairActionHealthLast = health;
		m_bEmpiresEngineerRepairActionProgressLogged = true;
		// Building to full health takes longer than one window; progress keeps it open.
		if (m_bEmpiresEngineerRepairActionRequireFullHealth)
			m_flEmpiresEngineerRepairActionDeadline = now + 12.0f;
	}

	if (health > m_iEmpiresEngineerRepairActionHealthBefore &&
		(!m_bEmpiresEngineerRepairActionRequireFullHealth || health >= maxHealth))
	{
		m_pButtons->letGo(IN_ATTACK);
		m_iButtons &= ~IN_ATTACK;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires engineer repair=result=PASS index=%d userid=%d building_index=%d health_before=%d health_after=%d max_health=%d frames=%d input=normal-primary-attack",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresEngineerRepairActionBuildingIndex,
				m_iEmpiresEngineerRepairActionHealthBefore, health, maxHealth,
				m_iEmpiresEngineerRepairActionFrames);
		m_eEmpiresEngineerRepairActionState =
			EmpiresEngineerRepairActionState::COMPLETE;
		m_bEmpiresEngineerRepairActionRequireFullHealth = false;
		return;
	}

	if (now >= m_flEmpiresEngineerRepairActionDeadline)
	{
		failEmpiresEngineerRepairAction("repair-health-did-not-increase");
		return;
	}

	// The kit's normal primary path is a held attack; the engine controls the
	// actual repair cadence and resource consumption.
	primaryAttack(true, 1.0f);
}
