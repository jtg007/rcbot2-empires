#include "bot_order_protocol_telemetry.h"

#include <cmath>
#include <cstdio>

namespace RCBotOrderTelemetry
{
namespace
{
std::string Number(const double value)
{
	if (!std::isfinite(value))
		return "null";

	char buffer[64] = {};
	snprintf(buffer, sizeof(buffer), "%.6f", value);
	return buffer;
}

std::string Bool(const bool value)
{
	return value ? "true" : "false";
}

std::string State(const bool present, const RCBotOrder::OrderState state)
{
	return present ?
		"\"" + std::string(RCBotOrder::OrderStateName(state)) + "\"" : "null";
}

std::string Outcome(const bool present, const RCBotOrder::OrderOutcome outcome)
{
	return present ?
		"\"" + std::string(RCBotOrder::OrderOutcomeName(outcome)) + "\"" : "null";
}
}

const char* EventTypeName(const EventType type)
{
	switch (type)
	{
	case EventType::NONE: return "NONE";
	case EventType::ORDER_PROTOCOL_ISSUED: return "ORDER_PROTOCOL_ISSUED";
	case EventType::ORDER_PROTOCOL_TRANSITION: return "ORDER_PROTOCOL_TRANSITION";
	case EventType::ORDER_PROTOCOL_REJECTED: return "ORDER_PROTOCOL_REJECTED";
	case EventType::ORDER_PROTOCOL_TERMINAL: return "ORDER_PROTOCOL_TERMINAL";
	}
	return "UNKNOWN";
}

const char* NavigationProviderName(const NavigationProvider provider)
{
	switch (provider)
	{
	case NavigationProvider::UNKNOWN: return "UNKNOWN";
	case NavigationProvider::WAYPOINT: return "WAYPOINT";
	case NavigationProvider::NAVMESH: return "NAVMESH";
	case NavigationProvider::LOCAL_EXPLORER: return "LOCAL_EXPLORER";
	}
	return "UNKNOWN";
}

const char* NavigationTerminalResultName(
	const NavigationTerminalResult result)
{
	switch (result)
	{
	case NavigationTerminalResult::NONE: return "NONE";
	case NavigationTerminalResult::COMPLETE: return "COMPLETE";
	case NavigationTerminalResult::CANCELLED: return "CANCELLED";
	case NavigationTerminalResult::PATH_FAILED: return "PATH_FAILED";
	case NavigationTerminalResult::PATH_STUCK: return "PATH_STUCK";
	case NavigationTerminalResult::UNREACHABLE: return "UNREACHABLE";
	}
	return "NONE";
}

EventType Classify(const RCBotOrder::OrderTransition& transition)
{
	if (!transition.valid || transition.updateResult == RCBotOrder::UpdateStatus::NOT_ISSUED)
		return EventType::NONE;
	if (transition.updateResult != RCBotOrder::UpdateStatus::APPLIED)
		return EventType::ORDER_PROTOCOL_REJECTED;
	if (!transition.mutationApplied)
		return EventType::NONE;
	if (transition.operation == RCBotOrder::OrderOperation::ISSUE &&
		transition.newHasOrder && transition.newState == RCBotOrder::OrderState::ISSUED)
		return EventType::ORDER_PROTOCOL_ISSUED;
	if (transition.newHasOrder &&
		transition.newState == RCBotOrder::OrderState::TERMINAL)
		return EventType::ORDER_PROTOCOL_TERMINAL;
	return EventType::ORDER_PROTOCOL_TRANSITION;
}

TelemetryEvent BuildEvent(const RCBotOrder::UpdateResult& result)
{
	return BuildEvent(result, RuntimeContext{});
}

TelemetryEvent BuildEvent(const RCBotOrder::UpdateResult& result,
	const RuntimeContext& context)
{
	TelemetryEvent event;
	event.type = Classify(result.transition);
	event.transition = result.transition;
	event.context = context;
	return event;
}

std::string Serialize(const TelemetryEvent& event)
{
	if (event.type == EventType::NONE)
		return {};

	const RCBotOrder::OrderTransition& transition = event.transition;
	std::string result = std::string("{\"event\":\"") +
		EventTypeName(event.type) +
		"\",\"order_id\":" + std::to_string(transition.orderId) +
		",\"generation\":" + std::to_string(transition.generation) +
		",\"issuer_id\":" + std::to_string(transition.issuerId) +
		",\"recipient_kind\":\"" +
		RCBotOrder::RecipientKindName(transition.recipient.kind) +
		"\",\"recipient_id\":" +
		std::to_string(transition.recipient.id) +
		",\"recipient_generation\":" +
		std::to_string(transition.recipient.generation) +
		",\"order_type\":\"" +
		RCBotOrder::OrderTypeName(transition.orderType) +
		"\",\"target\":[" + Number(transition.target.x) + "," +
		Number(transition.target.y) + "," + Number(transition.target.z) +
		"],\"operation\":\"" +
		RCBotOrder::OrderOperationName(transition.operation) +
		"\",\"previous_state\":" +
		State(transition.previousHasOrder, transition.previousState) +
		",\"new_state\":" + State(transition.newHasOrder, transition.newState) +
		",\"previous_outcome\":" +
		Outcome(transition.previousHasOrder, transition.previousOutcome) +
		",\"new_outcome\":" + Outcome(transition.newHasOrder, transition.newOutcome) +
		",\"order_outcome\":" + Outcome(transition.newHasOrder, transition.newOutcome) +
		",\"detail_reason\":\"" +
		RCBotOrder::DetailReasonName(transition.detailReason) +
		"\",\"update_result\":\"" +
		RCBotOrder::UpdateStatusName(transition.updateResult) +
		"\",\"mutation_applied\":" + Bool(transition.mutationApplied) +
		",\"timestamp\":" + Number(transition.timestamp);

	if (transition.hasUpdateKey)
	{
		result += ",\"update_context\":{\"order_id\":" +
			std::to_string(transition.updateKey.orderId) +
			",\"generation\":" +
			std::to_string(transition.updateKey.generation) + "}";
	}
	if (transition.recipient.kind == RCBotOrder::RecipientKind::SQUAD)
	{
		result += ",\"squad_id\":" +
			std::to_string(transition.recipient.id) +
			",\"squad_generation\":" +
			std::to_string(transition.recipient.generation);
	}
	if (event.correlation.hasParentOrder)
	{
		result += ",\"parent_order_id\":" +
			std::to_string(event.correlation.parentOrder.orderId) +
			",\"parent_generation\":" +
			std::to_string(event.correlation.parentOrder.generation);
	}
	if (event.correlation.hasMemberId)
		result += ",\"member_id\":" +
			std::to_string(event.correlation.memberId);
	if (event.correlation.hasChildExecution)
	{
		result += ",\"child_execution_id\":" +
			std::to_string(event.correlation.childExecution.orderId) +
			",\"child_execution_generation\":" +
			std::to_string(event.correlation.childExecution.generation);
	}
	if (event.correlation.hasAggregateOutcome)
		result += ",\"parent_aggregate_result\":\"" +
			std::string(RCBotOrder::OrderOutcomeName(
				event.correlation.aggregateOutcome)) + "\"";
	if (event.context.hasNavigationIntentId)
		result += ",\"navigation_intent_id\":" +
			std::to_string(event.context.navigationIntentId);
	if (event.context.hasNavigationAttemptId)
		result += ",\"navigation_attempt_id\":" +
			std::to_string(event.context.navigationAttemptId);
	if (event.context.hasProvider)
		result += ",\"provider\":\"" +
			std::string(NavigationProviderName(event.context.provider)) + "\"";
	if (event.context.hasNavigationTerminalResult)
		result += ",\"navigation_terminal_result\":\"" +
			std::string(NavigationTerminalResultName(
				event.context.navigationTerminalResult)) + "\"";
	else if (event.type == EventType::ORDER_PROTOCOL_TERMINAL)
		result += ",\"navigation_terminal_result\":\"NONE\"";

	result += "}\n";
	return result;
}
}
