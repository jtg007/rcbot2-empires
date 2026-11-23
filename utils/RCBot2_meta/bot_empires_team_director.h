// Team director (TODO 5.4 / 5.8 / 5.28): one small picture of the situation per team ("blackboard"),
// rebuilt every ~1.5 s from that team's own buildings, CV and capture points. Other modules only read it:
// the seated bot commander orders the roles (bot_empires_commander_orders.cpp), the spawn choice prefers
// the base on an alert (resolveBootstrapSpawnId) and free defenders walk to the threatened building
// (pickEmpiresNavmeshGoal). Kill-spawn re-deploys a defender or a far vehicle buyer by `kill`.
//
// This header is SDK-free (unit test: scripts/run_team_director_tests.sh); the runtime is in
// bot_empires_team_director.cpp.
#ifndef BOT_EMPIRES_TEAM_DIRECTOR_H
#define BOT_EMPIRES_TEAM_DIRECTOR_H

namespace EmpiresTeamDirector
{
constexpr int kMaxClients = 65; // client indices 1..64
constexpr int kMaxGoals = 2;

enum Phase
{
	PHASE_PUSH = 0,     // normal expansion / push
	PHASE_BASE_ALERT,   // own CV, base building or refinery hit, or enemies / enemy buildings next to it
	PHASE_STABILIZE     // a core building was just lost, CV badly hurt or tickets low
};

enum Role
{
	ROLE_NONE = 0,  // not a free infantry bot (commander, engineer, EngiBot, driver, foreign order)
	ROLE_PUSH,
	ROLE_HOLD,
	ROLE_DEFENSE
};

inline const char* PhaseName(int phase)
{
	switch (phase)
	{
	case PHASE_BASE_ALERT: return "BASE_ALERT";
	case PHASE_STABILIZE: return "STABILIZE";
	default: return "PUSH";
	}
}

inline const char* RoleName(int role)
{
	switch (role)
	{
	case ROLE_PUSH: return "push";
	case ROLE_HOLD: return "hold";
	case ROLE_DEFENSE: return "defense";
	default: return "none";
	}
}

struct RoleSplit
{
	int push = 0;
	int hold = 0;
	int defense = 0;
};

// ~50 % push, ~30 % hold, ~20 % defense of the free infantry; at least 1 defender from 8 team bots and
// 2 from 12. An alert puts about a third on defense, STABILIZE shifts push to hold. Without a hold goal
// the hold share pushes. leanDefense: the standing defender share is floor(0.2 n), so 3-4 free bots all go
// forward without an alert (crossroads 165502: 3 free bots per team as push/hold/defense, the defenders
// gave half of all 30 s defend-holds, 27 of 52, idle at home).
inline RoleSplit SplitRoles(int eligible, int teamBots, int phase, bool haveHold, bool leanDefense = false)
{
	RoleSplit split;
	if (eligible <= 0)
		return split;
	int defense = leanDefense ? eligible / 5 : (eligible * 2 + 5) / 10; // floor / round(0.2 n)
	if (teamBots >= 8 && defense < 1)
		defense = 1;
	if (teamBots >= 12 && defense < 2)
		defense = 2;
	if (phase == PHASE_STABILIZE && eligible >= 3 && defense < 1)
		defense = 1;
	if (defense > eligible - 1)
		defense = eligible - 1; // somebody always pushes
	// Alert override: the threatened building always gets an answer, in small teams (5 bots, one free
	// infantryman, palmbay 160030: alert with roles 1/0/0) the only free bot defends; 2 from 4 free bots,
	// about a third in bigger teams.
	if (phase == PHASE_BASE_ALERT)
	{
		int want = eligible >= 4 ? 2 : 1;
		if (want < (eligible + 2) / 3)
			want = (eligible + 2) / 3;
		if (want > eligible)
			want = eligible;
		if (defense < want)
			defense = want;
	}
	if (defense < 0)
		defense = 0;
	const int rest = eligible - defense;
	// hold = round(rest * 3/8) (30 % of the whole is 3/8 of the 80 % left), STABILIZE: half.
	int hold = phase == PHASE_STABILIZE ? (rest + 1) / 2 : (rest * 3 + 4) / 8;
	if (hold > rest - 1)
		hold = rest - 1;
	if (hold < 0 || !haveHold)
		hold = 0;
	split.defense = defense;
	split.hold = hold;
	split.push = rest - hold;
	return split;
}

// Alert target switch: a new alert, the same building, damage, a stronger threat or an alert building
// without own trigger for kAlertSwitchSeconds takes the target over; a weaker threat elsewhere only keeps
// the alert alive (with the 40 s alert hold an infantry alert at a forward refinery kept defenders there
// while an enemy vehicle stood at the CV).
constexpr float kAlertSwitchSeconds = 10.0f;
inline bool AlertTakesOver(bool alertActive, bool sameEntity, bool damage, int score, int alertScore,
	float alertQuietSeconds)
{
	// Scores are tier*10 + building weight: only a real step (one more enemy, a worse threat) switches,
	// not the weight alone (palmbay 174745: barracks 414 -> cv 415 at the same base).
	return !alertActive || sameEntity || damage || score >= alertScore + 10 || alertQuietSeconds > kAlertSwitchSeconds;
}

// Position of a point between the own base (0) and the enemy base (1), from its 2D distances to both.
inline float FrontShare(float distHome, float distEnemy)
{
	const float sum = distHome + distEnemy;
	return sum > 0.0f ? distHome / sum : 0.0f;
}

// Free resource point as a push goal without flags: only from just before the middle on (not on the
// own half; palmbay 160030: NF pushed resource 84/86, BE 95/46/92, each on its own half, 1 kill in the
// match against 38 without orders), the middle first. Lower is better, < 0 = not a push goal.
inline float ResourcePushScore(float distHome, float distEnemy)
{
	const float share = FrontShare(distHome, distEnemy);
	if (share < 0.4f)
		return -1.0f;
	return share > 0.55f ? share - 0.55f : 0.55f - share;
}

// A bot keeps a new role at least this long (palmbay 163951/164001: up to 8 push<->defense swaps per bot,
// median ~12 s, every front-refinery alert pulled the only pusher back).
constexpr float kRoleMinSeconds = 40.0f;

// Stable assignment: a locked bot (role younger than kRoleMinSeconds) keeps it even over the split; then
// a bot keeps its previous role while that role still has room; the rest fill the roles in the order
// defense, hold, push. If the split wants defenders and all bots are locked elsewhere, one bot of the
// biggest other role breaks its lock (an alert always gets an answer). prev/out hold count entries
// (ROLE_* values), locked may be nullptr.
inline void AssignRoles(const int* prev, const bool* locked, int count, const RoleSplit& split, int* out)
{
	int room[4] = { 0, split.push, split.hold, split.defense };
	for (int i = 0; i < count; ++i)
	{
		out[i] = ROLE_NONE;
		const int role = prev[i];
		if (locked != nullptr && locked[i] && role >= ROLE_PUSH && role <= ROLE_DEFENSE)
		{
			out[i] = role;
			--room[role];
		}
	}
	for (int i = 0; i < count; ++i)
	{
		if (out[i] != ROLE_NONE)
			continue;
		const int role = prev[i];
		if (role >= ROLE_PUSH && role <= ROLE_DEFENSE && room[role] > 0)
		{
			out[i] = role;
			--room[role];
		}
	}
	const int order[3] = { ROLE_DEFENSE, ROLE_HOLD, ROLE_PUSH };
	for (int i = 0; i < count; ++i)
	{
		if (out[i] != ROLE_NONE)
			continue;
		for (const int role : order)
			if (room[role] > 0)
			{
				out[i] = role;
				--room[role];
				break;
			}
		if (out[i] == ROLE_NONE)
			out[i] = prev[i] >= ROLE_PUSH && prev[i] <= ROLE_DEFENSE ? prev[i] : ROLE_PUSH;
	}
	if (split.defense <= 0 || count <= 0)
		return;
	int have[4] = {};
	for (int i = 0; i < count; ++i)
		++have[out[i]];
	if (have[ROLE_DEFENSE] > 0)
		return;
	const int from = have[ROLE_HOLD] >= have[ROLE_PUSH] ? ROLE_HOLD : ROLE_PUSH;
	for (int i = count - 1; i >= 0; --i)
		if (out[i] == from)
		{
			out[i] = ROLE_DEFENSE;
			return;
		}
}

inline void AssignRoles(const int* prev, int count, const RoleSplit& split, int* out)
{
	AssignRoles(prev, nullptr, count, split, out);
}

// Kill-spawn rules (TODO 5.28).
constexpr float kKillSpawnMinSaved = 3800.0f;     // spawn must be this much closer to the goal than the bot
constexpr float kKillSpawnNoCombat = 6.0f;        // no damage taken / enemy seen for this long
constexpr float kKillSpawnBotCooldown = 90.0f;    // per bot
constexpr float kKillSpawnTeamCooldown = 15.0f;   // one per team in this window
constexpr float kKillSpawnMinTickets = 20.0f;
constexpr float kKillSpawnMinTicketShare = 0.25f;

struct KillSpawnInput
{
	bool enabled = true;
	bool alive = false;
	bool onFoot = false;
	bool busy = false;          // build / repair / revive job, commander duty
	bool foreignOrder = false;  // an order of a human (or someone else) stands
	float sinceCombat = 0.0f;
	float savedDistance = 0.0f;
	bool ticketsKnown = false;
	float tickets = 0.0f;
	float ticketsMax = 0.0f;
	float sinceOwnKillSpawn = 1.0e9f;
	float sinceTeamKillSpawn = 1.0e9f;
};

// nullptr = allowed, else the first rule that blocks.
inline const char* KillSpawnBlock(const KillSpawnInput& in)
{
	if (!in.enabled)
		return "cvar-off";
	if (!in.alive)
		return "dead";
	if (!in.onFoot)
		return "not-on-foot";
	if (in.busy)
		return "busy";
	if (in.foreignOrder)
		return "foreign-order";
	if (in.sinceCombat < kKillSpawnNoCombat)
		return "combat";
	if (in.savedDistance < kKillSpawnMinSaved)
		return "too-close";
	if (!in.ticketsKnown)
		return "tickets-unknown";
	if (in.tickets <= kKillSpawnMinTickets ||
		(in.ticketsMax > 0.0f && in.tickets <= in.ticketsMax * kKillSpawnMinTicketShare))
		return "tickets-low";
	if (in.sinceOwnKillSpawn < kKillSpawnBotCooldown)
		return "bot-cooldown";
	if (in.sinceTeamKillSpawn < kKillSpawnTeamCooldown)
		return "team-cooldown";
	return nullptr;
}

struct FlagPushProgress
{
	float bestDistance = -1.0f;
	float lastProgress = 0.0f;
	bool occupied = false;

	bool stalled(float now, float distance, bool ownOccupants)
	{
		if (bestDistance < 0.0f || distance <= bestDistance - 128.0f)
		{
			bestDistance = distance;
			lastProgress = now;
		}
		if (ownOccupants && !occupied)
			lastProgress = now;
		occupied = ownOccupants;
		return now - lastProgress >= 180.0f;
	}
};

struct Stabilization
{
	float until = -1.0f;
	const char* reason = "";

	void extend(float deadline, const char* trigger)
	{
		if (deadline > until)
		{
			until = deadline;
			reason = trigger;
		}
	}
};

struct Goal
{
	int entity = -1;
	float pos[3] = { 0.0f, 0.0f, 0.0f };
	const char* kind = "";
	bool valid() const { return entity >= 0 || kind[0] != '\0'; }
};

struct Board
{
	float updatedAt = -1.0f;
	int phase = PHASE_PUSH;
	Goal push[kMaxGoals];
	int pushCount = 0;
	Goal hold[kMaxGoals];
	int holdCount = 0;
	Goal alert;               // threatened own building / CV (BASE_ALERT)
	const char* alertReason = "";
	Goal base;                // own CV or main base, the defenders' spot without an alert
	bool ticketsKnown = false;
	float tickets = 0.0f;
	float ticketsMax = 0.0f;
	int teamBots = 0;
	RoleSplit split;
	int role[kMaxClients] = {};
	int goal[kMaxClients] = {};   // index into push[] / hold[] for the role
	float lastKillSpawn = -1.0e9f;
};

// Goal the client's role points to (push/hold/alert/base), nullptr without a role or goal.
inline const Goal* RoleGoal(const Board& board, int client)
{
	if (client <= 0 || client >= kMaxClients)
		return nullptr;
	const int g = board.goal[client];
	switch (board.role[client])
	{
	case ROLE_PUSH:
		return g >= 0 && g < board.pushCount ? &board.push[g] : nullptr;
	case ROLE_HOLD:
		return g >= 0 && g < board.holdCount ? &board.hold[g] : nullptr;
	case ROLE_DEFENSE:
		if (board.phase == PHASE_BASE_ALERT && board.alert.valid())
			return &board.alert;
		return board.base.valid() ? &board.base : nullptr;
	default:
		return nullptr;
	}
}
} // namespace EmpiresTeamDirector

// Runtime (bot_empires_team_director.cpp). Board of team 2/3 or nullptr before the first update.
const EmpiresTeamDirector::Board* EmpiresTeamDirectorBoard(int team);
void EmpiresTeamDirectorLevelReset();

#endif
