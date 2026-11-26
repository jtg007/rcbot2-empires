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

#include <cmath>

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
// leanDefense 2: no standing defender at all in the calm PUSH phase (11v11 20261126-114928: 1 defense +
// hold at the base refinery parked bots at home with no enemy known); STABILIZE and alerts as before.
inline RoleSplit SplitRoles(int eligible, int teamBots, int phase, bool haveHold, int leanDefense = 0)
{
	RoleSplit split;
	if (eligible <= 0)
		return split;
	int defense = leanDefense ? eligible / 5 : (eligible * 2 + 5) / 10; // floor / round(0.2 n)
	if (teamBots >= 8 && defense < 1)
		defense = 1;
	if (teamBots >= 12 && defense < 2)
		defense = 2;
	if (leanDefense >= 2 && phase == PHASE_PUSH)
		defense = 0;
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
	// Squad flanks (rcbot_empires_squad_flanks): side of each goal and of each bot's squad (FLANK_*).
	bool flanks = false;
	int pushSide[kMaxGoals] = {};
	int holdSide[kMaxGoals] = {};
	int flank[kMaxClients] = {};
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

namespace EmpiresTeamDirector
{
// Hot zones (TODO 8.1, short-term memory against the meat grinder): kHotDeathCount deaths of one team within
// kHotDeathWindow seconds and kHotDeathRadius of each other make that spot hot for kHotZoneSeconds; another
// death inside keeps it hot. Routes pay for every unit inside a hot zone (RCBotNavMesh::SetTeamHotZones), so
// the next wave takes a detour or the shortest way through. Only real kills count (no kill-spawn / void
// suicide, no deaths in vehicles). Bot-vs-bot matches have almost no such clusters (15 crossroads matches
// 23./24.11.: 1x), the target are human or turret positions.
constexpr float kHotDeathWindow = 30.0f;
constexpr float kHotDeathRadius = 700.0f;
constexpr int kHotDeathCount = 3;
constexpr float kHotZoneSeconds = 90.0f;
constexpr float kHotZoneMinRadius = 500.0f;
constexpr float kHotZoneMaxRadius = 1200.0f;
constexpr float kHotZoneMargin = 350.0f;   // around the farthest death of the cluster
constexpr int kMaxDeaths = 16;
constexpr int kMaxHotZones = 4;

enum HotResult
{
	HOT_NONE = 0,
	HOT_NEW,
	HOT_EXTEND
};

struct HotZone
{
	float center[3] = { 0.0f, 0.0f, 0.0f };
	float radius = 0.0f;
	float until = -1.0f;
	int deaths = 0;
	bool active(float now) const { return radius > 0.0f && now < until; }
};

struct DeathMemory
{
	float pos[kMaxDeaths][3] = {};
	float time[kMaxDeaths] = {};
	int next = 0;
	HotZone zone[kMaxHotZones];

	DeathMemory()
	{
		for (float& t : time)
			t = -1.0e9f;
	}
};

inline float HotDist2D(const float a[3], const float b[3])
{
	const float dx = a[0] - b[0];
	const float dy = a[1] - b[1];
	return std::sqrt(dx * dx + dy * dy);
}

// Records one death; zoneIndex gets the zone it made or extended.
inline HotResult RecordDeath(DeathMemory& m, const float at[3], float now, int& zoneIndex)
{
	zoneIndex = -1;
	for (int z = 0; z < kMaxHotZones; ++z)
		if (m.zone[z].active(now) && HotDist2D(m.zone[z].center, at) <= m.zone[z].radius)
		{
			m.zone[z].until = now + kHotZoneSeconds;
			++m.zone[z].deaths;
			zoneIndex = z;
			return HOT_EXTEND;
		}
	const int slot = m.next;
	m.next = (m.next + 1) % kMaxDeaths;
	for (int k = 0; k < 3; ++k)
		m.pos[slot][k] = at[k];
	m.time[slot] = now;

	int members[kMaxDeaths];
	int count = 0;
	for (int i = 0; i < kMaxDeaths; ++i)
		if (now - m.time[i] <= kHotDeathWindow && HotDist2D(m.pos[i], at) <= kHotDeathRadius)
			members[count++] = i;
	if (count < kHotDeathCount)
		return HOT_NONE;

	HotZone zone;
	for (int i = 0; i < count; ++i)
		for (int k = 0; k < 3; ++k)
			zone.center[k] += m.pos[members[i]][k] / static_cast<float>(count);
	float spread = 0.0f;
	for (int i = 0; i < count; ++i)
	{
		const float d = HotDist2D(zone.center, m.pos[members[i]]);
		if (d > spread)
			spread = d;
		m.time[members[i]] = -1.0e9f; // used up: the next cluster needs new deaths
	}
	zone.radius = spread + kHotZoneMargin;
	if (zone.radius < kHotZoneMinRadius)
		zone.radius = kHotZoneMinRadius;
	if (zone.radius > kHotZoneMaxRadius)
		zone.radius = kHotZoneMaxRadius;
	zone.until = now + kHotZoneSeconds;
	zone.deaths = count;
	// Free slot, else the one that runs out first.
	int target = 0;
	for (int z = 0; z < kMaxHotZones; ++z)
	{
		if (!m.zone[z].active(now))
		{
			target = z;
			break;
		}
		if (m.zone[z].until < m.zone[target].until)
			target = z;
	}
	m.zone[target] = zone;
	zoneIndex = target;
	return HOT_NEW;
}

// Self-preservation (TODO 8.2): kShakenDeaths teammates killed by the enemy within kShakenWindow seconds and
// kShakenRadius of a bot make it take cover or fall back for a few seconds instead of walking on.
// Every team member counts (humans too), also crews of vehicles: a tank blown up next to you is a warning.
constexpr float kShakenWindow = 8.0f;
constexpr float kShakenRadius = 600.0f;
constexpr int kShakenDeaths = 2;
constexpr int kMaxNearDeaths = 8;

struct NearDeathRing
{
	float pos[kMaxNearDeaths][3] = {};
	float threat[kMaxNearDeaths][3] = {};   // killer position, or the death spot when unknown
	float time[kMaxNearDeaths] = {};
	int next = 0;

	NearDeathRing()
	{
		for (float& t : time)
			t = -1.0e9f;
	}
};

inline void RecordNearDeath(NearDeathRing& r, const float at[3], const float threat[3], float now)
{
	const int slot = r.next;
	r.next = (r.next + 1) % kMaxNearDeaths;
	for (int k = 0; k < 3; ++k)
	{
		r.pos[slot][k] = at[k];
		r.threat[slot][k] = threat[k];
	}
	r.time[slot] = now;
}

// Deaths within kShakenRadius of me in the last kShakenWindow seconds that happened after `after` (the last
// reaction, so one pair does not trigger twice). threatOut gets the mean threat position of those deaths.
inline int CountNearDeaths(const NearDeathRing& r, const float me[3], float now, float after, float& newest,
	float threatOut[3])
{
	int count = 0;
	newest = after;
	threatOut[0] = threatOut[1] = threatOut[2] = 0.0f;
	for (int i = 0; i < kMaxNearDeaths; ++i)
	{
		if (r.time[i] <= after || now - r.time[i] > kShakenWindow || r.time[i] > now ||
			HotDist2D(r.pos[i], me) > kShakenRadius)
			continue;
		++count;
		if (r.time[i] > newest)
			newest = r.time[i];
		for (int k = 0; k < 3; ++k)
			threatOut[k] += r.threat[i][k];
	}
	if (count > 0)
		for (int k = 0; k < 3; ++k)
			threatOut[k] /= static_cast<float>(count);
	return count;
}

// Squad flanks (rcbot_empires_squad_flanks): seen from the own base towards the enemy base, odd squads
// (Alpha 1, Charlie 3) take the push/hold goal left of that axis, even squads (Bravo 2, Delta 4) the one
// right of it. Bots without a squad or in a human-led squad are spread as before. Only goals at least
// kFlankMinOffset off the axis count as a side, so a linear map (emp_rainbowroad) keeps its rank order;
// small teams (fewer than kFlankMinFree free infantry bots) stay together.
enum FlankSide
{
	FLANK_NONE = 0,
	FLANK_A,  // left of the base axis
	FLANK_B   // right of the base axis
};
constexpr float kFlankMinOffset = 1000.0f;
constexpr int kFlankMinFree = 3;

inline const char* FlankName(int side)
{
	return side == FLANK_A ? "A" : (side == FLANK_B ? "B" : "-");
}

// Side of p relative to the 2D axis home -> enemy; FLANK_NONE within minOffset of the axis.
inline int FlankSideOf(const float home[3], const float enemy[3], const float p[3], float minOffset = kFlankMinOffset)
{
	const float ax = enemy[0] - home[0];
	const float ay = enemy[1] - home[1];
	const float length = std::sqrt(ax * ax + ay * ay);
	if (length < 1.0f)
		return FLANK_NONE;
	const float lateral = (ax * (p[1] - home[1]) - ay * (p[0] - home[0])) / length;
	if (lateral >= minOffset)
		return FLANK_A;
	if (lateral <= -minOffset)
		return FLANK_B;
	return FLANK_NONE;
}

inline int SquadFlank(int squad, bool humanLed)
{
	if (squad <= 0 || humanLed)
		return FLANK_NONE;
	return (squad % 2) != 0 ? FLANK_A : FLANK_B;
}

inline bool FlanksActive(bool enabled, int freeInfantry)
{
	return enabled && freeInfantry >= kFlankMinFree;
}

// From count candidates in rank order (side[] per candidate) pick up to kMaxGoals: with flanks the best
// A and the best B candidate (kept in rank order) when both exist, else the first kMaxGoals as before.
// pick[] gets candidate indices; returns how many.
inline int PickFlankGoals(const int* side, int count, bool flanks, int* pick)
{
	if (flanks)
	{
		int a = -1, b = -1;
		for (int i = 0; i < count; ++i)
		{
			if (side[i] == FLANK_A && a < 0)
				a = i;
			else if (side[i] == FLANK_B && b < 0)
				b = i;
		}
		if (a >= 0 && b >= 0)
		{
			pick[0] = a < b ? a : b;
			pick[1] = a < b ? b : a;
			return 2;
		}
	}
	int n = 0;
	for (int i = 0; i < count && n < kMaxGoals; ++i)
		pick[n++] = i;
	return n;
}

// Goal index on the bot's squad side, -1 when the bot has no side or no goal lies there (caller spreads
// it like a bot without a squad).
inline int FlankGoal(const int* goalSide, int goalCount, int botSide)
{
	if (botSide == FLANK_NONE)
		return -1;
	for (int g = 0; g < goalCount; ++g)
		if (goalSide[g] == botSide)
			return g;
	return -1;
}
} // namespace EmpiresTeamDirector

// Runtime (bot_empires_team_director.cpp). Board of team 2/3 or nullptr before the first update.
const EmpiresTeamDirector::Board* EmpiresTeamDirectorBoard(int team);
void EmpiresTeamDirectorLevelReset();
void EmpiresMatchFlowLevelReset(); // bot_empires_match_flow.cpp
// TODO 8.1: a team member on foot was killed at pos (rcbot_empires_hot_zones).
void EmpiresTeamDirectorRecordDeath(int team, float x, float y, float z, int userId, const char* how);
// Test only (rcbotd empires_test_kill_zone): a living bot of the armed team inside the zone is killed once
// (Hit marks it, Take reads the mark back in died()).
struct edict_t;
class Vector;
bool EmpiresTestKillZoneHit(edict_t* bot, int team, const Vector& at);
bool EmpiresTestKillZoneTake(int client);
void EmpiresTestKillZoneArm(float x, float y, float z, float radius, int team);

#endif
