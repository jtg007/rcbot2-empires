// Engineer seismic grenade against enemy buildings (TODO 5.31.2 / 5.7).
// Seismics damage every building (user 23.11.); InfantrySeismicResist is a resistance:
// 0 on large buildings = full damage, wall 0.5 = half, turrets -0.7 / camera/radar -1.0 = extra.
// The throw itself runs through the shared mode-1 grenade action in handleEmpiresBuildingAttack;
// this file picks the target, solves the throw pitch and traces flight and damage.

#include "bot.h"
#include "bot_empires.h"
#include "bot_empires_internal.h"
#include "bot_empires_seismic_logic.h"
#include "bot_globals.h"
#include "bot_getprop.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include "bot_weapons.h"
#include <cmath>
#include <cstring>

extern IServerGameEnts* servergameents;

namespace
{
constexpr int kSeismicNoDamageThrows = 5; // ~2 detonations land while the next throws are in the air

const char* SeismicWeaponName(const int team)
{
	return team == EMP_TEAM_BE ? "weapon_emp_imp_seismic_grenade" : "weapon_emp_nf_seismic_grenade";
}

const char* SeismicProjectileName(const int team)
{
	return team == EMP_TEAM_BE ? "emp_imp_seismic_grenade_projectile" : "emp_nf_seismic_grenade_projectile";
}

Vector BuildingCenter(edict_t* e)
{
	Vector at = CBotGlobals::entityOrigin(e);
	if (ICollideable* col = e->GetCollideable())
	{
		const char* cls = e->GetClassName();
		if (EmpiresSeismic::IsMapModelClass(cls))
		{
			Vector mn, mx;
			col->WorldSpaceSurroundingBounds(&mn, &mx);
			return (mn + mx) * 0.5f;
		}
		at = col->GetCollisionOrigin() + (col->OBBMins() + col->OBBMaxs()) * 0.5f;
	}
	return at;
}

float Dist2D(const Vector& a, const Vector& b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// Clear lob path: line from the eye to the floor point in front of the target (where the
// grenade lands and rolls in). Accepts a hit on the target or on any eligible building/deployable part.
bool SeismicLaneClear(edict_t* self, edict_t* target, const Vector& eye, const Vector& land, const char*& hitClass)
{
	hitClass = "-";
	CBotGlobals::isVisibleHitAllExceptPlayer(self, eye, land, target);
	const trace_t* tr = CBotGlobals::getTraceResult();
	if (tr == nullptr || tr->fraction >= 0.95f)
		return true;
	if (tr->m_pEnt != nullptr)
	{
		edict_t* hit = servergameents != nullptr ? servergameents->BaseEntityToEdict(tr->m_pEnt) : nullptr;
		if (hit != nullptr && hit->GetClassName() != nullptr)
		{
			hitClass = hit->GetClassName();
			if ((hit == target || EmpiresSeismic::IsSeismicTargetClass(hitClass)) && (tr->endpos - land).Length() < 64.0f)
				return true;
		}
	}
	return false;
}

// Floor point 12 HU in front of the building's vehicle physbox (or its own box) on the side facing
// the thrower. Uses WorldSpaceSurroundingBounds for rotated map models.
Vector SeismicLandPoint(edict_t* building, const Vector& origin, const Vector& eye)
{
	Vector mn(-32, -32, 0), mx(32, 32, 0), centre = origin;
	float floorZ = origin.z;
	const char* cls = building->GetClassName();
	if (ICollideable* col = building->GetCollideable())
	{
		if (EmpiresSeismic::IsMapModelClass(cls))
		{
			Vector wmin, wmax;
			col->WorldSpaceSurroundingBounds(&wmin, &wmax);
			centre = (wmin + wmax) * 0.5f;
			mn = wmin - centre;
			mx = wmax - centre;
			floorZ = wmin.z;
		}
		else
		{
			mn = col->OBBMins();
			mx = col->OBBMaxs();
			centre = col->GetCollisionOrigin();
		}
	}
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* e = INDEXENT(index);
		if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr ||
			std::strcmp(e->GetClassName(), "emp_building_vehicle_physbox") != 0)
			continue;
		ICollideable* col = e->GetCollideable();
		if (col == nullptr || Dist2D(col->GetCollisionOrigin(), origin) > 64.0f)
			continue;
		mn = col->OBBMins();
		mx = col->OBBMaxs();
		centre = col->GetCollisionOrigin();
		break;
	}
	Vector p(eye.x - centre.x, eye.y - centre.y, 0.0f);
	const float cx = (std::max)(mn.x, (std::min)(mx.x, p.x));
	const float cy = (std::max)(mn.y, (std::min)(mx.y, p.y));
	Vector out(centre.x + cx, centre.y + cy, floorZ + 4.0f);
	Vector toEye(eye.x - out.x, eye.y - out.y, 0.0f);
	if (toEye.NormalizeInPlace() > 1.0f)
		out += toEye * 12.0f;
	return out;
}
}

bool CBotEmpires::armEmpiresSeismicThrow(const float now, const int userId, const int team)
{
	const char* weaponName = SeismicWeaponName(team);
	edict_t* seismic = nullptr;
	int ammoType = -1;
	if (const CBaseHandle* weapons = CClassInterface::getWeaponList(m_pEdict))
	{
		for (int slot = 0; slot < MAX_WEAPONS && seismic == nullptr; ++slot)
		{
			edict_t* weapon = EmpiresCarriedWeapon(weapons[slot]);
			if (weapon != nullptr && std::strcmp(weapon->GetClassName(), weaponName) == 0)
			{
				seismic = weapon;
				int secondary = -1;
				CClassInterface::getAmmoTypes(seismic, &ammoType, &secondary);
			}
		}
	}
	if (seismic == nullptr || ammoType < 0 || getAmmo(ammoType) <= 0)
	{
		m_flEmpiresBuildingAttackNext = now + 3.0f;
		return false;
	}
	const int enemyTeam = team == EMP_TEAM_NF ? EMP_TEAM_BE : EMP_TEAM_NF;
	const Vector eye = getEyePosition();
	edict_t* best = nullptr;
	const char* bestClass = nullptr;
	float bestScore = 1.0e9f;
	float bestDist = 0.0f;
	float bestPitch = 0.0f, bestElev = 0.0f, bestSpeed = 0.0f;
	bool bestRunning = false;
	int bestHealth = 0;
	Vector bestAt;
	Vector bestLand;
	float bestLandDist = 0.0f;
	edict_t* nearest = nullptr;
	float nearestDist = 1.0e9f;
	const char* nearestWhy = "-";
	int nearestHealth = 0;
	const char* nearestHit = "-";
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* e = INDEXENT(index);
		if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr || e->GetUnknown() == nullptr)
			continue;
		const char* cls = e->GetClassName();
		if (!EmpiresSeismic::IsSeismicTargetClass(cls))
			continue;
		if (EmpiresSeismic::IsMapModelClass(cls))
		{
			// Only throw at built solid obstacles that accept non-grenadier blast damage
			// (CEngineerMapObject::GetModifiedDamage 0x6329b0 checks m_bGrenadierOnly).
			if (!RCBotNavMesh::IsBuiltEmpiresMapObject(e))
				continue;
			bool grenadierOnly = false;
			if (readEmpiresEntityBoolProperty(e, "m_bGrenadierOnly", grenadierOnly) && grenadierOnly)
				continue;
		}
		int buildingTeam = -1;
		if (!readEmpiresEntityIntProperty(e, "m_iTeamNum", buildingTeam) ||
			(buildingTeam != EMP_TEAM_NF && buildingTeam != EMP_TEAM_BE))
			buildingTeam = std::strncmp(cls, "emp_building_nf_", 16) == 0 ? EMP_TEAM_NF :
				(std::strncmp(cls, "emp_building_imp_", 17) == 0 ? EMP_TEAM_BE : -1);
		if (buildingTeam != enemyTeam)
			continue;
		int health = 0;
		const bool healthOk = readEmpiresEntityIntProperty(e, "m_iHealth", health) && health > 0;
		const Vector at = BuildingCenter(e);
		const float dist = Dist2D(eye, at);
		const bool isNearest = dist < nearestDist;
		if (isNearest)
		{
			nearestDist = dist;
			nearest = e;
			nearestHealth = health;
			nearestWhy = healthOk ? "range" : "health";
		}
		if (!healthOk || dist > EmpiresSeismic::kRunningMaxRange + 600.0f)
			continue;
		if (index == m_iSeismicSkipTarget && now < m_flSeismicSkipUntil)
			continue;
		const float score = dist + EmpiresSeismic::TargetPreference(cls);
		if (score >= bestScore)
			continue;
		// Aim at the floor just in front of the building's vehicle physbox (it stops grenades,
		// run 20261123: bounce at the box face 353 HU from the barracks centre).
		const Vector origin = CBotGlobals::entityOrigin(e);
		const Vector land = SeismicLandPoint(e, origin, eye);
		const float landDist = Dist2D(eye, land);
		if (landDist < EmpiresSeismic::kMinRange || landDist > EmpiresSeismic::kRunningMaxRange)
		{
			if (isNearest)
				nearestWhy = "range";
			continue;
		}
		const EmpiresSeismic::ThrowSolution sol = EmpiresSeismic::PlanThrow(landDist, land.z - eye.z);
		if (!sol.valid)
		{
			if (isNearest)
				nearestWhy = "arc";
			continue;
		}
		const char* hitClass = "-";
		if (!SeismicLaneClear(m_pEdict, e, eye, land + Vector(0, 0, 16), hitClass))
		{
			if (isNearest)
			{
				nearestWhy = "not-visible";
				nearestHit = hitClass;
			}
			continue;
		}
		best = e;
		bestClass = cls;
		bestScore = score;
		bestDist = dist;
		bestPitch = sol.pitch;
		bestElev = sol.elev;
		bestSpeed = sol.speed;
		bestRunning = sol.running;
		bestHealth = health;
		bestAt = origin;
		bestLand = land;
		bestLandDist = landDist;
	}
	if (best == nullptr)
	{
		// Nothing left to throw at: put the grenade away, back to the gun.
		const edict_t* active = CClassInterface::getCurrentWeapon(m_pEdict);
		if (active != nullptr && !active->IsFree() && active->GetClassName() != nullptr &&
			std::strstr(active->GetClassName(), "seismic") != nullptr)
		{
			const char* back = team == EMP_TEAM_NF ? "weapon_emp_nf_pistol" : "weapon_emp_imp_pistol1";
			if (m_pPrimaryWeapon != nullptr && m_pPrimaryWeapon->getWeaponInfo() != nullptr &&
				m_pPrimaryWeapon->getWeaponInfo()->getWeaponName() != nullptr)
				back = m_pPrimaryWeapon->getWeaponInfo()->getWeaponName();
			selectWeaponName(back);
			RCBOT_DIAG("[RCBOT2] seismic holster userid=%d to=%s\n", userId, back);
		}
		if (nearest != nullptr && nearestDist < 2500.0f && now >= m_flSeismicNoneLog)
		{
			m_flSeismicNoneLog = now + 5.0f;
			const Vector at = CBotGlobals::entityOrigin(nearest);
			RCBOT_DIAG("[RCBOT2] seismic none userid=%d nearest=%d class=%s dist=%.0f health=%d why=%s hit=%s at=(%.0f %.0f %.0f)\n",
				userId, engine->IndexOfEdict(nearest), nearest->GetClassName(), nearestDist, nearestHealth, nearestWhy, nearestHit,
				at.x, at.y, at.z);
		}
		m_flEmpiresBuildingAttackNext = now + 1.0f;
		return false;
	}
	// Progress check: a target that keeps its health over several throws (repaired, or the grenades
	// do not reach it) is dropped for a while, like a human giving up on a bad angle.
	const int bestIndex = engine->IndexOfEdict(best);
	if (bestIndex != m_iSeismicBurstTarget)
	{
		m_iSeismicBurstTarget = bestIndex;
		m_iSeismicBurstHealth = bestHealth;
		m_iSeismicBurstThrows = 0;
	}
	else if (bestHealth < m_iSeismicBurstHealth)
	{
		m_iSeismicBurstHealth = bestHealth;
		m_iSeismicBurstThrows = 0;
	}
	else if (m_iSeismicBurstThrows >= kSeismicNoDamageThrows)
	{
		RCBOT_DIAG("[RCBOT2] stall-escalate who=seismic stage=2 reason=no-damage userid=%d target=%d class=%s "
			"health=%d throws=%d land_dist=%.0f\n", userId, bestIndex, bestClass, bestHealth, m_iSeismicBurstThrows,
			bestLandDist);
		m_iSeismicSkipTarget = bestIndex;
		m_flSeismicSkipUntil = now + 30.0f;
		m_iSeismicBurstTarget = 0;
		m_iSeismicBurstThrows = 0;
		m_flEmpiresBuildingAttackNext = now + 1.0f;
		return false;
	}
	++m_iSeismicBurstThrows;
	cancelNavigationAttempt("seismic");
	m_pSchedules->freeMemory();
	if (m_bEmpiresNavGoalActive)
		endEmpiresNavmeshGoal("drop", "seismic", 0.0f);
	if (!bestRunning)
		stopMoving();
	m_hEmpiresBuildingActionTarget = best;
	m_iEmpiresBuildingActionMode = 1;
	m_bEmpiresBuildingActionSeismic = true;
	m_bEmpiresBuildingActionSeismicRunning = bestRunning;
	m_flEmpiresSeismicPitch = bestPitch;
	m_flEmpiresBuildingActionUntil = now + (bestRunning ? 5.0f : 4.0f);
	m_flEmpiresBuildingActionReadyAt = now + 0.75f;
	m_flEmpiresBuildingActionPressUntil = 0.0f;
	m_bEmpiresBuildingActionThrown = false;
	m_vSeismicTrackTargetAt = bestLand;
	selectWeaponName(weaponName);
	RCBOT_DIAG("[RCBOT2] seismic armed userid=%d target=%d class=%s dist=%.0f dz=%.0f pitch=%.1f elev=%.1f v=%.0f "
		"run=%d health=%d ammo=%d at=(%.0f %.0f %.0f) land=(%.0f %.0f %.0f) land_dist=%.0f\n",
		userId, engine->IndexOfEdict(best), bestClass, bestDist, bestAt.z - eye.z, bestPitch, bestElev, bestSpeed,
		bestRunning ? 1 : 0, bestHealth, getAmmo(ammoType), bestAt.x, bestAt.y, bestAt.z, bestLand.x, bestLand.y,
		bestLand.z, bestLandDist);
	return true;
}

// Follows the thrown projectile until it detonates (SUB_Remove in Detonate 0x3de010), then reads
// the target health once more. Purely diagnostic: the measurement for range, roll and damage.
void CBotEmpires::tickEmpiresSeismicTrack()
{
	if (m_flSeismicTrackStart <= 0.0f || gpGlobals == nullptr || engine == nullptr)
		return;
	const float now = gpGlobals->curtime;
	const int userId = m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1;
	const int team = getTeam();
	edict_t* target = m_iSeismicTrackTarget > 0 ? engine->PEntityOfEntIndex(m_iSeismicTrackTarget) : nullptr;
	if (m_flSeismicTrackResultAt > 0.0f)
	{
		if (now < m_flSeismicTrackResultAt)
			return;
		int health = 0;
		const bool alive = target != nullptr && !target->IsFree() &&
			readEmpiresEntityIntProperty(target, "m_iHealth", health);
		RCBOT_DIAG("[RCBOT2] seismic result userid=%d target=%d health=%d->%d%s\n", userId, m_iSeismicTrackTarget,
			m_iSeismicTrackHealth, alive ? health : 0, alive ? "" : " destroyed");
		m_flSeismicTrackResultAt = 0.0f;
		m_flSeismicTrackStart = 0.0f;
		m_iSeismicTrackTarget = 0;
		return;
	}
	if (m_iSeismicTrackProjectile == 0)
	{
		if (now > m_flSeismicTrackStart + 1.0f)
		{
			RCBOT_DIAG("[RCBOT2] seismic lost userid=%d reason=no-projectile\n", userId);
			m_flSeismicTrackStart = 0.0f;
			return;
		}
		const char* projName = SeismicProjectileName(team);
		const Vector eye = getEyePosition();
		for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
		{
			edict_t* e = INDEXENT(index);
			if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr ||
				std::strcmp(e->GetClassName(), projName) != 0)
				continue;
			const Vector at = CBotGlobals::entityOrigin(e);
			if ((at - eye).Length() > 100.0f) // fresh spawn only, not an earlier grenade on the ground
				continue;
			m_iSeismicTrackProjectile = index;
			m_vSeismicTrackFirst = at;
			m_vSeismicTrackLast = at;
			m_flSeismicTrackLastSeen = now;
			break;
		}
		return;
	}
	edict_t* proj = engine->PEntityOfEntIndex(m_iSeismicTrackProjectile);
	const bool present = proj != nullptr && !proj->IsFree() && proj->GetClassName() != nullptr &&
		std::strcmp(proj->GetClassName(), SeismicProjectileName(team)) == 0;
	if (!present || now > m_flSeismicTrackStart + 12.0f)
	{
		const Vector d = m_vSeismicTrackLast - m_vSeismicTrackFirst;
		const Vector t = m_vSeismicTrackLast - m_vSeismicTrackTargetAt;
		RCBOT_DIAG("[RCBOT2] seismic detonated userid=%d at=(%.0f %.0f %.0f) travel2d=%.0f flight=%.1f "
			"to_land2d=%.0f dz=%.0f%s\n", userId, m_vSeismicTrackLast.x, m_vSeismicTrackLast.y,
			m_vSeismicTrackLast.z, std::sqrt(d.x * d.x + d.y * d.y), m_flSeismicTrackLastSeen - m_flSeismicTrackStart,
			std::sqrt(t.x * t.x + t.y * t.y), t.z, present ? " timeout" : "");
		m_iSeismicTrackProjectile = 0;
		m_flSeismicTrackResultAt = now + 0.5f;
		return;
	}
	m_vSeismicTrackLast = CBotGlobals::entityOrigin(proj);
	m_flSeismicTrackLastSeen = now;
}
