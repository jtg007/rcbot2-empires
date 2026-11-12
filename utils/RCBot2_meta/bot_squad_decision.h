#ifndef __RCBOT_SQUAD_DECISION_H__
#define __RCBOT_SQUAD_DECISION_H__

#include "bot_order_protocol.h"

#include <cstdint>
#include <vector>

namespace RCBotSquadDecision
{
	using RCBotOrder::Generation;
	using RCBotOrder::OrderId;
	using RCBotOrder::OrderKey;
	using RCBotOrder::OrderState;
	using RCBotOrder::RecipientId;
	using RCBotOrder::TargetPosition;

	using TeamId = std::int32_t;
	using DecisionRevision = std::uint64_t;
	using Fingerprint = std::uint64_t;

	struct SquadIdentity
	{
		TeamId team = 0;
		RecipientId squadId = 0;
		Generation squadGeneration = 0;

		bool IsValid() const;
		bool operator==(const SquadIdentity& other) const
		{
			return team == other.team && squadId == other.squadId &&
				squadGeneration == other.squadGeneration;
		}
		bool operator!=(const SquadIdentity& other) const
		{
			return !(*this == other);
		}
	};

	// This is an immutable reference to an already-issued Commander parent
	// order.  The decision core never allocates, replaces, or advances it.
	struct CommanderOrderRef
	{
		bool present = false;
		OrderKey parentOrder;
		TargetPosition target;
		double deadline = 0.0;
		OrderState orderState = OrderState::ISSUED;

		bool IsValid() const;
	};

	enum class MemberLifecycle : std::uint8_t
	{
		ACTIVE = 0,
		JOINED,
		LEFT,
		DEAD,
		DISCONNECTED
	};

	struct MemberState
	{
		RecipientId memberId = 0;
		bool alive = false;
		bool connected = false;
		bool eligible = false;
		bool isLeader = false;
		std::int32_t squadPosition = -1;
		TargetPosition origin;
		TargetPosition velocity;
		MemberLifecycle lifecycle = MemberLifecycle::ACTIVE;

		bool IsValid() const;
		bool IsEligible() const;
	};

	// This is the only geometry threshold used by Phase 11.2.  Distances at
	// or below cohesionRadius are participating; an eligible member farther
	// away is catching up.  Eligibility, not distance, is the only reason for
	// EXCLUDED, so no valid member is silently removed by geometry alone.
	struct SquadParticipationPolicy
	{
		double cohesionRadius = 256.0;

		bool IsValid() const;
	};

	enum class ParticipationState : std::uint8_t
	{
		PARTICIPATING = 0,
		CATCHING_UP,
		EXCLUDED
	};

	struct CohesionMemberFacts
	{
		RecipientId memberId = 0;
		double distanceToCentroid = 0.0;
		double distanceToObjective = 0.0;
	};

	struct CohesionFacts
	{
		bool valid = false;
		bool objectivePresent = false;
		std::uint64_t eligibleMemberCount = 0;
		TargetPosition centroid;
		TargetPosition boundsMin;
		TargetPosition boundsMax;
		double minDistanceFromCentroid = 0.0;
		double maxDistanceFromCentroid = 0.0;
		double averageDistanceFromCentroid = 0.0;
		double distanceSpread = 0.0;
		double objectiveDistanceFromCentroid = 0.0;
		// Entries follow the canonical member order.  When there are no
		// eligible members, the centroid is the neutral zero position and all
		// distances are explicitly zero.
		std::vector<CohesionMemberFacts> memberFacts;
	};

	struct ParticipationDecision
	{
		RecipientId memberId = 0;
		ParticipationState state = ParticipationState::EXCLUDED;
		double distanceToCentroid = 0.0;
		double distanceToObjective = 0.0;
	};

	struct SquadDecisionInput
	{
		SquadIdentity identity;
		// Revisions are supplied by the caller.  The core deliberately owns no
		// process-global counter and never invents a second Squad generation.
		DecisionRevision decisionRevision = 0;
		CommanderOrderRef commanderOrder;
		SquadParticipationPolicy participationPolicy;
		std::vector<MemberState> members;
	};

	struct SquadDecisionRevisionContext
	{
		bool hasPrevious = false;
		SquadIdentity identity;
		DecisionRevision decisionRevision = 0;
		Fingerprint canonicalFingerprint = 0;

		bool IsValid() const;
	};

	enum class DecisionDiagnostic : std::uint8_t
	{
		NONE = 0,
		INVALID_SQUAD_IDENTITY,
		EMPTY_MEMBERSHIP,
		INVALID_PARENT_ORDER,
		INVALID_DECISION_REVISION,
		INVALID_PREVIOUS_CONTEXT,
		INVALID_MEMBER_ID,
		DUPLICATE_MEMBER_ID,
		INVALID_MEMBER_LIFECYCLE,
		NON_FINITE_MEMBER_FACT,
		NON_FINITE_ORDER_FACT,
		MULTIPLE_LEADERS,
		INVALID_PARTICIPATION_POLICY,
		NON_FINITE_COHESION_FACT,
		STALE_SQUAD_IDENTITY,
		STALE_SQUAD_GENERATION,
		STALE_DECISION_REVISION,
		REVISION_WITHOUT_INPUT_CHANGE
	};

	enum class DecisionStatus : std::uint8_t
	{
		ACCEPTED_NEW = 0,
		ACCEPTED_UNCHANGED,
		REJECTED_INVALID_INPUT,
		REJECTED_INVALID_CONTEXT,
		REJECTED_STALE_SQUAD,
		REJECTED_STALE_REVISION,
		REJECTED_REVISION_WITHOUT_CHANGE
	};

	// The canonical snapshot is a value-only contract.  It contains facts and
	// identity only; it contains no action, target claim, formation slot, or
	// navigation policy.
	struct SquadCanonicalSnapshot
	{
		bool valid = false;
		DecisionDiagnostic diagnostic = DecisionDiagnostic::NONE;
		SquadIdentity identity;
		CommanderOrderRef commanderOrder;
		SquadParticipationPolicy participationPolicy;
		std::vector<MemberState> members;
		std::vector<RecipientId> eligibleMembers;
		RecipientId leaderId = 0;
		Fingerprint canonicalFingerprint = 0;
	};

	struct SquadDecisionResult
	{
		DecisionStatus status = DecisionStatus::REJECTED_INVALID_INPUT;
		DecisionDiagnostic diagnostic = DecisionDiagnostic::NONE;
		SquadIdentity identity;
		DecisionRevision decisionRevision = 0;
		CommanderOrderRef commanderOrder;
		SquadParticipationPolicy participationPolicy;
		std::vector<MemberState> canonicalMembers;
		std::vector<RecipientId> eligibleMembers;
		RecipientId leaderId = 0;
		Fingerprint canonicalFingerprint = 0;
		CohesionFacts cohesion;
		std::vector<ParticipationDecision> participation;
		bool changed = false;

		bool Accepted() const
		{
			return status == DecisionStatus::ACCEPTED_NEW ||
				status == DecisionStatus::ACCEPTED_UNCHANGED;
		}
	};

	class SquadDecisionCore
	{
	public:
		static SquadCanonicalSnapshot Canonicalize(
			const SquadDecisionInput& input);

		static SquadDecisionResult Evaluate(
			const SquadDecisionInput& input,
			const SquadDecisionRevisionContext& previous = {});
	};

	const char* DecisionDiagnosticName(DecisionDiagnostic diagnostic);
	const char* DecisionStatusName(DecisionStatus status);
	const char* MemberLifecycleName(MemberLifecycle lifecycle);
	const char* ParticipationStateName(ParticipationState state);
}

#endif // __RCBOT_SQUAD_DECISION_H__
