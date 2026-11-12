#include "bot_squad_claims.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
using namespace RCBotOrder;
using namespace RCBotSquadClaims;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

ClaimContext Context(const Generation squadGeneration = 7,
	const DecisionRevision decisionRevision = 3, const double now = 0.0,
	const Generation parentGeneration = 4)
{
	ClaimContext context;
	context.scope.squadGeneration = squadGeneration;
	context.scope.decisionRevision = decisionRevision;
	context.scope.hasParentOrder = true;
	context.scope.parentOrder = { 9001, parentGeneration };
	context.now = now;
	return context;
}

ClaimKey Key(const std::uint64_t resourceId,
	const Generation resourceGeneration = 1,
	const ResourceKind kind = ResourceKind::GENERIC_RESOURCE)
{
	return { kind, resourceId, resourceGeneration };
}

ClaimRecord Record(const ClaimContext& context, const ClaimKey& key,
	const RecipientId owner, const Priority priority,
	const double issuedAt = 0.0, const double leaseExpiresAt = 10.0)
{
	ClaimRecord claim;
	claim.key = key;
	claim.ownerMemberId = owner;
	claim.squadGeneration = context.scope.squadGeneration;
	claim.decisionRevision = context.scope.decisionRevision;
	claim.priority = priority;
	claim.issuedAt = issuedAt;
	claim.leaseExpiresAt = leaseExpiresAt;
	claim.hasParentOrder = context.scope.hasParentOrder;
	claim.parentOrder = context.scope.parentOrder;
	return claim;
}

ClaimRequest Acquire(const ClaimContext& context, const ClaimKey& key,
	const RecipientId owner, const Priority priority,
	const double issuedAt = 0.0, const double leaseExpiresAt = 10.0)
{
	ClaimRequest request;
	request.operation = ClaimOperation::ACQUIRE;
	request.claim = Record(context, key, owner, priority, issuedAt,
		leaseExpiresAt);
	return request;
}

ClaimRequest Renew(const ClaimRecord& current, const double leaseExpiresAt)
{
	ClaimRequest request;
	request.operation = ClaimOperation::RENEW;
	request.claim = current;
	request.claim.leaseExpiresAt = leaseExpiresAt;
	return request;
}

ClaimRequest Release(const ClaimContext& context, const ClaimKey& key,
	const RecipientId owner)
{
	ClaimRequest request;
	request.operation = ClaimOperation::RELEASE;
	request.claim = Record(context, key, owner, 0, 0.0, 10.0);
	request.operation = ClaimOperation::RELEASE;
	return request;
}

ClaimSnapshot Empty(const ClaimContext& context)
{
	const ClaimSnapshot snapshot = SquadClaimCore::EmptySnapshot(context);
	Require(snapshot.IsValid(), "empty snapshot is valid");
	return snapshot;
}

ClaimSnapshot AcquireOne(const ClaimContext& context, const ClaimKey& key,
	const RecipientId owner, const Priority priority,
	const double leaseExpiresAt = 10.0)
{
	const ClaimSnapshot empty = Empty(context);
	const ClaimResolutionResult result = SquadClaimCore::Resolve(empty, context,
		{ Acquire(context, key, owner, priority, 0.0, leaseExpiresAt) });
	Require(result.status == ClaimResolutionStatus::ACCEPTED_NEW &&
		result.snapshot.claims.size() == 1,
		"single claim acquired");
	return result.snapshot;
}

void TestSingleAcquire()
{
	const ClaimContext context = Context();
	const ClaimSnapshot snapshot = AcquireOne(context, Key(11), 101, 4);
	Require(snapshot.claims[0].ownerMemberId == 101 &&
		snapshot.claims[0].key == Key(11),
		"CLAIM-01 valid claim owner");
	std::cout << "CLAIM-01 PASS\n";
}

void TestHigherPriorityWins()
{
	const ClaimContext context = Context();
	const ClaimSnapshot empty = Empty(context);
	const ClaimResolutionResult result = SquadClaimCore::Resolve(empty, context,
		{ Acquire(context, Key(12), 101, 2),
			Acquire(context, Key(12), 102, 9) });
	Require(result.Accepted() && result.snapshot.claims.size() == 1 &&
		result.snapshot.claims[0].ownerMemberId == 102,
		"CLAIM-02 higher priority wins");
	std::cout << "CLAIM-02 PASS\n";
}

void TestEqualPriorityUsesMemberId()
{
	const ClaimContext context = Context();
	const ClaimResolutionResult result = SquadClaimCore::Resolve(Empty(context),
		context, { Acquire(context, Key(13), 202, 5),
			Acquire(context, Key(13), 201, 5) });
	Require(result.snapshot.claims.size() == 1 &&
		result.snapshot.claims[0].ownerMemberId == 201,
		"CLAIM-03 equal priority uses smaller member id");
	std::cout << "CLAIM-03 PASS\n";
}

void TestRequestOrderIndependent()
{
	const ClaimContext context = Context();
	const ClaimRequest first = Acquire(context, Key(14), 301, 3);
	const ClaimRequest second = Acquire(context, Key(14), 302, 3);
	const ClaimResolutionResult left = SquadClaimCore::Resolve(Empty(context),
		context, { first, second });
	const ClaimResolutionResult right = SquadClaimCore::Resolve(Empty(context),
		context, { second, first });
	Require(left.snapshot.canonicalFingerprint ==
		right.snapshot.canonicalFingerprint &&
		left.snapshot.claims[0].ownerMemberId == 301,
		"CLAIM-04 request order does not affect winner");
	std::cout << "CLAIM-04 PASS\n";
}

void TestExpiredClaimDoesNotBlock()
{
	const ClaimContext initialContext = Context(7, 3, 0.0);
	const ClaimSnapshot current = AcquireOne(initialContext, Key(15), 401, 8,
		10.0);
	const ClaimContext expiredContext = Context(7, 3, 10.0);
	const ClaimResolutionResult result = SquadClaimCore::Resolve(current,
		expiredContext, { Acquire(expiredContext, Key(15), 402, 1, 10.0, 20.0) });
	Require(result.snapshot.claims.size() == 1 &&
		result.snapshot.claims[0].ownerMemberId == 402,
		"CLAIM-05 expired claim no longer blocks");
	std::cout << "CLAIM-05 PASS\n";
}

void TestLeaseBoundary()
{
	const ClaimContext initialContext = Context(7, 3, 0.0);
	const ClaimSnapshot current = AcquireOne(initialContext, Key(16), 501, 1,
		10.0);
	Require(current.claims[0].IsLeaseActive(9.999) &&
		!current.claims[0].IsLeaseActive(10.0),
		"CLAIM-06 exact lease boundary");
	const ClaimResolutionResult expired = SquadClaimCore::Expire(current,
		Context(7, 3, 10.0));
	Require(expired.Accepted() && expired.snapshot.claims.empty(),
		"CLAIM-06 boundary expiry purges claim");
	std::cout << "CLAIM-06 PASS\n";
}

void TestOwnerCanRenew()
{
	const ClaimContext initialContext = Context(7, 3, 0.0);
	const ClaimSnapshot current = AcquireOne(initialContext, Key(17), 601, 2,
		10.0);
	const ClaimContext renewContext = Context(7, 3, 5.0);
	const ClaimResolutionResult result = SquadClaimCore::Resolve(current,
		renewContext, { Renew(current.claims[0], 30.0) });
	Require(result.Accepted() && result.snapshot.claims.size() == 1 &&
		result.snapshot.claims[0].leaseExpiresAt == 30.0,
		"CLAIM-07 owner renews active claim");
	std::cout << "CLAIM-07 PASS\n";
}

void TestNonOwnerCannotOverwriteOrRelease()
{
	const ClaimContext context = Context();
	const ClaimSnapshot current = AcquireOne(context, Key(18), 701, 2, 20.0);
	const ClaimResolutionResult higherAcquire = SquadClaimCore::Resolve(current,
		context, { Acquire(context, Key(18), 702, 99, 0.0, 30.0) });
	Require(higherAcquire.status == ClaimResolutionStatus::ACCEPTED_UNCHANGED &&
		higherAcquire.snapshot.claims[0].ownerMemberId == 701,
		"CLAIM-08 active owner cannot be overwritten");

	const ClaimResolutionResult renew = SquadClaimCore::Resolve(current, context,
		{ Renew(Record(context, Key(18), 702, 0), 30.0) });
	Require(renew.status == ClaimResolutionStatus::REJECTED_NOT_OWNER &&
		renew.snapshot.canonicalFingerprint == current.canonicalFingerprint,
		"CLAIM-08 non-owner cannot renew");
	const ClaimResolutionResult release = SquadClaimCore::Resolve(current, context,
		{ Release(context, Key(18), 702) });
	Require(release.status == ClaimResolutionStatus::REJECTED_NOT_OWNER &&
		release.snapshot.canonicalFingerprint == current.canonicalFingerprint,
		"CLAIM-08 non-owner cannot release");
	std::cout << "CLAIM-08 PASS\n";
}

void TestStaleSquadGenerationRejected()
{
	const ClaimContext context = Context();
	const ClaimSnapshot current = AcquireOne(context, Key(19), 801, 1);
	const ClaimRequest stale = Acquire(Context(8, 3), Key(20), 802, 1);
	const ClaimResolutionResult result = SquadClaimCore::Resolve(current, context,
		{ stale });
	Require(result.status == ClaimResolutionStatus::REJECTED_STALE_CONTEXT &&
		result.diagnostic == ClaimDiagnostic::STALE_SQUAD_GENERATION &&
		result.snapshot.canonicalFingerprint == current.canonicalFingerprint,
		"CLAIM-09 stale squad generation rejected without mutation");
	std::cout << "CLAIM-09 PASS\n";
}

void TestStaleDecisionRevisionRejected()
{
	const ClaimContext context = Context();
	const ClaimSnapshot current = AcquireOne(context, Key(21), 901, 1);
	const ClaimRequest stale = Acquire(Context(7, 2), Key(22), 902, 1);
	const ClaimResolutionResult result = SquadClaimCore::Resolve(current, context,
		{ stale });
	Require(result.status == ClaimResolutionStatus::REJECTED_STALE_CONTEXT &&
		result.diagnostic == ClaimDiagnostic::STALE_DECISION_REVISION &&
		result.snapshot.canonicalFingerprint == current.canonicalFingerprint,
		"CLAIM-10 stale decision revision rejected without mutation");
	std::cout << "CLAIM-10 PASS\n";
}

void TestResourceGenerationSeparatesKeys()
{
	const ClaimContext context = Context();
	const ClaimResolutionResult result = SquadClaimCore::Resolve(Empty(context),
		context, { Acquire(context, Key(23, 1), 1001, 1),
			Acquire(context, Key(23, 2), 1002, 1) });
	Require(result.snapshot.claims.size() == 2 &&
		result.snapshot.claims[0].key != result.snapshot.claims[1].key,
		"CLAIM-11 resource generation separates reused resources");
	std::cout << "CLAIM-11 PASS\n";
}

void TestMemberLifecyclePurge()
{
	const ClaimContext context = Context();
	const ClaimSnapshot current = AcquireOne(context, Key(24), 1101, 1);
	const ClaimResolutionResult result = SquadClaimCore::PurgeMemberClaims(
		current, context, 1101);
	Require(result.Accepted() && result.snapshot.claims.empty(),
		"CLAIM-12 member lifecycle removes claims");
	std::cout << "CLAIM-12 PASS\n";
}

void TestReplacementPurgesOldScope()
{
	const ClaimContext oldContext = Context(7, 3, 0.0, 4);
	const ClaimSnapshot current = AcquireOne(oldContext, Key(25), 1201, 1);
	const ClaimContext replacement = Context(7, 4, 5.0, 5);
	const ClaimResolutionResult result = SquadClaimCore::PurgeStale(current,
		replacement);
	Require(result.Accepted() && result.snapshot.claims.empty() &&
		result.snapshot.scope.decisionRevision == 4 &&
		result.snapshot.scope.parentOrder.generation == 5,
		"CLAIM-13 replacement purges old revision and order");
	std::cout << "CLAIM-13 PASS\n";
}

void TestIdenticalInputIsStable()
{
	const ClaimContext context = Context();
	const ClaimRequest first = Acquire(context, Key(26), 1301, 3);
	const ClaimRequest second = Acquire(context, Key(27), 1302, 4);
	const ClaimResolutionResult left = SquadClaimCore::Resolve(Empty(context),
		context, { first, second });
	const ClaimResolutionResult right = SquadClaimCore::Resolve(Empty(context),
		context, { second, first });
	Require(left.snapshot.canonicalFingerprint ==
		right.snapshot.canonicalFingerprint,
		"CLAIM-14 identical input has stable snapshot fingerprint");
	const ClaimResolutionResult unchanged = SquadClaimCore::Resolve(
		left.snapshot, context, {});
	Require(unchanged.status == ClaimResolutionStatus::ACCEPTED_UNCHANGED &&
		unchanged.snapshot.canonicalFingerprint ==
			left.snapshot.canonicalFingerprint,
		"CLAIM-14 identical snapshot is unchanged");
	std::cout << "CLAIM-14 PASS\n";
}

void TestInvalidClaimDoesNotMutate()
{
	const ClaimContext context = Context();
	const ClaimSnapshot current = AcquireOne(context, Key(28), 1401, 1);
	ClaimRequest invalid = Acquire(context, Key(29), 1402, 1);
	invalid.claim.leaseExpiresAt = std::numeric_limits<double>::quiet_NaN();
	const ClaimResolutionResult result = SquadClaimCore::Resolve(current, context,
		{ invalid });
	Require(result.status == ClaimResolutionStatus::REJECTED_INVALID_INPUT &&
		result.diagnostic == ClaimDiagnostic::INVALID_CLAIM_RECORD &&
		result.snapshot.canonicalFingerprint == current.canonicalFingerprint &&
		result.snapshot.claims.size() == current.claims.size(),
		"CLAIM-15 invalid non-finite claim does not mutate snapshot");
	std::cout << "CLAIM-15 PASS\n";
}

void TestIndependentResources()
{
	const ClaimContext context = Context();
	const ClaimResolutionResult result = SquadClaimCore::Resolve(Empty(context),
		context, { Acquire(context, Key(30), 1501, 1),
			Acquire(context, Key(31), 1502, 1) });
	Require(result.snapshot.claims.size() == 2 &&
		result.snapshot.claims[0].ownerMemberId !=
			result.snapshot.claims[1].ownerMemberId,
		"CLAIM-16 independent keys have independent owners");
	std::cout << "CLAIM-16 PASS\n";
}
}

int main()
{
	TestSingleAcquire();
	TestHigherPriorityWins();
	TestEqualPriorityUsesMemberId();
	TestRequestOrderIndependent();
	TestExpiredClaimDoesNotBlock();
	TestLeaseBoundary();
	TestOwnerCanRenew();
	TestNonOwnerCannotOverwriteOrRelease();
	TestStaleSquadGenerationRejected();
	TestStaleDecisionRevisionRejected();
	TestResourceGenerationSeparatesKeys();
	TestMemberLifecyclePurge();
	TestReplacementPurgesOldScope();
	TestIdenticalInputIsStable();
	TestInvalidClaimDoesNotMutate();
	TestIndependentResources();
	std::cout << "RESULT CLAIM-01..CLAIM-16 PASS\n";
	return 0;
}
