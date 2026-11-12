#include "bot_decision_core.h"

namespace RCBotDecision
{
DecisionResult ChooseBestCandidate(const std::vector<Candidate>& candidates)
{
	const Candidate* best = nullptr;
	for (const Candidate& candidate : candidates)
	{
		if (!candidate.valid)
			continue;

		if (best == nullptr || candidate.score < best->score ||
			(candidate.score == best->score && candidate.id < best->id))
		{
			best = &candidate;
		}
	}

	if (best == nullptr)
		return { DecisionType::NO_DECISION, {}, 0, "NO_VALID_CANDIDATE", false };

	return {
		DecisionType::SELECT_CANDIDATE,
		best->id,
		best->score,
		"LOWEST_VALID_SCORE",
		true
	};
}

const char* DecisionTypeName(const DecisionType type)
{
	switch (type)
	{
	case DecisionType::NO_DECISION: return "NO_DECISION";
	case DecisionType::SELECT_CANDIDATE: return "SELECT_CANDIDATE";
	}
	return "UNKNOWN";
}
}
