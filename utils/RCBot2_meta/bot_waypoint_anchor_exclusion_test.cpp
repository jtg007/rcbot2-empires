#include "bot_waypoint_anchor_exclusion.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
	void require(const bool condition, const char* label)
	{
		if (!condition)
		{
			std::cerr << label << " FAIL\n";
			std::exit(EXIT_FAILURE);
		}
		std::cout << label << " PASS\n";
	}

	int selectAnchor(const std::vector<int>& ranked,
		RCBotWaypoint::StartAnchorExclusions& excluded,
		const std::uint64_t goalKey, const float now)
	{
		for (const int waypoint : ranked)
			if (!excluded.IsExcluded(waypoint, goalKey, now))
				return waypoint;
		return -1;
	}
}

int main()
{
	constexpr std::uint64_t originalGoal = 0x147148;
	constexpr std::uint64_t otherGoal = 0x148149;
	constexpr float lifetime = 15.0f;
	const std::vector<int> ranked{147, 146, 145};

	RCBotWaypoint::StartAnchorExclusions botA;
	RCBotWaypoint::StartAnchorExclusions botB;
	// WAEX-01: a successful nearest anchor is unchanged.
	require(selectAnchor(ranked, botA, originalGoal, 0.0f) == 147,
		"WAEX-01 nearest valid anchor unchanged");

	// WAEX-02/03: a local no-progress exclusion advances to candidate two.
	require(botA.MarkFailed(147, originalGoal, 1.0f, lifetime),
		"WAEX-02 failed local start anchor recorded");
	require(selectAnchor(ranked, botA, originalGoal, 1.1f) == 146,
		"WAEX-03 next ranked start anchor selected");

	// WAEX-04: exclusions are keyed to the original task goal.
	require(selectAnchor(ranked, botA, originalGoal, 1.2f) == 146 &&
		selectAnchor(ranked, botA, otherGoal, 1.2f) == 147,
		"WAEX-04 higher-level goal preserved");

	// WAEX-05: a second bot has independent anchor-failure state.
	require(selectAnchor(ranked, botB, originalGoal, 1.2f) == 147,
		"WAEX-05 exclusions are per bot");

	// WAEX-06: the exclusion expires and the original nearest anchor is eligible.
	require(selectAnchor(ranked, botA, originalGoal, 16.0f) == 147 &&
		botA.FailureCount(originalGoal, 16.0f) == 0,
		"WAEX-06 exclusion expires");

	// WAEX-07: no anchor can be selected twice during one bounded fallback chain.
	for (std::size_t index = 0; index < ranked.size(); ++index)
	{
		const int next = selectAnchor(ranked, botA, originalGoal, 20.0f);
		require(next == ranked[index], "WAEX-07 ranked retry advances once");
		require(botA.MarkFailed(next, originalGoal, 20.0f, lifetime),
			"WAEX-07 retry remains bounded");
	}
	require(selectAnchor(ranked, botA, originalGoal, 20.1f) == -1 &&
		botA.FailureCount(originalGoal, 20.1f) == ranked.size(),
		"WAEX-07 no anchor cycling after candidate bound");
	return EXIT_SUCCESS;
}
