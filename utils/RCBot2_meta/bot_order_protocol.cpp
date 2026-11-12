#include "bot_order_protocol.h"

#include <cmath>

namespace RCBotOrder
{
namespace
{
DetailReason ValidateSpec(const OrderSpec& spec)
{
	if (spec.orderId == 0)
		return DetailReason::INVALID_ORDER_ID;
	if (spec.generation == 0)
		return DetailReason::INVALID_GENERATION;
	if (spec.issuerId == 0)
		return DetailReason::INVALID_ISSUER_ID;
	if (spec.recipient.id == 0)
		return DetailReason::INVALID_RECIPIENT_ID;
	if (spec.recipient.kind != RecipientKind::BOT &&
		spec.recipient.kind != RecipientKind::SQUAD)
		return DetailReason::INVALID_RECIPIENT_ID;
	if (spec.recipient.kind == RecipientKind::SQUAD &&
		spec.recipient.generation == 0)
		return DetailReason::INVALID_GENERATION;
	if (spec.type != OrderType::MOVE)
		return DetailReason::UNSUPPORTED_ORDER_TYPE;
	if (!spec.target.IsFinite())
		return DetailReason::NON_FINITE_TARGET;
	if (!std::isfinite(spec.issuedAt))
		return DetailReason::INVALID_ISSUED_AT;
	if (!std::isfinite(spec.deadline) || spec.deadline <= spec.issuedAt)
		return DetailReason::INVALID_DEADLINE;

	return DetailReason::NONE;
}

bool IsFailureOutcomeValue(const OrderOutcome outcome)
{
	switch (outcome)
	{
	case OrderOutcome::BLOCKED:
	case OrderOutcome::STUCK:
	case OrderOutcome::TARGET_LOST:
	case OrderOutcome::INVALID:
	case OrderOutcome::NEEDS_SUPPORT:
	case OrderOutcome::UNREACHABLE:
	case OrderOutcome::PARTIAL_FAILURE:
		return true;
	case OrderOutcome::NONE:
	case OrderOutcome::COMPLETE:
	case OrderOutcome::ABORTED:
		return false;
	}
	return false;
}
}

bool OrderRecipient::IsValid() const
{
	if (id == 0)
		return false;
	if (kind == RecipientKind::BOT)
		return true;
	return kind == RecipientKind::SQUAD && generation != 0;
}

bool TargetPosition::IsFinite() const
{
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool IsTerminal(const OrderState state)
{
	return state == OrderState::TERMINAL;
}

bool IsTerminalOutcome(const OrderOutcome outcome)
{
	return outcome != OrderOutcome::NONE;
}

const char* OrderTypeName(const OrderType type)
{
	switch (type)
	{
	case OrderType::MOVE: return "MOVE";
	}
	return "UNKNOWN";
}

const char* RecipientKindName(const RecipientKind kind)
{
	switch (kind)
	{
	case RecipientKind::BOT: return "BOT";
	case RecipientKind::SQUAD: return "SQUAD";
	}
	return "UNKNOWN";
}

const char* OrderStateName(const OrderState state)
{
	switch (state)
	{
	case OrderState::ISSUED: return "ISSUED";
	case OrderState::ACKNOWLEDGED: return "ACKNOWLEDGED";
	case OrderState::PLANNING: return "PLANNING";
	case OrderState::MOVING: return "MOVING";
	case OrderState::EXECUTING: return "EXECUTING";
	case OrderState::TERMINAL: return "TERMINAL";
	}
	return "UNKNOWN";
}

const char* OrderOutcomeName(const OrderOutcome outcome)
{
	switch (outcome)
	{
	case OrderOutcome::NONE: return "NONE";
	case OrderOutcome::COMPLETE: return "COMPLETE";
	case OrderOutcome::BLOCKED: return "BLOCKED";
	case OrderOutcome::STUCK: return "STUCK";
	case OrderOutcome::TARGET_LOST: return "TARGET_LOST";
	case OrderOutcome::INVALID: return "INVALID";
	case OrderOutcome::NEEDS_SUPPORT: return "NEEDS_SUPPORT";
	case OrderOutcome::UNREACHABLE: return "UNREACHABLE";
	case OrderOutcome::PARTIAL_FAILURE: return "PARTIAL_FAILURE";
	case OrderOutcome::ABORTED: return "ABORTED";
	}
	return "UNKNOWN";
}

const char* OrderOperationName(const OrderOperation operation)
{
	switch (operation)
	{
	case OrderOperation::ISSUE: return "ISSUE";
	case OrderOperation::ACKNOWLEDGE: return "ACKNOWLEDGE";
	case OrderOperation::BEGIN_PLANNING: return "BEGIN_PLANNING";
	case OrderOperation::BEGIN_MOVING: return "BEGIN_MOVING";
	case OrderOperation::BEGIN_EXECUTING: return "BEGIN_EXECUTING";
	case OrderOperation::COMPLETE: return "COMPLETE";
	case OrderOperation::FAIL: return "FAIL";
	case OrderOperation::ABORT: return "ABORT";
	case OrderOperation::REPLACE: return "REPLACE";
	case OrderOperation::TICK: return "TICK";
	}
	return "UNKNOWN";
}

const char* DetailReasonName(const DetailReason reason)
{
	switch (reason)
	{
	case DetailReason::NONE: return "NONE";
	case DetailReason::INVALID_ORDER_ID: return "INVALID_ORDER_ID";
	case DetailReason::INVALID_GENERATION: return "INVALID_GENERATION";
	case DetailReason::INVALID_ISSUER_ID: return "INVALID_ISSUER_ID";
	case DetailReason::INVALID_RECIPIENT_ID: return "INVALID_RECIPIENT_ID";
	case DetailReason::UNSUPPORTED_ORDER_TYPE: return "UNSUPPORTED_ORDER_TYPE";
	case DetailReason::NON_FINITE_TARGET: return "NON_FINITE_TARGET";
	case DetailReason::INVALID_ISSUED_AT: return "INVALID_ISSUED_AT";
	case DetailReason::INVALID_DEADLINE: return "INVALID_DEADLINE";
	case DetailReason::DEADLINE_EXPIRED: return "DEADLINE_EXPIRED";
	case DetailReason::REPLACED: return "REPLACED";
	case DetailReason::DEATH_OR_LIFECYCLE: return "DEATH_OR_LIFECYCLE";
	case DetailReason::CANCELLED: return "CANCELLED";
	case DetailReason::FAILURE_REPORTED: return "FAILURE_REPORTED";
	case DetailReason::MISSING_REASON: return "MISSING_REASON";
	case DetailReason::INVALID_FAILURE_OUTCOME: return "INVALID_FAILURE_OUTCOME";
	case DetailReason::INVALID_PROTOCOL_TIME: return "INVALID_TIME";
	case DetailReason::STALE_CONTEXT: return "STALE_CONTEXT";
	case DetailReason::CHILDREN_AGGREGATED: return "CHILDREN_AGGREGATED";
	case DetailReason::CHILDREN_EXHAUSTED: return "CHILDREN_EXHAUSTED";
	}
	return "UNKNOWN";
}

const char* UpdateStatusName(const UpdateStatus status)
{
	switch (status)
	{
	case UpdateStatus::APPLIED: return "APPLIED";
	case UpdateStatus::DUPLICATE: return "DUPLICATE";
	case UpdateStatus::STALE: return "STALE";
	case UpdateStatus::ILLEGAL_TRANSITION: return "ILLEGAL_TRANSITION";
	case UpdateStatus::ALREADY_TERMINAL: return "ALREADY_TERMINAL";
	case UpdateStatus::NOT_ISSUED: return "NOT_ISSUED";
	}
	return "UNKNOWN";
}

bool OrderProtocol::HasOrder() const
{
	return m_hasOrder;
}

OrderRecord OrderProtocol::Snapshot() const
{
	return m_record;
}

UpdateResult OrderProtocol::MakeResult(const OrderOperation operation,
	const double timestamp, const OrderRecord& before, const bool beforeHasOrder,
	const OrderKey& updateKey, const bool hasUpdateKey, const UpdateStatus status,
	const bool changed, const DetailReason rejectionReason) const
{
	UpdateResult result;
	result.status = status;
	result.changed = changed;
	result.hasOrder = m_hasOrder;
	result.rejectionReason = rejectionReason;
	result.snapshot = m_record;

	OrderTransition& transition = result.transition;
	transition.valid = m_hasOrder || beforeHasOrder;
	transition.operation = operation;
	transition.hasUpdateKey = hasUpdateKey;
	transition.updateKey = updateKey;
	transition.previousHasOrder = beforeHasOrder;
	if (beforeHasOrder)
	{
		transition.previousState = before.state;
		transition.previousOutcome = before.outcome;
	}
	transition.newHasOrder = m_hasOrder;
	if (m_hasOrder)
	{
		transition.newState = m_record.state;
		transition.newOutcome = m_record.outcome;
	}
	transition.updateResult = status;
	transition.mutationApplied = changed;
	transition.timestamp = timestamp;

	const OrderRecord* identity = m_hasOrder ? &m_record :
		(beforeHasOrder ? &before : nullptr);
	if (identity != nullptr)
	{
		transition.orderId = identity->orderId;
		transition.generation = identity->generation;
		transition.issuerId = identity->issuerId;
		transition.recipient = identity->recipient;
		transition.orderType = identity->type;
		transition.target = identity->target;
	}
	transition.detailReason = rejectionReason != DetailReason::NONE ? rejectionReason :
		(identity != nullptr ? identity->detailReason : DetailReason::NONE);

	return result;
}

bool OrderProtocol::Matches(const OrderKey& key) const
{
	return m_hasOrder && key.orderId == m_record.orderId &&
		key.generation == m_record.generation;
}

UpdateResult OrderProtocol::Issue(const OrderSpec& spec)
{
	const OrderRecord before = m_record;
	const bool beforeHasOrder = m_hasOrder;
	const OrderKey noUpdateKey;
	if (m_hasOrder)
	{
		if (IsTerminal(m_record.state))
			return MakeResult(OrderOperation::ISSUE, spec.issuedAt, before,
				beforeHasOrder, noUpdateKey, false, UpdateStatus::ALREADY_TERMINAL, false);
		return MakeResult(OrderOperation::ISSUE, spec.issuedAt, before,
			beforeHasOrder, noUpdateKey, false, UpdateStatus::ILLEGAL_TRANSITION, false);
	}

	m_record.valid = true;
	m_record.orderId = spec.orderId;
	m_record.generation = spec.generation;
	m_record.issuerId = spec.issuerId;
	m_record.recipient = spec.recipient;
	m_record.type = spec.type;
	m_record.target = spec.target;
	m_record.state = OrderState::ISSUED;
	m_record.outcome = OrderOutcome::NONE;
	m_record.detailReason = DetailReason::NONE;
	m_record.issuedAt = spec.issuedAt;
	m_record.deadline = spec.deadline;
	m_hasOrder = true;

	const DetailReason invalidReason = ValidateSpec(spec);
	if (invalidReason != DetailReason::NONE)
	{
		m_record.state = OrderState::TERMINAL;
		m_record.outcome = OrderOutcome::INVALID;
		m_record.detailReason = invalidReason;
	}

	return MakeResult(OrderOperation::ISSUE, spec.issuedAt, before, beforeHasOrder,
		noUpdateKey, false, UpdateStatus::APPLIED, true);
}

UpdateResult OrderProtocol::Transition(const OrderKey& key, const OrderOperation operation,
	const double now, const OrderState expected, const OrderState next)
{
	const OrderRecord before = m_record;
	const bool beforeHasOrder = m_hasOrder;
	if (!m_hasOrder)
		return MakeResult(operation, now, before, beforeHasOrder, key, true,
			UpdateStatus::NOT_ISSUED, false);
	if (!Matches(key))
		return MakeResult(operation, now, before, beforeHasOrder, key, true,
			UpdateStatus::STALE, false, DetailReason::STALE_CONTEXT);
	if (IsTerminal(m_record.state))
		return MakeResult(operation, now, before, beforeHasOrder, key, true,
			UpdateStatus::ALREADY_TERMINAL, false);
	if (m_record.state == next)
		return MakeResult(operation, now, before, beforeHasOrder, key, true,
			UpdateStatus::DUPLICATE, false);
	if (m_record.state != expected)
		return MakeResult(operation, now, before, beforeHasOrder, key, true,
			UpdateStatus::ILLEGAL_TRANSITION, false);

	m_record.state = next;
	return MakeResult(operation, now, before, beforeHasOrder, key, true,
		UpdateStatus::APPLIED, true);
}

UpdateResult OrderProtocol::Acknowledge(const OrderKey& key, const double now)
{
	return Transition(key, OrderOperation::ACKNOWLEDGE, now,
		OrderState::ISSUED, OrderState::ACKNOWLEDGED);
}

UpdateResult OrderProtocol::BeginPlanning(const OrderKey& key, const double now)
{
	return Transition(key, OrderOperation::BEGIN_PLANNING, now,
		OrderState::ACKNOWLEDGED, OrderState::PLANNING);
}

UpdateResult OrderProtocol::BeginMoving(const OrderKey& key, const double now)
{
	return Transition(key, OrderOperation::BEGIN_MOVING, now,
		OrderState::PLANNING, OrderState::MOVING);
}

UpdateResult OrderProtocol::BeginExecuting(const OrderKey& key, const double now)
{
	return Transition(key, OrderOperation::BEGIN_EXECUTING, now,
		OrderState::MOVING, OrderState::EXECUTING);
}

UpdateResult OrderProtocol::Complete(const OrderKey& key, const double now)
{
	const OrderRecord before = m_record;
	const bool beforeHasOrder = m_hasOrder;
	if (!m_hasOrder)
		return MakeResult(OrderOperation::COMPLETE, now, before, beforeHasOrder, key, true,
			UpdateStatus::NOT_ISSUED, false);
	if (!Matches(key))
		return MakeResult(OrderOperation::COMPLETE, now, before, beforeHasOrder, key, true,
			UpdateStatus::STALE, false, DetailReason::STALE_CONTEXT);
	if (IsTerminal(m_record.state))
		return MakeResult(OrderOperation::COMPLETE, now, before, beforeHasOrder, key, true,
			UpdateStatus::ALREADY_TERMINAL, false);
	if (m_record.state != OrderState::EXECUTING)
		return MakeResult(OrderOperation::COMPLETE, now, before, beforeHasOrder, key, true,
			UpdateStatus::ILLEGAL_TRANSITION, false);

	m_record.state = OrderState::TERMINAL;
	m_record.outcome = OrderOutcome::COMPLETE;
	m_record.detailReason = DetailReason::NONE;
	return MakeResult(OrderOperation::COMPLETE, now, before, beforeHasOrder, key, true,
		UpdateStatus::APPLIED, true);
}

UpdateResult OrderProtocol::Fail(const OrderKey& key, const OrderOutcome outcome,
	const DetailReason reason, const double now)
{
	const OrderRecord before = m_record;
	const bool beforeHasOrder = m_hasOrder;
	if (!m_hasOrder)
		return MakeResult(OrderOperation::FAIL, now, before, beforeHasOrder, key, true,
			UpdateStatus::NOT_ISSUED, false);
	if (!Matches(key))
		return MakeResult(OrderOperation::FAIL, now, before, beforeHasOrder, key, true,
			UpdateStatus::STALE, false, DetailReason::STALE_CONTEXT);
	if (IsTerminal(m_record.state))
		return MakeResult(OrderOperation::FAIL, now, before, beforeHasOrder, key, true,
			UpdateStatus::ALREADY_TERMINAL, false);
	if (!IsFailureOutcomeValue(outcome))
		return MakeResult(OrderOperation::FAIL, now, before, beforeHasOrder, key, true,
			UpdateStatus::ILLEGAL_TRANSITION, false, DetailReason::INVALID_FAILURE_OUTCOME);
	if (reason == DetailReason::NONE)
		return MakeResult(OrderOperation::FAIL, now, before, beforeHasOrder, key, true,
			UpdateStatus::ILLEGAL_TRANSITION, false, DetailReason::MISSING_REASON);

	m_record.state = OrderState::TERMINAL;
	m_record.outcome = outcome;
	m_record.detailReason = reason;
	return MakeResult(OrderOperation::FAIL, now, before, beforeHasOrder, key, true,
		UpdateStatus::APPLIED, true);
}

UpdateResult OrderProtocol::AbortInternal(const OrderKey& key,
	const OrderOperation operation, const double now, const DetailReason reason)
{
	const OrderRecord before = m_record;
	const bool beforeHasOrder = m_hasOrder;
	if (!m_hasOrder)
		return MakeResult(operation, now, before, beforeHasOrder, key, true,
			UpdateStatus::NOT_ISSUED, false);
	if (!Matches(key))
		return MakeResult(operation, now, before, beforeHasOrder, key, true,
			UpdateStatus::STALE, false, DetailReason::STALE_CONTEXT);
	if (IsTerminal(m_record.state))
		return MakeResult(operation, now, before, beforeHasOrder, key, true,
			UpdateStatus::ALREADY_TERMINAL, false);
	if (reason == DetailReason::NONE)
		return MakeResult(operation, now, before, beforeHasOrder, key, true,
			UpdateStatus::ILLEGAL_TRANSITION, false, DetailReason::MISSING_REASON);

	m_record.state = OrderState::TERMINAL;
	m_record.outcome = OrderOutcome::ABORTED;
	m_record.detailReason = reason;
	return MakeResult(operation, now, before, beforeHasOrder, key, true,
		UpdateStatus::APPLIED, true);
}

UpdateResult OrderProtocol::Abort(const OrderKey& key, const DetailReason reason,
	const double now)
{
	return AbortInternal(key, OrderOperation::ABORT, now, reason);
}

UpdateResult OrderProtocol::Replace(const OrderKey& key, const double now)
{
	return AbortInternal(key, OrderOperation::REPLACE, now, DetailReason::REPLACED);
}

UpdateResult OrderProtocol::Tick(const OrderKey& key, const double now)
{
	const OrderRecord before = m_record;
	const bool beforeHasOrder = m_hasOrder;
	if (!m_hasOrder)
		return MakeResult(OrderOperation::TICK, now, before, beforeHasOrder, key, true,
			UpdateStatus::NOT_ISSUED, false);
	if (!Matches(key))
		return MakeResult(OrderOperation::TICK, now, before, beforeHasOrder, key, true,
			UpdateStatus::STALE, false, DetailReason::STALE_CONTEXT);
	if (IsTerminal(m_record.state))
		return MakeResult(OrderOperation::TICK, now, before, beforeHasOrder, key, true,
			UpdateStatus::ALREADY_TERMINAL, false);
	if (!std::isfinite(now))
		return MakeResult(OrderOperation::TICK, now, before, beforeHasOrder, key, true,
			UpdateStatus::ILLEGAL_TRANSITION, false,
			DetailReason::INVALID_PROTOCOL_TIME);
	if (now <= m_record.deadline)
		return MakeResult(OrderOperation::TICK, now, before, beforeHasOrder, key, true,
			UpdateStatus::APPLIED, false);

	m_record.state = OrderState::TERMINAL;
	m_record.outcome = OrderOutcome::ABORTED;
	m_record.detailReason = DetailReason::DEADLINE_EXPIRED;
	return MakeResult(OrderOperation::TICK, now, before, beforeHasOrder, key, true,
		UpdateStatus::APPLIED, true);
}
}
