// Team director runtime (TODO 5.4 / 5.8 / 5.28), see bot_empires_team_director.h.
// Every bot calls tickTeamDirector() from modThink; the board of its team is rebuilt at most every
// kUpdateSeconds from the team's own view: own CV, own buildings and their health, capture points,
// enemies and unfinished enemy buildings next to own buildings.
#include "bot.h"
#include "bot_cvars.h"
#include "bot_empires.h"
#include "bot_empires_binary.h"
#include "bot_empires_lawliet.h"
#include "bot_empires_team_director.h"
#include "bot_getprop.h"
#include "bot_globals.h"
#include "bot_mods.h"
#include "bot_navmesh_api.h"
#include "server_class.h"
#include "iplayerinfo.h"
#include "tier1/strtools.h"
#include "toolframework/itoolentity.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern IVEngineServer* engine;
extern IPlayerInfoManager* playerinfomanager;
extern IServerPluginHelpers* helpers;
extern IServerTools* servertools;

bool EmpiresCapturePointOnMesh(const CEmpiresMod::CapturePointObservation& point); // bot_empires_objective.cpp
bool EmpiresCommanderOrderIsOurs(int team, int client); // bot_empires_commander_orders.cpp
int EmpiresVehicleReadInt(const CBotEmpires* bot, const edict_t* e, const char* prop, int fallback); // bot_empires_vehicle.cpp
int EmpiresVehiclePlayerVehicle(const CBotEmpires* bot, const edict_t* player); // bot_empires_vehicle.cpp
bool EmpiresVehicleAlive(const CBotEmpires* bot, const edict_t* vehicle); // bot_empires_vehicle.cpp

using namespace EmpiresTeamDirector;

namespace
{
constexpr float kUpdateSeconds = 1.5f;
constexpr float kLogEvery = 30.0f;
// Alert stays this long after the last trigger. 15 s let alerts at a forward refinery come back 1-12 s after
// the defender left (palmbay 171039: the only free BE bot swapped 6x); 40 s matches kRoleMinSeconds.
constexpr float kAlertHold = 40.0f;
constexpr float kStabilizeHold = 60.0f;
constexpr float kEnemyInfantryRadius = 1500.0f;
constexpr float kEnemyVehicleRadius = 2500.0f;
constexpr float kEnemyBuildingRadius = 2000.0f;
constexpr float kForwardBarracks = 3000.0f; // barracks this far from its CV counts as forward
constexpr float kFlagAvoid = 120.0f;

struct HealthSample
{
	int serial = -1;
	int health = 0;
};

struct TeamState
{
	Board board;
	float nextUpdate = 0.0f;
	float nextLog = 0.0f;
	std::string lastSignature;
	float alertUntil = -1.0f;
	int alertScore = 0;       // threat score of board.alert's own building, last time it triggered
	float alertSeen = -1.0f;  // last time board.alert's building triggered itself
	Stabilization stabilization;
	int coreCounts[4] = { -1, -1, -1, -1 }; // barracks, factory, refinery, radar
	std::unordered_map<int, HealthSample> health;
	std::unordered_map<int, FlagPushProgress> flagProgress;
	std::unordered_map<int, float> pushAvoid;   // flag entity -> avoided until
	int lastRole[kMaxClients] = {};
	DeathMemory deaths;        // TODO 8.1 hot zones
	float roleSince[kMaxClients] = {};          // game time the current role was given
	std::unordered_map<int, int> flagOwner;     // flag entity -> last known owner (gain/loss log)
	EmpiresTeamIntel::Board intel;              // 3.20 fair enemy contacts
	int intelSources[6] = {};                   // diagnostic: upserts per IntelSource since the last log
	int intelTraces = 0;                        // diagnostic: LOS traces since the last log
	int intelHidden = 0;                        // diagnostic: enemy candidates not known since the last log
};
TeamState s_team[4];

// 3.20: what an enemy entity is for the board (-1 = not a contact).
EmpiresTeamIntel::ContactKind IntelKindOf(const CBotEmpires* reader, edict_t* e)
{
	if (e == nullptr || e->IsFree() || engine == nullptr || gpGlobals == nullptr)
		return EmpiresTeamIntel::CONTACT_NONE;
	const int index = engine->IndexOfEdict(e);
	if (index >= 1 && index <= gpGlobals->maxClients)
		return EmpiresTeamIntel::CONTACT_INFANTRY;
	const char* cls = e->GetClassName();
	if (cls == nullptr)
		return EmpiresTeamIntel::CONTACT_NONE;
	if (std::strcmp(cls, "emp_nf_commander") == 0 || std::strcmp(cls, "emp_imp_commander") == 0)
		return EmpiresTeamIntel::CONTACT_ENEMY_CV;
	if (std::strcmp(cls, "emp_vehicle") == 0)
	{
		const int chassis = reader != nullptr ? EmpiresVehicleReadInt(reader, e, "m_iChassis", -1) : -1;
		if (chassis >= 0)
		{
			if (chassis >= 14 && chassis <= 16)
				return EmpiresTeamIntel::CONTACT_ENEMY_CV;
			if (chassis == 1 || chassis == 9)
				return EmpiresTeamIntel::CONTACT_APC;
		}
		return EmpiresTeamIntel::CONTACT_VEHICLE;
	}
	if (std::strncmp(cls, "emp_building_", 13) == 0 || std::strncmp(cls, "emp_eng_", 8) == 0)
		return EmpiresTeamIntel::CONTACT_BUILDING;
	return EmpiresTeamIntel::CONTACT_NONE;
}

float Dist2D(const Vector& a, const Vector& b)
{
	return (a - b).Length2D();
}

void SetGoal(Goal& goal, int entity, const Vector& at, const char* kind)
{
	goal.entity = entity;
	goal.pos[0] = at.x;
	goal.pos[1] = at.y;
	goal.pos[2] = at.z;
	goal.kind = kind;
}

Vector GoalVector(const Goal& goal)
{
	return Vector(goal.pos[0], goal.pos[1], goal.pos[2]);
}

// Own building weight for the base alert, 0 = not watched (turrets, walls, cameras, mines).
int WatchedWeight(const char* cls, const char*& kind)
{
	kind = "";
	if (std::strstr(cls, "turret") != nullptr || std::strstr(cls, "wall") != nullptr ||
		std::strstr(cls, "camera") != nullptr || std::strstr(cls, "eng_radar") != nullptr ||
		std::strstr(cls, "recycle") != nullptr)
		return 0;
	if (std::strstr(cls, "barracks") != nullptr) { kind = "barracks"; return 4; }
	if (std::strstr(cls, "vehiclefactory") != nullptr) { kind = "factory"; return 4; }
	if (std::strstr(cls, "refinery") != nullptr) { kind = "refinery"; return 3; }
	if (std::strstr(cls, "radar") != nullptr) { kind = "radar"; return 2; }
	if (std::strstr(cls, "armory") != nullptr) { kind = "armory"; return 2; }
	if (std::strstr(cls, "repairstation") != nullptr) { kind = "repairpad"; return 1; }
	return 0;
}

int CoreSlot(const char* kind)
{
	if (std::strcmp(kind, "barracks") == 0) return 0;
	if (std::strcmp(kind, "factory") == 0) return 1;
	if (std::strcmp(kind, "refinery") == 0) return 2;
	if (std::strcmp(kind, "radar") == 0) return 3;
	return -1;
}

bool BuildingTeam(const CBotEmpires* reader, edict_t* e, const char* cls, int& team)
{
	team = EmpiresVehicleReadInt(reader, e, "m_iTeamNum", -1);
	if (team == EMP_TEAM_NF || team == EMP_TEAM_BE)
		return true;
	team = std::strncmp(cls, "emp_building_nf_", 16) == 0 ? EMP_TEAM_NF :
		(std::strncmp(cls, "emp_building_imp_", 17) == 0 ? EMP_TEAM_BE : -1);
	return team > 0;
}

// Team tickets: CEmpTeam "tickets_balance" (NetworkVar at +0x3cc, the value
// CEmpLogicTicketListener::GetTeamCount 0x63cd50 reports). Team number from m_iTeamNum, raw +0x3a8.
bool ReadTeamTickets(const CBotEmpires* reader, int team, float& tickets)
{
	for (int i = 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetNetworkable() == nullptr)
			continue;
		ServerClass* cls = e->GetNetworkable()->GetServerClass();
		if (cls == nullptr || std::strcmp(cls->GetName(), "CEmpTeam") != 0)
			continue;
		CBaseEntity* base = e->GetUnknown() != nullptr ? e->GetUnknown()->GetBaseEntity() : nullptr;
		int number = EmpiresVehicleReadInt(reader, e, "m_iTeamNum", -1);
		if (number < 0 && base != nullptr && EmpiresBinaryOffsetsTrusted())
			number = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(base) + 0x3a8);
		if (number != team)
			continue;
		SendProp* prop = cls->m_pTable != nullptr ? UTIL_FindSendProp(cls->m_pTable, "tickets_balance") : nullptr;
		unsigned offset = 0;
		if (prop != nullptr && prop->GetType() == DPT_Float &&
			UTIL_FindSendPropInfo(cls, "tickets_balance", &offset) && offset != 0)
		{
			CClassInterfaceValue value(cls->m_pNetworkName, "tickets_balance", 0);
			value.findOffset();
			if (const float* p = value.getFloatPointer(e))
			{
				tickets = *p;
				return true;
			}
		}
		if (base != nullptr && EmpiresBinaryOffsetsTrusted())
		{
			tickets = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(base) + 0x3cc);
			return tickets >= 0.0f && tickets < 1.0e6f;
		}
		return false;
	}
	return false;
}

bool SnapToMesh(const Vector& at, float radius, Vector& out)
{
	if (!RCBotNavMesh::IsLoaded())
	{
		out = at;
		return true;
	}
	return RCBotNavMesh::ClosestPointOnMesh(at, radius, out);
}

// Walkable spot in front of an object (building, resource pad, CV) on the side toward `from`: the
// object's own origin sits inside its hull or prop (crossroads dev_check 20261121-153156: push to a
// resource point ended 66 away with 10 no-progress retries; 20261122-213339: barracks offset 300
// landed inside the 1120-wide barracks box).
bool ApproachSpot(const Vector& target, bool fromKnown, const Vector& from, float offset, Vector& out)
{
	Vector baseDir(1.0f, 0.0f, 0.0f);
	if (fromKnown)
	{
		Vector dir = from - target;
		dir.z = 0.0f;
		const float len = dir.Length2D();
		if (len > 1.0f)
			baseDir = dir / len;
	}
	bool fallbackFound = false;
	Vector fallback;
	static const float kAngles[] = { 0.0f, 0.785398f, -0.785398f, 1.570796f, -1.570796f, 2.356194f, -2.356194f, 3.141593f };
	for (float extra : { 0.0f, 250.0f, 500.0f, 750.0f })
	{
		const float r = offset + extra;
		for (float a : kAngles)
		{
			const Vector dir(baseDir.x * std::cos(a) - baseDir.y * std::sin(a),
				baseDir.x * std::sin(a) + baseDir.y * std::cos(a), 0.0f);
			const Vector probe = target + dir * r;
			Vector candidate;
			if (!SnapToMesh(probe, 350.0f, candidate))
				continue;
			if (!fallbackFound)
			{
				fallback = candidate;
				fallbackFound = true;
			}
			if (!RCBotNavMesh::IsLoaded() || !RCBotNavMesh::PointNearBuilding(candidate, 48.0f))
			{
				out = candidate;
				return true;
			}
		}
	}
	if (fallbackFound)
	{
		out = fallback;
		return true;
	}
	return SnapToMesh(target, 500.0f, out);
}

struct Candidate
{
	int entity;
	Vector spot;
	const char* kind;
	float sortKey;
};
} // namespace

const Board* EmpiresTeamDirectorBoard(int team)
{
	if (team < 2 || team > 3 || s_team[team].board.updatedAt < 0.0f)
		return nullptr;
	return &s_team[team].board;
}

void EmpiresTeamDirectorLevelReset()
{
	for (TeamState& state : s_team)
		state = TeamState();
}

bool EmpiresFairIntelEnabled(int team)
{
	return (team == EMP_TEAM_NF || team == EMP_TEAM_BE) &&
		EmpiresTeamIntel::FairIntelEnabledForTeam(rcbot_empires_fair_intel.GetInt(), team);
}

const EmpiresTeamIntel::Board* EmpiresTeamIntelBoard(int team)
{
	return EmpiresFairIntelEnabled(team) ? &s_team[team].intel : nullptr;
}

bool EmpiresTeamIntelKnownPos(int team, edict_t* enemy, Vector& at)
{
	if (enemy == nullptr || enemy->IsFree() || engine == nullptr || gpGlobals == nullptr)
		return false;
	if (!EmpiresFairIntelEnabled(team))
	{
		at = CBotGlobals::entityOrigin(enemy);
		return true;
	}
	const EmpiresTeamIntel::Contact* c = EmpiresTeamIntel::FindContact(s_team[team].intel, engine->IndexOfEdict(enemy),
		enemy->m_NetworkSerialNumber, gpGlobals->curtime);
	if (c == nullptr)
		return false;
	at = Vector(c->pos[0], c->pos[1], c->pos[2]);
	return true;
}

bool EmpiresTeamIntelKnows(int team, edict_t* enemy)
{
	Vector at;
	return EmpiresTeamIntelKnownPos(team, enemy, at);
}

void EmpiresTeamIntelReportSight(const CBotEmpires* reporter, edict_t* enemy)
{
	if (reporter == nullptr || enemy == nullptr || enemy->IsFree() || gpGlobals == nullptr || engine == nullptr)
		return;
	const int team = CClassInterface::getTeam(reporter->getEdict());
	if (!EmpiresFairIntelEnabled(team) || CClassInterface::getTeam(enemy) == team)
		return;
	const EmpiresTeamIntel::ContactKind kind = IntelKindOf(reporter, enemy);
	if (kind == EmpiresTeamIntel::CONTACT_NONE)
		return;
	const Vector at = CBotGlobals::entityOrigin(enemy);
	const float pos[3] = { at.x, at.y, at.z };
	if (EmpiresTeamIntel::Upsert(s_team[team].intel, engine->IndexOfEdict(enemy), enemy->m_NetworkSerialNumber, kind,
			EmpiresTeamIntel::SOURCE_SIGHT, pos, gpGlobals->curtime))
		++s_team[team].intelSources[0]; // slot 0: bot sight reports
}

bool EmpiresTeamIntelEnemyBase(int team, Vector& at, bool& live)
{
	live = false;
	if ((team != EMP_TEAM_NF && team != EMP_TEAM_BE) || gpGlobals == nullptr)
		return false;
	float pos[3];
	if (!EmpiresTeamIntel::EnemyBaseAnchor(s_team[team].intel, gpGlobals->curtime, pos, live))
		return false;
	at = Vector(pos[0], pos[1], pos[2]);
	return true;
}

namespace
{
// Hands the team's active hot zones to the route cost (TODO 8.1).
void PushHotZones(int team, const DeathMemory& memory, float now)
{
	Vector centers[kMaxHotZones];
	float radii[kMaxHotZones];
	int count = 0;
	for (const HotZone& zone : memory.zone)
		if (zone.active(now))
		{
			centers[count] = Vector(zone.center[0], zone.center[1], zone.center[2]);
			radii[count] = zone.radius;
			++count;
		}
	RCBotNavMesh::SetTeamHotZones(team, centers, radii, count);
}

void ExpireHotZones(int team, float now)
{
	DeathMemory& memory = s_team[team].deaths;
	bool changed = false;
	for (int z = 0; z < kMaxHotZones; ++z)
	{
		HotZone& zone = memory.zone[z];
		if (zone.radius <= 0.0f || zone.active(now))
			continue;
		RCBOT_DIAG("[RCBOT2] director hot-zone end team=%d zone=%d pos=(%.0f %.0f %.0f) deaths=%d\n", team, z,
			zone.center[0], zone.center[1], zone.center[2], zone.deaths);
		zone = HotZone();
		changed = true;
	}
	if (changed)
		PushHotZones(team, memory, now);
}
} // namespace

namespace
{
Vector g_testKillAt;
float g_testKillRadius = 0.0f;
int g_testKillTeam = 0;
bool g_testKillMark[kMaxClients] = {};
} // namespace

void EmpiresTestKillZoneArm(float x, float y, float z, float radius, int team)
{
	g_testKillAt = Vector(x, y, z);
	g_testKillRadius = radius;
	g_testKillTeam = team;
	RCBOT_DIAG("[RCBOT2] test-kill-zone armed pos=(%.0f %.0f %.0f) radius=%.0f team=%d\n", x, y, z, radius, team);
}

bool EmpiresTestKillZoneHit(edict_t* bot, int team, const Vector& at)
{
	if (g_testKillRadius <= 0.0f || bot == nullptr || engine == nullptr || (g_testKillTeam != 0 && team != g_testKillTeam))
		return false;
	const int client = engine->IndexOfEdict(bot);
	if (client <= 0 || client >= kMaxClients || g_testKillMark[client] || Dist2D(at, g_testKillAt) > g_testKillRadius ||
		std::fabs(at.z - g_testKillAt.z) > 200.0f)
		return false;
	g_testKillMark[client] = true;
	return true;
}

bool EmpiresTestKillZoneTake(int client)
{
	if (client <= 0 || client >= kMaxClients || !g_testKillMark[client])
		return false;
	g_testKillMark[client] = false;
	return true;
}

void EmpiresTeamDirectorRecordDeath(int team, float x, float y, float z, int userId, const char* how)
{
	if (gpGlobals == nullptr || team < 2 || team > 3 || !rcbot_empires_hot_zones.GetBool())
		return;
	const float now = gpGlobals->curtime;
	DeathMemory& memory = s_team[team].deaths;
	const float at[3] = { x, y, z };
	int zoneIndex = -1;
	const HotResult result = RecordDeath(memory, at, now, zoneIndex);
	RCBOT_DIAG("[RCBOT2] director death team=%d userid=%d how=%s pos=(%.0f %.0f %.0f) hot=%s zone=%d\n", team, userId,
		how != nullptr ? how : "", x, y, z, result == HOT_NEW ? "new" : (result == HOT_EXTEND ? "extend" : "none"),
		zoneIndex);
	if (result == HOT_NONE)
		return;
	const HotZone& zone = memory.zone[zoneIndex];
	if (result == HOT_NEW)
		RCBOT_DIAG("[RCBOT2] director hot-zone team=%d zone=%d pos=(%.0f %.0f %.0f) radius=%.0f deaths=%d window=%.0f secs=%.0f\n",
			team, zoneIndex, zone.center[0], zone.center[1], zone.center[2], zone.radius, zone.deaths,
			kHotDeathWindow, kHotZoneSeconds);
	PushHotZones(team, memory, now);
}

void CBotEmpires::tickTeamDirector()
{
	if (gpGlobals == nullptr || engine == nullptr || playerinfomanager == nullptr || m_pEdict == nullptr)
		return;
	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return;
	TeamState& state = s_team[team];
	const float now = gpGlobals->curtime;
	if (now < state.nextUpdate)
		return;
	state.nextUpdate = now + kUpdateSeconds;
	ExpireHotZones(team, now);
	const CEmpiresMod* mod = static_cast<const CEmpiresMod*>(CBotGlobals::getCurrentMod());
	if (mod == nullptr)
		return;
	Board& board = state.board;
	const int enemyTeam = team == EMP_TEAM_NF ? EMP_TEAM_BE : EMP_TEAM_NF;
	const char* ownCvClass = team == EMP_TEAM_NF ? "emp_nf_commander" : "emp_imp_commander";
	const char* enemyCvClass = team == EMP_TEAM_NF ? "emp_imp_commander" : "emp_nf_commander";

	// --- 3.20 fair intel: the director only uses enemies the team could know (game spotting, own
	// radar/camera, a living teammate's line of sight, bot sight reports) plus the initial enemy base.
	const bool fair = EmpiresFairIntelEnabled(team);
	EmpiresTeamIntel::Board& intel = state.intel;
	EmpiresTeamIntel::Prune(intel, now);
	enum IntelDispatch { DISPATCH_CV, DISPATCH_VEHICLE, DISPATCH_BUILDING, DISPATCH_INFANTRY };
	struct IntelPending
	{
		edict_t* e;
		int entity;
		Vector at;
		EmpiresTeamIntel::ContactKind kind;
		IntelDispatch dispatch;
		bool built;
		const char* buildingKind;
		bool needsTrace;
	};
	std::vector<IntelPending> intelPending;
	std::vector<EmpiresTeamIntel::Sensor> sensors;
	std::vector<Vector> ownEyes;
	if (fair)
	{
		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			IPlayerInfo* info = e != nullptr && !e->IsFree() ? playerinfomanager->GetPlayerInfo(e) : nullptr;
			if (info == nullptr || !info->IsConnected() || info->IsDead() || info->IsObserver() || info->GetHealth() <= 0)
				continue;
			if (info->GetTeamIndex() == team)
				ownEyes.push_back(CBotGlobals::entityOrigin(e) + Vector(0, 0, 64));
			else if (info->GetTeamIndex() == enemyTeam)
				intelPending.push_back({ e, i, CBotGlobals::entityOrigin(e), EmpiresTeamIntel::CONTACT_INFANTRY,
					DISPATCH_INFANTRY, true, "", false });
		}
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			const char* cls = e != nullptr && !e->IsFree() ? e->GetClassName() : nullptr;
			if (cls == nullptr || (std::strcmp(cls, "emp_eng_camera") != 0 && std::strcmp(cls, "emp_eng_radar") != 0))
				continue;
			int sTeam = -1, hp = 0;
			if (!readEmpiresEntityIntProperty(e, "m_iTeamNum", sTeam) || sTeam != team ||
				!readEmpiresEntityIntProperty(e, "m_iHealth", hp) || hp <= 0)
				continue;
			EmpiresTeamIntel::Sensor sensor;
			const Vector at = CBotGlobals::entityOrigin(e);
			sensor.pos[0] = at.x;
			sensor.pos[1] = at.y;
			sensor.pos[2] = at.z + 32.0f;
			sensor.isRadar = std::strcmp(cls, "emp_eng_radar") == 0;
			sensors.push_back(sensor);
		}
	}

	// --- Entities: own CV and buildings, enemy CV, buildings, vehicles, resource points.
	struct Watched
	{
		int entity;
		Vector at;
		const char* kind;
		int weight;
		bool damaged;
	};
	std::vector<Watched> watched;
	std::vector<Vector> enemyVehicles;
	std::vector<Vector> enemyUnfinished;
	std::vector<Candidate> enemyTargets;
	std::vector<Candidate> ownForward;
	std::vector<Vector> refineries; // any team, to tell free resource points
	std::vector<Candidate> resources;
	bool ownCvKnown = false, enemyCvKnown = false;
	Vector ownCv, enemyCv;
	int ownCvEntity = -1, enemyCvEntity = -1;
	float cvHealthShare = 1.0f;
	int coreCounts[4] = {};
	std::vector<Candidate> ownBarracks;
	std::vector<Candidate> enemyBarracks;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetIServerEntity() == nullptr || e->GetClassName() == nullptr)
			continue;
		const char* cls = e->GetClassName();
		const bool cvClass = std::strcmp(cls, ownCvClass) == 0 || std::strcmp(cls, enemyCvClass) == 0;
		const bool vehicle = std::strcmp(cls, "emp_vehicle") == 0;
		if (cvClass || vehicle)
		{
			int vTeam = -1, chassis = -1;
			if (cvClass)
				vTeam = std::strcmp(cls, ownCvClass) == 0 ? team : enemyTeam;
			else if (!readEmpiresEntityIntProperty(e, "m_iTeamNum", vTeam))
				continue;
			const bool cv = cvClass || (readEmpiresEntityIntProperty(e, "m_iChassis", chassis) && chassis >= 14 && chassis <= 16);
			if (!EmpiresVehicleAlive(this, e))
				continue;
			const Vector at = CBotGlobals::entityOrigin(e);
			if (cv && vTeam == team && !ownCvKnown)
			{
				ownCvKnown = true;
				ownCv = at;
				ownCvEntity = i;
				int hp = 0, maxHp = 0;
				if (readEmpiresEntityIntProperty(e, "m_iHealth", hp) && readEmpiresEntityIntProperty(e, "m_iMaxHealth", maxHp) &&
					hp > 0 && maxHp > 0)
					cvHealthShare = static_cast<float>(hp) / maxHp;
				watched.push_back({ i, at, "cv", 5, false });
			}
			else if (cv && vTeam == enemyTeam && !enemyCvKnown)
			{
				// The enemy main base at round start is map knowledge (a player knows where it spawns).
				const float pos[3] = { at.x, at.y, at.z };
				EmpiresTeamIntel::SetInitialEnemyCv(intel, pos);
				if (fair)
					intelPending.push_back({ e, i, at, EmpiresTeamIntel::CONTACT_ENEMY_CV, DISPATCH_CV, true, "", false });
				else
				{
					enemyCvKnown = true;
					enemyCv = at;
					enemyCvEntity = i;
				}
			}
			else if (!cv && vTeam == enemyTeam)
			{
				if (fair)
					intelPending.push_back({ e, i, at, chassis == 1 || chassis == 9 ? EmpiresTeamIntel::CONTACT_APC :
						EmpiresTeamIntel::CONTACT_VEHICLE, DISPATCH_VEHICLE, true, "", false });
				else
					enemyVehicles.push_back(at);
			}
			continue;
		}
		if (std::strcmp(cls, "emp_resource_point") == 0)
		{
			resources.push_back({ i, CBotGlobals::entityOrigin(e), "resource", 0.0f });
			continue;
		}
		if (std::strncmp(cls, "emp_building_", 13) != 0 || std::strstr(cls, "physbox") != nullptr ||
			std::strstr(cls, "panel") != nullptr)
			continue;
		int bTeam = -1;
		if (!BuildingTeam(this, e, cls, bTeam))
			continue;
		int hp = 0, maxHp = 0;
		if (!readEmpiresEntityIntProperty(e, "m_iHealth", hp) || hp <= 0)
			continue;
		readEmpiresEntityIntProperty(e, "m_iMaxHealth", maxHp);
		// Finished = m_bFullyBuilt; full HP only as fallback (a damaged finished barracks at 999/1000
		// counted as lost and masked its real loss later, review 22.11.).
		bool built = false;
		if (!readEmpiresEntityBoolProperty(e, "m_bFullyBuilt", built))
			built = maxHp <= 0 || hp >= maxHp;
		// Only SetHealth sets the flag (0x61b2d5): a map-placed building at full HP may never pass it.
		built = built || (maxHp > 0 && hp >= maxHp);
		const Vector at = CBotGlobals::entityOrigin(e);
		if (std::strstr(cls, "refinery") != nullptr)
			refineries.push_back(at);
		const char* kind = "";
		const int weight = WatchedWeight(cls, kind);
		if (bTeam == team)
		{
			if (weight > 0)
			{
				// Damage: health went down since the last sample (unfinished buildings only go up).
				HealthSample& sample = state.health[i];
				const int serial = e->m_NetworkSerialNumber;
				const bool damaged = sample.serial == serial && hp + 3 <= sample.health;
				sample.serial = serial;
				sample.health = hp;
				watched.push_back({ i, at, kind, weight, damaged });
				const int core = CoreSlot(kind);
				if (core >= 0 && built)
					++coreCounts[core];
				if (std::strcmp(kind, "refinery") == 0)
					ownForward.push_back({ i, at, "own-refinery", 0.0f });
				else if (std::strcmp(kind, "barracks") == 0)
					ownBarracks.push_back({ i, at, "own-rax", 0.0f });
			}
		}
		else if (bTeam == enemyTeam)
		{
			if (fair)
				intelPending.push_back({ e, i, at, EmpiresTeamIntel::CONTACT_BUILDING, DISPATCH_BUILDING, built, kind, false });
			else
			{
				if (!built)
					enemyUnfinished.push_back(at);
				if (std::strcmp(kind, "refinery") == 0)
					enemyTargets.push_back({ i, at, "enemy-refinery", 0.0f });
				else if (std::strcmp(kind, "barracks") == 0)
					enemyBarracks.push_back({ i, at, "enemy-rax", 0.0f });
			}
		}
	}
	std::vector<Vector> fairInfantry;
	if (fair)
	{
		// Cheap sources first: game spotting (players m_VisibilityState 3, the rest m_bSpotted) and radar.
		for (IntelPending& p : intelPending)
		{
			const float pos[3] = { p.at.x, p.at.y, p.at.z };
			int visibility = 0;
			bool spotted = false;
			EmpiresTeamIntel::IntelSource source = EmpiresTeamIntel::SOURCE_NONE;
			if ((readEmpiresEntityIntProperty(p.e, "m_VisibilityState", visibility) && visibility == 3) ||
				(readEmpiresEntityBoolProperty(p.e, "m_bSpotted", spotted) && spotted))
				source = EmpiresTeamIntel::SOURCE_SPOTTED;
			else
				for (const EmpiresTeamIntel::Sensor& sensor : sensors)
					if (sensor.isRadar && EmpiresTeamIntel::SensorInRange(sensor, pos))
					{
						source = EmpiresTeamIntel::SOURCE_RADAR;
						break;
					}
			if (source != EmpiresTeamIntel::SOURCE_NONE)
			{
				if (EmpiresTeamIntel::Upsert(intel, p.entity, p.e->m_NetworkSerialNumber, p.kind, source, pos, now))
					++state.intelSources[source];
			}
			else
				p.needsTrace = true;
		}
		// Line of sight from own cameras (1200) and living teammates (3500): at most
		// kMaxTraceBudgetPerTick traces per update, starting at a rotating candidate, first hit ends.
		const size_t n = intelPending.size();
		int traces = 0;
		size_t done = 0;
		const size_t start = n > 0 ? static_cast<size_t>(intel.traceCursor) % n : 0;
		for (; done < n && traces < EmpiresTeamIntel::kMaxTraceBudgetPerTick; ++done)
		{
			IntelPending& p = intelPending[(start + done) % n];
			if (!p.needsTrace)
				continue;
			const Vector target = p.at + Vector(0, 0, p.kind == EmpiresTeamIntel::CONTACT_INFANTRY ? 48.0f : 64.0f);
			EmpiresTeamIntel::IntelSource source = EmpiresTeamIntel::SOURCE_NONE;
			for (const EmpiresTeamIntel::Sensor& sensor : sensors)
			{
				const Vector eye(sensor.pos[0], sensor.pos[1], sensor.pos[2]);
				if (sensor.isRadar || (eye - target).Length() > EmpiresTeamIntel::kCameraRange)
					continue;
				if (traces >= EmpiresTeamIntel::kMaxTraceBudgetPerTick)
					break;
				++traces;
				if (CBotGlobals::isVisible(eye, target))
				{
					source = EmpiresTeamIntel::SOURCE_CAMERA;
					break;
				}
			}
			for (size_t k = 0; source == EmpiresTeamIntel::SOURCE_NONE && k < ownEyes.size(); ++k)
			{
				if ((ownEyes[k] - target).Length() > EmpiresTeamIntel::kSightRange)
					continue;
				if (traces >= EmpiresTeamIntel::kMaxTraceBudgetPerTick)
					break;
				++traces;
				if (CBotGlobals::isVisible(ownEyes[k], target))
					source = EmpiresTeamIntel::SOURCE_SIGHT;
			}
			if (source != EmpiresTeamIntel::SOURCE_NONE)
			{
				const float pos[3] = { p.at.x, p.at.y, p.at.z };
				if (EmpiresTeamIntel::Upsert(intel, p.entity, p.e->m_NetworkSerialNumber, p.kind, source, pos, now))
					++state.intelSources[source];
			}
		}
		intel.traceCursor = static_cast<int>((start + done) % (n > 0 ? n : 1));
		state.intelTraces += traces;
		// Hand the known ones (at the remembered position) to the director lists.
		for (const IntelPending& p : intelPending)
		{
			const EmpiresTeamIntel::Contact* c = EmpiresTeamIntel::FindContact(intel, p.entity,
				p.e->m_NetworkSerialNumber, now);
			if (c == nullptr)
			{
				++state.intelHidden;
				continue;
			}
			const Vector at(c->pos[0], c->pos[1], c->pos[2]);
			switch (p.dispatch)
			{
			case DISPATCH_CV:
				enemyCvKnown = true;
				enemyCv = at;
				enemyCvEntity = p.entity;
				break;
			case DISPATCH_VEHICLE:
				enemyVehicles.push_back(at);
				break;
			case DISPATCH_INFANTRY:
				fairInfantry.push_back(at);
				break;
			case DISPATCH_BUILDING:
				if (!p.built)
					enemyUnfinished.push_back(at);
				if (std::strcmp(p.buildingKind, "refinery") == 0)
					enemyTargets.push_back({ p.entity, at, "enemy-refinery", 0.0f });
				else if (std::strcmp(p.buildingKind, "barracks") == 0)
					enemyBarracks.push_back({ p.entity, at, "enemy-rax", 0.0f });
				break;
			}
		}
	}
	// Without a seen enemy CV the base anchor (round-start position, else last sighting) gives the
	// front direction and the last push goal, never a live position.
	bool enemyCvFromAnchor = false;
	if (fair && !enemyCvKnown)
	{
		float pos[3];
		bool live = false;
		if (EmpiresTeamIntel::EnemyBaseAnchor(intel, now, pos, live))
		{
			enemyCvKnown = true;
			enemyCvFromAnchor = true;
			enemyCv = Vector(pos[0], pos[1], pos[2]);
			enemyCvEntity = -1;
		}
	}
	for (const Candidate& rax : ownBarracks)
		if (!ownCvKnown || Dist2D(rax.spot, ownCv) > kForwardBarracks)
			ownForward.push_back(rax);
	for (const Candidate& rax : enemyBarracks)
		if (!enemyCvKnown || Dist2D(rax.spot, enemyCv) > kForwardBarracks)
			enemyTargets.push_back(rax);

	// --- Base / home.
	board.base = Goal();
	Vector home;
	bool homeKnown = false;
	if (ownCvKnown)
	{
		home = ownCv;
		homeKnown = true;
		// The nearest own spawn point is open ground; a spot in front of the CV ended at the barracks
		// (palmbay 20261121-153845: navmesh-stuck hit=emp_building_nf_barracks).
		Vector spot;
		int spawnId = -1;
		if ((findNearestEmpiresSpawn(team, ownCv, spawnId, spot) && Dist2D(spot, ownCv) < 2500.0f &&
				SnapToMesh(spot, 200.0f, spot)) ||
			ApproachSpot(ownCv, enemyCvKnown, enemyCv, 400.0f, spot))
			SetGoal(board.base, ownCvEntity, spot, "cv");
	}
	else if (!ownBarracks.empty())
	{
		home = ownBarracks.front().spot;
		homeKnown = true;
		Vector spot;
		if (ApproachSpot(home, enemyCvKnown, enemyCv, 300.0f, spot))
			SetGoal(board.base, ownBarracks.front().entity, spot, "rax");
	}

	// --- Base alert: damage, enemies or unfinished enemy buildings next to a watched own object.
	int teamBots = 0;
	std::vector<Vector> enemyInfantry;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		IPlayerInfo* info = e != nullptr && !e->IsFree() ? playerinfomanager->GetPlayerInfo(e) : nullptr;
		if (info == nullptr || !info->IsConnected())
			continue;
		if (info->GetTeamIndex() == team && info->IsFakeClient())
			++teamBots;
		if (!fair && info->GetTeamIndex() == enemyTeam && !info->IsDead() && !info->IsObserver() && info->GetHealth() > 0)
			enemyInfantry.push_back(CBotGlobals::entityOrigin(e));
	}
	if (fair)
		enemyInfantry = fairInfantry;
	const Watched* threat = nullptr;
	const char* threatReason = "";
	int threatScore = 0;
	for (const Watched& w : watched)
	{
		int score = 0;
		const char* reason = "";
		if (w.damaged)
		{
			score = 100;
			reason = "damage";
			const float pos[3] = { w.at.x, w.at.y, w.at.z };
			EmpiresTeamIntel::RecordOwnDamage(intel, w.entity, pos, std::strcmp(w.kind, "cv") == 0, now);
		}
		for (const Vector& v : enemyVehicles)
			if (Dist2D(v, w.at) < kEnemyVehicleRadius && score < 60)
			{
				score = 60;
				reason = "enemy-vehicle";
			}
		int near = 0;
		for (const Vector& p : enemyInfantry)
			if (Dist2D(p, w.at) < kEnemyInfantryRadius && std::fabs(p.z - w.at.z) < 600.0f)
				++near;
		if (near > 0 && score < 40 + near)
		{
			score = 40 + near;
			reason = "enemy-infantry";
		}
		for (const Vector& b : enemyUnfinished)
			if (Dist2D(b, w.at) < kEnemyBuildingRadius && score < 30)
			{
				score = 30;
				reason = "enemy-building";
			}
		if (score == 0)
			continue;
		score = score * 10 + w.weight;
		if (board.alert.valid() && w.entity == board.alert.entity)
		{
			state.alertScore = score;
			state.alertSeen = now;
		}
		if (score > threatScore)
		{
			threatScore = score;
			threat = &w;
			threatReason = reason;
		}
	}
	if (threat != nullptr)
	{
		Vector spot;
		// A new alert or a stronger one replaces the target; a weaker one only keeps the alert alive.
		const bool replace = AlertTakesOver(now <= state.alertUntil && board.alert.valid(),
			board.alert.entity == threat->entity, std::strcmp(threatReason, "damage") == 0, threatScore,
			state.alertScore, now - state.alertSeen);
		if (replace && ApproachSpot(threat->at, homeKnown, home, 300.0f, spot))
		{
			if (board.alert.valid() && board.alert.entity != threat->entity)
				RCBOT_DIAG("[RCBOT2] team-director team=%d alert-switch from=%s:%d to=%s:%d score=%d old=%d quiet=%.0f\n",
					team, board.alert.kind, board.alert.entity, threat->kind, threat->entity, threatScore,
					state.alertScore, now - state.alertSeen);
			SetGoal(board.alert, threat->entity, spot, threat->kind);
			board.alertReason = threatReason;
			state.alertScore = threatScore;
			state.alertSeen = now;
		}
		state.alertUntil = now + kAlertHold;
	}
	const bool alert = now <= state.alertUntil && board.alert.valid();
	if (!alert)
	{
		board.alert = Goal();
		board.alertReason = "";
	}

	// --- Stabilize: a finished core building lost, CV badly hurt, tickets low.
	for (int k = 0; k < 4; ++k)
	{
		static const char* const names[4] = { "lost-barracks", "lost-factory", "lost-refinery", "lost-radar" };
		if (state.coreCounts[k] >= 0 && coreCounts[k] < state.coreCounts[k])
		{
			state.stabilization.extend(now + kStabilizeHold, names[k]);
		}
		state.coreCounts[k] = coreCounts[k];
	}
	if (ownCvKnown && cvHealthShare < 0.35f)
	{
		state.stabilization.extend(now + kUpdateSeconds * 2.0f, "cv-health");
	}
	float tickets = 0.0f;
	board.ticketsKnown = ReadTeamTickets(this, team, tickets);
	if (board.ticketsKnown)
	{
		board.tickets = tickets;
		if (tickets > board.ticketsMax)
			board.ticketsMax = tickets;
		if (tickets <= kKillSpawnMinTickets || tickets <= board.ticketsMax * kKillSpawnMinTicketShare)
		{
			state.stabilization.extend(now + kUpdateSeconds * 2.0f, "tickets");
		}
	}
	board.phase = alert ? PHASE_BASE_ALERT : (now <= state.stabilization.until ? PHASE_STABILIZE : PHASE_PUSH);

	// --- Push goals: open / enemy flags nearest home; without flags enemy refineries, forward barracks
	// and free resource points; the enemy CV last.
	auto reachable = [&](const Vector& spot)
	{
		return !homeKnown || !RCBotNavMesh::IsLoaded() || !board.base.valid() ||
			RCBotNavMesh::CanReachComponent(GoalVector(board.base), spot);
	};
	std::vector<Candidate> push;
	std::vector<Candidate> hold;
	for (const CEmpiresMod::CapturePointObservation& point : mod->getCapturePoints())
	{
		if (point.entityIndex > 0 && point.teamOwnerKnown)
		{
			auto known = state.flagOwner.find(point.entityIndex);
			if (known != state.flagOwner.end() && known->second != point.teamOwner &&
				(point.teamOwner == team || known->second == team))
				RCBOT_DIAG("[RCBOT2] team-director team=%d flag-%s entity=%d owner=%d from=%d\n", team,
					point.teamOwner == team ? "gained" : "lost", point.entityIndex, point.teamOwner, known->second);
			state.flagOwner[point.entityIndex] = point.teamOwner;
		}
		if (!point.originKnown || point.entityIndex <= 0 || !EmpiresCapturePointOnMesh(point))
			continue;
		const Vector spot = point.groundApproachKnown ? point.groundApproachOrigin : point.origin;
		if (point.teamOwnerKnown && point.teamOwner == team)
		{
			if (reachable(spot))
				hold.push_back({ point.entityIndex, spot, "flag", 0.0f });
			continue;
		}
		const bool cappableKnown = team == EMP_TEAM_NF ? point.nfCappableKnown : point.beCappableKnown;
		const int cappable = team == EMP_TEAM_NF ? point.nfCappable : point.beCappable;
		if (cappableKnown && cappable == 0)
			continue;
		auto avoid = state.pushAvoid.find(point.entityIndex);
		if (avoid != state.pushAvoid.end() && now < avoid->second)
			continue;
		if (!reachable(spot))
			continue;
		push.push_back({ point.entityIndex, spot, "flag", homeKnown ? Dist2D(spot, home) : 0.0f });
	}
	if (push.empty())
	{
		// Without flags: the nearest enemy refinery / forward barracks and the free resource point nearest
		// the middle of the map, alternating (enemy first). Resource points on the own half are no push
		// goals (ResourcePushScore); without both CVs known the nearest ones as before.
		const bool front = homeKnown && enemyCvKnown;
		std::vector<Candidate> enemy, free;
		for (const Candidate& c : enemyTargets)
		{
			Vector spot;
			if (ApproachSpot(c.spot, homeKnown, home, 350.0f, spot) && reachable(spot))
				enemy.push_back({ c.entity, spot, c.kind, homeKnown ? Dist2D(spot, home) : 0.0f });
		}
		for (const Candidate& r : resources)
		{
			bool taken = false;
			for (const Vector& ref : refineries)
				if (Dist2D(ref, r.spot) < 300.0f)
					taken = true;
			const float score = front ? ResourcePushScore(Dist2D(r.spot, home), Dist2D(r.spot, enemyCv)) :
				(homeKnown ? Dist2D(r.spot, home) : 0.0f);
			Vector spot;
			if (!taken && score >= 0.0f && ApproachSpot(r.spot, homeKnown, home, 250.0f, spot) && reachable(spot))
				free.push_back({ r.entity, spot, "resource", score });
		}
		auto bySortKey = [](const Candidate& a, const Candidate& b) { return a.sortKey < b.sortKey; };
		std::sort(enemy.begin(), enemy.end(), bySortKey);
		std::sort(free.begin(), free.end(), bySortKey);
		for (size_t k = 0; k < enemy.size() || k < free.size(); ++k)
		{
			if (k < enemy.size())
				push.push_back({ enemy[k].entity, enemy[k].spot, enemy[k].kind, static_cast<float>(push.size()) });
			if (k < free.size())
				push.push_back({ free[k].entity, free[k].spot, free[k].kind, static_cast<float>(push.size()) });
		}
	}
	if (push.empty() && enemyCvKnown)
	{
		Vector spot;
		if (ApproachSpot(enemyCv, homeKnown, home, 450.0f, spot) && reachable(spot))
			push.push_back({ enemyCvEntity, spot, enemyCvFromAnchor ? "enemy-base" : "enemy-cv", 0.0f });
	}
	std::sort(push.begin(), push.end(), [](const Candidate& a, const Candidate& b) { return a.sortKey < b.sortKey; });
	board.pushCount = 0;
	for (const Candidate& c : push)
	{
		if (board.pushCount >= kMaxGoals)
			break;
		SetGoal(board.push[board.pushCount++], c.entity, c.spot, c.kind);
	}

	// --- Hold goals: own flags, refineries and forward barracks nearest the front.
	for (const Candidate& c : ownForward)
	{
		Vector spot;
		if (ApproachSpot(c.spot, homeKnown, home, 300.0f, spot) && reachable(spot))
			hold.push_back({ c.entity, spot, c.kind, 0.0f });
	}
	const bool frontKnown = enemyCvKnown || board.pushCount > 0;
	const Vector front = enemyCvKnown ? enemyCv : (board.pushCount > 0 ? GoalVector(board.push[0]) : Vector());
	for (Candidate& c : hold)
		c.sortKey = frontKnown ? Dist2D(c.spot, front) : (homeKnown ? -Dist2D(c.spot, home) : 0.0f);
	std::sort(hold.begin(), hold.end(), [](const Candidate& a, const Candidate& b) { return a.sortKey < b.sortKey; });
	board.holdCount = 0;
	for (const Candidate& c : hold)
	{
		if (board.holdCount >= kMaxGoals)
			break;
		// The base itself is the defenders' spot, not a hold goal.
		if (homeKnown && Dist2D(c.spot, home) < 1500.0f)
			continue;
		SetGoal(board.hold[board.holdCount++], c.entity, c.spot, c.kind);
	}

	// --- Roles of the free infantry bots (stable by client index).
	int clients[kMaxClients];
	int prev[kMaxClients];
	bool locked[kMaxClients];
	int count = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i < kMaxClients; ++i)
	{
		board.role[i] = ROLE_NONE;
		board.goal[i] = -1;
		edict_t* e = engine->PEntityOfEntIndex(i);
		IPlayerInfo* info = e != nullptr && !e->IsFree() ? playerinfomanager->GetPlayerInfo(e) : nullptr;
		if (info == nullptr || !info->IsConnected() || !info->IsFakeClient() || info->GetTeamIndex() != team ||
			EmpiresIsEngibot(e))
			continue;
		CBot* bot = CBots::getBotPointer(e);
		if (bot == nullptr)
			continue;
		const CBotEmpires* emp = static_cast<const CBotEmpires*>(bot);
		if (emp->m_iCommanderEntryStage != 0 || emp->m_iCommanderBuildStage != 0 ||
			emp->m_vehicle.stage != EmpiresVehicleStage::NONE ||
			EmpiresLawlietIsCrew(team, i))
			continue;
		int classId = -1;
		bool desired = false, desiredKnown = false;
		const char* classProperty = nullptr;
		if ((readEmpiresClass(e, classId, desired, desiredKnown, classProperty) && classId == EMP_CLASS_ENGINEER) ||
			emp->m_iDesiredClass == EMP_CLASS_ENGINEER)
			continue;
		if (!info->IsDead() && EmpiresVehiclePlayerVehicle(this, e) >= 0)
			continue;
		int order = 0;
		if (readEmpiresIntProperty(e, "m_iCommandType", order) && order != 0 && !EmpiresCommanderOrderIsOurs(team, i))
			continue; // a human (or anyone else) ordered this bot: hands off
		clients[count] = i;
		prev[count] = state.lastRole[i];
		locked[count] = state.lastRole[i] != ROLE_NONE && now - state.roleSince[i] < kRoleMinSeconds;
		++count;
	}
	board.teamBots = teamBots;
	// Alert at a hold goal (a forward own refinery): the hold bots answer it, no extra defenders are
	// pulled from the push (palmbay 164001: the only pusher became defender at every such alert).
	int alertHold = -1;
	if (board.phase == PHASE_BASE_ALERT)
		for (int g = 0; g < board.holdCount; ++g)
			if (board.hold[g].entity >= 0 && board.hold[g].entity == board.alert.entity)
				alertHold = g;
	const bool leanDefense = rcbot_empires_lean_defense.GetBool();
	board.split = SplitRoles(count, teamBots, board.phase, board.holdCount > 0, leanDefense);
	const RoleSplit calm = SplitRoles(count, teamBots, PHASE_PUSH, board.holdCount > 0, leanDefense);
	if (alertHold >= 0 && calm.hold > 0)
		board.split = calm;
	else
		alertHold = -1;
	const int alertOverride = board.split.defense - calm.defense;
	if (board.pushCount == 0)
	{
		// Nothing to push: pushers hold, or defend without hold goals.
		if (board.holdCount > 0)
			board.split.hold += board.split.push;
		else
			board.split.defense += board.split.push;
		board.split.push = 0;
	}
	int roles[kMaxClients];
	AssignRoles(prev, locked, count, board.split, roles);
	int pushIndex = 0, holdIndex = 0;
	for (int k = 0; k < count; ++k)
	{
		const int i = clients[k];
		board.role[i] = roles[k];
		if (roles[k] == ROLE_PUSH)
			board.goal[i] = board.pushCount > 0 ? pushIndex++ % board.pushCount : -1;
		else if (roles[k] == ROLE_HOLD)
			board.goal[i] = alertHold >= 0 ? alertHold : (board.holdCount > 0 ? holdIndex++ % board.holdCount : -1);
		if (roles[k] != state.lastRole[i])
		{
			const Goal* goal = RoleGoal(board, i);
			RCBOT_DIAG("[RCBOT2] team-director role team=%d userid=%d role=%s from=%s goal=%s:%d t=%.0f held=%.0f\n", team,
				engine->GetPlayerUserId(engine->PEntityOfEntIndex(i)), RoleName(roles[k]), RoleName(state.lastRole[i]),
				goal != nullptr ? goal->kind : "-", goal != nullptr ? goal->entity : -1, now,
				state.lastRole[i] != ROLE_NONE ? now - state.roleSince[i] : 0.0f);
			state.roleSince[i] = now;
		}
	}
	// Keep a stall timer only while a living attacker is assigned to this flag.
	for (const auto& point : mod->getCapturePoints())
	{
		float nearest = -1.0f;
		for (int k = 0; k < count; ++k)
		{
			const int client = clients[k];
			const Goal* goal = RoleGoal(board, client);
			if (roles[k] != ROLE_PUSH || goal == nullptr || goal->entity != point.entityIndex)
				continue;
			edict_t* player = engine->PEntityOfEntIndex(client);
			if (!CBotGlobals::entityIsAlive(player))
				continue;
			const float distance = (CBotGlobals::entityOrigin(player) - GoalVector(*goal)).Length();
			if (nearest < 0.0f || distance < nearest)
				nearest = distance;
		}
		if (nearest < 0.0f)
		{
			state.flagProgress.erase(point.entityIndex);
			continue;
		}
		const bool occupied = team == EMP_TEAM_NF ? !point.nfOccupantUserIds.empty() : !point.beOccupantUserIds.empty();
		if (state.flagProgress[point.entityIndex].stalled(now, nearest, occupied))
		{
			state.pushAvoid[point.entityIndex] = now + kFlagAvoid;
			state.flagProgress.erase(point.entityIndex);
			RCBOT_DIAG("[RCBOT2] team-director team=%d push-avoid entity=%d seconds=%.0f reason=no-progress\n",
				team, point.entityIndex, kFlagAvoid);
		}
	}
	for (int i = 1; i < kMaxClients; ++i)
		state.lastRole[i] = board.role[i];
	board.updatedAt = now;

	// --- Log on change of phase / goals / split, else every kLogEvery.
	char sig[256];
	auto goalText = [](const Goal* goals, int n, char* out, size_t size)
	{
		out[0] = '\0';
		size_t used = 0;
		for (int g = 0; g < n && used + 1 < size; ++g)
			used += snprintf(out + used, size - used, "%s%s:%d", g > 0 ? "," : "", goals[g].kind, goals[g].entity);
		if (n == 0)
			snprintf(out, size, "-");
	};
	char pushText[96], holdText[96];
	goalText(board.push, board.pushCount, pushText, sizeof(pushText));
	goalText(board.hold, board.holdCount, holdText, sizeof(holdText));
	snprintf(sig, sizeof(sig), "%s|%s|%s|%d|%d/%d/%d", PhaseName(board.phase), pushText, holdText, board.alert.entity,
		board.split.push, board.split.hold, board.split.defense);
	if (state.lastSignature != sig || now >= state.nextLog)
	{
		state.lastSignature = sig;
		state.nextLog = now + kLogEvery;
		RCBOT_DIAG("[RCBOT2] team-director team=%d phase=%s push=%s hold=%s alert=%s:%d reason=%s override=%d alert-hold=%d stabilize=%s roles=%d/%d/%d free=%d bots=%d tickets=%.0f/%.0f known=%d\n",
			team, PhaseName(board.phase), pushText, holdText, board.alert.kind, board.alert.entity, board.alertReason,
			alertOverride, alertHold,
			board.phase == PHASE_STABILIZE ? state.stabilization.reason : "-", board.split.push, board.split.hold,
			board.split.defense, count, teamBots, board.tickets, board.ticketsMax, board.ticketsKnown ? 1 : 0);
		if (fair)
		{
			// sources since the last line: report=bot sight report, then IntelSource 1..4
			RCBOT_DIAG("[RCBOT2] team-intel team=%d contacts=%d inf=%d veh=%d bld=%d cv=%d report=%d radar=%d camera=%d "
				"spotted=%d sight=%d traces=%d hidden=%d cv_from=%s\n", team,
				EmpiresTeamIntel::CountActive(intel, 0u, now),
				EmpiresTeamIntel::CountActive(intel, EmpiresTeamIntel::KindBit(EmpiresTeamIntel::CONTACT_INFANTRY), now),
				EmpiresTeamIntel::CountActive(intel, EmpiresTeamIntel::KindBit(EmpiresTeamIntel::CONTACT_VEHICLE) |
					EmpiresTeamIntel::KindBit(EmpiresTeamIntel::CONTACT_APC), now),
				EmpiresTeamIntel::CountActive(intel, EmpiresTeamIntel::KindBit(EmpiresTeamIntel::CONTACT_BUILDING), now),
				EmpiresTeamIntel::CountActive(intel, EmpiresTeamIntel::KindBit(EmpiresTeamIntel::CONTACT_ENEMY_CV), now),
				state.intelSources[0], state.intelSources[1], state.intelSources[2], state.intelSources[3],
				state.intelSources[4], state.intelTraces, state.intelHidden, enemyCvFromAnchor ? "anchor" : "seen");
			for (int& c : state.intelSources)
				c = 0;
			state.intelTraces = 0;
			state.intelHidden = 0;
		}
	}
}

// Nearest enabled own spawn point to `to` (same entities and enabled flag as resolveBootstrapSpawnId).
bool CBotEmpires::findNearestEmpiresSpawn(int team, const Vector& to, int& spawnId, Vector& at) const
{
	spawnId = -1;
	if (servertools == nullptr)
		return false;
	const char* classname = team == EMP_TEAM_NF ? "emp_info_player_nf" : "emp_info_player_imp";
	// APC spawns put the bot in a passenger seat, not near the goal (expert review 21.11., palmbay 161706:
	// kill-spawn vf-buyer spawned in APC 229, exit blocked, never reached the factory).
	std::vector<Vector> apcs;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr || std::strcmp(e->GetClassName(), "emp_vehicle") != 0)
			continue;
		int vTeam = -1, chassis = -1;
		if (readEmpiresEntityIntProperty(e, "m_iTeamNum", vTeam) && vTeam == team &&
			readEmpiresEntityIntProperty(e, "m_iChassis", chassis) && (chassis == 1 || chassis == 9) &&
			EmpiresVehicleAlive(this, e))
			apcs.push_back(CBotGlobals::entityOrigin(e));
	}
	float best = 0.0f;
	for (CBaseEntity* entity = servertools->FirstEntity(); entity != nullptr; entity = servertools->NextEntity(entity))
	{
		IServerUnknown* unknown = reinterpret_cast<IServerUnknown*>(entity);
		IServerNetworkable* net = unknown != nullptr ? unknown->GetNetworkable() : nullptr;
		edict_t* e = net != nullptr ? net->GetEdict() : nullptr;
		const char* cls = net != nullptr ? net->GetClassName() : nullptr;
		// Barracks spawn points use NF/Imp, while APC spawns use lowercase classnames.
		if (e == nullptr || e->IsFree() || cls == nullptr || V_stricmp(cls, classname) != 0)
			continue;
		char raw[32] = {};
		if (!servertools->GetKeyValue(entity, "pointNumber", raw, sizeof(raw)) || raw[0] < '0' || raw[0] > '9')
			continue;
		if (EmpiresBinaryOffsetsTrusted() && *(reinterpret_cast<const unsigned char*>(entity) + 0x354) == 0)
			continue; // CEmpSpawn::SetEnabled 0x67fd70
		const Vector pos = CBotGlobals::entityOrigin(e);
		bool apcSpawn = false;
		for (const Vector& apc : apcs)
			apcSpawn = apcSpawn || Dist2D(pos, apc) < 350.0f;
		if (apcSpawn)
			continue;
		const float d = Dist2D(pos, to);
		if (spawnId < 0 || d < best)
		{
			spawnId = std::atoi(raw);
			at = pos;
			best = d;
		}
	}
	return spawnId >= 0;
}

// Kill-spawn (TODO 5.28): a defender on a base alert or a vehicle buyer far from the factory respawns
// at a spawn much closer to the goal, only when every rule in KillSpawnBlock passes.
void CBotEmpires::tickEmpiresKillSpawn()
{
	if (gpGlobals == nullptr || engine == nullptr || helpers == nullptr || m_pEdict == nullptr || m_pEdict->IsFree())
		return;
	const float now = gpGlobals->curtime;
	if (now < m_flKillSpawnNextCheck)
		return;
	m_flKillSpawnNextCheck = now + 1.0f;
	if (!rcbot_empires_kill_spawn.GetBool())
		return;
	const int team = getTeam();
	TeamState* state = team == EMP_TEAM_NF || team == EMP_TEAM_BE ? &s_team[team] : nullptr;
	const Board* board = EmpiresTeamDirectorBoard(team);
	if (state == nullptr || board == nullptr)
		return;
	const int client = engine->IndexOfEdict(m_pEdict);
	if (client <= 0 || client >= kMaxClients)
		return;
	if (m_bKillSpawnMissed)
	{
		// The pre-selected spawn was not usable at the respawn: no further kill-spawns for this bot.
		m_bKillSpawnMissed = false;
		m_flKillSpawnLast = now + 600.0f;
		RCBOT_DIAG("[RCBOT2] kill-spawn missed userid=%d cooldown=690\n", engine->GetPlayerUserId(m_pEdict));
	}

	// Occasion and goal.
	const char* reason = nullptr;
	Vector goal;
	if (board->phase == PHASE_BASE_ALERT && board->role[client] == ROLE_DEFENSE && board->alert.valid())
	{
		reason = "base-alert";
		goal = GoalVector(board->alert);
	}
	else if (m_vehicle.stage == EmpiresVehicleStage::GOTO_FACTORY)
	{
		edict_t* factory = nullptr;
		int factoryIndex = -1;
		if (findEmpiresVehicleFactory(team, factory, goal, factoryIndex))
			reason = "vf-buyer";
	}
	else if (EmpiresLawlietBoardingGoal(team, client, goal))
		reason = "lawliet-board";
	if (reason == nullptr)
		return;

	KillSpawnInput in;
	in.enabled = true;
	in.alive = isAlive() && m_pPlayerInfo != nullptr && !m_pPlayerInfo->IsDead();
	in.onFoot = EmpiresVehiclePlayerVehicle(this, m_pEdict) < 0 && m_iCommanderEntryStage == 0 &&
		!commanderVehicleEntryActive();
	in.busy = m_iBuildAssignStage != 0 || m_iReviveTarget != 0 || m_iCommanderBuildStage != 0 ||
		m_hEmpiresBuildingActionTarget.get() != nullptr;
	int order = 0;
	in.foreignOrder = readEmpiresIntProperty(m_pEdict, "m_iCommandType", order) && order != 0 &&
		!EmpiresCommanderOrderIsOurs(team, client);
	const float real = engine->Time();
	const float lastCombat = (std::max)(m_fLastHurtTime, m_fLastSeeEnemy);
	in.sinceCombat = lastCombat > 0.0f ? real - lastCombat : 1.0e9f;
	int spawnId = -1;
	Vector spawnAt;
	if (!findNearestEmpiresSpawn(team, goal, spawnId, spawnAt))
		return;
	const Vector origin = getOrigin();
	in.savedDistance = Dist2D(origin, goal) - Dist2D(spawnAt, goal);
	in.ticketsKnown = board->ticketsKnown;
	in.tickets = board->tickets;
	in.ticketsMax = board->ticketsMax;
	in.sinceOwnKillSpawn = now - m_flKillSpawnLast;
	in.sinceTeamKillSpawn = now - state->board.lastKillSpawn;
	const char* block = KillSpawnBlock(in);
	if (block != nullptr)
	{
		// Only the interesting refusals, once per 20 s per bot: a goal far enough away.
		if (in.savedDistance >= kKillSpawnMinSaved && now >= m_flKillSpawnNextBlockLog)
		{
			m_flKillSpawnNextBlockLog = now + 20.0f;
			RCBOT_DIAG("[RCBOT2] kill-spawn skip userid=%d reason=%s block=%s saved_dist=%.0f\n",
				engine->GetPlayerUserId(m_pEdict), reason, block, in.savedDistance);
		}
		return;
	}
	m_flKillSpawnLast = now;
	state->board.lastKillSpawn = now;
	m_iKillSpawnId = spawnId;
	m_flKillSpawnIdUntil = now + 30.0f;
	RCBOT_DIAG("[RCBOT2] kill-spawn userid=%d team=%d reason=%s spawn_id=%d saved_dist=%.0f goal=(%.0f %.0f %.0f) tickets=%.0f\n",
		engine->GetPlayerUserId(m_pEdict), team, reason, spawnId, in.savedDistance, goal.x, goal.y, goal.z, in.tickets);
	helpers->ClientCommand(m_pEdict, "kill\n");
}
