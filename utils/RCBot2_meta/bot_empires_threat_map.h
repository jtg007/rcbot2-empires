// Lean 2D threat map per team (influence map after CircuitAI): a coarse grid over the whole
// Source world (64 x 64 cells of 512 HU), rebuilt by the team director every 1.5 s only from
// fair team knowledge (EmpiresTeamIntel contacts, active hot zones, own building damage).
// Each source adds weight * (1 - distance / radius) to every cell centre in its radius, so the
// value falls off softly into neighbouring cells; Sample() interpolates bilinearly between centres.
//
// Two channels: CH_TOTAL (everything) and CH_VEHICLE (enemy tanks/APCs only, for the engineer
// blockade check). Vehicle-block marks remember a passage where an engineer stalled against a
// known enemy vehicle, so the next engineer is not sent blindly into the same spot.
//
// Cvar rcbot_empires_threat_map (default 0 during A/B). SDK-free, unit-tested offline
// (scripts/run_threat_map_tests.sh).
#ifndef BOT_EMPIRES_THREAT_MAP_H
#define BOT_EMPIRES_THREAT_MAP_H

#include <algorithm>
#include <cmath>

namespace EmpiresThreatMap
{
constexpr float kCellSize = 512.0f;
constexpr int kCells = 64;                 // 64 * 512 = 32768 = full Source world extent
constexpr float kWorldMin = -16384.0f;
constexpr int kCellCount = kCells * kCells;

enum Channel
{
	CH_TOTAL = 0,
	CH_VEHICLE = 1,
	kChannels = 2
};

// Source weights (threat at the source position) and falloff radii (HU).
constexpr float kInfantryWeight = 1.0f;
constexpr float kInfantryRadius = 1200.0f;  // rifle/SMG reach in practice
constexpr float kVehicleWeight = 3.0f;
constexpr float kVehicleRadius = 2000.0f;   // tank cannon + MG
constexpr float kApcWeight = 1.5f;
constexpr float kApcRadius = 1500.0f;
constexpr float kTurretWeight = 2.5f;       // radius from the turret level (2000/3000/4000, empiresMgTurretRange)
constexpr float kHotZoneWeight = 2.0f;      // radius = hot-zone radius + one cell
constexpr float kOwnDamageWeight = 1.0f;    // own building under fire, attacker unseen
constexpr float kOwnDamageRadius = 800.0f;
constexpr float kMinAgeFactor = 0.5f;       // a contact at the end of its TTL keeps half its weight

// Engineer blockade (TODO 9.8): vehicle channel at or above this counts as "enemy vehicle here"
// (a tank within ~800 HU of the point).
constexpr float kVehicleBlockThreshold = 0.6f * kVehicleWeight;
constexpr int kMaxBlocks = 4;
constexpr float kBlockMinSeconds = 30.0f;   // a mark holds at least this long
constexpr float kBlockMaxSeconds = 120.0f;  // and at most this long, even if the vehicle stays
constexpr float kBlockRadius = 600.0f;      // a building within this of a mark counts as behind it

// Cover consumer (rcbot_empires_navmesh_hurt_cover / fight_cover).
constexpr float kCoverCostPerThreat = 300.0f;  // path units per threat unit at the cover spot
constexpr float kRetreatMinGain = 0.5f;        // threat drop needed for a retreat without LOS cover

inline float AgeFactor(float age, float ttl)
{
	if (ttl <= 0.0f)
		return 1.0f;
	const float t = (std::max)(0.0f, (std::min)(1.0f, age / ttl));
	return 1.0f - (1.0f - kMinAgeFactor) * t;
}

struct Block
{
	int building = -1;
	float pos[2] = { 0.0f, 0.0f };
	float since = -1.0e9f;
	bool used() const { return building > 0; }
};

struct Map
{
	float cell[kChannels][kCellCount];
	Block blocks[kMaxBlocks];
	int sources = 0;
	float peak = 0.0f;
	bool built = false;

	Map() { Reset(); }

	void Clear()
	{
		for (int c = 0; c < kChannels; ++c)
			for (int i = 0; i < kCellCount; ++i)
				cell[c][i] = 0.0f;
		sources = 0;
		peak = 0.0f;
	}

	void Reset()
	{
		Clear();
		for (Block& b : blocks)
			b = Block();
		built = false;
	}
};

inline float CellCentre(int index)
{
	return kWorldMin + (static_cast<float>(index) + 0.5f) * kCellSize;
}

inline int ClampCell(int index)
{
	return index < 0 ? 0 : (index >= kCells ? kCells - 1 : index);
}

// Adds one source. vehicle = also goes to CH_VEHICLE.
inline void Stamp(Map& map, float x, float y, float weight, float radius, bool vehicle)
{
	if (weight <= 0.0f || radius <= 0.0f || !std::isfinite(x) || !std::isfinite(y))
		return;
	const int x0 = ClampCell(static_cast<int>(std::floor((x - radius - kWorldMin) / kCellSize)));
	const int x1 = ClampCell(static_cast<int>(std::floor((x + radius - kWorldMin) / kCellSize)));
	const int y0 = ClampCell(static_cast<int>(std::floor((y - radius - kWorldMin) / kCellSize)));
	const int y1 = ClampCell(static_cast<int>(std::floor((y + radius - kWorldMin) / kCellSize)));
	for (int cy = y0; cy <= y1; ++cy)
	{
		const float dy = CellCentre(cy) - y;
		for (int cx = x0; cx <= x1; ++cx)
		{
			const float dx = CellCentre(cx) - x;
			const float d = std::sqrt(dx * dx + dy * dy);
			if (d >= radius)
				continue;
			const float v = weight * (1.0f - d / radius);
			const int i = cy * kCells + cx;
			map.cell[CH_TOTAL][i] += v;
			map.peak = (std::max)(map.peak, map.cell[CH_TOTAL][i]);
			if (vehicle)
				map.cell[CH_VEHICLE][i] += v;
		}
	}
	++map.sources;
}

enum SourceKind
{
	SRC_INFANTRY = 0,
	SRC_VEHICLE,     // tank / armed vehicle
	SRC_APC,
	SRC_TURRET,      // known enemy MG/rocket turret; radius = its range
	SRC_HOT_ZONE,    // own deaths cluster (team director hot zone); radius = zone radius
	SRC_OWN_DAMAGE   // own building or CV losing health, attacker unseen
};

// One fair input. age/ttl scale contacts down as they get older (ttl <= 0: no ageing).
// radius <= 0 takes the kind's default.
inline void AddSource(Map& map, SourceKind kind, float x, float y, float age, float ttl, float radius = 0.0f)
{
	const float f = AgeFactor(age, ttl);
	switch (kind)
	{
	case SRC_INFANTRY:
		Stamp(map, x, y, kInfantryWeight * f, radius > 0.0f ? radius : kInfantryRadius, false);
		break;
	case SRC_VEHICLE:
		Stamp(map, x, y, kVehicleWeight * f, radius > 0.0f ? radius : kVehicleRadius, true);
		break;
	case SRC_APC:
		Stamp(map, x, y, kApcWeight * f, radius > 0.0f ? radius : kApcRadius, true);
		break;
	case SRC_TURRET:
		Stamp(map, x, y, kTurretWeight * f, radius > 0.0f ? radius : 2000.0f, false);
		break;
	case SRC_HOT_ZONE:
		Stamp(map, x, y, kHotZoneWeight * f, (radius > 0.0f ? radius : 500.0f) + kCellSize, false);
		break;
	case SRC_OWN_DAMAGE:
		Stamp(map, x, y, kOwnDamageWeight * f, radius > 0.0f ? radius : kOwnDamageRadius, false);
		break;
	}
}

// Bilinear between cell centres (soft across cell borders); 0 outside the world.
inline float Sample(const Map& map, float x, float y, Channel channel = CH_TOTAL)
{
	if (!std::isfinite(x) || !std::isfinite(y))
		return 0.0f;
	const float fx = (x - kWorldMin) / kCellSize - 0.5f;
	const float fy = (y - kWorldMin) / kCellSize - 0.5f;
	if (fx < -0.5f || fy < -0.5f || fx > kCells - 0.5f || fy > kCells - 0.5f)
		return 0.0f;
	const int ix = static_cast<int>(std::floor(fx));
	const int iy = static_cast<int>(std::floor(fy));
	const float tx = fx - static_cast<float>(ix);
	const float ty = fy - static_cast<float>(iy);
	const float* c = map.cell[channel];
	const auto at = [c](int cx, int cy) { return c[ClampCell(cy) * kCells + ClampCell(cx)]; };
	const float top = at(ix, iy) * (1.0f - tx) + at(ix + 1, iy) * tx;
	const float bottom = at(ix, iy + 1) * (1.0f - tx) + at(ix + 1, iy + 1) * tx;
	return top * (1.0f - ty) + bottom * ty;
}

inline int HotCells(const Map& map, float threshold)
{
	int n = 0;
	for (int i = 0; i < kCellCount; ++i)
		if (map.cell[CH_TOTAL][i] >= threshold)
			++n;
	return n;
}

// --- Engineer blockade marks (TODO 9.8) ---

inline bool BlockActive(const Map& map, const Block& b, float now)
{
	if (!b.used())
		return false;
	const float age = now - b.since;
	if (age < 0.0f || age >= kBlockMaxSeconds)
		return false;
	if (age < kBlockMinSeconds)
		return true;
	// After the minimum: only while the team still knows a vehicle there (half threshold: the
	// contact ages and its weight sinks before the TTL ends).
	return Sample(map, b.pos[0], b.pos[1], CH_VEHICLE) >= 0.5f * kVehicleBlockThreshold;
}

// Remembers that an engineer stalled at (x, y) against a known enemy vehicle on the way to
// `building`. Refreshes the building's mark (keeps its start), else takes a free or the oldest slot.
inline void MarkBlock(Map& map, int building, float x, float y, float now)
{
	if (building <= 0)
		return;
	Block* slot = nullptr;
	for (Block& b : map.blocks)
		if (b.building == building && BlockActive(map, b, now))
		{
			b.pos[0] = x;
			b.pos[1] = y;
			return;
		}
	for (Block& b : map.blocks)
		if (!BlockActive(map, b, now) && slot == nullptr)
			slot = &b;
	if (slot == nullptr)
	{
		slot = &map.blocks[0];
		for (Block& b : map.blocks)
			if (b.since < slot->since)
				slot = &b;
	}
	slot->building = building;
	slot->pos[0] = x;
	slot->pos[1] = y;
	slot->since = now;
}

// True while an engineer must not be sent to `building` (at bx, by): its own mark is active, or
// another active mark lies within kBlockRadius of it (same blocked passage, e.g. two buildings side
// by side behind one tank). outBlock gets the mark.
inline bool BuildingBlocked(const Map& map, int building, float bx, float by, float now, Block& outBlock)
{
	for (const Block& b : map.blocks)
	{
		if (!BlockActive(map, b, now))
			continue;
		const float dx = b.pos[0] - bx, dy = b.pos[1] - by;
		if (b.building == building || std::sqrt(dx * dx + dy * dy) <= kBlockRadius)
		{
			outBlock = b;
			return true;
		}
	}
	return false;
}

inline int ActiveBlocks(const Map& map, float now)
{
	int n = 0;
	for (const Block& b : map.blocks)
		if (BlockActive(map, b, now))
			++n;
	return n;
}

// --- Cover consumer ---

// Extra cost for a cover candidate in path units.
inline float CoverThreatCost(float threatAtSpot)
{
	return (std::max)(0.0f, threatAtSpot) * kCoverCostPerThreat;
}

// No line-of-sight cover: accept a reachable retreat spot only when it is clearly safer.
inline bool RetreatWorthIt(float threatHere, float threatSpot)
{
	return threatHere - threatSpot >= kRetreatMinGain;
}
} // namespace EmpiresThreatMap

#endif
