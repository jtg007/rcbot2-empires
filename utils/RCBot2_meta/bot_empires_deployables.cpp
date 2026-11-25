// Engineer deployables (TODO 5.9 / 5.31.4), per own refinery (nearest first, one engineer per refinery):
//  - a three-part wall (kit item 6) on the refinery's enemy side: a middle piece across the line to the enemy CV
//    and two wings bent back, clicked as one chained line (BuildWalls 0x3cecc0 -> CompleteWalls 0x397fc0).
//    Walls are free for engineers (user rule 2026-11-24: walls only at refineries),
//  - behind it a camera (kit item 4); a radar (item 5) only with a second detection slot
//    (m_iEngineerDetectionBuilt/Allotted, grows with rank).
// Each click goes through the kit's own GetBuildLocation (0x3cd800): ground within 150 of the eye, normal
// z >= 0.7, no player or vehicle in a +-25 box. Success = a new own entity of the class near the spot.
#include "bot_empires.h"
#include "bot_globals.h"
#include "bot_getprop.h"
#include "bot_navmesh_api.h"
#include "bot_schedule.h"
#include "bot_cvars.h"
#include "in_buttons.h"
#include "bot_empires_binary.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

bool EmpiresTeamResources(int team, float& resources); // bot_empires_commander_economy.cpp
extern IServerGameEnts* servergameents;

namespace
{
constexpr float kSearchRange = 8000.0f;
constexpr float kArrive = 40.0f;
constexpr float kDetectCover = 900.0f;  // a camera/radar this close covers the refinery
constexpr float kWallRadius[] = {420.0f, 480.0f, 370.0f}; // middle piece distance from the refinery
constexpr float kWallHalf = 110.0f;     // half of the middle piece; endpoints stay within the kit's 150
constexpr float kWingBack = 140.0f;     // wings bend back toward the refinery ...
constexpr float kWingOut = 100.0f;      // ... and outward
constexpr float kStandOff = 50.0f;      // the engineer stands this far behind a segment
constexpr int kMaxTries = 3;

class CTraceFilterSimpleLocal : public CTraceFilter
{
public:
	explicit CTraceFilterSimpleLocal(const IHandleEntity* skip) : m_skip(skip) {}
	bool ShouldHitEntity(IHandleEntity* entity, int) override { return entity != m_skip; }
private:
	const IHandleEntity* m_skip;
};

struct Detector
{
	Vector at;
	bool radar;
};

struct Mark
{
	int serial = -1;
	float until = 0.0f;
};
Mark s_claim[MAX_EDICTS];  // refinery -> engineer working there (or paused after a failure)
Mark s_walled[MAX_EDICTS]; // refinery -> wall finished (pieces may have been destroyed since: rebuilt later)

float Dist2D(const Vector& a, const Vector& b)
{
	return (a - b).Length2D();
}

bool IsClass(edict_t* e, const char* cls)
{
	return e != nullptr && !e->IsFree() && e->GetClassName() != nullptr && std::strcmp(e->GetClassName(), cls) == 0;
}

const char* ItemClass(int item)
{
	return item == 4 ? "emp_eng_camera" : item == 5 ? "emp_eng_radar" : "emp_eng_walls";
}

const char* ItemName(int item)
{
	return item == 4 ? "camera" : item == 5 ? "radar" : "walls";
}

int Key(int index, const edict_t* e)
{
	return index + static_cast<int>(e->m_NetworkSerialNumber) * 4096;
}

bool Marked(const Mark* marks, int index, float now)
{
	edict_t* e = engine->PEntityOfEntIndex(index);
	return e != nullptr && marks[index].serial == static_cast<int>(e->m_NetworkSerialNumber) && now < marks[index].until;
}

void SetMark(Mark* marks, int index, float until)
{
	edict_t* e = engine->PEntityOfEntIndex(index);
	if (e != nullptr && index > 0 && index < MAX_EDICTS)
		marks[index] = {static_cast<int>(e->m_NetworkSerialNumber), until};
}

void OwnNear(const char* cls, int team, const Vector& at, float radius, std::vector<int>& out)
{
	out.clear();
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (IsClass(e, cls) && CClassInterface::getTeam(e) == team && Dist2D(CBotGlobals::entityOrigin(e), at) < radius)
			out.push_back(Key(i, e));
	}
}

// New own entities of `cls` near `at` that were not in `before`; returns the nearest one.
edict_t* NewOwnNear(const char* cls, int team, const Vector& at, float radius, const std::vector<int>& before, int* count)
{
	edict_t* best = nullptr;
	float bestDist = radius;
	int n = 0;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (!IsClass(e, cls) || CClassInterface::getTeam(e) != team ||
			std::find(before.begin(), before.end(), Key(i, e)) != before.end())
			continue;
		const float d = Dist2D(CBotGlobals::entityOrigin(e), at);
		if (d >= radius)
			continue;
		++n;
		if (d < bestDist)
		{
			bestDist = d;
			best = e;
		}
	}
	if (count != nullptr)
		*count = n;
	return best;
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
	if (hit.fraction >= 1.0f || hit.startsolid || hit.plane.normal.z < 0.7f)
		return false;
	ground = hit.endpos;
	return true;
}

// Wall points the kit holds (CWeaponNFEngineerKit +0x5c4: 0 none, 1 = a start point is stored; BuildWalls
// 0x3cecc0, expert 24.11.); -1 when unreadable.
int KitWallPoints(const edict_t* kit)
{
	if (kit == nullptr || kit->IsFree() || !EmpiresBinaryOffsetsTrusted() ||
		const_cast<edict_t*>(kit)->GetUnknown() == nullptr)
		return -1;
	CBaseEntity* base = const_cast<edict_t*>(kit)->GetUnknown()->GetBaseEntity();
	if (base == nullptr)
		return -1;
	const int n = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(base) + 0x5c4);
	return n >= 0 && n <= 2 ? n : -1;
}

// Distance from p to segment ab in 2D.
float SegDist2D(const Vector& p, const Vector& a, const Vector& b)
{
	Vector ab = b - a;
	ab.z = 0.0f;
	Vector ap = p - a;
	ap.z = 0.0f;
	const float len2 = ab.LengthSqr();
	const float t = len2 > 1.0f ? (std::max)(0.0f, (std::min)(1.0f, ap.Dot(ab) / len2)) : 0.0f;
	return (ap - ab * t).Length2D();
}

// The three wall segments for a refinery at `at` facing `dir` (2D unit vector to the enemy).
void WallSegments(const Vector& at, const Vector& dir, float radius, Vector seg[3][2])
{
	const Vector lat(-dir.y, dir.x, 0.0f);
	const Vector mid = at + dir * radius;
	seg[0][0] = mid - lat * kWallHalf;
	seg[0][1] = mid + lat * kWallHalf;
	seg[1][0] = seg[0][0];
	seg[1][1] = mid - dir * kWingBack - lat * (kWallHalf + kWingOut);
	seg[2][0] = seg[0][1];
	seg[2][1] = mid - dir * kWingBack + lat * (kWallHalf + kWingOut);
}

// The wall is one chained line (BuildWalls 0x3cecc0: after the first point every click extends from the last
// end, expert 24.11.): left wing end -> middle left -> middle right -> right wing end.
void WallPoints(const Vector& at, const Vector& dir, float radius, Vector pts[4])
{
	Vector seg[3][2];
	WallSegments(at, dir, radius, seg);
	pts[0] = seg[1][1];
	pts[1] = seg[0][0];
	pts[2] = seg[0][1];
	pts[3] = seg[2][1];
}

// Stand point behind a segment (on the refinery side).
Vector SegmentStand(const Vector seg[2], const Vector& refinery)
{
	const Vector mid = (seg[0] + seg[1]) * 0.5f;
	Vector along = seg[1] - seg[0];
	along.z = 0.0f;
	along = along.Length() > 1.0f ? along.Normalized() : Vector(1, 0, 0);
	Vector n(-along.y, along.x, 0.0f);
	Vector toRef = refinery - mid;
	toRef.z = 0.0f;
	if (n.Dot(toRef) < 0.0f)
		n = -n;
	return mid + n * kStandOff;
}
// Stand for clicking wall point k: 70 behind it, off the piece that this click creates.
Vector WallPointStand(const Vector pts[4], int k, const Vector& refinery, float off)
{
	const int a = k == 0 ? 0 : k - 1, b = k == 0 ? 1 : k;
	Vector along = pts[b] - pts[a];
	along.z = 0.0f;
	along = along.Length() > 1.0f ? along.Normalized() : Vector(1, 0, 0);
	Vector n(-along.y, along.x, 0.0f);
	Vector toRef = refinery - pts[k];
	toRef.z = 0.0f;
	if (n.Dot(toRef) < 0.0f)
		n = -n;
	return pts[k] + n * off;
}
} // namespace

bool CBotEmpires::handleEmpiresEngineerDeployables()
{
	EmpiresDeployJob& job = m_deploy;
	const char* blocked = gpGlobals == nullptr || engine == nullptr || !rcbot_empires_engineer_deployables.GetBool() ||
		!isEmpiresEngineerClass() || EmpiresIsEngibot(m_pEdict) ? "off" : m_iReviveTarget != 0 ? "revive" :
		m_iBuildAssignStage > 0 ? "build-assign" : m_iTurretRefinery != 0 || m_iTurretBuilding != 0 ||
		m_bTurretBuildArmed || m_iTurretUpgrade != 0 ? "turret" : m_iAmmoCrateStage != 0 ? "ammo-crate" : commanderDutyBlocksSideTasks() ? "commander" : nullptr;
	if (blocked != nullptr)
	{
		if (job.stage != 0)
		{
			RCBOT_DIAG("[RCBOT2] deploy end userid=%d item=%s refinery=%d segment=%d reason=preempted-%s\n",
				engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1, ItemName(job.item), job.refinery, job.segment,
				blocked);
			if (job.refinery > 0)
				SetMark(s_claim, job.refinery, gpGlobals != nullptr ? gpGlobals->curtime + 20.0f : 0.0f);
			job.stage = 0;
		}
		return false;
	}
	const float now = gpGlobals->curtime;
	const int team = getTeam();
	const int userId = engine->GetPlayerUserId(m_pEdict);

	auto stop = [&](const char* reason, float claim, float pause)
	{
		RCBOT_DIAG("[RCBOT2] deploy end userid=%d item=%s refinery=%d segment=%d reason=%s tries=%d\n", userId,
			ItemName(job.item), job.refinery, job.segment, reason, job.tries);
		SetMark(s_claim, job.refinery, now + claim);
		job.stage = 0;
		job.nextSearch = now + pause;
		cancelNavigationAttempt("deploy-end");
		setLookAtTask(LOOK_NONE);
		return false;
	};

	if (job.stage == 0)
	{
		if (now < job.nextSearch)
			return false;
		job.nextSearch = now + 5.0f;

		int detBuilt = -1, detAllotted = -1;
		const int mode = rcbot_empires_engineer_deployables.GetInt();
		const bool detectFree = (mode & 1) != 0 && readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerDetectionBuilt", detBuilt) &&
			readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerDetectionAllotted", detAllotted) &&
			detAllotted > 0 && detBuilt < detAllotted;
		float resources = 0.0f;
		// Engineer walls cost the team nothing (user 24.11.); resources are only logged.
		EmpiresTeamResources(team, resources);
		const bool wallsOn = (mode & 2) != 0;
		if (!detectFree && !wallsOn)
			return false;

		const char* cvClass = team == 2 ? "emp_nf_commander" : "emp_imp_commander";
		const char* enemyCvClass = team == 2 ? "emp_imp_commander" : "emp_nf_commander";
		Vector home, enemyHome;
		bool haveHome = false, haveEnemyHome = false;
		struct Blocker
		{
			Vector at;
			float clearance;
		};
		std::vector<edict_t*> refineries;
		std::vector<Detector> detectors;
		std::vector<Vector> walls;
		std::vector<Blocker> blockers;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
				continue;
			const char* cls = e->GetClassName();
			const int t = CClassInterface::getTeam(e);
			if (std::strcmp(cls, "emp_building_refinery") == 0 ||
				(std::strncmp(cls, team == 2 ? "emp_building_nf_" : "emp_building_imp_", team == 2 ? 16 : 17) == 0 &&
					std::strstr(cls, "refinery") != nullptr))
				refineries.push_back(e);
			else if ((std::strcmp(cls, "emp_eng_camera") == 0 || std::strcmp(cls, "emp_eng_radar") == 0) && t == team)
			{
				detectors.push_back({CBotGlobals::entityOrigin(e), std::strcmp(cls, "emp_eng_radar") == 0});
			}
			else if (std::strcmp(cls, "emp_eng_walls") == 0 && t == team)
				walls.push_back(CBotGlobals::entityOrigin(e));
			else if (std::strncmp(cls, "emp_building_", 13) == 0)
				blockers.push_back({CBotGlobals::entityOrigin(e),
					std::strstr(cls, "turret") != nullptr ? 75.0f : 110.0f});
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
		if (!haveEnemyHome)
			return false;

		// Per refinery (nearest first, another engineer takes the next one): the three-part wall first,
		// then a radar and a camera behind it (user rule 2026-11-24).
		float best = kSearchRange;
		int pickItem = 0, nOurs = 0, nClaimed = 0, nDone = 0, nNoSpot = 0;
		int pickRefinery = 0;
		float pickRadius = 0.0f;
		Vector pickAt, pickDir, pickStand, pickTarget;
		for (edict_t* ref : refineries)
		{
			const int index = engine->IndexOfEdict(ref);
			const Vector at = CBotGlobals::entityOrigin(ref);
			const int refTeam = CClassInterface::getTeam(ref);
			const bool ours = refTeam == team || (refTeam != 2 && refTeam != 3 && haveHome &&
				Dist2D(at, home) < Dist2D(at, enemyHome));
			if (!ours)
				continue;
			++nOurs;
			Vector dir = enemyHome - at;
			dir.z = 0.0f;
			if (dir.Length() < 1.0f)
				continue;
			dir = dir.Normalized();
			const float d = Dist2D(at, getOrigin());
			if (d >= best)
				continue;
			if (Marked(s_claim, index, now))
			{
				++nClaimed;
				continue;
			}

			// Own wall in front of this refinery: its distance along the enemy direction.
			float wallAlong = 0.0f;
			for (const Vector& w : walls)
			{
				const float along = (w - at).Dot(dir);
				if (Dist2D(w, at) < 650.0f && along > 0.0f && (wallAlong <= 0.0f || along > wallAlong))
					wallAlong = along;
			}
			const bool walled = wallAlong > 0.0f || Marked(s_walled, index, now);
			if (!walled && wallsOn)
			{
				// First radius whose three segments have ground under every endpoint and keep 110 off buildings.
				float radius = 0.0f;
				for (float r : kWallRadius)
				{
					Vector seg[3][2];
					WallSegments(at, dir, r, seg);
					bool ok = true;
					for (int s = 0; s < 3 && ok; ++s)
					{
						Vector g0, g1, onMesh;
						ok = GroundBelow(seg[s][0], g0) && GroundBelow(seg[s][1], g1) &&
							std::fabs(g0.z - at.z) < 120.0f && std::fabs(g1.z - at.z) < 120.0f;
						for (const Blocker& b : blockers)
							ok = ok && SegDist2D(b.at, seg[s][0], seg[s][1]) > b.clearance; // building boxes reach out
						ok = ok && (!RCBotNavMesh::IsLoaded() ||
							RCBotNavMesh::ClosestPointOnMesh(SegmentStand(seg[s], at), 80.0f, onMesh));
					}
					if (ok)
					{
						radius = r;
						break;
					}
				}
				if (radius > 0.0f)
				{
					best = d;
					pickRefinery = index;
					pickItem = 6;
					pickAt = at;
					pickDir = dir;
					pickRadius = radius;
					continue;
				}
				++nNoSpot;
				if (!detectFree)
					continue;
			}
			if (!detectFree)
			{
				++nDone;
				continue;
			}
			bool haveRadar = false, haveCamera = false;
			for (const Detector& det : detectors)
				if (Dist2D(det.at, at) < kDetectCover)
					(det.radar ? haveRadar : haveCamera) = true;
			// A camera first; a radar only when the engineer may place two devices (allotment grows with rank,
			// user 24.11.: about 30 points), so it comes later.
			const bool radarOk = !haveRadar && detAllotted >= 2 && now >= job.radarBlockedUntil;
			if (haveCamera && !radarOk)
			{
				++nDone;
				continue;
			}
			const int item = !haveCamera ? 4 : 5;
			// Behind the wall (refinery side): radar left, camera right of the enemy line.
			const float along = (wallAlong > 0.0f ? wallAlong : kWallRadius[0]) - 110.0f;
			const Vector lat(-dir.y, dir.x, 0.0f);
			const Vector spot = at + dir * (std::max)(along, 280.0f) + lat * (item == 5 ? -90.0f : 90.0f);
			Vector stand = spot - dir * 80.0f, onMesh, ground;
			if (RCBotNavMesh::IsLoaded())
			{
				if (!RCBotNavMesh::ClosestPointOnMesh(stand, 120.0f, onMesh))
				{
					++nNoSpot;
					continue;
				}
				stand = onMesh;
			}
			if (!GroundBelow(spot, ground))
			{
				++nNoSpot;
				continue;
			}
			best = d;
			pickRefinery = index;
			pickItem = item;
			pickStand = stand;
			pickTarget = ground;
		}
		if (pickRefinery == 0)
		{
			if (now >= job.nextLog)
			{
				job.nextLog = now + 30.0f;
				RCBOT_DIAG("[RCBOT2] deploy none userid=%d detect=%d/%d resources=%.0f refineries=%d ours=%d claimed=%d "
					"done=%d no_spot=%d\n", userId, detBuilt, detAllotted, resources,
					static_cast<int>(refineries.size()), nOurs, nClaimed, nDone, nNoSpot);
			}
			return false;
		}
		job.item = pickItem;
		job.refinery = pickRefinery;
		job.tries = 0;
		job.click = 0;
		job.built = 0;
		job.segment = 0;
		job.before.clear();
		job.skip.clear();
		job.pieceSeg.clear();
		job.resetChain = pickItem == 6;
		if (pickItem == 6)
		{
			job.aim[0] = pickAt;   // refinery origin
			job.aim[1] = pickDir;  // enemy direction (2D)
			OwnNear("emp_eng_walls", team, pickAt, 900.0f, job.before);
			job.radius = pickRadius;
		}
		else
		{
			job.stand = pickStand;
			job.aim[0] = pickTarget;
		}
		job.stage = 1;
		job.stageAt = now;
		job.progressAt = now;
		job.bestDist = 1.0e9f;
		job.nextNav = 0.0f;
		job.until = now + (pickItem == 6 ? 360.0f : 150.0f);
		SetMark(s_claim, pickRefinery, now + 5.0f);
		RCBOT_DIAG("[RCBOT2] deploy start userid=%d item=%s refinery=%d dist=%.0f resources=%.0f detect=%d/%d\n", userId,
			ItemName(pickItem), pickRefinery, best, resources, detBuilt, detAllotted);
	}

	edict_t* refinery = engine->PEntityOfEntIndex(job.refinery);
	if (refinery == nullptr || refinery->IsFree())
		return stop("refinery-gone", 60.0f, 3.0f);
	if (now > job.until)
	{
		RCBOT_DIAG("[RCBOT2] stall-escalate who=deploy stage=%d reason=timeout\n", job.stage);
		return stop("timeout", 120.0f, 10.0f);
	}
	SetMark(s_claim, job.refinery, now + 5.0f);
	// Another task held the bot (heal, fight, seat): start the current segment/device over from the walk.
	if (job.stage >= 2 && job.stage <= 4 && now - job.lastTick > 1.5f)
	{
		RCBOT_DIAG("[RCBOT2] deploy resume userid=%d item=%s segment=%d away=%.1f\n", userId, ItemName(job.item),
			job.segment, now - job.lastTick);
		job.stage = 1;
		if (job.item == 6)
		{
			// A weapon switch resets the kit's point counter: start the chain again from the last good point.
			job.segment = job.segment > 0 ? job.segment - 1 : 0;
			job.resetChain = true;
		}
		job.stageAt = now;
		job.progressAt = now;
		job.bestDist = 1.0e9f;
	}
	job.lastTick = now;

	// Walls: job.segment = index of the wall point to click next (0..3).
	Vector pts[4];
	if (job.item == 6)
	{
		WallPoints(job.aim[0], job.aim[1], job.radius, pts);
		// After a rejected point stand a bit farther back (the piece's box trace must not hit the bot).
		job.stand = WallPointStand(pts, job.segment, job.aim[0], (std::min)(110.0f, 70.0f + 20.0f * static_cast<float>(job.tries)));
		Vector onMesh;
		if (RCBotNavMesh::IsLoaded() && RCBotNavMesh::ClosestPointOnMesh(job.stand, 80.0f, onMesh))
			job.stand = onMesh;
	}

	const Vector origin = getOrigin();
	if (job.stage == 1)
	{
		const float dist = Dist2D(origin, job.stand);
		if (dist > kArrive || std::fabs(origin.z - job.stand.z) > 80.0f)
		{
			if (dist < job.bestDist - 150.0f)
			{
				job.bestDist = dist;
				job.progressAt = now;
			}
			if (now - job.progressAt > 25.0f)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=deploy stage=1 reason=walk-stuck dist=%.0f\n", dist);
				return stop("walk-stuck", 120.0f, 10.0f);
			}
			const bool idle = m_pSchedules == nullptr || m_pSchedules->isEmpty();
			if (now >= job.nextNav && (idle || now >= job.nextNav + 10.0f))
			{
				job.nextNav = now + 3.0f;
				if (m_bEmpiresNavGoalActive)
					endEmpiresNavmeshGoal("drop", "deploy", 0.0f);
				cancelNavigationAttempt("deploy");
				if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
					m_pSchedules->freeMemory();
				if (dist < 200.0f || !requestNavigation(job.stand, RCBotNavigation::NavigationPolicy::NAVMESH_ONLY, 0))
					setMoveTo(job.stand);
				RCBOT_DIAG("[RCBOT2] deploy walk userid=%d item=%s refinery=%d segment=%d dist=%.0f\n", userId,
					ItemName(job.item), job.refinery, job.segment, dist);
			}
			if (dist < 200.0f)
				setMoveTo(job.stand);
			return true;
		}
		cancelNavigationAttempt("deploy-place");
		if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
			m_pSchedules->freeMemory();
		job.stage = 2;
		job.stageAt = now;
		job.nextNav = 0.0f;
	}

	stopMoving();
	// A path schedule or a navmesh goal would take the view (LOOK_WAYPOINT) while we aim.
	if (m_bEmpiresNavGoalActive)
		endEmpiresNavmeshGoal("drop", "deploy-aim", 0.0f);
	if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
		m_pSchedules->freeMemory();
	const char* kit = team == EMP_TEAM_NF ? "weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
	const edict_t* weapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool kitActive = weapon != nullptr && !weapon->IsFree() && weapon->GetClassName() != nullptr &&
		std::strcmp(weapon->GetClassName(), kit) == 0;

	// Build up a placed device or wall piece with the kit (item 0 = repair path).
	if (job.stage == 5)
	{
		edict_t* e = engine->PEntityOfEntIndex(job.built);
		int health = -1, maxHealth = -1;
		const bool known = e != nullptr && !e->IsFree() && readEmpiresEntityIntProperty(e, "m_iHealth", health) &&
			readEmpiresEntityIntProperty(e, "m_iMaxHealth", maxHealth) && maxHealth > 0;
		// Health progress per piece (the kit builds ~2 HP/s without clip, so a piece can take most of a minute).
		if (job.progressPiece != job.built)
		{
			job.progressPiece = job.built;
			job.progressHealth = -1;
			job.buildAt = now;
		}
		if (known && health > job.progressHealth)
		{
			job.progressHealth = health;
			job.buildAt = now;
		}
		const bool stalled = now - job.buildAt > 15.0f || now - job.stageAt > 90.0f;
		if (!known || health >= maxHealth || stalled)
		{
			if (job.built > 0)
				RCBOT_DIAG("[RCBOT2] deploy built userid=%d item=%s entity=%d health=%d/%d secs=%.1f\n", userId,
					ItemName(job.item), job.built, health, maxHealth, now - job.stageAt);
			if (known && health < maxHealth)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=deploy stage=5 reason=build-up-timeout\n");
				job.skip.push_back(Key(job.built, e)); // another engineer or the dispatcher may finish it
			}
			job.built = 0;
			job.stageAt = now; // each piece gets its own window
			job.sideFlip = false;
			job.aimAlt = false;
			job.lastHealth = -1;
			job.healthAt = now;
			if (job.item == 6)
			{
				// Next unfinished new wall piece of this job.
				for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && job.built == 0; ++i)
				{
					edict_t* w = engine->PEntityOfEntIndex(i);
					int h = -1, mh = -1;
					if (IsClass(w, "emp_eng_walls") && CClassInterface::getTeam(w) == team &&
						std::find(job.before.begin(), job.before.end(), Key(i, w)) == job.before.end() &&
						std::find(job.skip.begin(), job.skip.end(), Key(i, w)) == job.skip.end() &&
						Dist2D(CBotGlobals::entityOrigin(w), job.aim[0]) < 900.0f &&
						readEmpiresEntityIntProperty(w, "m_iHealth", h) && readEmpiresEntityIntProperty(w, "m_iMaxHealth", mh) &&
						mh > 0 && h < mh)
						job.built = i;
				}
				if (job.built != 0)
					return true;
			}
			RCBOT_DIAG("[RCBOT2] deploy done userid=%d item=%s refinery=%d\n", userId, ItemName(job.item), job.refinery);
			if (job.item == 6)
				SetMark(s_walled, job.refinery, now + 600.0f);
			SetMark(s_claim, job.refinery, now + 2.0f);
			job.stage = 0;
			job.nextSearch = now + 3.0f;
			setLookAtTask(LOOK_NONE);
			return false;
		}
		if (!kitActive)
		{
			selectWeaponName(kit);
			return true;
		}
		int item = -1;
		readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerBuildItem", item);
		if (item != 0)
		{
			if (now >= job.nextNav && helpers != nullptr)
			{
				job.nextNav = now + 0.5f;
				helpers->ClientCommand(m_pEdict, "engSelect 0\n");
			}
			return true;
		}
		// Work from the refinery side of the piece (walking to its center runs into the wall); without getting
		// into reach for 6 s try the other side (match 20261124-121214: stuck 187 away on a slope).
		const Vector eyeNow = getEyePosition();
		Vector c = CBotGlobals::worldCenter(e);
		Vector side = job.item == 6 ? job.aim[0] - c : getOrigin() - c;
		side.z = 0.0f;
		side = side.Length() > 1.0f ? side.Normalized() : Vector(1, 0, 0);
		if ((c - eyeNow).Length() <= 110.0f || job.reachAt < job.stageAt)
			job.reachAt = now; // last time in reach
		else if (now - job.reachAt > 6.0f)
		{
			job.sideFlip = !job.sideFlip;
			job.reachAt = now;
		}
		if (job.sideFlip)
			side = -side;
		Vector workAt = CBotGlobals::entityOrigin(e) + side * 60.0f;
		if (job.item == 6)
		{
			// Behind the middle of the segment this piece was placed with; aim at the segment middle at chest height
			// (a chained piece's origin/box center can sit near a corner and the view then hits the neighbour).
			int bestSeg = 0;
			for (const auto& ps : job.pieceSeg)
				if (ps.first == Key(job.built, e))
					bestSeg = ps.second;
			float bestSegDist = 1.0e9f;
			for (int k = 1; k < 4 && bestSeg == 0; ++k)
			{
				const float d = SegDist2D(c, pts[k - 1], pts[k]);
				if (d < bestSegDist)
				{
					bestSegDist = d;
					bestSeg = k;
				}
			}
			const Vector seg2[2] = {pts[bestSeg - 1], pts[bestSeg]};
			workAt = SegmentStand(seg2, job.aim[0]);
			if (job.sideFlip)
				workAt = (seg2[0] + seg2[1]) * 0.5f * 2.0f - workAt; // mirror to the far side
			Vector mid = (seg2[0] + seg2[1]) * 0.5f, g;
			if (GroundBelow(mid, g))
				mid = g;
			// No health gain for 4 s: switch between the segment middle and the piece's box center.
			if (health > job.lastHealth)
			{
				job.lastHealth = health;
				job.healthAt = now;
			}
			else if (now - job.healthAt > 4.0f)
			{
				job.aimAlt = !job.aimAlt;
				job.healthAt = now;
			}
			if (!job.aimAlt)
				c = mid + Vector(0, 0, 40.0f);
		}
		if (Dist2D(getOrigin(), workAt) > 30.0f && (c - eyeNow).Length() > 80.0f)
			setMoveTo(workAt);
		if (m_iLookTask != LOOK_VECTOR)
			m_fLookSetTime = 0.0f;
		setLookVector(c);
		setLookAtTask(LOOK_VECTOR);
		primaryAttack(true, 0.3f);
		if (now >= job.nextLog)
		{
			job.nextLog = now + 5.0f;
			Vector fwd;
			AngleVectors(m_vViewAngles, &fwd);
			const Vector eye = getEyePosition();
			CTraceFilterSimpleLocal self(m_pEdict->GetIServerEntity());
			CBotGlobals::traceLine(eye, eye + fwd * 150.0f, MASK_SHOT, &self);
			const trace_t* tr = CBotGlobals::getTraceResult();
			edict_t* hit = tr != nullptr && tr->m_pEnt != nullptr && servergameents != nullptr ?
				servergameents->BaseEntityToEdict(tr->m_pEnt) : nullptr;
			RCBOT_DIAG("[RCBOT2] deploy build-up userid=%d entity=%d health=%d/%d eye_dist=%.0f hit=%d/%s frac=%.2f\n",
				userId, job.built, health, maxHealth, (c - eye).Length(), hit != nullptr ? engine->IndexOfEdict(hit) : -1,
				hit != nullptr && hit->GetClassName() != nullptr ? hit->GetClassName() : "-", tr != nullptr ? tr->fraction : -1.0f);
		}
		return true;
	}

	if (job.stage == 2)
	{
		if (!kitActive)
		{
			selectWeaponName(kit);
			if (now - job.stageAt > 4.0f)
				return stop("no-kit", 60.0f, 10.0f);
			return true;
		}
		if (job.item == 6 && job.resetChain)
		{
			// SecondaryAttack clears the kit's wall point counter (0x3cc330); it also cycles the item.
			secondaryAttack(false);
			job.resetChain = false;
			job.firstPoint = true;
			job.stageAt = now;
			return true;
		}
		// After the reset tap give the kit time to accept the next primary (a swallowed first click made the
		// next point the start and every retry there p0 == p1, run 20261124-1240).
		if (job.item == 6 && job.firstPoint && (now - job.stageAt < 0.8f || KitWallPoints(weapon) > 0))
		{
			if (now - job.stageAt > 4.0f)
				job.resetChain = true;
			return true;
		}
		int item = -1;
		readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerBuildItem", item);
		if (item != job.item)
		{
			if (now - job.stageAt > 4.0f)
			{
				RCBOT_DIAG("[RCBOT2] deploy item-not-applied userid=%d want=%d item=%d\n", userId, job.item, item);
				if (job.item == 5)
					job.radarBlockedUntil = now + 300.0f;
				return stop("item-not-applied", 30.0f, 3.0f);
			}
			if (now >= job.nextNav && helpers != nullptr)
			{
				job.nextNav = now + 0.5f;
				char cmd[32];
				snprintf(cmd, sizeof(cmd), "engSelect %d\n", job.item);
				helpers->ClientCommand(m_pEdict, cmd);
			}
			return true;
		}
		if (job.item != 6)
			OwnNear(ItemClass(job.item), team, origin, 600.0f, job.before);
		job.stage = 3;
		job.stageAt = now;
	}

	// Click target: the device spot, or the current segment's endpoint.
	Vector target = job.aim[0];
	if (job.item == 6)
	{
		Vector g;
		target = pts[job.segment];
		if (job.segment > 0 && job.tries > 0)
		{
			// A rejected piece: shorten it toward the previous point (fewer pieces, other ground under them).
			Vector back = pts[job.segment - 1] - target;
			back.z = 0.0f;
			if (back.Length() > 1.0f)
				target = target + back.Normalized() * (30.0f * static_cast<float>(job.tries));
		}
		if (GroundBelow(target, g))
			target = g;
	}
	else if (job.tries > 0)
	{
		// Retry: turn the spot around the stand point.
		Vector dir = job.aim[0] - job.stand;
		dir.z = 0.0f;
		dir = dir.Length() > 1.0f ? dir.Normalized() : Vector(1, 0, 0);
		const float turn = (job.tries % 2 == 1 ? 50.0f : -50.0f) * 3.14159265f / 180.0f;
		dir = Vector(dir.x * std::cos(turn) - dir.y * std::sin(turn), dir.x * std::sin(turn) + dir.y * std::cos(turn), 0.0f);
		Vector g;
		target = job.stand + dir * 80.0f;
		if (GroundBelow(target, g))
			target = g;
	}
	// Stand still for real: a still-valid move target makes checkStuck() lock the view on LOOK_WAYPOINT for 2-4 s
	// (bot.cpp ~2090), and stopMoving() yields to a higher move priority.
	m_bMoveToIsValid = false;
	if (m_iLookTask != LOOK_VECTOR)
		m_fLookSetTime = 0.0f;
	setLookVector(target);
	setLookAtTask(LOOK_VECTOR);

	if (job.stage == 3)
	{
		if (now - job.stageAt < 0.5f || !kitActive)
			return true;
		// A player in the new piece's line makes CompleteWalls reject it (box trace, run 20261124-1250: the
		// refinery guard 15 units from the wing): wait up to 6 s for the line to clear.
		if (job.item == 6 && job.segment > 0 && now - job.stageAt < 6.0f)
		{
			for (int i = 1; i <= gpGlobals->maxClients; ++i)
			{
				edict_t* p = engine->PEntityOfEntIndex(i);
				if (p == nullptr || p->IsFree() || p == m_pEdict || !CBotGlobals::entityIsAlive(p) ||
					SegDist2D(CBotGlobals::entityOrigin(p), pts[job.segment - 1], target) > 40.0f)
					continue;
				if (now >= job.nextLog)
				{
					job.nextLog = now + 3.0f;
					RCBOT_DIAG("[RCBOT2] deploy wall-line-wait userid=%d point=%d player=%d\n", userId, job.segment, i);
				}
				return true;
			}
		}
		// Click only once the view is on the target (the kit traces along the view, 150 from the eye).
		QAngle want;
		VectorAngles(target - getEyePosition(), want);
		CBotGlobals::fixFloatAngle(&want.x);
		CBotGlobals::fixFloatAngle(&want.y);
		float dp = m_vViewAngles.x - want.x, dy = m_vViewAngles.y - want.y;
		while (dy > 180.0f) dy -= 360.0f;
		while (dy < -180.0f) dy += 360.0f;
		if (std::fabs(dp) > 3.0f || std::fabs(dy) > 3.0f)
		{
			// Never click off target: a wall click would extend the chain to a wrong point.
			if (now - job.stageAt < 4.0f)
				return true;
			RCBOT_DIAG("[RCBOT2] deploy aim-blocked userid=%d item=%s view=(%.0f %.0f) want=(%.0f %.0f) look_task=%d\n",
				userId, ItemName(job.item), m_vViewAngles.x, m_vViewAngles.y, want.x, want.y, static_cast<int>(m_iLookTask));
			if (++job.tries >= kMaxTries)
			{
				RCBOT_DIAG("[RCBOT2] stall-escalate who=deploy stage=3 reason=aim-blocked\n");
				return stop("aim-blocked", 120.0f, 10.0f);
			}
			job.stageAt = now;
			return true;
		}
		primaryAttack(false); // a tap: a held button would fire BuildWalls again on refire
		job.stage = 4;
		job.stageAt = now;
		RCBOT_DIAG("[RCBOT2] deploy click userid=%d item=%s refinery=%d segment=%d click=%d target=(%.0f %.0f %.0f) "
			"eye_dist=%.0f view=(%.0f %.0f) want=(%.0f %.0f) look_task=%d try=%d\n", userId, ItemName(job.item), job.refinery, job.segment,
			job.click, target.x, target.y, target.z, (target - getEyePosition()).Length(), m_vViewAngles.x, m_vViewAngles.y,
			want.x, want.y, static_cast<int>(m_iLookTask), job.tries);
		return true;
	}

	// job.stage == 4: observe.
	if (job.item == 6)
	{
		if (now - job.stageAt < 0.5f)
			return true;
		// job.click counts the new pieces so far: the first point creates none, each later click adds some.
		int pieces = 0;
		NewOwnNear("emp_eng_walls", team, job.aim[0], 900.0f, job.before, &pieces);
		const int stored = KitWallPoints(weapon);
		if (job.firstPoint && stored == 0)
		{
			if (now - job.stageAt < 1.5f)
				return true;
			RCBOT_DIAG("[RCBOT2] deploy wall-start-lost userid=%d point=%d tries=%d\n", userId, job.segment, job.tries);
			if (++job.tries >= kMaxTries)
				return stop("wall-start-lost", 120.0f, 10.0f);
			job.stage = 3;
			job.stageAt = now;
			return true;
		}
		if (!job.firstPoint && stored == 0 && pieces <= job.click)
		{
			// The chain was lost (weapon switch): start again from the previous point.
			RCBOT_DIAG("[RCBOT2] deploy wall-chain-lost userid=%d point=%d\n", userId, job.segment);
			job.segment = (std::max)(0, job.segment - 1);
			job.firstPoint = true;
			job.stage = 1;
			job.stageAt = now;
			job.progressAt = now;
			job.bestDist = 1.0e9f;
			return true;
		}
		if (job.firstPoint || pieces > job.click)
		{
			RCBOT_DIAG("[RCBOT2] deploy wall-point userid=%d refinery=%d point=%d pieces=%d stored=%d tries=%d\n", userId,
				job.refinery, job.segment, pieces, stored, job.tries);
			// Remember which segment the new pieces belong to (their box centers can sit near a corner).
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && job.segment > 0; ++i)
			{
				edict_t* w = engine->PEntityOfEntIndex(i);
				if (!IsClass(w, "emp_eng_walls") || CClassInterface::getTeam(w) != team)
					continue;
				const int key = Key(i, w);
				if (std::find(job.before.begin(), job.before.end(), key) != job.before.end())
					continue;
				bool known = false;
				for (const auto& ps : job.pieceSeg)
					known = known || ps.first == key;
				if (!known && Dist2D(CBotGlobals::entityOrigin(w), job.aim[0]) < 900.0f)
					job.pieceSeg.emplace_back(key, job.segment);
			}
			job.click = pieces;
			job.tries = 0;
			job.firstPoint = false;
			if (++job.segment < 4)
			{
				job.stage = 1;
				job.stageAt = now;
				job.progressAt = now;
				job.bestDist = 1.0e9f;
				return true;
			}
			job.segment = 3;
			job.built = 0; // stage 5 picks the first unfinished piece
			job.stage = 5;
			job.stageAt = now;
			job.nextNav = 0.0f;
			return true;
		}
		if (now - job.stageAt < 2.0f)
			return true;
	}
	int count = 0;
	edict_t* placed = job.item == 6 ? nullptr : NewOwnNear(ItemClass(job.item), team, origin, 400.0f, job.before, &count);
	if (placed != nullptr)
	{
		int health = -1, maxHealth = -1;
		readEmpiresEntityIntProperty(placed, "m_iHealth", health);
		readEmpiresEntityIntProperty(placed, "m_iMaxHealth", maxHealth);
		RCBOT_DIAG("[RCBOT2] deploy placed userid=%d item=%s refinery=%d segment=%d entity=%d pieces=%d health=%d/%d "
			"tries=%d\n", userId, ItemName(job.item), job.refinery, job.segment, engine->IndexOfEdict(placed), count, health,
			maxHealth, job.tries);
		job.built = engine->IndexOfEdict(placed);
		job.stage = 5;
		job.stageAt = now;
		job.nextNav = 0.0f;
		return true;
	}
	if (now - job.stageAt < 2.0f)
		return true;
	{
		int item = -1, detBuilt = -1, detAllotted = -1, wallsBuilt = -1, wallsAllotted = -1;
		float resources = -1.0f;
		readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerBuildItem", item);
		readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerDetectionBuilt", detBuilt);
		readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerDetectionAllotted", detAllotted);
		readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerWallsBuilt", wallsBuilt);
		readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerWallsAllotted", wallsAllotted);
		EmpiresTeamResources(team, resources);
		RCBOT_DIAG("[RCBOT2] deploy miss userid=%d item=%s/%d detect=%d/%d walls=%d/%d resources=%.0f view=(%.0f %.0f) "
			"try=%d\n", userId, ItemName(job.item), item, detBuilt, detAllotted, wallsBuilt, wallsAllotted, resources,
			m_vViewAngles.x, m_vViewAngles.y, job.tries);
		if (job.item == 6 && job.segment > 0)
		{
			// What stands in the new piece's line (CompleteWalls box trace rejects any non-world entity).
			int nearIdx = -1;
			float nearDist = 120.0f;
			for (int i = 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				if (e == nullptr || e->IsFree() || e == m_pEdict || e->GetCollideable() == nullptr ||
					e->GetCollideable()->GetSolid() == SOLID_NONE || (i <= gpGlobals->maxClients && !CBotGlobals::entityIsAlive(e)))
					continue;
				const float d = SegDist2D(CBotGlobals::entityOrigin(e), pts[job.segment - 1], pts[job.segment]);
				if (d < nearDist)
				{
					nearDist = d;
					nearIdx = i;
				}
			}
			edict_t* ne = nearIdx > 0 ? engine->PEntityOfEntIndex(nearIdx) : nullptr;
			RCBOT_DIAG("[RCBOT2] deploy wall-line userid=%d point=%d stored=%d near=%d/%s dist=%.0f bot_dist=%.0f\n", userId,
				job.segment, KitWallPoints(weapon),
				nearIdx, ne != nullptr && ne->GetClassName() != nullptr ? ne->GetClassName() : "-", nearDist,
				SegDist2D(origin, pts[job.segment - 1], pts[job.segment]));
		}
	}
	if (job.item == 6 && job.click > 0 && job.tries + 1 >= kMaxTries)
	{
		// Keep the pieces that stand and build them up; the rest of the chain is given up.
		RCBOT_DIAG("[RCBOT2] stall-escalate who=deploy stage=4 reason=wall-point-%d-rejected pieces=%d\n", job.segment,
			job.click);
		job.built = 0;
		job.stage = 5;
		job.stageAt = now;
		job.nextNav = 0.0f;
		return true;
	}
	if (++job.tries >= kMaxTries)
	{
		if (job.item == 5)
			job.radarBlockedUntil = now + 300.0f;
		RCBOT_DIAG("[RCBOT2] stall-escalate who=deploy stage=4 reason=no-%s\n", ItemName(job.item));
		return stop("not-placed", 300.0f, 10.0f);
	}
	if (job.item != 6)
		job.click = 0;
	job.stage = job.item == 6 ? 1 : 2; // walls: walk to the farther stand first
	job.progressAt = now;
	job.bestDist = 1.0e9f;
	job.stageAt = now;
	return true;
}

void EmpiresDeployablesLevelReset()
{
	for (Mark& m : s_claim)
		m = Mark();
	for (Mark& m : s_walled)
		m = Mark();
}
