#include "bot_empires_squad_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RCBotEmpiresSquadRuntime
{
namespace
{
	constexpr double kGeometryEpsilon = 0.000001;
	constexpr double kSemanticMemberMove = 32.0;
	constexpr double kSemanticTargetMove = 8.0;
	constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
	constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

	bool SamePosition(const TargetPosition& left, const TargetPosition& right,
		const double tolerance)
	{
		const double dx = static_cast<double>(left.x) - right.x;
		const double dy = static_cast<double>(left.y) - right.y;
		const double dz = static_cast<double>(left.z) - right.z;
		return dx * dx + dy * dy + dz * dz <= tolerance * tolerance;
	}

	double Distance(const TargetPosition& left, const TargetPosition& right)
	{
		const double dx = static_cast<double>(left.x) - right.x;
		const double dy = static_cast<double>(left.y) - right.y;
		const double dz = static_cast<double>(left.z) - right.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	TargetPosition Subtract(const TargetPosition& left,
		const TargetPosition& right)
	{
		return { left.x - right.x, left.y - right.y, left.z - right.z };
	}

	TargetPosition Normalize(const TargetPosition& value)
	{
		const double length = std::sqrt(
			static_cast<double>(value.x) * value.x +
			static_cast<double>(value.y) * value.y +
			static_cast<double>(value.z) * value.z);
		if (!std::isfinite(length) || length <= kGeometryEpsilon)
			return {};
		return { static_cast<float>(value.x / length),
			static_cast<float>(value.y / length),
			static_cast<float>(value.z / length) };
	}

	TargetPosition DeterministicRight(const TargetPosition& forward)
	{
		const TargetPosition horizontal = { -forward.y, forward.x, 0.0f };
		const TargetPosition normalized = Normalize(horizontal);
		return normalized.IsFinite() &&
			Distance(normalized, TargetPosition()) > kGeometryEpsilon ?
			normalized : TargetPosition{ 0.0f, 1.0f, 0.0f };
	}

	TargetPosition Centroid(const std::vector<MemberObservation>& members)
	{
		TargetPosition result;
		std::size_t count = 0;
		for (const MemberObservation& observation : members)
		{
			if (!observation.state.IsEligible())
				continue;
			result.x += observation.state.origin.x;
			result.y += observation.state.origin.y;
			result.z += observation.state.origin.z;
			++count;
		}
		if (count == 0)
			return {};
		const float divisor = static_cast<float>(count);
		return { result.x / divisor, result.y / divisor, result.z / divisor };
	}

	RCBotSquadSpatial::FormationFrame MakeFrame(
		const TargetPosition& centroid, const TargetPosition& objective)
	{
		RCBotSquadSpatial::FormationFrame frame;
		// Active squad MOVE slots are final targets around the Commander
		// objective, not a local formation around the current centroid.
		frame.anchor = objective;
		const TargetPosition delta = Subtract(objective, centroid);
		const TargetPosition horizontal = { delta.x, delta.y, 0.0f };
		if (Distance(horizontal, TargetPosition()) <= kGeometryEpsilon)
		{
			// Explicit fallback: horizontal orientation is undefined when the
			// objective is at the centroid (or differs only in height).
			frame.forward = { 1.0f, 0.0f, 0.0f };
			frame.right = { 0.0f, 1.0f, 0.0f };
			return frame;
		}
		frame.forward = Normalize(horizontal);
		frame.right = DeterministicRight(frame.forward);
		return frame;
	}

	std::uint64_t MixByte(std::uint64_t hash, const std::uint8_t value)
	{
		return (hash ^ value) * kFnvPrime;
	}

	std::uint64_t MixFloat(std::uint64_t hash, float value)
	{
		if (value == 0.0f)
			value = 0.0f;
		std::uint32_t bits = 0;
		static_assert(sizeof(bits) == sizeof(value), "float must be IEEE-sized");
		std::memcpy(&bits, &value, sizeof(bits));
		for (unsigned shift = 0; shift < 32; shift += 8)
			hash = MixByte(hash, static_cast<std::uint8_t>((bits >> shift) & 0xffU));
		return hash;
	}

	std::uint64_t ParentObservationId(const TargetPosition& target)
	{
		std::uint64_t hash = kFnvOffset;
		hash = MixFloat(hash, target.x);
		hash = MixFloat(hash, target.y);
		hash = MixFloat(hash, target.z);
		return hash == 0 ? 1 : hash;
	}

	bool ContainsHuman(const std::vector<MemberObservation>& members)
	{
		for (const MemberObservation& observation : members)
			if (observation.human && observation.state.connected)
				return true;
		return false;
	}

	std::vector<MemberObservation> CanonicalMembers(
		const std::vector<MemberObservation>& members)
	{
		std::vector<MemberObservation> result = members;
		std::sort(result.begin(), result.end(),
			[](const MemberObservation& left, const MemberObservation& right) {
				return left.state.memberId < right.state.memberId;
			});
		return result;
	}

	bool MemberFactsChanged(const std::vector<MemberObservation>& members,
		const std::vector<MemberState>& previous)
	{
		if (members.size() != previous.size())
			return true;
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const MemberState& current = members[i].state;
			const MemberState& old = previous[i];
			if (current.memberId != old.memberId || current.alive != old.alive ||
				current.connected != old.connected || current.eligible != old.eligible ||
				current.isLeader != old.isLeader ||
				current.squadPosition != old.squadPosition ||
				current.lifecycle != old.lifecycle)
				return true;
		}
		return false;
	}
}

bool ParentOrderObservation::IsValid() const
{
	return !present || (((parentOrder.orderId == 0 && parentOrder.generation == 0) ||
		(parentOrder.orderId != 0 && parentOrder.generation != 0)) &&
		target.IsFinite() && std::isfinite(deadline) && deadline >= 0.0);
}

bool Input::IsValid() const
{
	if (team == 0 || squadId == 0 || !std::isfinite(now) ||
		!parentOrder.IsValid() || members.empty())
		return false;
	for (const MemberObservation& observation : members)
		if (!observation.state.IsValid())
			return false;
	return true;
}

Adapter::State* Adapter::FindState(const int team, const RecipientId squadId)
{
	if (squadId == 0 || squadId > kMaxSquadsPerTeam ||
		(team != 2 && team != 3))
		return nullptr;
	const std::size_t teamIndex = team == 2 ? 0 : 1;
	State& state = m_states[teamIndex][static_cast<std::size_t>(squadId - 1)];
	if (!state.inUse || state.team != team || state.squadId != squadId)
	{
		state = State();
		state.inUse = true;
		state.team = team;
		state.squadId = squadId;
	}
	return &state;
}

const Adapter::State* Adapter::FindState(const int team,
	const RecipientId squadId) const
{
	if (squadId == 0 || squadId > kMaxSquadsPerTeam ||
		(team != 2 && team != 3))
		return nullptr;
	const std::size_t teamIndex = team == 2 ? 0 : 1;
	const State& state = m_states[teamIndex][static_cast<std::size_t>(squadId - 1)];
	return state.inUse && state.team == team && state.squadId == squadId ?
		&state : nullptr;
}

void Adapter::Emit(TelemetrySink sink, void* const context,
	const Telemetry& telemetry) const
{
	if (sink != nullptr)
		sink(context, telemetry);
}

void Adapter::EmitDecision(TelemetrySink sink, void* const context,
	const State& state, const Input& input, const bool revised) const
{
	Telemetry telemetry;
	telemetry.event = revised ? EventType::SQUAD_DECISION_REVISED :
		EventType::SQUAD_DECISION_CREATED;
	telemetry.team = input.team;
	telemetry.squadId = input.squadId;
	telemetry.squadGeneration = state.decision.identity.squadGeneration;
	telemetry.decisionRevision = state.decision.decisionRevision;
	telemetry.snapshotFingerprint = state.decision.canonicalFingerprint;
	telemetry.parentOrder = state.decision.commanderOrder.parentOrder;
	telemetry.formationGeneration = state.formationGeneration;
	telemetry.formationType = FormationType::WEDGE;
	telemetry.autonomous = true;
	Emit(sink, context, telemetry);
}

	void Adapter::EmitFormation(TelemetrySink sink, void* const context,
		const State& state, const Input& input) const
{
	for (const auto& participation : state.decision.participation)
	{
		Telemetry telemetry;
		telemetry.event = EventType::SQUAD_MEMBER_PARTICIPATION;
		telemetry.team = input.team;
		telemetry.squadId = input.squadId;
		telemetry.squadGeneration = state.decision.identity.squadGeneration;
		telemetry.decisionRevision = state.decision.decisionRevision;
		telemetry.snapshotFingerprint = state.decision.canonicalFingerprint;
		telemetry.parentOrder = state.decision.commanderOrder.parentOrder;
		telemetry.formationGeneration = state.formationGeneration;
		telemetry.formationType = FormationType::WEDGE;
		telemetry.memberId = participation.memberId;
		telemetry.participation = participation.state;
		telemetry.autonomous = true;
		Emit(sink, context, telemetry);
	}
	for (const auto& assignment : state.formation.assignments)
	{
		Telemetry telemetry;
		telemetry.event = EventType::SQUAD_SLOT_ASSIGNED;
		telemetry.team = input.team;
		telemetry.squadId = input.squadId;
		telemetry.squadGeneration = state.decision.identity.squadGeneration;
		telemetry.decisionRevision = state.decision.decisionRevision;
		telemetry.snapshotFingerprint = state.decision.canonicalFingerprint;
		telemetry.parentOrder = state.decision.commanderOrder.parentOrder;
		telemetry.formationGeneration = state.formationGeneration;
		telemetry.formationType = FormationType::WEDGE;
		telemetry.memberId = assignment.memberId;
		telemetry.participation = assignment.participation;
		telemetry.slotId = assignment.slotId;
		telemetry.claimActive = assignment.claimStatus ==
			RCBotSquadSpatial::SpatialClaimStatus::CLAIMED;
		telemetry.autonomous = true;
		Emit(sink, context, telemetry);
	}
}

void Adapter::EmitReleasedClaims(TelemetrySink sink, void* const context,
	const State& state, const Input& input,
	const ClaimSnapshot& nextClaims) const
{
	if (!state.claims.IsValid())
		return;
	for (const auto& oldClaim : state.claims.claims)
	{
		const bool retained = std::any_of(nextClaims.claims.begin(),
			nextClaims.claims.end(), [&oldClaim](const auto& nextClaim) {
			return oldClaim.key == nextClaim.key &&
				oldClaim.ownerMemberId == nextClaim.ownerMemberId;
		});
		if (retained)
			continue;
		Telemetry telemetry;
		telemetry.event = EventType::SQUAD_CLAIM_RELEASED;
		telemetry.team = input.team;
		telemetry.squadId = input.squadId;
		telemetry.squadGeneration = state.decision.identity.squadGeneration;
		telemetry.decisionRevision = state.decision.decisionRevision;
		telemetry.snapshotFingerprint = state.decision.canonicalFingerprint;
		telemetry.parentOrder = state.decision.commanderOrder.parentOrder;
		telemetry.formationGeneration = state.formationGeneration;
		telemetry.formationType = FormationType::WEDGE;
		telemetry.memberId = oldClaim.ownerMemberId;
		telemetry.slotId = oldClaim.key.resourceId;
		telemetry.claimActive = false;
		telemetry.autonomous = true;
		Emit(sink, context, telemetry);
	}
}

Result Adapter::Observe(const Input& rawInput, TelemetrySink sink,
	void* const context)
{
	Result invalid;
	if (!rawInput.IsValid())
		return invalid;

	State* state = FindState(rawInput.team, rawInput.squadId);
	if (state == nullptr)
		return invalid;
	Input input = rawInput;
	input.members = CanonicalMembers(rawInput.members);
	if (input.parentOrder.present && input.parentOrder.parentOrder.orderId == 0)
	{
		const bool sameTarget = state->hasParent &&
			SamePosition(state->parentTarget, input.parentOrder.target, 0.01);
		input.parentOrder.parentOrder.orderId =
			ParentObservationId(input.parentOrder.target);
		input.parentOrder.parentOrder.generation = sameTarget &&
			state->parentOrder.generation != 0 ? state->parentOrder.generation :
			(state->hasParent ? state->parentOrder.generation + 1 : 1);
		if (input.parentOrder.parentOrder.generation == 0)
			input.parentOrder.parentOrder.generation = 1;
	}
	if (!input.parentOrder.IsValid())
		return invalid;

	std::vector<RecipientId> memberIds;
	memberIds.reserve(input.members.size());
	for (std::size_t i = 0; i < input.members.size(); ++i)
	{
		if (input.members[i].state.memberId == 0 ||
			(i > 0 && input.members[i - 1].state.memberId ==
				input.members[i].state.memberId))
			return invalid;
		memberIds.push_back(input.members[i].state.memberId);
	}
	const SquadMembershipObservation membership =
		state->identityTracker.Observe(input.squadId, memberIds);
	if (!membership.recipient.IsValid())
		return invalid;

	Result result;
	result.identity.team = input.team;
	result.identity.squadId = input.squadId;
	result.identity.squadGeneration = membership.recipient.generation;
	result.autonomous = !ContainsHuman(input.members);
	result.parentOrder = input.parentOrder.parentOrder;

	if (!input.parentOrder.present)
	{
		if (state->hasDecision)
		{
			Telemetry telemetry;
			telemetry.event = EventType::SQUAD_DECISION_INVALIDATED;
			telemetry.team = input.team;
			telemetry.squadId = input.squadId;
			telemetry.squadGeneration = state->decision.identity.squadGeneration;
			telemetry.decisionRevision = state->decision.decisionRevision;
			telemetry.snapshotFingerprint = state->decision.canonicalFingerprint;
			telemetry.parentOrder = state->decision.commanderOrder.parentOrder;
			telemetry.formationGeneration = state->formationGeneration;
			telemetry.formationType = FormationType::WEDGE;
			telemetry.autonomous = result.autonomous;
			Emit(sink, context, telemetry);
		}
		ClaimSnapshot emptyClaims;
		EmitReleasedClaims(sink, context, *state, input, emptyClaims);
		state->claims = emptyClaims;
		state->hasParent = false;
		state->hasDecision = false;
		result.status = Status::NO_PARENT_ORDER;
		result.current = true;
		return result;
	}

	if (!result.autonomous)
	{
		const bool semanticInvalidation = state->hasDecision || state->hasParent ||
			state->claims.IsValid();
		if (semanticInvalidation)
		{
			Telemetry telemetry;
			telemetry.event = EventType::SQUAD_DECISION_INVALIDATED;
			telemetry.team = input.team;
			telemetry.squadId = input.squadId;
			telemetry.squadGeneration = result.identity.squadGeneration;
			telemetry.autonomous = false;
			Emit(sink, context, telemetry);
		}
		ClaimSnapshot emptyClaims;
		EmitReleasedClaims(sink, context, *state, input, emptyClaims);
		state->claims = emptyClaims;
		state->hasDecision = false;
		state->hasParent = false;
		result.status = Status::NOT_AUTONOMOUS;
		result.current = true;
		return result;
	}

	const TargetPosition centroid = Centroid(input.members);
	const TargetPosition objective = input.parentOrder.target;
	const RCBotSquadSpatial::FormationFrame frame =
		MakeFrame(centroid, objective);
	const bool geometryChanged = !state->hasCentroid ||
		Distance(centroid, state->lastCentroid) > kSemanticMemberMove;
	const bool identityChanged = !state->hasDecision ||
		state->decision.identity != result.identity;
	const bool parentChanged = !state->hasParent ||
		!SamePosition(state->parentTarget, input.parentOrder.target,
			kSemanticTargetMove) || state->parentOrder.orderId !=
			input.parentOrder.parentOrder.orderId || state->parentOrder.generation !=
			input.parentOrder.parentOrder.generation;
	const bool memberFactsChanged = !state->hasDecision ||
		MemberFactsChanged(input.members, state->decision.canonicalMembers);

	// The pure canonicalizer supplies the platform-stable snapshot fingerprint.
	// In active final-slot mode, member motion alone is not a semantic event:
	// keep the objective frame and claims stable until Parent replacement or a
	// real membership/lifecycle/leader change. Shadow observation retains its
	// existing bounded geometry-revision behavior.
	RCBotSquadDecision::SquadDecisionInput decisionInput;
	decisionInput.identity = result.identity;
	decisionInput.decisionRevision = state->hasDecision && !identityChanged ?
		state->decision.decisionRevision : 1;
	decisionInput.commanderOrder.present = true;
	decisionInput.commanderOrder.parentOrder = input.parentOrder.parentOrder;
	decisionInput.commanderOrder.target = input.parentOrder.target;
	decisionInput.commanderOrder.deadline = input.parentOrder.deadline;
	decisionInput.commanderOrder.orderState = input.parentOrder.orderState;
	decisionInput.members.reserve(input.members.size());
	for (const MemberObservation& observation : input.members)
		decisionInput.members.push_back(observation.state);

	const auto canonical = RCBotSquadDecision::SquadDecisionCore::Canonicalize(
		decisionInput);
	if (!canonical.valid)
		return invalid;
	const bool spatialGeometryChanged = geometryChanged &&
		!input.freezeSpatialPlanForActiveParent;
	const bool semanticChanged = identityChanged || parentChanged ||
		memberFactsChanged || spatialGeometryChanged;
	if (!semanticChanged && state->cached.current)
	{
		Result unchanged = state->cached;
		unchanged.semanticChange = false;
		return unchanged;
	}

	if (!identityChanged && state->hasDecision)
		decisionInput.decisionRevision = state->decision.decisionRevision + 1;
	const SquadDecisionRevisionContext previous = {
		state->hasDecision && !identityChanged,
		state->decision.identity,
		state->decision.decisionRevision,
		state->decision.canonicalFingerprint
	};
	const SquadDecisionResult decision =
		RCBotSquadDecision::SquadDecisionCore::Evaluate(decisionInput, previous);
	if (!decision.Accepted())
		return invalid;

	const bool newIdentity = identityChanged;
	if (newIdentity)
		state->formationGeneration = 1;
	else if (state->formationGeneration == 0 || parentChanged ||
			spatialGeometryChanged || memberFactsChanged)
		++state->formationGeneration;
	if (state->formationGeneration == 0)
		state->formationGeneration = 1;

	RCBotSquadSpatial::SpatialFormationInput spatialInput;
	spatialInput.decision = decision;
	spatialInput.formationType = FormationType::WEDGE;
	spatialInput.policy.maxSlots = 8;
	spatialInput.policy.memberSpacing = 64.0;
	spatialInput.policy.rowSpacing = 64.0;
	// Active movement is a single final-slot execution that can outlive the
	// old shadow-only one-second lease. Parent/decision replacement still
	// purges this bounded per-squad claim snapshot immediately.
	spatialInput.policy.slotLeaseDuration = 3600.0;
	spatialInput.frame = frame;
	spatialInput.formationGeneration = state->formationGeneration;
	spatialInput.now = input.now;
	spatialInput.hasExistingClaims = state->claims.IsValid();
	spatialInput.existingClaims = state->claims;
	const FormationDecisionResult formation =
		RCBotSquadSpatial::SquadSpatialCore::Evaluate(spatialInput);
	if (!formation.Accepted())
		return invalid;

	const bool revised = state->hasDecision;
	state->hasDecision = true;
	state->hasParent = true;
	state->parentTarget = input.parentOrder.target;
	state->parentOrder = input.parentOrder.parentOrder;
	state->lastCentroid = centroid;
	state->hasCentroid = true;
	state->semanticFingerprint = canonical.canonicalFingerprint;
	state->decision = decision;
	state->formation = formation;
	EmitReleasedClaims(sink, context, *state, input,
		formation.claimSnapshot);
	state->claims = formation.claimSnapshot;

	result.status = Status::ACCEPTED;
	result.semanticChange = true;
	result.current = true;
	result.autonomous = true;
	result.identity = decision.identity;
	result.decisionRevision = decision.decisionRevision;
	result.snapshotFingerprint = decision.canonicalFingerprint;
	result.parentOrder = decision.commanderOrder.parentOrder;
	result.formationGeneration = state->formationGeneration;
	result.formationType = FormationType::WEDGE;
	result.decision = decision;
	result.formation = formation;
	state->cached = result;
	EmitDecision(sink, context, *state, input, revised);
	EmitFormation(sink, context, *state, input);
	return result;
}

bool Adapter::RejectStaleCallback(const int team, const RecipientId squadId,
	const Generation squadGeneration, const DecisionRevision decisionRevision) const
{
	const State* state = FindState(team, squadId);
	return state == nullptr || !state->hasDecision ||
		state->decision.identity.squadGeneration != squadGeneration ||
		state->decision.decisionRevision != decisionRevision;
}

void Adapter::Reset()
{
	for (std::size_t team = 0; team < kMaxTeams; ++team)
		for (std::size_t squad = 0; squad < kMaxSquadsPerTeam; ++squad)
			m_states[team][squad] = State();
}

const char* StatusName(const Status status)
{
	switch (status)
	{
	case Status::NO_PARENT_ORDER: return "NO_PARENT_ORDER";
	case Status::NOT_AUTONOMOUS: return "NOT_AUTONOMOUS";
	case Status::ACCEPTED: return "ACCEPTED";
	case Status::REJECTED_INVALID_INPUT: return "REJECTED_INVALID_INPUT";
	case Status::STALE_CALLBACK: return "STALE_CALLBACK";
	}
	return "UNKNOWN";
}

const char* EventTypeName(const EventType event)
{
	switch (event)
	{
	case EventType::SQUAD_DECISION_CREATED: return "SQUAD_DECISION_CREATED";
	case EventType::SQUAD_DECISION_REVISED: return "SQUAD_DECISION_REVISED";
	case EventType::SQUAD_MEMBER_PARTICIPATION: return "SQUAD_MEMBER_PARTICIPATION";
	case EventType::SQUAD_SLOT_ASSIGNED: return "SQUAD_SLOT_ASSIGNED";
	case EventType::SQUAD_CLAIM_RELEASED: return "SQUAD_CLAIM_RELEASED";
	case EventType::SQUAD_DECISION_INVALIDATED: return "SQUAD_DECISION_INVALIDATED";
	}
	return "UNKNOWN";
}

const char* FormationTypeName(const FormationType type)
{
	return RCBotSquadSpatial::FormationTypeName(type);
}
}
