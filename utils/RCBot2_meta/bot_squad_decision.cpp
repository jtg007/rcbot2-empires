#include "bot_squad_decision.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RCBotSquadDecision
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
	// Explicit little-endian byte order makes the fingerprint independent of
	// host endianness and avoids using std::hash as a protocol identity.
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

void MixFloat(Fingerprint& hash, float value)
{
	if (value == 0.0f)
		value = 0.0f;
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value), "float must be IEEE-sized");
	std::memcpy(&bits, &value, sizeof(bits));
	MixU32(hash, bits);
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

bool IsFinite(const TargetPosition& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) &&
		std::isfinite(value.z);
}

TargetPosition NormalizeZeroes(TargetPosition value)
{
	if (value.x == 0.0f)
		value.x = 0.0f;
	if (value.y == 0.0f)
		value.y = 0.0f;
	if (value.z == 0.0f)
		value.z = 0.0f;
	return value;
}

bool IsKnownOrderState(const OrderState state)
{
	return static_cast<std::uint8_t>(state) <=
		static_cast<std::uint8_t>(OrderState::TERMINAL);
}

bool IsKnownLifecycle(const MemberLifecycle lifecycle)
{
	return static_cast<std::uint8_t>(lifecycle) <=
		static_cast<std::uint8_t>(MemberLifecycle::DISCONNECTED);
}

void MixTarget(Fingerprint& hash, const TargetPosition& value)
{
	MixFloat(hash, value.x);
	MixFloat(hash, value.y);
	MixFloat(hash, value.z);
}

void MixIdentity(Fingerprint& hash, const SquadIdentity& identity)
{
	MixI32(hash, identity.team);
	MixU64(hash, identity.squadId);
	MixU64(hash, identity.squadGeneration);
}

void MixOrder(Fingerprint& hash, const CommanderOrderRef& order)
{
	MixBool(hash, order.present);
	if (!order.present)
		return;
	MixU64(hash, order.parentOrder.orderId);
	MixU64(hash, order.parentOrder.generation);
	MixTarget(hash, order.target);
	MixDouble(hash, order.deadline);
	MixByte(hash, static_cast<std::uint8_t>(order.orderState));
}

void MixParticipationPolicy(Fingerprint& hash,
	const SquadParticipationPolicy& policy)
{
	MixDouble(hash, policy.cohesionRadius);
}

void MixMember(Fingerprint& hash, const MemberState& member)
{
	MixU64(hash, member.memberId);
	MixBool(hash, member.alive);
	MixBool(hash, member.connected);
	MixBool(hash, member.eligible);
	MixBool(hash, member.isLeader);
	MixI32(hash, member.squadPosition);
	MixTarget(hash, member.origin);
	MixTarget(hash, member.velocity);
	MixByte(hash, static_cast<std::uint8_t>(member.lifecycle));
}

Fingerprint FingerprintOf(const SquadIdentity& identity,
	const CommanderOrderRef& order, const SquadParticipationPolicy& policy,
	const std::vector<MemberState>& members)
{
	Fingerprint hash = kFnvOffset;
	MixIdentity(hash, identity);
	MixOrder(hash, order);
	MixParticipationPolicy(hash, policy);
	MixU64(hash, static_cast<std::uint64_t>(members.size()));
	for (const MemberState& member : members)
		MixMember(hash, member);
	return hash;
}

bool SameTeamAndSquad(const SquadIdentity& left, const SquadIdentity& right)
{
	return left.team == right.team && left.squadId == right.squadId;
}

void CopyCanonical(SquadDecisionResult& result,
	const SquadCanonicalSnapshot& canonical)
{
	result.identity = canonical.identity;
	result.commanderOrder = canonical.commanderOrder;
	result.participationPolicy = canonical.participationPolicy;
	result.canonicalMembers = canonical.members;
	result.eligibleMembers = canonical.eligibleMembers;
	result.leaderId = canonical.leaderId;
	result.canonicalFingerprint = canonical.canonicalFingerprint;
}

struct DoublePosition
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

DoublePosition ToDouble(const TargetPosition& position)
{
	return { static_cast<double>(position.x),
		static_cast<double>(position.y), static_cast<double>(position.z) };
}

bool IsFinite(const DoublePosition& position)
{
	return std::isfinite(position.x) && std::isfinite(position.y) &&
		std::isfinite(position.z);
}

double Distance(const DoublePosition& left, const DoublePosition& right)
{
	const double dx = left.x - right.x;
	const double dy = left.y - right.y;
	const double dz = left.z - right.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool StorePosition(const DoublePosition& source, TargetPosition& target)
{
	if (!IsFinite(source))
		return false;
	target = { static_cast<float>(source.x), static_cast<float>(source.y),
		static_cast<float>(source.z) };
	return IsFinite(target);
}

bool ComputeCohesion(const SquadCanonicalSnapshot& canonical,
	CohesionFacts& cohesion, std::vector<ParticipationDecision>& participation)
{
	CohesionFacts facts;
	facts.objectivePresent = canonical.commanderOrder.present;
	facts.eligibleMemberCount =
		static_cast<std::uint64_t>(canonical.eligibleMembers.size());
	facts.memberFacts.reserve(canonical.members.size());
	participation.clear();
	participation.reserve(canonical.members.size());

	DoublePosition centroid;
	TargetPosition boundsMin;
	TargetPosition boundsMax;
	bool haveEligible = false;
	for (const MemberState& member : canonical.members)
	{
		if (!member.IsEligible())
			continue;

		const DoublePosition position = ToDouble(member.origin);
		centroid.x += position.x;
		centroid.y += position.y;
		centroid.z += position.z;
		if (!haveEligible)
		{
			boundsMin = member.origin;
			boundsMax = member.origin;
			haveEligible = true;
		}
		else
		{
			boundsMin.x = (std::min)(boundsMin.x, member.origin.x);
			boundsMin.y = (std::min)(boundsMin.y, member.origin.y);
			boundsMin.z = (std::min)(boundsMin.z, member.origin.z);
			boundsMax.x = (std::max)(boundsMax.x, member.origin.x);
			boundsMax.y = (std::max)(boundsMax.y, member.origin.y);
			boundsMax.z = (std::max)(boundsMax.z, member.origin.z);
		}
	}

	if (haveEligible)
	{
		const double count =
			static_cast<double>(facts.eligibleMemberCount);
		centroid.x /= count;
		centroid.y /= count;
		centroid.z /= count;
		if (!StorePosition(centroid, facts.centroid))
			return false;
		facts.boundsMin = boundsMin;
		facts.boundsMax = boundsMax;
	}

	const DoublePosition objective =
		ToDouble(canonical.commanderOrder.target);
	double distanceSum = 0.0;
	double minDistance = 0.0;
	double maxDistance = 0.0;
	bool haveDistance = false;
	for (const MemberState& member : canonical.members)
	{
		double distanceToCentroid = 0.0;
		if (haveEligible)
			distanceToCentroid = Distance(ToDouble(member.origin), centroid);

		double distanceToObjective = 0.0;
		if (canonical.commanderOrder.present)
			distanceToObjective = Distance(ToDouble(member.origin), objective);
		if (!std::isfinite(distanceToCentroid) ||
			!std::isfinite(distanceToObjective))
			return false;

		facts.memberFacts.push_back(
			{ member.memberId, distanceToCentroid, distanceToObjective });
		ParticipationDecision decision;
		decision.memberId = member.memberId;
		decision.distanceToCentroid = distanceToCentroid;
		decision.distanceToObjective = distanceToObjective;
		if (!member.IsEligible())
			decision.state = ParticipationState::EXCLUDED;
		else if (distanceToCentroid <=
			canonical.participationPolicy.cohesionRadius)
			decision.state = ParticipationState::PARTICIPATING;
		else
			decision.state = ParticipationState::CATCHING_UP;
		participation.push_back(decision);

		if (!member.IsEligible())
			continue;
		if (!haveDistance)
		{
			minDistance = distanceToCentroid;
			maxDistance = distanceToCentroid;
			haveDistance = true;
		}
		else
		{
			minDistance = (std::min)(minDistance, distanceToCentroid);
			maxDistance = (std::max)(maxDistance, distanceToCentroid);
		}
		distanceSum += distanceToCentroid;
	}

	if (haveEligible)
	{
		facts.minDistanceFromCentroid = minDistance;
		facts.maxDistanceFromCentroid = maxDistance;
		facts.averageDistanceFromCentroid =
			distanceSum / static_cast<double>(facts.eligibleMemberCount);
		facts.distanceSpread = maxDistance - minDistance;
		if (canonical.commanderOrder.present)
			facts.objectiveDistanceFromCentroid = Distance(centroid, objective);
	}

	if (!std::isfinite(facts.minDistanceFromCentroid) ||
		!std::isfinite(facts.maxDistanceFromCentroid) ||
		!std::isfinite(facts.averageDistanceFromCentroid) ||
		!std::isfinite(facts.distanceSpread) ||
		!std::isfinite(facts.objectiveDistanceFromCentroid))
		return false;
	facts.valid = true;
	cohesion = facts;
	return true;
}
}

bool SquadIdentity::IsValid() const
{
	return team != 0 && squadId != 0 && squadGeneration != 0;
}

bool CommanderOrderRef::IsValid() const
{
	if (!present)
		return true;
	return parentOrder.orderId != 0 && parentOrder.generation != 0 &&
		target.IsFinite() && std::isfinite(deadline) && deadline >= 0.0 &&
		IsKnownOrderState(orderState);
}

bool MemberState::IsValid() const
{
	return memberId != 0 && IsFinite(origin) && IsFinite(velocity) &&
		IsKnownLifecycle(lifecycle);
}

bool MemberState::IsEligible() const
{
	return alive && connected && eligible &&
		lifecycle != MemberLifecycle::LEFT &&
		lifecycle != MemberLifecycle::DEAD &&
		lifecycle != MemberLifecycle::DISCONNECTED;
}

bool SquadParticipationPolicy::IsValid() const
{
	return std::isfinite(cohesionRadius) && cohesionRadius >= 0.0;
}

bool SquadDecisionRevisionContext::IsValid() const
{
	if (!hasPrevious)
		return true;
	return identity.IsValid() && decisionRevision != 0 &&
		canonicalFingerprint != 0;
}

SquadCanonicalSnapshot SquadDecisionCore::Canonicalize(
	const SquadDecisionInput& input)
{
	SquadCanonicalSnapshot result;
	result.identity = input.identity;
	if (!input.identity.IsValid())
	{
		result.diagnostic = DecisionDiagnostic::INVALID_SQUAD_IDENTITY;
		return result;
	}

	if (!input.commanderOrder.IsValid())
	{
		result.diagnostic = input.commanderOrder.present &&
			!input.commanderOrder.target.IsFinite() ?
			DecisionDiagnostic::NON_FINITE_ORDER_FACT :
			DecisionDiagnostic::INVALID_PARENT_ORDER;
		return result;
	}

	if (!input.participationPolicy.IsValid())
	{
		result.diagnostic = DecisionDiagnostic::INVALID_PARTICIPATION_POLICY;
		return result;
	}

	if (input.members.empty())
	{
		result.diagnostic = DecisionDiagnostic::EMPTY_MEMBERSHIP;
		return result;
	}

	result.commanderOrder = input.commanderOrder;
	result.participationPolicy = input.participationPolicy;
	if (result.participationPolicy.cohesionRadius == 0.0)
		result.participationPolicy.cohesionRadius = 0.0;
	if (!result.commanderOrder.present)
		result.commanderOrder = CommanderOrderRef();
	else
		result.commanderOrder.target = NormalizeZeroes(result.commanderOrder.target);

	result.members = input.members;
	std::sort(result.members.begin(), result.members.end(),
		[](const MemberState& left, const MemberState& right) {
			return left.memberId < right.memberId;
		});

	for (std::size_t i = 0; i < result.members.size(); ++i)
	{
		MemberState& member = result.members[i];
		if (member.memberId == 0)
		{
			result.diagnostic = DecisionDiagnostic::INVALID_MEMBER_ID;
			result.members.clear();
			return result;
		}
		if (i > 0 && result.members[i - 1].memberId == member.memberId)
		{
			result.diagnostic = DecisionDiagnostic::DUPLICATE_MEMBER_ID;
			result.members.clear();
			return result;
		}
		if (!IsKnownLifecycle(member.lifecycle))
		{
			result.diagnostic = DecisionDiagnostic::INVALID_MEMBER_LIFECYCLE;
			result.members.clear();
			return result;
		}
		if (!IsFinite(member.origin) || !IsFinite(member.velocity))
		{
			result.diagnostic = DecisionDiagnostic::NON_FINITE_MEMBER_FACT;
			result.members.clear();
			return result;
		}
		member.origin = NormalizeZeroes(member.origin);
		member.velocity = NormalizeZeroes(member.velocity);
		if (member.isLeader)
		{
			if (result.leaderId != 0)
			{
				result.diagnostic = DecisionDiagnostic::MULTIPLE_LEADERS;
				result.members.clear();
				return result;
			}
			result.leaderId = member.memberId;
		}
		if (member.IsEligible())
			result.eligibleMembers.push_back(member.memberId);
	}

	result.canonicalFingerprint = FingerprintOf(result.identity,
		result.commanderOrder, result.participationPolicy, result.members);
	result.valid = true;
	return result;
}

SquadDecisionResult SquadDecisionCore::Evaluate(
	const SquadDecisionInput& input,
	const SquadDecisionRevisionContext& previous)
{
	SquadDecisionResult result;
	result.identity = input.identity;
	result.decisionRevision = input.decisionRevision;

	const SquadCanonicalSnapshot canonical = Canonicalize(input);
	if (!canonical.valid)
	{
		result.status = DecisionStatus::REJECTED_INVALID_INPUT;
		result.diagnostic = canonical.diagnostic;
		return result;
	}
	if (input.decisionRevision == 0)
	{
		result.status = DecisionStatus::REJECTED_INVALID_INPUT;
		result.diagnostic = DecisionDiagnostic::INVALID_DECISION_REVISION;
		return result;
	}
	if (!previous.IsValid())
	{
		result.status = DecisionStatus::REJECTED_INVALID_CONTEXT;
		result.diagnostic = DecisionDiagnostic::INVALID_PREVIOUS_CONTEXT;
		return result;
	}

	if (previous.hasPrevious)
	{
		if (!SameTeamAndSquad(input.identity, previous.identity))
		{
			result.status = DecisionStatus::REJECTED_STALE_SQUAD;
			result.diagnostic = DecisionDiagnostic::STALE_SQUAD_IDENTITY;
			return result;
		}
		if (input.identity.squadGeneration != previous.identity.squadGeneration)
		{
			result.status = DecisionStatus::REJECTED_STALE_SQUAD;
			result.diagnostic = DecisionDiagnostic::STALE_SQUAD_GENERATION;
			return result;
		}
		if (input.decisionRevision < previous.decisionRevision)
		{
			result.status = DecisionStatus::REJECTED_STALE_REVISION;
			result.diagnostic = DecisionDiagnostic::STALE_DECISION_REVISION;
			return result;
		}

		const bool inputChanged = canonical.canonicalFingerprint !=
			previous.canonicalFingerprint;
		if (!inputChanged && input.decisionRevision != previous.decisionRevision)
		{
			result.status = DecisionStatus::REJECTED_REVISION_WITHOUT_CHANGE;
			result.diagnostic = DecisionDiagnostic::REVISION_WITHOUT_INPUT_CHANGE;
			return result;
		}
		if (inputChanged && input.decisionRevision <= previous.decisionRevision)
		{
			result.status = DecisionStatus::REJECTED_STALE_REVISION;
			result.diagnostic = DecisionDiagnostic::STALE_DECISION_REVISION;
			return result;
		}

		result.status = inputChanged ? DecisionStatus::ACCEPTED_NEW :
			DecisionStatus::ACCEPTED_UNCHANGED;
		result.changed = inputChanged;
	}
	else
	{
		result.status = DecisionStatus::ACCEPTED_NEW;
		result.changed = true;
	}

	CohesionFacts cohesion;
	std::vector<ParticipationDecision> participation;
	if (!ComputeCohesion(canonical, cohesion, participation))
	{
		result.status = DecisionStatus::REJECTED_INVALID_INPUT;
		result.diagnostic = DecisionDiagnostic::NON_FINITE_COHESION_FACT;
		return result;
	}

	CopyCanonical(result, canonical);
	result.cohesion = cohesion;
	result.participation = participation;
	return result;
}

const char* DecisionDiagnosticName(const DecisionDiagnostic diagnostic)
{
	switch (diagnostic)
	{
	case DecisionDiagnostic::NONE: return "NONE";
	case DecisionDiagnostic::INVALID_SQUAD_IDENTITY: return "INVALID_SQUAD_IDENTITY";
	case DecisionDiagnostic::EMPTY_MEMBERSHIP: return "EMPTY_MEMBERSHIP";
	case DecisionDiagnostic::INVALID_PARENT_ORDER: return "INVALID_PARENT_ORDER";
	case DecisionDiagnostic::INVALID_DECISION_REVISION: return "INVALID_DECISION_REVISION";
	case DecisionDiagnostic::INVALID_PREVIOUS_CONTEXT: return "INVALID_PREVIOUS_CONTEXT";
	case DecisionDiagnostic::INVALID_MEMBER_ID: return "INVALID_MEMBER_ID";
	case DecisionDiagnostic::DUPLICATE_MEMBER_ID: return "DUPLICATE_MEMBER_ID";
	case DecisionDiagnostic::INVALID_MEMBER_LIFECYCLE: return "INVALID_MEMBER_LIFECYCLE";
	case DecisionDiagnostic::NON_FINITE_MEMBER_FACT: return "NON_FINITE_MEMBER_FACT";
	case DecisionDiagnostic::NON_FINITE_ORDER_FACT: return "NON_FINITE_ORDER_FACT";
	case DecisionDiagnostic::MULTIPLE_LEADERS: return "MULTIPLE_LEADERS";
	case DecisionDiagnostic::INVALID_PARTICIPATION_POLICY: return "INVALID_PARTICIPATION_POLICY";
	case DecisionDiagnostic::NON_FINITE_COHESION_FACT: return "NON_FINITE_COHESION_FACT";
	case DecisionDiagnostic::STALE_SQUAD_IDENTITY: return "STALE_SQUAD_IDENTITY";
	case DecisionDiagnostic::STALE_SQUAD_GENERATION: return "STALE_SQUAD_GENERATION";
	case DecisionDiagnostic::STALE_DECISION_REVISION: return "STALE_DECISION_REVISION";
	case DecisionDiagnostic::REVISION_WITHOUT_INPUT_CHANGE: return "REVISION_WITHOUT_INPUT_CHANGE";
	}
	return "UNKNOWN";
}

const char* DecisionStatusName(const DecisionStatus status)
{
	switch (status)
	{
	case DecisionStatus::ACCEPTED_NEW: return "ACCEPTED_NEW";
	case DecisionStatus::ACCEPTED_UNCHANGED: return "ACCEPTED_UNCHANGED";
	case DecisionStatus::REJECTED_INVALID_INPUT: return "REJECTED_INVALID_INPUT";
	case DecisionStatus::REJECTED_INVALID_CONTEXT: return "REJECTED_INVALID_CONTEXT";
	case DecisionStatus::REJECTED_STALE_SQUAD: return "REJECTED_STALE_SQUAD";
	case DecisionStatus::REJECTED_STALE_REVISION: return "REJECTED_STALE_REVISION";
	case DecisionStatus::REJECTED_REVISION_WITHOUT_CHANGE: return "REJECTED_REVISION_WITHOUT_CHANGE";
	}
	return "UNKNOWN";
}

const char* MemberLifecycleName(const MemberLifecycle lifecycle)
{
	switch (lifecycle)
	{
	case MemberLifecycle::ACTIVE: return "ACTIVE";
	case MemberLifecycle::JOINED: return "JOINED";
	case MemberLifecycle::LEFT: return "LEFT";
	case MemberLifecycle::DEAD: return "DEAD";
	case MemberLifecycle::DISCONNECTED: return "DISCONNECTED";
	}
	return "UNKNOWN";
}

const char* ParticipationStateName(const ParticipationState state)
{
	switch (state)
	{
	case ParticipationState::PARTICIPATING: return "PARTICIPATING";
	case ParticipationState::CATCHING_UP: return "CATCHING_UP";
	case ParticipationState::EXCLUDED: return "EXCLUDED";
	}
	return "UNKNOWN";
}
}
