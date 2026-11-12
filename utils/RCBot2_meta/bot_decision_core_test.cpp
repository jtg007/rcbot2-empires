#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "bot_decision_core.h"

using namespace RCBotDecision;

static bool SameDecision(const DecisionResult& left, const DecisionResult& right)
{
	return left.type == right.type && left.selectedId == right.selectedId &&
		left.score == right.score && left.reason == right.reason &&
		left.valid == right.valid;
}

static void PrintPass(const char* name)
{
	std::cout << name << " PASS" << std::endl;
}

int main()
{
	// AI-CORE-01: choose the best valid candidate.
	const std::vector<Candidate> candidates = {
		{ "A", 300, true },
		{ "B", 120, true },
		{ "C", 450, true }
	};
	const DecisionResult best = ChooseBestCandidate(candidates);
	assert(best.type == DecisionType::SELECT_CANDIDATE);
	assert(best.selectedId == "B");
	assert(best.score == 120);
	assert(best.valid);
	PrintPass("AI-CORE-01");

	// AI-CORE-02: validity is evaluated before score.
	const std::vector<Candidate> invalidBest = {
		{ "INVALID_CHEAP", 20, false },
		{ "VALID", 100, true },
		{ "VALID_EXPENSIVE", 300, true }
	};
	const DecisionResult valid = ChooseBestCandidate(invalidBest);
	assert(valid.type == DecisionType::SELECT_CANDIDATE);
	assert(valid.selectedId == "VALID");
	assert(valid.score == 100);
	PrintPass("AI-CORE-02");

	// AI-CORE-03: equal scores use the ID tie-break, not input order.
	const std::vector<Candidate> tieForward = {
		{ "ZETA", 50, true },
		{ "ALPHA", 50, true }
	};
	const std::vector<Candidate> tieReverse = {
		{ "ALPHA", 50, true },
		{ "ZETA", 50, true }
	};
	const DecisionResult forward = ChooseBestCandidate(tieForward);
	const DecisionResult reverse = ChooseBestCandidate(tieReverse);
	assert(forward.selectedId == "ALPHA");
	assert(reverse.selectedId == "ALPHA");
	PrintPass("AI-CORE-03");

	// AI-CORE-04: no valid candidate is an explicit terminal no-decision.
	const std::vector<Candidate> none = {
		{ "A", 1, false },
		{ "B", 2, false }
	};
	const DecisionResult noDecision = ChooseBestCandidate(none);
	assert(noDecision.type == DecisionType::NO_DECISION);
	assert(noDecision.selectedId.empty());
	assert(noDecision.reason == "NO_VALID_CANDIDATE");
	assert(!noDecision.valid);
	PrintPass("AI-CORE-04");

	// AI-CORE-05: identical input produces an identical structured result.
	const std::vector<Candidate> repeatedInput = {
		{ "A", 10, true },
		{ "B", 20, true },
		{ "C", 5, false }
	};
	const DecisionResult first = ChooseBestCandidate(repeatedInput);
	const DecisionResult second = ChooseBestCandidate(repeatedInput);
	assert(SameDecision(first, second));
	assert(first.selectedId == "A");
	PrintPass("AI-CORE-05");

	std::cout << "RESULT 5/5 PASS" << std::endl;
	return 0;
}
