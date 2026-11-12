#ifndef __RCBOT_SQUAD_CLAIMS_H__
#define __RCBOT_SQUAD_CLAIMS_H__

#include "bot_order_protocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RCBotSquadClaims
{
	using RCBotOrder::Generation;
	using RCBotOrder::OrderKey;
	using RCBotOrder::RecipientId;

	using DecisionRevision = std::uint64_t;
	using Fingerprint = std::uint64_t;
	using Priority = std::int32_t;

	constexpr std::size_t kMaxClaimsPerSnapshot = 64;
	constexpr std::size_t kMaxRequestsPerResolution = 128;

	// Resource kinds stay generic.  Their gameplay meaning is deliberately
	// outside this core and is not used for target, combat, or formation policy.
	enum class ResourceKind : std::uint8_t
	{
		GENERIC_RESOURCE = 0,
		OBJECTIVE_RESOURCE,
		TARGET_RESOURCE,
		SPATIAL_RESOURCE
	};

	struct ClaimKey
	{
		ResourceKind resourceKind = ResourceKind::GENERIC_RESOURCE;
		std::uint64_t resourceId = 0;
		Generation resourceGeneration = 0;

		bool IsValid() const;
		bool operator==(const ClaimKey& other) const
		{
			return resourceKind == other.resourceKind &&
				resourceId == other.resourceId &&
				resourceGeneration == other.resourceGeneration;
		}
		bool operator!=(const ClaimKey& other) const
		{
			return !(*this == other);
		}
		bool operator<(const ClaimKey& other) const;
	};

	struct ClaimScope
	{
		Generation squadGeneration = 0;
		DecisionRevision decisionRevision = 0;
		bool hasParentOrder = false;
		OrderKey parentOrder;

		bool IsValid() const;
	};

	struct ClaimContext
	{
		ClaimScope scope;
		double now = 0.0;

		bool IsValid() const;
	};

	struct ClaimRecord
	{
		ClaimKey key;
		RecipientId ownerMemberId = 0;
		Generation squadGeneration = 0;
		DecisionRevision decisionRevision = 0;
		Priority priority = 0;
		double issuedAt = 0.0;
		double leaseExpiresAt = 0.0;
		bool hasParentOrder = false;
		OrderKey parentOrder;

		bool IsValid() const;
		bool IsLeaseActive(double now) const;
	};

	enum class ClaimOperation : std::uint8_t
	{
		ACQUIRE = 0,
		RENEW,
		RELEASE
	};

	struct ClaimRequest
	{
		ClaimOperation operation = ClaimOperation::ACQUIRE;
		ClaimRecord claim;

		bool IsValid() const;
	};

	struct ClaimSnapshot
	{
		bool valid = false;
		ClaimScope scope;
		std::vector<ClaimRecord> claims;
		Fingerprint canonicalFingerprint = 0;

		bool IsValid() const;
	};

	enum class ClaimDiagnostic : std::uint8_t
	{
		NONE = 0,
		INVALID_CONTEXT,
		INVALID_SNAPSHOT,
		INVALID_CLAIM_KEY,
		INVALID_CLAIM_RECORD,
		INVALID_REQUEST,
		DUPLICATE_REQUEST,
		STALE_SQUAD_GENERATION,
		STALE_DECISION_REVISION,
		STALE_PARENT_ORDER,
		EXPIRED_LEASE,
		NOT_CURRENT_OWNER,
		CLAIM_NOT_FOUND,
		CLAIM_CAPACITY,
		INVALID_MEMBER_ID
	};

	enum class ClaimResolutionStatus : std::uint8_t
	{
		ACCEPTED_NEW = 0,
		ACCEPTED_UNCHANGED,
		REJECTED_INVALID_INPUT,
		REJECTED_STALE_CONTEXT,
		REJECTED_NOT_OWNER,
		REJECTED_NOT_FOUND,
		REJECTED_CAPACITY
	};

	struct ClaimResolutionResult
	{
		ClaimResolutionStatus status =
			ClaimResolutionStatus::REJECTED_INVALID_INPUT;
		ClaimDiagnostic diagnostic = ClaimDiagnostic::NONE;
		ClaimSnapshot snapshot;
		bool changed = false;

		bool Accepted() const
		{
			return status == ClaimResolutionStatus::ACCEPTED_NEW ||
				status == ClaimResolutionStatus::ACCEPTED_UNCHANGED;
		}
	};

	class SquadClaimCore
	{
	public:
		static ClaimSnapshot EmptySnapshot(const ClaimContext& context);

		// Resolve is pure: the input snapshot and requests are never mutated.
		// Expired and stale records are removed while producing the next value
		// snapshot.  A rejected request returns the original snapshot unchanged.
		static ClaimResolutionResult Resolve(
			const ClaimSnapshot& current,
			const ClaimContext& context,
			const std::vector<ClaimRequest>& requests);

		// Explicit lifecycle/replacement helpers.  Both produce a new value
		// snapshot and do not own a mutable claim table or a timer.
		static ClaimResolutionResult Expire(
			const ClaimSnapshot& current, const ClaimContext& context);
		static ClaimResolutionResult PurgeStale(
			const ClaimSnapshot& current, const ClaimContext& context);
		static ClaimResolutionResult PurgeMemberClaims(
			const ClaimSnapshot& current,
			const ClaimContext& context,
			RecipientId memberId);
		static ClaimResolutionResult PurgeMemberClaimsOfKind(
			const ClaimSnapshot& current,
			const ClaimContext& context,
			RecipientId memberId,
			ResourceKind resourceKind);
		static ClaimResolutionResult PurgeResourceGeneration(
			const ClaimSnapshot& current,
			const ClaimContext& context,
			ResourceKind resourceKind,
			Generation resourceGeneration);
	};

	const char* ResourceKindName(ResourceKind kind);
	const char* ClaimOperationName(ClaimOperation operation);
	const char* ClaimDiagnosticName(ClaimDiagnostic diagnostic);
	const char* ClaimResolutionStatusName(ClaimResolutionStatus status);
}

#endif // __RCBOT_SQUAD_CLAIMS_H__
