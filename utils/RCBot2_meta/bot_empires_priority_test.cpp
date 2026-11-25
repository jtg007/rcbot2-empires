// Offline unit tests for bot_empires_priority.h (TODO 9.2 priority tiers).
#include "bot_empires_priority.h"

#include <cstdio>

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
} // namespace

int main()
{
	using namespace EmpiresPriority;
	// Building tiers.
	Expect(BuildingTier("emp_building_radar", false, false) == TIER_CORE, "radar is core");
	Expect(BuildingTier("emp_building_refinery", false, false) == TIER_CORE, "refinery is core");
	Expect(BuildingTier("emp_building_vehiclefactory", false, false) == TIER_CORE, "factory is core");
	Expect(BuildingTier("emp_building_barracks", true, false) == TIER_CORE, "first barracks is core");
	Expect(BuildingTier("emp_building_barracks", false, false) == TIER_FRONT, "second barracks is forward");
	Expect(BuildingTier("emp_building_repairstation", false, false) == TIER_FRONT, "repair pad is front");
	Expect(BuildingTier("emp_building_armory", false, false) == TIER_FRONT, "armory is front");
	Expect(BuildingTier("emp_building_radar", false, true) == TIER_FRONT, "damaged core repair is front");
	Expect(BuildingTier("emp_building_mgturret", false, false) == TIER_COMFORT, "MG turret is comfort");
	Expect(BuildingTier("emp_building_mlturret", false, false) == TIER_COMFORT, "ML turret is comfort");
	Expect(BuildingTier("emp_eng_map_brush", false, false) == TIER_MAP, "map ramp is map play");
	Expect(BuildingTier("emp_building_nf_wall_eng", false, false) == TIER_COMFORT, "engineer deployable is comfort");
	Expect(BuildingTier(nullptr, false, false) == TIER_MAP, "unknown is map play");

	// Preemption: higher tier interrupts lower, never equal or higher, never a non-interruptible job.
	const Job idle;
	const Job wall{TIER_COMFORT, true};
	const Job turretPlacing{TIER_COMFORT, false};
	const Job forward{TIER_FRONT, true};
	const Job radar{TIER_CORE, true};
	Expect(Preempts(TIER_COMFORT, idle), "idle takes anything");
	Expect(Preempts(TIER_CORE, wall), "radar preempts a wall (run 133129: NF radar 922 s unaccepted)");
	Expect(Preempts(TIER_FRONT, wall), "repair pad preempts a wall (run 133129: BE pad given up)");
	Expect(Preempts(TIER_CORE, forward), "core preempts forward barracks");
	Expect(!Preempts(TIER_FRONT, radar), "pad does not preempt the radar");
	Expect(!Preempts(TIER_CORE, radar), "equal tier does not preempt");
	Expect(!Preempts(TIER_CORE, turretPlacing), "a running placement finishes");
	Expect(!Preempts(TIER_COMFORT, wall), "a turret does not preempt a wall");

	// Candidate order: idle first, then lowest-priority job, then distance.
	Expect(BetterCandidate(idle, 5000.0f, wall, 100.0f), "idle engineer before a busy one");
	Expect(BetterCandidate(wall, 5000.0f, forward, 100.0f), "wall builder before forward-barracks builder");
	Expect(BetterCandidate(wall, 100.0f, wall, 200.0f), "same tier: nearer first");
	Expect(!BetterCandidate(wall, 300.0f, wall, 200.0f), "same tier: farther later");

	// Escalation: tier 1/2 never given up.
	Expect(NeverGiveUp(TIER_CORE) && NeverGiveUp(TIER_FRONT), "core and front are retried");
	Expect(!NeverGiveUp(TIER_COMFORT) && !NeverGiveUp(TIER_MAP), "lower tiers may be given up");

	if (g_failures != 0)
	{
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("priority tests PASS\n");
	return 0;
}
