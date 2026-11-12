#ifndef __RCBOT_ORDER_PROTOCOL_H__
#define __RCBOT_ORDER_PROTOCOL_H__

#include <cstdint>

namespace RCBotOrder
{
	using OrderId = std::uint64_t;
	using Generation = std::uint64_t;
	using IssuerId = std::uint64_t;
	using RecipientId = std::uint64_t;

	struct OrderKey
	{
		OrderId orderId = 0;
		Generation generation = 0;
	};

	struct TargetPosition
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;

		bool IsFinite() const;
	};

	enum class OrderType : std::uint8_t
	{
		MOVE = 0
	};

	enum class RecipientKind : std::uint8_t
	{
		BOT = 0,
		SQUAD
	};

	struct OrderRecipient
	{
		RecipientKind kind = RecipientKind::BOT;
		RecipientId id = 0;
		Generation generation = 0;

		static OrderRecipient Bot(RecipientId botId)
		{
			return { RecipientKind::BOT, botId, 0 };
		}

		static OrderRecipient Squad(RecipientId squadId,
			Generation squadGeneration)
		{
			return { RecipientKind::SQUAD, squadId, squadGeneration };
		}

		bool IsValid() const;
		bool operator==(const OrderRecipient& other) const
		{
			return kind == other.kind && id == other.id &&
				generation == other.generation;
		}
		bool operator!=(const OrderRecipient& other) const
		{
			return !(*this == other);
		}
	};

	enum class OrderState : std::uint8_t
	{
		ISSUED = 0,
		ACKNOWLEDGED,
		PLANNING,
		MOVING,
		EXECUTING,
		TERMINAL
	};

	enum class OrderOutcome : std::uint8_t
	{
		NONE = 0,
		COMPLETE,
		BLOCKED,
		STUCK,
		TARGET_LOST,
		INVALID,
		NEEDS_SUPPORT,
		UNREACHABLE,
		PARTIAL_FAILURE,
		ABORTED
	};

	enum class OrderOperation : std::uint8_t
	{
		ISSUE = 0,
		ACKNOWLEDGE,
		BEGIN_PLANNING,
		BEGIN_MOVING,
		BEGIN_EXECUTING,
		COMPLETE,
		FAIL,
		ABORT,
		REPLACE,
		TICK
	};

	// DetailReason is deliberately independent from OrderOutcome.  The
	// outcome is stable protocol semantics; the reason preserves why the
	// outcome was reached for later telemetry and adapters.
	enum class DetailReason : std::uint8_t
	{
		NONE = 0,
		INVALID_ORDER_ID,
		INVALID_GENERATION,
		INVALID_ISSUER_ID,
		INVALID_RECIPIENT_ID,
		UNSUPPORTED_ORDER_TYPE,
		NON_FINITE_TARGET,
		INVALID_ISSUED_AT,
		INVALID_DEADLINE,
		DEADLINE_EXPIRED,
		REPLACED,
		DEATH_OR_LIFECYCLE,
		CANCELLED,
		// Compatibility names retained for callers from the handoff phases.
		BOT_DIED = DEATH_OR_LIFECYCLE,
		EXPLICIT_ABORT = CANCELLED,
		FAILURE_REPORTED,
		MISSING_REASON,
		INVALID_FAILURE_OUTCOME,
		INVALID_PROTOCOL_TIME,
		STALE_CONTEXT,
		CHILDREN_AGGREGATED,
		CHILDREN_EXHAUSTED
	};

	enum class UpdateStatus : std::uint8_t
	{
		APPLIED = 0,
		DUPLICATE,
		STALE,
		ILLEGAL_TRANSITION,
		ALREADY_TERMINAL,
		NOT_ISSUED
	};

	struct OrderTransition
	{
		bool valid = false;
		OrderId orderId = 0;
		Generation generation = 0;
		IssuerId issuerId = 0;
		OrderRecipient recipient;
		OrderType orderType = OrderType::MOVE;
		TargetPosition target;

		OrderOperation operation = OrderOperation::ISSUE;
		bool hasUpdateKey = false;
		OrderKey updateKey;

		bool previousHasOrder = false;
		OrderState previousState = OrderState::ISSUED;
		OrderOutcome previousOutcome = OrderOutcome::NONE;
		bool newHasOrder = false;
		OrderState newState = OrderState::ISSUED;
		OrderOutcome newOutcome = OrderOutcome::NONE;

		DetailReason detailReason = DetailReason::NONE;
		UpdateStatus updateResult = UpdateStatus::NOT_ISSUED;
		bool mutationApplied = false;
		double timestamp = 0.0;
	};

	struct OrderSpec
	{
		OrderId orderId = 0;
		Generation generation = 0;
		IssuerId issuerId = 0;
		OrderRecipient recipient;
		OrderType type = OrderType::MOVE;
		TargetPosition target;
		double issuedAt = 0.0;
		double deadline = 0.0;
	};

	struct OrderRecord
	{
		// valid=false means that the protocol object has not been issued yet;
		// it is not an OrderState and is excluded from protocol invariants.
		bool valid = false;
		OrderId orderId = 0;
		Generation generation = 0;
		IssuerId issuerId = 0;
		OrderRecipient recipient;
		OrderType type = OrderType::MOVE;
		TargetPosition target;
		OrderState state = OrderState::ISSUED;
		OrderOutcome outcome = OrderOutcome::NONE;
		DetailReason detailReason = DetailReason::NONE;
		double issuedAt = 0.0;
		double deadline = 0.0;
	};

	struct UpdateResult
	{
		UpdateStatus status = UpdateStatus::NOT_ISSUED;
		bool changed = false;
		bool hasOrder = false;
		DetailReason rejectionReason = DetailReason::NONE;
		OrderRecord snapshot;
		OrderTransition transition;
	};

	bool IsTerminal(OrderState state);
	bool IsTerminalOutcome(OrderOutcome outcome);
	const char* OrderTypeName(OrderType type);
	const char* RecipientKindName(RecipientKind kind);
	const char* OrderStateName(OrderState state);
	const char* OrderOutcomeName(OrderOutcome outcome);
	const char* OrderOperationName(OrderOperation operation);
	const char* DetailReasonName(DetailReason reason);
	const char* UpdateStatusName(UpdateStatus status);

	class OrderProtocol
	{
	public:
		OrderProtocol() = default;

		bool HasOrder() const;
		OrderRecord Snapshot() const;

		UpdateResult Issue(const OrderSpec& spec);
		UpdateResult Acknowledge(const OrderKey& key, double now);
		UpdateResult BeginPlanning(const OrderKey& key, double now);
		UpdateResult BeginMoving(const OrderKey& key, double now);
		UpdateResult BeginExecuting(const OrderKey& key, double now);
		UpdateResult Complete(const OrderKey& key, double now);
		UpdateResult Fail(const OrderKey& key, OrderOutcome outcome,
			DetailReason reason, double now);
		UpdateResult Abort(const OrderKey& key, DetailReason reason, double now);
		// Replacement is intentionally local: it terminalizes this old order.
		// The caller creates and issues the replacement in another instance.
		UpdateResult Replace(const OrderKey& key, double now);
		UpdateResult Tick(const OrderKey& key, double now);

	private:
		OrderRecord m_record;
		bool m_hasOrder = false;

		UpdateResult MakeResult(OrderOperation operation, double timestamp,
			const OrderRecord& before, bool beforeHasOrder,
			const OrderKey& updateKey, bool hasUpdateKey, UpdateStatus status,
			bool changed, DetailReason rejectionReason = DetailReason::NONE) const;
		UpdateResult Transition(const OrderKey& key, OrderOperation operation,
			double now, OrderState expected, OrderState next);
		UpdateResult AbortInternal(const OrderKey& key, OrderOperation operation,
			double now, DetailReason reason);
		bool Matches(const OrderKey& key) const;
	};
}

#endif // __RCBOT_ORDER_PROTOCOL_H__
