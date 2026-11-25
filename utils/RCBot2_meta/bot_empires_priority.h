// Priority tiers for engineer work (TODO 9.2): one ladder for build orders and side jobs instead of
// single exceptions (the former "vehicle factory before the refinery wall" rule). A higher tier (lower
// number) preempts a running job of a lower tier; equal or higher running work is not interrupted.
//   Tier 0  emergency: save the CV, BASE_ALERT, enemy APC at the base
//   Tier 1  core infrastructure: refinery, radar, vehicle factory, the first barracks, research
//   Tier 2  front stability: forward barracks, repair pad, armory, vehicle purchase/repair, damaged core
//   Tier 3  active map play: push/hold sectors, flags, enemy buildings, map ramps and bridges
//   Tier 4  comfort: walls, cameras, extra turrets, turret upgrades, spare ammo crates
// SDK-free (unit test: scripts/run_priority_tests.sh).
#ifndef BOT_EMPIRES_PRIORITY_H
#define BOT_EMPIRES_PRIORITY_H

#include <cstring>

namespace EmpiresPriority
{
enum Tier
{
	TIER_EMERGENCY = 0,
	TIER_CORE = 1,
	TIER_FRONT = 2,
	TIER_MAP = 3,
	TIER_COMFORT = 4,
	TIER_IDLE = 5 // no job: takes anything
};

inline const char* TierName(int tier)
{
	switch (tier)
	{
	case TIER_EMERGENCY: return "emergency";
	case TIER_CORE: return "core";
	case TIER_FRONT: return "front";
	case TIER_MAP: return "map";
	case TIER_COMFORT: return "comfort";
	default: return "idle";
	}
}

// Tier of building work on `className`. firstBarracks: the team has no finished barracks (else a new
// barracks is a forward barracks). damagedFinished: repair of a finished building, not construction.
inline int BuildingTier(const char* className, bool firstBarracks, bool damagedFinished)
{
	if (className == nullptr)
		return TIER_MAP;
	if (std::strncmp(className, "emp_eng_map_", 12) == 0)
		return TIER_MAP; // map ramps, bridges, barricades
	if (std::strstr(className, "turret") != nullptr || std::strstr(className, "_eng") != nullptr)
		return TIER_COMFORT; // turrets and engineer deployables (walls, cameras, small radar)
	if (std::strncmp(className, "emp_building_", 13) != 0)
		return TIER_MAP;
	const bool core = std::strstr(className, "refinery") != nullptr || std::strstr(className, "radar") != nullptr ||
		std::strstr(className, "vehiclefactory") != nullptr;
	const bool barracks = std::strstr(className, "barracks") != nullptr;
	if (damagedFinished)
		return core || barracks ? TIER_FRONT : TIER_MAP;
	if (core || (barracks && firstBarracks))
		return TIER_CORE;
	return TIER_FRONT; // forward barracks, repair pad, armory
}

// The running job of an engineer, as far as preemption is concerned.
struct Job
{
	int tier = TIER_IDLE;
	bool interruptible = true; // false: seconds from done (placement action, ammo crate, claimed repair)
};

inline bool Preempts(int incomingTier, const Job& running)
{
	return running.tier == TIER_IDLE || (running.interruptible && incomingTier < running.tier);
}

// Order of candidates for an order of `tier`: idle engineers first, then the lowest-priority job,
// distance last. Returns true when a should come before b.
inline bool BetterCandidate(const Job& a, float distanceA, const Job& b, float distanceB)
{
	if (a.tier != b.tier)
		return a.tier > b.tier;
	return distanceA < distanceB;
}

// Escalation of an order the commander could not get accepted (R1/R2 in MATCH_FLOW.md). Tier 1/2
// orders are never given up: after kRetryTries refused tries the counter restarts after kRetryPause and
// the order may also go to the commander himself. Lower tiers are given up after kRetryTries.
constexpr int kRetryTries = 6;
constexpr float kRetryPause = 20.0f;
constexpr float kNoProgressSeconds = 30.0f; // accepted but no health gain: hand it to another engineer

inline bool NeverGiveUp(int tier)
{
	return tier <= TIER_FRONT;
}
} // namespace EmpiresPriority

#endif
