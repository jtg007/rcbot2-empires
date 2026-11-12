#include "bot_empires_commander_order_adapter.h"

namespace RCBotCommanderOrder
{
CommanderMoveOrderAdapter::CommanderMoveOrderAdapter(
	const TelemetrySink sink, void* const context)
	: m_sink(sink), m_sinkContext(context)
{
}

void CommanderMoveOrderAdapter::SetTelemetrySink(
	const TelemetrySink sink, void* const context)
{
	m_sink = sink;
	m_sinkContext = context;
}

void CommanderMoveOrderAdapter::Reset()
{
	m_protocol = RCBotOrder::OrderProtocol();
	m_activeKey = RCBotOrder::OrderKey();
	m_active = false;
	m_hasLastTerminal = false;
	m_lastTerminal = RCBotOrder::OrderRecord();
	m_nextFallbackOrderId = 0;
	m_nextGeneration = 0;
	m_lastTerminalNavigationIntentId = 0;
	m_runtimeContext = RCBotOrderTelemetry::RuntimeContext{};
}

bool CommanderMoveOrderAdapter::HasActiveOrder() const
{
	return m_active;
}

RCBotOrder::OrderKey CommanderMoveOrderAdapter::ActiveKey() const
{
	return m_activeKey;
}

RCBotOrder::OrderRecord CommanderMoveOrderAdapter::Snapshot() const
{
	return m_protocol.Snapshot();
}

bool CommanderMoveOrderAdapter::HasLastTerminalOrder() const
{
	return m_hasLastTerminal;
}

RCBotOrder::OrderRecord CommanderMoveOrderAdapter::LastTerminalSnapshot() const
{
	return m_lastTerminal;
}

void CommanderMoveOrderAdapter::ObserveNewMove(
	const RCBotOrder::TargetPosition& target,
	const RCBotOrder::OrderId preferredOrderId,
	const RCBotOrder::IssuerId issuerId,
	const RCBotOrder::RecipientId recipientId,
	const double now, const double deadline)

{
	ObserveNewMove(target, preferredOrderId, issuerId,
		RCBotOrder::OrderRecipient::Bot(recipientId), now, deadline);
}

void CommanderMoveOrderAdapter::ObserveNewMove(
	const RCBotOrder::TargetPosition& target,
	const RCBotOrder::OrderId preferredOrderId,
	const RCBotOrder::IssuerId issuerId,
	const RCBotOrder::OrderRecipient& recipient,
	const double now, const double deadline)
{
	if (m_active)
		ObserveReplacement(now);

	// A completed or failed protocol instance is immutable.  Start the next
	// order in a fresh instance instead of rewriting the terminal record.
	m_protocol = RCBotOrder::OrderProtocol();
	m_runtimeContext = RCBotOrderTelemetry::RuntimeContext{};

	RCBotOrder::OrderId orderId = preferredOrderId;
	if (orderId == 0)
		orderId = ++m_nextFallbackOrderId;
	else if (orderId > m_nextFallbackOrderId)
		m_nextFallbackOrderId = orderId;

	const RCBotOrder::Generation generation = ++m_nextGeneration;
	const RCBotOrder::OrderSpec spec = {
		orderId,
		generation,
		issuerId,
		recipient,
		RCBotOrder::OrderType::MOVE,
		target,
		now,
		deadline
	};

	const RCBotOrder::UpdateResult issued = m_protocol.Issue(spec);
	Emit(issued);
	if (issued.status != RCBotOrder::UpdateStatus::APPLIED ||
		!issued.snapshot.valid ||
		RCBotOrder::IsTerminal(issued.snapshot.state))
	{
		RememberTerminal(issued);
		return;
	}

	m_active = true;
	m_activeKey.orderId = issued.snapshot.orderId;
	m_activeKey.generation = issued.snapshot.generation;
	const RCBotOrder::UpdateResult acknowledged =
		m_protocol.Acknowledge(m_activeKey, now);
	Emit(acknowledged);
}

void CommanderMoveOrderAdapter::ObservePlanning(const double now)
{
	ObservePlanning(now, RCBotOrderTelemetry::RuntimeContext{});
}

void CommanderMoveOrderAdapter::ObservePlanning(
	const double now, const RCBotOrderTelemetry::RuntimeContext& context)
{
	if (!m_active)
		return;
	if (!SetRuntimeContext(context))
		return;
	Emit(m_protocol.BeginPlanning(m_activeKey, now));
}

void CommanderMoveOrderAdapter::ObserveMovementStarted(const double now)
{
	ObserveMovementStarted(now, RCBotOrderTelemetry::RuntimeContext{});
}

void CommanderMoveOrderAdapter::ObserveMovementStarted(
	const double now, const RCBotOrderTelemetry::RuntimeContext& context)
{
	if (!m_active)
		return;
	if (!SetRuntimeContext(context))
		return;
	Emit(m_protocol.BeginMoving(m_activeKey, now));
}

void CommanderMoveOrderAdapter::ObserveGoalExecution(const double now)
{
	ObserveGoalExecution(now, RCBotOrderTelemetry::RuntimeContext{});
}

void CommanderMoveOrderAdapter::ObserveGoalExecution(
	const double now, const RCBotOrderTelemetry::RuntimeContext& context)
{
	if (!m_active)
		return;
	if (!SetRuntimeContext(context))
		return;
	Emit(m_protocol.BeginExecuting(m_activeKey, now));
}

bool CommanderMoveOrderAdapter::ObserveComplete(const double now)
{
	return ObserveComplete(now, RCBotOrderTelemetry::RuntimeContext{});
}

bool CommanderMoveOrderAdapter::ObserveComplete(
	const double now, const RCBotOrderTelemetry::RuntimeContext& context)
{
	if (!m_active)
		return false;
	if (!SetRuntimeContext(context))
		return false;
	const RCBotOrder::UpdateResult result =
		m_protocol.Complete(m_activeKey, now);
	Emit(result);
	if (result.status == RCBotOrder::UpdateStatus::APPLIED)
	{
		RememberTerminal(result);
		ClearActive();
		return true;
	}
	return false;
}

bool CommanderMoveOrderAdapter::ObserveFailure(
	const RCBotOrder::OrderOutcome outcome,
	const RCBotOrder::DetailReason reason, const double now)
{
	return ObserveFailure(outcome, reason, now,
		RCBotOrderTelemetry::RuntimeContext{});
}

bool CommanderMoveOrderAdapter::ObserveFailure(
	const RCBotOrder::OrderOutcome outcome,
	const RCBotOrder::DetailReason reason, const double now,
	const RCBotOrderTelemetry::RuntimeContext& context)
{
	if (!m_active)
		return false;
	if (!SetRuntimeContext(context))
		return false;
	const RCBotOrder::UpdateResult result =
		m_protocol.Fail(m_activeKey, outcome, reason, now);
	Emit(result);
	if (result.status == RCBotOrder::UpdateStatus::APPLIED)
	{
		RememberTerminal(result);
		ClearActive();
		return true;
	}
	return false;
}

bool CommanderMoveOrderAdapter::ObserveReplacement(const double now)
{
	if (!m_active)
		return false;
	if (m_runtimeContext.hasNavigationIntentId)
	{
		m_runtimeContext.hasNavigationTerminalResult = true;
		m_runtimeContext.navigationTerminalResult =
			RCBotOrderTelemetry::NavigationTerminalResult::CANCELLED;
	}
	const RCBotOrder::UpdateResult result =
		m_protocol.Replace(m_activeKey, now);
	Emit(result);
	if (result.status == RCBotOrder::UpdateStatus::APPLIED)
	{
		RememberTerminal(result);
		ClearActive();
		return true;
	}
	return false;
}

bool CommanderMoveOrderAdapter::ObserveAbort(
	const RCBotOrder::DetailReason reason, const double now)
{
	return ObserveAbort(reason, now, RCBotOrderTelemetry::RuntimeContext{});
}

bool CommanderMoveOrderAdapter::ObserveAbort(
	const RCBotOrder::DetailReason reason, const double now,
	const RCBotOrderTelemetry::RuntimeContext& context)
{
	if (!m_active)
		return false;
	if (!SetRuntimeContext(context))
		return false;
	const RCBotOrder::UpdateResult result =
		m_protocol.Abort(m_activeKey, reason, now);
	Emit(result);
	if (result.status == RCBotOrder::UpdateStatus::APPLIED)
	{
		RememberTerminal(result);
		ClearActive();
		return true;
	}
	return false;
}

bool CommanderMoveOrderAdapter::ObserveNavigationTerminal(
	const RCBotOrderTelemetry::RuntimeContext& context, const double now)
{
	if (!m_active)
		return false;
	if (!context.hasNavigationIntentId || context.navigationIntentId == 0 ||
		!context.hasNavigationTerminalResult ||
		context.navigationTerminalResult ==
			RCBotOrderTelemetry::NavigationTerminalResult::NONE)
		return false;

	if (!SetRuntimeContext(context))
		return false;
	switch (context.navigationTerminalResult)
	{
	case RCBotOrderTelemetry::NavigationTerminalResult::COMPLETE:
	{
		RCBotOrderTelemetry::RuntimeContext executingContext = m_runtimeContext;
		executingContext.hasNavigationTerminalResult = false;
		executingContext.navigationTerminalResult =
			RCBotOrderTelemetry::NavigationTerminalResult::NONE;
		m_runtimeContext = executingContext;
		RCBotOrder::OrderRecord snapshot = m_protocol.Snapshot();
		if (snapshot.state == RCBotOrder::OrderState::ACKNOWLEDGED)
		{
			ObservePlanning(now, executingContext);
			snapshot = m_protocol.Snapshot();
		}
		if (snapshot.state == RCBotOrder::OrderState::PLANNING)
		{
			ObserveMovementStarted(now, executingContext);
			snapshot = m_protocol.Snapshot();
		}
		if (snapshot.state == RCBotOrder::OrderState::MOVING)
		{
			ObserveGoalExecution(now, executingContext);
			snapshot = m_protocol.Snapshot();
		}
		if (snapshot.state != RCBotOrder::OrderState::EXECUTING)
			return false;
		return ObserveComplete(now, context);
	}
	case RCBotOrderTelemetry::NavigationTerminalResult::PATH_STUCK:
		return ObserveFailure(RCBotOrder::OrderOutcome::STUCK,
			RCBotOrder::DetailReason::FAILURE_REPORTED, now, context);
	case RCBotOrderTelemetry::NavigationTerminalResult::UNREACHABLE:
		return ObserveFailure(RCBotOrder::OrderOutcome::UNREACHABLE,
			RCBotOrder::DetailReason::FAILURE_REPORTED, now, context);
	case RCBotOrderTelemetry::NavigationTerminalResult::PATH_FAILED:
		return ObserveFailure(RCBotOrder::OrderOutcome::BLOCKED,
			RCBotOrder::DetailReason::FAILURE_REPORTED, now, context);
	case RCBotOrderTelemetry::NavigationTerminalResult::CANCELLED:
		return ObserveAbort(RCBotOrder::DetailReason::CANCELLED, now, context);
	case RCBotOrderTelemetry::NavigationTerminalResult::NONE:
		return false;
	}
	return false;
}

bool CommanderMoveOrderAdapter::Update(const double now)
{
	if (!m_active)
		return false;
	const RCBotOrder::UpdateResult result = m_protocol.Tick(m_activeKey, now);
	Emit(result);
	if (result.status == RCBotOrder::UpdateStatus::APPLIED && result.changed &&
		result.snapshot.valid &&
		RCBotOrder::IsTerminal(result.snapshot.state))
	{
		RememberTerminal(result);
		ClearActive();
		return true;
	}
	return false;
}

void CommanderMoveOrderAdapter::Emit(const RCBotOrder::UpdateResult& result)
{
	if (m_sink == nullptr)
		return;
	const RCBotOrderTelemetry::TelemetryEvent event =
		RCBotOrderTelemetry::BuildEvent(result, m_runtimeContext);
	if (event.type != RCBotOrderTelemetry::EventType::NONE)
		m_sink(m_sinkContext, event);
}

bool CommanderMoveOrderAdapter::SetRuntimeContext(
	const RCBotOrderTelemetry::RuntimeContext& context)
{
	if (!context.hasNavigationIntentId)
		return true;
	if (context.navigationIntentId == 0)
		return false;
	if (!m_runtimeContext.hasNavigationIntentId &&
		m_lastTerminalNavigationIntentId != 0 &&
		m_lastTerminalNavigationIntentId == context.navigationIntentId)
		return false;
	if (m_runtimeContext.hasNavigationIntentId &&
		m_runtimeContext.navigationIntentId != context.navigationIntentId)
		return false;
	if (m_runtimeContext.hasProvider && context.hasProvider &&
		m_runtimeContext.provider != context.provider)
	{
		// A provider switch is valid only when the navigation layer also gives
		// us a newer attempt for the same intent. A different provider on the
		// same attempt is stale/corrupt context.
		const bool newerAttempt = context.hasNavigationAttemptId &&
			(!m_runtimeContext.hasNavigationAttemptId ||
				context.navigationAttemptId > m_runtimeContext.navigationAttemptId);
		const bool terminalObservation = context.hasNavigationTerminalResult &&
			context.navigationTerminalResult !=
				RCBotOrderTelemetry::NavigationTerminalResult::NONE;
		if (!newerAttempt && !terminalObservation)
			return false;
	}
	if (m_runtimeContext.hasNavigationAttemptId &&
		context.hasNavigationAttemptId &&
		context.navigationAttemptId < m_runtimeContext.navigationAttemptId)
		return false;
	if (context.hasNavigationIntentId)
	{
		m_runtimeContext.hasNavigationIntentId = true;
		m_runtimeContext.navigationIntentId = context.navigationIntentId;
	}
	if (context.hasNavigationAttemptId)
	{
		m_runtimeContext.hasNavigationAttemptId = true;
		m_runtimeContext.navigationAttemptId = context.navigationAttemptId;
	}
	if (context.hasProvider)
	{
		m_runtimeContext.hasProvider = true;
		m_runtimeContext.provider = context.provider;
	}
	if (context.hasNavigationTerminalResult)
	{
		m_runtimeContext.hasNavigationTerminalResult = true;
		m_runtimeContext.navigationTerminalResult =
			context.navigationTerminalResult;
	}
	return true;
}

void CommanderMoveOrderAdapter::RememberTerminal(
	const RCBotOrder::UpdateResult& result)
{
	if (result.status == RCBotOrder::UpdateStatus::APPLIED &&
		result.snapshot.valid && RCBotOrder::IsTerminal(result.snapshot.state))
	{
		m_lastTerminal = result.snapshot;
		m_hasLastTerminal = true;
	}
}

void CommanderMoveOrderAdapter::ClearActive()
{
	if (m_runtimeContext.hasNavigationIntentId)
		m_lastTerminalNavigationIntentId = m_runtimeContext.navigationIntentId;
	m_active = false;
	m_activeKey = RCBotOrder::OrderKey();
}
}
