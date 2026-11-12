#ifndef __RCBOT_COMMANDER_MOVE_TEST_FIXTURE_H__
#define __RCBOT_COMMANDER_MOVE_TEST_FIXTURE_H__

#include "bot_squad_order_recipient.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace RCBotCommanderMoveTestFixture
{
	enum class Mode
	{
		BOT,
		SQUAD
	};

	struct Request
	{
		Mode mode = Mode::BOT;
		RCBotOrder::RecipientId botUserId = 0;
		int team = 0;
		RCBotOrder::RecipientId squadId = 0;
		RCBotOrder::TargetPosition target;
	};

	inline bool ParseInteger(const std::string& text, long minimum,
		long maximum, long& value)
	{
		if (text.empty())
			return false;
		char* end = nullptr;
		const long parsed = std::strtol(text.c_str(), &end, 10);
		if (end == text.c_str() || *end != '\0' || parsed < minimum ||
			parsed > maximum)
			return false;
		value = parsed;
		return true;
	}

	inline bool ParseCoordinate(const std::string& text, float& value)
	{
		if (text.empty())
			return false;
		char* end = nullptr;
		const float parsed = std::strtof(text.c_str(), &end);
		if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed) ||
			parsed < -32768.0f || parsed > 32768.0f)
			return false;
		value = parsed;
		return true;
	}

	inline bool ParseTarget(const std::vector<std::string>& arguments,
		const std::size_t start, Request& request)
	{
		return ParseCoordinate(arguments[start], request.target.x) &&
			ParseCoordinate(arguments[start + 1], request.target.y) &&
			ParseCoordinate(arguments[start + 2], request.target.z) &&
			request.target.IsFinite();
	}

	// Supports the legacy <bot_userid> form and the explicit `bot` form,
	// plus `squad <team> <squad_id> <x> <y> <z>`.
	inline bool ParseArguments(const std::vector<std::string>& arguments,
		Request& request)
	{
		Request parsed;
		long first = 0;
		if (arguments.size() == 5 && arguments[0] == "bot")
		{
			if (!ParseInteger(arguments[1], 1, 2147483647, first) ||
				!ParseTarget(arguments, 2, parsed))
				return false;
			parsed.mode = Mode::BOT;
			parsed.botUserId = static_cast<RCBotOrder::RecipientId>(first);
		}
		else if (arguments.size() == 4 && arguments[0] != "squad")
		{
			if (!ParseInteger(arguments[0], 1, 2147483647, first) ||
				!ParseTarget(arguments, 1, parsed))
				return false;
			parsed.mode = Mode::BOT;
			parsed.botUserId = static_cast<RCBotOrder::RecipientId>(first);
		}
		else if (arguments.size() == 6 && arguments[0] == "squad")
		{
			long team = 0;
			long squad = 0;
			if (!ParseInteger(arguments[1], 2, 3, team) ||
				!ParseInteger(arguments[2], 1, 64, squad) ||
				!ParseTarget(arguments, 3, parsed))
				return false;
			parsed.mode = Mode::SQUAD;
			parsed.team = static_cast<int>(team);
			parsed.squadId = static_cast<RCBotOrder::RecipientId>(squad);
		}
		else
		{
			return false;
		}

		request = parsed;
		return true;
	}

	inline RCBotOrder::RecipientId TeamScopedSquadId(const int team,
		const RCBotOrder::RecipientId squadId)
	{
		return (static_cast<RCBotOrder::RecipientId>(team) << 32) | squadId;
	}

	inline RCBotOrder::OrderRecipient MakeRecipient(const Request& request,
		const RCBotOrder::Generation squadGeneration = 1)
	{
		if (request.mode == Mode::BOT)
			return RCBotOrder::OrderRecipient::Bot(request.botUserId);
		return RCBotOrder::OrderRecipient::Squad(
			TeamScopedSquadId(request.team, request.squadId), squadGeneration);
	}

	inline bool IssueSquadMove(const Request& request,
		const std::vector<RCBotOrder::RecipientId>& memberIds,
		RCBotSquadOrder::SquadIdentityTracker& identityTracker,
		RCBotSquadOrder::SquadMoveRecipient& recipient,
		const RCBotOrder::OrderId orderId,
		const RCBotOrder::Generation orderGeneration,
		const RCBotOrder::IssuerId issuerId, const double now,
		const double deadline,
		RCBotSquadOrder::SquadMembershipObservation* membershipOutput = nullptr)
	{
		if (request.mode != Mode::SQUAD || request.team < 2 || request.team > 3 ||
			request.squadId == 0 || request.squadId > 64 || memberIds.empty() ||
			orderId == 0 || orderGeneration == 0 || issuerId == 0 ||
			!request.target.IsFinite() || !std::isfinite(now) ||
			!std::isfinite(deadline) || deadline <= now)
			return false;

		std::vector<RCBotOrder::RecipientId> members = memberIds;
		if (std::any_of(members.begin(), members.end(),
			[](const auto memberId) { return memberId == 0; }))
			return false;
		std::sort(members.begin(), members.end());
		if (std::adjacent_find(members.begin(), members.end()) != members.end())
			return false;

		const RCBotSquadOrder::SquadMembershipObservation membership =
			identityTracker.Observe(TeamScopedSquadId(request.team,
				request.squadId), members);
		RCBotSquadOrder::SquadMoveSpec spec;
		spec.orderId = orderId;
		spec.generation = orderGeneration;
		spec.issuerId = issuerId;
		spec.snapshot = { membership.recipient, membership.members };
		spec.target = request.target;
		spec.issuedAt = now;
		spec.deadline = deadline;
		// The Empires runtime must observe and validate the canonical spatial
		// assignments after the Parent exists and before any Child navigation
		// request is created.
		if (!spec.snapshot.IsValid() || !recipient.BeginMove(spec))
			return false;
		if (membershipOutput != nullptr)
			*membershipOutput = membership;
		return true;
	}
}

#endif // __RCBOT_COMMANDER_MOVE_TEST_FIXTURE_H__
