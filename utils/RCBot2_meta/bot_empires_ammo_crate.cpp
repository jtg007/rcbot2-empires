// TODO 3.10b step 3: an engineer answers a vehicle's "I need ammo" call (EmpiresAmmoRequest,
// set by the driver in bot_empires_vehicle.cpp with emp_menu_command 29). He walks to the vehicle,
// selects build item 7 (`engSelect 7` writes m_iEngineerBuildItem +0x128c, Command_engSelect
// 0x64e0e0) with the kit active and fires once: PrimaryAttack -> BuildAmmoCrate 0x3ce830 puts an
// emp_eng_crate at the build location at once (no construction). The crate feeds same-team
// vehicles within 1000 every second (RE vehicle-driving.md "Vehicle ammo"); the driver then picks
// it as its nearest source (step 2). Cooldown: m_flEngineerNextSupply.
#include "bot_empires.h"
#include "bot_globals.h"
#include "bot_getprop.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include "bot_cvars.h"
#include "in_buttons.h"
#include <cmath>
#include <cstdio>
#include <cstring>

int EmpiresVehiclePlayerVehicle(const CBotEmpires* bot, const edict_t* player);

namespace
{
constexpr float kSearchRange = 16000.0f; // same as the repair call; the tank drives home meanwhile
constexpr float kPlaceRange = 450.0f; // from the vehicle's origin; the crate reaches 1000
constexpr int kMaxTries = 3;

float Dist2D(const Vector& a, const Vector& b)
{
	return (a - b).Length2D();
}

bool IsCrate(edict_t* e)
{
	return e != nullptr && !e->IsFree() && e->GetClassName() != nullptr &&
		std::strcmp(e->GetClassName(), "emp_eng_crate") == 0;
}

int CratesNear(const Vector& at, float radius)
{
	int n = 0;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (IsCrate(e) && Dist2D(CBotGlobals::entityOrigin(e), at) < radius)
			++n;
	}
	return n;
}

// A live call whose driver still sits alive in that vehicle (a call outlives an exit or death by up
// to 40 s, and a freed index may belong to another vehicle by then: expert 15.11.).
edict_t* RequestVehicle(const CBotEmpires* bot, const EmpiresAmmoRequest* r)
{
	if (r == nullptr || r->vehicle <= 0 || gpGlobals->curtime > r->until)
		return nullptr;
	edict_t* e = engine->PEntityOfEntIndex(r->vehicle);
	if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr || std::strcmp(e->GetClassName(), "emp_vehicle") != 0)
		return nullptr;
	edict_t* driver = r->driverUserId >= 0 ? CBotGlobals::playerByUserId(r->driverUserId) : nullptr;
	if (driver == nullptr || !CBotGlobals::entityIsAlive(driver) || EmpiresVehiclePlayerVehicle(bot, driver) != r->vehicle)
		return nullptr;
	return e;
}

// The nearest live call that no other engineer holds (one engineer per vehicle).
EmpiresAmmoRequest* OpenRequest(const CBotEmpires* bot, int team, int userId, const Vector& from, float& dist)
{
	EmpiresAmmoRequest* slots = EmpiresAmmoRequests(team);
	EmpiresAmmoRequest* best = nullptr;
	dist = 1.0e9f;
	for (int i = 0; i < kEmpiresAmmoRequestSlots; ++i)
	{
		EmpiresAmmoRequest& r = slots[i];
		edict_t* e = RequestVehicle(bot, &r);
		if (e == nullptr || (r.engineerUserId >= 0 && r.engineerUserId != userId && gpGlobals->curtime <= r.claimUntil))
			continue;
		const float d = Dist2D(from, CBotGlobals::entityOrigin(e));
		if (d < dist)
		{
			dist = d;
			best = &r;
		}
	}
	return best;
}
} // namespace

bool EmpiresEngibotWithinLeash(const edict_t* player, const Vector& at); // bot_empires_engibot.cpp

bool CBotEmpires::handleEmpiresAmmoRequest()
{
	const char* busy = gpGlobals == nullptr || engine == nullptr || m_pPlayerInfo == nullptr || m_pPlayerInfo->IsDead() ||
		!isEmpiresEngineerClass() ? "" : empiresVehicleOwnsBot() ? "vehicle" : m_iReviveTarget != 0 ? "revive" :
		m_iBuildAssignStage > 0 ? "build-assign" : commanderDutyBlocksSideTasks() ? "commander" : m_bTurretBuildArmed || m_iTurretBuilding > 0 ? "turret" : nullptr;
	if (busy != nullptr)
	{
		float openDist = 0.0f;
		if (busy[0] != '\0' && gpGlobals->curtime >= m_flAmmoCrateNextNav + 10.0f &&
			OpenRequest(this, getTeam(), engine->GetPlayerUserId(m_pEdict), getOrigin(), openDist) != nullptr)
		{
			m_flAmmoCrateNextNav = gpGlobals->curtime;
			RCBOT_DIAG("[RCBOT2] ammo-crate busy userid=%d reason=%s\n", engine->GetPlayerUserId(m_pEdict), busy);
		}
		if (m_iAmmoCrateStage != 0)
		{
			EmpiresAmmoRequest* r = EmpiresAmmoRequestForVehicle(getTeam(), m_iAmmoCrateVehicle, false);
			if (r != nullptr && r->engineerUserId == engine->GetPlayerUserId(m_pEdict))
				r->engineerUserId = -1;
			m_iAmmoCrateStage = 0;
			m_iAmmoCrateVehicle = -1;
		}
		return false;
	}
	const float now = gpGlobals->curtime;
	const int team = getTeam();
	const int userId = engine->GetPlayerUserId(m_pEdict);
	EmpiresAmmoRequest* req = m_iAmmoCrateStage != 0 ? EmpiresAmmoRequestForVehicle(team, m_iAmmoCrateVehicle, false) : nullptr;

	auto stop = [&](const char* reason, float pause) {
		RCBOT_DIAG("[RCBOT2] ammo-crate end userid=%d veh=%d reason=%s tries=%d\n", userId, m_iAmmoCrateVehicle, reason,
			m_iAmmoCrateTries);
		if (req != nullptr && req->engineerUserId == userId)
			req->engineerUserId = -1;
		m_iAmmoCrateStage = 0;
		m_iAmmoCrateVehicle = -1;
		m_flAmmoCrateNextSearch = now + pause;
		cancelNavigationAttempt("ammo-crate-end");
		return false;
	};

	if (m_iAmmoCrateStage == 0)
	{
		if (now < m_flAmmoCrateNextSearch)
			return false;
		float dist = 0.0f;
		req = OpenRequest(this, team, userId, getOrigin(), dist);
		if (req == nullptr)
			return false;
		if (commanderOrderHoldsFrom(RequestVehicle(this, req)))
		{
			m_flAmmoCrateNextSearch = now + 5.0f;
			return false; // the commander's order goes first
		}
		if (EmpiresIsEngibot(m_pEdict))
		{
			edict_t* calling = RequestVehicle(this, req);
			if (calling == nullptr || !EmpiresEngibotWithinLeash(m_pEdict, CBotGlobals::entityOrigin(calling)))
			{
				m_flAmmoCrateNextSearch = now + 5.0f;
				return false; // an EngiBot serves the base only
			}
		}
		if (dist > kSearchRange)
		{
			RCBOT_DIAG("[RCBOT2] ammo-crate far userid=%d veh=%d dist=%.0f\n", userId, req->vehicle, dist);
			m_flAmmoCrateNextSearch = now + 5.0f;
			return false;
		}
		float nextSupply = 0.0f;
		if (readEmpiresFloatProperty(m_pEdict, "m_flEngineerNextSupply", nextSupply) && nextSupply > now)
		{
			m_flAmmoCrateNextSearch = now + 2.0f;
			return false; // cooldown: another engineer may take it
		}
		m_iAmmoCrateStage = 1;
		m_iAmmoCrateVehicle = req->vehicle;
		m_iAmmoCrateTries = 0;
		m_flAmmoCrateNextNav = 0.0f;
		m_flAmmoCrateStageAt = now;
		m_flAmmoCrateBestDist = dist;
		m_flAmmoCrateProgressAt = now;
		RCBOT_DIAG("[RCBOT2] ammo-crate start userid=%d veh=%d dist=%.0f\n", userId, req->vehicle, dist);
	}

	// The driver clears the call as soon as it sees the crate: check our result first.
	if (m_iAmmoCrateStage == 4 && CratesNear(getOrigin(), 300.0f) > m_iAmmoCrateBaseline)
	{
		RCBOT_DIAG("[RCBOT2] ammo-crate placed userid=%d veh=%d tries=%d\n", userId, m_iAmmoCrateVehicle, m_iAmmoCrateTries);
		if (req != nullptr && req->engineerUserId == userId)
			req->engineerUserId = -1;
		m_iAmmoCrateStage = 0;
		m_iAmmoCrateVehicle = -1;
		m_flAmmoCrateNextSearch = now + 5.0f;
		return false;
	}
	edict_t* vehicle = RequestVehicle(this, req);
	if (vehicle == nullptr)
		return stop("call-gone", 3.0f);
	if (commanderOrderHoldsFrom(vehicle))
		return stop("order", 5.0f);
	EmpiresAmmoRequest& r = *req;
	if (now - m_flAmmoCrateStageAt > (m_iAmmoCrateStage == 1 ? 120.0f : 6.0f))
		return stop(m_iAmmoCrateStage == 1 ? "walk-timeout" : "place-timeout", 20.0f);
	// Back after death, a repair job or a seat: another engineer may hold the call by now (expert).
	if (r.engineerUserId >= 0 && r.engineerUserId != userId && now <= r.claimUntil)
		return stop("claimed-other", 5.0f);
	r.engineerUserId = userId;
	r.claimUntil = now + 3.0f;

	const Vector origin = getOrigin();
	const Vector at = CBotGlobals::entityOrigin(vehicle);
	const float dist = Dist2D(origin, at);

	if (m_iAmmoCrateStage == 1)
	{
		if (dist > kPlaceRange)
		{
			// No 200 units gained for 20 s (run 002901: navmesh-stuck at 1757 for 100 s while the
			// tank waited): give the call up; another engineer or the pad takes over.
			if (dist < m_flAmmoCrateBestDist - 200.0f)
			{
				m_flAmmoCrateBestDist = dist;
				m_flAmmoCrateProgressAt = now;
			}
			else if (now - m_flAmmoCrateProgressAt > 20.0f && dist >= 900.0f)
				return stop("walk-stuck", 30.0f);
			// Stuck close by (run 20261115-0035: 688, the tank in the way): place here, the crate
			// reaches 1000 and the tank drives up to it (850).
			const bool placeHere = now - m_flAmmoCrateProgressAt > 6.0f && dist < 900.0f;
			const bool idle = m_pSchedules == nullptr || m_pSchedules->isEmpty();
			if (now >= m_flAmmoCrateNextNav && (idle || now >= m_flAmmoCrateNextNav + 10.0f))
			{
				m_flAmmoCrateNextNav = now + 3.0f;
				Vector away = origin - at;
				away.z = 0.0f;
				Vector spot = at + (away.Length() > 1.0f ? away.Normalized() : Vector(1, 0, 0)) * 250.0f;
				Vector onMesh;
				if (RCBotNavMesh::ClosestPointOnMesh(spot, 250.0f, onMesh))
					spot = onMesh;
				if (m_bEmpiresNavGoalActive)
					endEmpiresNavmeshGoal("drop", "ammo-crate", 0.0f);
				cancelNavigationAttempt("ammo-crate");
				if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
					m_pSchedules->freeMemory();
				if (!requestNavigation(spot, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
					setMoveTo(spot);
				RCBOT_DIAG("[RCBOT2] ammo-crate walk userid=%d veh=%d dist=%.0f\n", userId, m_iAmmoCrateVehicle, dist);
			}
			if (!placeHere)
				return true;
			RCBOT_DIAG("[RCBOT2] ammo-crate stuck-place userid=%d veh=%d dist=%.0f\n", userId, m_iAmmoCrateVehicle, dist);
		}
		cancelNavigationAttempt("ammo-crate-place");
		if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
			m_pSchedules->freeMemory();
		m_iAmmoCrateStage = 2;
		m_flAmmoCrateStageAt = now;
		m_flAmmoCrateNextNav = 0.0f;
	}

	stopMoving();
	const char* kit = team == EMP_TEAM_NF ? "weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
	const edict_t* weapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool kitActive = weapon != nullptr && !weapon->IsFree() && weapon->GetClassName() != nullptr &&
		std::strcmp(weapon->GetClassName(), kit) == 0;

	if (m_iAmmoCrateStage == 2)
	{
		if (!kitActive)
		{
			selectWeaponName(kit);
			return true;
		}
		int item = -1;
		readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerBuildItem", item);
		if (item != 7)
		{
			if (now >= m_flAmmoCrateNextNav && helpers != nullptr)
			{
				m_flAmmoCrateNextNav = now + 0.5f;
				helpers->ClientCommand(m_pEdict, "engSelect 7\n");
			}
			return true;
		}
		// Aim at the ground just ahead, on the vehicle's side (the kit traces 100 from the eye).
		Vector dir = at - origin;
		dir.z = 0.0f;
		dir = dir.Length() > 1.0f ? dir.Normalized() : Vector(1, 0, 0);
		const float turn = 40.0f * static_cast<float>(m_iAmmoCrateTries) * 3.14159265f / 180.0f;
		dir = Vector(dir.x * std::cos(turn) - dir.y * std::sin(turn), dir.x * std::sin(turn) + dir.y * std::cos(turn), 0.0f);
		// From the eye, 80 ahead at foot height: about 100 from the eye (trace 150) and clear of
		// the engineer's own box (GetBuildLocation 0x3cd800 checks +-25 against players).
		const Vector eyeAt = getEyePosition();
		m_vAmmoCrateTarget = Vector(eyeAt.x, eyeAt.y, origin.z) + dir * 80.0f;
		m_iAmmoCrateBaseline = CratesNear(origin, 300.0f);
		m_iAmmoCrateStage = 3;
		m_flAmmoCrateStageAt = now;
	}

	if (m_iAmmoCrateStage == 3)
	{
		setLookVector(m_vAmmoCrateTarget);
		setLookAtTask(LOOK_VECTOR);
		if (now - m_flAmmoCrateStageAt < 0.8f || !kitActive)
			return true;
		primaryAttack(true, 0.35f);
		m_iAmmoCrateStage = 4;
		m_flAmmoCrateStageAt = now;
		RCBOT_DIAG("[RCBOT2] ammo-crate place userid=%d veh=%d dist=%.0f try=%d\n", userId, m_iAmmoCrateVehicle, dist,
			m_iAmmoCrateTries);
		return true;
	}

	// m_iAmmoCrateStage == 4: a new crate near us is the success signal.
	setLookVector(m_vAmmoCrateTarget);
	setLookAtTask(LOOK_VECTOR);
	if (CratesNear(origin, 300.0f) > m_iAmmoCrateBaseline)
	{
		RCBOT_DIAG("[RCBOT2] ammo-crate placed userid=%d veh=%d dist=%.0f\n", userId, m_iAmmoCrateVehicle, dist);
		r = EmpiresAmmoRequest(); // this vehicle's call is answered; the driver now sees the crate
		m_iAmmoCrateStage = 0;
		m_iAmmoCrateVehicle = -1;
		m_flAmmoCrateNextSearch = now + 5.0f;
		return false;
	}
	if (now - m_flAmmoCrateStageAt < 2.0f)
		return true;
	{
		int item = -1;
		float nextSupply = -1.0f;
		readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerBuildItem", item);
		readEmpiresFloatProperty(m_pEdict, "m_flEngineerNextSupply", nextSupply);
		Vector forward;
		AngleVectors(m_vViewAngles, &forward);
		const Vector eye = getEyePosition();
		CBotGlobals::traceLine(eye, eye + forward * 150.0f, MASK_SOLID, nullptr);
		const trace_t* tr = CBotGlobals::getTraceResult();
		RCBOT_DIAG("[RCBOT2] ammo-crate miss userid=%d item=%d weapon=%s next_supply=%.1f now=%.1f view=(%.0f %.0f) "
			"target=(%.0f %.0f %.0f) eye=(%.0f %.0f %.0f) trace=%.2f end=(%.0f %.0f %.0f) crates=%d/%d all=%d\n", userId, item,
			weapon != nullptr && weapon->GetClassName() != nullptr ? weapon->GetClassName() : "-", nextSupply, now,
			m_vViewAngles.x, m_vViewAngles.y, m_vAmmoCrateTarget.x, m_vAmmoCrateTarget.y, m_vAmmoCrateTarget.z, eye.x,
			eye.y, eye.z, tr != nullptr ? tr->fraction : -1.0f, tr != nullptr ? tr->endpos.x : 0.0f,
			tr != nullptr ? tr->endpos.y : 0.0f, tr != nullptr ? tr->endpos.z : 0.0f, CratesNear(origin, 300.0f),
			m_iAmmoCrateBaseline, CratesNear(origin, 1.0e9f));
	}
	if (++m_iAmmoCrateTries >= kMaxTries)
		return stop("no-crate", 30.0f);
	m_iAmmoCrateStage = 2; // select again and turn a little for the next try
	m_flAmmoCrateStageAt = now;
	m_flAmmoCrateNextNav = 0.0f;
	return true;
}
