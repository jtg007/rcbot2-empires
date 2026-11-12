/* Minimal production-facing NavMesh path API for RCBot2. */
#include <set>
#include <map>
#include "bot_navmesh_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <cstring>
#include <vector>

#include "eiface.h"
#include "edict.h"
#include "engine/IEngineTrace.h"
#include "engine/ICollideable.h"
#include "collisionutils.h"
#include "cmodel.h"
#include "bot_empires_telemetry.h"
#include "bot_navigation_experience.h"
#include "navmesh/nav_mesh/nav_mesh.h"
#include "navmesh/nav_mesh/nav_pathfind.h"
#include "navmesh/nav_mesh/nav_ladder.h"
#include "filesystem.h"
extern IFileSystem* filesystem;

CNavMesh *TheNavMesh = nullptr;

// The Empires server does not export these SDK collision helpers to plugin
// DSOs. The navmesh spike only needs their local geometry behavior; keeping
// the definitions here avoids depending on private server symbols.
float IntersectRayWithTriangle(const Ray_t &ray, const Vector &v1,
	const Vector &v2, const Vector &v3, bool oneSided)
{
	const Vector edge1 = v2 - v1;
	const Vector edge2 = v3 - v1;
	const Vector pvec = CrossProduct(ray.m_Delta, edge2);
	const float determinant = DotProduct(edge1, pvec);

	if (oneSided && determinant <= 0.0f)
		return -1.0f;
	if (std::fabs(determinant) < 1e-6f)
		return -1.0f;

	const float inverseDeterminant = 1.0f / determinant;
	const Vector tvec = ray.m_Start - v1;
	const float u = DotProduct(tvec, pvec) * inverseDeterminant;
	if (u < 0.0f || u > 1.0f)
		return -1.0f;

	const Vector qvec = CrossProduct(tvec, edge1);
	const float v = DotProduct(ray.m_Delta, qvec) * inverseDeterminant;
	if (v < 0.0f || u + v > 1.0f)
		return -1.0f;

	const float distance = DotProduct(edge2, qvec) * inverseDeterminant;
	return distance < 0.0f || distance > 1.0f ? -1.0f : distance;
}

bool IsBoxIntersectingBox(const Vector &boxMin1, const Vector &boxMax1,
	const Vector &boxMin2, const Vector &boxMax2)
{
	return !(boxMin1.x > boxMax2.x || boxMax1.x < boxMin2.x
		|| boxMin1.y > boxMax2.y || boxMax1.y < boxMin2.y
		|| boxMin1.z > boxMax2.z || boxMax1.z < boxMin2.z);
}

namespace
{
bool s_loadAttempted = false;

void AppendPoint(RCBotNavMesh::Path &path, const Vector &point)
{
	if (!path.points.empty() &&
		(path.points.back() - point).LengthSqr() <= 24.0f * 24.0f)
		return;
	path.points.push_back(point);
}

Vector InsetPortalPoint(const CNavArea *area, const Vector &portalPoint,
	float maxInset)
{
	if (area == nullptr)
		return portalPoint;

	Vector towardCenter = area->GetCenter() - portalPoint;
	towardCenter.z = 0.0f;
	const float distance = towardCenter.Length2D();
	if (distance <= 1.0f)
		return portalPoint;

	// ComputeClosestPointInPortal deliberately returns a point on the shared
	// edge. Move only one generation step into the destination area so a
	// player hull does not have to turn while straddling that edge.
	const float inset = std::min(maxInset, 0.5f * distance);
	Vector point = portalPoint + towardCenter * (inset / distance);
	point.z = area->GetZ(point);
	if (area->Contains(point))
		return point;

	// The area is narrower than the fixed inset. Its center can be far along a
	// long strip, so stay at the nearest point of the area to the portal.
	Vector closest;
	area->GetClosestPointOnArea(point, &closest);
	closest.z = area->GetZ(closest);
	return closest;
}

}

extern NavAreaVector TheNavAreas;
extern IEngineTrace *enginetrace;
extern IVEngineServer *engine;
extern CGlobalVars *gpGlobals;
extern IServerGameEnts *servergameents;

namespace RCBotNavMesh
{
// Area links that the mesh has but the world does not (a bot hit worldspawn
// there and could not move on). Valid for the loaded map only.
std::set<std::pair<unsigned, unsigned>> s_blockedEdges;
std::map<std::pair<unsigned, unsigned>, float> s_timedBlockedEdges; // until gpGlobals->curtime
// Links a vehicle got stuck on (infantry may still use them).
std::map<std::pair<unsigned, unsigned>, float> s_vehicleBlockedEdges;

unsigned s_blockGeneration = 0;

unsigned BlockGeneration()
{
	return s_blockGeneration;
}

void BlockEdge(unsigned fromAreaId, unsigned toAreaId, const float seconds, const bool learned)
{
	if (learned)
		++s_blockGeneration;
	if (seconds > 0.0f && gpGlobals != nullptr)
		s_timedBlockedEdges[{fromAreaId, toAreaId}] = gpGlobals->curtime + seconds;
	else
		s_blockedEdges.insert({fromAreaId, toAreaId});
}

bool BlockEdgeNearestOnPath(const Vector &position, const std::vector<unsigned int> &areaIds,
	unsigned &fromAreaId, unsigned &toAreaId, const float seconds)
{
	if (!IsLoaded())
		return false;
	float best = std::numeric_limits<float>::max();
	std::size_t bestIndex = areaIds.size();
	for (std::size_t i = 0; i + 1 < areaIds.size(); ++i)
	{
		CNavArea *area = TheNavMesh->GetNavAreaByID(areaIds[i]);
		if (area == nullptr)
			continue;
		Vector closest;
		area->GetClosestPointOnArea(position, &closest);
		const float distance = (closest - position).Length();
		if (distance < best)
		{
			best = distance;
			bestIndex = i;
		}
	}
	// Only a nearby area can be the one the bot is stuck on.
	if (bestIndex >= areaIds.size() || best > 64.0f)
		return false;
	fromAreaId = areaIds[bestIndex];
	toAreaId = areaIds[bestIndex + 1];
	BlockEdge(fromAreaId, toAreaId, seconds);
	return true;
}

// Slab test of the segment a->b against the box.
static bool SegmentCrossesBox(const Vector &a, const Vector &b, const Vector &mins, const Vector &maxs)
{
	float t0 = 0.0f, t1 = 1.0f;
	for (int axis = 0; axis < 3; ++axis)
	{
		const float d = b[axis] - a[axis];
		if (std::fabs(d) < 1e-4f)
		{
			if (a[axis] < mins[axis] || a[axis] > maxs[axis])
				return false;
			continue;
		}
		float ta = (mins[axis] - a[axis]) / d, tb = (maxs[axis] - a[axis]) / d;
		if (ta > tb)
			std::swap(ta, tb);
		t0 = std::max(t0, ta);
		t1 = std::min(t1, tb);
		if (t0 > t1)
			return false;
	}
	return true;
}

int BlockEdgesBehindBox(const Vector &from, const Vector &mins, const Vector &maxs, const float seconds)
{
	if (!IsLoaded())
		return 0;
	// From inside the box every segment to a portal "crosses" it, so every link of every
	// overlapped area would be blocked, the way back too (as_escort barricade 346, rotated
	// plank with a 350x545 box: bot inside it, no-path to anything for 60 s).
	if (from.x >= mins.x && from.x <= maxs.x && from.y >= mins.y && from.y <= maxs.y)
		return 0;
	// One generated area can lie under a closed door, half on each side (as_escort
	// gate 228: area 39 x=[-750 -225], gate near x=-300). A bot in the outer strip then
	// plans 39->2, 39->269, ... straight through the gate, and no link crosses the box.
	// Block the links of such areas whose shared edge lies behind the door seen from 'from'.
	const Vector lift(0, 0, 20);
	int count = 0;
	for (CNavArea *area : TheNavAreas)
	{
		if (area == nullptr)
			continue;
		const Vector &nw = area->GetCorner(NORTH_WEST);
		const Vector &se = area->GetCorner(SOUTH_EAST);
		const float topZ = std::max(nw.z, se.z) + 20.0f;
		if (se.x < mins.x || nw.x > maxs.x || se.y < mins.y || nw.y > maxs.y ||
			topZ < mins.z || std::min(nw.z, se.z) > maxs.z)
			continue;
		for (int dir = 0; dir < NUM_DIRECTIONS; ++dir)
			for (int k = 0; k < area->GetAdjacentCount(static_cast<NavDirType>(dir)); ++k)
			{
				CNavArea *other = area->GetAdjacentArea(static_cast<NavDirType>(dir), k);
				if (other == nullptr)
					continue;
				Vector portal;
				float halfWidth = 0.0f;
				area->ComputePortal(other, static_cast<NavDirType>(dir), &portal, &halfWidth);
				if (!SegmentCrossesBox(from + lift, portal + lift, mins, maxs))
					continue;
				BlockEdge(area->GetID(), other->GetID(), seconds);
				++count;
			}
	}
	return count;
}

int BlockEdgesThroughBox(const Vector &mins, const Vector &maxs, const float seconds, const bool learned)
{
	if (!IsLoaded())
		return 0;
	const auto crosses = [&mins, &maxs](const Vector &a, const Vector &b)
	{
		return SegmentCrossesBox(a, b, mins, maxs);
	};
	const Vector middle = (mins + maxs) * 0.5f;
	const float reach = (maxs - mins).Length() + 1500.0f;
	int count = 0;
	FOR_EACH_VEC(TheNavAreas, i)
	{
		CNavArea *area = TheNavAreas[i];
		if ((area->GetCenter() - middle).Length() > reach)
			continue;
		for (int dir = 0; dir < NUM_DIRECTIONS; ++dir)
			for (int k = 0; k < area->GetAdjacentCount(static_cast<NavDirType>(dir)); ++k)
			{
				CNavArea *other = area->GetAdjacentArea(static_cast<NavDirType>(dir), k);
				// Feet height of a walking player: the box test uses centres lifted by 20.
				if (other == nullptr || !crosses(area->GetCenter() + Vector(0, 0, 20), other->GetCenter() + Vector(0, 0, 20)))
					continue;
				BlockEdge(area->GetID(), other->GetID(), seconds, learned);
				++count;
			}
	}
	return count;
}

int BlockDropsUnderFloor(const Vector &floorPoint, const float radius)
{
	if (!IsLoaded())
		return 0;
	int count = 0;
	for (CNavArea *area : TheNavAreas)
	{
		if (area == nullptr)
			continue;
		Vector top;
		area->GetClosestPointOnArea(floorPoint, &top);
		// Areas on this floor near the point...
		if ((top - floorPoint).Length2D() > radius || std::fabs(top.z - floorPoint.z) > 40.0f)
			continue;
		for (int dir = 0; dir < NUM_DIRECTIONS; ++dir)
			for (int k = 0; k < area->GetAdjacentCount(static_cast<NavDirType>(dir)); ++k)
			{
				CNavArea *other = area->GetAdjacentArea(static_cast<NavDirType>(dir), k);
				if (other == nullptr)
					continue;
				// ...linked down to an area that lies under the same spot.
				Vector below;
				other->GetClosestPointOnArea(floorPoint, &below);
				if ((below - floorPoint).Length2D() > radius || floorPoint.z - below.z < 64.0f)
					continue;
				BlockEdge(area->GetID(), other->GetID(), 0.0f);
				++count;
			}
	}
	return count;
}

// bot.h cannot be included next to the navmesh headers; bot_navmesh_provider.cpp defines it.
int ObserverTeam(CBot* bot);

struct DangerCircle
{
	Vector center;
	float radius = 0.0f;
};
DangerCircle s_teamDanger[4];

void SetTeamDangerCircle(const int team, const Vector &center, const float radius)
{
	if (team < 0 || team >= 4)
		return;
	const bool changed = s_teamDanger[team].radius != radius || s_teamDanger[team].center != center;
	s_teamDanger[team] = { center, radius > 0.0f ? radius : 0.0f };
	if (changed)
		++s_blockGeneration;
}

void ClearBlockedEdges()
{
	s_blockedEdges.clear();
	s_timedBlockedEdges.clear();
	s_vehicleBlockedEdges.clear();
	for (DangerCircle &circle : s_teamDanger)
		circle.radius = 0.0f;
}

const char *PathResultName(const PathResult result)
{
	switch (result)
	{
	case PathResult::SUCCESS:
		return "success";
	case PathResult::NAVMESH_NOT_LOADED:
		return "navmesh-not-loaded";
	case PathResult::START_AREA_NOT_FOUND:
		return "start-area-not-found";
	case PathResult::GOAL_AREA_NOT_FOUND:
		return "goal-area-not-found";
	case PathResult::NO_PATH:
		return "no-path";
	}
	return "unknown";
}

void Initialize()
{
	if (TheNavMesh == nullptr)
		TheNavMesh = new CNavMesh();
}

void LevelInit()
{
	s_loadAttempted = false;
}

namespace
{
// Ladders on Empires maps are world brushes with CONTENTS_LADDER, not entities,
// so CNavMesh::BuildLadders (name search) finds none and the .nav files have no
// ladders (as_escort: 25 ladder brushes, BE base exit and NF upper level).
// Read the brush bounds from the BSP and let the Valve code connect them.
int AddLaddersFromBsp()
{
	if (filesystem == nullptr || gpGlobals == nullptr || TheNavMesh == nullptr)
		return -1;
	char name[256];
	std::snprintf(name, sizeof(name), "maps/%s.bsp", STRING(gpGlobals->mapname));
	FileHandle_t file = filesystem->Open(name, "rb", "GAME");
	if (file == nullptr)
		return -1;
	const int size = static_cast<int>(filesystem->Size(file));
	std::vector<unsigned char> data(size > 0 ? size : 0);
	const int got = size > 0 ? filesystem->Read(data.data(), size, file) : 0;
	filesystem->Close(file);
	if (got != size || size < 8 + 64 * 16)
		return -1;
	auto lump = [&](int index, int& offset, int& length)
	{
		std::memcpy(&offset, &data[8 + index * 16], 4);
		std::memcpy(&length, &data[8 + index * 16 + 4], 4);
		return offset >= 0 && length >= 0 && offset + length <= size;
	};
	int planeOfs, planeLen, brushOfs, brushLen, sideOfs, sideLen;
	if (!lump(1, planeOfs, planeLen) || !lump(18, brushOfs, brushLen) || !lump(19, sideOfs, sideLen))
		return -1;
	const int planes = planeLen / 20, brushes = brushLen / 12, sides = sideLen / 8;
	int added = 0;
	for (int b = 0; b < brushes; ++b)
	{
		int firstSide, numSides, contents;
		std::memcpy(&firstSide, &data[brushOfs + b * 12], 4);
		std::memcpy(&numSides, &data[brushOfs + b * 12 + 4], 4);
		std::memcpy(&contents, &data[brushOfs + b * 12 + 8], 4);
		if ((contents & CONTENTS_LADDER) == 0 || firstSide < 0 || firstSide + numSides > sides)
			continue;
		Vector mins(-1.0e9f, -1.0e9f, -1.0e9f), maxs(1.0e9f, 1.0e9f, 1.0e9f);
		for (int s = firstSide; s < firstSide + numSides; ++s)
		{
			unsigned short planeNum;
			std::memcpy(&planeNum, &data[sideOfs + s * 8], 2);
			if (planeNum >= planes)
				continue;
			float plane[4];
			std::memcpy(plane, &data[planeOfs + planeNum * 20], 16);
			for (int axis = 0; axis < 3; ++axis)
			{
				if (plane[axis] > 0.999f)
					maxs[axis] = plane[3];
				else if (plane[axis] < -0.999f)
					mins[axis] = -plane[3];
			}
		}
		if (mins.x < -1.0e8f || maxs.x > 1.0e8f || mins.z < -1.0e8f || maxs.z > 1.0e8f ||
			maxs.z - mins.z < 48.0f)
			continue;
		TheNavMesh->CreateLadder(mins, maxs, 0.0f);
		const CNavLadder* ladder = TheNavMesh->GetLadders().Tail();
		Msg("[RCBOT2] navmesh-ladder add bottom=(%.0f %.0f %.0f) top=(%.0f %.0f %.0f) dir=%d bottom_area=%d top_area=%d\n",
			ladder->m_bottom.x, ladder->m_bottom.y, ladder->m_bottom.z,
			ladder->m_top.x, ladder->m_top.y, ladder->m_top.z, static_cast<int>(ladder->GetDir()),
			ladder->m_bottomArea != nullptr ? static_cast<int>(ladder->m_bottomArea->GetID()) : -1,
			ladder->m_topForwardArea != nullptr ? static_cast<int>(ladder->m_topForwardArea->GetID()) :
			(ladder->m_topBehindArea != nullptr ? static_cast<int>(ladder->m_topBehindArea->GetID()) : -1));
		++added;
	}
	return added;
}

const CNavLadder* FindLadderBetween(const CNavArea* from, const CNavArea* to, const bool up)
{
	const NavLadderConnectVector* list = from->GetLadders(up ? CNavLadder::LADDER_UP : CNavLadder::LADDER_DOWN);
	FOR_EACH_VEC((*list), i)
	{
		const CNavLadder* ladder = (*list)[i].ladder;
		if (ladder == nullptr)
			continue;
		if (up ? (ladder->m_topForwardArea == to || ladder->m_topLeftArea == to ||
			ladder->m_topRightArea == to || ladder->m_topBehindArea == to) : ladder->m_bottomArea == to)
			return ladder;
	}
	return nullptr;
}
}

void Frame(const bool simulating)
{
	if (!simulating || TheNavMesh == nullptr)
		return;

	if (!s_loadAttempted)
	{
		const NavErrorType result = TheNavMesh->Load();
		Msg("[RCBOT2] NavMesh load result=%d areas=%u\n",
			static_cast<int>(result), TheNavMesh->GetNavAreaCount());
		if (TheNavMesh->IsLoaded() && TheNavMesh->GetLadders().Count() == 0)
			Msg("[RCBOT2] navmesh-ladder bsp added=%d\n", AddLaddersFromBsp());
		s_loadAttempted = true;
	}

	// p_nav_generate runs inside Update(); a map without .nav is not loaded yet.
	if (TheNavMesh->IsLoaded() || TheNavMesh->IsGenerating())
		TheNavMesh->Update();
}

void LevelShutdown()
{
	if (TheNavMesh != nullptr)
		TheNavMesh->Reset();

	s_loadAttempted = false;
	ClearBlockedEdges();
}

void Shutdown()
{
	if (TheNavMesh == nullptr)
		return;

	TheNavMesh->Reset();
	delete TheNavMesh;
	TheNavMesh = nullptr;
	s_loadAttempted = false;
}

bool IsLoaded()
{
	return TheNavMesh != nullptr && TheNavMesh->IsLoaded();
}

LocalGroundSamples CollectLocalGroundSamples(const Vector& origin, float radius,
	unsigned /*maxCells*/, unsigned /*maxEntries*/, unsigned maxAreas)
{
	LocalGroundSamples result;
	if (!IsLoaded() || !origin.IsValid() || !std::isfinite(radius) || radius <= 0.0f)
		return result;
	result.available = true;

	std::vector<std::pair<float, CNavArea*>> nearby;
	FOR_EACH_VEC(TheNavAreas, i)
	{
		CNavArea* area = TheNavAreas[i];
		const float distance = (area->GetCenter() - origin).Length();
		if (distance <= radius)
			nearby.emplace_back(distance, area);
	}
	std::sort(nearby.begin(), nearby.end(),
		[](const std::pair<float, CNavArea*>& a, const std::pair<float, CNavArea*>& b)
		{ return a.first < b.first; });
	if (nearby.size() > maxAreas)
	{
		nearby.resize(maxAreas);
		result.truncated = true;
	}
	result.areasInspected = static_cast<unsigned>(nearby.size());

	for (const auto& entry : nearby)
	{
		const CNavArea* area = entry.second;
		Vector closest;
		area->GetClosestPointOnArea(origin, &closest);
		Vector points[6] = {area->GetCenter(), closest};
		for (int corner = 0; corner < 4; ++corner)
			points[corner + 2] = (area->GetCorner(static_cast<NavCornerType>(corner)) + area->GetCenter()) * 0.5f;
		for (Vector point : points)
		{
			point.z = area->GetZ(point);
			if (point.IsValid() && area->IsOverlapping(point))
				result.samples.push_back({area->GetID(), point});
		}
	}
	return result;
}

unsigned int GetAreaCount()
{
	return TheNavMesh != nullptr ? TheNavMesh->GetNavAreaCount() : 0;
}

int GetNearestAreaId(const Vector &position)
{
	if (!IsLoaded() || !position.IsValid())
		return -1;
	CNavArea *area = TheNavMesh->GetNearestNavArea(position);
	return area != nullptr ? static_cast<int>(area->GetID()) : -1;
}

bool ClosestPointOnMesh(const Vector &position, const float maxDistance, Vector &out)
{
	if (!IsLoaded() || !position.IsValid())
		return false;
	CNavArea *area = TheNavMesh->GetNearestNavArea(position, maxDistance, false, false);
	if (area == nullptr)
		return false;
	area->GetClosestPointOnArea(position, &out);
	return (out - position).Length() <= maxDistance;
}

int GetAreaIdAtPosition(const Vector &position)
{
	if (!IsLoaded() || !position.IsValid())
		return -1;
	CNavArea *area = TheNavMesh->GetNavArea(position);
	return area != nullptr ? static_cast<int>(area->GetID()) : -1;
}

namespace
{
// A standing jump clears about 42 units; higher links (e.g. onto railings) strand bots.
constexpr float kMaxClimbHeight = 40.0f;
constexpr float kMaxDropHeight = 200.0f;

class LearnedNavMeshCost
{
public:
	LearnedNavMeshCost(CBot* observer) : m_observer(observer)
	{
		const int team = observer != nullptr ? ObserverTeam(observer) : -1;
		if (team >= 0 && team < 4 && s_teamDanger[team].radius > 0.0f)
		{
			m_dangerCenter = s_teamDanger[team].center;
			m_dangerRadius = s_teamDanger[team].radius;
		}
	}

	float operator()(CNavArea* area, CNavArea* fromArea,
		const CNavLadder* ladder, const CFuncElevator* elevator,
		const float length) const
	{
		const float baseCost = m_base(area, fromArea, ladder, elevator, length);
		if (baseCost < 0.0f || fromArea == nullptr || area == nullptr)
			return baseCost;
		// Generated meshes also link ledges that can only be dropped down from.
		if (s_blockedEdges.count({fromArea->GetID(), area->GetID()}) != 0)
			return -1.0f;
		const auto timed = s_timedBlockedEdges.find({fromArea->GetID(), area->GetID()});
		if (timed != s_timedBlockedEdges.end() && gpGlobals != nullptr && gpGlobals->curtime < timed->second)
			return -1.0f;
		if (m_dangerRadius > 0.0f &&
			(area->GetCenter() - m_dangerCenter).Length2D() < m_dangerRadius &&
			(fromArea->GetCenter() - m_dangerCenter).Length2D() >= m_dangerRadius)
			return -1.0f;
		if (ladder == nullptr && elevator == nullptr)
		{
			const float heightChange = fromArea->ComputeAdjacentConnectionHeightChange(area);
			// Deep drops hurt or kill, and on generated meshes they often run
			// through walls (emp_streetsoffire (2243 4584): 320 down behind a wall).
			if (heightChange > kMaxClimbHeight || heightChange < -kMaxDropHeight)
				return -1.0f;
		}
		// The existing Transition Experience records NavMesh floor crossings
		// with UNKNOWN traversal. Keep that identity stable; no guessing from
		// geometry is allowed to create a different learned key.
		const std::int64_t fromNode =
			static_cast<std::int64_t>(fromArea->GetID());
		const std::int64_t toNode = static_cast<std::int64_t>(area->GetID());
		const auto traversal = RCBotNavigationExperience::Traversal::UNKNOWN;
		const auto learned = EmpiresAnalysis::EvaluateLearnedNavigationCost(
			RCBotNavigationExperience::Provider::NAVMESH, fromNode, toNode,
			traversal);
		const float effectiveCost = baseCost +
			static_cast<float>(learned.penalty);
		if (learned.penalty > 0.0 && m_observer != nullptr)
			EmpiresAnalysis::RecordLearnedNavigationEdgeConsidered(m_observer,
				RCBotNavigationExperience::Provider::NAVMESH, fromNode, toNode,
				traversal, baseCost, effectiveCost, learned);
		return effectiveCost;
	}

private:
	ShortestPathCost m_base;
	CBot* m_observer = nullptr;
	Vector m_dangerCenter;
	float m_dangerRadius = 0.0f;
};
}

namespace
{
// The navmesh is baked without buildings, so routes run straight through them
// (and through the walls of the barracks bots spawn in). Buildings can also
// appear at runtime, so detours are planned against their real collision.
constexpr float kDetourCell = 32.0f;
// Hull half-width for the detour planner: 16 for players, half the vehicle
// width while BuildVehiclePath runs (a tank followed player detours into the
// vehicle factory bay, run 20261112-120043).
float g_detourHalf = 16.0f;
float DetourMargin() { return 96.0f + (g_detourHalf - 16.0f); }
constexpr float kDetourFloorClearance = 12.0f;
constexpr int kDetourMaxCells = 8100;

struct BuildingBox
{
	IHandleEntity* handle;
	Vector mins;
	Vector maxs;
	const char* className;
};

std::vector<BuildingBox> CollectSolidBuildings()
{
	std::vector<BuildingBox> boxes;
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* edict = engine->PEntityOfEntIndex(index);
		if (edict == nullptr || edict->IsFree())
			continue;
		const char* name = edict->GetClassName();
		// The physbox only stops vehicles; players walk through it, and it
		// fills the hollow interior of the building it belongs to.
		// The Commander vehicle stands in the base like a building and is just as unknown
		// to the mesh (BE Engineer pushed into emp_imp_commander for 245 s).
		const bool commanderVehicle = name != nullptr &&
			(std::strcmp(name, "emp_nf_commander") == 0 || std::strcmp(name, "emp_imp_commander") == 0);
		if (name == nullptr || (!commanderVehicle && (std::strncmp(name, "emp_building_", 13) != 0 ||
			std::strstr(name, "physbox") != nullptr)))
			continue;
		ICollideable* collideable = edict->GetCollideable();
		IServerEntity* serverEntity = edict->GetIServerEntity();
		if (collideable == nullptr || serverEntity == nullptr ||
			collideable->GetSolid() == SOLID_NONE)
			continue;
		BuildingBox box;
		box.handle = serverEntity;
		box.className = name;
		collideable->WorldSpaceSurroundingBounds(&box.mins, &box.maxs);
		boxes.push_back(box);
	}
	return boxes;
}

bool InPaddedBox(const Vector& point, const BuildingBox& box, float pad)
{
	return point.x >= box.mins.x - pad && point.x <= box.maxs.x + pad &&
		point.y >= box.mins.y - pad && point.y <= box.maxs.y + pad;
}

// 2D slab test; on a hit, enter is where the segment first touches the box.
bool SegmentHitsBox(const Vector& a, const Vector& b, const BuildingBox& box,
	float pad, float* enter = nullptr)
{
	float tMin = 0.0f;
	float tMax = 1.0f;
	for (int axis = 0; axis < 2; ++axis)
	{
		const float delta = b[axis] - a[axis];
		const float low = box.mins[axis] - pad;
		const float high = box.maxs[axis] + pad;
		if (std::fabs(delta) < 1e-4f)
		{
			if (a[axis] < low || a[axis] > high)
				return false;
			continue;
		}
		float t0 = (low - a[axis]) / delta;
		float t1 = (high - a[axis]) / delta;
		if (t0 > t1)
			std::swap(t0, t1);
		tMin = std::max(tMin, t0);
		tMax = std::min(tMax, t1);
		if (tMin > tMax)
			return false;
	}
	if (enter != nullptr)
		*enter = tMin;
	return true;
}

bool HullClearOfBuilding(const Vector& a, const Vector& b, const BuildingBox& box)
{
	Ray_t ray;
	ray.Init(a, b, Vector(-g_detourHalf, -g_detourHalf, 0.0f), Vector(g_detourHalf, g_detourHalf, 72.0f));
	trace_t hit;
	enginetrace->ClipRayToEntity(ray, MASK_PLAYERSOLID, box.handle, &hit);
	return !hit.startsolid && hit.fraction >= 1.0f;
}

bool HullHitsWorld(const Vector& point)
{
	Ray_t ray;
	ray.Init(point, point, Vector(-g_detourHalf, -g_detourHalf, 0.0f), Vector(g_detourHalf, g_detourHalf, 72.0f));
	CTraceFilterWorldOnly filter;
	trace_t hit;
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &hit);
	return hit.startsolid;
}

bool PlanDetour(const Vector& from, const Vector& target, float planeZ,
	const BuildingBox& box, const std::vector<BuildingBox>& boxes,
	std::vector<Vector>& detour)
{
	const float originX = box.mins.x - DetourMargin();
	const float originY = box.mins.y - DetourMargin();
	const int width = static_cast<int>((box.maxs.x - box.mins.x + 2.0f * DetourMargin()) / kDetourCell) + 1;
	const int height = static_cast<int>((box.maxs.y - box.mins.y + 2.0f * DetourMargin()) / kDetourCell) + 1;
	if (width <= 0 || height <= 0 || width * height > kDetourMaxCells)
		return false;

	std::vector<float> cellZ(width * height, planeZ);
	const auto cellPoint = [&](int x, int y)
	{
		return Vector(originX + x * kDetourCell, originY + y * kDetourCell, cellZ[y * width + x]);
	};
	std::vector<char> blocked(width * height, 0);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			char solid = 1;
			// Spawn floors can sit above the baked mesh. Find the lowest clear
			// hull up to the spawn height, retaining ground level outside.
			const float ceiling = InPaddedBox(from, box, 0.0f) ?
				std::max(planeZ, std::min(planeZ + 128.0f, from.z + kDetourFloorClearance)) : planeZ;
			for (float z = planeZ; z <= ceiling + 0.1f; z += 4.0f)
			{
				cellZ[y * width + x] = z;
				const Vector point = cellPoint(x, y);
				solid = HullHitsWorld(point) ? 1 : 0;
				for (const BuildingBox& other : boxes)
					if (!solid && InPaddedBox(point, other, 24.0f + g_detourHalf - 16.0f) &&
						!HullClearOfBuilding(point, point, other))
						solid = 1;
				if (!solid) break;
			}
			blocked[y * width + x] = solid;
		}

	// Enter the grid where the segment reaches it; a start inside stays put.
	Vector entry(from.x, from.y, planeZ);
	float enter = 0.0f;
	if (SegmentHitsBox(from, target, box, DetourMargin() - kDetourCell, &enter) && enter > 0.0f)
	{
		const Vector along = from + (target - from) * enter;
		entry = Vector(along.x, along.y, planeZ);
	}

	// A bot that has just spawned is still falling; at that height its hull
	// overlaps the barracks model. Start from the first clear height below.
	Vector actualEntry(entry.x, entry.y, std::max(entry.z, from.z + kDetourFloorClearance));
	const auto entrySolid = [&](const Vector& point)
	{
		for (const BuildingBox& other : boxes)
			if (!HullClearOfBuilding(point, point, other))
				return true;
		return HullHitsWorld(point);
	};
	while (actualEntry.z - 4.0f >= entry.z && entrySolid(actualEntry))
		actualEntry.z -= 4.0f;

	int startCell = -1;
	float startDistance = std::numeric_limits<float>::max();
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			if (blocked[y * width + x])
				continue;
			bool reachable = true;
			for (const BuildingBox& other : boxes)
				if (!HullClearOfBuilding(actualEntry, cellPoint(x, y), other))
					reachable = false;
			if (!reachable) continue;
			const float distance = (cellPoint(x, y) - entry).Length();
			if (distance < startDistance)
			{
				startDistance = distance;
				startCell = y * width + x;
			}
		}
	if (startCell < 0 || startDistance > kDetourCell * 4.0f)
	{
		Msg("[RCBOT2] nav-detour-fail reason=no-start entry=(%.0f %.0f %.0f) nearest=%.0f box=(%.0f %.0f)-(%.0f %.0f)\n",
			entry.x, entry.y, actualEntry.z, startDistance, box.mins.x, box.mins.y, box.maxs.x, box.maxs.y);
		return false;
	}

	std::vector<float> cost(width * height, std::numeric_limits<float>::max());
	std::vector<int> parent(width * height, -1);
	using QueueEntry = std::pair<float, int>;
	std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> open;
	cost[startCell] = 0.0f;
	open.push({0.0f, startCell});
	while (!open.empty())
	{
		const QueueEntry current = open.top();
		open.pop();
		if (current.first > cost[current.second])
			continue;
		const int cx = current.second % width;
		const int cy = current.second / width;
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0)
					continue;
				const int nx = cx + dx;
				const int ny = cy + dy;
				if (nx < 0 || ny < 0 || nx >= width || ny >= height ||
					blocked[ny * width + nx])
					continue;
				if (dx != 0 && dy != 0 &&
					(blocked[cy * width + nx] || blocked[ny * width + cx]))
					continue;
				const float rise = cellZ[ny * width + nx] - cellZ[current.second];
				if (rise > 40.0f || rise < -200.0f)
					continue;
				const float step = (dx != 0 && dy != 0) ? 1.4142f : 1.0f;
				const float nextCost = current.first + step * kDetourCell;
				const int next = ny * width + nx;
				if (nextCost < cost[next])
				{
					cost[next] = nextCost;
					parent[next] = current.second;
					open.push({nextCost, next});
				}
			}
	}

	// Leave the building at the cell with the shortest total way to the target
	// from which the rest of the way is really free.
	const Vector targetAtPlane(target.x, target.y, planeZ);
	std::vector<QueueEntry> exits;
	for (int cell = 0; cell < width * height; ++cell)
	{
		if (cost[cell] == std::numeric_limits<float>::max())
			continue;
		const Vector point = cellPoint(cell % width, cell / width);
		if (!InPaddedBox(point, box, 48.0f))
			exits.push_back({cost[cell] + (point - targetAtPlane).Length(), cell});
	}
	std::sort(exits.begin(), exits.end());
	int exitCell = -1;
	if (InPaddedBox(target, box, 48.0f))
	{
		// The route ends at the building itself: walk to the free cell nearest
		// to the target instead of looking for a way past the building.
		float nearest = kDetourCell * 2.0f;
		for (int cell = 0; cell < width * height; ++cell)
		{
			if (cost[cell] == std::numeric_limits<float>::max())
				continue;
			const float distance = (cellPoint(cell % width, cell / width) - targetAtPlane).Length();
			if (distance < nearest)
			{
				nearest = distance;
				exitCell = cell;
			}
		}
	}
	for (const QueueEntry& candidate : exits)
	{
		if (exitCell >= 0)
			break;
		const Vector point = cellPoint(candidate.second % width, candidate.second / width);
		if (HullClearOfBuilding(point, targetAtPlane, box))
		{
			exitCell = candidate.second;
			break;
		}
	}
	if (exitCell < 0)
	{
		Msg("[RCBOT2] nav-detour-fail reason=no-exit exits=%zu z=%.0f target=(%.0f %.0f %.0f) box=(%.0f %.0f)-(%.0f %.0f)\n",
			exits.size(), planeZ, target.x, target.y, target.z, box.mins.x, box.mins.y, box.maxs.x, box.maxs.y);
		return false;
	}

	std::vector<int> cells;
	for (int cell = exitCell; cell != -1; cell = parent[cell])
		cells.push_back(cell);
	std::reverse(cells.begin(), cells.end());
	Vector first = cellPoint(startCell % width, startCell / width);
	first.z -= kDetourFloorClearance;
	detour.push_back(first);

	const auto lineFree = [&](int fromCell, int toCell)
	{
		const Vector start = cellPoint(fromCell % width, fromCell / width);
		const Vector end = cellPoint(toCell % width, toCell / width);
		for (const BuildingBox& other : boxes)
			if (!HullClearOfBuilding(start, end, other))
				return false;
		Ray_t ray;
		ray.Init(start, end, Vector(-16, -16, 0), Vector(16, 16, 72));
		CTraceFilterWorldOnly filter;
		trace_t hit;
		enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &hit);
		if (hit.startsolid || hit.fraction < 1.0f)
			return false;
		const int steps = std::max(1, static_cast<int>((end - start).Length() / 8.0f));
		for (int i = 0; i <= steps; ++i)
		{
			const Vector sample = start + (end - start) * (static_cast<float>(i) / steps);
			const int x = static_cast<int>((sample.x - originX) / kDetourCell + 0.5f);
			const int y = static_cast<int>((sample.y - originY) / kDetourCell + 0.5f);
			if (x < 0 || y < 0 || x >= width || y >= height || blocked[y * width + x])
				return false;
		}
		return true;
	};

	// Keep only the corners, so bots do not zigzag along the grid.
	std::size_t anchor = 0;
	while (anchor + 1 < cells.size())
	{
		std::size_t furthest = anchor + 1;
		for (std::size_t probe = cells.size() - 1; probe > anchor + 1; --probe)
			if (lineFree(cells[anchor], cells[probe]))
			{
				furthest = probe;
				break;
			}
		Vector corner = cellPoint(cells[furthest] % width, cells[furthest] / width);
		corner.z -= kDetourFloorClearance;
		detour.push_back(corner);
		anchor = furthest;
	}
	return true;
}

void ApplyBuildingDetours(Path& path, bool skipGoalBuilding = false)
{
	if (enginetrace == nullptr || gpGlobals == nullptr || path.points.size() < 2)
		return;
	const auto started = std::chrono::steady_clock::now();
	const std::vector<BuildingBox> boxes = CollectSolidBuildings();
	int applied = 0;
	for (std::size_t i = 0; i + 1 < path.points.size() && applied < 6; ++i)
	{
		const Vector a = path.points[i];
		const Vector b = path.points[i + 1];
		const float planeZ = b.z + kDetourFloorClearance;
		for (const BuildingBox& box : boxes)
		{
			// Vehicles driving onto a repair pad: the goal building is the way in. Also a
			// goal right next to a building (BE armory beside the CV, run 20261112-182815):
			// the vehicle stops before it anyway.
			if (skipGoalBuilding && InPaddedBox(path.points.back(), box, 300.0f))
				continue;
			if (planeZ < box.mins.z || planeZ > box.maxs.z ||
				!SegmentHitsBox(a, b, box, 16.0f) ||
				HullClearOfBuilding(Vector(a.x, a.y, planeZ), Vector(b.x, b.y, planeZ), box))
				continue;

			// Plan to the first route point beyond this building.
			std::size_t last = i + 1;
			// Also skip points inside any other building: the mesh still has
			// areas under buildings placed after it was generated (barracks
			// detour ended inside the armory, run 205019).
			auto insideAnyBuilding = [&](const Vector& p) {
				for (const BuildingBox& other : boxes)
					if (p.z + kDetourFloorClearance >= other.mins.z && p.z + kDetourFloorClearance <= other.maxs.z &&
						InPaddedBox(p, other, 48.0f))
						return true;
				return false;
			};
			while (last + 1 < path.points.size() &&
				(InPaddedBox(path.points[last], box, 48.0f) || insideAnyBuilding(path.points[last])))
				++last;
			// A failed plan costs up to 80 ms and the same route is asked again every few
			// seconds (262 failures at one armory in a few minutes): remember failures 15 s.
			struct Failed { int bx, by, ax, ay, tx, ty; float until; };
			static std::vector<Failed> failed;
			const float now = gpGlobals->curtime;
			const Failed key{int(box.mins.x) / 64, int(box.mins.y) / 64, int(a.x) / 128, int(a.y) / 128,
				int(path.points[last].x) / 128, int(path.points[last].y) / 128, now + 15.0f};
			bool knownBad = false;
			for (const Failed& f : failed)
				knownBad = knownBad || (now < f.until && now >= f.until - 15.0f && f.bx == key.bx && f.by == key.by &&
					f.ax == key.ax && f.ay == key.ay && f.tx == key.tx && f.ty == key.ty);
			if (knownBad)
				break;
			std::vector<Vector> detour;
			const bool planned = PlanDetour(a, path.points[last], planeZ, box, boxes, detour);
			if (!planned)
			{
				failed.erase(std::remove_if(failed.begin(), failed.end(),
					[&](const Failed& f) { return now >= f.until || now < f.until - 15.0f; }), failed.end());
				failed.push_back(key);
			}
			const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - started).count();
			Msg("[RCBOT2] nav-detour building=%s planned=%d corners=%zu from=(%.0f %.0f) to=(%.0f %.0f) us=%lld\n",
				box.className, planned ? 1 : 0, detour.size(), a.x, a.y,
				path.points[last].x, path.points[last].y, static_cast<long long>(micros));
			if (planned)
			{
				path.points.erase(path.points.begin() + i + 1, path.points.begin() + last);
				path.points.insert(path.points.begin() + i + 1, detour.begin(), detour.end());
				i += detour.size();
				++applied;
			}
			break;
		}
	}
}
}

PathResult BuildPath(const Vector &from, const Vector &to, Path &path,
	CBot* observer)
{
	path.clear();

	if (!IsLoaded())
		return PathResult::NAVMESH_NOT_LOADED;

	CNavArea *startArea = TheNavMesh->GetNearestNavArea(from);
	// A bot standing on a prop or in the air fails the ground probe too.
	if (startArea == nullptr)
		startArea = TheNavMesh->GetNearestNavArea(from, 10000.0f, false, false);
	if (startArea == nullptr)
		return PathResult::START_AREA_NOT_FOUND;
	path.startAreaId = static_cast<int>(startArea->GetID());

	CNavArea *goalArea = TheNavMesh->GetNearestNavArea(to);
	// The ground probe fails for goals that sit inside geometry or over
	// water (emp_coast resource points); fall back to plain distance.
	if (goalArea == nullptr)
		goalArea = TheNavMesh->GetNearestNavArea(to, 10000.0f, false, false);
	if (goalArea == nullptr)
		return PathResult::GOAL_AREA_NOT_FOUND;
	path.goalAreaId = static_cast<int>(goalArea->GetID());

	CNavArea *closestArea = nullptr;
	if (!NavAreaBuildPath(startArea, goalArea, &to,
		LearnedNavMeshCost(observer), &closestArea))
		return PathResult::NO_PATH;
	if (closestArea != goalArea)
		return PathResult::NO_PATH;

	std::vector<CNavArea *> areaPath;
	for (CNavArea *area = goalArea; area != nullptr; area = area->GetParent())
		areaPath.push_back(area);
	if (areaPath.empty() || areaPath.back() != startArea)
		return PathResult::NO_PATH;

	std::reverse(areaPath.begin(), areaPath.end());
	goalArea->GetClosestPointOnArea(to, &path.walkGoal);
	path.targetAboveWalkPlane =
		(to - path.walkGoal).LengthSqr() > 48.0f * 48.0f;
	path.areaIds.reserve(areaPath.size());
	for (CNavArea *area : areaPath)
		path.areaIds.push_back(area->GetID());

	AppendPoint(path, from);
	Vector previous = from;
	for (std::size_t i = 0; i + 1 < areaPath.size(); ++i)
	{
		CNavArea *current = areaPath[i];
		CNavArea *next = areaPath[i + 1];
		const NavTraverseType traverse = next->GetParentHow();
		if (traverse == GO_LADDER_UP || traverse == GO_LADDER_DOWN)
		{
			const bool up = traverse == GO_LADDER_UP;
			const CNavLadder* ladder = FindLadderBetween(current, next, up);
			if (ladder == nullptr)
				return PathResult::NO_PATH;
			const Vector normal = ladder->GetNormal();
			Vector foot = ladder->m_bottom + normal * 24.0f;
			Vector upper = ladder->m_top - normal * 32.0f;
			if (up)
			{
				foot.z = current->GetZ(foot);
				next->GetClosestPointOnArea(upper, &upper);
				AppendPoint(path, foot);
				AppendPoint(path, upper);
				path.ladderPoints.push_back(upper);
				previous = upper;
			}
			else
			{
				current->GetClosestPointOnArea(upper, &upper);
				foot.z = next->GetZ(foot);
				AppendPoint(path, upper);
				AppendPoint(path, foot);
				path.ladderPoints.push_back(foot);
				previous = foot;
			}
			continue;
		}
		if (static_cast<int>(traverse) >= static_cast<int>(NUM_DIRECTIONS))
			return PathResult::NO_PATH;

		// Match Valve's Source SDK NextBot area crossing: choose the closest
		// point in the shared portal based on the current position, then keep
		// its Z on the area being exited.
		Vector transition = vec3_origin;
		const NavDirType direction = static_cast<NavDirType>(traverse);
		current->ComputeClosestPointInPortal(next, direction, previous,
			&transition);
		transition.z = current->GetZ(transition);
		// After a drop the point must clear the ledge the bot walks off;
		// otherwise it sits under the bot and can never be reached.
		const bool drop = transition.z - next->GetZ(transition) > 24.0f;
		transition = InsetPortalPoint(next, transition, drop ? 56.0f : 18.0f);
		AppendPoint(path, transition);
		previous = transition;
	}
	if (path.targetAboveWalkPlane)
		AppendPoint(path, path.walkGoal);
	else
		AppendPoint(path, to);

	ApplyBuildingDetours(path);

	for (std::size_t i = 1; i < path.points.size(); ++i)
		path.length += (path.points[i] - path.points[i - 1]).Length();
	return PathResult::SUCCESS;
}

namespace
{
bool FindDirectionTo(const CNavArea* from, const CNavArea* to, NavDirType& dir)
{
	for (int d = 0; d < NUM_DIRECTIONS; ++d)
		if (from->IsConnected(to, static_cast<NavDirType>(d)))
		{
			dir = static_cast<NavDirType>(d);
			return true;
		}
	return false;
}

// Vehicles: no ladders, no jump steps, no crouch holes, no deep drops; narrow
// links cost extra; links a vehicle got stuck on are closed for vehicles only.
class VehicleNavMeshCost
{
public:
	explicit VehicleNavMeshCost(float width) : m_width(width) {}
	float operator()(CNavArea* area, CNavArea* fromArea, const CNavLadder* ladder,
		const CFuncElevator* elevator, const float length) const
	{
		if (fromArea == nullptr)
			return 0.0f;
		if (ladder != nullptr || elevator != nullptr || area == nullptr)
			return -1.0f;
		if (area->GetAttributes() & (NAV_MESH_CROUCH | NAV_MESH_JUMP))
			return -1.0f;
		const std::pair<unsigned, unsigned> key{fromArea->GetID(), area->GetID()};
		if (s_blockedEdges.count(key) != 0)
			return -1.0f;
		const float now = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
		const auto timed = s_timedBlockedEdges.find(key);
		if (timed != s_timedBlockedEdges.end() && now < timed->second)
			return -1.0f;
		const auto vehicle = s_vehicleBlockedEdges.find(key);
		if (vehicle != s_vehicleBlockedEdges.end() && now < vehicle->second)
			return -1.0f;
		const float heightChange = fromArea->ComputeAdjacentConnectionHeightChange(area);
		if (heightChange > kVehicleMaxStep || heightChange < -kVehicleMaxDrop)
			return -1.0f;
		const float dist = length > 0.0f ? length : (area->GetCenter() - fromArea->GetCenter()).Length();
		float cost = dist + fromArea->GetCostSoFar();
		NavDirType dir;
		if (FindDirectionTo(fromArea, area, dir))
		{
			Vector center;
			float halfWidth = 0.0f;
			fromArea->ComputePortal(area, dir, &center, &halfWidth);
			if (2.0f * halfWidth < m_width)
				cost += dist * 4.0f;
		}
		// Small areas mean clutter (generated meshes cut around props and walls).
		if (std::min(area->GetSizeX(), area->GetSizeY()) < m_width * 0.6f)
			cost += dist * 1.5f;
		if (area->IsUnderwater())
			cost += dist * 2.0f;
		return cost;
	}

private:
	static constexpr float kVehicleMaxStep = 30.0f;
	static constexpr float kVehicleMaxDrop = 160.0f;
	float m_width;
};
}

PathResult BuildVehiclePath(const Vector &from, const Vector &to, Path &path, float width, bool allowPartial)
{
	path.clear();
	if (!IsLoaded())
		return PathResult::NAVMESH_NOT_LOADED;
	CNavArea *startArea = TheNavMesh->GetNearestNavArea(from);
	if (startArea == nullptr)
		startArea = TheNavMesh->GetNearestNavArea(from, 10000.0f, false, false);
	if (startArea == nullptr)
		return PathResult::START_AREA_NOT_FOUND;
	path.startAreaId = static_cast<int>(startArea->GetID());
	CNavArea *goalArea = TheNavMesh->GetNearestNavArea(to);
	if (goalArea == nullptr)
		goalArea = TheNavMesh->GetNearestNavArea(to, 10000.0f, false, false);
	if (goalArea == nullptr)
		return PathResult::GOAL_AREA_NOT_FOUND;
	path.goalAreaId = static_cast<int>(goalArea->GetID());

	CNavArea *closestArea = nullptr;
	const bool full = NavAreaBuildPath(startArea, goalArea, &to, VehicleNavMeshCost(width), &closestArea) &&
		closestArea == goalArea;
	Vector goal = to;
	if (!full)
	{
		// Rainbowroad: base to enemy CV had no full vehicle path, the vehicles drove
		// the straight line and hung at edges (run 20261112-163322). Take the way
		// to the nearest reachable area instead and plan again from there.
		if (!allowPartial || closestArea == nullptr || closestArea == startArea)
			return PathResult::NO_PATH;
		Vector near;
		closestArea->GetClosestPointOnArea(to, &near);
		if ((near - to).Length2D() > (from - to).Length2D() - 800.0f)
			return PathResult::NO_PATH;
		goalArea = closestArea;
		goal = near;
		path.goalAreaId = static_cast<int>(goalArea->GetID());
		path.partial = true;
	}
	std::vector<CNavArea *> areaPath;
	for (CNavArea *area = goalArea; area != nullptr; area = area->GetParent())
		areaPath.push_back(area);
	if (areaPath.empty() || areaPath.back() != startArea)
		return PathResult::NO_PATH;
	std::reverse(areaPath.begin(), areaPath.end());
	goalArea->GetClosestPointOnArea(goal, &path.walkGoal);
	for (CNavArea *area : areaPath)
		path.areaIds.push_back(area->GetID());

	AppendPoint(path, from);
	Vector previous = from;
	const float margin = width * 0.5f + 16.0f;
	for (std::size_t i = 0; i + 1 < areaPath.size(); ++i)
	{
		CNavArea *current = areaPath[i];
		CNavArea *next = areaPath[i + 1];
		const NavTraverseType traverse = next->GetParentHow();
		if (static_cast<int>(traverse) >= static_cast<int>(NUM_DIRECTIONS))
			return PathResult::NO_PATH;
		const NavDirType direction = static_cast<NavDirType>(traverse);
		// Closest point in the portal, but kept half a vehicle away from its ends.
		Vector center;
		float halfWidth = 0.0f;
		current->ComputePortal(next, direction, &center, &halfWidth);
		Vector point = center;
		const float room = std::max(0.0f, halfWidth - margin);
		if (direction == NORTH || direction == SOUTH)
			point.x = std::max(center.x - room, std::min(center.x + room, previous.x));
		else
			point.y = std::max(center.y - room, std::min(center.y + room, previous.y));
		point.z = current->GetZ(point);
		AppendPoint(path, point);
		previous = point;
	}
	AppendPoint(path, path.walkGoal);
	g_detourHalf = std::max(16.0f, width * 0.5f);
	ApplyBuildingDetours(path, true);
	g_detourHalf = 16.0f;
	for (std::size_t i = 1; i < path.points.size(); ++i)
		path.length += (path.points[i] - path.points[i - 1]).Length();
	return PathResult::SUCCESS;
}

int BlockVehicleEdgeAt(const Vector &position, const std::vector<unsigned int> &areaIds, float seconds)
{
	if (!IsLoaded() || areaIds.size() < 2 || gpGlobals == nullptr)
		return -1;
	CNavArea *here = TheNavMesh->GetNearestNavArea(position, 300.0f, false, false);
	if (here == nullptr)
		return -1;
	for (std::size_t i = 0; i + 1 < areaIds.size(); ++i)
	{
		if (areaIds[i] != here->GetID())
			continue;
		s_vehicleBlockedEdges[{areaIds[i], areaIds[i + 1]}] = gpGlobals->curtime + seconds;
		return static_cast<int>(i);
	}
	// Off the path areas: close the link into the path area nearest to us.
	std::size_t best = 0;
	float bestDist = 1.0e18f;
	for (std::size_t i = 1; i < areaIds.size(); ++i)
	{
		CNavArea *area = TheNavMesh->GetNavAreaByID(areaIds[i]);
		if (area == nullptr)
			continue;
		const float d = (area->GetCenter() - position).Length2DSqr();
		if (d < bestDist)
		{
			bestDist = d;
			best = i;
		}
	}
	if (best == 0)
		return -1;
	s_vehicleBlockedEdges[{areaIds[best - 1], areaIds[best]}] = gpGlobals->curtime + seconds;
	return static_cast<int>(best - 1);
}

namespace { class ScanFilter : public CTraceFilter {
public: bool ShouldHitEntity(IHandleEntity*, int) override { return true; } }; }
void DebugNavScan(const Vector& from, const Vector& to, float half, float step)
{
	Path path;
	const PathResult result = BuildPath(from, to, path, nullptr);
	Msg("[RCBOT2] scan route=%s points=%zu areas=%zu\n", PathResultName(result), path.points.size(), path.areaIds.size());
	for (std::size_t i = 0; i < path.areaIds.size() && i < 6; ++i)
	{
		const CNavArea* area = TheNavMesh->GetNavAreaByID(path.areaIds[i]);
		if (area == nullptr) continue;
		const Vector nw = area->GetCorner(NORTH_WEST), se = area->GetCorner(SOUTH_EAST);
		Msg("[RCBOT2] scan area=%u x=[%.0f %.0f] y=[%.0f %.0f] z=[%.0f %.0f] attr=0x%x\n", area->GetID(), nw.x, se.x, nw.y, se.y, nw.z, se.z, area->GetAttributes());
		Msg("[RCBOT2] scan area=%u cornerZ nw=%.0f ne=%.0f se=%.0f sw=%.0f\n", area->GetID(),
			nw.z, area->GetCorner(NORTH_EAST).z, se.z, area->GetCorner(SOUTH_WEST).z);
	}
	for (std::size_t i = 0; i < path.points.size() && i < 8; ++i)
		Msg("[RCBOT2] scan point=%zu (%.0f %.0f %.0f)\n", i, path.points[i].x, path.points[i].y, path.points[i].z);
	for (float z : {from.z + 12.0f, from.z + 24.0f, from.z + 48.0f})
	{
		Msg("[RCBOT2] scan grid z=%.0f xmin=%.0f step=%.0f\n", z, from.x - half, step);
		for (float y = from.y + half; y >= from.y - half; y -= step)
		{
			char line[200]; int col = 0;
			for (float x = from.x - half; x <= from.x + half && col < 199; x += step, ++col)
			{
				Ray_t ray; const Vector point(x, y, z);
				ray.Init(point, point, Vector(-16,-16,0), Vector(16,16,72));
				ScanFilter filter; trace_t hit;
				enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &hit);
				edict_t* edict = hit.m_pEnt != nullptr ? servergameents->BaseEntityToEdict(hit.m_pEnt) : nullptr;
				const char* name = edict != nullptr ? edict->GetClassName() : "";
				if (hit.startsolid && edict != nullptr && std::strcmp(name, "worldspawn") != 0)
					Msg("[RCBOT2] scan ent=%s at=(%.0f %.0f)\n", name, x, y);
				line[col] = !hit.startsolid ? '.' : (edict == nullptr || std::strcmp(name, "worldspawn") == 0 ? '#' : 'E');
			}
			line[col] = 0;
			Msg("[RCBOT2] scan y=%.0f %s\n", y, line);
		}
	}
}

bool IsPointInPathAreas(const Path &path, const Vector &point)
{
	if (TheNavMesh == nullptr || path.areaIds.empty())
		return true;

	for (const unsigned int areaId : path.areaIds)
	{
		CNavArea *area = TheNavMesh->GetNavAreaByID(areaId);
		if (area != nullptr && area->Contains(point))
			return true;
	}
	return false;
}
}
