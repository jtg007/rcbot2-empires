#include <cassert>
#include <iostream>

#include "bot_navigation_attempt.h"

using namespace RCBotNavigation;

static AttemptKey MakeWaypointKey(const int goalId, const Vector& goal,
	const int currentWaypoint, const int nextWaypoint)
{
	AttemptKey key;
	key.provider = Provider::WAYPOINT;
	key.goalId = goalId;
	key.goalSignature = NavigationAttempt::MakeGoalSignature(goalId, goal);
	key.segment = {currentWaypoint, nextWaypoint};
	return key;
}

int main()
{
	const Vector goal(100.0f, 0.0f, 0.0f);
	const AttemptKey first = MakeWaypointKey(7, goal, 1, 2);
	NavigationAttempt attempt;

	// A: real distance progress and a segment transition keep the attempt alive.
	assert(attempt.Start(0, 42, 2, first, Vector(0, 0, 0), 0.0f, 100.0f));
	assert(attempt.Observe(1.0f, Vector(1, 0, 0), 98.0f, {1, 2}) ==
		Observation::NONE);
	assert(attempt.Observe(1.1f, Vector(1, 0, 0), 95.0f, {1, 2}) ==
		Observation::PROGRESS);
	assert(attempt.Observe(1.2f, Vector(1, 0, 0), 95.0f, {2, 3}) ==
		Observation::PROGRESS);
	assert(attempt.End(Result::COMPLETE, 1.3f));
	assert(!attempt.IsActive());
	assert(attempt.GetResult() == Result::COMPLETE);

	// B: no-progress timeout ends a blocked attempt as PATH_STUCK.
	assert(attempt.Start(0, 42, 2, first, Vector(0, 0, 0), 2.0f, 100.0f));
	assert(attempt.Observe(3.0f, Vector(1, 0, 0), 100.0f, {1, 2}) ==
		Observation::NONE);
	attempt.PauseNoProgress(2.1f);
	assert(attempt.GetProgressCount() == 0);
	assert(attempt.Observe(4.9f, Vector(-1, 0, 0), 100.0f, {1, 2}) ==
		Observation::NONE);
	assert(attempt.GetProgressCount() == 0);
	assert(attempt.Observe(5.1f, Vector(0, 0, 0), 100.0f, {1, 2}) ==
		Observation::NO_PROGRESS);
	assert(attempt.End(Result::PATH_STUCK, 5.1f));

	// C: immediate retry of the same goal/segment is blocked by cooldown.
	assert(!attempt.CanStart(first, 5.2f));
	assert(!attempt.Start(0, 42, 2, first, Vector(0, 0, 0), 5.2f, 100.0f));
	assert(!attempt.End(Result::PATH_STUCK, 5.3f));

	// D: a changed segment is an alternate route; expiry also unblocks.
	const AttemptKey alternate = MakeWaypointKey(7, goal, 1, 3);
	assert(attempt.CanStart(alternate, 5.2f));
	assert(attempt.Start(0, 42, 2, alternate, Vector(0, 0, 0), 5.2f, 100.0f));
	assert(attempt.End(Result::CANCELLED, 5.3f));
	assert(attempt.Start(0, 42, 2, first, Vector(0, 0, 0), 20.2f, 100.0f));
	assert(attempt.End(Result::CANCELLED, 20.3f));

	std::cout << "A-D PASS" << std::endl;
	return 0;
}
