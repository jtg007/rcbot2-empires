// Offline unit tests for bot_empires_threat_map.h (lean 2D threat map, rcbot_empires_threat_map).
#include "bot_empires_threat_map.h"

#include <chrono>
#include <cstdio>
#include <memory>

namespace
{
using namespace EmpiresThreatMap;

int g_failures = 0;

void Expect(bool cond, const char* msg)
{
	if (!cond)
	{
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
}

bool Near(float a, float b, float eps = 1.0e-3f)
{
	return std::fabs(a - b) <= eps;
}

void TestEmptyAndBounds()
{
	auto map = std::make_unique<Map>();
	Expect(Near(Sample(*map, 0.0f, 0.0f), 0.0f), "empty map samples 0");
	Expect(Near(Sample(*map, 1.0e6f, 0.0f), 0.0f), "outside the world samples 0");
	Expect(Near(Sample(*map, NAN, 0.0f), 0.0f), "NaN position samples 0");
	AddSource(*map, SRC_INFANTRY, NAN, 0.0f, 0.0f, 10.0f);
	Expect(map->sources == 0, "NaN source is ignored");
	// A source at the world edge must not write outside the grid (ASan/UBSan would catch it).
	AddSource(*map, SRC_TURRET, 16380.0f, -16380.0f, 0.0f, 0.0f, 4000.0f);
	Expect(Sample(*map, 16000.0f, -16000.0f) > 0.0f, "edge source is stamped");
}

void TestFalloffAndChannels()
{
	auto map = std::make_unique<Map>();
	// Infantry at a cell centre: full weight there, soft into the neighbours, zero beyond the radius.
	const float cx = CellCentre(32), cy = CellCentre(32);
	AddSource(*map, SRC_INFANTRY, cx, cy, 0.0f, 10.0f);
	const float centre = Sample(*map, cx, cy);
	const float neighbour = Sample(*map, cx + kCellSize, cy);
	const float far = Sample(*map, cx + 3.0f * kCellSize, cy);
	Expect(Near(centre, kInfantryWeight), "infantry: full weight at its cell centre");
	Expect(neighbour > 0.0f && neighbour < centre, "infantry: soft falloff into the neighbour cell");
	Expect(Near(far, 0.0f), "infantry: nothing beyond its radius");
	Expect(Near(Sample(*map, cx, cy, CH_VEHICLE), 0.0f), "infantry does not touch the vehicle channel");
	// Bilinear sampling: halfway between two centres is the mean of both.
	Expect(Near(Sample(*map, cx + 0.5f * kCellSize, cy), 0.5f * (centre + neighbour)), "bilinear between centres");

	AddSource(*map, SRC_VEHICLE, cx, cy, 0.0f, 20.0f);
	Expect(Near(Sample(*map, cx, cy, CH_VEHICLE), kVehicleWeight), "vehicle goes to the vehicle channel");
	Expect(Near(Sample(*map, cx, cy), kInfantryWeight + kVehicleWeight), "channels add up in total");
	Expect(Near(map->peak, kInfantryWeight + kVehicleWeight), "peak tracks the hottest cell");
	Expect(map->sources == 2, "source count");
	map->Clear();
	Expect(Near(Sample(*map, cx, cy), 0.0f) && map->sources == 0 && Near(map->peak, 0.0f), "Clear empties the map");
}

void TestAgeing()
{
	Expect(Near(AgeFactor(0.0f, 10.0f), 1.0f), "fresh contact full weight");
	Expect(Near(AgeFactor(10.0f, 10.0f), kMinAgeFactor), "contact at its TTL keeps the minimum weight");
	Expect(Near(AgeFactor(50.0f, 10.0f), kMinAgeFactor), "age clamps at the TTL");
	Expect(Near(AgeFactor(5.0f, 0.0f), 1.0f), "no TTL means no ageing");
	auto map = std::make_unique<Map>();
	const float cx = CellCentre(10), cy = CellCentre(10);
	AddSource(*map, SRC_VEHICLE, cx, cy, 20.0f, 20.0f);
	Expect(Near(Sample(*map, cx, cy, CH_VEHICLE), kVehicleWeight * kMinAgeFactor), "old vehicle contact half weight");
}

void TestCoverRanking()
{
	// Two cover spots of equal path length: the one away from the enemy tank wins after the threat cost.
	auto map = std::make_unique<Map>();
	AddSource(*map, SRC_VEHICLE, 0.0f, 0.0f, 0.0f, 20.0f);
	const float towardTank = Sample(*map, 300.0f, 0.0f);
	const float awayFromTank = Sample(*map, 1700.0f, 0.0f);
	Expect(towardTank > awayFromTank, "spot near the tank is more dangerous");
	Expect(250.0f + CoverThreatCost(awayFromTank) < 250.0f + CoverThreatCost(towardTank), "threat cost ranks away spot first");
	Expect(Near(CoverThreatCost(-1.0f), 0.0f), "negative threat costs nothing");
	Expect(RetreatWorthIt(2.0f, 1.0f), "retreat when the spot is clearly safer");
	Expect(!RetreatWorthIt(1.0f, 0.8f), "no retreat for a marginal gain");
}

void TestBlocks()
{
	auto map = std::make_unique<Map>();
	const float tx = CellCentre(40), ty = CellCentre(20);
	AddSource(*map, SRC_VEHICLE, tx, ty, 0.0f, 20.0f);
	Expect(Sample(*map, tx + 200.0f, ty, CH_VEHICLE) >= kVehicleBlockThreshold, "engineer next to a tank is above the threshold");
	Expect(Sample(*map, tx + 1500.0f, ty, CH_VEHICLE) < kVehicleBlockThreshold, "1500 HU away is below the threshold");

	Block out;
	Expect(!BuildingBlocked(*map, 526, tx, ty + 1000.0f, 100.0f, out), "no mark, not blocked");
	MarkBlock(*map, 526, tx + 200.0f, ty, 100.0f);
	Expect(BuildingBlocked(*map, 526, tx, ty + 1000.0f, 110.0f, out) && out.building == 526, "own mark blocks");
	Expect(BuildingBlocked(*map, 527, tx + 300.0f, ty, 110.0f, out), "building next to the mark is blocked too");
	Expect(!BuildingBlocked(*map, 528, tx + 3000.0f, ty, 110.0f, out), "far building stays free");
	Expect(ActiveBlocks(*map, 110.0f) == 1, "one active mark");

	// After the minimum it holds only while the vehicle is still known there.
	Expect(BuildingBlocked(*map, 526, 0.0f, 0.0f, 100.0f + kBlockMinSeconds + 5.0f, out), "vehicle still known: holds");
	map->Clear();
	Expect(BuildingBlocked(*map, 526, 0.0f, 0.0f, 100.0f + kBlockMinSeconds - 1.0f, out), "minimum hold without vehicle");
	Expect(!BuildingBlocked(*map, 526, 0.0f, 0.0f, 100.0f + kBlockMinSeconds + 1.0f, out), "vehicle gone: released");
	AddSource(*map, SRC_VEHICLE, tx, ty, 0.0f, 20.0f);
	Expect(!BuildingBlocked(*map, 526, 0.0f, 0.0f, 100.0f + kBlockMaxSeconds + 1.0f, out), "hard maximum releases it");

	// Refresh keeps the start; a full table replaces the oldest mark.
	map->Reset();
	AddSource(*map, SRC_VEHICLE, tx, ty, 0.0f, 20.0f);
	MarkBlock(*map, 600, tx, ty, 10.0f);
	MarkBlock(*map, 600, tx + 50.0f, ty, 20.0f);
	Expect(ActiveBlocks(*map, 21.0f) == 1 && Near(map->blocks[0].since, 10.0f) && Near(map->blocks[0].pos[0], tx + 50.0f),
		"refresh moves the mark, keeps its start");
	for (int b = 0; b < kMaxBlocks; ++b)
		MarkBlock(*map, 700 + b, tx, ty, 30.0f + b);
	bool has600 = false;
	for (const Block& b : map->blocks)
		has600 = has600 || b.building == 600;
	Expect(!has600, "full table drops the oldest mark");
	MarkBlock(*map, 0, tx, ty, 40.0f);
	MarkBlock(*map, -5, tx, ty, 40.0f);
	Expect(ActiveBlocks(*map, 41.0f) == kMaxBlocks, "invalid building index is ignored");
}

void TestUpdateCost()
{
	// Worst case per team and update: 32 contacts (incl. lvl3 turrets), 4 hot zones, 8 damage events.
	auto map = std::make_unique<Map>();
	constexpr int kRounds = 200;
	const auto start = std::chrono::steady_clock::now();
	for (int r = 0; r < kRounds; ++r)
	{
		map->Clear();
		for (int i = 0; i < 32; ++i)
		{
			const float x = -6000.0f + 400.0f * i, y = 3000.0f - 150.0f * i;
			const SourceKind kind = i % 8 == 0 ? SRC_TURRET : (i % 5 == 0 ? SRC_VEHICLE : (i % 7 == 0 ? SRC_APC : SRC_INFANTRY));
			AddSource(*map, kind, x, y, 2.0f, 10.0f, kind == SRC_TURRET ? 4000.0f : 0.0f);
		}
		for (int z = 0; z < 4; ++z)
			AddSource(*map, SRC_HOT_ZONE, 1000.0f * z, 0.0f, 0.0f, 0.0f, 1200.0f);
		for (int d = 0; d < 8; ++d)
			AddSource(*map, SRC_OWN_DAMAGE, -2000.0f, 500.0f * d, 0.0f, 0.0f);
	}
	const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kRounds;
	std::printf("threat map worst-case update: %.1f us (cells %d, sources %d, hot>=1: %d)\n", us, kCellCount,
		map->sources, HotCells(*map, 1.0f));
	// Generous bound so a loaded CI machine does not flake; the target in game is well under 1 ms.
	Expect(us < 5000.0, "worst-case update well under 5 ms");
	Expect(map->sources == 44, "all worst-case sources stamped");
}
} // namespace

int main()
{
	TestEmptyAndBounds();
	TestFalloffAndChannels();
	TestAgeing();
	TestCoverRanking();
	TestBlocks();
	TestUpdateCost();
	if (g_failures != 0)
	{
		std::fprintf(stderr, "%d threat map test(s) failed\n", g_failures);
		return 1;
	}
	std::printf("threat map tests passed\n");
	return 0;
}
