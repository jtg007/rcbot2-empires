#include "bot_empires_squad_observation.h"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace RCBotEmpiresSquadObservation;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

SquadFieldObservation Fields(const bool squadKnown, const int squad,
	const bool positionKnown, const int position,
	const bool explicitKnown, const bool explicitLeader)
{
	SquadFieldObservation observation;
	observation.squad = squad;
	observation.squadKnown = squadKnown;
	observation.position = position;
	observation.positionKnown = positionKnown;
	observation.explicitLeader = explicitLeader;
	observation.explicitLeaderKnown = explicitKnown;
	return observation;
}

void TestLeaderEncodingAgreement()
{
	// Live T1 shape: position==0 plus explicit marker agree on leader.
	const LeaderInterpretation leader = InterpretSquadLeader(
		Fields(true, 1, true, 0, true, true));
	Require(leader.isLeader && leader.sourcesAgree &&
		leader.encodingMatch && !leader.ambiguous,
		"SQO-U1 agreeing sources identify the leader");
	const LeaderInterpretation member = InterpretSquadLeader(
		Fields(true, 1, true, 1, true, false));
	Require(!member.isLeader && member.sourcesAgree &&
		member.encodingMatch && !member.ambiguous,
		"SQO-U1 agreeing sources identify a member");
	std::cout << "SQO-U1 PASS\n";
}

void TestLeaderEncodingDisagreement()
{
	// Transient disagreement must surface as ambiguous, never silently pick
	// one source. Explicit marker wins for isLeader while flagged.
	const LeaderInterpretation conflict = InterpretSquadLeader(
		Fields(true, 1, true, 0, true, false));
	Require(!conflict.sourcesAgree && !conflict.encodingMatch &&
		conflict.ambiguous,
		"SQO-U2 disagreeing sources are ambiguous");
	const LeaderInterpretation conflictMember = InterpretSquadLeader(
		Fields(true, 1, true, 2, true, true));
	Require(conflictMember.ambiguous && conflictMember.isLeader,
		"SQO-U2 explicit marker decides under disagreement");
	std::cout << "SQO-U2 PASS\n";
}

void TestLeaderMissingSources()
{
	const LeaderInterpretation positionOnly = InterpretSquadLeader(
		Fields(true, 1, true, 0, false, false));
	Require(positionOnly.isLeader && positionOnly.encodingMatch &&
		!positionOnly.ambiguous,
		"SQO-U3 position-only observation identifies the leader");
	const LeaderInterpretation explicitOnly = InterpretSquadLeader(
		Fields(false, -1, false, -1, true, true));
	Require(explicitOnly.isLeader && explicitOnly.encodingMatch &&
		!explicitOnly.ambiguous,
		"SQO-U3 explicit-only observation identifies the leader");
	const LeaderInterpretation neither = InterpretSquadLeader(
		Fields(false, -1, false, -1, false, false));
	Require(!neither.isLeader && neither.ambiguous,
		"SQO-U3 no sources is ambiguous, not a member");
	std::cout << "SQO-U3 PASS\n";
}

void TestAliveEngineerPredicate()
{
	Require(IsAliveEngineer(true, 3, true),
		"SQO-U4 alive engineer detected");
	Require(!IsAliveEngineer(true, 3, false),
		"SQO-U4 dead engineer excluded");
	Require(!IsAliveEngineer(true, 1, true),
		"SQO-U4 alive non-engineer excluded");
	Require(!IsAliveEngineer(false, 3, true),
		"SQO-U4 unknown class excluded");
	Require(!IsAliveEngineer(false, -1, false),
		"SQO-U4 unknown dead excluded");
	std::cout << "SQO-U4 PASS\n";
}

void TestTeamScopedKey()
{
	Require(TeamScopedSquadKey(2, 1) != TeamScopedSquadKey(3, 1),
		"SQO-U5 same squad id on different teams keys differently");
	Require(TeamScopedSquadKey(2, 1) != TeamScopedSquadKey(2, 2),
		"SQO-U5 different squads key differently");
	Require(TeamScopedSquadKey(2, 1) == TeamScopedSquadKey(2, 1),
		"SQO-U5 identical scope keys identically");
	std::cout << "SQO-U5 PASS\n";
}

void TestXpSharingClassification()
{
	Require(ClassifySquadXpSharing(true, 40, true, 40) ==
		SquadXpSharing::SAME_VALUE,
		"SQO-U6 equal values classify as mirrored");
	Require(ClassifySquadXpSharing(true, 40, true, 25) ==
		SquadXpSharing::DIFFERENT_VALUES,
		"SQO-U6 differing values classify as per-member");
	Require(ClassifySquadXpSharing(false, 0, true, 40) ==
		SquadXpSharing::UNKNOWN,
		"SQO-U6 missing value classifies as unknown");
	Require(ClassifySquadXpSharing(false, 0, false, 0) ==
		SquadXpSharing::UNKNOWN,
		"SQO-U6 missing values never classify as mirrored");
	std::cout << "SQO-U6 PASS\n";
}

void TestStatusNames()
{
	Require(std::string(SquadFieldStatusName(SquadFieldStatus::RESOLVED)) ==
		"RESOLVED", "SQO-U7 resolved name");
	Require(std::string(SquadFieldStatusName(SquadFieldStatus::NOT_FOUND)) ==
		"NOT_FOUND", "SQO-U7 not-found name");
	Require(std::string(SquadFieldStatusName(SquadFieldStatus::AMBIGUOUS)) ==
		"AMBIGUOUS", "SQO-U7 ambiguous name");
	Require(std::string(SquadFieldStatusName(SquadFieldStatus::UNSUPPORTED)) ==
		"UNSUPPORTED", "SQO-U7 unsupported name");
	Require(std::string(SquadXpSharingName(SquadXpSharing::SAME_VALUE)) ==
		"SAME_VALUE", "SQO-U7 sharing name");
	std::cout << "SQO-U7 PASS\n";
}

} // namespace

int main()
{
	TestLeaderEncodingAgreement();
	TestLeaderEncodingDisagreement();
	TestLeaderMissingSources();
	TestAliveEngineerPredicate();
	TestTeamScopedKey();
	TestXpSharingClassification();
	TestStatusNames();
	std::cout << "SQO-U1..U7 PASS\n";
	return 0;
}
