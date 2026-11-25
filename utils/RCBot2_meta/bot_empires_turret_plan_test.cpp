// Offline unit tests for bot_empires_turret_plan.h (TODO 5.31 ML turrets).
#include "bot_empires_turret_plan.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
int g_failures = 0;

void Expect(bool cond, const char* msg)
{
	if (!cond)
	{
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
}

void TestStandardMapPlan()
{
	const auto target = EmpiresTurretPlan::TargetForMap("emp_crossroads");
	Expect(target.maxMG == 1 && target.maxML == 1 && !target.lineFormation,
		"standard map targets 1 MG + 1 ML without line formation");

	// Empty refinery -> MG first at 0 deg offset.
	const auto first = EmpiresTurretPlan::PlanNextRefineryTurret(target, 0, 0, 90.0f, true, 90.0f, 0);
	Expect(first.shouldBuild && first.buildItem == EmpiresTurretPlan::kItemMGTurret && std::fabs(first.yawOffsetDeg) < 0.1f,
		"empty refinery chooses MG turret first");

	// 1 MG standing -> ML turret second at +-70 deg.
	const auto second = EmpiresTurretPlan::PlanNextRefineryTurret(target, 1, 0, 90.0f, true, 45.0f, 0);
	Expect(second.shouldBuild && second.buildItem == EmpiresTurretPlan::kItemMLTurret &&
			std::fabs(second.yawOffsetDeg - 70.0f) < 0.1f,
		"refinery with MG chooses ML turret second on sheltered side");

	// 1 MG + 1 ML -> covered.
	const auto full = EmpiresTurretPlan::PlanNextRefineryTurret(target, 1, 1, 90.0f, true, 45.0f, 0);
	Expect(!full.shouldBuild, "refinery with 1 MG + 1 ML needs no more turrets");

	// 0 MG + 1 ML (MG destroyed) -> rebuilds MG.
	const auto rebuildMG = EmpiresTurretPlan::PlanNextRefineryTurret(target, 0, 1, 90.0f, false, 0.0f, 0);
	Expect(rebuildMG.shouldBuild && rebuildMG.buildItem == EmpiresTurretPlan::kItemMGTurret,
		"refinery missing MG rebuilds MG even when ML stands");
}

void TestRainbowroadLinePlan()
{
	const auto target = EmpiresTurretPlan::TargetForMap("emp_rainbowroad_v2");
	Expect(target.maxMG == 1 && target.maxML == 4 && target.lineFormation && target.countRadius >= 700.0f,
		"rainbowroad targets 1 MG + 4 ML in a line");

	const auto mg = EmpiresTurretPlan::PlanNextRefineryTurret(target, 0, 0, 0.0f, false, 0.0f, 0);
	Expect(mg.shouldBuild && mg.buildItem == EmpiresTurretPlan::kItemMGTurret && std::fabs(mg.lateralOffset) < 0.1f,
		"rainbowroad places MG in center of line");

	const auto ml1 = EmpiresTurretPlan::PlanNextRefineryTurret(target, 1, 0, 0.0f, false, 0.0f, 0);
	const auto ml2 = EmpiresTurretPlan::PlanNextRefineryTurret(target, 1, 1, 0.0f, false, 0.0f, 0);
	const auto ml3 = EmpiresTurretPlan::PlanNextRefineryTurret(target, 1, 2, 0.0f, false, 0.0f, 0);
	const auto ml4 = EmpiresTurretPlan::PlanNextRefineryTurret(target, 1, 3, 0.0f, false, 0.0f, 0);
	const auto done = EmpiresTurretPlan::PlanNextRefineryTurret(target, 1, 4, 0.0f, false, 0.0f, 0);
	Expect(ml1.shouldBuild && ml1.buildItem == EmpiresTurretPlan::kItemMLTurret && std::fabs(ml1.lateralOffset + 220.0f) < 0.1f,
		"rainbowroad ML #1 at -220");
	Expect(ml2.shouldBuild && ml2.buildItem == EmpiresTurretPlan::kItemMLTurret && std::fabs(ml2.lateralOffset - 220.0f) < 0.1f,
		"rainbowroad ML #2 at +220");
	Expect(ml3.shouldBuild && ml3.buildItem == EmpiresTurretPlan::kItemMLTurret && std::fabs(ml3.lateralOffset + 440.0f) < 0.1f,
		"rainbowroad ML #3 at -440");
	Expect(ml4.shouldBuild && ml4.buildItem == EmpiresTurretPlan::kItemMLTurret && std::fabs(ml4.lateralOffset - 440.0f) < 0.1f,
		"rainbowroad ML #4 at +440");
	Expect(!done.shouldBuild, "rainbowroad stops after 1 MG + 4 ML");
}

void TestLabelsAndClasses()
{
	Expect(std::strcmp(EmpiresTurretPlan::TurretClassForItem(1), "emp_building_mgturret") == 0, "item 1 -> mgturret");
	Expect(std::strcmp(EmpiresTurretPlan::TurretClassForItem(2), "emp_building_mlturret") == 0, "item 2 -> mlturret");
	Expect(std::strcmp(EmpiresTurretPlan::TurretLabelForItem(1), "mg") == 0, "item 1 label mg");
	Expect(std::strcmp(EmpiresTurretPlan::TurretLabelForItem(2), "ml") == 0, "item 2 label ml");
}
} // namespace

int main()
{
	TestStandardMapPlan();
	TestRainbowroadLinePlan();
	TestLabelsAndClasses();
	if (g_failures > 0)
	{
		std::fprintf(stderr, "bot_empires_turret_plan_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("bot_empires_turret_plan_test: PASS\n");
	return 0;
}
