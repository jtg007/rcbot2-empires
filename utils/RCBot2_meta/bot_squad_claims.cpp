#include "bot_squad_claims.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace RCBotSquadClaims
{
namespace
{
constexpr Fingerprint kFnvOffset = 1469598103934665603ULL;
constexpr Fingerprint kFnvPrime = 1099511628211ULL;

void MixByte(Fingerprint& hash, const std::uint8_t value)
{
	hash ^= value;
	hash *= kFnvPrime;
}

void MixBool(Fingerprint& hash, const bool value)
{
	MixByte(hash, value ? 1 : 0);
}

void MixU32(Fingerprint& hash, const std::uint32_t value)
{
	for (unsigned shift = 0; shift < 32; shift += 8)
		MixByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
}

void MixU64(Fingerprint& hash, const std::uint64_t value)
{
	for (unsigned shift = 0; shift < 64; shift += 8)
		MixByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffULL));
}

void MixI32(Fingerprint& hash, const std::int32_t value)
{
	MixU32(hash, static_cast<std::uint32_t>(value));
}

void MixDouble(Fingerprint& hash, double value)
{
	if (value == 0.0)
		value = 0.0;
	std::uint64_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value), "double must be IEEE-sized");
	std::memcpy(&bits, &value, sizeof(bits));
	MixU64(hash, bits);
}

bool IsKnownResourceKind(const ResourceKind kind)
{
	return static_cast<std::uint8_t>(kind) <=
		static_cast<std::uint8_t>(ResourceKind::SPATIAL_RESOURCE);
}

bool IsKnownOperation(const ClaimOperation operation)
{
	return static_cast<std::uint8_t>(operation) <=
		static_cast<std::uint8_t>(ClaimOperation::RELEASE);
}

bool IsValidOrderKey(const OrderKey& order)
{
	return order.orderId != 0 && order.generation != 0;
}

bool IsZeroOrderKey(const OrderKey& order)
{
	return order.orderId == 0 && order.generation == 0;
}

bool SameOrder(const bool leftPresent, const OrderKey& left,
	const bool rightPresent, const OrderKey& right)
{
	return leftPresent == rightPresent &&
		(!leftPresent || (left.orderId == right.orderId &&
			left.generation == right.generation));
}

bool SameScope(const ClaimScope& left, const ClaimScope& right)
{
	return left.squadGeneration == right.squadGeneration &&
		left.decisionRevision == right.decisionRevision &&
		SameOrder(left.hasParentOrder, left.parentOrder,
			right.hasParentOrder, right.parentOrder);
}

bool SameScope(const ClaimRecord& record, const ClaimContext& context)
{
	return record.squadGeneration == context.scope.squadGeneration &&
		record.decisionRevision == context.scope.decisionRevision &&
		SameOrder(record.hasParentOrder, record.parentOrder,
			context.scope.hasParentOrder, context.scope.parentOrder);
}

void MixOrder(Fingerprint& hash, const bool present, const OrderKey& order)
{
	MixBool(hash, present);
	if (present)
	{
		MixU64(hash, order.orderId);
		MixU64(hash, order.generation);
	}
}

void MixScope(Fingerprint& hash, const ClaimScope& scope)
{
	MixU64(hash, scope.squadGeneration);
	MixU64(hash, scope.decisionRevision);
	MixOrder(hash, scope.hasParentOrder, scope.parentOrder);
}

void MixClaimKey(Fingerprint& hash, const ClaimKey& key)
{
	MixByte(hash, static_cast<std::uint8_t>(key.resourceKind));
	MixU64(hash, key.resourceId);
	MixU64(hash, key.resourceGeneration);
}

void MixClaim(Fingerprint& hash, const ClaimRecord& claim)
{
	MixClaimKey(hash, claim.key);
	MixU64(hash, claim.ownerMemberId);
	MixU64(hash, claim.squadGeneration);
	MixU64(hash, claim.decisionRevision);
	MixI32(hash, claim.priority);
	MixDouble(hash, claim.issuedAt);
	MixDouble(hash, claim.leaseExpiresAt);
	MixOrder(hash, claim.hasParentOrder, claim.parentOrder);
}

Fingerprint FingerprintOf(const ClaimSnapshot& snapshot)
{
	Fingerprint hash = kFnvOffset;
	MixScope(hash, snapshot.scope);
	MixU64(hash, static_cast<std::uint64_t>(snapshot.claims.size()));
	for (const ClaimRecord& claim : snapshot.claims)
		MixClaim(hash, claim);
	return hash;
}

bool SameClaim(const ClaimRecord& left, const ClaimRecord& right)
{
	return left.key == right.key && left.ownerMemberId == right.ownerMemberId &&
		left.squadGeneration == right.squadGeneration &&
		left.decisionRevision == right.decisionRevision &&
		left.priority == right.priority && left.issuedAt == right.issuedAt &&
		left.leaseExpiresAt == right.leaseExpiresAt &&
		SameOrder(left.hasParentOrder, left.parentOrder,
			right.hasParentOrder, right.parentOrder);
}

bool SameSnapshot(const ClaimSnapshot& left, const ClaimSnapshot& right)
{
	if (!left.valid || !right.valid || !SameScope(left.scope, right.scope) ||
		left.claims.size() != right.claims.size())
		return false;
	for (std::size_t i = 0; i < left.claims.size(); ++i)
	{
		if (!SameClaim(left.claims[i], right.claims[i]))
			return false;
	}
	return true;
}

int FindClaim(const std::vector<ClaimRecord>& claims, const ClaimKey& key)
{
	for (std::size_t i = 0; i < claims.size(); ++i)
	{
		if (claims[i].key == key)
			return static_cast<int>(i);
	}
	return -1;
}

ClaimSnapshot BuildSnapshot(const ClaimScope& scope,
	std::vector<ClaimRecord> claims)
{
	ClaimSnapshot snapshot;
	snapshot.valid = true;
	snapshot.scope = scope;
	std::sort(claims.begin(), claims.end(),
		[](const ClaimRecord& left, const ClaimRecord& right) {
			return left.key < right.key;
		});
	snapshot.claims = std::move(claims);
	snapshot.canonicalFingerprint = FingerprintOf(snapshot);
	return snapshot;
}

struct Candidate
{
	ClaimRecord claim;
	bool existing = false;
};

bool BetterCandidate(const Candidate& left, const Candidate& right)
{
	if (left.claim.priority != right.claim.priority)
		return left.claim.priority > right.claim.priority;
	if (left.claim.ownerMemberId != right.claim.ownerMemberId)
		return left.claim.ownerMemberId < right.claim.ownerMemberId;
	// A current owner's active record wins an exact self-acquire tie.  This
	// keeps ACQUIRE idempotent; RENEW is the explicit lease extension path.
	if (left.existing != right.existing)
		return left.existing;
	if (left.claim.leaseExpiresAt != right.claim.leaseExpiresAt)
		return left.claim.leaseExpiresAt > right.claim.leaseExpiresAt;
	return left.claim.issuedAt < right.claim.issuedAt;
}

ClaimResolutionResult Reject(const ClaimSnapshot& current,
	const ClaimResolutionStatus status, const ClaimDiagnostic diagnostic)
{
	ClaimResolutionResult result;
	result.status = status;
	result.diagnostic = diagnostic;
	result.snapshot = current;
	return result;
}

ClaimResolutionResult BuildResult(const ClaimSnapshot& current,
	const ClaimSnapshot& next)
{
	ClaimResolutionResult result;
	result.snapshot = next;
	result.changed = !SameSnapshot(current, next);
	result.status = result.changed ? ClaimResolutionStatus::ACCEPTED_NEW :
		ClaimResolutionStatus::ACCEPTED_UNCHANGED;
	return result;
}

ClaimDiagnostic ValidateRequest(const ClaimRequest& request,
	const ClaimContext& context)
{
	if (!IsKnownOperation(request.operation))
		return ClaimDiagnostic::INVALID_REQUEST;
	if (!request.claim.key.IsValid())
		return ClaimDiagnostic::INVALID_CLAIM_KEY;
	if (!request.claim.IsValid())
		return ClaimDiagnostic::INVALID_CLAIM_RECORD;
	if (request.claim.squadGeneration != context.scope.squadGeneration)
		return ClaimDiagnostic::STALE_SQUAD_GENERATION;
	if (request.claim.decisionRevision != context.scope.decisionRevision)
		return ClaimDiagnostic::STALE_DECISION_REVISION;
	if (!SameOrder(request.claim.hasParentOrder, request.claim.parentOrder,
		context.scope.hasParentOrder, context.scope.parentOrder))
		return ClaimDiagnostic::STALE_PARENT_ORDER;
	if ((request.operation == ClaimOperation::ACQUIRE ||
		request.operation == ClaimOperation::RENEW) &&
		request.claim.leaseExpiresAt <= context.now)
		return ClaimDiagnostic::EXPIRED_LEASE;
	return ClaimDiagnostic::NONE;
}

ClaimResolutionResult RejectForDiagnostic(const ClaimSnapshot& current,
	const ClaimDiagnostic diagnostic)
{
	ClaimResolutionStatus status = ClaimResolutionStatus::REJECTED_INVALID_INPUT;
	if (diagnostic == ClaimDiagnostic::STALE_SQUAD_GENERATION ||
		diagnostic == ClaimDiagnostic::STALE_DECISION_REVISION ||
		diagnostic == ClaimDiagnostic::STALE_PARENT_ORDER)
		status = ClaimResolutionStatus::REJECTED_STALE_CONTEXT;
	else if (diagnostic == ClaimDiagnostic::NOT_CURRENT_OWNER)
		status = ClaimResolutionStatus::REJECTED_NOT_OWNER;
	else if (diagnostic == ClaimDiagnostic::CLAIM_NOT_FOUND)
		status = ClaimResolutionStatus::REJECTED_NOT_FOUND;
	else if (diagnostic == ClaimDiagnostic::CLAIM_CAPACITY)
		status = ClaimResolutionStatus::REJECTED_CAPACITY;
	return Reject(current, status, diagnostic);
}
}

bool ClaimKey::IsValid() const
{
	return IsKnownResourceKind(resourceKind) && resourceId != 0 &&
		resourceGeneration != 0;
}

bool ClaimKey::operator<(const ClaimKey& other) const
{
	const std::uint8_t leftKind = static_cast<std::uint8_t>(resourceKind);
	const std::uint8_t rightKind = static_cast<std::uint8_t>(other.resourceKind);
	if (leftKind != rightKind)
		return leftKind < rightKind;
	if (resourceId != other.resourceId)
		return resourceId < other.resourceId;
	return resourceGeneration < other.resourceGeneration;
}

bool ClaimScope::IsValid() const
{
	if (squadGeneration == 0 || decisionRevision == 0)
		return false;
	if (hasParentOrder)
		return IsValidOrderKey(parentOrder);
	return IsZeroOrderKey(parentOrder);
}

bool ClaimContext::IsValid() const
{
	return scope.IsValid() && std::isfinite(now);
}

bool ClaimRecord::IsValid() const
{
	if (!key.IsValid() || ownerMemberId == 0 || squadGeneration == 0 ||
		decisionRevision == 0 || !std::isfinite(issuedAt) ||
		!std::isfinite(leaseExpiresAt) || leaseExpiresAt <= issuedAt)
		return false;
	if (hasParentOrder)
		return IsValidOrderKey(parentOrder);
	return IsZeroOrderKey(parentOrder);
}

bool ClaimRecord::IsLeaseActive(const double now) const
{
	return IsValid() && std::isfinite(now) && now < leaseExpiresAt;
}

bool ClaimRequest::IsValid() const
{
	return IsKnownOperation(operation) && claim.IsValid();
}

bool ClaimSnapshot::IsValid() const
{
	if (!valid || !scope.IsValid() || claims.size() > kMaxClaimsPerSnapshot ||
		canonicalFingerprint == 0)
		return false;
	for (std::size_t i = 0; i < claims.size(); ++i)
	{
		const ClaimRecord& claim = claims[i];
		if (!claim.IsValid() || claim.squadGeneration != scope.squadGeneration ||
			claim.decisionRevision != scope.decisionRevision ||
			!SameOrder(claim.hasParentOrder, claim.parentOrder,
				scope.hasParentOrder, scope.parentOrder))
			return false;
		if (i > 0 && !(claims[i - 1].key < claim.key))
			return false;
	}
	return canonicalFingerprint == FingerprintOf(*this);
}

ClaimSnapshot SquadClaimCore::EmptySnapshot(const ClaimContext& context)
{
	if (!context.IsValid())
		return {};
	return BuildSnapshot(context.scope, {});
}

ClaimResolutionResult SquadClaimCore::Resolve(
	const ClaimSnapshot& current, const ClaimContext& context,
	const std::vector<ClaimRequest>& requests)
{
	if (!context.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_CONTEXT);
	if (!current.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_SNAPSHOT);
	if (requests.size() > kMaxRequestsPerResolution)
		return RejectForDiagnostic(current, ClaimDiagnostic::CLAIM_CAPACITY);

	for (std::size_t i = 0; i < requests.size(); ++i)
	{
		const ClaimDiagnostic diagnostic = ValidateRequest(requests[i], context);
		if (diagnostic != ClaimDiagnostic::NONE)
			return RejectForDiagnostic(current, diagnostic);
		for (std::size_t j = 0; j < i; ++j)
		{
			if (requests[i].claim.key == requests[j].claim.key &&
				requests[i].claim.ownerMemberId ==
					requests[j].claim.ownerMemberId)
				return RejectForDiagnostic(current,
					ClaimDiagnostic::DUPLICATE_REQUEST);
		}
	}

	std::vector<ClaimRecord> working;
	working.reserve(current.claims.size());
	for (const ClaimRecord& claim : current.claims)
	{
		if (SameScope(claim, context) && claim.IsLeaseActive(context.now))
			working.push_back(claim);
	}

	for (const ClaimRequest& request : requests)
	{
		if (request.operation != ClaimOperation::RENEW &&
			request.operation != ClaimOperation::RELEASE)
			continue;
		const int index = FindClaim(working, request.claim.key);
		if (index < 0)
			return RejectForDiagnostic(current, ClaimDiagnostic::CLAIM_NOT_FOUND);
		if (working[static_cast<std::size_t>(index)].ownerMemberId !=
			request.claim.ownerMemberId)
			return RejectForDiagnostic(current, ClaimDiagnostic::NOT_CURRENT_OWNER);
		if (request.operation == ClaimOperation::RENEW &&
			request.claim.leaseExpiresAt <=
				working[static_cast<std::size_t>(index)].issuedAt)
			return RejectForDiagnostic(current,
				ClaimDiagnostic::INVALID_CLAIM_RECORD);
	}

	for (const ClaimRequest& request : requests)
	{
		if (request.operation != ClaimOperation::RELEASE)
			continue;
		const int index = FindClaim(working, request.claim.key);
		working.erase(working.begin() + index);
	}

	for (const ClaimRequest& request : requests)
	{
		if (request.operation != ClaimOperation::RENEW)
			continue;
		const int index = FindClaim(working, request.claim.key);
		if (index >= 0)
			working[static_cast<std::size_t>(index)].leaseExpiresAt =
				request.claim.leaseExpiresAt;
	}

	std::vector<ClaimRequest> acquires;
	for (const ClaimRequest& request : requests)
	{
		if (request.operation == ClaimOperation::ACQUIRE)
			acquires.push_back(request);
	}
	std::sort(acquires.begin(), acquires.end(),
		[](const ClaimRequest& left, const ClaimRequest& right) {
			return left.claim.key < right.claim.key;
		});

	std::size_t acquireIndex = 0;
	while (acquireIndex < acquires.size())
	{
		const ClaimKey key = acquires[acquireIndex].claim.key;
		std::vector<Candidate> candidates;
		const int existingIndex = FindClaim(working, key);
		if (existingIndex >= 0)
			candidates.push_back({
				working[static_cast<std::size_t>(existingIndex)], true });
		while (acquireIndex < acquires.size() &&
			acquires[acquireIndex].claim.key == key)
		{
			candidates.push_back({ acquires[acquireIndex].claim, false });
			++acquireIndex;
		}
		// An active claim is not preempted by a later ACQUIRE.  Priority
		// resolves simultaneous requests for an unowned key; expiry or an
		// explicit owner release is required before another member takes over.
		if (existingIndex >= 0)
			continue;

		const Candidate* winner = &candidates[0];
		for (std::size_t i = 1; i < candidates.size(); ++i)
		{
			if (BetterCandidate(candidates[i], *winner))
				winner = &candidates[i];
		}
		if (existingIndex >= 0)
			working[static_cast<std::size_t>(existingIndex)] = winner->claim;
		else
			working.push_back(winner->claim);
	}

	if (working.size() > kMaxClaimsPerSnapshot)
		return RejectForDiagnostic(current, ClaimDiagnostic::CLAIM_CAPACITY);
	return BuildResult(current, BuildSnapshot(context.scope, working));
}

ClaimResolutionResult SquadClaimCore::Expire(
	const ClaimSnapshot& current, const ClaimContext& context)
{
	return Resolve(current, context, {});
}

ClaimResolutionResult SquadClaimCore::PurgeStale(
	const ClaimSnapshot& current, const ClaimContext& context)
{
	return Resolve(current, context, {});
}

ClaimResolutionResult SquadClaimCore::PurgeMemberClaims(
	const ClaimSnapshot& current, const ClaimContext& context,
	const RecipientId memberId)
{
	if (memberId == 0)
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_MEMBER_ID);
	if (!context.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_CONTEXT);
	if (!current.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_SNAPSHOT);

	std::vector<ClaimRecord> working;
	working.reserve(current.claims.size());
	for (const ClaimRecord& claim : current.claims)
	{
		if (SameScope(claim, context) && claim.IsLeaseActive(context.now) &&
			claim.ownerMemberId != memberId)
			working.push_back(claim);
	}
	if (working.size() > kMaxClaimsPerSnapshot)
		return RejectForDiagnostic(current, ClaimDiagnostic::CLAIM_CAPACITY);
	return BuildResult(current, BuildSnapshot(context.scope, working));
}

ClaimResolutionResult SquadClaimCore::PurgeMemberClaimsOfKind(
	const ClaimSnapshot& current, const ClaimContext& context,
	const RecipientId memberId, const ResourceKind resourceKind)
{
	if (memberId == 0)
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_MEMBER_ID);
	if (!ClaimKey{ resourceKind, 1, 1 }.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_CLAIM_KEY);
	if (!context.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_CONTEXT);
	if (!current.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_SNAPSHOT);

	std::vector<ClaimRecord> working;
	working.reserve(current.claims.size());
	for (const ClaimRecord& claim : current.claims)
	{
		const bool remove = SameScope(claim, context) &&
			claim.IsLeaseActive(context.now) &&
			claim.key.resourceKind == resourceKind &&
			claim.ownerMemberId == memberId;
		if (!remove)
			working.push_back(claim);
	}
	return BuildResult(current, BuildSnapshot(context.scope, working));
}

ClaimResolutionResult SquadClaimCore::PurgeResourceGeneration(
	const ClaimSnapshot& current, const ClaimContext& context,
	const ResourceKind resourceKind, const Generation resourceGeneration)
{
	if (!ClaimKey{ resourceKind, 1, resourceGeneration }.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_CLAIM_KEY);
	if (!context.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_CONTEXT);
	if (!current.IsValid())
		return RejectForDiagnostic(current, ClaimDiagnostic::INVALID_SNAPSHOT);

	std::vector<ClaimRecord> working;
	working.reserve(current.claims.size());
	for (const ClaimRecord& claim : current.claims)
	{
		const bool remove = SameScope(claim, context) &&
			claim.IsLeaseActive(context.now) &&
			claim.key.resourceKind == resourceKind &&
			claim.key.resourceGeneration != resourceGeneration;
		if (!remove)
			working.push_back(claim);
	}
	return BuildResult(current, BuildSnapshot(context.scope, working));
}

const char* ResourceKindName(const ResourceKind kind)
{
	switch (kind)
	{
	case ResourceKind::GENERIC_RESOURCE: return "GENERIC_RESOURCE";
	case ResourceKind::OBJECTIVE_RESOURCE: return "OBJECTIVE_RESOURCE";
	case ResourceKind::TARGET_RESOURCE: return "TARGET_RESOURCE";
	case ResourceKind::SPATIAL_RESOURCE: return "SPATIAL_RESOURCE";
	}
	return "UNKNOWN";
}

const char* ClaimOperationName(const ClaimOperation operation)
{
	switch (operation)
	{
	case ClaimOperation::ACQUIRE: return "ACQUIRE";
	case ClaimOperation::RENEW: return "RENEW";
	case ClaimOperation::RELEASE: return "RELEASE";
	}
	return "UNKNOWN";
}

const char* ClaimDiagnosticName(const ClaimDiagnostic diagnostic)
{
	switch (diagnostic)
	{
	case ClaimDiagnostic::NONE: return "NONE";
	case ClaimDiagnostic::INVALID_CONTEXT: return "INVALID_CONTEXT";
	case ClaimDiagnostic::INVALID_SNAPSHOT: return "INVALID_SNAPSHOT";
	case ClaimDiagnostic::INVALID_CLAIM_KEY: return "INVALID_CLAIM_KEY";
	case ClaimDiagnostic::INVALID_CLAIM_RECORD: return "INVALID_CLAIM_RECORD";
	case ClaimDiagnostic::INVALID_REQUEST: return "INVALID_REQUEST";
	case ClaimDiagnostic::DUPLICATE_REQUEST: return "DUPLICATE_REQUEST";
	case ClaimDiagnostic::STALE_SQUAD_GENERATION: return "STALE_SQUAD_GENERATION";
	case ClaimDiagnostic::STALE_DECISION_REVISION: return "STALE_DECISION_REVISION";
	case ClaimDiagnostic::STALE_PARENT_ORDER: return "STALE_PARENT_ORDER";
	case ClaimDiagnostic::EXPIRED_LEASE: return "EXPIRED_LEASE";
	case ClaimDiagnostic::NOT_CURRENT_OWNER: return "NOT_CURRENT_OWNER";
	case ClaimDiagnostic::CLAIM_NOT_FOUND: return "CLAIM_NOT_FOUND";
	case ClaimDiagnostic::CLAIM_CAPACITY: return "CLAIM_CAPACITY";
	case ClaimDiagnostic::INVALID_MEMBER_ID: return "INVALID_MEMBER_ID";
	}
	return "UNKNOWN";
}

const char* ClaimResolutionStatusName(const ClaimResolutionStatus status)
{
	switch (status)
	{
	case ClaimResolutionStatus::ACCEPTED_NEW: return "ACCEPTED_NEW";
	case ClaimResolutionStatus::ACCEPTED_UNCHANGED: return "ACCEPTED_UNCHANGED";
	case ClaimResolutionStatus::REJECTED_INVALID_INPUT: return "REJECTED_INVALID_INPUT";
	case ClaimResolutionStatus::REJECTED_STALE_CONTEXT: return "REJECTED_STALE_CONTEXT";
	case ClaimResolutionStatus::REJECTED_NOT_OWNER: return "REJECTED_NOT_OWNER";
	case ClaimResolutionStatus::REJECTED_NOT_FOUND: return "REJECTED_NOT_FOUND";
	case ClaimResolutionStatus::REJECTED_CAPACITY: return "REJECTED_CAPACITY";
	}
	return "UNKNOWN";
}
}
