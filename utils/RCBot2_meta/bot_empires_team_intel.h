// Fair, short-lived team intelligence board (TODO 3.20 / Audit C9):
// Replaces omniscient full-entity-list scans across Team Director, Commander, Objective,
// Vehicle targeting, and Lawliet with a single fixed-size per-team memory fed only by fair
// sources: direct teammate sight, Empires spotting (m_VisibilityState / m_bSpotted), own
// engineer cameras/radars, own building/CV damage alerts, and static map knowledge (capture
// points, resource nodes, initial enemy main-base anchor).
//
// SDK-free so it can be unit-tested offline (scripts/run_team_intel_tests.sh).
#ifndef BOT_EMPIRES_TEAM_INTEL_H
#define BOT_EMPIRES_TEAM_INTEL_H

#include <algorithm>
#include <cmath>

namespace EmpiresTeamIntel
{
constexpr int kMaxContacts = 32;
constexpr int kMaxDamageEvents = 8;
constexpr int kMaxTraceBudgetPerTick = 64; // hard per-team trace cap at 22 bots

// Memory lifetimes (seconds) tuned so stealth/flanks work without making bots passive.
constexpr float kTtlInfantry = 10.0f;
constexpr float kTtlVehicle = 20.0f;  // tanks & APCs
constexpr float kTtlBuilding = 180.0f;
constexpr float kTtlEnemyCv = 25.0f;
constexpr float kTtlDamageEvent = 15.0f;

// Fair sensor detection radii (HU).
constexpr float kSightRange = 3500.0f;
constexpr float kCameraRange = 1200.0f; // requires line of sight
constexpr float kRadarRange = 1800.0f;  // sweeps without LOS

enum ContactKind
{
	CONTACT_NONE = 0,
	CONTACT_INFANTRY = 1,
	CONTACT_VEHICLE = 2,
	CONTACT_APC = 3,
	CONTACT_BUILDING = 4,
	CONTACT_ENEMY_CV = 5
};

enum IntelSource
{
	SOURCE_NONE = 0,
	SOURCE_RADAR = 1,
	SOURCE_CAMERA = 2,
	SOURCE_SPOTTED = 3,
	SOURCE_SIGHT = 4,
	SOURCE_INITIAL_BASE = 5
};

inline unsigned KindBit(ContactKind kind)
{
	return kind > CONTACT_NONE ? (1u << static_cast<unsigned>(kind)) : 0u;
}

inline float TtlForKind(ContactKind kind)
{
	switch (kind)
	{
	case CONTACT_INFANTRY: return kTtlInfantry;
	case CONTACT_VEHICLE:
	case CONTACT_APC:      return kTtlVehicle;
	case CONTACT_BUILDING: return kTtlBuilding;
	case CONTACT_ENEMY_CV: return kTtlEnemyCv;
	default:               return 0.0f;
	}
}

// Eviction priority when all 32 slots are occupied (higher = kept longer).
inline int EvictionPriority(ContactKind kind)
{
	switch (kind)
	{
	case CONTACT_ENEMY_CV: return 5;
	case CONTACT_APC:      return 4;
	case CONTACT_VEHICLE:  return 3;
	case CONTACT_BUILDING: return 2;
	case CONTACT_INFANTRY: return 1;
	default:               return 0;
	}
}

// Cvar rcbot_empires_fair_intel: 0 = off, 1 = all teams, 2 = NF only, 3 = BE only (A/B testing).
inline bool FairIntelEnabledForTeam(int cvarValue, int team)
{
	if (cvarValue <= 0)
		return false;
	if (cvarValue == 1)
		return true;
	return cvarValue == team;
}

inline float Dist2D(const float a[3], const float b[3])
{
	const float dx = a[0] - b[0];
	const float dy = a[1] - b[1];
	return std::sqrt(dx * dx + dy * dy);
}

inline float Dist3D(const float a[3], const float b[3])
{
	const float dx = a[0] - b[0];
	const float dy = a[1] - b[1];
	const float dz = a[2] - b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Sensor
{
	float pos[3] = { 0.0f, 0.0f, 0.0f };
	bool isRadar = false; // false = emp_eng_camera, true = emp_eng_radar
};

inline bool SensorInRange(const Sensor& s, const float targetPos[3])
{
	const float limit = s.isRadar ? kRadarRange : kCameraRange;
	return Dist3D(s.pos, targetPos) <= limit;
}

inline IntelSource ClassifyFairSource(bool directSight, bool gameSpotted,
	bool radarDetected, bool cameraDetected)
{
	if (directSight)
		return SOURCE_SIGHT;
	if (gameSpotted)
		return SOURCE_SPOTTED;
	if (cameraDetected)
		return SOURCE_CAMERA;
	if (radarDetected)
		return SOURCE_RADAR;
	return SOURCE_NONE;
}

struct Contact
{
	int entity = -1;
	int serial = -1; // guards against recycled entity indices
	ContactKind kind = CONTACT_NONE;
	IntelSource source = SOURCE_NONE;
	float pos[3] = { 0.0f, 0.0f, 0.0f };
	float seenAt = -1.0e9f;
	float expiresAt = -1.0e9f;

	bool active(float now) const
	{
		return entity > 0 && kind != CONTACT_NONE && now <= expiresAt;
	}
};

struct DamageEvent
{
	int ownEntity = -1;
	float ownPos[3] = { 0.0f, 0.0f, 0.0f };
	bool isOwnCv = false;
	float at = -1.0e9f;

	bool active(float now) const
	{
		return ownEntity > 0 && now <= at + kTtlDamageEvent;
	}
};

struct Board
{
	Contact contacts[kMaxContacts];
	DamageEvent damageEvents[kMaxDamageEvents];
	int nextDamageSlot = 0;

	// Static/semi-static base knowledge: initial enemy CV spawn (like a human knowing the map)
	// plus last confirmed CV sighting.
	float initialEnemyCvPos[3] = { 0.0f, 0.0f, 0.0f };
	bool initialEnemyCvKnown = false;
	float lastKnownEnemyCvPos[3] = { 0.0f, 0.0f, 0.0f };
	bool lastKnownEnemyCvValid = false;
	int lastKnownEnemyCvEntity = -1;

	int traceCursor = 0; // rotating cursor for kMaxTraceBudgetPerTick

	void Reset()
	{
		for (Contact& c : contacts)
			c = Contact();
		for (DamageEvent& d : damageEvents)
			d = DamageEvent();
		nextDamageSlot = 0;
		for (int i = 0; i < 3; ++i)
		{
			initialEnemyCvPos[i] = 0.0f;
			lastKnownEnemyCvPos[i] = 0.0f;
		}
		initialEnemyCvKnown = false;
		lastKnownEnemyCvValid = false;
		lastKnownEnemyCvEntity = -1;
		traceCursor = 0;
	}
};

// Records the enemy main-base spawn position once at round start.
inline void SetInitialEnemyCv(Board& board, const float pos[3])
{
	if (board.initialEnemyCvKnown)
		return;
	for (int i = 0; i < 3; ++i)
		board.initialEnemyCvPos[i] = pos[i];
	board.initialEnemyCvKnown = true;
}

inline void Prune(Board& board, float now)
{
	for (Contact& c : board.contacts)
	{
		if (c.entity > 0 && !c.active(now))
			c = Contact();
	}
	for (DamageEvent& d : board.damageEvents)
	{
		if (d.ownEntity > 0 && !d.active(now))
			d = DamageEvent();
	}
}

// Inserts or refreshes a fair contact. Evicts lowest-priority / oldest entry when all 32 slots are busy.
inline bool Upsert(Board& board, int entity, int serial, ContactKind kind, IntelSource source,
	const float pos[3], float now)
{
	if (entity <= 0 || kind == CONTACT_NONE || source == SOURCE_NONE)
		return false;

	const float ttl = TtlForKind(kind);
	if (ttl <= 0.0f)
		return false;

	int slot = -1;
	int freeSlot = -1;
	for (int i = 0; i < kMaxContacts; ++i)
	{
		if (board.contacts[i].entity == entity)
		{
			slot = i;
			break;
		}
		if (freeSlot < 0 && !board.contacts[i].active(now))
			freeSlot = i;
	}
	if (slot < 0)
		slot = freeSlot;
	if (slot < 0)
	{
		// All 32 slots active: evict lowest eviction priority, tie-breaking by oldest seenAt.
		int victim = 0;
		for (int i = 1; i < kMaxContacts; ++i)
		{
			const int pI = EvictionPriority(board.contacts[i].kind);
			const int pV = EvictionPriority(board.contacts[victim].kind);
			if (pI < pV || (pI == pV && board.contacts[i].seenAt < board.contacts[victim].seenAt))
				victim = i;
		}
		// Never evict a strictly higher-priority active contact for a lower-priority one.
		if (EvictionPriority(board.contacts[victim].kind) > EvictionPriority(kind))
			return false;
		slot = victim;
	}

	Contact& c = board.contacts[slot];
	c.entity = entity;
	c.serial = serial;
	c.kind = kind;
	c.source = source;
	for (int k = 0; k < 3; ++k)
		c.pos[k] = pos[k];
	c.seenAt = now;
	c.expiresAt = now + ttl;

	if (kind == CONTACT_ENEMY_CV)
	{
		for (int k = 0; k < 3; ++k)
			board.lastKnownEnemyCvPos[k] = pos[k];
		board.lastKnownEnemyCvValid = true;
		board.lastKnownEnemyCvEntity = entity;
	}
	return true;
}

inline void Remove(Board& board, int entity)
{
	if (entity <= 0)
		return;
	for (Contact& c : board.contacts)
	{
		if (c.entity == entity)
			c = Contact();
	}
}

inline const Contact* FindContact(const Board& board, int entity, int serial, float now)
{
	if (entity <= 0)
		return nullptr;
	for (const Contact& c : board.contacts)
	{
		if (c.entity == entity && c.active(now))
		{
			if (serial >= 0 && c.serial >= 0 && c.serial != serial)
				return nullptr;
			return &c;
		}
	}
	return nullptr;
}

inline bool IsKnown(const Board& board, int entity, int serial, float now)
{
	return FindContact(board, entity, serial, now) != nullptr;
}

inline int CountActive(const Board& board, unsigned kindMask, float now)
{
	int n = 0;
	for (const Contact& c : board.contacts)
	{
		if (c.active(now) && (kindMask == 0u || (KindBit(c.kind) & kindMask) != 0u))
			++n;
	}
	return n;
}

// Records fair damage taken by an own building or CV (never leaks the unseen attacker's position).
inline void RecordOwnDamage(Board& board, int ownEntity, const float ownPos[3], bool isOwnCv, float now)
{
	if (ownEntity <= 0)
		return;
	for (DamageEvent& d : board.damageEvents)
	{
		if (d.ownEntity == ownEntity && d.active(now))
		{
			for (int k = 0; k < 3; ++k)
				d.ownPos[k] = ownPos[k];
			d.isOwnCv = isOwnCv;
			d.at = now;
			return;
		}
	}
	DamageEvent& slot = board.damageEvents[board.nextDamageSlot];
	board.nextDamageSlot = (board.nextDamageSlot + 1) % kMaxDamageEvents;
	slot.ownEntity = ownEntity;
	for (int k = 0; k < 3; ++k)
		slot.ownPos[k] = ownPos[k];
	slot.isOwnCv = isOwnCv;
	slot.at = now;
}

inline bool HasRecentOwnDamage(const Board& board, int ownEntity, float now)
{
	for (const DamageEvent& d : board.damageEvents)
	{
		if (d.ownEntity == ownEntity && d.active(now))
			return true;
	}
	return false;
}

inline bool LatestOwnDamage(const Board& board, float now, DamageEvent& out)
{
	bool found = false;
	for (const DamageEvent& d : board.damageEvents)
	{
		if (!d.active(now))
			continue;
		if (!found || (d.isOwnCv && !out.isOwnCv) || (d.isOwnCv == out.isOwnCv && d.at > out.at))
		{
			out = d;
			found = true;
		}
	}
	return found;
}

// Resolves the best known enemy CV / base position without cheating:
//   1. Live enemy CV contact within 25 s TTL (isLiveSight = true)
//   2. Last confirmed CV sighting (isLiveSight = false)
//   3. Initial enemy main-base spawn anchor (isLiveSight = false)
inline bool EnemyBaseAnchor(const Board& board, float now, float outPos[3], bool& isLiveSight)
{
	isLiveSight = false;
	const Contact* bestCv = nullptr;
	for (const Contact& c : board.contacts)
	{
		if (c.kind == CONTACT_ENEMY_CV && c.active(now))
		{
			if (bestCv == nullptr || c.seenAt > bestCv->seenAt)
				bestCv = &c;
		}
	}
	if (bestCv != nullptr)
	{
		for (int k = 0; k < 3; ++k)
			outPos[k] = bestCv->pos[k];
		isLiveSight = true;
		return true;
	}
	if (board.lastKnownEnemyCvValid)
	{
		for (int k = 0; k < 3; ++k)
			outPos[k] = board.lastKnownEnemyCvPos[k];
		return true;
	}
	if (board.initialEnemyCvKnown)
	{
		for (int k = 0; k < 3; ++k)
			outPos[k] = board.initialEnemyCvPos[k];
		return true;
	}
	return false;
}

// Nearest active contact matching kindMask within maxRange (maxRange <= 0 means unlimited).
inline bool NearestKnown(const Board& board, const float fromPos[3], unsigned kindMask,
	float maxRange, float now, Contact& out)
{
	bool found = false;
	float bestDist = maxRange > 0.0f ? maxRange : 1.0e9f;
	for (const Contact& c : board.contacts)
	{
		if (!c.active(now))
			continue;
		if (kindMask != 0u && (KindBit(c.kind) & kindMask) == 0u)
			continue;
		const float d = Dist2D(fromPos, c.pos);
		if (d < bestDist)
		{
			bestDist = d;
			out = c;
			found = true;
		}
	}
	return found;
}

// --- Tactical helpers for Commit 3 & Commit 4 consumers (Vehicle, APC, Lawliet, Commander CV) ---

struct VehicleIntelGoal
{
	enum Kind
	{
		GOAL_NONE = 0,
		GOAL_DEFEND_VEHICLE,
		GOAL_DEFEND_DAMAGE,
		GOAL_HUNT_VEHICLE,
		GOAL_PUSH_BUILDING,
		GOAL_BASE_ANCHOR
	};
	Kind kind = GOAL_NONE;
	int entity = -1;
	float pos[3] = { 0.0f, 0.0f, 0.0f };
	bool isApc = false;
};

// Fair strategic vehicle goal selection (replaces omniscient defend/hunt/push/cv scans in tickEmpiresVehicle):
//   1. Defend: known enemy vehicle/APC near own assets (immediate CV threat < 1500 overrides APC)
//      or recent damage alert at an own building/CV even if attacker is unseen.
//   2. Hunt: known enemy vehicle/APC within 6000 HU (APC 7000 HU).
//   3. Push: known enemy building closer than EnemyBaseAnchor - 1500 HU.
//   4. Fallback: EnemyBaseAnchor so tanks always push toward the front (keeps MATCH_FLOW R5 green).
inline VehicleIntelGoal SelectVehicleStrategicGoal(const Board& board, const float origin[3],
	const float (*ownAssets)[3], int ownAssetCount, const float* ownCvPos, float now)
{
	VehicleIntelGoal result;

	// 1 & 2: Defend / Hunt known enemy vehicles and APCs.
	float bestDefendScore = 1.0e9f;
	const Contact* bestDefend = nullptr;
	float bestHuntScore = 1.0e9f;
	const Contact* bestHunt = nullptr;

	for (const Contact& c : board.contacts)
	{
		if (!c.active(now) || (c.kind != CONTACT_VEHICLE && c.kind != CONTACT_APC))
			continue;
		const bool isApc = (c.kind == CONTACT_APC);
		const float d = Dist2D(origin, c.pos);
		float minAssetDist = 1.0e9f;
		for (int a = 0; a < ownAssetCount; ++a)
			minAssetDist = (std::min)(minAssetDist, Dist2D(ownAssets[a], c.pos));
		const bool nearAsset = minAssetDist < (isApc ? 3500.0f : 3000.0f);
		const bool immediateCvThreat = !isApc && ownCvPos != nullptr && Dist2D(ownCvPos, c.pos) < 1500.0f;
		const float defendScore = d - (immediateCvThreat ? 6000.0f : (isApc ? 3000.0f : 0.0f));
		const float huntScore = d - (isApc ? 3000.0f : 0.0f);

		if (nearAsset && defendScore < bestDefendScore)
		{
			bestDefendScore = defendScore;
			bestDefend = &c;
		}
		if (d < (isApc ? 7000.0f : 6000.0f) && huntScore < bestHuntScore)
		{
			bestHuntScore = huntScore;
			bestHunt = &c;
		}
	}

	if (bestDefend != nullptr)
	{
		result.kind = VehicleIntelGoal::GOAL_DEFEND_VEHICLE;
		result.entity = bestDefend->entity;
		for (int k = 0; k < 3; ++k)
			result.pos[k] = bestDefend->pos[k];
		result.isApc = (bestDefend->kind == CONTACT_APC);
		return result;
	}

	// Unseen attacker damaging an own building/CV -> fair defense response to the own building.
	DamageEvent dmg;
	if (LatestOwnDamage(board, now, dmg))
	{
		result.kind = VehicleIntelGoal::GOAL_DEFEND_DAMAGE;
		result.entity = dmg.ownEntity;
		for (int k = 0; k < 3; ++k)
			result.pos[k] = dmg.ownPos[k];
		return result;
	}

	if (bestHunt != nullptr)
	{
		result.kind = VehicleIntelGoal::GOAL_HUNT_VEHICLE;
		result.entity = bestHunt->entity;
		for (int k = 0; k < 3; ++k)
			result.pos[k] = bestHunt->pos[k];
		result.isApc = (bestHunt->kind == CONTACT_APC);
		return result;
	}

	// 3. Known enemy building closer than the enemy base anchor - 1500.
	float anchorPos[3] = { 0.0f, 0.0f, 0.0f };
	bool anchorLive = false;
	const bool haveAnchor = EnemyBaseAnchor(board, now, anchorPos, anchorLive);
	const float anchorDist = haveAnchor ? Dist2D(origin, anchorPos) : 1.0e9f;
	float bestBuildingDist = anchorDist - 1500.0f;
	const Contact* bestBuilding = nullptr;
	for (const Contact& c : board.contacts)
	{
		if (!c.active(now) || c.kind != CONTACT_BUILDING)
			continue;
		const float d = Dist2D(origin, c.pos);
		if (d < bestBuildingDist)
		{
			bestBuildingDist = d;
			bestBuilding = &c;
		}
	}
	if (bestBuilding != nullptr)
	{
		result.kind = VehicleIntelGoal::GOAL_PUSH_BUILDING;
		result.entity = bestBuilding->entity;
		for (int k = 0; k < 3; ++k)
			result.pos[k] = bestBuilding->pos[k];
		return result;
	}

	// 4. Fallback to enemy base anchor (live CV, last known CV, or initial main-base spawn).
	if (haveAnchor)
	{
		result.kind = VehicleIntelGoal::GOAL_BASE_ANCHOR;
		result.entity = board.lastKnownEnemyCvEntity;
		for (int k = 0; k < 3; ++k)
			result.pos[k] = anchorPos[k];
	}
	return result;
}

// Fair APC cover-park anchor on flagless maps (e.g. emp_crossroads):
// prefers nearest known enemy building to ownSidePos, else nearest provided mid-map resource point,
// else EnemyBaseAnchor (so the spawn APC never stalls in home base before scouting).
inline bool SelectApcCoverAnchor(const Board& board, const float ownSidePos[3],
	const float (*midResources)[3], int midResourceCount, float now, float outAnchor[3], bool& fromBuilding)
{
	fromBuilding = false;
	Contact nearestBldg;
	if (NearestKnown(board, ownSidePos, KindBit(CONTACT_BUILDING), 0.0f, now, nearestBldg))
	{
		for (int k = 0; k < 3; ++k)
			outAnchor[k] = nearestBldg.pos[k];
		fromBuilding = true;
		return true;
	}
	if (midResources != nullptr && midResourceCount > 0)
	{
		int bestIdx = 0;
		float bestDist = Dist2D(ownSidePos, midResources[0]);
		for (int i = 1; i < midResourceCount; ++i)
		{
			const float d = Dist2D(ownSidePos, midResources[i]);
			if (d < bestDist)
			{
				bestDist = d;
				bestIdx = i;
			}
		}
		for (int k = 0; k < 3; ++k)
			outAnchor[k] = midResources[bestIdx][k];
		return true;
	}
	bool live = false;
	return EnemyBaseAnchor(board, now, outAnchor, live);
}

// Fair Lawliet CV target: starts on initial base anchor and upgrades to live CV coordinates
// as soon as any teammate or crew member spots the enemy CV.
inline bool ResolveLawlietCvTarget(const Board& board, float now, float outPos[3], bool& isLiveSight)
{
	return EnemyBaseAnchor(board, now, outPos, isLiveSight);
}

// Fair CV retreat threat: only reacts to a known enemy vehicle within maxThreatDist (2200 HU).
// Under unseen heavy fire (returns false), the caller retreats toward own repair pad / home
// instead of reading the nearest living enemy's position worldwide.
inline bool ResolveCvRetreatThreat(const Board& board, const float ownCvPos[3],
	float maxThreatDist, float now, float outThreatPos[3])
{
	Contact threat;
	if (!NearestKnown(board, ownCvPos, KindBit(CONTACT_VEHICLE) | KindBit(CONTACT_APC), maxThreatDist, now, threat))
		return false;
	for (int k = 0; k < 3; ++k)
		outThreatPos[k] = threat.pos[k];
	return true;
}
} // namespace EmpiresTeamIntel

#endif
