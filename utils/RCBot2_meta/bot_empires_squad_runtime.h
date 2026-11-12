#ifndef __RCBOT_EMPIRES_SQUAD_RUNTIME_H__
#define __RCBOT_EMPIRES_SQUAD_RUNTIME_H__

#include "bot_squad_decision.h"
#include "bot_squad_order_recipient.h"
#include "bot_squad_spatial.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RCBotEmpiresSquadRuntime
{
	using RCBotOrder::Generation;
	using RCBotOrder::OrderKey;
	using RCBotOrder::OrderState;
	using RCBotOrder::RecipientId;
	using RCBotOrder::TargetPosition;
	using RCBotSquadDecision::DecisionRevision;
	using RCBotSquadDecision::Fingerprint;
	using RCBotSquadDecision::MemberLifecycle;
	using RCBotSquadDecision::MemberState;
	using RCBotSquadDecision::ParticipationState;
	using RCBotSquadDecision::SquadDecisionResult;
	using RCBotSquadDecision::SquadDecisionRevisionContext;
	using RCBotSquadDecision::SquadIdentity;
	using RCBotSquadClaims::ClaimSnapshot;
	using RCBotSquadOrder::SquadIdentityTracker;
	using RCBotSquadOrder::SquadMembershipObservation;
	using RCBotSquadSpatial::FormationDecisionResult;
	using RCBotSquadSpatial::FormationType;

	// This is the only runtime input contract.  The Empires adapter fills it
	// from authoritative SendProps; the decision/formation stack never reads
	// an edict, controller, task, schedule or navigator.
	struct MemberObservation
	{
		MemberState state;
		bool human = false;
	};

	struct ParentOrderObservation
	{
		bool present = false;
		OrderKey parentOrder;
		TargetPosition target;
		double deadline = 0.0;
		OrderState orderState = OrderState::EXECUTING;

		bool IsValid() const;
	};

	struct Input
	{
		int team = 0;
		RecipientId squadId = 0;
		std::vector<MemberObservation> members;
		ParentOrderObservation parentOrder;
		double now = 0.0;
		// Active final-slot movement freezes its objective frame for this Parent;
		// ordinary member motion must not turn into formation marching.
		bool freezeSpatialPlanForActiveParent = false;

		bool IsValid() const;
	};

	enum class Status : std::uint8_t
	{
		NO_PARENT_ORDER = 0,
		NOT_AUTONOMOUS,
		ACCEPTED,
		REJECTED_INVALID_INPUT,
		STALE_CALLBACK
	};

	enum class EventType : std::uint8_t
	{
		SQUAD_DECISION_CREATED = 0,
		SQUAD_DECISION_REVISED,
		SQUAD_MEMBER_PARTICIPATION,
		SQUAD_SLOT_ASSIGNED,
		SQUAD_CLAIM_RELEASED,
		SQUAD_DECISION_INVALIDATED
	};

	// Value-only telemetry.  Runtime serialization is a separate sink, so the
	// adapter remains usable by headless tests and cannot call engine APIs.
	struct Telemetry
	{
		EventType event = EventType::SQUAD_DECISION_INVALIDATED;
		int team = 0;
		RecipientId squadId = 0;
		Generation squadGeneration = 0;
		DecisionRevision decisionRevision = 0;
		Fingerprint snapshotFingerprint = 0;
		OrderKey parentOrder;
		Generation formationGeneration = 0;
		FormationType formationType = FormationType::WEDGE;
		RecipientId memberId = 0;
		ParticipationState participation = ParticipationState::EXCLUDED;
		std::uint64_t slotId = 0;
		bool claimActive = false;
		bool autonomous = false;
		bool movementRequested = false;
	};

	using TelemetrySink = void (*)(void* context, const Telemetry& telemetry);

	struct Result
	{
		Status status = Status::REJECTED_INVALID_INPUT;
		bool semanticChange = false;
		bool current = false;
		bool autonomous = false;
		SquadIdentity identity;
		DecisionRevision decisionRevision = 0;
		Fingerprint snapshotFingerprint = 0;
		OrderKey parentOrder;
		Generation formationGeneration = 0;
		FormationType formationType = FormationType::WEDGE;
		SquadDecisionResult decision;
		FormationDecisionResult formation;

		bool Accepted() const { return status == Status::ACCEPTED; }
	};

	// A fixed two-team/64-squad table bounds all state.  There is one
	// canonical decision and one formation result per logical squad; repeated
	// member observations return that same value snapshot and do not emit
	// per-tick telemetry.
	class Adapter
	{
	public:
		static const std::size_t kMaxTeams = 2;
		static const std::size_t kMaxSquadsPerTeam = 64;

		Result Observe(const Input& input, TelemetrySink sink = nullptr,
			void* context = nullptr);
		bool RejectStaleCallback(int team, RecipientId squadId,
			Generation squadGeneration, DecisionRevision decisionRevision) const;
		void Reset();

	private:
		struct State
		{
			bool inUse = false;
			int team = 0;
			RecipientId squadId = 0;
			SquadIdentityTracker identityTracker;
			bool hasDecision = false;
			bool hasParent = false;
			TargetPosition parentTarget;
			OrderKey parentOrder;
			Generation formationGeneration = 0;
			SquadDecisionResult decision;
			FormationDecisionResult formation;
			ClaimSnapshot claims;
			TargetPosition lastCentroid;
			bool hasCentroid = false;
			Fingerprint semanticFingerprint = 0;
			Result cached;
		};

		State m_states[kMaxTeams][kMaxSquadsPerTeam];

		State* FindState(int team, RecipientId squadId);
		const State* FindState(int team, RecipientId squadId) const;
		void Emit(TelemetrySink sink, void* context,
			const Telemetry& telemetry) const;
		void EmitDecision(TelemetrySink sink, void* context,
			const State& state, const Input& input, bool revised) const;
		void EmitFormation(TelemetrySink sink, void* context,
			const State& state, const Input& input) const;
		void EmitReleasedClaims(TelemetrySink sink, void* context,
			const State& state, const Input& input,
			const ClaimSnapshot& nextClaims) const;
	};

	const char* StatusName(Status status);
	const char* EventTypeName(EventType event);
	const char* FormationTypeName(FormationType type);
}

#endif // __RCBOT_EMPIRES_SQUAD_RUNTIME_H__
