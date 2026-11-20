#ifndef RCBOT_EMPIRES_COMMANDER_ECONOMY_H
#define RCBOT_EMPIRES_COMMANDER_ECONOMY_H

// Build policy of the automatic Commander. Engine-free so it can be unit tested.
// Building type numbers follow CEmpBuildingParse::GetBuildingType in server_srv.so
// (reverse-engineering/notes/buildings.md).

namespace EmpiresCommanderEconomy
{
enum BuildType { Refinery = 0, Barracks = 1, Radar = 2, Armory = 3, Repair = 4, VehicleFactory = 5, TypeCount = 6 };

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

// Next research item to start, or 0 if nothing should be started yet / tree complete.
// Post-VF items wait until the Vehicle Factory is built so research never starves VF placement.
inline int NextResearch(const bool done[kResearchItemCount], bool hasVehicleFactory)
{
	if (!done[kResearchStructural])
		return kResearchStructural;
	if (!done[kResearchVehicleFactory])
		return kResearchVehicleFactory;
	if (!hasVehicleFactory)
		return 0;
	for (const int item : kResearchOrder)
	{
		if (item >= 0 && item < kResearchItemCount && !done[item])
			return item;
	}
	return 0;
}

constexpr float kCost[TypeCount] = {100.0f, 300.0f, 400.0f, 100.0f, 150.0f, 500.0f};

struct Step { BuildType type; int targetCount; };

// Economy first (income is the only source), then infantry support, vehicles, supply.
constexpr Step kBuildOrder[] = {
	{Refinery, 1}, {Refinery, 2}, {Barracks, 1}, {Radar, 1}, {VehicleFactory, 1},
	{Armory, 1}, {Refinery, 3}, {Repair, 1}, {Refinery, 4},
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
}

#endif
