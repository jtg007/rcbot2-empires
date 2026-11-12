#include "bot_empires_leader_actuation.h"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace RCBotEmpiresLeadActuation;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

LeadActuationParty Party(const int userId, const int entIndex,
	const int team, const int squad, const bool positionKnown,
	const int position, const bool xpKnown = false, const int xp = -1,
	const bool connected = true)
{
	LeadActuationParty party;
	party.identity.userId = userId;
	party.identity.entIndex = entIndex;
	party.connected = connected;
	party.team = team;
	party.squad = squad;
	party.position = position;
	party.positionKnown = positionKnown;
	party.squadXp = xp;
	party.squadXpKnown = xpKnown;
	return party;
}

LeadActuationRequest Request()
{
	LeadActuationRequest request;
	request.issuer = Party(2, 1, 2, 1, true, 0, true, 5);
	request.target = Party(3, 2, 2, 1, true, 1, true, 5);
	request.status = LeadActuationStatus::REQUESTED;
	return request;
}

void TestStatusAndReasonNames()
{
	Require(LeadActuationStatusName(
		LeadActuationStatus::NOT_REQUESTED) != nullptr &&
		LeadActuationStatusName(LeadActuationStatus::REQUESTED) !=
		LeadActuationStatusName(LeadActuationStatus::CONFIRMED) &&
		LeadActuationStatusName(LeadActuationStatus::FAILED) !=
		LeadActuationStatusName(LeadActuationStatus::INVALIDATED),
		"LAA-U1 status names are distinct");
	Require(LeadActuationRejectReasonName(
		LeadActuationRejectReason::NONE) != nullptr &&
		LeadActuationRejectReasonName(
		LeadActuationRejectReason::SELF_PROMOTION) !=
		LeadActuationRejectReasonName(
		LeadActuationRejectReason::BAD_SQUAD),
		"LAA-U1 reject reason names are distinct");
	std::cout << "LAA-U1 PASS\n";
}

void TestIdentityValidity()
{
	LeadActuationIdentity both;
	Require(!both.IsValid(), "LAA-U2 default identity is invalid");
	LeadActuationIdentity userOnly;
	userOnly.userId = 2;
	Require(!userOnly.IsValid(),
		"LAA-U2 userid without entity index is invalid");
	LeadActuationIdentity indexOnly;
	indexOnly.entIndex = 2;
	Require(!indexOnly.IsValid(),
		"LAA-U2 entity index without userid is invalid");
	LeadActuationIdentity paired;
	paired.userId = 3;
	paired.entIndex = 2;
	Require(paired.IsValid(), "LAA-U2 userid+index pair is valid");
	std::cout << "LAA-U2 PASS\n";
}

void TestValidateHappyPath()
{
	LeadActuationRejectReason reason =
		LeadActuationRejectReason::BAD_ISSUER_IDENTITY;
	Require(ValidateLeadActuationParties(
		Party(2, 1, 2, 1, true, 0),
		Party(3, 2, 2, 1, true, 1), reason) &&
		reason == LeadActuationRejectReason::NONE,
		"LAA-U3 leader-to-member same-squad request validates");
	std::cout << "LAA-U3 PASS\n";
}

void TestValidateRejections()
{
	LeadActuationRejectReason reason =
		LeadActuationRejectReason::NONE;
	// Issuer == target by userid.
	Require(!ValidateLeadActuationParties(
		Party(2, 1, 2, 1, true, 0),
		Party(2, 2, 2, 1, true, 1), reason) &&
		reason == LeadActuationRejectReason::SELF_PROMOTION,
		"LAA-U4 same userid is self-promotion");
	// Issuer == target by entity index.
	Require(!ValidateLeadActuationParties(
		Party(2, 1, 2, 1, true, 0),
		Party(3, 1, 2, 1, true, 1), reason) &&
		reason == LeadActuationRejectReason::SELF_PROMOTION,
		"LAA-U4 same entity index is self-promotion");
	// Invalid squad.
	Require(!ValidateLeadActuationParties(
		Party(2, 1, 2, 0, true, 0),
		Party(3, 2, 2, 0, true, 1), reason) &&
		reason == LeadActuationRejectReason::BAD_SQUAD,
		"LAA-U4 zero squad is rejected");
	// Different squads.
	Require(!ValidateLeadActuationParties(
		Party(2, 1, 2, 1, true, 0),
		Party(3, 2, 2, 2, true, 1), reason) &&
		reason == LeadActuationRejectReason::SQUAD_MISMATCH,
		"LAA-U4 different squads are rejected");
	// Different teams.
	Require(!ValidateLeadActuationParties(
		Party(2, 1, 2, 1, true, 0),
		Party(3, 2, 3, 1, true, 1), reason) &&
		reason == LeadActuationRejectReason::TEAM_MISMATCH,
		"LAA-U4 different teams are rejected");
	// Non-play team.
	Require(!ValidateLeadActuationParties(
		Party(2, 1, 1, 1, true, 0),
		Party(3, 2, 1, 1, true, 1), reason) &&
		reason == LeadActuationRejectReason::BAD_TEAM,
		"LAA-U4 non-play team is rejected");
	// Issuer not at position 0.
	Require(!ValidateLeadActuationParties(
		Party(2, 1, 2, 1, true, 2),
		Party(3, 2, 2, 1, true, 1), reason) &&
		reason ==
		LeadActuationRejectReason::ISSUER_NOT_LEADER_POSITION,
		"LAA-U4 non-zero issuer position is rejected");
	// Issuer position unknown.
	Require(!ValidateLeadActuationParties(
		Party(2, 1, 2, 1, false, -1),
		Party(3, 2, 2, 1, true, 1), reason) &&
		reason ==
		LeadActuationRejectReason::ISSUER_NOT_LEADER_POSITION,
		"LAA-U4 unknown issuer position is rejected");
	// Bad target entity index (userid/index distinction).
	Require(!ValidateLeadActuationParties(
		Party(2, 1, 2, 1, true, 0),
		Party(3, -1, 2, 1, true, 1), reason) &&
		reason == LeadActuationRejectReason::BAD_TARGET_IDENTITY,
		"LAA-U4 missing target entity index is rejected");
	std::cout << "LAA-U4 PASS\n";
}

void TestStaleness()
{
	const LeadActuationRequest request = Request();
	Require(!IsLeadActuationRequestStale(request, request.issuer,
		request.target, 0),
		"LAA-U5 unchanged context is not stale");
	Require(IsLeadActuationRequestStale(request,
		Party(2, 1, 2, 1, true, 0, true, 5, false),
		request.target, 0),
		"LAA-U5 issuer disconnect is stale");
	Require(IsLeadActuationRequestStale(request, request.issuer,
		Party(3, 2, 2, 1, true, 1, true, 5, false), 0),
		"LAA-U5 target disconnect is stale");
	Require(IsLeadActuationRequestStale(request,
		Party(2, 1, 3, 1, true, 0), request.target, 0),
		"LAA-U5 issuer team change is stale");
	Require(IsLeadActuationRequestStale(request, request.issuer,
		Party(3, 2, 2, 0, false, -1), 0),
		"LAA-U5 target squad leave is stale");
	LeadActuationRequest tracked = Request();
	tracked.contextGeneration = 7;
	Require(!IsLeadActuationRequestStale(tracked, tracked.issuer,
		tracked.target, 7),
		"LAA-U5 matching generation is not stale");
	Require(IsLeadActuationRequestStale(tracked, tracked.issuer,
		tracked.target, 8),
		"LAA-U5 superseding generation is stale");
	std::cout << "LAA-U5 PASS\n";
}

void TestReadback()
{
	const LeadActuationRequest request = Request();
	// Exact native swap: target takes 0, issuer takes target's old slot,
	// squad and XP unchanged.
	Require(InterpretLeadActuationReadback(request,
		Party(2, 1, 2, 1, true, 1, true, 5),
		Party(3, 2, 2, 1, true, 0, true, 5)) ==
		LeadActuationStatus::CONFIRMED,
		"LAA-U6 exact position swap with unchanged XP confirms");
	// Silent no-op: fully known but unmoved positions fail, never pend.
	Require(InterpretLeadActuationReadback(request,
		Party(2, 1, 2, 1, true, 0, true, 5),
		Party(3, 2, 2, 1, true, 1, true, 5)) ==
		LeadActuationStatus::FAILED,
		"LAA-U6 unmoved positions are a failed no-op");
	// Wrong swap target.
	Require(InterpretLeadActuationReadback(request,
		Party(2, 1, 2, 1, true, 2, true, 5),
		Party(3, 2, 2, 1, true, 0, true, 5)) ==
		LeadActuationStatus::FAILED,
		"LAA-U6 issuer in the wrong slot fails");
	// XP moved: contradicts the swap-only model.
	Require(InterpretLeadActuationReadback(request,
		Party(2, 1, 2, 1, true, 1, true, 1),
		Party(3, 2, 2, 1, true, 0, true, 1)) ==
		LeadActuationStatus::FAILED,
		"LAA-U6 changed squad XP fails");
	// Unknown positions stay pending.
	Require(InterpretLeadActuationReadback(request,
		Party(2, 1, 2, 1, false, -1, true, 5),
		Party(3, 2, 2, 1, true, 0, true, 5)) ==
		LeadActuationStatus::REQUESTED,
		"LAA-U6 unknown positions stay pending");
	// Squad leave fails.
	Require(InterpretLeadActuationReadback(request,
		Party(2, 1, 2, 0, false, -1),
		Party(3, 2, 2, 1, true, 0, true, 5)) ==
		LeadActuationStatus::INVALIDATED,
		"LAA-U6 issuer squad leave invalidates");
	std::cout << "LAA-U6 PASS\n";
}

void TestPollSettleWindow()
{
	const LeadActuationRequest request = Request();
	// Fully known but unmoved positions immediately after submission
	// mean "not yet processed": pending, never failed.
	Require(PollLeadActuationRequest(request,
		Party(2, 1, 2, 1, true, 0, true, 5),
		Party(3, 2, 2, 1, true, 1, true, 5), 0.0f) ==
		LeadActuationStatus::REQUESTED,
		"LAA-U7 fresh unmoved readback stays pending");
	Require(PollLeadActuationRequest(request,
		Party(2, 1, 2, 1, true, 0, true, 5),
		Party(3, 2, 2, 1, true, 1, true, 5),
		kLeadActuationSettleSeconds - 0.1f) ==
		LeadActuationStatus::REQUESTED,
		"LAA-U7 unsettled readback stays pending");
	// Only a settled unmoved readback is a silent native no-op.
	Require(PollLeadActuationRequest(request,
		Party(2, 1, 2, 1, true, 0, true, 5),
		Party(3, 2, 2, 1, true, 1, true, 5),
		kLeadActuationSettleSeconds) ==
		LeadActuationStatus::FAILED,
		"LAA-U7 settled unmoved readback fails");
	// A settled swap still confirms regardless of the window.
	Require(PollLeadActuationRequest(request,
		Party(2, 1, 2, 1, true, 1, true, 5),
		Party(3, 2, 2, 1, true, 0, true, 5),
		kLeadActuationSettleSeconds) ==
		LeadActuationStatus::CONFIRMED,
		"LAA-U7 settled swap confirms");
	// Transiently unreadable squad never kills or fails the request.
	LeadActuationParty transientIssuer = request.issuer;
	transientIssuer.squad = -1;
	Require(PollLeadActuationRequest(request, transientIssuer,
		request.target, kLeadActuationSettleSeconds) ==
		LeadActuationStatus::REQUESTED,
		"LAA-U7 unreadable squad stays pending");
	// Disconnect invalidates even inside the settle window.
	LeadActuationParty goneIssuer = request.issuer;
	goneIssuer.connected = false;
	Require(PollLeadActuationRequest(request, goneIssuer,
		request.target, 0) == LeadActuationStatus::INVALIDATED,
		"LAA-U7 disconnect invalidates");
	Require(!IsLeadActuationRequestStale(request, transientIssuer,
		request.target, 0),
		"LAA-U7 unreadable squad is not stale");
	std::cout << "LAA-U7 PASS\n";
}

} // namespace

int main()
{
	TestStatusAndReasonNames();
	TestIdentityValidity();
	TestValidateHappyPath();
	TestValidateRejections();
	TestStaleness();
	TestReadback();
	TestPollSettleWindow();
	std::cout << "LAA-U1..U7 PASS; leader_actuation_behavior_changes=0\n";
	return 0;
}
