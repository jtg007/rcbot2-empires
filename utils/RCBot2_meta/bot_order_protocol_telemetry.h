#ifndef __RCBOT_ORDER_PROTOCOL_TELEMETRY_H__
#define __RCBOT_ORDER_PROTOCOL_TELEMETRY_H__

#include "bot_order_protocol.h"

#include <cstdint>
#include <string>

namespace RCBotOrderTelemetry
{
	enum class NavigationProvider : std::uint8_t
	{
		UNKNOWN = 0,
		WAYPOINT,
		NAVMESH,
		LOCAL_EXPLORER
	};

	enum class NavigationTerminalResult : std::uint8_t
	{
		NONE = 0,
		COMPLETE,
		CANCELLED,
		PATH_FAILED,
		PATH_STUCK,
		UNREACHABLE
	};

	enum class EventType : std::uint8_t
	{
		NONE = 0,
		ORDER_PROTOCOL_ISSUED,
		ORDER_PROTOCOL_TRANSITION,
		ORDER_PROTOCOL_REJECTED,
		ORDER_PROTOCOL_TERMINAL
	};

	// Runtime context is intentionally optional and value-only.  The adapter
	// attaches only IDs and results observed from the real navigation runtime.
	struct RuntimeContext
	{
		bool hasNavigationIntentId = false;
		std::uint64_t navigationIntentId = 0;
		bool hasNavigationAttemptId = false;
		std::uint64_t navigationAttemptId = 0;
		bool hasProvider = false;
		NavigationProvider provider = NavigationProvider::UNKNOWN;
		bool hasNavigationTerminalResult = false;
		NavigationTerminalResult navigationTerminalResult =
			NavigationTerminalResult::NONE;
	};

	struct OrderCorrelation
	{
		bool hasParentOrder = false;
		RCBotOrder::OrderKey parentOrder;
		bool hasMemberId = false;
		RCBotOrder::RecipientId memberId = 0;
		bool hasChildExecution = false;
		RCBotOrder::OrderKey childExecution;
		bool hasAggregateOutcome = false;
		RCBotOrder::OrderOutcome aggregateOutcome =
			RCBotOrder::OrderOutcome::NONE;
	};

	struct TelemetryEvent
	{
		EventType type = EventType::NONE;
		RCBotOrder::OrderTransition transition;
		RuntimeContext context;
		OrderCorrelation correlation;
	};

	EventType Classify(const RCBotOrder::OrderTransition& transition);
	TelemetryEvent BuildEvent(const RCBotOrder::UpdateResult& result);
	TelemetryEvent BuildEvent(const RCBotOrder::UpdateResult& result,
		const RuntimeContext& context);
	std::string Serialize(const TelemetryEvent& event);
	const char* EventTypeName(EventType type);
	const char* NavigationProviderName(NavigationProvider provider);
	const char* NavigationTerminalResultName(NavigationTerminalResult result);
}

#endif // __RCBOT_ORDER_PROTOCOL_TELEMETRY_H__
