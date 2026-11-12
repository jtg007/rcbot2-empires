#include "bot_squad_spatial_movement.h"
#include "bot_squad_order_recipient.h"
#include "bot_empires_squad_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace RCBotOrder;
using namespace RCBotOrderTelemetry;
using namespace RCBotSquadOrder;
using RCBotEmpiresSquadRuntime::Adapter;
using RCBotEmpiresSquadRuntime::MemberObservation;
using RCBotEmpiresSquadRuntime::MemberLifecycle;
using RCBotSquadDecision::ParticipationState;
using RCBotSquadMovement::Diagnostic;
using RCBotSquadMovement::MemberTarget;
using RCBotSquadMovement::ShouldSuppressLegacyMovement;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

TargetPosition Pos(const float x, const float y, const float z = 0.0f)
{
	return { x, y, z };
}

MemberObservation Member(const RecipientId id, const TargetPosition origin,
	const bool leader = false, const bool eligible = true)
{
	MemberObservation member;
	member.state.memberId = id;
	member.state.alive = eligible;
	member.state.connected = true;
	member.state.eligible = eligible;
	member.state.isLeader = leader;
	member.state.squadPosition = leader ? 0 : static_cast<int>(id - 100);
	member.state.origin = origin;
	member.state.velocity = Pos(0, 0, 0);
	member.state.lifecycle = eligible ? MemberLifecycle::ACTIVE :
		MemberLifecycle::DEAD;
	return member;
}

RCBotEmpiresSquadRuntime::Input RuntimeInput()
{
	RCBotEmpiresSquadRuntime::Input input;
	input.team = 2;
	input.squadId = 7;
	input.now = 1.0;
	input.members = { Member(101, Pos(0, 0), true),
		Member(102, Pos(64, 0)) };
	input.parentOrder.present = true;
	input.parentOrder.parentOrder = { 7001, 1 };
	input.parentOrder.target = Pos(128, 0);
	input.parentOrder.deadline = 20.0;
	return input;
}

RCBotSquadMovement::Input MovementInput(
	const RCBotEmpiresSquadRuntime::Result& runtime,
	const std::vector<RecipientId>& members, const double now = 1.0)
{
	RCBotSquadMovement::Input input;
	input.identity = runtime.identity;
	input.decisionRevision = runtime.decisionRevision;
	input.formationGeneration = runtime.formationGeneration;
	input.parentOrder = runtime.parentOrder;
	input.memberSnapshot = members;
	input.participation = runtime.decision.participation;
	input.formation = runtime.formation;
	input.now = now;
	return input;
}

ChildMovementTarget RecipientTarget(const MemberTarget& target,
	const Generation squadGeneration, const std::uint64_t revision,
	const Generation formationGeneration)
{
	ChildMovementTarget result;
	result.memberId = target.memberId;
	result.hasTarget = target.hasTarget;
	result.target = target.target;
	result.slotId = target.slotId;
	result.squadGeneration = squadGeneration;
	result.decisionRevision = revision;
	result.formationGeneration = formationGeneration;
	result.claimResourceKind = static_cast<std::uint8_t>(
		target.claimKey.resourceKind);
	result.claimResourceId = target.claimKey.resourceId;
	result.claimResourceGeneration = target.claimKey.resourceGeneration;
	return result;
}

struct Capture
{
	std::vector<NavigationIntentRequest> starts;
	std::vector<std::uint64_t> cancels;
	std::vector<std::string> sequence;
};

bool Start(void* context, const NavigationIntentRequest& request,
	std::uint64_t* id)
{
	Capture& capture = *static_cast<Capture*>(context);
	capture.sequence.push_back("start");
	capture.starts.push_back(request);
	*id = 900 + capture.starts.size();
	return true;
}

void Cancel(void* context, const NavigationIntentRequest&,
	const std::uint64_t id)
{
	Capture& capture = *static_cast<Capture*>(context);
	capture.sequence.push_back("cancel");
	capture.cancels.push_back(id);
}

SquadMoveSpec Spec()
{
	SquadMembershipSnapshot snapshot;
	snapshot.recipient = OrderRecipient::Squad(77, 1);
	snapshot.members = { 101, 102 };
	SquadMoveSpec spec;
	spec.orderId = 7001;
	spec.generation = 1;
	spec.issuerId = 9;
	spec.snapshot = snapshot;
	spec.target = Pos(128, 0);
	spec.issuedAt = 1.0;
	spec.deadline = 20.0;
	return spec;
}

void TestSlotTargetAndDistinctAssignments()
{
	Adapter adapter;
	const RCBotEmpiresSquadRuntime::Result runtime =
		adapter.Observe(RuntimeInput());
	Require(runtime.Accepted(), "SMOVE-U1 runtime decision accepted");
	const RCBotSquadMovement::Result plan = RCBotSquadMovement::Resolve(
		MovementInput(runtime, { 101, 102 }));
	Require(plan.valid && plan.members.size() == 2,
		"SMOVE-U1 final slot assignments resolve");
	const auto assigned = std::find_if(runtime.formation.assignments.begin(),
		runtime.formation.assignments.end(), [&plan](const auto& value) {
			return value.memberId == plan.members[0].memberId;
		});
	Require(assigned != runtime.formation.assignments.end() &&
		plan.members[0].hasTarget && plan.members[0].target.x ==
		assigned->targetPosition.x &&
		plan.members[0].slotId == assigned->slotId,
		"SMOVE-U1 assigned slot is the movement target");
	Require(plan.members[0].slotId != plan.members[1].slotId &&
		(plan.members[0].target.x != plan.members[1].target.x ||
		plan.members[0].target.y != plan.members[1].target.y ||
		plan.members[0].target.z != plan.members[1].target.z),
		"SMOVE-U2 members have distinct slots and destinations");
	Require(runtime.formation.frame.anchor.x == 128.0f &&
		runtime.formation.frame.anchor.y == 0.0f,
		"SMOVE-U1 active formation is anchored on the Commander objective");
	std::cout << "SMOVE-U1 PASS\nSMOVE-U2 PASS\n";
}

void TestExcludedAndCatchingUp()
{
	Adapter excludedAdapter;
	RCBotEmpiresSquadRuntime::Input excludedInput = RuntimeInput();
	excludedInput.members.push_back(Member(103, Pos(90, 0), false, false));
	const RCBotEmpiresSquadRuntime::Result excludedRuntime =
		excludedAdapter.Observe(excludedInput);
	const RCBotSquadMovement::Result excludedPlan = RCBotSquadMovement::Resolve(
		MovementInput(excludedRuntime, { 101, 102, 103 }));
	Require(excludedPlan.valid && excludedPlan.members.size() == 3 &&
		!excludedPlan.members[2].hasTarget,
		"SMOVE-U3 EXCLUDED member receives no movement target");
	Capture excludedCapture;
	SquadMoveRecipient excludedRecipient(Start, Cancel, nullptr,
		&excludedCapture);
	SquadMoveSpec excludedSpec = Spec();
	excludedSpec.snapshot.members.push_back(103);
	Require(excludedRecipient.BeginMove(excludedSpec),
		"SMOVE-U3 excluded child parent begins");
	std::vector<ChildMovementTarget> excludedTargets;
	for (const MemberTarget& member : excludedPlan.members)
		excludedTargets.push_back(RecipientTarget(member,
			excludedRuntime.identity.squadGeneration,
			excludedRuntime.decisionRevision,
			excludedRuntime.formationGeneration));
	Require(excludedRecipient.DispatchChildren(
		excludedRecipient.ActiveParentKey(), excludedTargets, 1.0) &&
		excludedCapture.starts.size() == 2 &&
		excludedRecipient.ActiveChildren()[2].record.outcome ==
			OrderOutcome::ABORTED &&
		!excludedRecipient.HasActiveMovementForMember(103),
		"SMOVE-U3 excluded child creates no navigation request");

	Adapter catchingAdapter;
	RCBotEmpiresSquadRuntime::Input catchingInput = RuntimeInput();
	catchingInput.members[0] = Member(101, Pos(0, 0), true);
	catchingInput.members[1] = Member(102, Pos(600, 0));
	catchingInput.parentOrder.target = Pos(900, 0);
	const RCBotEmpiresSquadRuntime::Result catchingRuntime =
		catchingAdapter.Observe(catchingInput);
	const RCBotSquadMovement::Result catchingPlan =
		RCBotSquadMovement::Resolve(
		MovementInput(catchingRuntime, { 101, 102 }));
	bool catchingCanMove = false;
	for (const MemberTarget& member : catchingPlan.members)
		if (member.participation == ParticipationState::CATCHING_UP)
			catchingCanMove = member.hasTarget;
	Require(catchingPlan.valid && catchingCanMove,
		"SMOVE-U4 CATCHING_UP member may execute an active slot");
	std::cout << "SMOVE-U3 PASS\nSMOVE-U4 PASS\n";
}

void TestStaleAssignmentRejected()
{
	Adapter adapter;
	const RCBotEmpiresSquadRuntime::Result runtime =
		adapter.Observe(RuntimeInput());
	RCBotSquadMovement::Input stale = MovementInput(runtime, { 101, 102 });
	++stale.decisionRevision;
	const RCBotSquadMovement::Result rejected =
		RCBotSquadMovement::Resolve(stale);
	Require(!rejected.valid && rejected.diagnostic == Diagnostic::INVALID_FORMATION,
		"SMOVE-U5 stale revision cannot produce movement");
	std::cout << "SMOVE-U5 PASS\n";
}

void TestDispatchOnceAndReplacement()
{
	Adapter adapter;
	const RCBotEmpiresSquadRuntime::Result runtime =
		adapter.Observe(RuntimeInput());
	const RCBotSquadMovement::Result plan = RCBotSquadMovement::Resolve(
		MovementInput(runtime, { 101, 102 }));
	Capture capture;
	SquadMoveRecipient recipient(Start, Cancel, nullptr, &capture);
	const SquadMoveSpec spec = Spec();
	Require(recipient.BeginMove(spec), "SMOVE-U6 parent is created before children");
	Require(recipient.HasActiveMovementForMember(101) &&
		recipient.HasActiveMovementForMember(102),
		"SMOVE-U12 parent dispatch window suppresses legacy movement");
	std::vector<ChildMovementTarget> targets;
	for (const MemberTarget& member : plan.members)
		targets.push_back(RecipientTarget(member, runtime.identity.squadGeneration,
			runtime.decisionRevision, runtime.formationGeneration));
	const OrderKey parent = recipient.ActiveParentKey();
	Require(recipient.DispatchChildren(parent, targets, 1.0),
		"SMOVE-U6 child targets dispatch");
	Require(!recipient.DispatchChildren(parent, targets, 1.0) &&
		capture.starts.size() == 2 && recipient.ActiveChildren().size() == 2,
		"SMOVE-U6 one active movement per child; no tick re-dispatch");
	Require(recipient.HasActiveMovementForMember(101) &&
		recipient.HasActiveMovementForMember(102),
		"SMOVE-U12 each nonterminal modern child owns its member");
	for (std::size_t i = 0; i < targets.size(); ++i)
		Require(recipient.ActiveChildren()[i].record.target.x ==
			targets[i].target.x && recipient.ActiveChildren()[i].record.target.y ==
			targets[i].target.y && recipient.ActiveChildren()[i].record.target.z ==
			targets[i].target.z,
			"SMOVE-U6 resolved slot is stored in the Child order");
	const ChildExecution requested = recipient.ActiveChildren()[0];
	Require(recipient.ObserveNavigationStarted(parent, requested.key,
		requested.memberId, requested.navigationRequestToken, 4001) &&
		recipient.ActiveChildren()[0].navigationIntentBound &&
		recipient.ActiveChildren()[0].navigationIntentId == 4001,
		"SMOVE-U6 normal callback binds the real NavigationIntent id");
	const std::vector<ChildExecution> oldChildren = recipient.ActiveChildren();
	const std::size_t sequenceBeforeReplacement = capture.sequence.size();
	SquadMoveSpec replacement = spec;
	replacement.orderId = 7002;
	replacement.generation = 2;
	replacement.issuedAt = 2.0;
	Require(recipient.BeginMove(replacement), "SMOVE-U7 replacement parent issued");
	const OrderKey newParent = recipient.ActiveParentKey();
	Require(capture.cancels.size() == 2 && capture.sequence.size() >=
		sequenceBeforeReplacement + 2 &&
		capture.sequence[sequenceBeforeReplacement] == "cancel" &&
		capture.sequence[sequenceBeforeReplacement + 1] == "cancel",
		"SMOVE-U7 old executions are cancelled before any replacement starts");
	Require(recipient.DispatchChildren(newParent, targets, 2.0),
		"SMOVE-U7 replacement child targets dispatch");
	Require(!recipient.ObserveNavigationTerminal(parent, oldChildren[0].key,
		oldChildren[0].memberId, oldChildren[0].navigationIntentId,
		NavigationTerminalResult::COMPLETE, 3.0),
		"SMOVE-U8 stale completion cannot affect replacement");
	std::cout << "SMOVE-U6 PASS\nSMOVE-U7 PASS\nSMOVE-U8 PASS\n";
}

void TestLifecycleCompletionAndFailure()
{
	Capture capture;
	SquadMoveRecipient recipient(Start, Cancel, nullptr, &capture);
	const SquadMoveSpec spec = Spec();
	Require(recipient.IssueMove(spec), "SMOVE-U9 lifecycle parent issued");
	const OrderKey parent = recipient.ActiveParentKey();
	const std::vector<ChildExecution> children = recipient.ActiveChildren();
	Require(recipient.ObserveMemberLifecycle(parent, spec.snapshot.recipient,
		101, DetailReason::DEATH_OR_LIFECYCLE, 2.0) &&
		capture.cancels.size() == 1 && recipient.HasActiveParent(),
		"SMOVE-U9 death cancels only its own child movement");
	Require(recipient.ObserveNavigationTerminal(parent, children[1].key, 102,
		children[1].navigationIntentId, NavigationTerminalResult::COMPLETE, 3.0) &&
		recipient.ActiveChildren()[1].record.outcome == OrderOutcome::COMPLETE &&
		!recipient.HasActiveMovementForMember(101) &&
		!recipient.HasActiveMovementForMember(102) &&
		recipient.ParentSnapshot().outcome == OrderOutcome::PARTIAL_FAILURE,
		"SMOVE-U10 final COMPLETE maps to correct child and aggregate");

	Capture failureCapture;
	SquadMoveRecipient failureRecipient(Start, Cancel, nullptr, &failureCapture);
	Require(failureRecipient.IssueMove(spec), "SMOVE-U11 failure parent issued");
	const OrderKey failureParent = failureRecipient.ActiveParentKey();
	const std::vector<ChildExecution> failureChildren =
		failureRecipient.ActiveChildren();
	Require(failureRecipient.ObserveNavigationTerminal(failureParent,
		failureChildren[0].key, 101,
		failureChildren[0].navigationIntentId,
		NavigationTerminalResult::PATH_STUCK, 2.0) &&
		failureRecipient.ActiveChildren()[0].record.outcome == OrderOutcome::STUCK,
		"SMOVE-U11 navigation failure maps through child semantics");
	std::cout << "SMOVE-U9 PASS\nSMOVE-U10 PASS\nSMOVE-U11 PASS\n";
}

void TestLegacyMovementGate()
{
	Require(ShouldSuppressLegacyMovement(true, true) &&
		!ShouldSuppressLegacyMovement(true, false) &&
		!ShouldSuppressLegacyMovement(false, true),
		"SMOVE-U12 legacy movement suppressed only for modern owned member");
	std::cout << "SMOVE-U12 PASS\n";
}
}

int main()
{
	TestSlotTargetAndDistinctAssignments();
	TestExcludedAndCatchingUp();
	TestStaleAssignmentRejected();
	TestDispatchOnceAndReplacement();
	TestLifecycleCompletionAndFailure();
	TestLegacyMovementGate();
	return 0;
}
