// Unit tests for the automatic Commander build policy (bot_empires_commander_economy.h).
#include "bot_empires_commander_economy.h"

#include <cstdlib>
#include <iostream>

using namespace EmpiresCommanderEconomy;

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

Inventory AllSites()
{
	Inventory inventory;
	for (bool& site : inventory.siteAvailable)
		site = true;
	return inventory;
}
}

int main()
{
	BuildType type = Radar;

	Inventory inventory = AllSites();
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == Refinery, "ECON-1 starts with a refinery");
	Require(NextBuild(inventory, 5000.0f, type, Repair) == Decision::Build && type == Repair, "ECON-FIRST forced type first");
	{
		Inventory forced = AllSites();
		forced.count[Repair] = 1;
		Require(NextBuild(forced, 5000.0f, type, Repair) == Decision::Build && type == Refinery, "ECON-FIRST normal order once built");
	}

	inventory.count[Refinery] = 1;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == Refinery, "ECON-2 second refinery before barracks");

	inventory.count[Refinery] = 2;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == Barracks, "ECON-3 barracks after two refineries");

	inventory.count[Barracks] = 1;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == Radar, "ECON-4a radar (research building) before the factory");

	inventory.count[Radar] = 1;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == VehicleFactory, "ECON-4 factory next");

	inventory.count[VehicleFactory] = 1;
	Require(NextBuild(inventory, 400.0f, type) == Decision::Build && type == Armory, "ECON-5 armory next");

	Require(NextBuild(inventory, 50.0f, type) == Decision::Saving && type == Armory, "ECON-6 saves instead of skipping");

	// Blocked refinery must not stall the rest of the order.
	inventory = AllSites();
	inventory.siteAvailable[Refinery] = false;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == Barracks, "ECON-7 no refinery site skips to barracks");

	// Expensive step is saved for, a cheaper later step does not jump the queue.
	inventory = AllSites();
	inventory.count[Refinery] = 2;
	inventory.count[Barracks] = 1;
	inventory.count[Radar] = 1;
	Require(NextBuild(inventory, 200.0f, type) == Decision::Saving && type == VehicleFactory, "ECON-8 saves for the factory");

	// Nothing available at all.
	inventory = Inventory();
	Require(NextBuild(inventory, 5000.0f, type) == Decision::NoSite, "ECON-9 no site anywhere");

	// Everything built.
	inventory = AllSites();
	inventory.count[Refinery] = 4;
	inventory.count[Barracks] = 1;
	inventory.count[Radar] = 1;
	inventory.count[VehicleFactory] = 1;
	inventory.count[Armory] = 1;
	inventory.count[Repair] = 1;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Done, "ECON-10 order complete");

	Require(BackoffSeconds(1) == 5.0f && BackoffSeconds(2) == 10.0f && BackoffSeconds(3) == 20.0f, "ECON-11 backoff doubles");
	Require(BackoffSeconds(30) == 120.0f, "ECON-12 backoff capped");
	Require(MaxUnfinished(0) == 1 && MaxUnfinished(1) == 1 && MaxUnfinished(2) == 2 && MaxUnfinished(5) == 2, "ECON-13 unfinished limit");

	// Research progression (TODO 5.1).
	{
		bool done[kResearchItemCount] = {};
		Require(NextResearch(done, false) == kResearchStructural, "RES-1 starts with Structural Engineering");
		done[kResearchStructural] = true;
		Require(NextResearch(done, false) == kResearchVehicleFactory, "RES-2 unlocks Vehicle Factory next");
		done[kResearchVehicleFactory] = true;
		Require(NextResearch(done, false) == 0, "RES-3 waits for Vehicle Factory to be built before post-VF research");
		Require(NextResearch(done, true) == kResearchMgTurret2, "RES-4 continues with MG turret lvl 2 once VF stands");
		for (const int item : kResearchOrder)
			done[item] = true;
		Require(NextResearch(done, true) == 0, "RES-5 returns 0 when tech path is complete");
	}

	std::cout << "PASS commander economy policy\n";
	return 0;
}
