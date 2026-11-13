// Engineer builds an MG turret at each own refinery (user rule 2026-11-12:
// refineries first, turned sensibly). Only the Engineer with the Repair
// role builds; the Revive one stays with the team (user rule 2026-11-12). The turret goes on the refinery's front
// side: heading = FrontYaw (vehicle route toward the enemy CV), the bot stands
// there facing the front and places with the existing build action
// (requestEmpiresEngineerBuildOnce, Engineer kit primary input), which aims at
// a ground point just ahead (kit trace 100 from the eye).
#include "bot_empires.h"
#include "bot_globals.h"
#include "bot_getprop.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include "bot_cvars.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

bool EmpiresFrontYaw(const Vector& at, int team, float& yaw);

namespace
{
constexpr float kSearchRange = 8000.0f;
constexpr float kStandOut = 330.0f;   // refinery hull reaches ~250 from its origin
constexpr float kTurretNear = 500.0f; // a turret this close to the spot covers the refinery
constexpr float kArrive = 40.0f;

struct Claim
{
	int engineerUserId = -1;
	float until = 0.0f;
};
Claim s_claim[MAX_EDICTS];

bool IsClassPrefix(edict_t* e, const char* prefix, const char* part)
{
	return e != nullptr && !e->IsFree() && e->GetClassName() != nullptr &&
		std::strncmp(e->GetClassName(), prefix, std::strlen(prefix)) == 0 &&
		std::strstr(e->GetClassName(), part) != nullptr;
}

float Dist2D(const Vector& a, const Vector& b)
{
	return (a - b).Length2D();
}

bool GroundBelow(const Vector& p, Vector& ground)
{
	if (enginetrace == nullptr)
		return false;
	CTraceFilterWorldOnly filter;
	trace_t hit;
	Ray_t ray;
	ray.Init(p + Vector(0, 0, 60), p - Vector(0, 0, 200));
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &hit);
	if (hit.fraction >= 1.0f || hit.startsolid)
		return false;
	ground = hit.endpos;
	return true;
}
} // namespace

bool CBotEmpires::handleEmpiresRefineryTurret()
{
	if (gpGlobals == nullptr || engine == nullptr || !rcbot_empires_engineer_turrets.GetBool() ||
		!isEmpiresEngineerClass() || !m_bEmpiresRepairRole || m_iReviveTarget != 0)
	{
		m_iTurretRefinery = 0;
		m_iTurretBuilding = 0;
		m_bTurretBuildArmed = false;
		return false;
	}
	const float now = gpGlobals->curtime;
	const int team = getTeam();
	const int userId = engine->GetPlayerUserId(m_pEdict);

	// Commander-assigned building work comes first; the turret waits.
	if (m_iBuildAssignStage > 0)
		return false;

	// Finishing: the placed turret is remembered until it is full (or gone);
	// build-assign walks to it and builds while health rises.
	if (m_iTurretBuilding > 0)
	{
		edict_t* turret = engine->PEntityOfEntIndex(m_iTurretBuilding);
		int health = -1, maxHealth = -1;
		const bool alive = turret != nullptr && !turret->IsFree() &&
			readEmpiresEntityIntProperty(turret, "m_iHealth", health) &&
			readEmpiresEntityIntProperty(turret, "m_iMaxHealth", maxHealth) && maxHealth > 0;
		const bool full = alive && health >= maxHealth;
		if (alive && !full && now >= m_flTurretNextNav)
		{
			m_flTurretNextNav = now + 2.0f;
			if (requestEmpiresBuildAssignment(turret))
			{
				Msg("[RCBOT2] refinery-turret finish userid=%d turret=%d health=%d/%d\n", userId,
					m_iTurretBuilding, health, maxHealth);
				return true;
			}
			if (++m_iTurretAssignRejects < 5)
				return false;
		}
		else if (alive && !full)
			return false;
		Msg("[RCBOT2] refinery-turret %s userid=%d refinery=%d turret=%d health=%d/%d yaw=%.0f\n",
			full ? "complete" : (alive ? "unfinished reason=assign-rejected" : "failed reason=turret-gone"),
			userId, m_iTurretRefinery, m_iTurretBuilding, health, maxHealth, m_flTurretYaw);
		s_claim[m_iTurretRefinery] = {userId, now + 5.0f};
		m_iTurretBuilding = 0;
		m_iTurretRefinery = 0;
		m_flTurretNextSearch = now + 3.0f;
		return false;
	}

	// Placement running: the build action owns the bot; then hand the new turret to build-assign.
	if (m_bTurretBuildArmed)
	{
		if (isEmpiresEngineerBuildActionActive())
			return true;
		m_bTurretBuildArmed = false;
		edict_t* turret = nullptr;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && turret == nullptr; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (IsClassPrefix(e, "emp_building_", "turret") &&
				Dist2D(CBotGlobals::entityOrigin(e), m_vTurretSpot) < 250.0f)
				turret = e;
		}
		Msg("[RCBOT2] refinery-turret %s userid=%d refinery=%d turret=%d spot=(%.0f %.0f %.0f) yaw=%.0f\n",
			turret != nullptr ? "placed" : "failed", userId, m_iTurretRefinery,
			turret != nullptr ? engine->IndexOfEdict(turret) : -1, m_vTurretSpot.x, m_vTurretSpot.y,
			m_vTurretSpot.z, m_flTurretYaw);
		if (turret != nullptr)
		{
			m_iTurretBuilding = engine->IndexOfEdict(turret);
			m_iTurretAssignRejects = 0;
			m_flTurretNextNav = 0.0f;
			s_claim[m_iTurretRefinery] = {userId, now + 3.0f};
			return handleEmpiresRefineryTurret();
		}
		s_claim[m_iTurretRefinery] = {userId, now + (turret != nullptr ? 5.0f : 60.0f)};
		if (turret == nullptr)
			++m_iTurretAttempt; // next try turns the spot
		m_iTurretRefinery = 0;
		m_flTurretNextSearch = now + 3.0f;
		return false;
	}

	if (m_iTurretRefinery == 0)
	{
		if (now < m_flTurretNextSearch)
			return false;
		m_flTurretNextSearch = now + 2.0f;
		int built = -1, allotted = -1;
		if (readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerTurretsBuilt", built) &&
			readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerTurretsAllotted", allotted) &&
			allotted >= 0 && built >= allotted)
		{
			if (now >= m_flTurretNextLog)
			{
				m_flTurretNextLog = now + 30.0f;
				Msg("[RCBOT2] refinery-turret limit userid=%d built=%d allotted=%d\n", userId, built, allotted);
			}
			m_flTurretNextSearch = now + 10.0f;
			return false;
		}

		const char* cvClass = team == 2 ? "emp_nf_commander" : "emp_imp_commander";
		const char* enemyCvClass = team == 2 ? "emp_imp_commander" : "emp_nf_commander";
		Vector home, enemyHome;
		bool haveHome = false, haveEnemyHome = false;
		std::vector<edict_t*> refineries;
		std::vector<Vector> turrets;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
				continue;
			const char* cls = e->GetClassName();
			if (std::strcmp(cls, "emp_building_refinery") == 0 ||
				IsClassPrefix(e, team == 2 ? "emp_building_nf_" : "emp_building_imp_", "refinery"))
				refineries.push_back(e);
			else if (IsClassPrefix(e, "emp_building_", "turret"))
				turrets.push_back(CBotGlobals::entityOrigin(e));
			else if (!haveHome && std::strcmp(cls, cvClass) == 0)
			{
				home = CBotGlobals::entityOrigin(e);
				haveHome = true;
			}
			else if (!haveEnemyHome && std::strcmp(cls, enemyCvClass) == 0)
			{
				enemyHome = CBotGlobals::entityOrigin(e);
				haveEnemyHome = true;
			}
		}
		float best = kSearchRange;
		int nOurs = 0, nClaimed = 0, nNoYaw = 0, nCovered = 0, nNoMesh = 0, nNoPath = 0;
		for (edict_t* e : refineries)
		{
			const int index = engine->IndexOfEdict(e);
			const Vector at = CBotGlobals::entityOrigin(e);
			const int refTeam = CClassInterface::getTeam(e);
			const bool ours = refTeam == team || (refTeam != 2 && refTeam != 3 && haveHome &&
				(!haveEnemyHome || Dist2D(at, home) < Dist2D(at, enemyHome)));
			if (!ours)
				continue;
			++nOurs;
			// Own failed try: the turned spot is tried at once; a full turn (5 tries) waits the claim out.
			const bool ownRetry = s_claim[index].engineerUserId == userId && m_iTurretAttempt % 5 != 0;
			if (now < s_claim[index].until && !ownRetry)
			{
				++nClaimed;
				continue;
			}
			float yaw = 0.0f;
			if (!EmpiresFrontYaw(at, team, yaw))
			{
				if (!haveEnemyHome)
				{
					++nNoYaw;
					continue;
				}
				yaw = std::atan2(enemyHome.y - at.y, enemyHome.x - at.x) * 180.0f / 3.14159265f;
			}
			// Failed tries turn the spot: 0, +45, -45, +90, -90.
			static const float kTurn[] = {0.0f, 45.0f, -45.0f, 90.0f, -90.0f};
			yaw += kTurn[m_iTurretAttempt % 5];
			const float r = yaw * 3.14159265f / 180.0f;
			const Vector dir(std::cos(r), std::sin(r), 0.0f);
			Vector spot = at + dir * kStandOut;
			bool covered = false;
			for (const Vector& t : turrets)
				covered = covered || Dist2D(t, spot) < kTurretNear || Dist2D(t, at) < kTurretNear;
			if (covered)
			{
				++nCovered;
				continue;
			}
			Vector onMesh;
			if (RCBotNavMesh::IsLoaded())
			{
				if (!RCBotNavMesh::ClosestPointOnMesh(spot, 120.0f, onMesh))
				{
					++nNoMesh;
					continue;
				}
				spot = onMesh;
				// A target on a disconnected/island area can be silently snapped to
				// a distant area by BuildPath. Do not claim a stand point unless the
				// actual route reaches it within the same arrival tolerance used below.
				RCBotNavMesh::Path placementPath;
				const RCBotNavMesh::PathResult placementResult =
					RCBotNavMesh::BuildPath(getOrigin(), spot, placementPath, this);
				const bool placementReachable =
					placementResult == RCBotNavMesh::PathResult::SUCCESS &&
					!placementPath.points.empty() &&
					Dist2D(placementPath.points.back(), spot) <= kArrive &&
					std::fabs(placementPath.points.back().z - spot.z) <= 80.0f;
				if (!placementReachable)
				{
					++nNoPath;
					continue;
				}
			}
			const float d = Dist2D(at, getOrigin());
			if (d < best)
			{
				best = d;
				m_iTurretRefinery = index;
				m_vTurretSpot = spot;
				m_flTurretYaw = yaw;
			}
		}
		if (m_iTurretRefinery == 0)
		{
			if (now >= m_flTurretNextLog)
			{
				m_flTurretNextLog = now + 30.0f;
				Msg("[RCBOT2] refinery-turret none userid=%d refineries=%d ours=%d claimed=%d no_yaw=%d covered=%d no_mesh=%d no_path=%d\n",
					userId, static_cast<int>(refineries.size()), nOurs, nClaimed, nNoYaw, nCovered, nNoMesh, nNoPath);
			}
			return false;
		}
		m_flTurretUntil = now + 60.0f;
		m_flTurretNextNav = 0.0f;
		Msg("[RCBOT2] refinery-turret start userid=%d refinery=%d spot=(%.0f %.0f %.0f) yaw=%.0f dist=%.0f attempt=%d\n",
			userId, m_iTurretRefinery, m_vTurretSpot.x, m_vTurretSpot.y, m_vTurretSpot.z, m_flTurretYaw, best,
			m_iTurretAttempt);
	}
	s_claim[m_iTurretRefinery] = {userId, now + 3.0f};

	edict_t* refinery = engine->PEntityOfEntIndex(m_iTurretRefinery);
	if (refinery == nullptr || refinery->IsFree() || now > m_flTurretUntil)
	{
		Msg("[RCBOT2] refinery-turret end userid=%d refinery=%d reason=%s\n", userId, m_iTurretRefinery,
			now > m_flTurretUntil ? "timeout" : "gone");
		s_claim[m_iTurretRefinery] = {userId, now + 60.0f};
		++m_iTurretAttempt;
		m_iTurretRefinery = 0;
		cancelNavigationAttempt("refinery-turret-end");
		return false;
	}

	const Vector origin = getOrigin();
	const float dist = Dist2D(origin, m_vTurretSpot);
	if (dist > kArrive || std::fabs(origin.z - m_vTurretSpot.z) > 80.0f)
	{
		const bool idle = m_pSchedules == nullptr || m_pSchedules->isEmpty();
		if (now >= m_flTurretNextNav && (idle || now >= m_flTurretNextNav + 10.0f))
		{
			m_flTurretNextNav = now + 3.0f;
			if (m_bEmpiresNavGoalActive)
				endEmpiresNavmeshGoal("drop", "refinery-turret", 0.0f);
			cancelNavigationAttempt("refinery-turret");
			if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
				m_pSchedules->freeMemory();
			if (dist < 200.0f || !requestNavigation(m_vTurretSpot, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
				setMoveTo(m_vTurretSpot);
			Msg("[RCBOT2] refinery-turret walk userid=%d refinery=%d dist=%.0f\n", userId, m_iTurretRefinery, dist);
		}
		if (dist < 200.0f)
			setMoveTo(m_vTurretSpot);
		return true;
	}

	// On the spot: face the front and place on the ground just ahead.
	cancelNavigationAttempt("refinery-turret-place");
	const float r = m_flTurretYaw * 3.14159265f / 180.0f;
	Vector target = origin + Vector(std::cos(r), std::sin(r), 0.0f) * 60.0f;
	Vector ground;
	if (GroundBelow(target, ground))
		target = ground;
	if (requestEmpiresEngineerBuildOnce(-1, &target, false))
	{
		m_bTurretBuildArmed = true;
		Msg("[RCBOT2] refinery-turret place userid=%d refinery=%d target=(%.0f %.0f %.0f)\n", userId,
			m_iTurretRefinery, target.x, target.y, target.z);
		return true;
	}
	Msg("[RCBOT2] refinery-turret end userid=%d refinery=%d reason=build-rejected\n", userId, m_iTurretRefinery);
	s_claim[m_iTurretRefinery] = {userId, now + 30.0f};
	m_iTurretRefinery = 0;
	return false;
}

void EmpiresTurretBuildLevelReset()
{
	for (Claim& c : s_claim)
		c = Claim();
}
