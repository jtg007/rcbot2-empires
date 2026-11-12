#ifndef __RCBOT_SQUAD_ORDER_RECIPIENT_H__
#define __RCBOT_SQUAD_ORDER_RECIPIENT_H__

#include "bot_order_protocol.h"
#include "bot_order_protocol_telemetry.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace RCBotSquadOrder
{
	using RCBotOrder::Generation;
	using RCBotOrder::IssuerId;
	using RCBotOrder::OrderId;
	using RCBotOrder::OrderKey;
	using RCBotOrder::OrderOutcome;
	using RCBotOrder::OrderRecipient;
	using RCBotOrder::RecipientId;
	using RCBotOrder::TargetPosition;

	// The caller supplies a team-scoped squad id.  Empires' public m_iSquad is
	// only map-unique inside a team, so the production adapter must include the
	// team in this value before it reaches the protocol.
	struct SquadMembershipObservation
	{
		OrderRecipient recipient;
		std::vector<RecipientId> members;
		bool membershipChanged = false;
	};

	class SquadIdentityTracker
	{
	public:
		SquadMembershipObservation Observe(RecipientId squadId,
			const std::vector<RecipientId>& members);
		bool Forget(RecipientId squadId);
		bool Current(RecipientId squadId,
			SquadMembershipObservation& observation) const;

	private:
		struct Entry
		{
			Generation generation = 0;
			std::vector<RecipientId> members;
		};

		std::map<RecipientId, Entry> m_entries;
		std::map<RecipientId, Generation> m_nextGeneration;
	};

	struct SquadMembershipSnapshot
	{
		OrderRecipient recipient;
		std::vector<RecipientId> members;

		bool IsValid() const;
		bool Contains(RecipientId memberId) const;
	};

	struct SquadMoveSpec
	{
		OrderId orderId = 0;
		Generation generation = 0;
		IssuerId issuerId = 0;
		SquadMembershipSnapshot snapshot;
		TargetPosition target;
		double issuedAt = 0.0;
		double deadline = 0.0;
	};

	struct NavigationIntentRequest
	{
		OrderKey parentOrder;
		OrderKey childExecution;
		RecipientId memberId = 0;
		TargetPosition target;
		std::uint64_t slotId = 0;
		Generation squadGeneration = 0;
		std::uint64_t decisionRevision = 0;
		Generation formationGeneration = 0;
		std::uint8_t claimResourceKind = 0;
		std::uint64_t claimResourceId = 0;
		Generation claimResourceGeneration = 0;
	};

	struct ChildMovementTarget
	{
		RecipientId memberId = 0;
		bool hasTarget = false;
		TargetPosition target;
		std::uint64_t slotId = 0;
		Generation squadGeneration = 0;
		std::uint64_t decisionRevision = 0;
		Generation formationGeneration = 0;
		std::uint8_t claimResourceKind = 0;
		std::uint64_t claimResourceId = 0;
		Generation claimResourceGeneration = 0;
	};

	using NavigationIntentStart = bool (*)(void* context,
		const NavigationIntentRequest& request,
		std::uint64_t* navigationIntentId);
	using NavigationIntentCancel = void (*)(void* context,
		const NavigationIntentRequest& request,
		std::uint64_t navigationIntentId);
	using TelemetrySink = void (*)(void* context,
		const RCBotOrderTelemetry::TelemetryEvent& event);

	struct ChildExecution
	{
		OrderKey key;
		OrderKey parentOrder;
	RecipientId memberId = 0;
	std::uint64_t navigationIntentId = 0;
	std::uint64_t navigationRequestToken = 0;
	bool navigationIntentBound = false;
	bool navigationRequested = false;
		bool hasMovementTarget = false;
		TargetPosition movementTarget;
		std::uint64_t slotId = 0;
		Generation squadGeneration = 0;
		std::uint64_t decisionRevision = 0;
		Generation formationGeneration = 0;
		std::uint8_t claimResourceKind = 0;
		std::uint64_t claimResourceId = 0;
		Generation claimResourceGeneration = 0;
		RCBotOrder::OrderProtocol protocol;
		RCBotOrder::OrderRecord record;
	};

	struct AggregateSnapshot
	{
		bool terminal = false;
		OrderOutcome outcome = OrderOutcome::NONE;
		std::size_t completeCount = 0;
		std::size_t failureCount = 0;
		std::size_t cancelledCount = 0;
		std::size_t pendingCount = 0;
	};

	class SquadMoveRecipient
	{
	public:
		SquadMoveRecipient() = default;
		SquadMoveRecipient(NavigationIntentStart start,
			NavigationIntentCancel cancel, TelemetrySink telemetry,
			void* context);

		void SetNavigationSinks(NavigationIntentStart start,
			NavigationIntentCancel cancel);
		void SetTelemetrySink(TelemetrySink sink, void* context);
		void Reset();

		// Issuing while a parent is active performs replacement.  The old parent
		// is terminalized before the new snapshot is dispatched.
		bool IssueMove(const SquadMoveSpec& spec);
		// Runtime path: create/replace the authoritative Parent first, then
		// dispatch exactly one validated final movement target per snapshot member.
		bool BeginMove(const SquadMoveSpec& spec);
		bool DispatchChildren(const OrderKey& parentOrder,
			const std::vector<ChildMovementTarget>& targets, double now);
		bool ReplaceChildMovementTarget(const OrderKey& parentOrder,
			const OrderKey& childExecution, RecipientId memberId,
			const ChildMovementTarget& target, double now);
	bool ObserveNavigationStarted(const OrderKey& parentOrder,
		const OrderKey& childExecution, RecipientId memberId,
		std::uint64_t requestToken, std::uint64_t navigationIntentId);
	bool HasMemberInActiveSnapshot(RecipientId memberId) const;
	bool HasActiveMovementForMember(RecipientId memberId) const;
	bool CancelActiveParent(RCBotOrder::DetailReason reason, double now);
		bool HasActiveParent() const;
		OrderKey ActiveParentKey() const;
		RCBotOrder::OrderRecord ParentSnapshot() const;
		AggregateSnapshot ParentAggregate() const;
	std::vector<ChildExecution> ActiveChildren() const;
	std::vector<ChildExecution> RetiredChildren() const;
	std::vector<RecipientId> ActiveMemberSnapshot() const;
	bool ChildrenDispatched() const;

		// The update carries the full parent and child identity.  A result from a
		// prior parent, member or navigation intent is therefore rejected before
		// it can mutate any current child.
		bool ObserveNavigationTerminal(const OrderKey& parentOrder,
			const OrderKey& childExecution, RecipientId memberId,
			std::uint64_t navigationIntentId,
			RCBotOrderTelemetry::NavigationTerminalResult result, double now);
		bool ObserveMemberLifecycle(const OrderKey& parentOrder,
			const OrderRecipient& currentSquad, RecipientId memberId,
			RCBotOrder::DetailReason reason, double now);
		bool ObserveMembership(const OrderKey& parentOrder,
			const SquadMembershipSnapshot& current, double now);
		bool Update(double now);

		static const char* ParentTerminalRule();

	private:
		bool IsCurrentParent(const OrderKey& key) const;
		ChildExecution* FindActiveChild(const OrderKey& key,
			RecipientId memberId);
		void Emit(const RCBotOrder::UpdateResult& result,
			const RCBotOrderTelemetry::OrderCorrelation& correlation = {});
		void EmitChild(const ChildExecution& child,
			const RCBotOrder::UpdateResult& result,
			const RCBotOrderTelemetry::RuntimeContext& context = {});
		void CancelNavigation(ChildExecution& child);
		bool TerminalizeChild(ChildExecution& child,
			const RCBotOrder::UpdateResult& result);
		bool AggregateIfReady(double now);
		void RefreshAggregate();
		void RetireActiveChildren();
		bool CreateChild(const OrderKey& parentOrder,
			const ChildMovementTarget& target, double now,
			ChildExecution& child);
		bool StartNavigation(ChildExecution& child, double now);
		static bool SameMovementTarget(const ChildExecution& child,
			const ChildMovementTarget& target);
		std::uint64_t AllocateChildId();
		Generation AllocateChildGeneration();
		void ClearActive();

		NavigationIntentStart m_navigationStart = nullptr;
		NavigationIntentCancel m_navigationCancel = nullptr;
		TelemetrySink m_telemetry = nullptr;
		void* m_context = nullptr;
		RCBotOrder::OrderProtocol m_parent;
		OrderKey m_parentKey;
		OrderRecipient m_squadRecipient;
		TargetPosition m_target;
		bool m_active = false;
		std::vector<ChildExecution> m_children;
		std::vector<ChildExecution> m_retiredChildren;
		std::vector<RecipientId> m_memberSnapshot;
		bool m_dispatched = false;
		AggregateSnapshot m_aggregate;
		OrderId m_nextChildId = 1;
		Generation m_nextChildGeneration = 1;
		std::uint64_t m_nextNavigationIntentId = 1;
	};
}

#endif // __RCBOT_SQUAD_ORDER_RECIPIENT_H__
