// Unit test for the SDK-free part of the team director (scripts/run_team_director_tests.sh).
#include "bot_empires_team_director.h"

#include <cstdio>
#include <cstring>

using namespace EmpiresTeamDirector;

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

void CheckSplit(int n, int teamBots, int phase, bool hold, int p, int h, int d, const char* what)
{
	const RoleSplit s = SplitRoles(n, teamBots, phase, hold);
	if (s.push != p || s.hold != h || s.defense != d)
	{
		std::printf("FAIL %s: n=%d got %d/%d/%d want %d/%d/%d\n", what, n, s.push, s.hold, s.defense, p, h, d);
		++g_failures;
	}
	Check(s.push + s.hold + s.defense == n, "split sums to eligible");
}
}

int main()
{
	FlagPushProgress flag;
	Check(!flag.stalled(10, 2000, false), "start flag assignment");
	Check(!flag.stalled(189, 1800, false), "advance renews attack");
	Check(!flag.stalled(368, 1800, true), "entering capture zone renews attack");
	Check(flag.stalled(548, 1790, true), "stationary occupation and jitter still time out");
	Stabilization stabilize;
	stabilize.extend(70, "lost-barracks");
	stabilize.extend(14, "cv-health");
	Check(stabilize.until == 70 && std::strcmp(stabilize.reason, "lost-barracks") == 0, "keep building loss deadline");
	stabilize.extend(72, "tickets");
	Check(stabilize.until == 72 && std::strcmp(stabilize.reason, "tickets") == 0, "ongoing threat extends deadline");

	CheckSplit(0, 0, PHASE_PUSH, true, 0, 0, 0, "empty");
	CheckSplit(1, 5, PHASE_PUSH, true, 1, 0, 0, "single bot pushes");
	CheckSplit(2, 5, PHASE_PUSH, true, 1, 1, 0, "two bots push+hold");
	CheckSplit(3, 5, PHASE_PUSH, true, 1, 1, 1, "three bots all roles");
	CheckSplit(10, 12, PHASE_PUSH, true, 5, 3, 2, "50/30/20");
	CheckSplit(5, 8, PHASE_PUSH, true, 2, 2, 1, "min one defender from 8");
	CheckSplit(4, 12, PHASE_PUSH, true, 1, 1, 2, "min two defenders from 12");
	CheckSplit(2, 5, PHASE_BASE_ALERT, true, 1, 0, 1, "alert takes a defender");
	CheckSplit(6, 8, PHASE_BASE_ALERT, true, 2, 2, 2, "alert third on defense");
	CheckSplit(1, 5, PHASE_BASE_ALERT, true, 0, 0, 1, "alert override: only free bot defends");
	CheckSplit(4, 5, PHASE_BASE_ALERT, true, 1, 1, 2, "alert override: two of four");
	CheckSplit(10, 12, PHASE_BASE_ALERT, true, 4, 2, 4, "alert third in big team");
	Check(ResourcePushScore(2000.0f, 8000.0f) < 0.0f, "own-half resource is no push goal");
	Check(ResourcePushScore(5000.0f, 5000.0f) >= 0.0f && ResourcePushScore(5000.0f, 5000.0f) < ResourcePushScore(8000.0f, 2000.0f),
		"middle beats deep enemy half");
	CheckSplit(5, 5, PHASE_STABILIZE, true, 2, 2, 1, "stabilize holds more");
	CheckSplit(5, 8, PHASE_PUSH, false, 4, 0, 1, "no hold goal pushes");
	{
		struct Lean { int n, teamBots, phase, p, h, d; const char* what; };
		const Lean lean[] = {
			{ 3, 5, PHASE_PUSH, 2, 1, 0, "lean: three bots without a standing defender" },
			{ 4, 5, PHASE_PUSH, 2, 2, 0, "lean: four bots without a standing defender" },
			{ 5, 5, PHASE_PUSH, 2, 2, 1, "lean: one defender from five free bots" },
			{ 3, 8, PHASE_PUSH, 1, 1, 1, "lean: minimum from 8 team bots stays" },
			{ 3, 5, PHASE_BASE_ALERT, 1, 1, 1, "lean: alert still takes a defender" },
			{ 3, 5, PHASE_STABILIZE, 1, 1, 1, "lean: stabilize keeps a defender" },
		};
		for (const Lean& c : lean)
		{
			const RoleSplit s = SplitRoles(c.n, c.teamBots, c.phase, true, true);
			if (s.push != c.p || s.hold != c.h || s.defense != c.d)
			{
				std::printf("FAIL %s: got %d/%d/%d want %d/%d/%d\n", c.what, s.push, s.hold, s.defense, c.p, c.h, c.d);
				++g_failures;
			}
		}
	}

	// Stable assignment: previous roles are kept while they fit.
	{
		const RoleSplit split = SplitRoles(5, 8, PHASE_PUSH, true); // 2/2/1
		const int prev[5] = { ROLE_PUSH, ROLE_HOLD, ROLE_NONE, ROLE_DEFENSE, ROLE_PUSH };
		int out[5];
		AssignRoles(prev, 5, split, out);
		Check(out[0] == ROLE_PUSH && out[1] == ROLE_HOLD && out[3] == ROLE_DEFENSE && out[4] == ROLE_PUSH,
			"previous roles kept");
		Check(out[2] == ROLE_HOLD, "new bot fills the free hold slot");
	}
	{
		// Alert: the second pusher has no room and becomes the defender.
		const RoleSplit split = SplitRoles(3, 5, PHASE_BASE_ALERT, true); // 1/1/1
		const int prev[3] = { ROLE_PUSH, ROLE_HOLD, ROLE_PUSH };
		int out[3];
		AssignRoles(prev, 3, split, out);
		int counts[4] = {};
		for (int r : out)
			++counts[r];
		Check(counts[ROLE_PUSH] == split.push && counts[ROLE_HOLD] == split.hold &&
			counts[ROLE_DEFENSE] == split.defense, "alert counts match split");
		Check(out[0] == ROLE_PUSH && out[1] == ROLE_HOLD && out[2] == ROLE_DEFENSE, "extra pusher becomes the defender");
	}
	{
		// Alert just ended: a defender locked by the minimum role time stays defender over the split.
		RoleSplit split; // calm: 2/1/0
		split.push = 2;
		split.hold = 1;
		const int prev[3] = { ROLE_PUSH, ROLE_HOLD, ROLE_DEFENSE };
		const bool locked[3] = { false, false, true };
		int out[3];
		AssignRoles(prev, locked, 3, split, out);
		Check(out[0] == ROLE_PUSH && out[1] == ROLE_HOLD && out[2] == ROLE_DEFENSE, "locked defender stays");
		const bool unlocked[3] = { false, false, false };
		AssignRoles(prev, unlocked, 3, split, out);
		Check(out[2] == ROLE_PUSH, "unlocked defender returns to push");
	}
	{
		// New alert, everybody locked in push/hold: one bot of the biggest role still answers.
		const RoleSplit split = SplitRoles(3, 5, PHASE_BASE_ALERT, true); // 1/1/1
		const int prev[3] = { ROLE_PUSH, ROLE_HOLD, ROLE_PUSH };
		const bool locked[3] = { true, true, true };
		int out[3];
		AssignRoles(prev, locked, 3, split, out);
		int counts[4] = {};
		for (int r : out)
			++counts[r];
		Check(counts[ROLE_DEFENSE] == 1 && counts[ROLE_PUSH] == 1 && counts[ROLE_HOLD] == 1,
			"locked team still sends one defender from the pushers");
		// Only one pusher and one holder locked: the holder goes, the last pusher stays.
		const int prev2[2] = { ROLE_PUSH, ROLE_HOLD };
		const bool locked2[2] = { true, true };
		int out2[2];
		AssignRoles(prev2, locked2, 2, SplitRoles(2, 5, PHASE_BASE_ALERT, true), out2);
		Check(out2[0] == ROLE_PUSH && out2[1] == ROLE_DEFENSE, "last pusher kept, holder defends");
	}
	{
		// Unlocked bots without room anywhere keep their role instead of falling to push.
		RoleSplit split;
		split.push = 1;
		const int prev[2] = { ROLE_HOLD, ROLE_HOLD };
		const bool locked[2] = { true, false };
		int out[2];
		AssignRoles(prev, locked, 2, split, out);
		Check(out[0] == ROLE_HOLD && out[1] == ROLE_PUSH, "free bot fills the push room");
	}

	// Kill-spawn rules.
	KillSpawnInput ok;
	ok.alive = true;
	ok.onFoot = true;
	ok.sinceCombat = 10.0f;
	ok.savedDistance = 5000.0f;
	ok.ticketsKnown = true;
	ok.tickets = 200.0f;
	ok.ticketsMax = 300.0f;
	Check(KillSpawnBlock(ok) == nullptr, "kill-spawn allowed");
	KillSpawnInput t = ok;
	t.enabled = false;
	Check(std::strcmp(KillSpawnBlock(t), "cvar-off") == 0, "cvar off");
	t = ok; t.sinceCombat = 3.0f;
	Check(std::strcmp(KillSpawnBlock(t), "combat") == 0, "recent combat");
	t = ok; t.savedDistance = 3000.0f;
	Check(std::strcmp(KillSpawnBlock(t), "too-close") == 0, "not enough saved");
	t = ok; t.tickets = 18.0f;
	Check(std::strcmp(KillSpawnBlock(t), "tickets-low") == 0, "tickets absolute");
	t = ok; t.tickets = 70.0f;
	Check(std::strcmp(KillSpawnBlock(t), "tickets-low") == 0, "tickets share");
	t = ok; t.ticketsKnown = false;
	Check(std::strcmp(KillSpawnBlock(t), "tickets-unknown") == 0, "tickets unknown");
	t = ok; t.sinceOwnKillSpawn = 60.0f;
	Check(std::strcmp(KillSpawnBlock(t), "bot-cooldown") == 0, "bot cooldown");
	t = ok; t.sinceTeamKillSpawn = 5.0f;
	Check(std::strcmp(KillSpawnBlock(t), "team-cooldown") == 0, "team cooldown");
	t = ok; t.busy = true;
	Check(std::strcmp(KillSpawnBlock(t), "busy") == 0, "busy");
	t = ok; t.foreignOrder = true;
	Check(std::strcmp(KillSpawnBlock(t), "foreign-order") == 0, "human order");
	t = ok; t.onFoot = false;
	Check(std::strcmp(KillSpawnBlock(t), "not-on-foot") == 0, "in vehicle");

	// Role goals.
	{
		Board b;
		b.pushCount = 2;
		b.push[1].entity = 7;
		b.role[3] = ROLE_PUSH;
		b.goal[3] = 1;
		Check(RoleGoal(b, 3) == &b.push[1], "push goal");
		b.role[4] = ROLE_DEFENSE;
		b.base.entity = 9;
		Check(RoleGoal(b, 4) == &b.base, "defense at base without alert");
		b.phase = PHASE_BASE_ALERT;
		b.alert.entity = 11;
		Check(RoleGoal(b, 4) == &b.alert, "defense on alert");
		b.role[5] = ROLE_HOLD;
		b.goal[5] = 0;
		Check(RoleGoal(b, 5) == nullptr, "hold without hold goal");
	}

	// Alert target switch.
	Check(AlertTakesOver(false, false, false, 410, 0, 0.0f), "new alert");
	Check(!AlertTakesOver(true, false, false, 413, 603, 2.0f), "weaker threat elsewhere keeps target");
	Check(AlertTakesOver(true, false, false, 605, 413, 2.0f), "stronger threat takes over");
	Check(!AlertTakesOver(true, false, false, 415, 414, 2.0f), "building weight alone does not switch");
	Check(AlertTakesOver(true, false, false, 424, 414, 2.0f), "one more enemy switches");
	Check(AlertTakesOver(true, false, true, 1001, 1004, 2.0f), "damage takes over");
	Check(AlertTakesOver(true, false, false, 303, 413, 12.0f), "quiet alert building is replaced");

	if (g_failures == 0)
		std::printf("team director tests PASS\n");
	return g_failures == 0 ? 0 : 1;
}
