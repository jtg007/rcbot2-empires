#pragma once

// Shared money picture of one team (TODO 5.5a): the Commander publishes what it needs for
// its next building/research (reserve); bots buying vehicles from the team pool leave that
// much in it. Purchases paid from the buyer's own wages never touch the pool (RE
// vehicle-driving.md "Wages and team pool"), so they are always allowed.
// Pure logic, no engine types: unit test bot_empires_team_economy_test.cpp.
namespace EmpiresTeamEconomy
{
constexpr int kTeams = 4;
constexpr float kReserveLifetime = 15.0f;   // no fresh plan from a Commander = no reserve
constexpr float kDefaultVehicleCost = 250.0f; // light tank, cannon+MG (runs 15:5x, 192/245)

struct TeamState
{
	float reserve = 0.0f;
	float reserveAt = -1.0e9f;
	float vehicleCost = kDefaultVehicleCost; // learned from the last purchase
	// Snapshot right before emp_vehicle_build, to learn the price afterwards.
	int pendingUser = -1;
	float poolBefore = 0.0f;
	float wagesBefore = 0.0f;
};

inline TeamState& Team(int team)
{
	static TeamState teams[kTeams];
	return teams[team >= 0 && team < kTeams ? team : 0];
}

inline void Reset()
{
	for (int i = 0; i < kTeams; ++i)
		Team(i) = TeamState();
}

inline void PublishReserve(TeamState& state, float reserve, float now)
{
	state.reserve = reserve > 0.0f ? reserve : 0.0f;
	state.reserveAt = now;
}

// The Commander is still on the same plan but waits (unfinished building, result check):
// keep its last reserve alive.
inline void KeepReserve(TeamState& state, float now)
{
	state.reserveAt = now;
}

inline float ActiveReserve(const TeamState& state, float now)
{
	return now - state.reserveAt <= kReserveLifetime ? state.reserve : 0.0f;
}

// The game takes wages first, the rest from the pool (0x6545bd / 0x65595f). Unknown wages
// (< 0) count as 0. Unknown pool (< 0): allowed, the game itself rejects what it cannot pay.
inline bool PurchaseAllowed(float wages, float pool, float cost, float reserve)
{
	if (wages < 0.0f)
		wages = 0.0f;
	if (wages >= cost || pool < 0.0f)
		return true;
	return pool - (cost - wages) >= reserve;
}

// Price = what left the pool plus what left the wages. Pool income in between makes it
// slightly low; implausible values are ignored.
inline void LearnVehicleCost(TeamState& state, float poolAfter, float wagesAfter)
{
	if (state.pendingUser < 0 || state.poolBefore < 0.0f || poolAfter < 0.0f)
		return;
	const float paid = (state.poolBefore - poolAfter) +
		(state.wagesBefore > 0.0f && wagesAfter >= 0.0f ? state.wagesBefore - wagesAfter : 0.0f);
	state.pendingUser = -1;
	if (paid >= 50.0f && paid <= 3000.0f)
		state.vehicleCost = paid;
}
} // namespace EmpiresTeamEconomy
