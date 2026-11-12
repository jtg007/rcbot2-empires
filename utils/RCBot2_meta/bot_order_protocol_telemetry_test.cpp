#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "bot_order_protocol_telemetry.h"

using namespace RCBotOrder;
using namespace RCBotOrderTelemetry;

static OrderSpec MakeSpec(const OrderId orderId = 1, const Generation generation = 1)
{
	return {
		orderId,
		generation,
		10,
		OrderRecipient::Bot(20),
		OrderType::MOVE,
		{ 100.0f, 200.0f, 300.0f },
		100.0,
		200.0
	};
}

static OrderKey Key(const OrderSpec& spec)
{
	return { spec.orderId, spec.generation };
}

static void PrintPass(const char* name)
{
	std::cout << name << " PASS" << std::endl;
}

static void AssertNoMutation(const OrderRecord& before, const UpdateResult& result)
{
	assert(!result.changed);
	assert(result.snapshot.state == before.state);
	assert(result.snapshot.outcome == before.outcome);
	assert(result.snapshot.detailReason == before.detailReason);
	assert(result.transition.mutationApplied == false);
	assert(result.transition.previousState == result.transition.newState);
	assert(result.transition.previousOutcome == result.transition.newOutcome);
}

static std::vector<std::string> SuccessfulEvents(OrderProtocol& protocol,
	const OrderSpec& spec)
{
	const OrderKey key = Key(spec);
	std::vector<std::string> events;
	events.push_back(Serialize(BuildEvent(protocol.Issue(spec))));
	events.push_back(Serialize(BuildEvent(protocol.Acknowledge(key, 101.0))));
	events.push_back(Serialize(BuildEvent(protocol.BeginPlanning(key, 102.0))));
	events.push_back(Serialize(BuildEvent(protocol.BeginMoving(key, 103.0))));
	events.push_back(Serialize(BuildEvent(protocol.BeginExecuting(key, 104.0))));
	events.push_back(Serialize(BuildEvent(protocol.Complete(key, 105.0))));
	return events;
}

int main()
{
	// ORDER-TEL-01: successful events preserve the applied operation order.
	{
		const OrderSpec spec = MakeSpec();
		OrderProtocol protocol;
		const OrderKey key = Key(spec);
		const UpdateResult issued = protocol.Issue(spec);
		const UpdateResult acknowledged = protocol.Acknowledge(key, 101.0);
		const UpdateResult planning = protocol.BeginPlanning(key, 102.0);
		const UpdateResult moving = protocol.BeginMoving(key, 103.0);
		const UpdateResult executing = protocol.BeginExecuting(key, 104.0);
		const UpdateResult complete = protocol.Complete(key, 105.0);
		assert(BuildEvent(issued).type == EventType::ORDER_PROTOCOL_ISSUED);
		assert(BuildEvent(acknowledged).type == EventType::ORDER_PROTOCOL_TRANSITION);
		assert(BuildEvent(planning).type == EventType::ORDER_PROTOCOL_TRANSITION);
		assert(BuildEvent(moving).type == EventType::ORDER_PROTOCOL_TRANSITION);
		assert(BuildEvent(executing).type == EventType::ORDER_PROTOCOL_TRANSITION);
		assert(BuildEvent(complete).type == EventType::ORDER_PROTOCOL_TERMINAL);
		assert(complete.transition.operation == OrderOperation::COMPLETE);
		assert(complete.transition.timestamp == 105.0);
		PrintPass("ORDER-TEL-01");
	}

	// ORDER-TEL-02: COMPLETE is explicit in the terminal event.
	{
		const OrderSpec spec = MakeSpec(2, 1);
		OrderProtocol protocol;
		const std::vector<std::string> events = SuccessfulEvents(protocol, spec);
		assert(events.back().find("\"event\":\"ORDER_PROTOCOL_TERMINAL\"") !=
			std::string::npos);
		assert(events.back().find("\"new_outcome\":\"COMPLETE\"") !=
			std::string::npos);
		PrintPass("ORDER-TEL-02");
	}

	// ORDER-TEL-03: failures carry both outcome and detail reason.
	{
		const OrderSpec spec = MakeSpec(3, 1);
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		const UpdateResult failed = protocol.Fail(Key(spec), OrderOutcome::STUCK,
			DetailReason::FAILURE_REPORTED, 101.0);
		const std::string event = Serialize(BuildEvent(failed));
		assert(BuildEvent(failed).type == EventType::ORDER_PROTOCOL_TERMINAL);
		assert(event.find("\"new_outcome\":\"STUCK\"") != std::string::npos);
		assert(event.find("\"detail_reason\":\"FAILURE_REPORTED\"") !=
			std::string::npos);
		PrintPass("ORDER-TEL-03");
	}

	// ORDER-TEL-04: stale updates are observable and do not mutate state.
	{
		const OrderSpec spec = MakeSpec(4, 7);
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		const OrderRecord before = protocol.Snapshot();
		const UpdateResult stale = protocol.BeginPlanning(
			{ spec.orderId, spec.generation + 1 }, 101.0);
		const TelemetryEvent event = BuildEvent(stale);
		AssertNoMutation(before, stale);
		assert(event.type == EventType::ORDER_PROTOCOL_REJECTED);
		assert(stale.transition.updateResult == UpdateStatus::STALE);
		const std::string serialized = Serialize(event);
		assert(serialized.find("\"update_result\":\"STALE\"") !=
			std::string::npos);
		assert(serialized.find("\"detail_reason\":\"STALE_CONTEXT\"") !=
			std::string::npos);
		PrintPass("ORDER-TEL-04");
	}

	// ORDER-TEL-05: illegal transitions are rejected by the actual result.
	{
		const OrderSpec spec = MakeSpec(5, 1);
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		const OrderRecord before = protocol.Snapshot();
		const UpdateResult illegal = protocol.BeginMoving(Key(spec), 101.0);
		AssertNoMutation(before, illegal);
		assert(BuildEvent(illegal).type == EventType::ORDER_PROTOCOL_REJECTED);
		assert(illegal.transition.updateResult == UpdateStatus::ILLEGAL_TRANSITION);
		PrintPass("ORDER-TEL-05");
	}

	// ORDER-TEL-06: replacement creates two separate correlated records.
	{
		const OrderSpec oldSpec = MakeSpec(6, 1);
		const OrderSpec newSpec = MakeSpec(7, 2);
		OrderProtocol oldProtocol;
		assert(oldProtocol.Issue(oldSpec).status == UpdateStatus::APPLIED);
		const UpdateResult replaced = oldProtocol.Replace(Key(oldSpec), 101.0);
		const UpdateResult issued = OrderProtocol().Issue(newSpec);
		const std::string oldEvent = Serialize(BuildEvent(replaced));
		const std::string newEvent = Serialize(BuildEvent(issued));
		assert(BuildEvent(replaced).type == EventType::ORDER_PROTOCOL_TERMINAL);
		assert(replaced.transition.newOutcome == OrderOutcome::ABORTED);
		assert(replaced.transition.detailReason == DetailReason::REPLACED);
		assert(oldEvent.find("\"order_id\":6") != std::string::npos);
		assert(newEvent.find("\"order_id\":7") != std::string::npos);
		assert(newEvent.find("\"event\":\"ORDER_PROTOCOL_ISSUED\"") !=
			std::string::npos);
		PrintPass("ORDER-TEL-06");
	}

	// ORDER-TEL-07: deadline uses the core's ABORTED/DEADLINE_EXPIRED result.
	{
		OrderSpec spec = MakeSpec(8, 1);
		spec.issuedAt = 10.0;
		spec.deadline = 20.0;
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		const UpdateResult expired = protocol.Tick(Key(spec), 20.001);
		const std::string event = Serialize(BuildEvent(expired));
		assert(expired.transition.newOutcome == OrderOutcome::ABORTED);
		assert(expired.transition.detailReason == DetailReason::DEADLINE_EXPIRED);
		assert(event.find("\"new_outcome\":\"ABORTED\"") != std::string::npos);
		assert(event.find("\"detail_reason\":\"DEADLINE_EXPIRED\"") !=
			std::string::npos);
		PrintPass("ORDER-TEL-07");
	}

	// ORDER-TEL-08: updates after terminal are rejected as ALREADY_TERMINAL.
	{
		const OrderSpec spec = MakeSpec(9, 1);
		OrderProtocol protocol;
		assert(protocol.Issue(spec).status == UpdateStatus::APPLIED);
		assert(protocol.Abort(Key(spec), DetailReason::EXPLICIT_ABORT, 101.0).status ==
			UpdateStatus::APPLIED);
		const OrderRecord before = protocol.Snapshot();
		const UpdateResult rejected = protocol.BeginPlanning(Key(spec), 102.0);
		AssertNoMutation(before, rejected);
		assert(BuildEvent(rejected).type == EventType::ORDER_PROTOCOL_REJECTED);
		assert(rejected.transition.updateResult == UpdateStatus::ALREADY_TERMINAL);
		PrintPass("ORDER-TEL-08");
	}

	// ORDER-TEL-09: identity fields remain stable throughout a lifecycle.
	{
		const OrderSpec spec = MakeSpec(10, 12);
		OrderProtocol protocol;
		const std::vector<std::string> events = SuccessfulEvents(protocol, spec);
		for (const std::string& event : events)
		{
			assert(event.find("\"order_id\":10") != std::string::npos);
			assert(event.find("\"generation\":12") != std::string::npos);
			assert(event.find("\"issuer_id\":10") != std::string::npos);
			assert(event.find("\"recipient_id\":20") != std::string::npos);
		}
		PrintPass("ORDER-TEL-09");
	}

	// ORDER-TEL-10: the same transition sequence serializes identically.
	{
		const OrderSpec spec = MakeSpec(11, 3);
		OrderProtocol left;
		OrderProtocol right;
		const std::vector<std::string> leftEvents = SuccessfulEvents(left, spec);
		const std::vector<std::string> rightEvents = SuccessfulEvents(right, spec);
		assert(leftEvents == rightEvents);
		PrintPass("ORDER-TEL-10");
	}

	std::cout << "RESULT 10/10 PASS" << std::endl;
	return 0;
}
