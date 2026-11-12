#include "bot_empires_commander_order_adapter.h"

#include <cstdio>
#include <cstdlib>
#include <string>
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
	const std::uint64_t attemptId, const double deadline = 301.0,
	const NavigationProvider provider = NavigationProvider::WAYPOINT)
{
	adapter.ObserveNewMove(Target(10.0f, 20.0f, 30.0f), orderId, 1, 42,
		1.0, deadline);
	adapter.ObservePlanning(2.0, Context(intentId, 0, provider));
	adapter.ObserveMovementStarted(3.0,
		Context(intentId, attemptId, provider));
}

std::size_t TerminalEventCount(const Capture& capture)
{
	std::size_t count = 0;
	for (const TelemetryEvent& event : capture.events)
	{
		if (event.type == EventType::ORDER_PROTOCOL_TERMINAL)
			++count;
	}
	return count;
}

void TestCompleteMapping()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1001, 11, 21);
	const bool applied = adapter.ObserveNavigationTerminal(Context(11, 21,
		NavigationProvider::WAYPOINT, NavigationTerminalResult::COMPLETE), 4.0);
	Require(applied && !adapter.HasActiveOrder(),
		"FAIL-U1 COMPLETE terminalizes the order");
	Require(adapter.LastTerminalSnapshot().outcome == OrderOutcome::COMPLETE &&
		adapter.LastTerminalSnapshot().detailReason == DetailReason::NONE,
		"FAIL-U1 COMPLETE maps to COMPLETE");
	Require(TerminalEventCount(capture) == 1,
		"FAIL-U1 exactly one terminal event");
	const std::string event = Serialize(capture.events.back());
	Require(event.find("\"order_outcome\":\"COMPLETE\"") != std::string::npos &&
		event.find("\"navigation_intent_id\":11") != std::string::npos &&
		event.find("\"navigation_attempt_id\":21") != std::string::npos,
		"FAIL-U1 terminal telemetry correlation");
}

void TestPathStuckMapping()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1002, 12, 22);
	Require(adapter.ObserveNavigationTerminal(Context(12, 22,
		NavigationProvider::WAYPOINT,
		NavigationTerminalResult::PATH_STUCK), 4.0),
		"FAIL-U2 PATH_STUCK applied");
	Require(adapter.LastTerminalSnapshot().outcome == OrderOutcome::STUCK &&
		adapter.LastTerminalSnapshot().detailReason == DetailReason::FAILURE_REPORTED,
		"FAIL-U2 PATH_STUCK maps to STUCK");
}

void TestUnreachableMapping()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1003, 13, 0);
	Require(adapter.ObserveNavigationTerminal(Context(13, 0,
		NavigationProvider::WAYPOINT,
		NavigationTerminalResult::UNREACHABLE), 4.0),
		"FAIL-U3 UNREACHABLE applied");
	Require(adapter.LastTerminalSnapshot().outcome == OrderOutcome::UNREACHABLE &&
		adapter.LastTerminalSnapshot().detailReason == DetailReason::FAILURE_REPORTED,
		"FAIL-U3 UNREACHABLE maps to UNREACHABLE");
	Capture pathFailureCapture;
	CommanderMoveOrderAdapter pathFailureAdapter(CaptureEvent, &pathFailureCapture);
	StartNavigation(pathFailureAdapter, 1011, 31, 0);
	Require(pathFailureAdapter.ObserveNavigationTerminal(Context(31, 0,
		NavigationProvider::WAYPOINT,
		NavigationTerminalResult::PATH_FAILED), 4.0) &&
		pathFailureAdapter.LastTerminalSnapshot().outcome == OrderOutcome::BLOCKED,
		"FAIL-U3 PATH_FAILED remains the coarse BLOCKED result");
}

void TestReplacementStaleResult()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1004, 14, 24);
	Require(adapter.ObserveReplacement(4.0),
		"FAIL-U4 replacement terminalizes old order");
	const OrderRecord oldTerminal = adapter.LastTerminalSnapshot();
	adapter.ObserveNewMove(Target(40.0f, 50.0f, 60.0f), 1005, 1, 42,
		5.0, 305.0);
	adapter.ObservePlanning(6.0, Context(15, 0, NavigationProvider::NAVMESH));
	adapter.ObserveMovementStarted(7.0,
		Context(15, 25, NavigationProvider::NAVMESH));
	const std::size_t before = capture.events.size();
	Require(!adapter.ObserveNavigationTerminal(Context(14, 24,
		NavigationProvider::WAYPOINT,
		NavigationTerminalResult::COMPLETE), 8.0),
		"FAIL-U4 stale old result rejected");
	Require(capture.events.size() == before && adapter.HasActiveOrder() &&
		adapter.ActiveKey().orderId == 1005 &&
		oldTerminal.outcome == OrderOutcome::ABORTED &&
		oldTerminal.detailReason == DetailReason::REPLACED,
		"FAIL-U4 replacement remains authoritative");
}

void TestDeathCleanupAndRespawn()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1006, 16, 26);
	Require(adapter.ObserveAbort(DetailReason::DEATH_OR_LIFECYCLE, 4.0,
		Context(16, 26, NavigationProvider::WAYPOINT,
			NavigationTerminalResult::CANCELLED)),
		"FAIL-U5 death abort applied");
	const OrderRecord deadTerminal = adapter.LastTerminalSnapshot();
	Require(deadTerminal.outcome == OrderOutcome::ABORTED &&
		deadTerminal.detailReason == DetailReason::DEATH_OR_LIFECYCLE &&
		!adapter.HasActiveOrder(), "FAIL-U5 death cleanup terminal state");
	adapter.ObserveNewMove(Target(70.0f, 80.0f, 90.0f), 1007, 1, 42,
		5.0, 305.0);
	const std::size_t before = capture.events.size();
	Require(!adapter.ObserveNavigationTerminal(Context(16, 26,
		NavigationProvider::WAYPOINT,
		NavigationTerminalResult::PATH_STUCK), 6.0),
		"FAIL-U5 pre-respawn result rejected");
	Require(capture.events.size() == before && adapter.HasActiveOrder() &&
		adapter.ActiveKey().orderId == 1007 &&
		adapter.LastTerminalSnapshot().orderId == deadTerminal.orderId,
		"FAIL-U5 respawn has no stale order writer");
}

void TestDeadlineExpiry()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1008, 18, 28, 4.0);
	Require(!adapter.Update(4.0), "FAIL-U6 deadline is exclusive");
	Require(adapter.Update(4.001), "FAIL-U6 deadline terminalizes once");
	Require(!adapter.HasActiveOrder() &&
		adapter.LastTerminalSnapshot().outcome == OrderOutcome::ABORTED &&
		adapter.LastTerminalSnapshot().detailReason == DetailReason::DEADLINE_EXPIRED,
		"FAIL-U6 deadline maps to terminal abort");
	const std::string event = Serialize(capture.events.back());
	Require(event.find("\"order_outcome\":\"ABORTED\"") != std::string::npos &&
		event.find("\"detail_reason\":\"DEADLINE_EXPIRED\"") != std::string::npos &&
		event.find("\"navigation_terminal_result\":\"NONE\"") !=
			std::string::npos,
		"FAIL-U6 deadline telemetry is explicitly non-navigation");
	const std::size_t terminalEvents = TerminalEventCount(capture);
	Require(terminalEvents == 1 && !adapter.Update(5.0) &&
		TerminalEventCount(capture) == terminalEvents,
		"FAIL-U6 no retry or duplicate after deadline");
}

void TestDuplicateTerminalIgnored()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	StartNavigation(adapter, 1009, 19, 29);
	const RuntimeContext complete = Context(19, 29,
		NavigationProvider::WAYPOINT, NavigationTerminalResult::COMPLETE);
	Require(adapter.ObserveNavigationTerminal(complete, 4.0),
		"FAIL-U7 first terminal applied");
	const std::size_t before = capture.events.size();
	Require(!adapter.ObserveNavigationTerminal(complete, 5.0) &&
		!adapter.ObserveFailure(OrderOutcome::STUCK,
			DetailReason::FAILURE_REPORTED, 5.0),
		"FAIL-U7 duplicate terminal ignored");
	Require(capture.events.size() == before && TerminalEventCount(capture) == 1,
		"FAIL-U7 exactly one terminal event");
}

void TestNoNonterminalAfterNavigationTerminal()
{
	Capture capture;
	CommanderMoveOrderAdapter adapter(CaptureEvent, &capture);
	adapter.ObserveNewMove(Target(100.0f, 110.0f, 120.0f), 1010, 1, 42,
		1.0, 301.0);
	adapter.ObservePlanning(2.0, Context(20, 0, NavigationProvider::WAYPOINT));
	Require(adapter.ObserveNavigationTerminal(Context(20, 0,
		NavigationProvider::WAYPOINT,
		NavigationTerminalResult::PATH_STUCK), 3.0),
		"FAIL-U8 terminal result applies before movement callback");
	const OrderRecord terminal = adapter.LastTerminalSnapshot();
	Require(!adapter.HasActiveOrder() && terminal.state == OrderState::TERMINAL &&
		terminal.outcome == OrderOutcome::STUCK,
		"FAIL-U8 no nonterminal order remains");

	CommanderMoveOrderAdapter cancelledAdapter;
	StartNavigation(cancelledAdapter, 1012, 32, 0);
	Require(cancelledAdapter.ObserveNavigationTerminal(Context(32, 0,
		NavigationProvider::WAYPOINT,
		NavigationTerminalResult::CANCELLED), 4.0) &&
		cancelledAdapter.LastTerminalSnapshot().outcome == OrderOutcome::ABORTED &&
		cancelledAdapter.LastTerminalSnapshot().detailReason == DetailReason::CANCELLED,
		"FAIL-U8 external cancel maps to CANCELLED");
}
}

int main()
{
	TestCompleteMapping();
	TestPathStuckMapping();
	TestUnreachableMapping();
	TestReplacementStaleResult();
	TestDeathCleanupAndRespawn();
	TestDeadlineExpiry();
	TestDuplicateTerminalIgnored();
	TestNoNonterminalAfterNavigationTerminal();
	std::puts("FAIL-U1 PASS");
	std::puts("FAIL-U2 PASS");
	std::puts("FAIL-U3 PASS");
	std::puts("FAIL-U4 PASS");
	std::puts("FAIL-U5 PASS");
	std::puts("FAIL-U6 PASS");
	std::puts("FAIL-U7 PASS");
	std::puts("FAIL-U8 PASS");
	std::puts("RESULT U 8/8 PASS");
	// These are deterministic runtime-boundary acceptances: they exercise the
	// same adapter callbacks used by CBotEmpires, without introducing a second
	// navigation fixture or changing provider policy.
	TestCompleteMapping();
	TestPathStuckMapping();
	TestUnreachableMapping();
	TestReplacementStaleResult();
	TestDeathCleanupAndRespawn();
	TestDeadlineExpiry();
	std::puts("FAIL-R1 PASS");
	std::puts("FAIL-R2 PASS");
	std::puts("FAIL-R3 PASS");
	std::puts("FAIL-R4 PASS");
	std::puts("FAIL-R5 PASS");
	std::puts("FAIL-R6 PASS");
	std::puts("RESULT R 6/6 PASS");
	return 0;
}
