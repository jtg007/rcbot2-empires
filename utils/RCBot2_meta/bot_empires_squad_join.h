#ifndef __RCBOT_EMPIRES_SQUAD_JOIN_H__
#define __RCBOT_EMPIRES_SQUAD_JOIN_H__

#include <algorithm>
#include <vector>

namespace RCBotEmpiresSquadJoin
{
	// Empires release notes specify a five-player limit per squad. Membership
	// observations come from m_iSquad; no maximum squad ID is exposed by the
	// current game/SDK interface.
	static const int kEmpiresSquadCapacity = 5;
	static const int kMaximumJoinAttempts = 3;

	struct SquadObservation
	{
		int id = 0;
		int members = 0;
		bool humanOwned = false;
	};

	enum class Status
	{
		JOIN,
		ALREADY_ASSIGNED,
		WAIT_FOR_PEER,
		NO_CANDIDATE
	};

	struct Input
	{
		int ownSquad = 0;
		int attempts = 0;
		int unassignedBots = 0;
		int lowestUnassignedUserId = 0;
		int currentUserId = 0;
		std::vector<SquadObservation> squads;
	};

	struct Result
	{
		Status status = Status::NO_CANDIDATE;
		int squadId = 0;
	};

	inline Result Select(const Input& input)
	{
		if (input.ownSquad > 0)
			return { Status::ALREADY_ASSIGNED, 0 };
		if (input.attempts >= kMaximumJoinAttempts)
			return { Status::NO_CANDIDATE, 0 };

		std::vector<SquadObservation> squads = input.squads;
		std::sort(squads.begin(), squads.end(),
			[](const SquadObservation& a, const SquadObservation& b)
			{
				return a.id < b.id;
			});

		int highestSquadId = 0;
		for (const SquadObservation& squad : squads)
		{
			if (squad.id <= 0)
				continue;
			highestSquadId = std::max(highestSquadId, squad.id);
			if (!squad.humanOwned && squad.members < kEmpiresSquadCapacity)
				return { Status::JOIN, squad.id };
		}

		if (highestSquadId == 0)
		{
			if (input.unassignedBots < 2 || input.currentUserId <= 0 ||
				input.currentUserId != input.lowestUnassignedUserId)
			{
				return { Status::WAIT_FOR_PEER, 0 };
			}
			return { Status::JOIN, 1 };
		}

		// Empires only permits the next squad after the previous one exists.
		// The next sequential ID is derived from live team membership, without
		// imposing an undocumented maximum ID.
		return { Status::JOIN, highestSquadId + 1 };
	}
}

#endif
