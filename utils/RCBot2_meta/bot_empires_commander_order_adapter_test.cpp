#include "bot_empires_commander_order_adapter.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
using namespace RCBotCommanderOrder;
using namespace RCBotOrder;
using RCBotOrderTelemetry::EventType;
using RCBotOrderTelemetry::TelemetryEvent;

struct Capture
{
	std::vector<TelemetryEvent> events;
};

void CaptureEvent(void* context, const TelemetryEvent& event)
{
	static_cast<Capture*>(context)->events.push_back(event);
}

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::exit(1);
	}
}

TargetPosition Target(const float x, const float y, const float z)
{
	TargetPosition target;
	target.x = x;
	target.y = y;
	target.z = z;
	return target;
}

void IssueAndMove(CommanderMoveOrderAdapter& adapter,
	const TargetPosition& target, const OrderId preferredId,
	const double now = 10.0)
{
	adapter.ObserveNewMove(target, preferredId, 1, 42, now, now + 300.0);
	adapter.ObservePlanning(now + 1.0);
	adapter.ObserveMovementStarted(now + 2.0);
}

void TestSuccessLifecycle()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	const TargetPosition target = Target(100.0f, 200.0f, 300.0f);
	IssueAndMove(adapter, target, 7001);
	adapter.ObserveGoalExecution(13.0);
	adapter.ObserveComplete(13.0);

	Require(capture.events.size() == 6, "SHADOW-01 event count");
	Require(capture.events[0].type == EventType::ORDER_PROTOCOL_ISSUED,
		"SHADOW-01 issued");
	Require(capture.events[1].type == EventType::ORDER_PROTOCOL_TRANSITION,
		"SHADOW-01 acknowledged");
	Require(capture.events[2].transition.newState == OrderState::PLANNING,
		"SHADOW-01 planning");
	Require(capture.events[3].transition.newState == OrderState::MOVING,
		"SHADOW-01 moving");
	Require(capture.events[4].transition.newState == OrderState::EXECUTING,
		"SHADOW-01 executing");
	Require(capture.events[5].type == EventType::ORDER_PROTOCOL_TERMINAL &&
		capture.events[5].transition.newOutcome == OrderOutcome::COMPLETE,
		"SHADOW-01 complete");
	Require(capture.events[5].transition.orderId == 7001 &&
		capture.events[5].transition.generation == 1,
		"SHADOW-01 identity stable");
	Require(capture.events[0].transition.target.x == target.x &&
		capture.events[0].transition.target.y == target.y &&
		capture.events[0].transition.target.z == target.z,
		"SHADOW-01 exact target");
	Require(!adapter.HasActiveOrder() && adapter.HasLastTerminalOrder(),
		"SHADOW-01 terminal cleanup");
}

void TestReplacement()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	IssueAndMove(adapter, Target(1.0f, 2.0f, 3.0f), 8001);
	adapter.ObserveReplacement(20.0);
	adapter.ObserveNewMove(Target(4.0f, 5.0f, 6.0f), 8002, 1, 42,
		20.0, 320.0);

	Require(capture.events.size() == 7, "SHADOW-02 event count");
	Require(capture.events[4].type == EventType::ORDER_PROTOCOL_TERMINAL &&
		capture.events[4].transition.newOutcome == OrderOutcome::ABORTED &&
		capture.events[4].transition.detailReason == DetailReason::REPLACED,
		"SHADOW-02 old order replaced");
	Require(capture.events[5].type == EventType::ORDER_PROTOCOL_ISSUED &&
		capture.events[5].transition.orderId == 8002,
		"SHADOW-02 new order issued");
	Require(capture.events[4].transition.orderId != capture.events[5].transition.orderId,
		"SHADOW-02 identities separated");
	Require(adapter.HasActiveOrder() && adapter.ActiveKey().orderId == 8002,
		"SHADOW-02 only new order active");
	Require(adapter.LastTerminalSnapshot().orderId == 8001,
		"SHADOW-02 old terminal immutable");
}

void TestFailure()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	IssueAndMove(adapter, Target(10.0f, 20.0f, 30.0f), 9001);
	adapter.ObserveFailure(OrderOutcome::UNREACHABLE,
		DetailReason::FAILURE_REPORTED, 15.0);
	Require(!adapter.HasActiveOrder(), "SHADOW-03 failure terminal");
	Require(adapter.LastTerminalSnapshot().outcome == OrderOutcome::UNREACHABLE,
		"SHADOW-03 outcome mapped");
}

void TestDeathAbort()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	IssueAndMove(adapter, Target(11.0f, 12.0f, 13.0f), 9101);
	adapter.ObserveAbort(DetailReason::BOT_DIED, 16.0);
	Require(!adapter.HasActiveOrder(), "SHADOW-04 death cleanup");
	Require(adapter.LastTerminalSnapshot().outcome == OrderOutcome::ABORTED &&
		adapter.LastTerminalSnapshot().detailReason == DetailReason::BOT_DIED,
		"SHADOW-04 death reason");
}

void TestCommandDisappearanceDoesNotCancel()
{
	CommanderMoveOrderAdapter adapter;
	IssueAndMove(adapter, Target(21.0f, 22.0f, 23.0f), 9201);
	adapter.Update(10.0);
	Require(adapter.HasActiveOrder(), "SHADOW-05 disappearance stays active");
}

void TestFallbackIdentity()
{
	CommanderMoveOrderAdapter adapter;
	adapter.ObserveNewMove(Target(31.0f, 32.0f, 33.0f), 0, 1, 42,
		1.0, 301.0);
	const OrderKey first = adapter.ActiveKey();
	adapter.ObserveAbort(DetailReason::EXPLICIT_ABORT, 2.0);
	adapter.ObserveNewMove(Target(34.0f, 35.0f, 36.0f), 0, 1, 42,
		3.0, 303.0);
	const OrderKey second = adapter.ActiveKey();
	Require(first.orderId == 1 && second.orderId == 2,
		"SHADOW-06 fallback order IDs monotonic");
	Require(first.generation == 1 && second.generation == 2,
		"SHADOW-06 generations monotonic");
}

void TestNoNavigationSurface()
{
	CommanderMoveOrderAdapter adapter;
	adapter.ObserveNewMove(Target(41.0f, 42.0f, 43.0f), 9301, 1, 42,
		1.0, 301.0);
	Require(adapter.Snapshot().target.x == 41.0f,
		"SHADOW-07 adapter stores observation only");
}

void TestTerminalResultKeepsIdentity()
{
	CommanderMoveOrderAdapter adapter;
	adapter.ObserveNewMove(Target(51.0f, 52.0f, 53.0f), 9401, 1, 42,
		1.0, 301.0);
	const OrderKey key = adapter.ActiveKey();
	adapter.ObservePlanning(2.0);
	adapter.ObserveMovementStarted(3.0);
	adapter.ObserveGoalExecution(4.0);
	adapter.ObserveComplete(5.0);
	Require(adapter.LastTerminalSnapshot().orderId == key.orderId &&
		adapter.LastTerminalSnapshot().generation == key.generation,
		"SHADOW-08 terminal identity retained");
}

void TestFailureBeforeMovement()
{
	CommanderMoveOrderAdapter adapter;
	adapter.ObserveNewMove(Target(61.0f, 62.0f, 63.0f), 9501, 1, 42,
		1.0, 301.0);
	adapter.ObservePlanning(2.0);
	adapter.ObserveFailure(OrderOutcome::BLOCKED,
		DetailReason::FAILURE_REPORTED, 3.0);
	Require(adapter.LastTerminalSnapshot().outcome == OrderOutcome::BLOCKED,
		"SHADOW-09 planning failure maps terminal");
}

void TestSequentialOrders()
{
	CommanderMoveOrderAdapter adapter;
	adapter.ObserveNewMove(Target(71.0f, 72.0f, 73.0f), 9601, 1, 42,
		1.0, 301.0);
	adapter.ObservePlanning(2.0);
	adapter.ObserveMovementStarted(3.0);
	adapter.ObserveGoalExecution(4.0);
	adapter.ObserveComplete(5.0);
	adapter.ObserveNewMove(Target(74.0f, 75.0f, 76.0f), 9602, 1, 42,
		6.0, 306.0);
	Require(adapter.HasActiveOrder() && adapter.ActiveKey().orderId == 9602,
		"SHADOW-10 sequential order active");
	Require(adapter.LastTerminalSnapshot().orderId == 9601,
		"SHADOW-10 prior terminal retained");
}
}

int main()
{
	TestSuccessLifecycle();
	TestReplacement();
	TestFailure();
	TestDeathAbort();
	TestCommandDisappearanceDoesNotCancel();
	TestFallbackIdentity();
	TestNoNavigationSurface();
	TestTerminalResultKeepsIdentity();
	TestFailureBeforeMovement();
	TestSequentialOrders();
	std::puts("SHADOW-01 PASS");
	std::puts("SHADOW-02 PASS");
	std::puts("SHADOW-03 PASS");
	std::puts("SHADOW-04 PASS");
	std::puts("SHADOW-05 PASS");
	std::puts("SHADOW-06 PASS");
	std::puts("SHADOW-07 PASS");
	std::puts("SHADOW-08 PASS");
	std::puts("SHADOW-09 PASS");
	std::puts("SHADOW-10 PASS");
	std::puts("RESULT 10/10 PASS");
	return 0;
}
