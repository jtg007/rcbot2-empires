#include "bot_squad_spatial_movement.h"

#include <algorithm>
#include <cmath>

namespace RCBotSquadMovement
{
namespace
{
	bool Contains(const std::vector<RecipientId>& values,
		const RecipientId value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	const RCBotSquadDecision::ParticipationDecision* FindParticipation(
		const std::vector<RCBotSquadDecision::ParticipationDecision>& values,
		const RecipientId memberId)
	{
		const auto it = std::find_if(values.begin(), values.end(),
			[memberId](const auto& value) { return value.memberId == memberId; });
		return it == values.end() ? nullptr : &*it;
	}

	const RCBotSquadSpatial::FormationAssignment* FindAssignment(
		const FormationDecisionResult& formation, const RecipientId memberId,
		std::size_t& count)
	{
		const RCBotSquadSpatial::FormationAssignment* found = nullptr;
		count = 0;
		for (const auto& assignment : formation.assignments)
		{
			if (assignment.memberId == memberId)
			{
				found = &assignment;
				++count;
			}
		}
		return found;
	}

	const RCBotSquadClaims::ClaimRecord* FindClaim(
		const RCBotSquadClaims::ClaimSnapshot& snapshot,
		const ClaimKey& key)
	{
		const auto it = std::find_if(snapshot.claims.begin(), snapshot.claims.end(),
			[&key](const auto& claim) { return claim.key == key; });
		return it == snapshot.claims.end() ? nullptr : &*it;
	}
}

Result Resolve(const Input& input)
{
	Result result;
	if (!input.identity.IsValid() || input.decisionRevision == 0 ||
		input.formationGeneration == 0 || input.parentOrder.orderId == 0 ||
		input.parentOrder.generation == 0 || !std::isfinite(input.now))
	{
		result.diagnostic = Diagnostic::INVALID_CONTEXT;
		return result;
	}
	if (!input.formation.Accepted() ||
		input.formation.formationGeneration != input.formationGeneration ||
		!input.formation.claimSnapshot.IsValid() ||
		input.formation.claimSnapshot.scope.squadGeneration !=
			input.identity.squadGeneration ||
		input.formation.claimSnapshot.scope.decisionRevision !=
			input.decisionRevision ||
		!input.formation.claimSnapshot.scope.hasParentOrder ||
		input.formation.claimSnapshot.scope.parentOrder.orderId !=
			input.parentOrder.orderId ||
		input.formation.claimSnapshot.scope.parentOrder.generation !=
			input.parentOrder.generation)
	{
		result.diagnostic = Diagnostic::INVALID_FORMATION;
		return result;
	}
	if (input.memberSnapshot.empty())
	{
		result.diagnostic = Diagnostic::INVALID_MEMBERS;
		return result;
	}

	std::vector<RecipientId> members = input.memberSnapshot;
	std::sort(members.begin(), members.end());
	if (members.front() == 0 ||
		std::adjacent_find(members.begin(), members.end()) != members.end())
	{
		result.diagnostic = Diagnostic::INVALID_MEMBERS;
		return result;
	}
	if (input.participation.size() != members.size())
	{
		result.diagnostic = Diagnostic::INVALID_MEMBERS;
		return result;
	}

	for (const RecipientId memberId : members)
	{
		const auto* participation = FindParticipation(input.participation,
			memberId);
		if (participation == nullptr)
		{
			result.members.clear();
			result.diagnostic = Diagnostic::INVALID_MEMBERS;
			return result;
		}
		std::size_t assignmentCount = 0;
		const auto* assignment = FindAssignment(input.formation, memberId,
			assignmentCount);
		if (assignmentCount > 1)
		{
			result.members.clear();
			result.diagnostic = Diagnostic::DUPLICATE_ASSIGNMENT;
			return result;
		}
		if (participation->state == ParticipationState::EXCLUDED)
		{
			if (assignmentCount != 0)
			{
				result.members.clear();
				result.diagnostic = Diagnostic::INVALID_FORMATION;
				return result;
			}
			MemberTarget excluded;
			excluded.memberId = memberId;
			excluded.participation = participation->state;
			result.members.push_back(excluded);
			continue;
		}
		if (assignment == nullptr || assignment->claimStatus !=
			RCBotSquadSpatial::SpatialClaimStatus::CLAIMED ||
			!assignment->targetPosition.IsFinite())
		{
			result.members.clear();
			result.diagnostic = Diagnostic::MISSING_ASSIGNMENT;
			return result;
		}
		const auto* claim = FindClaim(input.formation.claimSnapshot,
			assignment->claimKey);
		if (claim == nullptr || !claim->IsLeaseActive(input.now) ||
			claim->ownerMemberId != memberId ||
			claim->squadGeneration != input.identity.squadGeneration ||
			claim->decisionRevision != input.decisionRevision ||
			claim->key.resourceGeneration != input.formationGeneration ||
			claim->key.resourceKind !=
				RCBotSquadClaims::ResourceKind::SPATIAL_RESOURCE ||
			!claim->hasParentOrder || claim->parentOrder.orderId !=
				input.parentOrder.orderId || claim->parentOrder.generation !=
				input.parentOrder.generation)
		{
			result.members.clear();
			result.diagnostic = Diagnostic::STALE_CLAIM;
			return result;
		}
		MemberTarget target;
		target.memberId = memberId;
		target.hasTarget = true;
		target.target = assignment->targetPosition;
		target.slotId = assignment->slotId;
		target.participation = participation->state;
		target.claimKey = assignment->claimKey;
		result.members.push_back(target);
	}

	for (const auto& assignment : input.formation.assignments)
	{
		if (!Contains(members, assignment.memberId))
		{
			result.members.clear();
			result.diagnostic = Diagnostic::INVALID_FORMATION;
			return result;
		}
	}
	result.valid = true;
	result.diagnostic = Diagnostic::NONE;
	return result;
}

bool ShouldSuppressLegacyMovement(const bool modernParentOwnsMember,
	const bool memberIsInParentSnapshot)
{
	return modernParentOwnsMember && memberIsInParentSnapshot;
}
}
