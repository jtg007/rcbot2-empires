#ifndef __RCBOT_SQUAD_SPATIAL_H__
#define __RCBOT_SQUAD_SPATIAL_H__

#include "bot_squad_claims.h"
#include "bot_squad_decision.h"

#include <cstdint>
#include <vector>

namespace RCBotSquadSpatial
{
	using RCBotOrder::RecipientId;
	using RCBotOrder::TargetPosition;
	using RCBotSquadClaims::ClaimDiagnostic;
	using RCBotSquadClaims::ClaimContext;
	using RCBotSquadClaims::ClaimKey;
	using RCBotSquadClaims::ClaimOperation;
	using RCBotSquadClaims::ClaimRecord;
	using RCBotSquadClaims::ClaimResolutionResult;
	using RCBotSquadClaims::ClaimResolutionStatus;
	using RCBotSquadClaims::ClaimRequest;
	using RCBotSquadClaims::ClaimSnapshot;
	using RCBotSquadClaims::Generation;
	using RCBotSquadClaims::ResourceKind;
	using RCBotSquadClaims::SquadClaimCore;
	using RCBotSquadDecision::MemberState;
	using RCBotSquadDecision::ParticipationState;
	using RCBotSquadDecision::ParticipationDecision;
	using RCBotSquadDecision::SquadDecisionResult;

	using Fingerprint = std::uint64_t;

	enum class FormationType : std::uint8_t
	{
		COLUMN = 0,
		LINE,
		WEDGE
	};

	// The frame is supplied by a later policy/adapter.  This core never infers
	// orientation from engine state and canonicalizes the two directions before
	// generating any slot geometry.
	struct FormationFrame
	{
		TargetPosition anchor;
		TargetPosition forward;
		TargetPosition right;

		bool IsValid() const;
	};

	struct FormationPolicy
	{
		double memberSpacing = 64.0;
		double rowSpacing = 64.0;
		double slotLeaseDuration = 1.0;
		std::uint32_t maxSlots = 8;

		bool IsValid() const;
	};

	enum class SpatialRole : std::uint8_t
	{
		ANCHOR = 0,
		CENTER,
		LEFT,
		RIGHT,
		REAR,
		REAR_LEFT,
		REAR_RIGHT
	};

	struct FormationSlot
	{
		std::uint64_t slotId = 0;
		SpatialRole role = SpatialRole::ANCHOR;
		TargetPosition localOffset;
		TargetPosition worldPosition;
		ClaimKey claimKey;

		bool IsValid(Generation formationGeneration) const;
	};

	enum class SpatialClaimStatus : std::uint8_t
	{
		CLAIMED = 0,
		UNAVAILABLE
	};

	struct FormationAssignment
	{
		RecipientId memberId = 0;
		std::uint64_t slotId = 0;
		SpatialRole role = SpatialRole::ANCHOR;
		ParticipationState participation = ParticipationState::EXCLUDED;
		TargetPosition targetPosition;
		ClaimKey claimKey;
		SpatialClaimStatus claimStatus = SpatialClaimStatus::UNAVAILABLE;
		double geometricCost = 0.0;
	};

	struct SpatialFormationInput
	{
		SquadDecisionResult decision;
		FormationType formationType = FormationType::COLUMN;
		FormationPolicy policy;
		FormationFrame frame;
		Generation formationGeneration = 0;
		double now = 0.0;
		bool hasExistingClaims = false;
		ClaimSnapshot existingClaims;
};

	enum class FormationDiagnostic : std::uint8_t
	{
		NONE = 0,
		INVALID_DECISION,
		INVALID_FRAME,
		INVALID_POLICY,
		INVALID_FORMATION_GENERATION,
		INVALID_FORMATION_TYPE,
		NON_FINITE_GEOMETRY,
		CLAIM_RESOLUTION_FAILED
	};

	enum class FormationStatus : std::uint8_t
	{
		ACCEPTED = 0,
		REJECTED_INVALID_INPUT,
		REJECTED_CLAIMS
	};

	struct FormationDecisionResult
	{
		FormationStatus status = FormationStatus::REJECTED_INVALID_INPUT;
		FormationDiagnostic diagnostic = FormationDiagnostic::NONE;
		FormationType formationType = FormationType::COLUMN;
		Generation formationGeneration = 0;
		FormationFrame frame;
		FormationPolicy policy;
		std::vector<FormationSlot> slots;
		std::vector<FormationAssignment> assignments;
		ClaimSnapshot claimSnapshot;
		ClaimResolutionStatus claimResolutionStatus =
			ClaimResolutionStatus::REJECTED_INVALID_INPUT;
		ClaimDiagnostic claimDiagnostic = ClaimDiagnostic::NONE;
		Fingerprint canonicalFingerprint = 0;

		bool Accepted() const
		{
			return status == FormationStatus::ACCEPTED;
		}
	};

	class SquadSpatialCore
	{
	public:
		static FormationDecisionResult Evaluate(
			const SpatialFormationInput& input);
	};

	const char* FormationTypeName(FormationType type);
	const char* SpatialRoleName(SpatialRole role);
	const char* SpatialClaimStatusName(SpatialClaimStatus status);
	const char* FormationDiagnosticName(FormationDiagnostic diagnostic);
	const char* FormationStatusName(FormationStatus status);
}

#endif // __RCBOT_SQUAD_SPATIAL_H__
