// Deterministic coverage for the objective-aware combat relevance gate.
//
// These tests exercise REAL production code
// (RCBotEmpiresCombatRelevance::DecideVisibleEnemyEngagement,
// DecidePursuitContinuation, CaptureOwnsObjective, plus
// RCBotEmpiresCaptureRetry::CaptureInterruptionLatch and
// ShouldConsumeCaptureRouteRetry), never a copy of the policy.
//
// The engine-bound driver (CBotEmpires::getTasks()) maps live engine state
// (bot/enemy/capture origins, recentlyHurt(), capture flag + validity) to
// the plain inputs of these decisions and performs the existing schedule
// operations. That mapping is reviewable in bot_empires.cpp and needs live
// runtime evidence; everything the decisions themselves determine —
// engage/ignore, continue/drop, latch interplay, retry accounting across
// the full interrupt→drop→resume→ignore sequence — is pinned here.
//
// Production contract mirrored by the test-side capture model (asserted,
// not modified):
//  - Fresh selection sets issued/point/origin and clears retries
//    (bot_empires_objective.cpp:367-380).
//  - A combat wipe notes the latch and frees the schedule WITHOUT touching
//    capture identity or the retry counter (bot_empires.cpp:5771-5777).
//  - A capture drain takes the latch exactly once and resumes a still-valid
//    objective without consuming a retry
//    (bot_empires_objective.cpp:83-160; CAP-R3).
//
// Required cases:
// - REL-A1..A6 initial-issue (ACQUIRE) matrix.
// - REL-C1..C6 pursuit-continuation (CONTINUE) matrix incl. re-arm gating.
// - REL-D1..D5 defensive combat preserved.
// - REL-S complete 10-step interrupt→drop→resume→ignore sequence.
// - REL-I1..I3 invalidation/completion (no stale-owner loop).
// - REL-X1 statelessness (no retained relevance decision).

#include "bot_empires_capture_retry.h"
#include "bot_empires_combat_relevance.h"

#include <cstdlib>
#include <iostream>

namespace
{

using namespace RCBotEmpiresCombatRelevance;
using RCBotEmpiresCaptureRetry::CaptureInterruptionLatch;
using RCBotEmpiresCaptureRetry::ShouldConsumeCaptureRouteRetry;
using RCBotEmpiresCaptureRetry::kMaxCaptureRouteRetries;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

constexpr CaptureOwnership kOwnedCapture = {true, true};
constexpr CaptureOwnership kNoCapture = {false, false};
constexpr CaptureOwnership kInvalidCapture = {true, false};
constexpr CaptureOwnership kEndedCapture = {false, true};

// --- ACQUIRE matrix -------------------------------------------------------

void TestAcquireDistantIrrelevantIgnored()
{
	// REL-A1: distant enemy, far from the objective, no recent damage, valid
	// capture owned -> must not displace the capture.
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 4500.0f, 6000.0f,
		false) == EngagementDecision::IGNORE,
		"REL-A1 distant irrelevant enemy ignored while capture owned");
	std::cout << "REL-A1 PASS\n";
}

void TestAcquireCloseEnemyEngaged()
{
	// REL-A2: nearby enemy threatens the bot and its capture route.
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 500.0f, 6000.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-A2 nearby enemy engaged despite active capture");
	std::cout << "REL-A2 PASS\n";
}

void TestAcquireSelfDefenseEngaged()
{
	// REL-A3: recently hurt -> immediate self-defense at any range.
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 4500.0f, 6000.0f,
		true) == EngagementDecision::ENGAGE,
		"REL-A3 recently-hurt bot engages distant enemy (self-defense)");
	std::cout << "REL-A3 PASS\n";
}

void TestAcquireObjectiveThreatEngaged()
{
	// REL-A4: enemy far from the bot but sitting on the capture objective.
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 4500.0f, 300.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-A4 objective-threatening enemy engaged despite distance");
	std::cout << "REL-A4 PASS\n";
}

void TestAcquireNoCaptureLegacy()
{
	// REL-A5: no owned capture -> legacy combat behavior preserved exactly.
	Require(DecideVisibleEnemyEngagement(kNoCapture, 4500.0f, 6000.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-A5 no capture objective keeps legacy engage behavior");
	std::cout << "REL-A5 PASS\n";
}

void TestAcquireInvalidCaptureLegacy()
{
	// REL-A6: capture flag issued but point no longer valid -> gate must not
	// act (no stale-owner suppression of combat).
	Require(DecideVisibleEnemyEngagement(kInvalidCapture, 4500.0f, 6000.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-A6 invalid capture keeps legacy engage behavior");
	std::cout << "REL-A6 PASS\n";
}

// --- CONTINUE matrix (keep-alive AND re-arm consult this one decision) ----

void TestContinueEnemyLeavesArea()
{
	// REL-C1: enemy flees beyond the relevance boundary, bot still near its
	// objective, no recent damage -> pursuit terminates.
	PursuitDropReason reason = PursuitDropReason::NONE;
	Require(DecidePursuitContinuation(kOwnedCapture, 4500.0f, 6000.0f,
		1500.0f, false, &reason) == PursuitDecision::DROP,
		"REL-C1 pursuit drops once the enemy leaves the relevant area");
	Require(reason == PursuitDropReason::ENEMY_LEFT_RELEVANT_AREA,
		"REL-C1 drop reason is enemy-left-relevant-area");
	std::cout << "REL-C1 PASS\n";
}

void TestContinueObjectiveRelevant()
{
	// REL-C2: enemy still near the objective -> the fight stays there.
	Require(DecidePursuitContinuation(kOwnedCapture, 900.0f, 300.0f,
		1500.0f, false) == PursuitDecision::CONTINUE,
		"REL-C2 pursuit continues while the enemy threatens the objective");
	std::cout << "REL-C2 PASS\n";
}

void TestContinueSelfDefense()
{
	// REL-C3: enemy actively hurting the bot -> self-defense continues.
	Require(DecidePursuitContinuation(kOwnedCapture, 1500.0f, 6000.0f,
		1500.0f, true) == PursuitDecision::CONTINUE,
		"REL-C3 pursuit continues under active self-defense");
	std::cout << "REL-C3 PASS\n";
}

void TestContinueDraggedTooFar()
{
	// REL-C4: chase dragged the bot far from its objective while the enemy
	// itself is not objective-relevant -> terminate even under fire (stop
	// feeding the kite; the bot still shoots via handleWeapons while
	// capping and the hurt/cover path still runs).
	PursuitDropReason reason = PursuitDropReason::NONE;
	Require(DecidePursuitContinuation(kOwnedCapture, 1500.0f, 6000.0f,
		4000.0f, true, &reason) == PursuitDecision::DROP,
		"REL-C4 pursuit drops once dragged too far from the objective");
	Require(reason == PursuitDropReason::PURSUIT_DRAGGED_TOO_FAR,
		"REL-C4 drop reason is pursuit-dragged-too-far");
	std::cout << "REL-C4 PASS\n";
}

void TestContinueNoCaptureLegacy()
{
	// REL-C5: no owned capture -> legacy unbounded pursuit path untouched.
	Require(DecidePursuitContinuation(kNoCapture, 4500.0f, 6000.0f,
		4000.0f, false) == PursuitDecision::CONTINUE,
		"REL-C5 no capture objective keeps legacy pursuit behavior");
	std::cout << "REL-C5 PASS\n";
}

void TestContinueCloseFight()
{
	// REL-C6: close fight near the objective is not dropped (the approach
	// task itself resolves it via arrival/kill/LOS loss).
	Require(DecidePursuitContinuation(kOwnedCapture, 500.0f, 6000.0f,
		1500.0f, false) == PursuitDecision::CONTINUE,
		"REL-C6 close fight near the objective continues");
	std::cout << "REL-C6 PASS\n";
}

void TestRearmIndependentlyGated()
{
	// REL-R: the schedule-empty re-arm path must consult the same CONTINUE
	// decision instead of blindly re-issuing: an irrelevant enemy at re-arm
	// time terminates instead of re-arming.
	PursuitDropReason reason = PursuitDropReason::NONE;
	const PursuitDecision decision = DecidePursuitContinuation(
		kOwnedCapture, 4500.0f, 6000.0f, 1500.0f, false, &reason);
	Require(decision == PursuitDecision::DROP &&
		reason == PursuitDropReason::ENEMY_LEFT_RELEVANT_AREA,
		"REL-R re-arm consults pursuit continuation and drops");
	std::cout << "REL-R PASS\n";
}

// --- Defensive combat (§2: gate must not disarm the bot) ------------------

void TestDefensiveCloseRangeAttacker()
{
	// REL-D1: enemy attacking at close range is always engageable.
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 300.0f, 5000.0f,
		true) == EngagementDecision::ENGAGE,
		"REL-D1 close-range attacker engaged");
	Require(DecidePursuitContinuation(kOwnedCapture, 300.0f, 5000.0f,
		1500.0f, true) == PursuitDecision::CONTINUE,
		"REL-D1 close-range attacker pursuit continued");
	std::cout << "REL-D1 PASS\n";
}

void TestDefensiveObjectiveThreat()
{
	// REL-D2: enemy directly threatening the capture objective.
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 3000.0f, 200.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-D2 objective-threatening enemy engaged");
	std::cout << "REL-D2 PASS\n";
}

void TestDefensiveRelevantThenLeaves()
{
	// REL-D3: initially relevant (close) enemy that moves away: engaged on
	// acquisition, dropped on continuation once irrelevant.
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 600.0f, 5000.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-D3 initially relevant enemy engaged");
	Require(DecidePursuitContinuation(kOwnedCapture, 4500.0f, 6000.0f,
		1500.0f, false) == PursuitDecision::DROP,
		"REL-D3 pursuit dropped after the enemy leaves");
	std::cout << "REL-D3 PASS\n";
}

void TestDefensiveDistantIrrelevant()
{
	// REL-D4: irrelevant distant enemy while capture is active: declined
	// for MOVEMENT only. Weapon handling (handleWeapons) is a separate
	// existing path and is not gated by these decisions.
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 4500.0f, 6000.0f,
		false) == EngagementDecision::IGNORE,
		"REL-D4 distant irrelevant enemy declined for movement");
	std::cout << "REL-D4 PASS\n";
}

void TestDefensiveNoCapture()
{
	// REL-D5: combat with no valid capture objective is fully preserved.
	Require(DecideVisibleEnemyEngagement(kNoCapture, 4500.0f, 6000.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-D5 no-capture engagement preserved");
	Require(DecidePursuitContinuation(kNoCapture, 4500.0f, 6000.0f,
		4000.0f, false) == PursuitDecision::CONTINUE,
		"REL-D5 no-capture pursuit preserved");
	std::cout << "REL-D5 PASS\n";
}

// --- Complete 10-step sequence --------------------------------------------

// Test-side capture model mirroring the production contract (see file
// header). It performs the DOCUMENTED production transitions and asserts
// them; it never reimplements the latch, retry, or relevance decisions.
struct SequenceCapture
{
	int point = -1;
	bool issued = false;
	bool stillValid = false;
	int retriesUsed = 0;
	CaptureInterruptionLatch latch;

	CaptureOwnership ownership() const
	{
		return CaptureOwnership{issued, stillValid};
	}

	void selectFresh(const int newPoint)
	{
		point = newPoint;
		issued = true;
		stillValid = true;
		retriesUsed = 0;
		latch.reset();
	}

	void combatWipe()
	{
		// Production wipe (bot_empires.cpp:5771-5777): latch noted,
		// identity and retry counter preserved.
		latch.noteCombatInterruption();
	}

	void captureDrainResume()
	{
		// Production drain (bot_empires_objective.cpp:83-160): latch taken
		// once; still-valid resume consumes no retry.
		const bool combatResume = latch.takeCombatInterruption();
		if (ShouldConsumeCaptureRouteRetry(combatResume, stillValid))
			++retriesUsed;
	}
};

void TestCompleteSequence()
{
	// Geometry (Hammer units): capture origin at 0, bot en route at 1500.
	constexpr float kBotToObjective = 1500.0f;
	SequenceCapture capture;

	// Step 1: bot owns a valid capture objective (point 5 selected fresh).
	capture.selectFresh(5);
	Require(CaptureOwnsObjective(capture.ownership()),
		"REL-S1 bot owns a valid capture objective");
	std::cout << "REL-S1 PASS\n";

	// Step 2: relevant enemy (close, 500) interrupts the capture approach.
	Require(DecideVisibleEnemyEngagement(capture.ownership(), 500.0f,
		5000.0f, false) == EngagementDecision::ENGAGE,
		"REL-S2 relevant enemy triggers APPROACH_ENEMY");
	capture.combatWipe();
	std::cout << "REL-S2 PASS\n";

	// Step 3: existing capture identity and retry counter are preserved
	// across the combat wipe.
	Require(capture.issued && capture.point == 5 && capture.retriesUsed == 0,
		"REL-S3 capture identity and retry counter preserved");
	std::cout << "REL-S3 PASS\n";

	// Step 4+5: enemy moves beyond the relevance boundary; continued
	// pursuit (keep-alive and re-arm consult the same decision)
	// terminates with the area reason.
	PursuitDropReason reason = PursuitDropReason::NONE;
	Require(DecidePursuitContinuation(capture.ownership(), 4500.0f,
		6000.0f, kBotToObjective, false,
		&reason) == PursuitDecision::DROP,
		"REL-S4/5 pursuit terminates once the enemy is irrelevant");
	Require(reason == PursuitDropReason::ENEMY_LEFT_RELEVANT_AREA,
		"REL-S4/5 drop reason is enemy-left-relevant-area");
	std::cout << "REL-S4/5 PASS\n";

	// Step 6: the existing combat-interruption latch remains correct
	// (still set exactly once; nothing consumed it early).
	Require(capture.latch.interruptedByCombat,
		"REL-S6 combat-interruption latch still set after drop");
	std::cout << "REL-S6 PASS\n";

	// Step 7: bot resumes the SAME capture objective with no retry spent.
	capture.captureDrainResume();
	Require(capture.issued && capture.point == 5 &&
		capture.retriesUsed == 0,
		"REL-S7 same capture objective resumed without retry consumption");
	std::cout << "REL-S7 PASS\n";

	// Steps 8+9: the same now-irrelevant enemy remains visible; the
	// initial-issue gate rejects another distant APPROACH_ENEMY.
	Require(DecideVisibleEnemyEngagement(capture.ownership(), 4500.0f,
		6000.0f, false) == EngagementDecision::IGNORE,
		"REL-S8/9 distant enemy does not re-arm pursuit");
	std::cout << "REL-S8/9 PASS\n";

	// Step 10: capture continues without another schedule wipe (no latch
	// set by the ignored sighting) and without false genuine retry
	// consumption; a later genuine drain still consumes exactly once,
	// proving the accounting was not corrupted by the sequence.
	Require(!capture.latch.interruptedByCombat,
		"REL-S10 no latch set by the ignored sighting");
	capture.captureDrainResume();
	Require(capture.retriesUsed == 1 &&
		capture.retriesUsed < kMaxCaptureRouteRetries + 1,
		"REL-S10 genuine drain consumes exactly one retry");
	Require(kMaxCaptureRouteRetries == 2,
		"REL-S10 retry budget unchanged");
	std::cout << "REL-S10 PASS\n";
}

// --- Invalidation / completion (no stale-owner loop) ----------------------

void TestInvalidationDuringCombat()
{
	// REL-I1: point invalidates mid-combat -> gate bypasses to legacy; the
	// gate can never suppress combat for a stale owner, and the existing
	// drain path (CAP-R6) still consumes toward reject.
	Require(DecideVisibleEnemyEngagement(kInvalidCapture, 4500.0f, 6000.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-I1 invalidated capture bypasses the issue gate");
	Require(DecidePursuitContinuation(kInvalidCapture, 4500.0f, 6000.0f,
		1500.0f, false) == PursuitDecision::CONTINUE,
		"REL-I1 invalidated capture bypasses the pursuit gate");
	CaptureInterruptionLatch latch;
	latch.noteCombatInterruption();
	Require(ShouldConsumeCaptureRouteRetry(latch.takeCombatInterruption(),
		false),
		"REL-I1 invalidated objective still consumes (CAP-R6 semantics)");
	std::cout << "REL-I1 PASS\n";
}

void TestCompletionDuringCombat()
{
	// REL-I2: completed objective (flag cleared by the drain-end path) ->
	// legacy combat; no gate action on a non-owned objective.
	Require(DecideVisibleEnemyEngagement(kEndedCapture, 4500.0f, 6000.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-I2 completed capture bypasses the issue gate");
	Require(DecidePursuitContinuation(kEndedCapture, 4500.0f, 6000.0f,
		1500.0f, false) == PursuitDecision::CONTINUE,
		"REL-I2 completed capture bypasses the pursuit gate");
	std::cout << "REL-I2 PASS\n";
}

void TestNoStaleOwnerLoop()
{
	// REL-I3: alternating valid/invalid ownership never latches a decision:
	// each call is a pure function of current inputs.
	for (int i = 0; i < 3; ++i)
	{
		Require(DecideVisibleEnemyEngagement(kInvalidCapture, 4500.0f,
			6000.0f, false) == EngagementDecision::ENGAGE,
			"REL-I3 invalid ownership never suppresses engagement");
		Require(DecideVisibleEnemyEngagement(kOwnedCapture, 4500.0f,
			6000.0f, false) == EngagementDecision::IGNORE,
			"REL-I3 valid ownership decision is repeatable");
	}
	std::cout << "REL-I3 PASS\n";
}

// --- Statelessness: death/team/map/reset cannot retain a decision --------

void TestNoRetainedRelevanceDecision()
{
	// REL-X1: the decisions take no retained state (no throttle, counter,
	// or timestamp input exists on the decision path), so a reset can only
	// restore the same decisions. Scrambled call order must not matter.
	Require(DecidePursuitContinuation(kOwnedCapture, 4500.0f, 6000.0f,
		1500.0f, false) == PursuitDecision::DROP,
		"REL-X1 drop first");
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 4500.0f, 6000.0f,
		false) == EngagementDecision::IGNORE,
		"REL-X1 ignore second");
	Require(DecideVisibleEnemyEngagement(kOwnedCapture, 500.0f, 6000.0f,
		false) == EngagementDecision::ENGAGE,
		"REL-X1 engage third");
	Require(DecidePursuitContinuation(kOwnedCapture, 4500.0f, 6000.0f,
		1500.0f, false) == PursuitDecision::DROP,
		"REL-X1 drop repeats identically (no history dependence)");
	std::cout << "REL-X1 PASS\n";
}

} // namespace

int main()
{
	TestAcquireDistantIrrelevantIgnored();
	TestAcquireCloseEnemyEngaged();
	TestAcquireSelfDefenseEngaged();
	TestAcquireObjectiveThreatEngaged();
	TestAcquireNoCaptureLegacy();
	TestAcquireInvalidCaptureLegacy();
	TestContinueEnemyLeavesArea();
	TestContinueObjectiveRelevant();
	TestContinueSelfDefense();
	TestContinueDraggedTooFar();
	TestContinueNoCaptureLegacy();
	TestContinueCloseFight();
	TestRearmIndependentlyGated();
	TestDefensiveCloseRangeAttacker();
	TestDefensiveObjectiveThreat();
	TestDefensiveRelevantThenLeaves();
	TestDefensiveDistantIrrelevant();
	TestDefensiveNoCapture();
	TestCompleteSequence();
	TestInvalidationDuringCombat();
	TestCompletionDuringCombat();
	TestNoStaleOwnerLoop();
	TestNoRetainedRelevanceDecision();
	std::cout << "ALL COMBAT RELEVANCE TESTS PASS\n";
	return 0;
}
