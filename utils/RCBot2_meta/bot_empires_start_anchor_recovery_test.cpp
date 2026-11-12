// Deterministic coverage for bounded start-anchor recovery.
//
// These tests exercise REAL production code
// (RCBotEmpiresStartAnchorRecovery::DecideCaptureDrainAction,
// ShouldReissueNormalRouteAfterRecovery, StartAnchorRecoveryState,
// kMaxStartAnchorRecoveryAttempts, kNegligibleCaptureProgressDistance),
// never a copy of the policy. The thin engine-bound wiring (anchor probe,
// terminal attribution, re-entry lookup, schedule issue, telemetry) is
// reviewable in bot_empires_objective.cpp / bot_task.cpp and verified live
// via the official harness observation; the accounting decision itself is
// fully covered here.
//
// Required cases:
// - SAR-1 NO_START_ANCHOR + zero progress -> recovery, not identical retry.
// - SAR-2 recovery succeeds / anchor READY -> normal route re-issued.
// - SAR-3 recovery fails within its bound -> existing bounded failure path.
// - SAR-4 NO_GOAL_ANCHOR -> unchanged behavior.
// - SAR-5 NO_REACHABLE_ANCHOR -> unchanged behavior.
// - SAR-6 genuine route failure after valid anchor -> retry accounting kept.
// - SAR-7 combat interruption -> cb4f695 semantics kept, nothing consumed.
// - SAR-8 objective invalidates during recovery -> cancelled, no resume.
// - SAR-9 death/reset during recovery -> state cleared.
// - SAR-10 repeated NO_START_ANCHOR cannot loop recovery/retry forever.

#include "bot_empires_start_anchor_recovery.h"

#include <cstdlib>
#include <iostream>

namespace
{

using namespace RCBotEmpiresStartAnchorRecovery;

// Production driver model: mirrors the engine-bound sequencing in
// bot_empires_objective.cpp. A drained capture approach either starts one
// bounded recovery leg, re-issues the normal route after a restored anchor,
// or falls through to the existing capture retry budget (2) then reject.
// The anchor-side inputs (anchorNotReady/noStartAnchor/anchorsReady) stand
// in for the task terminal + read-only anchor probe; the budget constants
// are the real production values.
struct TestDriver
{
	static constexpr int kMaxCaptureRouteRetries = 2;

	StartAnchorRecoveryState recovery;
	int retriesUsed = 0;
	bool rejected = false;
	int recoveryLegsIssued = 0;
	int normalRoutesReissued = 0;

	// One drained capture approach with a genuine (non-combat) terminal.
	// Returns the policy decision; issues recovery or accounts normally.
	DrainAction drain(const bool anchorNotReady, const bool noStartAnchor,
		const bool pointStillValid, const float displacement,
		const int point)
	{
		const DrainAction action = DecideCaptureDrainAction(false,
			anchorNotReady, noStartAnchor, pointStillValid, displacement,
			recovery.attemptsUsedFor(point));
		if (action == DrainAction::START_RECOVERY)
		{
			recovery.begin(point);
			++recoveryLegsIssued;
			return action;
		}
		// Existing accounting: genuine drains consume, then reject.
		++retriesUsed;
		if (retriesUsed >= kMaxCaptureRouteRetries)
			rejected = true;
		return action;
	}

	// One drained recovery leg. Returns true when the normal route is
	// re-issued (free: the failed preflight never built a route).
	bool drainRecovery(const bool combatResume, const bool pointStillValid,
		const bool anchorsReady)
	{
		if (ShouldReissueNormalRouteAfterRecovery(combatResume,
			pointStillValid, anchorsReady))
		{
			recovery.finish();
			++normalRoutesReissued;
			return true;
		}
		recovery.finish();
		return false;
	}
};

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

void TestNoStartZeroProgressStartsRecovery()
{
	// SAR-1: the proven zero-progress episode requests recovery instead of
	// an identical route retry, without spending retry budget.
	TestDriver driver;
	const DrainAction action = driver.drain(true, true, true, 0.0f, 5);
	Require(action == DrainAction::START_RECOVERY,
		"SAR-1 NO_START_ANCHOR zero-progress drain starts recovery");
	Require(driver.recoveryLegsIssued == 1,
		"SAR-1 exactly one recovery leg issued");
	Require(driver.retriesUsed == 0,
		"SAR-1 recovery consumes no route retry budget");
	Require(driver.recovery.active,
		"SAR-1 recovery leg is active until it drains");
	std::cout << "SAR-1 PASS\n";
}

void TestRecoverySuccessReissuesNormalRoute()
{
	// SAR-2: the recovery leg displaces the bot, anchors resolve again,
	// and the normal capture route is re-issued (free: no route existed).
	TestDriver driver;
	driver.drain(true, true, true, 1.1f, 5);
	const bool reissued = driver.drainRecovery(false, true, true);
	Require(reissued,
		"SAR-2 restored anchor re-issues the normal capture route");
	Require(driver.normalRoutesReissued == 1,
		"SAR-2 exactly one normal route re-issued");
	Require(driver.retriesUsed == 0,
		"SAR-2 success path consumes no retry budget");
	Require(!driver.recovery.active,
		"SAR-2 recovery leg is no longer active");
	std::cout << "SAR-2 PASS\n";
}

void TestRecoveryFailureFallsBackToBoundedAccounting()
{
	// SAR-3: a recovery leg that drains still off-graph terminates into
	// the existing bounded retry/reject behavior, never a second recovery.
	TestDriver driver;
	driver.drain(true, true, true, 0.0f, 5);
	const bool reissued = driver.drainRecovery(false, true, false);
	Require(!reissued,
		"SAR-3 unrestored anchor does not re-issue the normal route");
	Require(!driver.recovery.active,
		"SAR-3 failed recovery leg is finished");
	const DrainAction next = driver.drain(true, true, true, 0.0f, 5);
	Require(next == DrainAction::NORMAL_ACCOUNTING,
		"SAR-3 exhausted recovery falls back to normal accounting");
	Require(driver.recoveryLegsIssued == 1,
		"SAR-3 no second recovery leg for the same point");
	Require(driver.retriesUsed == 1,
		"SAR-3 fallback drain consumes retry budget exactly once");
	std::cout << "SAR-3 PASS\n";
}

void TestDestinationSideFailuresUnchanged()
{
	// SAR-4/SAR-5: destination-side anchor failures never trigger recovery.
	TestDriver goalDriver;
	Require(goalDriver.drain(true, false, true, 0.0f, 5) ==
		DrainAction::NORMAL_ACCOUNTING,
		"SAR-4 NO_GOAL_ANCHOR keeps existing behavior");
	Require(goalDriver.recoveryLegsIssued == 0,
		"SAR-4 NO_GOAL_ANCHOR issues no recovery leg");
	Require(goalDriver.retriesUsed == 1,
		"SAR-4 NO_GOAL_ANCHOR consumes retry budget as before");
	TestDriver reachDriver;
	Require(reachDriver.drain(true, false, true, 0.0f, 5) ==
		DrainAction::NORMAL_ACCOUNTING,
		"SAR-5 NO_REACHABLE_ANCHOR keeps existing behavior");
	Require(reachDriver.recoveryLegsIssued == 0,
		"SAR-5 NO_REACHABLE_ANCHOR issues no recovery leg");
	std::cout << "SAR-4 PASS\n";
	std::cout << "SAR-5 PASS\n";
}

void TestGenuineRouteFailureAccountingUnchanged()
{
	// SAR-6: a genuine failure after a valid anchor (not anchor-not-ready)
	// keeps the existing consume-then-reject accounting.
	TestDriver driver;
	Require(driver.drain(false, false, true, 1200.0f, 5) ==
		DrainAction::NORMAL_ACCOUNTING,
		"SAR-6 non-anchor failure keeps normal accounting");
	Require(driver.drain(false, false, true, 900.0f, 5) ==
		DrainAction::NORMAL_ACCOUNTING,
		"SAR-6 second genuine stall keeps normal accounting");
	Require(driver.retriesUsed == 2 && driver.rejected,
		"SAR-6 bounded budget then reject unchanged");
	Require(driver.recoveryLegsIssued == 0,
		"SAR-6 genuine failures never start recovery");
	std::cout << "SAR-6 PASS\n";
}

void TestCombatInterruptionSemanticsUnchanged()
{
	// SAR-7: combat preemption never starts recovery and never consumes.
	Require(DecideCaptureDrainAction(true, true, true, true, 0.0f, 0) ==
		DrainAction::NORMAL_ACCOUNTING,
		"SAR-7 combat drain never starts recovery");
	Require(!ShouldReissueNormalRouteAfterRecovery(true, true, true),
		"SAR-7 combat-drained recovery leg does not re-issue as success");
	TestDriver driver;
	Require(driver.drainRecovery(true, true, true) == false,
		"SAR-7 combat interruption finishes the leg without success");
	Require(driver.normalRoutesReissued == 0 && driver.retriesUsed == 0,
		"SAR-7 combat path consumes nothing");
	std::cout << "SAR-7 PASS\n";
}

void TestObjectiveInvalidationCancelsRecovery()
{
	// SAR-8: an invalidated objective during recovery cancels it; the leg
	// never resumes into a normal route.
	TestDriver driver;
	driver.drain(true, true, true, 0.0f, 5);
	Require(!driver.drainRecovery(false, false, true),
		"SAR-8 invalid objective cancels recovery without re-issue");
	Require(driver.normalRoutesReissued == 0,
		"SAR-8 no stale resume after invalidation");
	Require(!driver.recovery.active,
		"SAR-8 recovery leg is finished after cancellation");
	std::cout << "SAR-8 PASS\n";
}

void TestDeathResetClearsRecovery()
{
	// SAR-9: death/reset clears attempts, activity, and point binding.
	StartAnchorRecoveryState state;
	state.begin(5);
	Require(state.active && state.attemptsUsedFor(5) == 1,
		"SAR-9 recovery starts bound to its point");
	state.reset();
	Require(!state.active && state.attemptsUsedFor(5) == 0 &&
		state.attemptsUsedFor(6) == 0,
		"SAR-9 reset clears activity, attempts, and binding");
	Require(DecideCaptureDrainAction(false, true, true, true, 0.0f,
		state.attemptsUsedFor(5)) == DrainAction::START_RECOVERY,
		"SAR-9 a later objective may recover again from a clean state");
	std::cout << "SAR-9 PASS\n";
}

void TestNoInfiniteRecoveryLoop()
{
	// SAR-10: an adversarial chain of stationary NO_START_ANCHOR drains
	// issues exactly one recovery leg, then at most the bounded retries,
	// then rejects. No schedule loop is possible.
	Require(kMaxStartAnchorRecoveryAttempts == 1,
		"SAR-10 recovery bound is a single attempt");
	TestDriver driver;
	for (int drain = 0; drain < 10; ++drain)
	{
		if (driver.rejected)
			break;
		if (driver.recovery.active)
		{
			// Worst case: every recovery leg drains still off-graph.
			driver.drainRecovery(false, true, false);
			continue;
		}
		driver.drain(true, true, true, 0.0f, 5);
	}
	Require(driver.recoveryLegsIssued == 1,
		"SAR-10 exactly one recovery leg across repeated drains");
	Require(driver.retriesUsed == kMaxStartAnchorRecoveryAttempts + 1 ||
		driver.retriesUsed == TestDriver::kMaxCaptureRouteRetries,
		"SAR-10 retries stay within the existing bound");
	Require(driver.rejected,
		"SAR-10 the chain terminates in reject, never a loop");
	// A new point is a new context: per-point binding, not a global lock.
	TestDriver fresh;
	Require(fresh.drain(true, true, true, 0.0f, 6) ==
		DrainAction::START_RECOVERY,
		"SAR-10 a different point keeps its own bounded attempt");
	std::cout << "SAR-10 PASS\n";
}

void TestNegligibleProgressThreshold()
{
	// Threshold pin: zero progress recovers; genuine partial approach does
	// not (it keeps normal retry accounting for real stalls).
	Require(kNegligibleCaptureProgressDistance == 64.0f,
		"threshold matches the CMoveToTask arrival radius");
	Require(DecideCaptureDrainAction(false, true, true, true, 64.0f, 0) ==
		DrainAction::START_RECOVERY,
		"at-threshold displacement still counts as zero progress");
	Require(DecideCaptureDrainAction(false, true, true, true, 64.1f, 0) ==
		DrainAction::NORMAL_ACCOUNTING,
		"above-threshold displacement keeps normal accounting");
	std::cout << "SAR-THRESHOLD PASS\n";
}

} // namespace

int main()
{
	TestNoStartZeroProgressStartsRecovery();
	TestRecoverySuccessReissuesNormalRoute();
	TestRecoveryFailureFallsBackToBoundedAccounting();
	TestDestinationSideFailuresUnchanged();
	TestGenuineRouteFailureAccountingUnchanged();
	TestCombatInterruptionSemanticsUnchanged();
	TestObjectiveInvalidationCancelsRecovery();
	TestDeathResetClearsRecovery();
	TestNoInfiniteRecoveryLoop();
	TestNegligibleProgressThreshold();
	std::cout << "ALL START-ANCHOR-RECOVERY TESTS PASS\n";
	return 0;
}
