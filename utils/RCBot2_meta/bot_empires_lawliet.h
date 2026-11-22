// Lawliet mode (TODO 5.14b): desperation APC rush on the enemy Command Vehicle.
// When a team is falling behind in research and map control/tickets (or forced via
// rcbot_empires_lawliet 2|3), a crewed APC (optimal: 2 Scouts + 1 Rifleman + 1 Engineer,
// min 1 Scout) drives straight to the enemy CV, disembarks, Scouts stun the CV, the
// Engineer drops an ammo crate (emp_eng_crate) next to it, and the crew resupplies
// stun/sticky grenades to finish the CV.
//
// Pure policy/selection logic lives here SDK-free (unit test: scripts/run_lawliet_tests.sh);
// runtime integration lives in bot_empires_lawliet.cpp.
#ifndef BOT_EMPIRES_LAWLIET_H
#define BOT_EMPIRES_LAWLIET_H

class Vector;

namespace EmpiresLawliet
{
constexpr int kMaxClients = 65; // client indices 1..64
constexpr int kMaxCrew = 6;

constexpr float kMinMatchSeconds = 180.0f;
constexpr int kForcedFullCrew = 3;
constexpr float kForcedCrewWait = 90.0f;
constexpr float kCooldownSeconds = 120.0f;
constexpr float kAcquireTimeout = 120.0f;
constexpr float kBoardingTimeout = 45.0f;
constexpr float kBoardingPartialWait = 18.0f;
constexpr float kBoardingReassignWait = 28.0f;
// Boarding waits past kBoardingTimeout while a missing crew member still closes in on the APC
// (001921: crew 6000-9000 HU away on foot, 3x boarding-timeout), up to the hard cap.
constexpr float kBoardingStallSeconds = 15.0f;
// Crew farther than this from the rally point (VF / APC) respawns there at once instead of walking
// (user 22.11.: assemble at base, then go).
constexpr float kCrewWalkRadius = 1500.0f;
constexpr float kBoardingHardCap = 150.0f;
constexpr float kBoardingProgressStep = 150.0f;
constexpr float kRushTimeout = 180.0f;
constexpr float kRushStallSeconds = 35.0f;
constexpr float kAssaultTimeout = 120.0f;

constexpr float kDisembarkRadius = 480.0f;
constexpr float kEarlyDisembarkRadius = 950.0f;
constexpr float kPoppedApcFootRadius = 1600.0f;

constexpr float kStunSpacingSeconds = 4.0f;
constexpr float kStunFuseFallbackSeconds = 6.5f;
constexpr float kStunReapplyWindow = 6.5f;

enum Phase
{
	PHASE_IDLE = 0,
	PHASE_ACQUIRE_APC,
	PHASE_BOARDING,
	PHASE_RUSHING,
	PHASE_ASSAULT
};

enum CrewRole
{
	ROLE_NONE = 0,
	ROLE_SCOUT,
	ROLE_RIFLEMAN,
	ROLE_ENGINEER,
	ROLE_SUPPORT
};

enum BoardingAction
{
	BOARDING_WAIT = 0,
	BOARDING_GO_FULL,
	BOARDING_GO_PARTIAL,
	BOARDING_REASSIGN_SCOUT,
	BOARDING_ABORT
};

inline const char* PhaseName(int phase)
{
	switch (phase)
	{
	case PHASE_ACQUIRE_APC: return "ACQUIRE_APC";
	case PHASE_BOARDING: return "BOARDING";
	case PHASE_RUSHING: return "RUSHING";
	case PHASE_ASSAULT: return "ASSAULT";
	default: return "IDLE";
	}
}

inline const char* RoleName(int role)
{
	switch (role)
	{
	case ROLE_SCOUT: return "scout";
	case ROLE_RIFLEMAN: return "rifleman";
	case ROLE_ENGINEER: return "engineer";
	case ROLE_SUPPORT: return "support";
	default: return "none";
	}
}

inline const char* BoardingActionName(int action)
{
	switch (action)
	{
	case BOARDING_GO_FULL: return "go-full";
	case BOARDING_GO_PARTIAL: return "go-partial";
	case BOARDING_REASSIGN_SCOUT: return "reassign-scout";
	case BOARDING_ABORT: return "abort";
	default: return "wait";
	}
}

struct DesperationInput
{
	int team = 0;
	int cvarMode = 1; // 0=off, 1=auto, 2=force NF, 3=force BE
	bool vehiclesEnabled = true;
	bool enemyCvAlive = false;
	bool hasVfOrApc = false;
	bool onCooldown = false;
	float matchElapsed = 0.0f;
	int eligibleScouts = 0;
	int eligibleTotal = 0;

	bool researchKnown = false;
	int ownResearchCount = 0;
	int enemyResearchCount = 0;
	bool ownHasMedTank = false;
	bool enemyHasMedTank = false;
	bool ownHasHeavyTank = false;
	bool enemyHasHeavyTank = false;

	int totalFlags = 0;
	int ownFlags = 0;
	int enemyFlags = 0;

	int ownRefineries = 0;
	int enemyRefineries = 0;

	bool ticketsKnown = false;
	float ownTickets = 0.0f;
	float ownTicketsMax = 0.0f;
	bool enemyTicketsKnown = false;
	float enemyTickets = 0.0f;

	float ownCvHealthShare = 1.0f;
};

// Returns nullptr if Lawliet should not trigger, else the trigger reason string.
inline const char* TriggerReason(const DesperationInput& in)
{
	if (in.cvarMode == 0 || !in.vehiclesEnabled || !in.enemyCvAlive || !in.hasVfOrApc ||
		in.onCooldown || in.eligibleScouts < 1)
		return nullptr;

	const bool forced = in.cvarMode == in.team && (in.team == 2 || in.team == 3);
	if (in.eligibleTotal < (forced ? 1 : 2))
		return nullptr;
	// Forced: wait for a real crew while the team is still joining (011639: lone scout rush, no damage).
	if (forced && in.eligibleTotal < kForcedFullCrew && in.matchElapsed < kForcedCrewWait)
		return nullptr;
	if (forced)
		return "forced";

	if (in.cvarMode != 1 || in.matchElapsed < kMinMatchSeconds)
		return nullptr;

	const bool researchBehind = in.researchKnown &&
		((in.enemyResearchCount - in.ownResearchCount >= 3) ||
		 (in.enemyHasHeavyTank && !in.ownHasHeavyTank && in.enemyResearchCount >= in.ownResearchCount + 2) ||
		 (in.enemyHasMedTank && !in.ownHasMedTank && in.enemyResearchCount >= in.ownResearchCount + 2));

	const bool flagsBehind = in.totalFlags >= 2 && in.enemyFlags > in.ownFlags &&
		(in.enemyFlags * 100 >= in.totalFlags * 65 || (in.ownFlags == 0 && in.enemyFlags >= 2));

	const bool refsBehind = in.enemyRefineries >= in.ownRefineries + 2 &&
		(in.ownRefineries == 0 || in.enemyRefineries >= in.ownRefineries * 2);

	const bool mapBehind = (in.totalFlags >= 2 ? flagsBehind : refsBehind) || (flagsBehind && refsBehind);

	const bool ticketsCritical = in.ticketsKnown && in.ownTicketsMax > 0.0f &&
		(in.ownTickets <= in.ownTicketsMax * 0.28f || in.ownTickets <= 30.0f) &&
		(!in.enemyTicketsKnown || in.enemyTickets >= in.ownTickets * 1.5f);

	if (researchBehind && (mapBehind || ticketsCritical))
		return "desperation-research-map";
	if (ticketsCritical && mapBehind)
		return "desperation-tickets";
	if (in.ownCvHealthShare < 0.35f && mapBehind)
		return "desperation-cv";
	return nullptr;
}

struct CrewCandidate
{
	int client = -1;
	int classId = -1; // 0=scout, 1=rifleman, 2=grenadier, 3=engineer
	bool hasPreferredGrenade = false;
	bool isCurrentApcDriver = false;
	float distToRally = 0.0f;
};

struct CrewSelection
{
	int clients[kMaxCrew] = {};
	int roles[kMaxCrew] = {};
	int count = 0;
	int driverClient = -1;
	int scoutCount = 0;
	int rifleCount = 0;
	int engCount = 0;
};

inline CrewRole RoleForClass(int classId)
{
	switch (classId)
	{
	case 0: return ROLE_SCOUT;
	case 1: return ROLE_RIFLEMAN;
	case 3: return ROLE_ENGINEER;
	default: return ROLE_SUPPORT;
	}
}

// Optimal crew: 2 Scouts + 1 Engineer + 1 Rifleman + up to 2 extra Riflemen/Grenadiers (max 6).
// Minimum viable crew: at least 1 Scout. Returns false if no valid Scout exists.
inline bool SelectCrew(const CrewCandidate* candidates, int count, CrewSelection& out)
{
	out = CrewSelection();
	if (candidates == nullptr || count <= 0)
		return false;

	auto alreadyPicked = [&](int client) -> bool
	{
		for (int i = 0; i < out.count; ++i)
			if (out.clients[i] == client)
				return true;
		return false;
	};

	auto addMember = [&](const CrewCandidate& c, int role) -> bool
	{
		if (c.client <= 0 || out.count >= kMaxCrew || alreadyPicked(c.client))
			return false;
		out.clients[out.count] = c.client;
		out.roles[out.count] = role;
		++out.count;
		if (role == ROLE_SCOUT)
			++out.scoutCount;
		else if (role == ROLE_RIFLEMAN)
			++out.rifleCount;
		else if (role == ROLE_ENGINEER)
			++out.engCount;
		return true;
	};

	auto pickBest = [&](int targetClass, bool preferGrenade, bool preferDriver) -> int
	{
		int bestIdx = -1;
		for (int i = 0; i < count; ++i)
		{
			const CrewCandidate& c = candidates[i];
			if (c.client <= 0 || c.classId != targetClass || alreadyPicked(c.client))
				continue;
			if (bestIdx < 0)
			{
				bestIdx = i;
				continue;
			}
			const CrewCandidate& b = candidates[bestIdx];
			if (preferGrenade && c.hasPreferredGrenade != b.hasPreferredGrenade)
			{
				if (c.hasPreferredGrenade)
					bestIdx = i;
				continue;
			}
			if (preferDriver && c.isCurrentApcDriver != b.isCurrentApcDriver)
			{
				if (c.isCurrentApcDriver)
					bestIdx = i;
				continue;
			}
			if (c.distToRally < b.distToRally)
				bestIdx = i;
		}
		return bestIdx;
	};

	// 1. Up to 2 Scouts (prefer sticky stun, then existing APC driver, then closest).
	for (int s = 0; s < 2 && out.count < kMaxCrew; ++s)
	{
		const int idx = pickBest(0, true, true);
		if (idx < 0)
			break;
		addMember(candidates[idx], ROLE_SCOUT);
	}
	if (out.scoutCount < 1)
		return false;

	// 2. 1 Engineer (closest).
	if (out.count < kMaxCrew)
	{
		const int idx = pickBest(3, false, false);
		if (idx >= 0)
			addMember(candidates[idx], ROLE_ENGINEER);
	}

	// 3. 1 Rifleman (prefer sticky grenade, then existing APC driver, then closest).
	if (out.count < kMaxCrew)
	{
		const int idx = pickBest(1, true, true);
		if (idx >= 0)
			addMember(candidates[idx], ROLE_RIFLEMAN);
	}

	// 4. Preserve an eligible existing APC driver if not already selected.
	if (out.count < kMaxCrew)
	{
		for (int i = 0; i < count; ++i)
		{
			const CrewCandidate& c = candidates[i];
			if (c.client > 0 && c.isCurrentApcDriver && !alreadyPicked(c.client))
			{
				addMember(c, RoleForClass(c.classId));
				break;
			}
		}
	}

	// 5. Extra passengers up to kMaxCrew: Riflemen (sticky first), then Grenadiers, then extra Scouts.
	while (out.count < kMaxCrew)
	{
		const int idx = pickBest(1, true, false);
		if (idx < 0)
			break;
		addMember(candidates[idx], ROLE_RIFLEMAN);
	}
	while (out.count < kMaxCrew)
	{
		const int idx = pickBest(2, false, false);
		if (idx < 0)
			break;
		addMember(candidates[idx], ROLE_SUPPORT);
	}
	while (out.count < kMaxCrew)
	{
		const int idx = pickBest(0, true, false);
		if (idx < 0)
			break;
		addMember(candidates[idx], ROLE_SCOUT);
	}

	// Choose driver: existing APC driver first, else least disruption to stun/crate duties:
	// extra Rifleman / Support -> first Rifleman -> 2nd+ Scout -> Engineer -> 1st Scout.
	int bestPrio = 99;
	int seenRifle = 0;
	int seenScout = 0;
	for (int i = 0; i < out.count; ++i)
	{
		const int client = out.clients[i];
		const int role = out.roles[i];
		bool isApcDriver = false;
		for (int j = 0; j < count; ++j)
		{
			if (candidates[j].client == client && candidates[j].isCurrentApcDriver)
			{
				isApcDriver = true;
				break;
			}
		}
		int prio = 5;
		if (isApcDriver)
			prio = 0;
		else if (role == ROLE_SUPPORT)
			prio = 1;
		else if (role == ROLE_RIFLEMAN)
			prio = (++seenRifle >= 2) ? 1 : 2;
		else if (role == ROLE_SCOUT)
			prio = (++seenScout >= 2) ? 3 : 5;
		else if (role == ROLE_ENGINEER)
			prio = 4;

		if (prio < bestPrio)
		{
			bestPrio = prio;
			out.driverClient = client;
		}
	}

	return out.count >= 1 && out.scoutCount >= 1 && out.driverClient > 0;
}

inline BoardingAction EvaluateBoarding(bool driverSeated, int scoutsBoarded, int totalBoarded, int aliveCrew,
	float boardingElapsed, float sinceFirstPassenger, bool scoutReassignTried, float sinceCrewProgress = 1.0e9f)
{
	const bool timedOut = boardingElapsed >= kBoardingHardCap ||
		(boardingElapsed >= kBoardingTimeout && sinceCrewProgress >= kBoardingStallSeconds);
	if (!driverSeated)
		return timedOut ? BOARDING_ABORT : BOARDING_WAIT;
	if (scoutsBoarded >= 1 && aliveCrew > 0 && totalBoarded >= aliveCrew)
		return BOARDING_GO_FULL;
	if (scoutsBoarded >= 1 &&
		(boardingElapsed >= kBoardingPartialWait || (sinceFirstPassenger >= 12.0f && totalBoarded >= 2)))
		return BOARDING_GO_PARTIAL;
	if (scoutsBoarded == 0 && boardingElapsed >= kBoardingReassignWait && !scoutReassignTried)
		return BOARDING_REASSIGN_SCOUT;
	if (timedOut)
		return BOARDING_ABORT;
	return BOARDING_WAIT;
}

inline bool ShouldThrowStun(bool cvStunned, float cvStunRemaining, float sinceLastTeamStunThrow)
{
	if (sinceLastTeamStunThrow < kStunSpacingSeconds)
		return false;
	if (!cvStunned)
		return true;
	return cvStunRemaining <= kStunReapplyWindow;
}
} // namespace EmpiresLawliet

// Runtime bridge (bot_empires_lawliet.cpp).
void EmpiresLawlietLevelReset();
void EmpiresLawlietOnBotReset(int client);
void EmpiresLawlietOnClientDisconnect(int client);
bool EmpiresLawlietActive(int team);
int EmpiresLawlietPhase(int team);
bool EmpiresLawlietIsCrew(int team, int client);
bool EmpiresLawlietIsDriver(int team, int client);
bool EmpiresLawlietWantsApcBuy(int team, int client);
bool EmpiresLawlietBlocksVehicleEntry(int team, int client);
bool EmpiresLawlietBlocksSideTasks(int team, int client);
bool EmpiresLawlietStayInVehicle(int team, int client, int vehicleIndex);
bool EmpiresLawlietShouldDisembark(int team, int client, int vehicleIndex);
bool EmpiresLawlietOverrideDriving(int team, int client, int vehicleIndex, Vector& driveTo, float& arrive,
	float& throttle, bool& holdPosition);
bool EmpiresLawlietWantsSpawnAnchor(int team, int client, Vector& anchor, int& apcEntity);
// Crew member (not the driver) that still has to reach the APC / rally point: goal for a kill-spawn.
bool EmpiresLawlietBoardingGoal(int team, int client, Vector& goal);
bool EmpiresLawlietForceGrenadeLoadout(int team, int classId, int& preferredVariant);
void EmpiresLawlietNoteStunThrown(int team, int targetEntity, float dist);

#endif
