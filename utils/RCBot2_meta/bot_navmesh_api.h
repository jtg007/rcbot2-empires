/* Minimal production-facing NavMesh path API for RCBot2. */
#ifndef __RCBOT_NAVMESH_API_H__
#define __RCBOT_NAVMESH_API_H__

#include <vector>

#include "vector.h"

class CBot;

namespace RCBotNavMesh
{
enum class PathResult
{
	SUCCESS,
	NAVMESH_NOT_LOADED,
	START_AREA_NOT_FOUND,
	GOAL_AREA_NOT_FOUND,
	NO_PATH,
};

struct Path
{
	std::vector<Vector> points;
	std::vector<unsigned int> areaIds;
	int startAreaId = -1;
	int goalAreaId = -1;
	Vector walkGoal = Vector(0.0f, 0.0f, 0.0f);
	bool targetAboveWalkPlane = false;
	float length = 0.0f;
	bool partial = false;
	// Points reached by climbing a ladder (dismount at the top or foot at the
	// bottom); the follower looks at them so forward movement climbs.
	std::vector<Vector> ladderPoints;

	void clear()
	{
		ladderPoints.clear();
		points.clear();
		areaIds.clear();
		startAreaId = -1;
		goalAreaId = -1;
		walkGoal = Vector(0.0f, 0.0f, 0.0f);
		targetAboveWalkPlane = false;
		length = 0.0f;
		partial = false;
	}
};

const char *PathResultName(PathResult result);

void Initialize();
void LevelInit();
void Frame(bool simulating);
void LevelShutdown();
void Shutdown();

bool IsLoaded();
unsigned int GetAreaCount();
// Stable, pointer-free area identity for analysis. Returns -1 when no loaded
// mesh area is available at the position; does not run pathfinding.
int GetNearestAreaId(const Vector &position);
// Exact containing/underfoot area identity for transition observation. Unlike
// GetNearestAreaId this does not snap to a neighboring area; returns -1 when
// the position cannot be assigned reliably.
int GetAreaIdAtPosition(const Vector &position);
// Closest point on the nearest area within maxDistance; false when there is none.
bool ClosestPointOnMesh(const Vector &position, float maxDistance, Vector &out);
// Forbid one area link for path planning until the map changes.
// seconds > 0: only for that long (a closed door that may open later).
// learned=false: refresh of a known block, does not count as new knowledge (BlockGeneration).
void BlockEdge(unsigned fromAreaId, unsigned toAreaId, float seconds = 0.0f, bool learned = true);
// Blocks the link leaving the path area closest to position (within 64).
bool BlockEdgeNearestOnPath(const Vector &position, const std::vector<unsigned int> &areaIds,
	unsigned &fromAreaId, unsigned &toAreaId, float seconds = 0.0f);
// Blocks every area link whose line between the area centres passes through the box
// (a closed door covers several links). Returns how many links were blocked.
int BlockEdgesThroughBox(const Vector &mins, const Vector &maxs, float seconds, bool learned = true);
// Links of areas under the box whose shared edge lies behind it, seen from 'from'.
int BlockEdgesBehindBox(const Vector &from, const Vector &mins, const Vector &maxs, float seconds);
// Team-only no-go circle (2D), e.g. a hostile turret's range: routes of that team's
// bots never enter an area whose centre lies inside it (leaving it stays allowed).
// radius <= 0 clears it. Cleared with the map.
void SetTeamDangerCircle(int team, const Vector &center, float radius);
// A drop link ends under solid floor: forbid every link from areas on this floor
// (within radius of floorPoint) down to areas under the same spot. Returns the count.
int BlockDropsUnderFloor(const Vector &floorPoint, float radius);
// Grows with every BlockEdge call: a changed count means routes may differ now.
unsigned BlockGeneration();
void ClearBlockedEdges();
PathResult BuildPath(const Vector &from, const Vector &to, Path &path,
	CBot* observer = nullptr);
bool IsPointInPathAreas(const Path &path, const Vector &point);
// True if point lies within pad (2D) of a building's collision box; commander vehicles are ignored.
bool PointNearBuilding(const Vector &point, float pad);
// Route for a vehicle of the given width: no ladders, jumps or crouch areas,
// small steps only, narrow links expensive, portal points kept off the edges.
// allowPartial: with no full path, return the way to the reachable area nearest the goal
// (only if that is clearly closer than the start); path.partial is then set.
PathResult BuildVehiclePath(const Vector &from, const Vector &to, Path &path, float width,
	bool allowPartial = false);
// Closes the path link the vehicle is stuck on, for vehicles only. Returns its
// index in areaIds or -1.
int BlockVehicleEdgeAt(const Vector &position, const std::vector<unsigned int> &areaIds, float seconds);

struct GroundSample
{
	unsigned areaId;
	Vector origin;
};
struct LocalGroundSamples
{
	bool available = false;
	bool truncated = false;
	unsigned cellsInspected = 0;
	unsigned entriesInspected = 0;
	unsigned areasInspected = 0;
	std::vector<GroundSample> samples;
};
// Ground points on the areas nearest `origin`. Keeps at most maxAreas areas.
LocalGroundSamples CollectLocalGroundSamples(const Vector& origin, float radius,
	unsigned maxCells, unsigned maxEntries, unsigned maxAreas);
void DebugNavScan(const Vector& from, const Vector& to, float half, float step);
}

#endif
