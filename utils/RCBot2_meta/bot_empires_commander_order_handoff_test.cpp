#include "bot_empires_commander_order_adapter.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
using namespace RCBotCommanderOrder;
using namespace RCBotOrder;
using namespace RCBotOrderTelemetry;

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
	return { x, y, z };
}

RuntimeContext Context(const std::uint64_t intentId,
	const std::uint64_t attemptId, const NavigationProvider provider,
	const NavigationTerminalResult result = NavigationTerminalResult::NONE)
{
	RuntimeContext context;
	context.hasNavigationIntentId = true;
	context.navigationIntentId = intentId;
	context.hasNavigationAttemptId = attemptId != 0;
	context.navigationAttemptId = attemptId;
	context.hasProvider = true;
	context.provider = provider;
	context.hasNavigationTerminalResult = result !=
		NavigationTerminalResult::NONE;
	context.navigationTerminalResult = result;
	return context;
}

void StartNavigation(CommanderMoveOrderAdapter& adapter,
	const OrderId orderId, const std::uint64_t intentId,
	const std::uint64_t attemptId = 0,
	const NavigationProvider provider = NavigationProvider::WAYPOINT)
{
	adapter.ObserveNewMove(Target(10.0f, 20.0f, 30.0f), orderId, 1, 42,
		1.0, 301.0);
	adapter.ObservePlanning(2.0, Context(intentId, 0, provider));
	adapter.ObserveMovementStarted(3.0,
		Context(intentId, attemptId, provider));
}

void TestOneMoveOneNavigationRequest()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1001, 77, 88);
	Require(capture.events.size() == 4, "HANDOFF-U1 one request lifecycle");
	Require(capture.events[2].context.hasNavigationIntentId &&
		capture.events[2].context.navigationIntentId == 77,
		"HANDOFF-U1 intent identity");
	Require(capture.events[3].context.hasNavigationAttemptId &&
		capture.events[3].context.navigationAttemptId == 88,
		"HANDOFF-U1 attempt identity");
}

void TestReplacementCancelsBeforeNewOrder()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1101, 101, 201);
	adapter.ObserveReplacement(4.0);
	adapter.ObserveNewMove(Target(40.0f, 50.0f, 60.0f), 1102, 1, 42,
		5.0, 305.0);
	adapter.ObservePlanning(6.0, Context(102, 0, NavigationProvider::WAYPOINT));
	Require(capture.events[4].transition.orderId == 1101 &&
		capture.events[4].transition.newOutcome == OrderOutcome::ABORTED,
		"HANDOFF-U2 old order terminal");
	Require(capture.events[5].transition.orderId == 1102 &&
		capture.events[5].type == EventType::ORDER_PROTOCOL_ISSUED,
		"HANDOFF-U2 replacement issued separately");
	Require(capture.events[7].context.navigationIntentId == 102,
		"HANDOFF-U2 new intent context");
}

void TestNavigationCompleteCompletesOrder()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1201, 301, 401, NavigationProvider::NAVMESH);
	adapter.ObserveNavigationTerminal(Context(301, 401,
		NavigationProvider::NAVMESH, NavigationTerminalResult::COMPLETE), 4.0);
	Require(!adapter.HasActiveOrder(), "HANDOFF-U3 complete cleanup");
	Require(capture.events.back().transition.newOutcome == OrderOutcome::COMPLETE &&
		capture.events.back().context.navigationIntentId == 301 &&
		capture.events.back().context.navigationTerminalResult ==
			NavigationTerminalResult::COMPLETE,
		"HANDOFF-U3 complete correlation");
}

void TestPathStuckMapsToOrderStuck()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1301, 501, 601);
	adapter.ObserveNavigationTerminal(Context(501, 601,
		NavigationProvider::WAYPOINT, NavigationTerminalResult::PATH_STUCK), 4.0);
	Require(adapter.LastTerminalSnapshot().outcome == OrderOutcome::STUCK,
		"HANDOFF-U4 PATH_STUCK mapping");
}

void TestUnreachableMapsToOrderUnreachable()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1401, 701, 0);
	adapter.ObserveNavigationTerminal(Context(701, 0,
		NavigationProvider::WAYPOINT, NavigationTerminalResult::UNREACHABLE), 4.0);
	Require(adapter.LastTerminalSnapshot().outcome == OrderOutcome::UNREACHABLE,
		"HANDOFF-U5 UNREACHABLE mapping");
}

void TestStaleOldNavigationCannotMutateReplacement()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1501, 801, 901);
	adapter.ObserveReplacement(4.0);
	adapter.ObserveNewMove(Target(40.0f, 50.0f, 60.0f), 1502, 1, 42,
		5.0, 305.0);
	adapter.ObservePlanning(6.0, Context(802, 0, NavigationProvider::NAVMESH));
	adapter.ObserveMovementStarted(7.0,
		Context(802, 902, NavigationProvider::NAVMESH));
	const std::size_t before = capture.events.size();
	adapter.ObserveNavigationTerminal(Context(801, 901,
		NavigationProvider::WAYPOINT, NavigationTerminalResult::PATH_STUCK), 8.0);
	Require(capture.events.size() == before && adapter.HasActiveOrder() &&
		adapter.ActiveKey().orderId == 1502,
		"HANDOFF-U6 stale result rejected");
}
}

int main()
{
	TestOneMoveOneNavigationRequest();
	TestReplacementCancelsBeforeNewOrder();
	TestNavigationCompleteCompletesOrder();
	TestPathStuckMapsToOrderStuck();
	TestUnreachableMapsToOrderUnreachable();
	TestStaleOldNavigationCannotMutateReplacement();
	std::puts("HANDOFF-U1 PASS");
	std::puts("HANDOFF-U2 PASS");
	std::puts("HANDOFF-U3 PASS");
	std::puts("HANDOFF-U4 PASS");
	std::puts("HANDOFF-U5 PASS");
	std::puts("HANDOFF-U6 PASS");
	std::puts("RESULT 6/6 PASS");
	return 0;
}
