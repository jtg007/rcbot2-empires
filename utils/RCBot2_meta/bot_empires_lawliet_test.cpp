// Unit tests for SDK-free Lawliet mode policy (scripts/run_lawliet_tests.sh).
#include "bot_empires_lawliet.h"

#include <cstdio>
#include <cstring>

using namespace EmpiresLawliet;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
	if (!ok)
	{
		std::printf("FAIL %s\n", what);
		++g_failures;
	}
}

DesperationInput BaseLosingInput()
{
	DesperationInput in;
	in.team = 2;
	in.cvarMode = 1;
	in.vehiclesEnabled = true;
	in.enemyCvAlive = true;
	in.hasVfOrApc = true;
	in.onCooldown = false;
	in.matchElapsed = 240.0f;
	in.eligibleScouts = 2;
	in.eligibleTotal = 5;
	in.researchKnown = true;
	in.ownResearchCount = 2;
	in.enemyResearchCount = 6;
	in.ownHasMedTank = false;
	in.enemyHasMedTank = true;
	in.totalFlags = 4;
	in.ownFlags = 1;
	in.enemyFlags = 3;
	return in;
}
} // namespace

int main()
{
	// 1. TriggerReason
	{
		DesperationInput in = BaseLosingInput();
		const char* r = TriggerReason(in);
		Check(r != nullptr && std::strcmp(r, "desperation-research-map") == 0,
			"triggers on research + flag disadvantage");

		DesperationInput early = in;
		early.matchElapsed = 100.0f;
		Check(TriggerReason(early) == nullptr, "blocks before min match seconds");

		DesperationInput off = in;
		off.cvarMode = 0;
		Check(TriggerReason(off) == nullptr, "cvar 0 turns off");

		DesperationInput forced = early;
		forced.cvarMode = 2;
		forced.researchKnown = false;
		forced.totalFlags = 0;
		forced.eligibleTotal = 1;
		forced.eligibleScouts = 1;
		r = TriggerReason(forced);
		Check(r != nullptr && std::strcmp(r, "forced") == 0, "forced mode triggers with 1 scout");

		DesperationInput forcedEarly = forced;
		forcedEarly.matchElapsed = 20.0f;
		Check(TriggerReason(forcedEarly) == nullptr, "forced mode waits for a crew while the team joins");
		forcedEarly.eligibleTotal = 3;
		Check(TriggerReason(forcedEarly) != nullptr, "forced mode starts early with a full crew");

		DesperationInput wrongForce = early;
		wrongForce.cvarMode = 3;
		Check(TriggerReason(wrongForce) == nullptr, "forced 3 does not force team 2");

		DesperationInput noScout = in;
		noScout.eligibleScouts = 0;
		Check(TriggerReason(noScout) == nullptr, "requires at least 1 scout");

		DesperationInput noCv = in;
		noCv.enemyCvAlive = false;
		Check(TriggerReason(noCv) == nullptr, "requires living enemy CV");

		DesperationInput cd = in;
		cd.onCooldown = true;
		Check(TriggerReason(cd) == nullptr, "blocks during cooldown");

		DesperationInput noflag;
		noflag.team = 3;
		noflag.cvarMode = 1;
		noflag.enemyCvAlive = true;
		noflag.hasVfOrApc = true;
		noflag.matchElapsed = 250.0f;
		noflag.eligibleScouts = 1;
		noflag.eligibleTotal = 4;
		noflag.researchKnown = true;
		noflag.ownResearchCount = 1;
		noflag.enemyResearchCount = 4;
		noflag.totalFlags = 0;
		noflag.ownRefineries = 1;
		noflag.enemyRefineries = 4;
		r = TriggerReason(noflag);
		Check(r != nullptr && std::strcmp(r, "desperation-research-map") == 0,
			"flagless map uses refinery deficit");

		DesperationInput tickets = in;
		tickets.researchKnown = false;
		tickets.ticketsKnown = true;
		tickets.ownTickets = 25.0f;
		tickets.ownTicketsMax = 150.0f;
		tickets.enemyTicketsKnown = true;
		tickets.enemyTickets = 110.0f;
		r = TriggerReason(tickets);
		Check(r != nullptr && std::strcmp(r, "desperation-tickets") == 0,
			"tickets + map behind triggers without research");

		DesperationInput unknownRes = in;
		unknownRes.researchKnown = false;
		Check(TriggerReason(unknownRes) == nullptr,
			"unknown research alone without ticket/CV emergency does not trigger");
	}

	// 2. SelectCrew
	{
		const CrewCandidate cands[] = {
			{ 2, 0, false, false, 150.0f }, // scout without stun
			{ 3, 0, true,  false, 300.0f }, // scout with stun
			{ 4, 0, true,  false, 200.0f }, // closer scout with stun
			{ 5, 3, false, false, 400.0f }, // far engineer
			{ 6, 3, false, false, 120.0f }, // closer engineer
			{ 7, 1, false, false, 100.0f }, // rifleman without sticky
			{ 8, 1, true,  false, 250.0f }, // rifleman with sticky
			{ 9, 2, false, false, 180.0f }, // grenadier
		};
		CrewSelection sel;
		Check(SelectCrew(cands, 8, sel), "selects optimal full crew");
		Check(sel.count == 6, "caps crew at 6");
		Check(sel.scoutCount == 2, "picks 2 scouts");
		Check(sel.engCount == 1 && sel.clients[2] == 6, "picks closest engineer");
		Check(sel.clients[0] == 4 && sel.clients[1] == 3, "prefers stun scouts closest first");
		Check(sel.clients[3] == 8, "prefers sticky rifleman first");
		Check(sel.driverClient == 7 || sel.driverClient == 9, "picks extra rifleman/support as driver");

		// Minimum 1 scout
		const CrewCandidate minCands[] = {
			{ 10, 0, true, false, 100.0f },
		};
		CrewSelection minSel;
		Check(SelectCrew(minCands, 1, minSel), "1 scout minimum succeeds");
		Check(minSel.count == 1 && minSel.driverClient == 10, "single scout is driver");

		// Preserve existing APC driver
		const CrewCandidate drvCands[] = {
			{ 11, 0, true, false, 100.0f },
			{ 12, 3, false, true, 900.0f }, // existing APC driver
			{ 13, 1, true, false, 150.0f },
		};
		CrewSelection drvSel;
		Check(SelectCrew(drvCands, 3, drvSel), "selects with existing driver");
		Check(drvSel.driverClient == 12, "preserves existing APC driver");

		// 0 scouts -> fail
		const CrewCandidate noScoutCands[] = {
			{ 14, 1, true, false, 100.0f },
			{ 15, 3, false, false, 120.0f },
		};
		CrewSelection failSel;
		Check(!SelectCrew(noScoutCands, 2, failSel), "0 scouts returns false");
	}

	// 3. EvaluateBoarding
	{
		Check(EvaluateBoarding(false, 1, 2, 4, 10.0f, 0.0f, false) == BOARDING_WAIT,
			"waits while driver unseated");
		Check(EvaluateBoarding(true, 2, 4, 4, 5.0f, 3.0f, false) == BOARDING_GO_FULL,
			"departs immediately when full");
		Check(EvaluateBoarding(true, 1, 3, 4, 19.0f, 10.0f, false) == BOARDING_GO_PARTIAL,
			"departs partial after wait when scout aboard");
		Check(EvaluateBoarding(true, 0, 2, 4, 29.0f, 15.0f, false) == BOARDING_REASSIGN_SCOUT,
			"reassigns scout when no scout aboard after wait");
		Check(EvaluateBoarding(true, 0, 2, 4, 46.0f, 20.0f, true) == BOARDING_ABORT,
			"aborts on boarding timeout without scout");
		Check(EvaluateBoarding(true, 0, 1, 4, 60.0f, 0.0f, true, 5.0f) == BOARDING_WAIT,
			"boarding keeps waiting past the timeout while crew still closes in");
		Check(EvaluateBoarding(true, 0, 1, 4, 60.0f, 0.0f, true, 16.0f) == BOARDING_ABORT,
			"boarding aborts after the timeout once crew progress stalls");
		Check(EvaluateBoarding(true, 0, 1, 4, 151.0f, 0.0f, true, 1.0f) == BOARDING_ABORT,
			"boarding aborts at the hard cap even with progress");
	}

	// 4. ShouldThrowStun
	{
		Check(ShouldThrowStun(false, 0.0f, 10.0f), "throws when CV not stunned");
		Check(!ShouldThrowStun(false, 0.0f, 2.0f), "respects 4 s team spacing");
		Check(!ShouldThrowStun(true, 9.0f, 5.0f), "holds while > 6.5 s stun remains");
		Check(ShouldThrowStun(true, 6.0f, 5.0f), "chain-throws when <= 6.5 s stun remains");
	}

	if (g_failures == 0)
		std::printf("lawliet tests PASS\n");
	return g_failures == 0 ? 0 : 1;
}
