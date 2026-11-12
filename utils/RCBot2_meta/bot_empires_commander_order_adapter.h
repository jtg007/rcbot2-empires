#ifndef __RCBOT_EMPIRES_COMMANDER_ORDER_ADAPTER_H__
#define __RCBOT_EMPIRES_COMMANDER_ORDER_ADAPTER_H__

#include "bot_order_protocol.h"
#include "bot_order_protocol_telemetry.h"

#include <cstdint>

namespace RCBotCommanderOrder
{
	// The runtime supplies a plain function-pointer sink.  This keeps the
	// adapter headless and avoids a framework hierarchy in the Commander path.
	using TelemetrySink = void (*)(void* context,
		const RCBotOrderTelemetry::TelemetryEvent& event);

	class CommanderMoveOrderAdapter
	{
	public:
		CommanderMoveOrderAdapter() = default;
		CommanderMoveOrderAdapter(TelemetrySink sink, void* context);

		void SetTelemetrySink(TelemetrySink sink, void* context);
		void Reset();

		bool HasActiveOrder() const;
		RCBotOrder::OrderKey ActiveKey() const;
		RCBotOrder::OrderRecord Snapshot() const;
		bool HasLastTerminalOrder() const;
		RCBotOrder::OrderRecord LastTerminalSnapshot() const;

		void ObserveNewMove(const RCBotOrder::TargetPosition& target,
			RCBotOrder::OrderId preferredOrderId,
			RCBotOrder::IssuerId issuerId,
			RCBotOrder::RecipientId recipientId,
			double now, double deadline);
		void ObserveNewMove(const RCBotOrder::TargetPosition& target,
			RCBotOrder::OrderId preferredOrderId,
			RCBotOrder::IssuerId issuerId,
			const RCBotOrder::OrderRecipient& recipient,
			double now, double deadline);
		void ObservePlanning(double now);
		void ObservePlanning(double now,
			const RCBotOrderTelemetry::RuntimeContext& context);
		void ObserveMovementStarted(double now);
		void ObserveMovementStarted(double now,
			const RCBotOrderTelemetry::RuntimeContext& context);
		void ObserveGoalExecution(double now);
		void ObserveGoalExecution(double now,
			const RCBotOrderTelemetry::RuntimeContext& context);
		bool ObserveComplete(double now);
		bool ObserveComplete(double now,
			const RCBotOrderTelemetry::RuntimeContext& context);
		bool ObserveFailure(RCBotOrder::OrderOutcome outcome,
			RCBotOrder::DetailReason reason, double now);
		bool ObserveFailure(RCBotOrder::OrderOutcome outcome,
			RCBotOrder::DetailReason reason, double now,
			const RCBotOrderTelemetry::RuntimeContext& context);
		bool ObserveReplacement(double now);
		bool ObserveAbort(RCBotOrder::DetailReason reason, double now);
		bool ObserveAbort(RCBotOrder::DetailReason reason, double now,
			const RCBotOrderTelemetry::RuntimeContext& context);
		// Consume a real terminal navigation result.  The mapping is deliberately
		// bounded: navigation owns retry/provider policy, the order only receives
		// the terminal semantic result.
		bool ObserveNavigationTerminal(
			const RCBotOrderTelemetry::RuntimeContext& context, double now);

		// This ticks only the Commander order deadline. NavigationAttempt timeout
		// and provider retry policy remain entirely in the navigation layer.
		bool Update(double now);

	private:
		void Emit(const RCBotOrder::UpdateResult& result);
		bool SetRuntimeContext(
			const RCBotOrderTelemetry::RuntimeContext& context);
		void RememberTerminal(const RCBotOrder::UpdateResult& result);
		void ClearActive();

		TelemetrySink m_sink = nullptr;
		void* m_sinkContext = nullptr;
		RCBotOrder::OrderProtocol m_protocol;
		RCBotOrder::OrderKey m_activeKey;
		bool m_active = false;
		bool m_hasLastTerminal = false;
		RCBotOrder::OrderRecord m_lastTerminal;
		std::uint64_t m_nextFallbackOrderId = 0;
		std::uint64_t m_nextGeneration = 0;
		std::uint64_t m_lastTerminalNavigationIntentId = 0;
		RCBotOrderTelemetry::RuntimeContext m_runtimeContext;
	};
}

#endif // __RCBOT_EMPIRES_COMMANDER_ORDER_ADAPTER_H__
