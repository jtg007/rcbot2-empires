#include "bot_squad_order_recipient.h"
#include "bot_commander_move_test_fixture.h"
#include "bot_empires_commander_order_adapter.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace RCBotOrder;
using namespace RCBotOrderTelemetry;
using namespace RCBotSquadOrder;

struct Capture
{
	std::vector<NavigationIntentRequest> starts;
	std::vector<std::uint64_t> cancels;
	std::vector<TelemetryEvent> events;
};

bool StartNavigation(void* context, const NavigationIntentRequest& request,
	std::uint64_t* navigationIntentId)
{
	Capture* capture = static_cast<Capture*>(context);
	capture->starts.push_back(request);
	*navigationIntentId = 500 + capture->starts.size();
	return true;
}

void CancelNavigation(void* context, const NavigationIntentRequest&,
	const std::uint64_t navigationIntentId)
{
	static_cast<Capture*>(context)->cancels.push_back(navigationIntentId);
}

void CaptureTelemetry(void* context, const TelemetryEvent& event)
{
	static_cast<Capture*>(context)->events.push_back(event);
}

TargetPosition Target()
{
	return { 100.0f, 200.0f, 300.0f };
}

SquadMembershipSnapshot Snapshot(const Generation generation = 1)
{
	return { OrderRecipient::Squad(42, generation), { 101, 102 } };
}

SquadMoveSpec Spec(const OrderId orderId = 7001,
	const Generation generation = 1)
{
	return { orderId, generation, 9, Snapshot(generation), Target(), 10.0, 310.0 };
}

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

SquadMoveRecipient NewRecipient(Capture& capture)
{
	return SquadMoveRecipient(StartNavigation, CancelNavigation,
		CaptureTelemetry, &capture);
}

void TestTwoMemberSquadMove()
{
	Capture capture;
	SquadMoveRecipient recipient = NewRecipient(capture);
	Require(recipient.IssueMove(Spec()), "SQUAD-R1 issue accepted");
	const std::vector<ChildExecution> children = recipient.ActiveChildren();
	Require(children.size() == 2 && recipient.HasActiveParent(),
		"SQUAD-R1 parent and two children active");
	Require(recipient.ParentSnapshot().recipient == OrderRecipient::Squad(42, 1),
		"SQUAD-U2 parent has squad recipient");
	const std::string parentIssued = Serialize(capture.events.front());
	Require(parentIssued.find("\"recipient_kind\":\"SQUAD\"") !=
			std::string::npos &&
		parentIssued.find("\"squad_generation\":1") != std::string::npos,
		"SQUAD-U2 squad recipient telemetry");
	for (const ChildExecution& child : children)
		Require(child.record.recipient.kind == RecipientKind::BOT,
			"SQUAD-U1 child has BOT recipient");
	std::cout << "SQUAD-R1 PASS\n";
}

void TestIndependentNavigationIntents()
{
	Capture capture;
	SquadMoveRecipient recipient = NewRecipient(capture);
	recipient.IssueMove(Spec());
	const std::vector<ChildExecution> children = recipient.ActiveChildren();
	Require(capture.starts.size() == 2, "SQUAD-R2 two navigation starts");
	Require(children[0].navigationIntentId != children[1].navigationIntentId,
		"SQUAD-U3 independent navigation intent ids");
	Require(children[0].key.orderId != children[1].key.orderId,
		"SQUAD-U3 independent child identities");
	Require(capture.starts[0].memberId == 101 &&
		capture.starts[1].memberId == 102,
		"SQUAD-U10 deterministic member dispatch order");
	std::cout << "SQUAD-R2 PASS\n";
}

void TestReplacementAndStaleResult()
{
	Capture capture;
	SquadMoveRecipient recipient = NewRecipient(capture);
	recipient.IssueMove(Spec(7001, 1));
	const std::vector<ChildExecution> oldChildren = recipient.ActiveChildren();
	const OrderKey oldParent = recipient.ActiveParentKey();
	recipient.IssueMove(Spec(7002, 2));
	Require(capture.cancels.size() == 2, "SQUAD-U7 replacement cancels all intents");
	Require(recipient.ActiveParentKey().orderId == 7002 &&
		recipient.RetiredChildren().size() == 2,
		"SQUAD-R3 only replacement children remain active");
	Require(!recipient.ObserveNavigationTerminal(oldParent, oldChildren[0].key,
		oldChildren[0].memberId, oldChildren[0].navigationIntentId,
		NavigationTerminalResult::COMPLETE, 20.0),
		"SQUAD-U8 stale child result rejected");
	Require(recipient.HasActiveParent() &&
		recipient.ParentSnapshot().orderId == 7002,
		"SQUAD-U8 stale result cannot mutate replacement");
	std::cout << "SQUAD-R3 PASS\n";
}

void TestMemberLeaveKeepsSurvivors()
{
	Capture capture;
	SquadMoveRecipient recipient = NewRecipient(capture);
	recipient.IssueMove(Spec());
	const OrderKey parent = recipient.ActiveParentKey();
	const std::vector<ChildExecution> children = recipient.ActiveChildren();
	Require(recipient.ObserveMemberLifecycle(parent, Snapshot().recipient, 101,
		DetailReason::DEATH_OR_LIFECYCLE, 20.0),
		"SQUAD-R4 member leave accepted");
	Require(capture.cancels.size() == 1,
		"SQUAD-U4 leave cancels only that child navigation");
	Require(recipient.HasActiveParent(),
		"SQUAD-U4 parent remains for snapshot survivors");
	Require(recipient.ObserveNavigationTerminal(parent, children[1].key, 102,
		children[1].navigationIntentId, NavigationTerminalResult::COMPLETE, 21.0),
		"SQUAD-R4 survivor completion accepted");
	Require(!recipient.HasActiveParent() &&
		recipient.ParentSnapshot().outcome == OrderOutcome::PARTIAL_FAILURE,
		"SQUAD-U9 mixed result is not full success");
	std::cout << "SQUAD-R4 PASS\n";
}

void TestGenerationAndLeaderSemantics()
{
	SquadIdentityTracker tracker;
	const SquadMembershipObservation first = tracker.Observe(42, { 101, 102 });
	const SquadMembershipObservation reordered = tracker.Observe(42, { 102, 101 });
	Require(first.recipient.generation == reordered.recipient.generation &&
		!reordered.membershipChanged,
		"SQUAD-U6 leader/order change keeps logical generation");
	const SquadMembershipObservation membership = tracker.Observe(42, { 102 });
	Require(membership.recipient.generation == first.recipient.generation &&
		membership.membershipChanged,
		"SQUAD-U4 membership change is observed without generation churn");
	tracker.Forget(42);
	const SquadMembershipObservation recreated = tracker.Observe(42, { 103, 104 });
	Require(recreated.recipient.generation != first.recipient.generation,
		"SQUAD-U5 recreated squad gets a stale-safe generation");

	Capture capture;
	SquadMoveRecipient recipient = NewRecipient(capture);
	recipient.IssueMove(Spec());
	const OrderKey parent = recipient.ActiveParentKey();
	Require(!recipient.ObserveMembership(parent, Snapshot(2), 20.0),
		"SQUAD-U5 stale squad generation rejected");
	Require(recipient.HasActiveParent(), "SQUAD-U5 stale update leaves parent intact");
	std::cout << "SQUAD-R5 PASS\n";
}

void CompleteBoth(SquadMoveRecipient& recipient)
{
	const OrderKey parent = recipient.ActiveParentKey();
	const std::vector<ChildExecution> children = recipient.ActiveChildren();
	for (const ChildExecution& child : children)
		recipient.ObserveNavigationTerminal(parent, child.key, child.memberId,
			child.navigationIntentId, NavigationTerminalResult::COMPLETE, 20.0);
}

void TestDeterministicAggregationAndNoDuplicateOwnership()
{
	Capture leftCapture;
	Capture rightCapture;
	SquadMoveRecipient left = NewRecipient(leftCapture);
	SquadMoveRecipient right = NewRecipient(rightCapture);
	left.IssueMove(Spec());
	right.IssueMove(Spec());
	const std::vector<ChildExecution> leftChildren = left.ActiveChildren();
	const std::vector<ChildExecution> rightChildren = right.ActiveChildren();
	Require(leftChildren.size() == rightChildren.size(),
		"SQUAD-U10 deterministic child count");
	for (std::size_t i = 0; i < leftChildren.size(); ++i)
	{
		Require(leftChildren[i].key.orderId == rightChildren[i].key.orderId &&
			leftChildren[i].key.generation == rightChildren[i].key.generation &&
			leftChildren[i].navigationIntentId == rightChildren[i].navigationIntentId,
			"SQUAD-U10 deterministic child identities");
		Require(leftChildren[i].memberId == rightChildren[i].memberId,
			"SQUAD-U10 deterministic member mapping");
	}
	CompleteBoth(left);
	CompleteBoth(right);
	Require(left.ParentSnapshot().outcome == OrderOutcome::COMPLETE &&
		!left.HasActiveParent(), "SQUAD-U9 all-complete parent aggregation");
	const std::string childTerminal =
		Serialize(leftCapture.events[leftCapture.events.size() - 2]);
	Require(childTerminal.find("\"parent_order_id\":7001") != std::string::npos &&
		childTerminal.find("\"child_execution_id\":") != std::string::npos &&
		childTerminal.find("\"navigation_terminal_result\":\"COMPLETE\"") !=
			std::string::npos,
		"SQUAD-U3 child telemetry correlation");
	const std::string parentTerminal = Serialize(leftCapture.events.back());
	Require(parentTerminal.find("\"parent_aggregate_result\":\"COMPLETE\"") !=
		std::string::npos,
		"SQUAD-U9 parent aggregate telemetry");
	Require(leftCapture.events.size() == rightCapture.events.size(),
		"SQUAD-U10 deterministic telemetry count");
	for (std::size_t i = 0; i < leftCapture.events.size(); ++i)
		Require(Serialize(leftCapture.events[i]) == Serialize(rightCapture.events[i]),
			"SQUAD-U10 deterministic telemetry");
	Require(leftChildren[0].memberId != leftChildren[1].memberId,
		"SQUAD-R6 one child owner per bot");
	std::cout << "SQUAD-R6 PASS\n";
}

void TestCommanderMoveFixtureBotRecipient()
{
	using namespace RCBotCommanderMoveTestFixture;
	Request request;
	Require(ParseArguments({ "bot", "101", "1", "2", "3" }, request),
		"FIX-SQUAD-01 explicit bot syntax parses");
	Require(request.mode == Mode::BOT && request.botUserId == 101,
		"FIX-SQUAD-01 bot identity retained");

	Capture capture;
	RCBotCommanderOrder::CommanderMoveOrderAdapter adapter(
		CaptureTelemetry, &capture);
	adapter.ObserveNewMove(request.target, 7100, 9,
		MakeRecipient(request), 10.0, 310.0);
	Require(adapter.Snapshot().recipient.kind == RecipientKind::BOT &&
		adapter.Snapshot().recipient.id == 101,
		"FIX-SQUAD-01 bot mode retains RecipientKind::BOT");
	std::cout << "FIX-SQUAD-01 PASS\n";
}

void TestCommanderMoveFixtureSquadRecipient()
{
	using namespace RCBotCommanderMoveTestFixture;
	Request request;
	Require(ParseArguments({ "squad", "2", "7", "100", "200", "300" },
		request), "FIX-SQUAD-02 squad syntax parses");
	Require(request.mode == Mode::SQUAD,
		"FIX-SQUAD-02 squad mode retained");

	Capture capture;
	SquadIdentityTracker tracker;
	SquadMoveRecipient recipient = NewRecipient(capture);
	SquadMembershipObservation membership;
	Require(IssueSquadMove(request, { 101, 102 }, tracker, recipient,
		7200, 1, 9, 10.0, 310.0, &membership),
		"FIX-SQUAD-02 existing Phase-10.6 SquadMoveRecipient issues parent");
	const OrderRecord parent = recipient.ParentSnapshot();
	Require(parent.recipient.kind == RecipientKind::SQUAD &&
		parent.type == OrderType::MOVE,
		"FIX-SQUAD-02 parent is a SQUAD MOVE");
	Require(recipient.ActiveChildren().empty() &&
		!recipient.ChildrenDispatched(),
		"FIX-SQUAD-02 parent waits for canonical spatial targets before child dispatch");
	Require(!capture.events.empty() &&
		Serialize(capture.events.front()).find("\"recipient_kind\":\"SQUAD\"") !=
			std::string::npos,
		"FIX-SQUAD-02 emitted protocol telemetry has SQUAD recipient");
	std::cout << "FIX-SQUAD-02 PASS\n";
}

void TestCommanderMoveFixturePreservesSquadIdentity()
{
	using namespace RCBotCommanderMoveTestFixture;
	Request request;
	Require(ParseArguments({ "squad", "3", "11", "-4.5", "6", "7.25" },
		request), "FIX-SQUAD-03 squad syntax parses");
	SquadIdentityTracker tracker;
	SquadMoveRecipient recipient;
	SquadMembershipObservation membership;
	Require(IssueSquadMove(request, { 203, 201, 202 }, tracker, recipient,
		7300, 1, 9, 10.0, 310.0, &membership),
		"FIX-SQUAD-03 squad move issue succeeds");
	const OrderRecord parent = recipient.ParentSnapshot();
	const RecipientId encodedSquad =
		TeamScopedSquadId(request.team, request.squadId);
	Require(parent.recipient.id == encodedSquad &&
		(parent.recipient.id >> 32) == 3 &&
		(parent.recipient.id & 0xffffffffULL) == 11,
		"FIX-SQUAD-03 team and raw squad id preserved in scoped identity");
	Require(membership.members == std::vector<RecipientId>({ 201, 202, 203 }) &&
		membership.recipient.id == encodedSquad &&
		membership.recipient.generation == parent.recipient.generation,
		"FIX-SQUAD-03 membership ids and squad generation preserved");
	Require(parent.target.x == -4.5f && parent.target.y == 6.0f &&
		parent.target.z == 7.25f,
		"FIX-SQUAD-03 target coordinates preserved");
	std::cout << "FIX-SQUAD-03 PASS\n";
}

void TestInvalidSquadFixtureDoesNotMutateOrder()
{
	using namespace RCBotCommanderMoveTestFixture;
	Capture capture;
	SquadIdentityTracker tracker;
	SquadMoveRecipient recipient = NewRecipient(capture);
	Require(recipient.IssueMove(Spec(7400, 1)),
		"FIX-SQUAD-04 establish existing parent snapshot");
	const OrderRecord before = recipient.ParentSnapshot();
	const std::size_t childrenBefore = recipient.ActiveChildren().size();
	const std::size_t eventsBefore = capture.events.size();
	Request invalid;
	Require(!ParseArguments({ "squad", "1", "7", "0", "0", "0" },
		invalid), "FIX-SQUAD-04 invalid team rejected");
	Require(!ParseArguments({ "squad", "2", "65", "0", "0", "0" },
		invalid), "FIX-SQUAD-04 out-of-range squad rejected");
	Require(!ParseArguments({ "squad", "2", "7", "nan", "0", "0" },
		invalid), "FIX-SQUAD-04 non-finite coordinate rejected");
	Require(!ParseArguments({ "squad", "2", "7", "0", "0" }, invalid),
		"FIX-SQUAD-04 incomplete arguments rejected");
	Require(recipient.ParentSnapshot().orderId == before.orderId &&
		recipient.ParentSnapshot().generation == before.generation &&
		recipient.ParentSnapshot().state == before.state &&
		recipient.ActiveChildren().size() == childrenBefore &&
		capture.events.size() == eventsBefore,
		"FIX-SQUAD-04 rejected arguments leave protocol snapshot unchanged");
	std::cout << "FIX-SQUAD-04 PASS\n";
}
}

int main()
{
	TestCommanderMoveFixtureBotRecipient();
	TestCommanderMoveFixtureSquadRecipient();
	TestCommanderMoveFixturePreservesSquadIdentity();
	TestInvalidSquadFixtureDoesNotMutateOrder();
	TestTwoMemberSquadMove();
	TestIndependentNavigationIntents();
	TestReplacementAndStaleResult();
	TestMemberLeaveKeepsSurvivors();
	TestGenerationAndLeaderSemantics();
	TestDeterministicAggregationAndNoDuplicateOwnership();
	std::cout << "SQUAD-U1 PASS\n"
		<< "SQUAD-U2 PASS\n"
		<< "SQUAD-U3 PASS\n"
		<< "SQUAD-U4 PASS\n"
		<< "SQUAD-U5 PASS\n"
		<< "SQUAD-U6 PASS\n"
		<< "SQUAD-U7 PASS\n"
		<< "SQUAD-U8 PASS\n"
		<< "SQUAD-U9 PASS\n"
		<< "SQUAD-U10 PASS\n"
		<< "RESULT FIX-SQUAD-01..04 PASS\n"
		<< "RESULT SQUAD-R1..R6 PASS\n";
	return 0;
}
