#ifndef __RCBOT_SQUAD_SPATIAL_MOVEMENT_H__
#define __RCBOT_SQUAD_SPATIAL_MOVEMENT_H__

#include "bot_squad_spatial.h"

#include <vector>

namespace RCBotSquadMovement
{
	using RCBotOrder::OrderKey;
	using RCBotOrder::RecipientId;
	using RCBotOrder::TargetPosition;
	using RCBotSquadClaims::ClaimKey;
	using RCBotSquadClaims::Generation;
	using RCBotSquadDecision::DecisionRevision;
	using RCBotSquadDecision::ParticipationState;
	using RCBotSquadDecision::SquadIdentity;
	using RCBotSquadSpatial::FormationDecisionResult;

	struct Input
	{
		SquadIdentity identity;
		DecisionRevision decisionRevision = 0;
		Generation formationGeneration = 0;
		OrderKey parentOrder;
		std::vector<RecipientId> memberSnapshot;
		std::vector<RCBotSquadDecision::ParticipationDecision> participation;
		FormationDecisionResult formation;
		double now = 0.0;
	};

	struct MemberTarget
	{
		RecipientId memberId = 0;
		bool hasTarget = false;
		TargetPosition target;
		std::uint64_t slotId = 0;
		ParticipationState participation = ParticipationState::EXCLUDED;
		ClaimKey claimKey;
	};

	enum class Diagnostic
	{
		NONE = 0,
		INVALID_CONTEXT,
		INVALID_FORMATION,
		INVALID_MEMBERS,
		STALE_CLAIM,
		MISSING_ASSIGNMENT,
		DUPLICATE_ASSIGNMENT
	};

	struct Result
	{
		bool valid = false;
		Diagnostic diagnostic = Diagnostic::NONE;
		std::vector<MemberTarget> members;
	};

	// Resolves only the final, already-claimed slot.  It has no engine or
	// navigation dependency and rejects the whole snapshot on any stale claim.
	Result Resolve(const Input& input);

	// Shared by runtime gating and its headless acceptance test.
	bool ShouldSuppressLegacyMovement(bool modernParentOwnsMember,
		bool memberIsInParentSnapshot);
}

#endif // __RCBOT_SQUAD_SPATIAL_MOVEMENT_H__
