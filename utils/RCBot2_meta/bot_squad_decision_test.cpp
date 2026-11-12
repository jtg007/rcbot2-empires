#include "bot_squad_decision.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace
{
using namespace RCBotOrder;
using namespace RCBotSquadDecision;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

TargetPosition Position(const float x, const float y, const float z)
{
	return { x, y, z };
}

SquadIdentity Identity(const Generation generation = 7)
{
	return { 2, 42, generation };
}

MemberState Member(const RecipientId id, const bool leader = false)
{
	MemberState member;
	member.memberId = id;
	member.alive = true;
	member.connected = true;
	member.eligible = true;
	member.isLeader = leader;
	member.squadPosition = leader ? 0 : static_cast<std::int32_t>(id - 100);
	member.origin = Position(static_cast<float>(id), 20.0f, 30.0f);
	member.velocity = Position(1.0f, 2.0f, 3.0f);
	return member;
}

SquadDecisionInput Input(const DecisionRevision revision = 1)
{
	SquadDecisionInput input;
	input.identity = Identity();
	input.decisionRevision = revision;
	input.commanderOrder.present = true;
	input.commanderOrder.parentOrder = { 9001, 4 };
	input.commanderOrder.target = Position(100.0f, 200.0f, 300.0f);
	input.commanderOrder.deadline = 120.0;
	input.commanderOrder.orderState = OrderState::EXECUTING;
	input.members = { Member(102), Member(101, true) };
	return input;
}

SquadDecisionRevisionContext Previous(const SquadDecisionResult& result)
{
	return { true, result.identity, result.decisionRevision,
		result.canonicalFingerprint };
}

void TestMemberOrderCanonicalization()
{
	const SquadDecisionResult left = SquadDecisionCore::Evaluate(Input());
	SquadDecisionInput reordered = Input();
	std::swap(reordered.members[0], reordered.members[1]);
	const SquadDecisionResult right = SquadDecisionCore::Evaluate(reordered);
	Require(left.Accepted() && right.Accepted(), "SDEC-01 accepted canonical inputs");
	Require(left.canonicalMembers[0].memberId == 101 &&
		right.canonicalMembers[0].memberId == 101,
		"SDEC-01 stable member ordering");
	Require(left.canonicalFingerprint == right.canonicalFingerprint,
		"SDEC-01 order-independent fingerprint");
	std::cout << "SDEC-01 PASS\n";
}

void TestIdenticalInputIsUnchanged()
{
	const SquadDecisionResult first = SquadDecisionCore::Evaluate(Input());
	const SquadDecisionResult second = SquadDecisionCore::Evaluate(Input(), Previous(first));
	Require(second.status == DecisionStatus::ACCEPTED_UNCHANGED && !second.changed,
		"SDEC-02 identical input has no semantic change");
	Require(first.canonicalFingerprint == second.canonicalFingerprint &&
		first.leaderId == second.leaderId &&
		first.eligibleMembers == second.eligibleMembers,
		"SDEC-02 identical result data");
	std::cout << "SDEC-02 PASS\n";
}

void TestDuplicateMemberRejected()
{
	SquadDecisionInput input = Input();
	input.members[1].memberId = input.members[0].memberId;
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.status == DecisionStatus::REJECTED_INVALID_INPUT &&
		result.diagnostic == DecisionDiagnostic::DUPLICATE_MEMBER_ID,
		"SDEC-03 duplicate member ids rejected");
	std::cout << "SDEC-03 PASS\n";
}

void TestInvalidFactsRejected()
{
	SquadDecisionInput input = Input();
	input.members[0].origin.x = std::numeric_limits<float>::quiet_NaN();
	const SquadDecisionResult memberResult = SquadDecisionCore::Evaluate(input);
	Require(memberResult.diagnostic == DecisionDiagnostic::NON_FINITE_MEMBER_FACT,
		"SDEC-04 non-finite member fact rejected");

	input = Input();
	input.commanderOrder.target.y = std::numeric_limits<float>::infinity();
	const SquadDecisionResult orderResult = SquadDecisionCore::Evaluate(input);
	Require(orderResult.diagnostic == DecisionDiagnostic::NON_FINITE_ORDER_FACT,
		"SDEC-04 non-finite order fact rejected");
	std::cout << "SDEC-04 PASS\n";
}

void TestLeaderRevisionSemantics()
{
	const SquadDecisionResult first = SquadDecisionCore::Evaluate(Input());
	SquadDecisionInput changed = Input(2);
	changed.members[0].isLeader = true;
	changed.members[0].squadPosition = 0;
	changed.members[1].isLeader = false;
	const SquadDecisionResult next = SquadDecisionCore::Evaluate(changed, Previous(first));
	Require(next.Accepted() && next.changed, "SDEC-05/06 leader change accepted");
	Require(next.identity == first.identity && next.identity.squadGeneration == 7,
		"SDEC-05 leader change preserves squad identity");
	Require(next.decisionRevision == 2 && next.leaderId == 102,
		"SDEC-06 leader change advances decision revision");
	std::cout << "SDEC-05 PASS\nSDEC-06 PASS\n";
}

void TestMembershipRevisionSemantics()
{
	const SquadDecisionResult first = SquadDecisionCore::Evaluate(Input());
	SquadDecisionInput changed = Input(2);
	changed.members.pop_back();
	const SquadDecisionResult next = SquadDecisionCore::Evaluate(changed, Previous(first));
	Require(next.status == DecisionStatus::ACCEPTED_NEW && next.changed,
		"SDEC-07 membership change advances snapshot");
	Require(next.canonicalMembers.size() == 1 && next.eligibleMembers.size() == 1,
		"SDEC-07 membership change canonicalized");
	std::cout << "SDEC-07 PASS\n";
}

void TestStaleSquadGenerationRejected()
{
	const SquadDecisionResult first = SquadDecisionCore::Evaluate(Input());
	SquadDecisionInput stale = Input(2);
	stale.identity.squadGeneration = 8;
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(stale, Previous(first));
	Require(result.status == DecisionStatus::REJECTED_STALE_SQUAD &&
		result.diagnostic == DecisionDiagnostic::STALE_SQUAD_GENERATION,
		"SDEC-08 stale squad generation rejected");
	std::cout << "SDEC-08 PASS\n";
}

void TestStaleDecisionRevisionRejected()
{
	const SquadDecisionResult first = SquadDecisionCore::Evaluate(Input(4));
	SquadDecisionInput stale = Input(3);
	stale.members[0].origin.x += 1.0f;
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(stale, Previous(first));
	Require(result.status == DecisionStatus::REJECTED_STALE_REVISION &&
		result.diagnostic == DecisionDiagnostic::STALE_DECISION_REVISION,
		"SDEC-09 stale decision revision rejected");
	std::cout << "SDEC-09 PASS\n";
}

void TestEligibleMemberView()
{
	SquadDecisionInput input = Input();
	input.members[0].alive = false;
	input.members[1].connected = false;
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.Accepted() && result.eligibleMembers.empty(),
		"SDEC-10 dead/disconnected members excluded");

	input = Input();
	input.members[0].eligible = false;
	const SquadDecisionResult ineligible = SquadDecisionCore::Evaluate(input);
	Require(ineligible.eligibleMembers.size() == 1 &&
		ineligible.eligibleMembers[0] == 101,
		"SDEC-10 ineligible member excluded");
	std::cout << "SDEC-10 PASS\n";
}

void TestCommanderOrderPreserved()
{
	const SquadDecisionInput input = Input();
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.commanderOrder.present &&
		result.commanderOrder.parentOrder.orderId == 9001 &&
		result.commanderOrder.parentOrder.generation == 4 &&
		result.commanderOrder.target.x == 100.0f &&
		result.commanderOrder.deadline == 120.0,
		"SDEC-11 parent order key and data preserved");
	std::cout << "SDEC-11 PASS\n";
}

void TestHeadlessValueContract()
{
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(Input());
	Require(result.Accepted() && result.canonicalMembers.size() == 2,
		"SDEC-12 headless value-only evaluation");
	Require(DecisionStatusName(result.status) != nullptr &&
		DecisionDiagnosticName(result.diagnostic) != nullptr,
		"SDEC-12 stable diagnostic names");
	std::cout << "SDEC-12 PASS\n";
}

void TestCohesionInputOrder()
{
	const SquadDecisionResult left = SquadDecisionCore::Evaluate(Input());
	SquadDecisionInput reordered = Input();
	std::swap(reordered.members[0], reordered.members[1]);
	const SquadDecisionResult right = SquadDecisionCore::Evaluate(reordered);
	Require(left.cohesion.valid && right.cohesion.valid,
		"COH-01 valid cohesion facts");
	Require(left.cohesion.centroid.x == right.cohesion.centroid.x &&
		left.cohesion.averageDistanceFromCentroid ==
			right.cohesion.averageDistanceFromCentroid &&
		left.cohesion.distanceSpread == right.cohesion.distanceSpread,
		"COH-01 order-independent cohesion facts");
	Require(left.cohesion.memberFacts[0].memberId == 101 &&
		right.cohesion.memberFacts[0].memberId == 101,
		"COH-01 canonical cohesion member order");
	std::cout << "COH-01 PASS\n";
}

void TestSingleEligibleMember()
{
	SquadDecisionInput input = Input();
	input.members.pop_back();
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.cohesion.valid && result.cohesion.eligibleMemberCount == 1,
		"COH-02 one eligible member is valid");
	Require(result.cohesion.minDistanceFromCentroid == 0.0 &&
		result.cohesion.maxDistanceFromCentroid == 0.0 &&
		result.cohesion.averageDistanceFromCentroid == 0.0 &&
		result.cohesion.distanceSpread == 0.0,
		"COH-02 zero single-member spread");
	Require(result.participation.size() == 1 &&
		result.participation[0].state == ParticipationState::PARTICIPATING,
		"COH-02 single member participates");
	std::cout << "COH-02 PASS\n";
}

void TestZeroEligibleMembers()
{
	SquadDecisionInput input = Input();
	for (MemberState& member : input.members)
		member.alive = false;
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.cohesion.valid && result.cohesion.eligibleMemberCount == 0,
		"COH-03 zero eligible members is explicit");
	Require(result.cohesion.centroid.x == 0.0f &&
		result.cohesion.centroid.y == 0.0f &&
		result.cohesion.centroid.z == 0.0f &&
		result.cohesion.distanceSpread == 0.0,
		"COH-03 neutral zero-member facts");
	for (const ParticipationDecision& decision : result.participation)
	{
		Require(decision.state == ParticipationState::EXCLUDED &&
			std::isfinite(decision.distanceToCentroid) &&
			std::isfinite(decision.distanceToObjective),
			"COH-03 excluded members have finite facts");
	}
	std::cout << "COH-03 PASS\n";
}

void TestOutlierSpread()
{
	SquadDecisionInput baseline = Input();
	baseline.members[0].origin = Position(0.0f, 0.0f, 0.0f);
	baseline.members[1].origin = Position(0.0f, 0.0f, 0.0f);
	const SquadDecisionResult compact = SquadDecisionCore::Evaluate(baseline);

	SquadDecisionInput outlier = baseline;
	outlier.members.push_back(Member(103));
	outlier.members[2].origin = Position(1000.0f, 0.0f, 0.0f);
	const SquadDecisionResult expanded = SquadDecisionCore::Evaluate(outlier);
	Require(expanded.cohesion.maxDistanceFromCentroid >
		compact.cohesion.maxDistanceFromCentroid &&
		expanded.cohesion.distanceSpread > compact.cohesion.distanceSpread,
		"COH-04 outlier increases cohesion spread");
	const SquadDecisionResult repeated = SquadDecisionCore::Evaluate(outlier);
	Require(expanded.cohesion.centroid.x == repeated.cohesion.centroid.x &&
		expanded.cohesion.distanceSpread == repeated.cohesion.distanceSpread,
		"COH-04 deterministic outlier facts");
	std::cout << "COH-04 PASS\n";
}

void TestCohesionNonFiniteFactsRejected()
{
	SquadDecisionInput input = Input();
	input.members[0].velocity.z = std::numeric_limits<float>::infinity();
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.status == DecisionStatus::REJECTED_INVALID_INPUT &&
		result.diagnostic == DecisionDiagnostic::NON_FINITE_MEMBER_FACT,
		"COH-05 non-finite geometry rejected");
	std::cout << "COH-05 PASS\n";
}

void TestObjectiveDistanceFacts()
{
	SquadDecisionInput input = Input();
	input.members[0].origin = Position(0.0f, 0.0f, 0.0f);
	input.members[1].origin = Position(0.0f, 0.0f, 4.0f);
	input.commanderOrder.target = Position(0.0f, 0.0f, 100.0f);
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.cohesion.objectivePresent &&
		result.cohesion.objectiveDistanceFromCentroid == 98.0 &&
		result.cohesion.memberFacts[0].memberId == 101 &&
		result.cohesion.memberFacts[0].distanceToObjective == 96.0 &&
		result.cohesion.memberFacts[1].memberId == 102 &&
		result.cohesion.memberFacts[1].distanceToObjective == 100.0,
		"COH-06 deterministic objective distances");
	std::cout << "COH-06 PASS\n";
}

void TestCohesionResults()
{
	TestCohesionInputOrder();
	TestSingleEligibleMember();
	TestZeroEligibleMembers();
	TestOutlierSpread();
	TestCohesionNonFiniteFactsRejected();
	TestObjectiveDistanceFacts();
	std::cout << "RESULT COH-01..COH-06 PASS\n";
}

void TestCohesiveMemberParticipates()
{
	SquadDecisionInput input = Input();
	input.members[0].origin = Position(0.0f, 0.0f, 0.0f);
	input.members[1].origin = Position(100.0f, 0.0f, 0.0f);
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.participation.size() == 2 &&
		result.participation[0].state == ParticipationState::PARTICIPATING &&
		result.participation[1].state == ParticipationState::PARTICIPATING,
		"PART-01 cohesive eligible members participate");
	std::cout << "PART-01 PASS\n";
}

void TestIneligibleAndDeadLeaderExcluded()
{
	SquadDecisionInput input = Input();
	input.members[1].alive = false;
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.leaderId == 101 && result.participation.size() == 2 &&
		result.participation[0].memberId == 101 &&
		result.participation[0].state == ParticipationState::EXCLUDED &&
		result.participation[1].state == ParticipationState::PARTICIPATING,
		"PART-02 dead leader is excluded without losing leader metadata");
	std::cout << "PART-02 PASS\n";
}

void TestDistantEligibleMembersCatchUp()
{
	SquadDecisionInput input = Input();
	input.members[0].origin = Position(0.0f, 0.0f, 0.0f);
	input.members[1].origin = Position(1000.0f, 0.0f, 0.0f);
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.participation[0].state == ParticipationState::CATCHING_UP &&
		result.participation[1].state == ParticipationState::CATCHING_UP,
		"PART-03 distant eligible members catch up");
	std::cout << "PART-03 PASS\n";
}

void TestCohesionThresholdIsInclusive()
{
	SquadDecisionInput input = Input();
	input.members[0].origin = Position(-256.0f, 0.0f, 0.0f);
	input.members[1].origin = Position(256.0f, 0.0f, 0.0f);
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(input);
	Require(result.participation[0].distanceToCentroid == 256.0 &&
		result.participation[1].distanceToCentroid == 256.0 &&
		result.participation[0].state == ParticipationState::PARTICIPATING &&
		result.participation[1].state == ParticipationState::PARTICIPATING,
		"PART-04 threshold is inclusive and deterministic");
	std::cout << "PART-04 PASS\n";
}

void TestLeaderChangeKeepsIdentity()
{
	const SquadDecisionResult first = SquadDecisionCore::Evaluate(Input());
	SquadDecisionInput changed = Input(2);
	changed.members[0].isLeader = true;
	changed.members[0].squadPosition = 0;
	changed.members[1].isLeader = false;
	const SquadDecisionResult next = SquadDecisionCore::Evaluate(changed,
		Previous(first));
	Require(next.Accepted() && next.changed && next.identity == first.identity &&
		next.leaderId == 102 && next.decisionRevision == 2,
		"PART-05 leader change preserves identity");
	std::cout << "PART-05 PASS\n";
}

void TestParticipationOrderIndependence()
{
	const SquadDecisionResult left = SquadDecisionCore::Evaluate(Input());
	SquadDecisionInput reordered = Input();
	std::swap(reordered.members[0], reordered.members[1]);
	const SquadDecisionResult right = SquadDecisionCore::Evaluate(reordered);
	Require(left.canonicalFingerprint == right.canonicalFingerprint &&
		left.participation.size() == right.participation.size(),
		"PART-06 input order preserves participation result");
	for (std::size_t i = 0; i < left.participation.size(); ++i)
	{
		Require(left.participation[i].memberId == right.participation[i].memberId &&
			left.participation[i].state == right.participation[i].state &&
			left.participation[i].distanceToCentroid ==
				right.participation[i].distanceToCentroid,
			"PART-06 canonical participation entries");
	}
	std::cout << "PART-06 PASS\n";
}

void TestIdenticalParticipationResult()
{
	const SquadDecisionResult first = SquadDecisionCore::Evaluate(Input());
	const SquadDecisionResult second = SquadDecisionCore::Evaluate(Input(),
		Previous(first));
	Require(second.status == DecisionStatus::ACCEPTED_UNCHANGED &&
		first.canonicalFingerprint == second.canonicalFingerprint &&
		first.cohesion.centroid.x == second.cohesion.centroid.x &&
		first.participation[0].state == second.participation[0].state,
		"PART-07 identical input has identical result");
	std::cout << "PART-07 PASS\n";
}

void TestObjectiveReplacementChangesSnapshot()
{
	const SquadDecisionResult first = SquadDecisionCore::Evaluate(Input());
	SquadDecisionInput changed = Input(2);
	changed.commanderOrder.target.x += 1000.0f;
	const SquadDecisionResult next = SquadDecisionCore::Evaluate(changed,
		Previous(first));
	Require(next.status == DecisionStatus::ACCEPTED_NEW && next.changed &&
		next.identity == first.identity &&
		next.canonicalFingerprint != first.canonicalFingerprint &&
		next.cohesion.objectiveDistanceFromCentroid !=
			first.cohesion.objectiveDistanceFromCentroid,
		"PART-08 objective replacement changes snapshot only");
	std::cout << "PART-08 PASS\n";
}

void TestParticipationStaleProtection()
{
	const SquadDecisionResult first = SquadDecisionCore::Evaluate(Input(4));
	SquadDecisionInput staleRevision = Input(3);
	staleRevision.members[0].origin.x += 1.0f;
	const SquadDecisionResult revisionResult =
		SquadDecisionCore::Evaluate(staleRevision, Previous(first));
	Require(revisionResult.status == DecisionStatus::REJECTED_STALE_REVISION,
		"PART-09 stale revision rejected");

	SquadDecisionInput staleGeneration = Input(5);
	staleGeneration.identity.squadGeneration = 8;
	const SquadDecisionResult generationResult =
		SquadDecisionCore::Evaluate(staleGeneration, Previous(first));
	Require(generationResult.status == DecisionStatus::REJECTED_STALE_SQUAD,
		"PART-09 stale generation rejected");
	std::cout << "PART-09 PASS\n";
}

void TestNoActionIntentProduced()
{
	const SquadDecisionResult result = SquadDecisionCore::Evaluate(Input());
	Require(result.cohesion.valid && !result.participation.empty() &&
		DecisionStatusName(result.status) != nullptr &&
		ParticipationStateName(result.participation[0].state) != nullptr,
		"PART-10 result contains facts and states only");
	std::cout << "PART-10 PASS\n";
}

void TestParticipationResults()
{
	TestCohesiveMemberParticipates();
	TestIneligibleAndDeadLeaderExcluded();
	TestDistantEligibleMembersCatchUp();
	TestCohesionThresholdIsInclusive();
	TestLeaderChangeKeepsIdentity();
	TestParticipationOrderIndependence();
	TestIdenticalParticipationResult();
	TestObjectiveReplacementChangesSnapshot();
	TestParticipationStaleProtection();
	TestNoActionIntentProduced();
	std::cout << "RESULT PART-01..PART-10 PASS\n";
}
}

int main()
{
	TestMemberOrderCanonicalization();
	TestIdenticalInputIsUnchanged();
	TestDuplicateMemberRejected();
	TestInvalidFactsRejected();
	TestLeaderRevisionSemantics();
	TestMembershipRevisionSemantics();
	TestStaleSquadGenerationRejected();
	TestStaleDecisionRevisionRejected();
	TestEligibleMemberView();
	TestCommanderOrderPreserved();
	TestHeadlessValueContract();
	std::cout << "RESULT SDEC-01..SDEC-12 PASS\n";
	TestCohesionResults();
	TestParticipationResults();
	return 0;
}
