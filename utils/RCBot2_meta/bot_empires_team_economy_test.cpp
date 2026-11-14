#include "bot_empires_team_economy.h"

#include <cstdio>
#include <cstdlib>

using namespace EmpiresTeamEconomy;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		++failures;
	}
}
} // namespace

int main()
{
	// Wages cover the price: pool and reserve do not matter.
	Check(PurchaseAllowed(300.0f, 0.0f, 250.0f, 1000.0f), "wages pay all");
	// Pool pays the rest only if the reserve stays.
	Check(PurchaseAllowed(0.0f, 650.0f, 250.0f, 400.0f), "pool keeps reserve");
	Check(!PurchaseAllowed(0.0f, 600.0f, 250.0f, 400.0f), "pool would cut reserve");
	Check(PurchaseAllowed(100.0f, 550.0f, 250.0f, 400.0f), "wages part, pool rest keeps reserve");
	Check(!PurchaseAllowed(-1.0f, 600.0f, 250.0f, 400.0f), "unknown wages count as 0");
	Check(PurchaseAllowed(0.0f, -1.0f, 250.0f, 400.0f), "unknown pool: let the game decide");

	TeamState state;
	Check(ActiveReserve(state, 10.0f) == 0.0f, "no plan, no reserve");
	PublishReserve(state, 500.0f, 100.0f);
	Check(ActiveReserve(state, 110.0f) == 500.0f, "fresh reserve");
	Check(ActiveReserve(state, 116.0f) == 0.0f, "stale reserve expires");
	PublishReserve(state, 500.0f, 100.0f);
	KeepReserve(state, 114.0f);
	Check(ActiveReserve(state, 125.0f) == 500.0f, "kept reserve stays");
	PublishReserve(state, -5.0f, 120.0f);
	Check(ActiveReserve(state, 120.0f) == 0.0f, "negative reserve clamps");

	state.pendingUser = 3;
	state.poolBefore = 8362.0f;
	state.wagesBefore = 0.0f;
	LearnVehicleCost(state, 8117.0f, 0.0f);
	Check(state.vehicleCost == 245.0f && state.pendingUser == -1, "learn from pool");
	state.pendingUser = 3;
	state.poolBefore = 1000.0f;
	state.wagesBefore = 100.0f;
	LearnVehicleCost(state, 850.0f, 0.0f);
	Check(state.vehicleCost == 250.0f, "learn from pool + wages");
	state.pendingUser = 3;
	state.poolBefore = 1000.0f;
	state.wagesBefore = 0.0f;
	LearnVehicleCost(state, 1010.0f, 0.0f);
	Check(state.vehicleCost == 250.0f, "ignore implausible");

	Check(DeferToRicherBuyer(0.0f, 50.0f, 222.0f, 0.0f, 30.0f), "defer to richer");
	Check(!DeferToRicherBuyer(230.0f, 500.0f, 222.0f, 0.0f, 30.0f), "own wages cover");
	Check(!DeferToRicherBuyer(20.0f, 25.0f, 222.0f, 0.0f, 30.0f), "small gap");
	Check(!DeferToRicherBuyer(0.0f, 50.0f, 222.0f, 30.0f, 30.0f), "defer bounded");
	Check(!DeferToRicherBuyer(0.0f, -1.0f, 222.0f, 0.0f, 30.0f), "nobody else");

	Team(2).reserve = 7.0f;
	Reset();
	Check(Team(2).reserve == 0.0f && &Team(99) == &Team(0), "reset and range");

	if (failures == 0)
		std::printf("team economy tests passed\n");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
