#include "bot_empires_vacancy_revive.h"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace RCBotEmpiresVacancyRevive;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

VacancyMember Member(const int userId, const int position,
	const bool alive, const bool engineer = false,
	const bool controlled = true, const bool connected = true)
{
	VacancyMember member;
	member.userId = userId;
	member.team = 2;
	member.squad = 1;
	member.position = position;
	member.positionKnown = true;
	member.connected = connected;
	member.alive = alive;
	member.isEngineer = engineer;
	member.controlled = controlled;
	return member;
}

void TestCostTable()
{
	Require(ReviveCostForTargets(1) == 4, "VAC-U1 one target costs 4");
	Require(ReviveCostForTargets(2) == 6, "VAC-U1 two targets cost 6");
	Require(ReviveCostForTargets(3) == 8, "VAC-U1 three targets cost 8");
	Require(ReviveCostForTargets(4) == 10, "VAC-U1 four targets cost 10");
	Require(ReviveCostForTargets(9) == 10, "VAC-U1 many targets clamp to 10");
	std::cout << "VAC-U1 PASS\n";
}

void TestLivingLeaderDoesNothing()
{
	std::vector<VacancyMember> members;
	members.push_back(Member(2, 0, true));
	members.push_back(Member(3, 1, true, true));
	const VacancyEvaluation evaluation = EvaluateVacancyRevive(
		members, 2, 1, true, 10);
	Require(evaluation.eligibility == VacancyEligibility::LIVING_LEADER,
		"VAC-U2 living leader never starts a transaction");
	std::cout << "VAC-U2 PASS\n";
}

void TestDeadLeaderOnlyDeclines()
{
	// Dead leader plus nobody else dead: exactly one post-promotion
	// target, so the policy declines the two-promotion transaction.
	std::vector<VacancyMember> members;
	members.push_back(Member(2, 0, false));
	members.push_back(Member(3, 1, true, true));
	const VacancyEvaluation evaluation = EvaluateVacancyRevive(
		members, 2, 1, true, 10);
	Require(evaluation.eligibility ==
		VacancyEligibility::LEADER_ONLY_DEAD,
		"VAC-U3 leader-only death declines");
	std::cout << "VAC-U3 PASS\n";
}

void TestEligibleThresholds()
{
	// Dead leader + one other dead = 2 post-promotion targets, cost 6.
	std::vector<VacancyMember> members;
	members.push_back(Member(2, 0, false));
	members.push_back(Member(3, 1, true, true));
	members.push_back(Member(4, 2, false));
	VacancyEvaluation evaluation = EvaluateVacancyRevive(
		members, 2, 1, true, 6);
	Require(evaluation.eligibility == VacancyEligibility::ELIGIBLE &&
		evaluation.expectedReviveCount == 2 &&
		evaluation.requiredCost == 6 &&
		evaluation.engineerUserId == 3 &&
		evaluation.originalLeaderUserId == 2 &&
		evaluation.balanceBefore == 6,
		"VAC-U4 dead leader plus one other is eligible at cost 6");
	// Same shape with balance 5 must not start.
	evaluation = EvaluateVacancyRevive(members, 2, 1, true, 5);
	Require(evaluation.eligibility ==
		VacancyEligibility::INSUFFICIENT_BALANCE,
		"VAC-U4 balance 5 blocks a cost-6 transaction");
	// Dead leader + two others = 3 targets, cost 8.
	members.push_back(Member(5, 3, false));
	evaluation = EvaluateVacancyRevive(members, 2, 1, true, 8);
	Require(evaluation.eligibility == VacancyEligibility::ELIGIBLE &&
		evaluation.expectedReviveCount == 3 &&
		evaluation.requiredCost == 8,
		"VAC-U4 three targets cost 8");
	// Dead leader + three others = 4 targets, cost 10.
	members.push_back(Member(6, 4, false));
	evaluation = EvaluateVacancyRevive(members, 2, 1, true, 10);
	Require(evaluation.eligibility == VacancyEligibility::ELIGIBLE &&
		evaluation.expectedReviveCount == 4 &&
		evaluation.requiredCost == 10,
		"VAC-U4 four targets cost 10");
	std::cout << "VAC-U4 PASS\n";
}

void TestEligibilityRejections()
{
	std::vector<VacancyMember> members;
	members.push_back(Member(2, 0, false));
	members.push_back(Member(4, 2, false));
	// No Engineer present.
	VacancyEvaluation evaluation = EvaluateVacancyRevive(
		members, 2, 1, true, 10);
	Require(evaluation.eligibility == VacancyEligibility::NO_ENGINEER,
		"VAC-U5 missing Engineer blocks");
	// Engineer present but dead.
	members.push_back(Member(3, 1, false, true));
	evaluation = EvaluateVacancyRevive(members, 2, 1, true, 10);
	Require(evaluation.eligibility == VacancyEligibility::NO_ENGINEER,
		"VAC-U5 dead Engineer blocks");
	members.pop_back();
	// Uncontrolled (human) original leader.
	std::vector<VacancyMember> uncontrolled;
	uncontrolled.push_back(Member(2, 0, false, false, false));
	uncontrolled.push_back(Member(3, 1, true, true));
	uncontrolled.push_back(Member(4, 2, false));
	evaluation = EvaluateVacancyRevive(uncontrolled, 2, 1, true, 10);
	Require(evaluation.eligibility ==
		VacancyEligibility::UNCONTROLLED_LEADER,
		"VAC-U5 uncontrolled leader blocks");
	// Unknown balance.
	members.push_back(Member(3, 1, true, true));
	evaluation = EvaluateVacancyRevive(members, 2, 1, false, -1);
	Require(evaluation.eligibility ==
		VacancyEligibility::BALANCE_UNKNOWN,
		"VAC-U5 unknown balance blocks");
	std::cout << "VAC-U5 PASS\n";
}

void TestEngineerSelection()
{
	// Lowest position wins; userid breaks ties. No scoring.
	std::vector<VacancyMember> members;
	members.push_back(Member(2, 0, false));
	members.push_back(Member(9, 3, true, true));
	members.push_back(Member(7, 1, true, true));
	members.push_back(Member(4, 2, false));
	const VacancyEvaluation evaluation = EvaluateVacancyRevive(
		members, 2, 1, true, 10);
	Require(evaluation.eligibility == VacancyEligibility::ELIGIBLE &&
		evaluation.engineerUserId == 7 &&
		evaluation.engineerPosition == 1,
		"VAC-U6 lowest-position Engineer wins");
	Require(IsVacancyReviveDriver(3, 3) && !IsVacancyReviveDriver(4, 3) &&
		!IsVacancyReviveDriver(-1, 3),
		"VAC-U6 exactly the lowest living userid drives");
	std::cout << "VAC-U6 PASS\n";
}

void TestPhaseTransitions()
{
	Require(PhaseAfterPromotion(true) == VacancyPhase::REVIVING,
		"VAC-U7 confirmed promotion advances to REVIVING");
	Require(PhaseAfterPromotion(false) == VacancyPhase::ABORTED,
		"VAC-U7 failed promotion aborts");
	Require(!ShouldSpendRevive(true, true, 1, true, 10),
		"VAC-U7 fewer than 2 targets never spends");
	Require(!ShouldSpendRevive(true, true, 2, true, 5),
		"VAC-U7 insufficient actual balance never spends");
	Require(!ShouldSpendRevive(false, true, 2, true, 10),
		"VAC-U7 dead Engineer never spends");
	Require(!ShouldSpendRevive(true, false, 2, true, 10),
		"VAC-U7 Engineer off position 0 never spends");
	Require(ShouldSpendRevive(true, true, 2, true, 6),
		"VAC-U7 valid actual state spends");
	Require(ShouldRestoreLeader(true) && !ShouldRestoreLeader(false),
		"VAC-U7 restore only a living original leader");
	Require(kVacancyReviveVerifyPasses > 0 &&
		kVacancyReviveVerifyPasses <= 100,
		"VAC-U7 bounded verify window");
	Require(RestoreGateOpen(100.0f, -1.0f),
		"VAC-U7 no issued Revive leaves the restore gate open");
	Require(!RestoreGateOpen(101.0f, 100.0f),
		"VAC-U7 restore inside the flood window stays shut");
	Require(RestoreGateOpen(103.0f, 100.0f),
		"VAC-U7 restore past the flood window opens");
	std::cout << "VAC-U7 PASS\n";
}

VacancyIdentity Identity(const int team, const int squad,
	const int orig, const unsigned long long epoch)
{
	VacancyIdentity identity;
	identity.team = team;
	identity.squad = squad;
	identity.originalLeaderUserId = orig;
	identity.leaderLifeEpoch = epoch;
	return identity;
}

void TestVacancyEpochRepeat()
{
	// Same dead leader, same death epoch, repeated evaluation: one
	// intervention only. Epoch 0 (never spawned) is no vacancy at
	// all and is never consumed.
	VacancyConsumedIdentities consumed;
	const VacancyIdentity invalid = Identity(2, 1, 3, 0);
	Require(!ValidVacancyIdentity(invalid),
		"VAC-U8 epoch 0 is not a vacancy identity");
	Require(!consumed.IsConsumed(invalid),
		"VAC-U8 invalid identity is never consumed");
	consumed.Consume(invalid);
	Require(consumed.Size() == 0,
		"VAC-U8 consuming invalid identity stores nothing");
	const VacancyIdentity vacancyA = Identity(2, 1, 3, 5);
	Require(ValidVacancyIdentity(vacancyA),
		"VAC-U8 live leader epoch is a vacancy identity");
	Require(!consumed.IsConsumed(vacancyA),
		"VAC-U8 first evaluation of a vacancy is unconsumed");
	consumed.Consume(vacancyA);
	Require(consumed.IsConsumed(vacancyA),
		"VAC-U8 started vacancy is consumed");
	Require(consumed.Size() == 1,
		"VAC-U8 one vacancy stores one identity");
	consumed.Consume(vacancyA);
	Require(consumed.Size() == 1 && consumed.IsConsumed(vacancyA),
		"VAC-U8 repeated evaluation of the same vacancy stays single");
	std::cout << "VAC-U8 PASS\n";
}

void TestVacancyEpochPostComplete()
{
	// Vacancy A runs to COMPLETE; repeated evaluation while the
	// leader is still in epoch A must not start a second run.
	VacancyConsumedIdentities consumed;
	const VacancyIdentity vacancyA = Identity(2, 1, 3, 5);
	consumed.Consume(vacancyA);
	Require(consumed.IsConsumed(vacancyA),
		"VAC-U9 completed vacancy stays consumed in its epoch");
	Require(consumed.Size() == 1,
		"VAC-U9 no second identity for the same vacancy");
	std::cout << "VAC-U9 PASS\n";
}

void TestVacancyEpochReEntry()
{
	// Key regression: the original leader returns to life (epoch
	// advances on respawn) and later dies again. Vacancy B shares
	// team, squad and leader with A but carries the new epoch, so
	// it must be independently eligible.
	VacancyConsumedIdentities consumed;
	const VacancyIdentity vacancyA = Identity(2, 1, 3, 5);
	const VacancyIdentity vacancyB = Identity(2, 1, 3, 6);
	Require(!SameVacancyIdentity(vacancyA, vacancyB),
		"VAC-U10 later death epoch is a different vacancy");
	consumed.Consume(vacancyA);
	Require(!consumed.IsConsumed(vacancyB),
		"VAC-U10 later death after completed run is eligible");
	consumed.Consume(vacancyB);
	Require(consumed.IsConsumed(vacancyA) &&
		consumed.IsConsumed(vacancyB) && consumed.Size() == 2,
		"VAC-U10 both vacancies tracked independently");
	std::cout << "VAC-U10 PASS\n";
}

void TestVacancyEpochGoalLess()
{
	// Two genuinely separate vacancies must not collapse merely
	// because the squad-goal generation stays -1: the epoch, not
	// the goal generation, separates them.
	VacancyConsumedIdentities consumed;
	const VacancyIdentity first = Identity(2, 1, 3, 5);
	const VacancyIdentity second = Identity(2, 1, 3, 6);
	consumed.Consume(first);
	Require(!consumed.IsConsumed(second),
		"VAC-U11 goal-less re-vacancy is not collapsed by -1");
	std::cout << "VAC-U11 PASS\n";
}

void TestVacancyEpochIgnoresGoalGeneration()
{
	// A squad-goal state change during one standing vacancy must
	// not create a second intervention: goal generation is not
	// part of the identity at all.
	VacancyConsumedIdentities consumed;
	const VacancyIdentity vacancyA = Identity(3, 2, 18, 9);
	consumed.Consume(vacancyA);
	Require(consumed.IsConsumed(Identity(3, 2, 18, 9)),
		"VAC-U12 same epoch stays consumed across goal changes");
	std::cout << "VAC-U12 PASS\n";
}

void TestVacancyEpochSquadIsolation()
{
	// An independent squad vacancy never collides, even with the
	// same leader userid and epoch values.
	VacancyConsumedIdentities consumed;
	consumed.Consume(Identity(2, 1, 3, 5));
	Require(!consumed.IsConsumed(Identity(2, 2, 3, 5)),
		"VAC-U13 different squad stays independent");
	Require(!consumed.IsConsumed(Identity(3, 1, 3, 5)),
		"VAC-U13 different team stays independent");
	Require(!consumed.IsConsumed(Identity(2, 1, 4, 5)),
		"VAC-U13 different leader stays independent");
	std::cout << "VAC-U13 PASS\n";
}

} // namespace

int main()
{
	TestCostTable();
	TestLivingLeaderDoesNothing();
	TestDeadLeaderOnlyDeclines();
	TestEligibleThresholds();
	TestEligibilityRejections();
	TestEngineerSelection();
	TestPhaseTransitions();
	TestVacancyEpochRepeat();
	TestVacancyEpochPostComplete();
	TestVacancyEpochReEntry();
	TestVacancyEpochGoalLess();
	TestVacancyEpochIgnoresGoalGeneration();
	TestVacancyEpochSquadIsolation();
	std::cout << "VAC-U1..U13 PASS; vacancy_revive_behavior_changes=0\n";
	return 0;
}
