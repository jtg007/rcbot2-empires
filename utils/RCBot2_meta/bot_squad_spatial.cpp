#include "bot_squad_spatial.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace RCBotSquadSpatial
{
namespace
{
constexpr std::uint32_t kMaxSpatialSlots = 64;
constexpr double kDirectionEpsilon = 0.000001;
constexpr Fingerprint kFnvOffset = 1469598103934665603ULL;
constexpr Fingerprint kFnvPrime = 1099511628211ULL;

struct DoublePosition
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

bool IsKnownFormationType(const FormationType type)
{
	return static_cast<std::uint8_t>(type) <=
		static_cast<std::uint8_t>(FormationType::WEDGE);
}

bool IsKnownSpatialRole(const SpatialRole role)
{
	return static_cast<std::uint8_t>(role) <=
		static_cast<std::uint8_t>(SpatialRole::REAR_RIGHT);
}

bool IsKnownParticipationState(const ParticipationState state)
{
	return static_cast<std::uint8_t>(state) <=
		static_cast<std::uint8_t>(ParticipationState::EXCLUDED);
}

bool IsFinite(const TargetPosition& position)
{
	return std::isfinite(position.x) && std::isfinite(position.y) &&
		std::isfinite(position.z);
}

DoublePosition ToDouble(const TargetPosition& position)
{
	return { static_cast<double>(position.x),
		static_cast<double>(position.y), static_cast<double>(position.z) };
}

TargetPosition ToTarget(const DoublePosition& position)
{
	return { static_cast<float>(position.x), static_cast<float>(position.y),
		static_cast<float>(position.z) };
}

DoublePosition Add(const DoublePosition& left, const DoublePosition& right)
{
	return { left.x + right.x, left.y + right.y, left.z + right.z };
}

DoublePosition Scale(const DoublePosition& value, const double scalar)
{
	return { value.x * scalar, value.y * scalar, value.z * scalar };
}

double Dot(const DoublePosition& left, const DoublePosition& right)
{
	return left.x * right.x + left.y * right.y + left.z * right.z;
}

double Length(const DoublePosition& value)
{
	return std::sqrt(Dot(value, value));
}

bool IsFinite(const DoublePosition& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) &&
		std::isfinite(value.z);
}

bool NormalizeFrame(const FormationFrame& input, FormationFrame& output)
{
	if (!IsFinite(input.anchor) || !IsFinite(input.forward) ||
		!IsFinite(input.right))
		return false;

	const DoublePosition forward = ToDouble(input.forward);
	const DoublePosition right = ToDouble(input.right);
	const double forwardLength = Length(forward);
	if (!std::isfinite(forwardLength) || forwardLength <= kDirectionEpsilon)
		return false;

	const DoublePosition normalizedForward =
		Scale(forward, 1.0 / forwardLength);
	const double rightAlongForward = Dot(right, normalizedForward);
	const DoublePosition orthogonalRight = Add(right,
		Scale(normalizedForward, -rightAlongForward));
	const double rightLength = Length(orthogonalRight);
	if (!std::isfinite(rightLength) || rightLength <= kDirectionEpsilon)
		return false;

	output.anchor = input.anchor;
	output.forward = ToTarget(normalizedForward);
	output.right = ToTarget(Scale(orthogonalRight, 1.0 / rightLength));
	return IsFinite(output.forward) && IsFinite(output.right);
}

double Distance(const TargetPosition& left, const TargetPosition& right)
{
	const DoublePosition a = ToDouble(left);
	const DoublePosition b = ToDouble(right);
	const DoublePosition delta = { a.x - b.x, a.y - b.y, a.z - b.z };
	return Length(delta);
}

void MixByte(Fingerprint& hash, const std::uint8_t value)
{
	hash ^= value;
	hash *= kFnvPrime;
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

void MixDouble(Fingerprint& hash, double value)
{
	if (value == 0.0)
		value = 0.0;
	std::uint64_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value), "double must be IEEE-sized");
	std::memcpy(&bits, &value, sizeof(bits));
	MixU64(hash, bits);
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

void MixTarget(Fingerprint& hash, const TargetPosition& position)
{
	MixFloat(hash, position.x);
	MixFloat(hash, position.y);
	MixFloat(hash, position.z);
}

void MixClaimKey(Fingerprint& hash, const ClaimKey& key)
{
	MixByte(hash, static_cast<std::uint8_t>(key.resourceKind));
	MixU64(hash, key.resourceId);
	MixU64(hash, key.resourceGeneration);
}

std::uint64_t MakeSlotId(const FormationType type, const std::size_t index)
{
	return (static_cast<std::uint64_t>(static_cast<std::uint8_t>(type) + 1) <<
		32) | static_cast<std::uint64_t>(index + 1);
}

SpatialRole RoleForSlot(const FormationType type, const std::size_t index,
	const std::size_t slotCount)
{
	if (type == FormationType::COLUMN || type == FormationType::WEDGE)
	{
		if (index == 0)
			return SpatialRole::ANCHOR;
		if (type == FormationType::COLUMN)
			return SpatialRole::REAR;
		return (index % 2 == 1) ? SpatialRole::REAR_LEFT :
			SpatialRole::REAR_RIGHT;
	}

	const std::size_t centerIndex = (slotCount - 1) / 2;
	if (index == centerIndex)
		return SpatialRole::CENTER;
	return index < centerIndex ? SpatialRole::LEFT : SpatialRole::RIGHT;
}

bool MakeSlot(const FormationType type, const FormationPolicy& policy,
	const FormationFrame& frame, const Generation formationGeneration,
	const std::size_t index, const std::size_t slotCount,
	FormationSlot& slot)
{
	DoublePosition local;
	if (type == FormationType::COLUMN)
	{
		local = Scale(ToDouble(frame.forward),
			-policy.rowSpacing * static_cast<double>(index));
	}
	else if (type == FormationType::LINE)
	{
		const double center = static_cast<double>(slotCount - 1) / 2.0;
		local = Scale(ToDouble(frame.right),
			(static_cast<double>(index) - center) * policy.memberSpacing);
	}
	else
	{
		if (index != 0)
		{
			const std::size_t row = (index + 1) / 2;
			const double side = index % 2 == 1 ? -1.0 : 1.0;
			const DoublePosition rear = Scale(ToDouble(frame.forward),
				-policy.rowSpacing * static_cast<double>(row));
			const DoublePosition lateral = Scale(ToDouble(frame.right),
				side * policy.memberSpacing * static_cast<double>(row));
			local = Add(rear, lateral);
		}
	}

	const DoublePosition world = Add(ToDouble(frame.anchor), local);
	if (!IsFinite(local) || !IsFinite(world))
		return false;
	const TargetPosition localTarget = ToTarget(local);
	const TargetPosition worldTarget = ToTarget(world);
	if (!IsFinite(localTarget) || !IsFinite(worldTarget))
		return false;

	slot.slotId = MakeSlotId(type, index);
	slot.role = RoleForSlot(type, index, slotCount);
	slot.localOffset = localTarget;
	slot.worldPosition = worldTarget;
	slot.claimKey = { RCBotSquadClaims::ResourceKind::SPATIAL_RESOURCE,
		slot.slotId, formationGeneration };
	return slot.IsValid(formationGeneration);
}

bool ValidateDecision(const SquadDecisionResult& decision)
{
	if (!decision.Accepted() || !decision.identity.IsValid() ||
		decision.decisionRevision == 0 || !decision.cohesion.valid ||
		decision.canonicalMembers.size() != decision.participation.size())
		return false;
	for (std::size_t i = 0; i < decision.canonicalMembers.size(); ++i)
	{
		const MemberState& member = decision.canonicalMembers[i];
		const ParticipationDecision& participation = decision.participation[i];
		if (!member.IsValid() || participation.memberId != member.memberId ||
			!IsKnownParticipationState(participation.state) ||
			!std::isfinite(participation.distanceToCentroid) ||
			!std::isfinite(participation.distanceToObjective))
			return false;
		if (i > 0 && decision.canonicalMembers[i - 1].memberId >=
			member.memberId)
			return false;
	}
	return true;
}

struct Participant
{
	RecipientId memberId = 0;
	ParticipationState state = ParticipationState::EXCLUDED;
	TargetPosition origin;
};

std::vector<Participant> ParticipantsOf(const SquadDecisionResult& decision)
{
	std::vector<Participant> participants;
	for (std::size_t i = 0; i < decision.canonicalMembers.size(); ++i)
	{
		const ParticipationDecision& participation = decision.participation[i];
		if (participation.state == ParticipationState::PARTICIPATING ||
			participation.state == ParticipationState::CATCHING_UP)
		{
			participants.push_back({ decision.canonicalMembers[i].memberId,
				participation.state, decision.canonicalMembers[i].origin });
		}
	}
	return participants;
}

ClaimContext ClaimContextOf(const SpatialFormationInput& input)
{
	ClaimContext context;
	context.scope.squadGeneration = input.decision.identity.squadGeneration;
	context.scope.decisionRevision = input.decision.decisionRevision;
	context.scope.hasParentOrder = input.decision.commanderOrder.present;
	if (context.scope.hasParentOrder)
		context.scope.parentOrder = input.decision.commanderOrder.parentOrder;
	context.now = input.now;
	return context;
}

bool IsParticipant(const std::vector<Participant>& participants,
	const RecipientId memberId)
{
	for (const Participant& participant : participants)
	{
		if (participant.memberId == memberId)
			return true;
	}
	return false;
}

void AddPurgeOwner(std::vector<RecipientId>& owners,
	const RecipientId memberId)
{
	if (std::find(owners.begin(), owners.end(), memberId) == owners.end())
		owners.push_back(memberId);
}

int FindClaimOwner(const ClaimSnapshot& snapshot, const ClaimKey& key)
{
	for (std::size_t i = 0; i < snapshot.claims.size(); ++i)
	{
		if (snapshot.claims[i].key == key)
			return static_cast<int>(i);
	}
	return -1;
}

void MixSlot(Fingerprint& hash, const FormationSlot& slot)
{
	MixU64(hash, slot.slotId);
	MixByte(hash, static_cast<std::uint8_t>(slot.role));
	MixTarget(hash, slot.localOffset);
	MixTarget(hash, slot.worldPosition);
	MixClaimKey(hash, slot.claimKey);
}

void MixAssignment(Fingerprint& hash, const FormationAssignment& assignment)
{
	MixU64(hash, assignment.memberId);
	MixU64(hash, assignment.slotId);
	MixByte(hash, static_cast<std::uint8_t>(assignment.role));
	MixByte(hash, static_cast<std::uint8_t>(assignment.participation));
	MixTarget(hash, assignment.targetPosition);
	MixClaimKey(hash, assignment.claimKey);
	MixByte(hash, static_cast<std::uint8_t>(assignment.claimStatus));
	MixDouble(hash, assignment.geometricCost);
}

Fingerprint FingerprintOf(const FormationDecisionResult& result)
{
	Fingerprint hash = kFnvOffset;
	MixByte(hash, static_cast<std::uint8_t>(result.formationType));
	MixU64(hash, result.formationGeneration);
	MixTarget(hash, result.frame.anchor);
	MixTarget(hash, result.frame.forward);
	MixTarget(hash, result.frame.right);
	MixDouble(hash, result.policy.memberSpacing);
	MixDouble(hash, result.policy.rowSpacing);
	MixDouble(hash, result.policy.slotLeaseDuration);
	MixU32(hash, result.policy.maxSlots);
	MixU64(hash, static_cast<std::uint64_t>(result.slots.size()));
	for (const FormationSlot& slot : result.slots)
		MixSlot(hash, slot);
	MixU64(hash, static_cast<std::uint64_t>(result.assignments.size()));
	for (const FormationAssignment& assignment : result.assignments)
		MixAssignment(hash, assignment);
	MixU64(hash, result.claimSnapshot.canonicalFingerprint);
	MixByte(hash, static_cast<std::uint8_t>(result.claimResolutionStatus));
	MixByte(hash, static_cast<std::uint8_t>(result.claimDiagnostic));
	return hash;
}

FormationDecisionResult Reject(const SpatialFormationInput& input,
	const FormationDiagnostic diagnostic)
{
	FormationDecisionResult result;
	result.formationType = input.formationType;
	result.formationGeneration = input.formationGeneration;
	result.policy = input.policy;
	result.diagnostic = diagnostic;
	return result;
}

FormationDecisionResult RejectClaims(const SpatialFormationInput& input,
	const ClaimResolutionResult& claimResult)
{
	FormationDecisionResult result = Reject(input,
		FormationDiagnostic::CLAIM_RESOLUTION_FAILED);
	result.status = FormationStatus::REJECTED_CLAIMS;
	result.claimResolutionStatus = claimResult.status;
	result.claimDiagnostic = claimResult.diagnostic;
	result.claimSnapshot = claimResult.snapshot;
	return result;
}
}

bool FormationFrame::IsValid() const
{
	FormationFrame canonical;
	return NormalizeFrame(*this, canonical);
}

bool FormationPolicy::IsValid() const
{
	return std::isfinite(memberSpacing) && memberSpacing > 0.0 &&
		std::isfinite(rowSpacing) && rowSpacing > 0.0 &&
		std::isfinite(slotLeaseDuration) && slotLeaseDuration > 0.0 &&
		maxSlots > 0 && maxSlots <= kMaxSpatialSlots;
}

bool FormationSlot::IsValid(const Generation formationGeneration) const
{
	return slotId != 0 && IsKnownSpatialRole(role) && IsFinite(localOffset) &&
		IsFinite(worldPosition) && claimKey.IsValid() &&
		claimKey.resourceKind == RCBotSquadClaims::ResourceKind::SPATIAL_RESOURCE &&
		claimKey.resourceId == slotId &&
		claimKey.resourceGeneration == formationGeneration;
}

FormationDecisionResult SquadSpatialCore::Evaluate(
	const SpatialFormationInput& input)
{
	if (!ValidateDecision(input.decision))
		return Reject(input, FormationDiagnostic::INVALID_DECISION);
	if (!IsKnownFormationType(input.formationType))
		return Reject(input, FormationDiagnostic::INVALID_FORMATION_TYPE);
	if (!input.policy.IsValid())
		return Reject(input, FormationDiagnostic::INVALID_POLICY);
	if (input.formationGeneration == 0)
		return Reject(input, FormationDiagnostic::INVALID_FORMATION_GENERATION);
	if (!std::isfinite(input.now))
		return Reject(input, FormationDiagnostic::INVALID_FRAME);

	FormationFrame frame;
	if (!NormalizeFrame(input.frame, frame))
		return Reject(input, FormationDiagnostic::INVALID_FRAME);

	const std::vector<Participant> participants = ParticipantsOf(input.decision);
	FormationDecisionResult result;
	result.status = FormationStatus::ACCEPTED;
	result.diagnostic = FormationDiagnostic::NONE;
	result.formationType = input.formationType;
	result.formationGeneration = input.formationGeneration;
	result.frame = frame;
	result.policy = input.policy;

	const std::size_t slotCount = participants.empty() ? 0 :
		static_cast<std::size_t>(input.policy.maxSlots);
	result.slots.reserve(slotCount);
	for (std::size_t i = 0; i < slotCount; ++i)
	{
		FormationSlot slot;
		if (!MakeSlot(input.formationType, input.policy, frame,
			input.formationGeneration, i, slotCount, slot))
			return Reject(input, FormationDiagnostic::NON_FINITE_GEOMETRY);
		result.slots.push_back(slot);
	}

	const ClaimContext claimContext = ClaimContextOf(input);
	ClaimSnapshot claims;
	if (input.hasExistingClaims)
		claims = input.existingClaims;
	else
		claims = SquadClaimCore::EmptySnapshot(claimContext);
	if (!claims.IsValid())
	{
		ClaimResolutionResult invalid;
		invalid.status = ClaimResolutionStatus::REJECTED_INVALID_INPUT;
		invalid.diagnostic = ClaimDiagnostic::INVALID_SNAPSHOT;
		invalid.snapshot = claims;
		return RejectClaims(input, invalid);
	}

	ClaimResolutionResult claimResult = SquadClaimCore::PurgeStale(claims,
		claimContext);
	if (!claimResult.Accepted())
		return RejectClaims(input, claimResult);
	claims = claimResult.snapshot;
	claimResult = SquadClaimCore::PurgeResourceGeneration(claims,
		claimContext, RCBotSquadClaims::ResourceKind::SPATIAL_RESOURCE,
		input.formationGeneration);
	if (!claimResult.Accepted())
		return RejectClaims(input, claimResult);
	claims = claimResult.snapshot;

	std::vector<RecipientId> purgeOwners;
	for (const ClaimRecord& claim : claims.claims)
	{
		if (claim.key.resourceKind ==
			RCBotSquadClaims::ResourceKind::SPATIAL_RESOURCE &&
			!IsParticipant(participants, claim.ownerMemberId))
			AddPurgeOwner(purgeOwners, claim.ownerMemberId);
	}
	for (const RecipientId memberId : purgeOwners)
	{
		claimResult = SquadClaimCore::PurgeMemberClaimsOfKind(claims,
			claimContext, memberId,
			RCBotSquadClaims::ResourceKind::SPATIAL_RESOURCE);
		if (!claimResult.Accepted())
			return RejectClaims(input, claimResult);
		claims = claimResult.snapshot;
	}

	struct ProposedAssignment
	{
		std::size_t participantIndex = 0;
		std::size_t slotIndex = 0;
		double cost = 0.0;
	};
	std::vector<bool> usedParticipants(participants.size(), false);
	std::vector<ProposedAssignment> proposed;
	proposed.reserve(std::min(participants.size(), result.slots.size()));
	for (std::size_t slotIndex = 0; slotIndex < result.slots.size(); ++slotIndex)
	{
		std::size_t bestParticipant = participants.size();
		double bestCost = std::numeric_limits<double>::infinity();
		for (std::size_t participantIndex = 0;
			participantIndex < participants.size(); ++participantIndex)
		{
			if (usedParticipants[participantIndex])
				continue;
			const double cost = Distance(participants[participantIndex].origin,
				result.slots[slotIndex].worldPosition);
			if (!std::isfinite(cost))
				return Reject(input, FormationDiagnostic::NON_FINITE_GEOMETRY);
			if (bestParticipant == participants.size() || cost < bestCost ||
				(cost == bestCost && participants[participantIndex].memberId <
					participants[bestParticipant].memberId))
			{
				bestParticipant = participantIndex;
				bestCost = cost;
			}
		}
		if (bestParticipant != participants.size())
		{
			usedParticipants[bestParticipant] = true;
			proposed.push_back({ bestParticipant, slotIndex, bestCost });
		}
	}

	std::vector<ClaimRequest> requests;
	requests.reserve(proposed.size());
	for (const ProposedAssignment& assignment : proposed)
	{
		ClaimRequest request;
		request.operation = ClaimOperation::ACQUIRE;
		request.claim.key = result.slots[assignment.slotIndex].claimKey;
		request.claim.ownerMemberId =
			participants[assignment.participantIndex].memberId;
		request.claim.squadGeneration = claimContext.scope.squadGeneration;
		request.claim.decisionRevision = claimContext.scope.decisionRevision;
		request.claim.priority = 0;
		request.claim.issuedAt = input.now;
		request.claim.leaseExpiresAt =
			input.now + input.policy.slotLeaseDuration;
		request.claim.hasParentOrder = claimContext.scope.hasParentOrder;
		request.claim.parentOrder = claimContext.scope.parentOrder;
		if (!request.claim.IsValid())
			return Reject(input, FormationDiagnostic::NON_FINITE_GEOMETRY);
		requests.push_back(request);
	}

	claimResult = SquadClaimCore::Resolve(claims, claimContext, requests);
	result.claimResolutionStatus = claimResult.status;
	result.claimDiagnostic = claimResult.diagnostic;
	result.claimSnapshot = claimResult.snapshot;
	if (!claimResult.Accepted())
		return RejectClaims(input, claimResult);

	for (const ProposedAssignment& assignment : proposed)
	{
		const FormationSlot& slot = result.slots[assignment.slotIndex];
		const Participant& participant =
			participants[assignment.participantIndex];
		FormationAssignment output;
		output.memberId = participant.memberId;
		output.slotId = slot.slotId;
		output.role = slot.role;
		output.participation = participant.state;
		output.targetPosition = slot.worldPosition;
		output.claimKey = slot.claimKey;
		output.geometricCost = assignment.cost;
		const int claimIndex = FindClaimOwner(result.claimSnapshot,
			slot.claimKey);
		if (claimIndex >= 0 && result.claimSnapshot.claims[
			static_cast<std::size_t>(claimIndex)].ownerMemberId == participant.memberId)
			output.claimStatus = SpatialClaimStatus::CLAIMED;
		else
			output.claimStatus = SpatialClaimStatus::UNAVAILABLE;
		result.assignments.push_back(output);
	}

	result.canonicalFingerprint = FingerprintOf(result);
	return result;
}

const char* FormationTypeName(const FormationType type)
{
	switch (type)
	{
	case FormationType::COLUMN: return "COLUMN";
	case FormationType::LINE: return "LINE";
	case FormationType::WEDGE: return "WEDGE";
	}
	return "UNKNOWN";
}

const char* SpatialRoleName(const SpatialRole role)
{
	switch (role)
	{
	case SpatialRole::ANCHOR: return "ANCHOR";
	case SpatialRole::CENTER: return "CENTER";
	case SpatialRole::LEFT: return "LEFT";
	case SpatialRole::RIGHT: return "RIGHT";
	case SpatialRole::REAR: return "REAR";
	case SpatialRole::REAR_LEFT: return "REAR_LEFT";
	case SpatialRole::REAR_RIGHT: return "REAR_RIGHT";
	}
	return "UNKNOWN";
}

const char* SpatialClaimStatusName(const SpatialClaimStatus status)
{
	switch (status)
	{
	case SpatialClaimStatus::CLAIMED: return "CLAIMED";
	case SpatialClaimStatus::UNAVAILABLE: return "UNAVAILABLE";
	}
	return "UNKNOWN";
}

const char* FormationDiagnosticName(const FormationDiagnostic diagnostic)
{
	switch (diagnostic)
	{
	case FormationDiagnostic::NONE: return "NONE";
	case FormationDiagnostic::INVALID_DECISION: return "INVALID_DECISION";
	case FormationDiagnostic::INVALID_FRAME: return "INVALID_FRAME";
	case FormationDiagnostic::INVALID_POLICY: return "INVALID_POLICY";
	case FormationDiagnostic::INVALID_FORMATION_GENERATION: return "INVALID_FORMATION_GENERATION";
	case FormationDiagnostic::INVALID_FORMATION_TYPE: return "INVALID_FORMATION_TYPE";
	case FormationDiagnostic::NON_FINITE_GEOMETRY: return "NON_FINITE_GEOMETRY";
	case FormationDiagnostic::CLAIM_RESOLUTION_FAILED: return "CLAIM_RESOLUTION_FAILED";
	}
	return "UNKNOWN";
}

const char* FormationStatusName(const FormationStatus status)
{
	switch (status)
	{
	case FormationStatus::ACCEPTED: return "ACCEPTED";
	case FormationStatus::REJECTED_INVALID_INPUT: return "REJECTED_INVALID_INPUT";
	case FormationStatus::REJECTED_CLAIMS: return "REJECTED_CLAIMS";
	}
	return "UNKNOWN";
}
}
