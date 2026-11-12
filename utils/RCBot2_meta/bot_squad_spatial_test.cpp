#include "bot_squad_spatial.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <vector>

namespace
{
using namespace RCBotOrder;
using namespace RCBotSquadDecision;
using namespace RCBotSquadSpatial;

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

bool SamePosition(const TargetPosition& left, const TargetPosition& right)
{
	return left.x == right.x && left.y == right.y && left.z == right.z;
}

MemberState Member(const RecipientId id, const TargetPosition origin,
	const bool leader = false)
{
	MemberState member;
	member.memberId = id;
	member.alive = true;
	member.connected = true;
	member.eligible = true;
	member.isLeader = leader;
	member.squadPosition = leader ? 0 : static_cast<std::int32_t>(id - 100);
	member.origin = origin;
	member.velocity = Position(0.0f, 0.0f, 0.0f);
	member.lifecycle = MemberLifecycle::ACTIVE;
	return member;
}

SquadDecisionResult Decision(const std::vector<MemberState>& members,
	const double cohesionRadius = 256.0)
{
	SquadDecisionInput input;
	input.identity = { 2, 42, 7 };
	input.decisionRevision = 1;
	input.commanderOrder.present = true;
	input.commanderOrder.parentOrder = { 9001, 4 };
	input.commanderOrder.target = Position(100.0f, 200.0f, 0.0f);
	input.commanderOrder.deadline = 120.0;
	input.commanderOrder.orderState = OrderState::EXECUTING;
	input.participationPolicy.cohesionRadius = cohesionRadius;
	input.members = members;
	return SquadDecisionCore::Evaluate(input);
}

FormationFrame Frame()
{
	FormationFrame frame;
	frame.anchor = Position(0.0f, 0.0f, 0.0f);
	frame.forward = Position(0.0f, 1.0f, 0.0f);
	frame.right = Position(1.0f, 0.0f, 0.0f);
	return frame;
}

SpatialFormationInput SpatialInput(const SquadDecisionResult& decision,
	const FormationType type = FormationType::COLUMN,
	const std::uint32_t maxSlots = 3,
	const Generation formationGeneration = 5,
	const double now = 0.0)
{
	SpatialFormationInput input;
	input.decision = decision;
	input.formationType = type;
	input.policy.memberSpacing = 10.0;
	input.policy.rowSpacing = 20.0;
	input.policy.slotLeaseDuration = 10.0;
	input.policy.maxSlots = maxSlots;
	input.frame = Frame();
	input.formationGeneration = formationGeneration;
	input.now = now;
	return input;
}

SquadDecisionResult ThreeMembers()
{
	return Decision({ Member(101, Position(0.0f, 0.0f), true),
		Member(102, Position(10.0f, 0.0f)),
		Member(103, Position(-10.0f, 0.0f)) });
}

void RequireAccepted(const FormationDecisionResult& result,
	const char* message)
{
	Require(result.Accepted(), message);
	Require(result.claimSnapshot.IsValid(), "accepted result has valid claims");
}

void TestColumnGeometry()
{
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(ThreeMembers(), FormationType::COLUMN, 3));
	RequireAccepted(result, "FORM-01 column accepted");
	Require(result.slots.size() == 3 &&
		SamePosition(result.slots[0].localOffset, Position(0, 0)) &&
		SamePosition(result.slots[1].localOffset, Position(0, -20)) &&
		SamePosition(result.slots[2].localOffset, Position(0, -40)),
		"FORM-01 column uses deterministic rear rows");
	Require(result.slots[0].role == SpatialRole::ANCHOR &&
		result.slots[1].role == SpatialRole::REAR,
		"FORM-01 column roles");
	std::cout << "FORM-01 PASS\n";
}

void TestLineGeometry()
{
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(ThreeMembers(), FormationType::LINE, 3));
	RequireAccepted(result, "FORM-02 line accepted");
	Require(result.slots.size() == 3 &&
		SamePosition(result.slots[0].localOffset, Position(-10, 0)) &&
		SamePosition(result.slots[1].localOffset, Position(0, 0)) &&
		SamePosition(result.slots[2].localOffset, Position(10, 0)),
		"FORM-02 line is symmetric around anchor");
	Require(result.slots[1].role == SpatialRole::CENTER &&
		result.slots[2].role == SpatialRole::RIGHT,
		"FORM-02 line roles");
	std::cout << "FORM-02 PASS\n";
}

void TestWedgeGeometry()
{
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(ThreeMembers(), FormationType::WEDGE, 5));
	RequireAccepted(result, "FORM-03 wedge accepted");
	Require(SamePosition(result.slots[0].localOffset, Position(0, 0)) &&
		SamePosition(result.slots[1].localOffset, Position(-10, -20)) &&
		SamePosition(result.slots[2].localOffset, Position(10, -20)) &&
		SamePosition(result.slots[3].localOffset, Position(-20, -40)) &&
		SamePosition(result.slots[4].localOffset, Position(20, -40)),
		"FORM-03 wedge is symmetric by row");
	Require(result.slots[1].role == SpatialRole::REAR_LEFT &&
		result.slots[2].role == SpatialRole::REAR_RIGHT,
		"FORM-03 wedge roles");
	std::cout << "FORM-03 PASS\n";
}

void TestMemberOrderDoesNotChangeResult()
{
	std::vector<MemberState> members = {
		Member(101, Position(0, 0), true), Member(102, Position(30, 0)),
		Member(103, Position(-30, 0)) };
	const SquadDecisionResult leftDecision = Decision(members);
	std::swap(members[0], members[2]);
	const SquadDecisionResult rightDecision = Decision(members);
	const FormationDecisionResult left = SquadSpatialCore::Evaluate(
		SpatialInput(leftDecision, FormationType::WEDGE, 3));
	const FormationDecisionResult right = SquadSpatialCore::Evaluate(
		SpatialInput(rightDecision, FormationType::WEDGE, 3));
	RequireAccepted(left, "FORM-04 left accepted");
	RequireAccepted(right, "FORM-04 right accepted");
	Require(left.canonicalFingerprint == right.canonicalFingerprint,
		"FORM-04 input member order preserves fingerprint");
	std::cout << "FORM-04 PASS\n";
}

void TestStableSlotClaims()
{
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(ThreeMembers(), FormationType::LINE, 4, 12));
	RequireAccepted(result, "FORM-05 accepted");
	std::set<std::uint64_t> ids;
	for (const FormationSlot& slot : result.slots)
	{
		Require(slot.claimKey.resourceKind == ResourceKind::SPATIAL_RESOURCE &&
			slot.claimKey.resourceId == slot.slotId &&
			slot.claimKey.resourceGeneration == 12,
			"FORM-05 slot claim key contains stable generation");
		ids.insert(slot.slotId);
	}
	Require(ids.size() == result.slots.size(),
		"FORM-05 slot ids are unique");
	std::cout << "FORM-05 PASS\n";
}

void TestNoDuplicateSlotAssignment()
{
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(Decision({ Member(101, Position(0, 0), true),
			Member(102, Position(0, 0)), Member(103, Position(0, 0)),
			Member(104, Position(0, 0)) }), FormationType::COLUMN, 2));
	RequireAccepted(result, "FORM-06 accepted");
	std::set<std::uint64_t> slots;
	for (const FormationAssignment& assignment : result.assignments)
		slots.insert(assignment.slotId);
	Require(result.assignments.size() == 2 && slots.size() == 2,
		"FORM-06 no slot is assigned twice");
	std::cout << "FORM-06 PASS\n";
}

void TestNoMemberGetsTwoSlots()
{
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(Decision({ Member(101, Position(0, 0), true),
			Member(102, Position(20, 0)), Member(103, Position(-20, 0)) }),
			FormationType::LINE, 8));
	RequireAccepted(result, "FORM-07 accepted");
	std::set<RecipientId> members;
	for (const FormationAssignment& assignment : result.assignments)
		members.insert(assignment.memberId);
	Require(members.size() == result.assignments.size() &&
		result.assignments.size() == 3,
		"FORM-07 each member receives at most one slot");
	std::cout << "FORM-07 PASS\n";
}

void TestExcludedMemberGetsNoSlot()
{
	std::vector<MemberState> members = { Member(101, Position(0, 0), true),
		Member(102, Position(10, 0)) };
	members[1].alive = false;
	members[1].lifecycle = MemberLifecycle::DEAD;
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(Decision(members), FormationType::COLUMN, 3));
	RequireAccepted(result, "FORM-08 accepted");
	for (const FormationAssignment& assignment : result.assignments)
		Require(assignment.memberId != 102, "FORM-08 excluded member has no slot");
	std::cout << "FORM-08 PASS\n";
}

void TestCatchingUpRemainsAssignable()
{
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(Decision({ Member(101, Position(0, 0), true),
			Member(102, Position(1000, 0)) }), FormationType::COLUMN, 2));
	RequireAccepted(result, "FORM-09 accepted");
	bool foundCatchingUp = false;
	for (const FormationAssignment& assignment : result.assignments)
	{
		if (assignment.memberId == 102)
		{
			foundCatchingUp = true;
			Require(assignment.participation == ParticipationState::CATCHING_UP &&
				assignment.claimStatus == SpatialClaimStatus::CLAIMED,
				"FORM-09 catching-up is assignable without movement action");
		}
	}
	Require(foundCatchingUp, "FORM-09 catching-up member retains assignment");
	std::cout << "FORM-09 PASS\n";
}

void TestTieUsesStableMemberId()
{
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(Decision({ Member(202, Position(0, 0), true),
			Member(201, Position(0, 0)) }), FormationType::COLUMN, 1));
	RequireAccepted(result, "FORM-10 accepted");
	Require(result.assignments.size() == 1 && result.assignments[0].memberId == 201,
		"FORM-10 equal geometric cost uses smaller member id");
	std::cout << "FORM-10 PASS\n";
}

void TestFormationGenerationPurgesOldClaims()
{
	const SquadDecisionResult decision = ThreeMembers();
	SpatialFormationInput firstInput = SpatialInput(decision,
		FormationType::COLUMN, 3, 5);
	const FormationDecisionResult first = SquadSpatialCore::Evaluate(firstInput);
	RequireAccepted(first, "FORM-11 first generation accepted");
	SpatialFormationInput nextInput = SpatialInput(decision,
		FormationType::COLUMN, 3, 6);
	nextInput.hasExistingClaims = true;
	nextInput.existingClaims = first.claimSnapshot;
	const FormationDecisionResult next = SquadSpatialCore::Evaluate(nextInput);
	RequireAccepted(next, "FORM-11 next generation accepted");
	for (const ClaimRecord& claim : next.claimSnapshot.claims)
		Require(claim.key.resourceKind != ResourceKind::SPATIAL_RESOURCE ||
			claim.key.resourceGeneration == 6,
			"FORM-11 old-generation spatial claim purged");
	for (const FormationAssignment& assignment : next.assignments)
		Require(assignment.claimKey.resourceGeneration == 6,
			"FORM-11 assignments use new formation generation");
	std::cout << "FORM-11 PASS\n";
}

void TestMemberLifecyclePurgesSlotClaim()
{
	const std::vector<MemberState> initialMembers = {
		Member(101, Position(0, 0), true), Member(102, Position(10, 0)) };
	const SquadDecisionResult initialDecision = Decision(initialMembers);
	const FormationDecisionResult first = SquadSpatialCore::Evaluate(
		SpatialInput(initialDecision, FormationType::COLUMN, 2));
	RequireAccepted(first, "FORM-12 first result accepted");

	std::vector<MemberState> nextMembers = initialMembers;
	nextMembers[0].alive = false;
	nextMembers[0].lifecycle = MemberLifecycle::DEAD;
	SpatialFormationInput nextInput = SpatialInput(Decision(nextMembers),
		FormationType::COLUMN, 2);
	nextInput.hasExistingClaims = true;
	nextInput.existingClaims = first.claimSnapshot;
	const FormationDecisionResult next = SquadSpatialCore::Evaluate(nextInput);
	RequireAccepted(next, "FORM-12 lifecycle result accepted");
	for (const ClaimRecord& claim : next.claimSnapshot.claims)
		Require(claim.ownerMemberId != 101,
			"FORM-12 dead member spatial claim purged");
	std::cout << "FORM-12 PASS\n";
}

void TestZeroParticipants()
{
	std::vector<MemberState> members = { Member(101, Position(0, 0), true),
		Member(102, Position(10, 0)) };
	for (MemberState& member : members)
	{
		member.alive = false;
		member.lifecycle = MemberLifecycle::DEAD;
	}
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(Decision(members), FormationType::WEDGE, 5));
	RequireAccepted(result, "FORM-13 zero participants accepted");
	Require(result.slots.empty() && result.assignments.empty(),
		"FORM-13 zero participants produce no slots or assignments");
	std::cout << "FORM-13 PASS\n";
}

void TestSingleParticipantWithExtraSlots()
{
	const FormationDecisionResult result = SquadSpatialCore::Evaluate(
		SpatialInput(Decision({ Member(101, Position(0, 0), true) }),
			FormationType::LINE, 4));
	RequireAccepted(result, "FORM-14 single participant accepted");
	Require(result.slots.size() == 4 && result.assignments.size() == 1,
		"FORM-14 slot geometry may exceed participant count");
	std::cout << "FORM-14 PASS\n";
}

void TestInvalidFrameRejected()
{
	SpatialFormationInput input = SpatialInput(ThreeMembers());
	input.frame.forward.x = std::numeric_limits<float>::quiet_NaN();
	const FormationDecisionResult nonFinite = SquadSpatialCore::Evaluate(input);
	Require(!nonFinite.Accepted() &&
		nonFinite.diagnostic == FormationDiagnostic::INVALID_FRAME,
		"FORM-15 nonfinite frame rejected");

	input = SpatialInput(ThreeMembers());
	input.frame.forward = Position(1, 0, 0);
	input.frame.right = Position(2, 0, 0);
	const FormationDecisionResult parallel = SquadSpatialCore::Evaluate(input);
	Require(!parallel.Accepted() &&
		parallel.diagnostic == FormationDiagnostic::INVALID_FRAME,
		"FORM-15 degenerate frame rejected");
	std::cout << "FORM-15 PASS\n";
}

void TestIdenticalInputHasIdenticalFingerprint()
{
	const SpatialFormationInput input = SpatialInput(ThreeMembers(),
		FormationType::WEDGE, 5, 9);
	const FormationDecisionResult left = SquadSpatialCore::Evaluate(input);
	const FormationDecisionResult right = SquadSpatialCore::Evaluate(input);
	RequireAccepted(left, "FORM-16 left accepted");
	RequireAccepted(right, "FORM-16 right accepted");
	Require(left.canonicalFingerprint == right.canonicalFingerprint,
		"FORM-16 identical input has identical fingerprint");
	Require(left.claimSnapshot.canonicalFingerprint ==
		right.claimSnapshot.canonicalFingerprint,
		"FORM-16 identical claim snapshot fingerprint");
	std::cout << "FORM-16 PASS\n";
}
}

int main()
{
	TestColumnGeometry();
	TestLineGeometry();
	TestWedgeGeometry();
	TestMemberOrderDoesNotChangeResult();
	TestStableSlotClaims();
	TestNoDuplicateSlotAssignment();
	TestNoMemberGetsTwoSlots();
	TestExcludedMemberGetsNoSlot();
	TestCatchingUpRemainsAssignable();
	TestTieUsesStableMemberId();
	TestFormationGenerationPurgesOldClaims();
	TestMemberLifecyclePurgesSlotClaim();
	TestZeroParticipants();
	TestSingleParticipantWithExtraSlots();
	TestInvalidFrameRejected();
	TestIdenticalInputHasIdenticalFingerprint();
	std::cout << "RESULT FORM-01..FORM-16 PASS\n";
	return 0;
}
