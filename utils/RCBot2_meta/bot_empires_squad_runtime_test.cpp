#include "bot_empires_squad_runtime.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
using namespace RCBotOrder;
using namespace RCBotSquadDecision;
using namespace RCBotEmpiresSquadRuntime;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

TargetPosition Position(const float x, const float y, const float z = 0.0f)
{
	return { x, y, z };
}

MemberObservation Member(const RecipientId id, const TargetPosition origin,
	const bool leader = false)
{
	MemberObservation result;
	result.state.memberId = id;
	result.state.alive = true;
	result.state.connected = true;
	result.state.eligible = true;
	result.state.isLeader = leader;
	result.state.squadPosition = leader ? 0 : static_cast<int>(id - 100);
	result.state.origin = origin;
	result.state.velocity = Position(0, 0, 0);
	result.state.lifecycle = MemberLifecycle::ACTIVE;
	return result;
}

Input BaseInput()
{
	Input input;
	input.team = 2;
	input.squadId = 7;
	input.now = 1.0;
	input.members = { Member(101, Position(0, 0), true),
		Member(102, Position(64, 0)) };
	input.parentOrder.present = true;
	input.parentOrder.target = Position(128, 0);
	input.parentOrder.deadline = 20.0;
	return input;
}

struct Events
{
	std::vector<Telemetry> values;
};

void Capture(void* context, const Telemetry& telemetry)
{
	static_cast<Events*>(context)->values.push_back(telemetry);
}

void TestCanonicalOrderAndOneDecision()
{
	Input left = BaseInput();
	Input right = BaseInput();
	std::swap(right.members[0], right.members[1]);
	Adapter leftAdapter;
	Adapter rightAdapter;
	const Result leftResult = leftAdapter.Observe(left);
	const Result rightResult = rightAdapter.Observe(right);
	Require(leftResult.Accepted() && rightResult.Accepted(),
		"SRT-U1 runtime observations accepted");
	Require(leftResult.snapshotFingerprint == rightResult.snapshotFingerprint,
		"SRT-U1 member order is canonical");

	Events events;
	Adapter observedAdapter;
	const Result first = observedAdapter.Observe(left, Capture, &events);
	const Result second = observedAdapter.Observe(left, Capture, &events);
	Require(first.Accepted() && second.Accepted() && !second.semanticChange,
		"SRT-U2 repeated observation reuses one logical decision");
	Require(events.values.size() == 5,
		"SRT-U2 repeated observation emits only one semantic event set");
	std::cout << "SRT-U1 PASS\nSRT-U2 PASS\n";
}

void TestLeaderChangeKeepsIdentity()
{
	Adapter adapter;
	Input input = BaseInput();
	const Result first = adapter.Observe(input);
	input.members[0].state.isLeader = false;
	input.members[0].state.squadPosition = 1;
	input.members[1].state.isLeader = true;
	input.members[1].state.squadPosition = 0;
	const Result changed = adapter.Observe(input);
	Require(first.Accepted() && changed.Accepted(), "SRT-U3 leader change accepted");
	Require(first.identity.squadGeneration == changed.identity.squadGeneration,
		"SRT-U3 leader change preserves SquadIdentity");
	Require(changed.decisionRevision > first.decisionRevision,
		"SRT-U3 leader change revises decision");
	std::cout << "SRT-U3 PASS\n";
}

void TestLeaveAndDeathCleanup()
{
	Adapter adapter;
	Input input = BaseInput();
	const Result first = adapter.Observe(input);
	Require(first.formation.claimSnapshot.claims.size() == 2,
		"SRT-U4 initial members receive spatial claims");
	input.members.erase(input.members.begin() + 1);
	const Result left = adapter.Observe(input);
	Require(left.Accepted(), "SRT-U4 leave revision accepted");
	for (const auto& claim : left.formation.claimSnapshot.claims)
		Require(claim.ownerMemberId != 102, "SRT-U4 leave purges member claim");

	input = BaseInput();
	const Result alive = adapter.Observe(input);
	input.members[1].state.alive = false;
	input.members[1].state.eligible = false;
	input.members[1].state.lifecycle = MemberLifecycle::DEAD;
	const Result dead = adapter.Observe(input);
	Require(alive.Accepted() && dead.Accepted(), "SRT-U5 death revision accepted");
	for (const auto& assignment : dead.formation.assignments)
		Require(assignment.memberId != 102, "SRT-U5 dead member gets no slot");
	std::cout << "SRT-U4 PASS\nSRT-U5 PASS\n";
}

void TestReplacementAndStaleCallback()
{
	Adapter adapter;
	Input input = BaseInput();
	const Result first = adapter.Observe(input);
	input.parentOrder.target = Position(0, 256);
	const Result replacement = adapter.Observe(input);
	Require(replacement.Accepted() && replacement.parentOrder.orderId !=
		first.parentOrder.orderId, "SRT-U6 replacement has a new parent key");
	Require(replacement.formationGeneration > first.formationGeneration,
		"SRT-U6 replacement invalidates old formation generation");
	Require(adapter.RejectStaleCallback(2, 7, first.identity.squadGeneration,
		first.decisionRevision), "SRT-U7 stale callback is rejected");
	Require(!adapter.RejectStaleCallback(2, 7,
		replacement.identity.squadGeneration, replacement.decisionRevision),
		"SRT-U7 current callback remains current");
	std::cout << "SRT-U6 PASS\nSRT-U7 PASS\n";
}

void TestHumanBoundaryAndFallback()
{
	Adapter adapter;
	Input human = BaseInput();
	human.members[1].human = true;
	const Result humanResult = adapter.Observe(human);
	Require(humanResult.status == Status::NOT_AUTONOMOUS,
		"SRT-U8 human-containing squad is not autonomous");

	Adapter firstAdapter;
	Adapter secondAdapter;
	Input fallback = BaseInput();
	fallback.parentOrder.target = Position(32, 0);
	fallback.members[0].state.origin = Position(16, 0);
	fallback.members[1].state.origin = Position(48, 0);
	const Result first = firstAdapter.Observe(fallback);
	const Result second = secondAdapter.Observe(fallback);
	Require(first.Accepted() && second.Accepted(), "SRT-U9 fallback accepted");
	Require(first.formation.frame.forward.x == second.formation.frame.forward.x &&
		first.formation.frame.right.y == second.formation.frame.right.y,
		"SRT-U9 centroid fallback is deterministic");
	std::cout << "SRT-U8 PASS\nSRT-U9 PASS\n";
}

void TestShadowHasNoMovementSideEffects()
{
	Adapter adapter;
	Events events;
	const Result result = adapter.Observe(BaseInput(), Capture, &events);
	Require(result.Accepted(), "SRT-U10 shadow decision accepted");
	for (const Telemetry& event : events.values)
		Require(!event.movementRequested, "SRT-U10 shadow never requests movement");
	std::cout << "SRT-U10 PASS\n";
}

void TestActiveSpatialPlanDoesNotMarch()
{
	Adapter adapter;
	Input input = BaseInput();
	input.freezeSpatialPlanForActiveParent = true;
	const Result initial = adapter.Observe(input);
	input.now += 1.0;
	input.members[0].state.origin = Position(96, 96);
	input.members[1].state.origin = Position(160, 96);
	const Result moved = adapter.Observe(input);
	Require(initial.Accepted() && moved.Accepted() && !moved.semanticChange,
		"SRT-U11 ordinary movement does not revise a frozen final-slot plan");
	Require(moved.decisionRevision == initial.decisionRevision &&
		moved.formationGeneration == initial.formationGeneration &&
		moved.formation.assignments[0].targetPosition.x ==
		initial.formation.assignments[0].targetPosition.x &&
		moved.formation.assignments[0].targetPosition.y ==
		initial.formation.assignments[0].targetPosition.y,
		"SRT-U11 final target frame stays fixed during the Parent");
	input.members[0].state.isLeader = false;
	input.members[0].state.squadPosition = 1;
	input.members[1].state.isLeader = true;
	input.members[1].state.squadPosition = 0;
	const Result leaderChange = adapter.Observe(input);
	Require(leaderChange.Accepted() && leaderChange.semanticChange &&
		leaderChange.decisionRevision > initial.decisionRevision,
		"SRT-U11 meaningful leader change still revises the active decision");
	std::cout << "SRT-U11 PASS\n";
}
}

int main()
{
	TestCanonicalOrderAndOneDecision();
	TestLeaderChangeKeepsIdentity();
	TestLeaveAndDeathCleanup();
	TestReplacementAndStaleCallback();
	TestHumanBoundaryAndFallback();
	TestShadowHasNoMovementSideEffects();
	TestActiveSpatialPlanDoesNotMarch();
	return 0;
}
