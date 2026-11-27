#ifndef RCBOT_EMPIRES_COMMANDER_ECONOMY_H
#define RCBOT_EMPIRES_COMMANDER_ECONOMY_H

// Build policy of the automatic Commander. Engine-free so it can be unit tested.
// Building type numbers follow CEmpBuildingParse::GetBuildingType in server_srv.so
// (reverse-engineering/notes/buildings.md).

#include <cmath>

namespace EmpiresCommanderEconomy
{
enum BuildType {
	Refinery = 0, Barracks = 1, Radar = 2, Armory = 3, Repair = 4, VehicleFactory = 5,
	MgTurret = 8, MlTurret = 9, TypeCount = 10
};

// Costs from scripts_000.vpk (buildings).
// Research item indexes (scripts/research_items.txt order, server_srv.so GetResearch 0x3a8440).
constexpr int kResearchItemCount = 128;
constexpr int kResearchMechanical = 27;       // Mechanical_Engineering
constexpr int kResearchUpgradedChassis = 28;  // Upgraded_Chassis, needs 27
constexpr int kResearchMediumTank = 29;       // Medium_Tank_Chassis, needs 28
constexpr int kResearchAdvancedChassis = 31;  // Advanced_Chassis, needs 28
constexpr int kResearchHeavyTank = 32;        // Heavy_Tank_Chassis, needs 31
constexpr int kResearchAdvancedMachining = 33;// Advanced_Machining, needs 27
constexpr int kResearchCompositeArmor = 34;   // Composite_Armor, needs 33
constexpr int kResearchGasTurbineEngine = 35; // Gas_Turbine_Engine, needs 33
constexpr int kResearchApcSpawn = 39;         // APC_Spawn_Point, needs 38 (Start_Researched)
constexpr int kResearchStructural = 60;       // Structural_Engineering
constexpr int kResearchVehicleFactory = 61;   // Vehicle_Factory, needs 60
constexpr int kResearchMgTurret2 = 62;        // Upgraded_Machine_Gun_Turrets_Lvl_2, needs 60
constexpr int kResearchMgTurret3 = 63;        // Upgraded_Machine_Gun_Turrets_Lvl_3, needs 62
constexpr int kResearchMlTurret2 = 64;        // Upgraded_Missile_Launcher_Turrets_Lvl_2, needs 60
constexpr int kResearchMlTurret3 = 65;        // Upgraded_Missile_Launcher_Turrets_Lvl_3, needs 64

// Ordered tech path (TODO 5.1): core VF unlock first, then turret upgrades, APC spawn,
// and Mechanical chassis/armor/engine progression once the Vehicle Factory is standing.
constexpr int kResearchOrder[] = {
	kResearchStructural,
	kResearchVehicleFactory,
	kResearchMgTurret2,
	kResearchMlTurret2,
	kResearchMechanical,
	kResearchApcSpawn,
	kResearchUpgradedChassis,
	kResearchMediumTank,
	kResearchAdvancedMachining,
	kResearchCompositeArmor,
	kResearchGasTurbineEngine,
	kResearchMgTurret3,
	kResearchMlTurret3,
	kResearchAdvancedChassis,
	kResearchHeavyTank,
};

constexpr float kCost[TypeCount] = {100.0f, 300.0f, 400.0f, 100.0f, 150.0f, 500.0f, 0.0f, 0.0f, 75.0f, 75.0f};

// Whole price of one research item: Cost (per second) x Time, scripts/research_items.txt in
// scripts_000.vpk. emp_sv_research_multiplier shortens the time; the full price is the safe bound.
inline float ResearchTotalCost(int item)
{
	switch (item)
	{
	case kResearchMechanical: return 4.0f * 60.0f;
	case kResearchUpgradedChassis: return 3.0f * 300.0f;
	case kResearchMediumTank: return 5.0f * 120.0f;
	case kResearchAdvancedChassis: return 4.0f * 300.0f;
	case kResearchHeavyTank: return 6.0f * 300.0f;
	case kResearchAdvancedMachining: return 3.0f * 180.0f;
	case kResearchCompositeArmor: return 5.0f * 120.0f;
	case kResearchGasTurbineEngine: return 5.0f * 120.0f;
	case kResearchApcSpawn: return 4.0f * 120.0f;
	case kResearchStructural: return 4.0f * 60.0f;
	case kResearchVehicleFactory: return 4.0f * 60.0f;
	case kResearchMgTurret2: return 4.0f * 120.0f;
	case kResearchMgTurret3: return 4.0f * 150.0f;
	case kResearchMlTurret2: return 4.0f * 120.0f;
	case kResearchMlTurret3: return 4.0f * 150.0f;
	default: return 4.0f * 120.0f;
	}
}

// Next research item to start, or 0 if nothing should be started yet / tree complete.
// Post-VF items (TODO 9.7): once a Vehicle Factory is placed its price is paid, so research goes on;
// before that only while the pool covers the factory and the whole item (Palmbay 20261125-154912:
// 150-181 s idle research with 5000+ resources between the VF research and the finished factory).
inline int NextResearch(const bool done[kResearchItemCount], bool factoryPlaced, float resources)
{
	if (!done[kResearchStructural])
		return kResearchStructural;
	if (!done[kResearchVehicleFactory])
		return kResearchVehicleFactory;
	for (const int item : kResearchOrder)
	{
		if (item < 0 || item >= kResearchItemCount || done[item])
			continue;
		if (!factoryPlaced && resources < kCost[VehicleFactory] + ResearchTotalCost(item))
			return 0;
		return item;
	}
	return 0;
}

constexpr float kNearVehicleRange = 3800.0f; // CanBuildAt allows 4000, keep a margin

// Discount map income by travel distance. One build radius halves the score.
inline float RefineryScore(float income, float distance)
{
	return income / (1.0f + distance / kNearVehicleRange);
}

// A point nearer the enemy CV than the own one is contested (TODO 5.5): half the score, so an equal
// point on the own side goes first; a much richer contested point can still win.
inline float RefineryScore(float income, float distance, bool enemySide)
{
	return RefineryScore(income, distance) * (enemySide ? 0.5f : 1.0f);
}

struct Step { BuildType type; int targetCount; };

// Economy first (income is the only source), then infantry support, vehicles, the repair pad right
// after the factory (TODO 9.5/M5: behind armory and third refinery it came 346-394 s after the factory),
// supply, and base defense turrets (TODO 5.23).
constexpr Step kBuildOrder[] = {
	{Refinery, 1}, {Refinery, 2}, {Barracks, 1}, {Radar, 1}, {VehicleFactory, 1},
	{Repair, 1}, {Armory, 1}, {Refinery, 3}, {MgTurret, 1}, {MlTurret, 1},
	{Refinery, 4}, {MgTurret, 2}, {MlTurret, 2},
};
constexpr int kBuildOrderSize = sizeof(kBuildOrder) / sizeof(kBuildOrder[0]);

struct Inventory
{
	int count[TypeCount] = {};        // own buildings including unfinished ones
	bool siteAvailable[TypeCount] = {}; // a placement candidate exists right now
};

enum class Decision { Build, Saving, NoSite, Done };

// First step in order that is still short. A step without a site is skipped so a blocked
// refinery does not stall the rest. An unaffordable step is waited for (saving) instead of
// being replaced by a cheaper one, otherwise the expensive buildings never get built.
// firstType >= 0 (test cvar rcbot_empires_commander_first): build one of that
// type before the normal order.
inline Decision NextBuild(const Inventory& inventory, float resources, BuildType& type, int firstType = -1)
{
	if (firstType >= 0 && firstType < TypeCount && inventory.count[firstType] < 1 &&
		inventory.siteAvailable[firstType])
	{
		type = static_cast<BuildType>(firstType);
		return resources >= kCost[firstType] ? Decision::Build : Decision::Saving;
	}
	bool skippedForSite = false;
	for (const Step& step : kBuildOrder)
	{
		if (inventory.count[step.type] >= step.targetCount)
			continue;
		// The pad serves vehicles: not before a factory is placed (Palmbay 20261125-164159: the pad came
		// before the VF research, changed the base layout and the CV took another route to the far site).
		if (step.type == Repair && inventory.count[VehicleFactory] < 1)
			continue;
		if (!inventory.siteAvailable[step.type])
		{
			skippedForSite = true;
			continue;
		}
		type = step.type;
		return resources >= kCost[step.type] ? Decision::Build : Decision::Saving;
	}
	return skippedForSite ? Decision::NoSite : Decision::Done;
}

// A building still under construction (TODO 5.5): m_bFullyBuilt when it can be read, else health.
// A finished but damaged building is repair work and must not hold up the next placement (with one or
// two engineers one scratched refinery stopped every rebuild, MaxUnfinished below).
inline bool UnderConstruction(int health, int maxHealth, bool builtKnown, bool built)
{
	return builtKnown ? !built : health < maxHealth;
}

// Retry delay after a rejected build of one type: 5 s, doubling, at most 120 s.
inline float BackoffSeconds(int failures)
{
	float seconds = 5.0f;
	for (int i = 1; i < failures && seconds < 120.0f; ++i)
		seconds *= 2.0f;
	return seconds > 120.0f ? 120.0f : seconds;
}

// How many unfinished buildings the Commander may have at once: one per living Engineer,
// at least one, at most two (resources stay free for repairs and the next step).
inline int MaxUnfinished(int livingEngineers)
{
	return livingEngineers < 1 ? 1 : (livingEngineers > 2 ? 2 : livingEngineers);
}

// Vehicle factory exit lane: from the factory box edge (local +X) to kFactoryExitLaneLength
// from its centre, at least 600 past the edge. The driver's exit search runs up to 2200
// (bot_empires_vehicle.cpp), crossroads 20261126-105230: a pad 980 in front of the NF bay.
constexpr float kFactoryExitLaneLength = 2200.0f;
constexpr float kFactoryExitLaneHalfWidth = 200.0f; // half a 284 wide tank plus margin
constexpr float kPadLaneHalfWidth = 100.0f;

inline float FactoryExitLaneEnd(float boxMaxX)
{
	return boxMaxX + 600.0f > kFactoryExitLaneLength ? boxMaxX + 600.0f : kFactoryExitLaneLength;
}

// True if the segment a-b (sampled every 50) runs through the box mins/maxs (local, rotated by
// yawDeg around at) grown by laneHalfWidth.
inline bool LaneHitsBox(float atX, float atY, float yawDeg, float minX, float minY, float maxX, float maxY,
	float ax, float ay, float bx, float by, float laneHalfWidth)
{
	const float r = yawDeg * 3.14159265f / 180.0f;
	const float c = std::cos(r), sn = std::sin(r);
	const float len = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
	int steps = int(len / 50.0f);
	if (steps < 1)
		steps = 1;
	for (int k = 0; k <= steps; ++k)
	{
		const float t = float(k) / float(steps);
		const float dx = ax + (bx - ax) * t - atX, dy = ay + (by - ay) * t - atY;
		const float lx = dx * c + dy * sn, ly = -dx * sn + dy * c;
		if (lx > minX - laneHalfWidth && lx < maxX + laneHalfWidth && ly > minY - laneHalfWidth && ly < maxY + laneHalfWidth)
			return true;
	}
	return false;
}

// A building footprint on the ground: local box mins/maxs, rotated by yawDeg around (x, y).
struct Box2D
{
	float x, y, yawDeg, minX, minY, maxX, maxY;
};

inline void BoxCorners(const Box2D& b, float out[4][2])
{
	const float r = b.yawDeg * 3.14159265f / 180.0f;
	const float c = std::cos(r), sn = std::sin(r);
	const float lx[4] = {b.minX, b.maxX, b.maxX, b.minX};
	const float ly[4] = {b.minY, b.minY, b.maxY, b.maxY};
	for (int k = 0; k < 4; ++k)
	{
		out[k][0] = b.x + lx[k] * c - ly[k] * sn;
		out[k][1] = b.y + lx[k] * sn + ly[k] * c;
	}
}

inline float PointSegmentDistance(float px, float py, float ax, float ay, float bx, float by)
{
	const float dx = bx - ax, dy = by - ay;
	const float len2 = dx * dx + dy * dy;
	float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	const float ex = ax + dx * t - px, ey = ay + dy * t - py;
	return std::sqrt(ex * ex + ey * ey);
}

// Shortest ground distance between two rotated footprints; 0 when they touch or overlap.
inline float BoxGap(const Box2D& a, const Box2D& b)
{
	float ca[4][2], cb[4][2];
	BoxCorners(a, ca);
	BoxCorners(b, cb);
	// Separating axis test on the four edge normals: no separating axis means overlap.
	bool separated = false;
	const float (*boxes[2])[2] = {ca, cb};
	for (int s = 0; s < 2 && !separated; ++s)
		for (int e = 0; e < 2 && !separated; ++e)
		{
			const float nx = boxes[s][e + 1][1] - boxes[s][e][1], ny = boxes[s][e][0] - boxes[s][e + 1][0];
			float minA = 1.0e30f, maxA = -1.0e30f, minB = 1.0e30f, maxB = -1.0e30f;
			for (int k = 0; k < 4; ++k)
			{
				const float pa = ca[k][0] * nx + ca[k][1] * ny, pb = cb[k][0] * nx + cb[k][1] * ny;
				minA = pa < minA ? pa : minA;
				maxA = pa > maxA ? pa : maxA;
				minB = pb < minB ? pb : minB;
				maxB = pb > maxB ? pb : maxB;
			}
			separated = maxA < minB || maxB < minA;
		}
	if (!separated)
		return 0.0f;
	// Separated convex shapes: the closest pair lies on a corner of one and an edge of the other.
	float best = 1.0e30f;
	for (int k = 0; k < 4; ++k)
		for (int e = 0; e < 4; ++e)
		{
			const int f = (e + 1) % 4;
			const float d1 = PointSegmentDistance(ca[k][0], ca[k][1], cb[e][0], cb[e][1], cb[f][0], cb[f][1]);
			const float d2 = PointSegmentDistance(cb[k][0], cb[k][1], ca[e][0], ca[e][1], ca[f][0], ca[f][1]);
			best = d1 < best ? d1 : best;
			best = d2 < best ? d2 : best;
		}
	return best;
}
}

#endif
