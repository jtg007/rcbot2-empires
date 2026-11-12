#ifndef __RCBOT_DECISION_CORE_H__
#define __RCBOT_DECISION_CORE_H__

#include <cstdint>
#include <string>
#include <vector>

namespace RCBotDecision
{
	enum class DecisionType : std::uint8_t
	{
		NO_DECISION = 0,
		SELECT_CANDIDATE
	};

	// This is deliberately a neutral value object.  An engine adapter may
	// populate it from bot, squad or objective state without exposing any
	// engine handle to the decision core.
	struct Candidate
	{
		std::string id;
		int score = 0;
		bool valid = false;
	};

	struct DecisionResult
	{
		DecisionType type = DecisionType::NO_DECISION;
		std::string selectedId;
		int score = 0;
		std::string reason;
		bool valid = false;
	};

	// Select the valid candidate with the lowest score.  Equal scores are
	// resolved by candidate ID, making the result independent of input order,
	// container traversal details or pointer addresses.
	DecisionResult ChooseBestCandidate(const std::vector<Candidate>& candidates);

	const char* DecisionTypeName(DecisionType type);
}

#endif // __RCBOT_DECISION_CORE_H__
