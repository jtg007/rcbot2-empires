// Unit tests for the automatic Commander build policy (bot_empires_commander_economy.h).
#include "bot_empires_commander_economy.h"

#include <cmath>
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
	Require(NextBuild(inventory, 400.0f, type) == Decision::Build && type == Repair, "ECON-5 repair pad right after the factory (M5)");

	Require(NextBuild(inventory, 50.0f, type) == Decision::Saving && type == Repair, "ECON-6 saves instead of skipping");

	inventory.count[Repair] = 1;
	Require(NextBuild(inventory, 400.0f, type) == Decision::Build && type == Armory, "ECON-5b armory after the pad");
	inventory.count[Repair] = 0;

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
	inventory.siteAvailable[VehicleFactory] = false; // research not done yet
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == Armory, "ECON-8b no pad before a factory is placed");
	inventory.siteAvailable[VehicleFactory] = true;

	// Nothing available at all.
	inventory = Inventory();
	Require(NextBuild(inventory, 5000.0f, type) == Decision::NoSite, "ECON-9 no site anywhere");

	// Base defense turrets after repair pad (TODO 5.23).
	inventory = AllSites();
	inventory.count[Refinery] = 3;
	inventory.count[Barracks] = 1;
	inventory.count[Radar] = 1;
	inventory.count[VehicleFactory] = 1;
	inventory.count[Armory] = 1;
	inventory.count[Repair] = 1;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == MgTurret, "ECON-9a base MG turret after repair");
	inventory.count[MgTurret] = 1;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == MlTurret, "ECON-9b base ML turret after MG turret");

	// Everything built.
	inventory = AllSites();
	inventory.count[Refinery] = 4;
	inventory.count[Barracks] = 1;
	inventory.count[Radar] = 1;
	inventory.count[VehicleFactory] = 1;
	inventory.count[Armory] = 1;
	inventory.count[Repair] = 1;
	inventory.count[MgTurret] = 2;
	inventory.count[MlTurret] = 2;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Done, "ECON-10 order complete");

	inventory.count[Barracks] = 0;
	Require(NextBuild(inventory, 5000.0f, type) == Decision::Build && type == Barracks, "rebuild lost barracks");
	Require(RefineryScore(2, 3433) > RefineryScore(1, 3163), "emp_arid richer site wins");
	Require(RefineryScore(1, 3163) > RefineryScore(1, 3433), "equal yields prefer shorter travel");
	Require(RefineryScore(1, 1000) > RefineryScore(2, 16000), "long escort can outweigh income");
	Require(RefineryScore(1, 3000, false) > RefineryScore(1, 2500, true), "ECON-17 own-side point before a slightly nearer contested one");
	Require(RefineryScore(3, 3000, true) > RefineryScore(1, 3000, false), "ECON-18 a much richer contested point still wins");

	Require(BackoffSeconds(1) == 5.0f && BackoffSeconds(2) == 10.0f && BackoffSeconds(3) == 20.0f, "ECON-11 backoff doubles");
	Require(BackoffSeconds(30) == 120.0f, "ECON-12 backoff capped");
	Require(UnderConstruction(150, 300, true, false), "ECON-14 placed building without m_bFullyBuilt is under construction");
	Require(!UnderConstruction(90, 300, true, true), "ECON-15 damaged finished building does not count as unfinished");
	Require(UnderConstruction(90, 300, false, false) && !UnderConstruction(300, 300, false, false), "ECON-16 health fallback");
	Require(MaxUnfinished(0) == 1 && MaxUnfinished(1) == 1 && MaxUnfinished(2) == 2 && MaxUnfinished(5) == 2, "ECON-13 unfinished limit");

	// Research progression (TODO 5.1).
	{
		bool done[kResearchItemCount] = {};
		Require(NextResearch(done, false, 0.0f) == kResearchStructural, "RES-1 starts with Structural Engineering");
		done[kResearchStructural] = true;
		Require(NextResearch(done, false, 0.0f) == kResearchVehicleFactory, "RES-2 unlocks Vehicle Factory next");
		done[kResearchVehicleFactory] = true;
		Require(NextResearch(done, false, 900.0f) == 0, "RES-3 no factory placed: keeps the factory price and the item free");
		Require(NextResearch(done, false, 980.0f) == kResearchMgTurret2, "RES-3b no factory yet, pool covers factory + MG lvl 2 (500 + 480)");
		Require(NextResearch(done, true, 0.0f) == kResearchMgTurret2, "RES-4 placed factory (also unfinished) is paid: research goes on");
		Require(ResearchTotalCost(kResearchUpgradedChassis) == 900.0f && ResearchTotalCost(kResearchHeavyTank) == 1800.0f,
			"RES-6 item prices from research_items.txt");
		for (const int item : kResearchOrder)
			done[item] = true;
		Require(NextResearch(done, true, 0.0f) == 0, "RES-5 returns 0 when tech path is complete");
	}

	// Factory exit lane (crossroads 20261126-105230: NF pad 980 in front of the bay).
	{
		Require(FactoryExitLaneEnd(920.0f) == 2200.0f && FactoryExitLaneEnd(1700.0f) == 2300.0f,
			"LANE-1 exit lane reaches 2200 from the centre, at least 600 past the box");
		// Factory at origin facing +X, NF box maxs.x 920: lane (920,0)-(2200,0).
		const float ax = 920.0f, bx = FactoryExitLaneEnd(920.0f);
		// Pad (+-495 x +-850) centred 980 ahead, rotated 90: in the lane.
		Require(LaneHitsBox(980.0f, 344.0f, 90.0f, -495.0f, -850.0f, 495.0f, 850.0f, ax, 0.0f, bx, 0.0f, kFactoryExitLaneHalfWidth),
			"LANE-2 pad 980 ahead of the bay (344 aside) blocks the exit lane");
		// The old 600 lane ended at 1520: a pad at 2200 ahead was allowed, now it is not.
		Require(LaneHitsBox(2200.0f, 0.0f, 0.0f, -495.0f, -850.0f, 495.0f, 850.0f, ax, 0.0f, bx, 0.0f, kFactoryExitLaneHalfWidth) &&
			!LaneHitsBox(2200.0f, 0.0f, 0.0f, -495.0f, -850.0f, 495.0f, 850.0f, ax, 0.0f, 1520.0f, 0.0f, kPadLaneHalfWidth),
			"LANE-3 pad 2200 ahead blocks the long lane, not the old one");
		// Beside the lane: a refinery box 600 to the side stays free.
		Require(!LaneHitsBox(1500.0f, 600.0f, 0.0f, -207.0f, -247.0f, 162.0f, 220.0f, ax, 0.0f, bx, 0.0f, kFactoryExitLaneHalfWidth),
			"LANE-4 refinery 600 beside the lane is free");
		// Tank-wide margin: a box edge 150 off the lane axis blocks (half width 200), 250 off does not.
		Require(LaneHitsBox(1500.0f, 400.0f, 0.0f, -250.0f, -250.0f, 250.0f, 250.0f, ax, 0.0f, bx, 0.0f, kFactoryExitLaneHalfWidth) &&
			!LaneHitsBox(1500.0f, 500.0f, 0.0f, -250.0f, -250.0f, 250.0f, 250.0f, ax, 0.0f, bx, 0.0f, kFactoryExitLaneHalfWidth),
			"LANE-5 lane half width 200");
	}
	{
		auto near = [](float a, float b) { return std::fabs(a - b) < 1.0f; };
		// Axis-aligned squares 100 apart.
		const Box2D a{0.0f, 0.0f, 0.0f, -100.0f, -100.0f, 100.0f, 100.0f};
		Require(near(BoxGap(a, Box2D{300.0f, 0.0f, 0.0f, -100.0f, -100.0f, 100.0f, 100.0f}), 100.0f), "GAP-1 squares 100 apart");
		// Rotation counts: the same square turned 45 deg reaches 141 toward a (corner at 400-141).
		Require(near(BoxGap(a, Box2D{400.0f, 0.0f, 45.0f, -100.0f, -100.0f, 100.0f, 100.0f}), 158.6f),
			"GAP-2 turned square: corner 158.6 away");
		Require(near(BoxGap(a, Box2D{400.0f, 0.0f, 0.0f, -100.0f, -100.0f, 100.0f, 100.0f}), 200.0f),
			"GAP-3 unturned square 200 away");
		// Overlap and touch are 0.
		Require(BoxGap(a, Box2D{150.0f, 0.0f, 30.0f, -100.0f, -100.0f, 100.0f, 100.0f}) == 0.0f, "GAP-4 overlap is 0");
		// Asymmetric box (factory -414..920): turning it 180 deg moves its long end away.
		const Box2D vf0{1500.0f, 0.0f, 180.0f, -414.0f, -828.0f, 920.0f, 828.0f};
		const Box2D vf180{1500.0f, 0.0f, 0.0f, -414.0f, -828.0f, 920.0f, 828.0f};
		Require(near(BoxGap(a, vf0), 480.0f) && near(BoxGap(a, vf180), 986.0f), "GAP-5 asymmetric box uses its yaw");
	}

	std::cout << "PASS commander economy policy\n";
	return 0;
}
