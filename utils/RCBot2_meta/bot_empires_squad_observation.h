#ifndef __RCBOT_EMPIRES_SQUAD_OBSERVATION_H__
#define __RCBOT_EMPIRES_SQUAD_OBSERVATION_H__

#include <cstdint>

namespace RCBotEmpiresSquadObservation
{
	// Resolution status for a single observed field. A missing SendProp is a
	// result, never a reason to guess an offset or substitute another field.
	enum class SquadFieldStatus : std::uint8_t
	{
		RESOLVED = 0,
		NOT_FOUND,
		AMBIGUOUS,
		UNSUPPORTED
	};

	inline const char* SquadFieldStatusName(const SquadFieldStatus status)
	{
		switch (status)
		{
		case SquadFieldStatus::RESOLVED: return "RESOLVED";
		case SquadFieldStatus::NOT_FOUND: return "NOT_FOUND";
		case SquadFieldStatus::AMBIGUOUS: return "AMBIGUOUS";
		case SquadFieldStatus::UNSUPPORTED: return "UNSUPPORTED";
		}
		return "UNSUPPORTED";
	}

	// Raw public membership fields for one player, with per-field knowledge.
	// Values are only meaningful when the matching known flag is set.
	struct SquadFieldObservation
	{
		int squad = -1;
		bool squadKnown = false;
		int position = -1;
		bool positionKnown = false;
		bool explicitLeader = false;
		bool explicitLeaderKnown = false;
	};

	struct LeaderInterpretation
	{
		bool isLeader = false;
		// True when both sources were known and agreed.
		bool sourcesAgree = false;
		// True when position==0 agreed with the explicit marker (or only one
		// source was known, so no contradiction was possible).
		bool encodingMatch = false;
		bool ambiguous = false;
	};

	inline LeaderInterpretation InterpretSquadLeader(
		const SquadFieldObservation& observation)
	{
		LeaderInterpretation result;
		const bool positionSaysLeader = observation.positionKnown &&
			observation.position == 0;
		if (observation.positionKnown && observation.explicitLeaderKnown)
		{
			result.sourcesAgree =
				positionSaysLeader == observation.explicitLeader;
			result.encodingMatch = result.sourcesAgree;
			result.ambiguous = !result.sourcesAgree;
			result.isLeader = observation.explicitLeader;
		}
		else if (observation.explicitLeaderKnown)
		{
			result.isLeader = observation.explicitLeader;
			result.encodingMatch = true;
		}
		else if (observation.positionKnown)
		{
			result.isLeader = positionSaysLeader;
			result.encodingMatch = true;
		}
		else
		{
			result.ambiguous = true;
		}
		return result;
	}

	// Mirrors EMP_CLASS_ENGINEER (bot_empires.h) without depending on the
	// engine-bound bot headers, so this unit stays headless.
	static const int kEmpiresEngineerClassId = 3;

	inline bool IsAliveEngineer(const bool classKnown, const int classId,
		const bool alive)
	{
		return alive && classKnown && classId == kEmpiresEngineerClassId;
	}

	// Squad ids repeat across teams; production keys them team-scoped.
	inline std::int64_t TeamScopedSquadKey(const int team, const int squad)
	{
		return (static_cast<std::int64_t>(team) << 32) |
			static_cast<std::uint32_t>(squad);
	}

	enum class SquadXpSharing : std::uint8_t
	{
		UNKNOWN = 0,
		SAME_VALUE,
		DIFFERENT_VALUES
	};

	inline const char* SquadXpSharingName(const SquadXpSharing sharing)
	{
		switch (sharing)
		{
		case SquadXpSharing::UNKNOWN: return "UNKNOWN";
		case SquadXpSharing::SAME_VALUE: return "SAME_VALUE";
		case SquadXpSharing::DIFFERENT_VALUES: return "DIFFERENT_VALUES";
		}
		return "UNKNOWN";
	}

	// Classifies whether two members report the same squad-XP value. This
	// answers only the mirroring question; it never infers what XP means.
	inline SquadXpSharing ClassifySquadXpSharing(const bool firstKnown,
		const int firstXp, const bool secondKnown, const int secondXp)
	{
		if (!firstKnown || !secondKnown)
			return SquadXpSharing::UNKNOWN;
		return firstXp == secondXp ? SquadXpSharing::SAME_VALUE :
			SquadXpSharing::DIFFERENT_VALUES;
	}
}

#endif // __RCBOT_EMPIRES_SQUAD_OBSERVATION_H__
