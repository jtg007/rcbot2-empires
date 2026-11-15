// Automatic Commander: takes the Commander seat, builds the base with emp_build and hands
// unfinished buildings to Engineers. Policy (what, in which order) is in
// bot_empires_commander_economy.h; the rules the game enforces are in
// reverse-engineering/notes/buildings.md ("Commander building via `emp_build`").
#include "bot_empires.h"
#include "bot_empires_binary.h"
#include "bot_empires_team_economy.h"
#include "in_buttons.h"
#include "bot_buttons.h"
#include "bot_cvars.h"
#include "bot_getprop.h"
#include "bot_globals.h"
#include "bot_empires_native_placement.h"

#include <cmath>
#include "bot_empires_spatial.h"
#include "bot_navmesh_api.h"
#include "engine/IEngineTrace.h"

#include "dt_send.h"
#include "server_class.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern IVEngineServer* engine;
extern IServerPluginHelpers* helpers;

int EmpiresVehiclePlayerVehicle(const CBotEmpires* bot, const edict_t* player);

namespace
{
using namespace EmpiresCommanderEconomy;

constexpr float kTickSeconds = 1.0f;
// A human who sits in the CV or presses use next to it gets the seat: the bot leaves and
// no bot takes the seat again until this long after the last such sign.
constexpr float kHumanSeatHoldSeconds = 60.0f;
constexpr float kHumanUseRadius = 300.0f;
float s_humanCommanderUntil[4] = {};
bool s_testYieldRequest[4] = {};
constexpr float kVerifyDelay = 0.3f;
constexpr float kPointBlockSeconds = 15.0f;
constexpr float kNearVehicleRange = 3800.0f; // CanBuildAt allows 4000, keep a margin
constexpr float kHullClearance = 250.0f;     // a player inside the refinery hull gets it rejected
constexpr float kSearchRadius = 2048.0f;
constexpr float kShiftedSearchOffset = 1500.0f;
constexpr float kInvalidSkipRadius = 250.0f;
constexpr float kEscortSeconds = 120.0f; // one escort at a time, then pick again
constexpr int kMaxAssignTries = 6;
// Commander Engineer leaves the CV only for buildings this close to it, with no enemy
// near the building and no other Engineer within the helper range.
// Base zone: from the CV start out to the nearest own refinery + margin (user: not
// further than about the first refinery), clamped.
constexpr float kCommanderZoneMin = 4000.0f; // run 155132: 3023 lost prey standing at the base edge (4031)
constexpr float kCommanderZoneMax = 6000.0f;
constexpr float kCommanderZoneMargin = 1500.0f;
// Farther than rcbot_empires_commander_drive_from: drive the CV first, stop this far
// before the building (at most half the way).
constexpr float kCommanderDriveStop = 700.0f;
constexpr float kCommanderWalkRange = 2500.0f; // after a stuck drive, walk only this far
// Pull the drive goal back towards the CV until it is clear of buildings: the goal used to
// lie in the BE barracks footprint (run 093847), the vehicle route skips detours around a
// building near its end, so the CV drove onto the barracks and the commander got out into
// a niche it could not leave for ~10 minutes.
bool CommanderDriveGoal(const Vector& at, const Vector& back, float cvDistance, Vector& result)
{
	// Leave room for the vehicle's arrival radius before the walk-range limit.
	const float maxStop = (std::min)(cvDistance, kCommanderWalkRange - 400.0f);
	const float firstStop = (std::min)(kCommanderDriveStop, cvDistance * 0.5f);
	const float angles[] = {0.0f, 0.5235988f, -0.5235988f, 1.0471976f, -1.0471976f,
		1.5707963f, -1.5707963f, 2.0943951f, -2.0943951f, 2.6179939f, -2.6179939f, 3.1415927f};
	for (float angle : angles)
	{
		const float c = std::cos(angle), s = std::sin(angle);
		const Vector direction(back.x * c - back.y * s, back.x * s + back.y * c, 0.0f);
		for (float stop = firstStop; stop <= maxStop; stop += 64.0f)
		{
			const Vector goal = at + direction * stop;
			if (!RCBotNavMesh::PointNearBuilding(goal, 150.0f))
			{
				RCBOT_DIAG("[RCBOT2] commander-drive-goal cv_distance=%.0f stop=%.0f angle=%.2f goal=(%.0f %.0f %.0f)\n",
					cvDistance, stop, angle, goal.x, goal.y, goal.z);
				result = goal;
				return true;
			}
		}
	}
	RCBOT_DIAG("[RCBOT2] commander-drive-goal none cv_distance=%.0f\n", cvDistance);
	return false;
}
constexpr float kCommanderDriveSeconds = 90.0f;
constexpr float kCommanderHuntSeconds = 120.0f;
constexpr float kCommanderHuntReach = 3000.0f; // the vehicle brain picks infantry up to 3000 away
Vector s_cvHome[4];
int s_cvHomeVehicle[4] = {}; // CV entity the home belongs to (new map, new CV)
constexpr float kCommanderBuildEnemyRange = 2000.0f;
constexpr float kCommanderBuildHelperRange = 2500.0f;

struct BuildingInfo
{
	int entity = 0;
	int type = -1;
	int health = 0;
	int maxHealth = 0;
};

const char* RawEntity(edict_t* e)
{
	// Only used for fixed-offset reads (+0x118, +0x3a8, +0x3b4, +0x4a8, +0x4b0).
	if (!EmpiresBinaryOffsetsTrusted())
		return nullptr;
	IServerUnknown* unknown = e != nullptr ? e->GetUnknown() : nullptr;
	return reinterpret_cast<const char*>(unknown != nullptr ? unknown->GetBaseEntity() : nullptr);
}

// Rejected buildings stay in the edict list until the end of the frame with EFL_KILLME set.
bool MarkedForDeletion(edict_t* e)
{
	const char* raw = RawEntity(e);
	if (!EmpiresBinaryOffsetsTrusted())
		return false; // cannot tell: mark nothing
	return raw == nullptr || (*reinterpret_cast<const int*>(raw + 0x118) & 0x1) != 0;
}

bool IsClass(edict_t* e, const char* prefix)
{
	return e != nullptr && !e->IsFree() && e->GetClassName() != nullptr &&
		std::strncmp(e->GetClassName(), prefix, std::strlen(prefix)) == 0;
}

// Exact class: IsClass matches a prefix, and emp_resource_point_prop (the model)
// has no m_iResIndex at +0x4b0 (read 1065353216 = 1.0f, emp_build rejected).
bool IsResourcePoint(edict_t* e)
{
	return e != nullptr && !e->IsFree() && e->GetClassName() != nullptr &&
		std::strcmp(e->GetClassName(), "emp_resource_point") == 0;
}

int BuildingType(const char* className)
{
	// emp_building_vehiclefactory_panel, *_physbox and engineer-built emp_building_*_eng_* are no base buildings.
	if (std::strstr(className, "_eng") != nullptr || std::strstr(className, "panel") != nullptr ||
		std::strstr(className, "physbox") != nullptr)
		return -1;
	if (std::strstr(className, "refinery") != nullptr) return Refinery;
	if (std::strstr(className, "barracks") != nullptr) return Barracks;
	if (std::strstr(className, "vehiclefactory") != nullptr) return VehicleFactory;
	if (std::strstr(className, "armory") != nullptr) return Armory;
	if (std::strstr(className, "repairstation") != nullptr) return Repair;
	if (std::strstr(className, "radar") != nullptr) return Radar;
	return -1;
}

// Vehicle factory exit: vehicles leave along the building's local +X (ramp
// side; reverse-engineering/notes/vehicle-driving.md section 9), so the yaw of
// emp_build is the world direction to the enemy command vehicle. The game checks
// an unrotated box only; the ramp side reaches 920 (NF) / 1200 (BE), so the exit
// lane must have ground and no wall, else turn in 45-degree steps.
bool ExitLaneClear(const Vector& at, float yaw)
{
	if (enginetrace == nullptr)
		return true;
	const float r = yaw * 3.14159265f / 180.0f;
	const Vector dir(std::cos(r), std::sin(r), 0.0f);
	CTraceFilterWorldOnly filter;
	trace_t hit;
	Ray_t ray;
	ray.Init(at + Vector(0, 0, 60), at + Vector(0, 0, 60) + dir * 1500.0f);
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &hit);
	if (hit.fraction < 1.0f)
		return false;
	for (float d = 600.0f; d <= 1500.0f; d += 300.0f)
	{
		const Vector p = at + dir * d;
		ray.Init(p + Vector(0, 0, 80), p - Vector(0, 0, 300));
		enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &hit);
		if (hit.fraction >= 1.0f)
			return false;
	}
	return true;
}

Vector EnemyCommanderOrigin(const Vector& at, int team)
{
	const char* enemyClass = team == 2 ? "emp_imp_commander" : "emp_nf_commander";
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (IsClass(e, enemyClass))
			return CBotGlobals::entityOrigin(e);
	}
	return at + Vector(1, 0, 0);
}

// Heading from a building spot toward the front: the vehicle route to the enemy
// CV, direction to the route point ~1500 out (the first leg, not the straight
// line, which on bent maps points into the base wall). Straight map without
// route: line to the enemy CV. Else false.
bool FrontYawUncached(const Vector& at, int team, float& yaw, const char*& source);

// The route search costs 10-20 ms and the refinery-turret search asks every 2 s per
// engineer from inside the refinery (run 20261112-170244: 873 detour no-start lines in
// 8 min). Same spot (64 grid) and team within 60 s: reuse the answer.
bool FrontYaw(const Vector& at, int team, float& yaw, const char*& source)
{
	struct Cached { int x, y, team; float until, yaw; const char* source; bool ok; };
	static std::vector<Cached> cache;
	const float now = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	const int cx = int(std::floor(at.x / 64.0f)), cy = int(std::floor(at.y / 64.0f));
	for (Cached& c : cache)
		if (c.x == cx && c.y == cy && c.team == team && now < c.until && now >= c.until - 60.0f)
		{
			yaw = c.yaw;
			source = c.source;
			return c.ok;
		}
	const bool ok = FrontYawUncached(at, team, yaw, source);
	cache.erase(std::remove_if(cache.begin(), cache.end(), [&](const Cached& c) {
		return now >= c.until || now < c.until - 60.0f || (c.x == cx && c.y == cy && c.team == team); }), cache.end());
	cache.push_back({cx, cy, team, now + 60.0f, yaw, source, ok});
	return ok;
}

bool FrontYawUncached(const Vector& at, int team, float& yaw, const char*& source)
{
	const Vector enemy = EnemyCommanderOrigin(at, team);
	if (RCBotNavMesh::IsLoaded())
	{
		RCBotNavMesh::Path route;
		if (RCBotNavMesh::BuildVehiclePath(at, enemy, route, 150.0f) == RCBotNavMesh::PathResult::SUCCESS &&
			!route.points.empty())
		{
			Vector prev = at, aim = route.points.back();
			float walked = 0.0f;
			for (const Vector& p : route.points)
			{
				walked += (p - prev).Length2D();
				prev = p;
				if (walked >= 1500.0f)
				{
					aim = p;
					break;
				}
			}
			if ((aim - at).Length2D() > 100.0f)
			{
				yaw = std::atan2(aim.y - at.y, aim.x - at.x) * 180.0f / 3.14159265f;
				source = "route";
				return true;
			}
		}
	}
	const char* map = gpGlobals != nullptr ? STRING(gpGlobals->mapname) : nullptr;
	if (map != nullptr && std::strstr(map, "rainbowroad") != nullptr)
	{
		yaw = std::atan2(enemy.y - at.y, enemy.x - at.x) * 180.0f / 3.14159265f;
		source = "enemy-cv";
		return true;
	}
	return false;
}

float FactoryExitYaw(const Vector& at, int team, bool quiet = false)
{
	float toFront = 0.0f;
	const char* source = "none";
	FrontYaw(at, team, toFront, source);
	static const float kTurns[] = {0, 45, -45, 90, -90, 135, -135, 180};
	for (const float turn : kTurns)
		if (ExitLaneClear(at, toFront + turn))
		{
			if (!quiet)
				RCBOT_DIAG("[RCBOT2] commander-econ factory-yaw front=%.0f src=%s yaw=%.0f\n", toFront, source, toFront + turn);
			return toFront + turn;
		}
	if (!quiet)
		RCBOT_DIAG("[RCBOT2] commander-econ factory-yaw front=%.0f src=%s no-clear-lane\n", toFront, source);
	return toFront;
}

// Repair pad: drive-through along its local Y (both ends open, half length 850,
// run 20261112-120808 side1/side3). Free length of the axis: a vehicle hull from
// the centre out to 1150 on both ends (world and buildings), the shorter end
// counts, 0 without ground under an end. 1150 on both = fully clear.
class PadTraceFilter : public CTraceFilter
{
public:
	bool ShouldHitEntity(IHandleEntity* handle, int) override
	{
		IServerUnknown* unknown = static_cast<IServerUnknown*>(handle);
		IServerNetworkable* networkable = unknown != nullptr ? unknown->GetNetworkable() : nullptr;
		edict_t* edict = networkable != nullptr ? networkable->GetEdict() : nullptr;
		if (edict == nullptr || gpGlobals == nullptr)
			return true;
		const int index = engine->IndexOfEdict(edict);
		return index <= 0 || index > gpGlobals->maxClients;
	}
};

float PadAxisFree(const Vector& at, float axisYaw)
{
	if (enginetrace == nullptr)
		return 1150.0f;
	const float r = axisYaw * 3.14159265f / 180.0f;
	const Vector dir(std::cos(r), std::sin(r), 0.0f);
	PadTraceFilter filter;
	trace_t hit;
	Ray_t ray;
	float free = 1150.0f;
	for (int end = -1; end <= 1; end += 2)
	{
		const Vector outside = at + dir * (1150.0f * end) + Vector(0, 0, 40);
		ray.Init(at + Vector(0, 0, 40), outside, Vector(-60, -60, 0), Vector(60, 60, 60));
		enginetrace->TraceRay(ray, MASK_SOLID, &filter, &hit);
		if (hit.startsolid)
			return 0.0f;
		free = (std::min)(free, 1150.0f * hit.fraction);
		ray.Init(outside + Vector(0, 0, 40), outside - Vector(0, 0, 300));
		enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &hit);
		if (hit.fraction >= 1.0f)
			free = (std::min)(free, 600.0f);
	}
	return free;
}

float RepairPadYaw(const Vector& at, int team, bool quiet = false, float* outTurn = nullptr,
	float* outFree = nullptr)
{
	float toFront = 0.0f;
	const char* source = "none";
	FrontYaw(at, team, toFront, source);
	// The axis is symmetric, so turns up to 90 cover all directions; first fully
	// clear turn wins, else the one with the longest free axis.
	static const float kTurns[] = {0, 30, -30, 60, -60, 90};
	float bestTurn = 0.0f, bestFree = -1.0f;
	for (const float turn : kTurns)
	{
		const float free = PadAxisFree(at, toFront + turn);
		if (free > bestFree + 1.0f)
		{
			bestFree = free;
			bestTurn = turn;
		}
		if (free >= 1150.0f)
			break;
	}
	if (outTurn != nullptr)
		*outTurn = bestTurn;
	if (outFree != nullptr)
		*outFree = bestFree;
	if (!quiet)
		RCBOT_DIAG("[RCBOT2] commander-econ pad-yaw front=%.0f src=%s axis=%.0f yaw=%.0f free=%.0f\n", toFront, source,
		toFront + bestTurn, toFront + bestTurn - 90.0f, bestFree);
	return toFront + bestTurn - 90.0f;
}

// Drive lanes of buildings already standing near the CV: both ends of each
// repair pad (local +-Y, 600 beyond the pad) and the exit ramp of each vehicle
// factory (local +X). A new building must keep them free (run
// 20261112-125413: the factory was placed on the pad's open entrance and the
// tank climbed onto it).
struct Lane { Vector a, b; };

std::vector<Lane> ReservedLanes(const Vector& cv)
{
	std::vector<Lane> lanes;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr)
			continue;
		const char* cls = e->GetClassName();
		const bool pad = std::strstr(cls, "repairstation") != nullptr;
		const bool factory = std::strstr(cls, "vehiclefactory") != nullptr && std::strstr(cls, "panel") == nullptr &&
			std::strstr(cls, "physbox") == nullptr;
		ICollideable* c = e->GetCollideable();
		if ((!pad && !factory) || c == nullptr)
			continue;
		const Vector centre = CBotGlobals::entityOrigin(e);
		if ((centre - cv).Length2D() > 5000.0f)
			continue;
		const float r = c->GetCollisionAngles().y * 3.14159265f / 180.0f;
		const Vector x(std::cos(r), std::sin(r), 0.0f), y(-std::sin(r), std::cos(r), 0.0f);
		const Vector mins = c->OBBMins(), maxs = c->OBBMaxs();
		if (pad)
		{
			lanes.push_back({centre + y * maxs.y, centre + y * (maxs.y + 600.0f)});
			lanes.push_back({centre + y * mins.y, centre + y * (mins.y - 600.0f)});
		}
		else
			lanes.push_back({centre + x * maxs.x, centre + x * (maxs.x + 600.0f)});
	}
	return lanes;
}

// Footprint (local OBB) of a new building. Measured run 20261112-130602 (NF):
// factory -414..920 x +-828, pad +-495 x +-850, refinery -207..162 x -247..220,
// armory -203..250 x +-200; BE factory ramp to 1200 (RE note). Barracks and
// radar: copy from one standing on the map, else an estimate.
void Footprint(int type, int team, Vector& mins, Vector& maxs)
{
	switch (type)
	{
	case VehicleFactory:
		mins = Vector(team == 3 ? -748.0f : -414.0f, -828.0f, 0.0f);
		maxs = Vector(team == 3 ? 1200.0f : 920.0f, 828.0f, 0.0f);
		return;
	case Repair: mins = Vector(-495, -850, 0); maxs = Vector(495, 850, 0); return;
	case Refinery: mins = Vector(-207, -247, 0); maxs = Vector(162, 220, 0); return;
	// BE armory measured in run 20261112-163322 (-576..576 x -245..234): with the NF size
	// it was placed in front of the BE pad's only open end.
	case Armory:
		if (team == 3)
		{
			mins = Vector(-576, -245, 0);
			maxs = Vector(576, 234, 0);
		}
		else
		{
			mins = Vector(-203, -200, 0);
			maxs = Vector(250, 200, 0);
		}
		return;
	default: break;
	}
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr || BuildingType(e->GetClassName()) != type)
			continue;
		ICollideable* c = e->GetCollideable();
		if (c != nullptr)
		{
			mins = c->OBBMins();
			maxs = c->OBBMaxs();
			return;
		}
	}
	const float h = type == Barracks ? 400.0f : 250.0f;
	mins = Vector(-h, -h, 0);
	maxs = Vector(h, h, 0);
}

// True if a lane runs through the building box (rotated by yaw, grown by 100
// for half a vehicle); lanes sampled every 50.
bool BlocksLane(const Vector& at, float yaw, const Vector& mins, const Vector& maxs, const std::vector<Lane>& lanes)
{
	const float r = yaw * 3.14159265f / 180.0f;
	const float c = std::cos(r), sn = std::sin(r);
	for (const Lane& lane : lanes)
	{
		const float len = (lane.b - lane.a).Length2D();
		const int steps = (std::max)(1, int(len / 50.0f));
		for (int k = 0; k <= steps; ++k)
		{
			const Vector p = lane.a + (lane.b - lane.a) * (float(k) / steps);
			const float dx = p.x - at.x, dy = p.y - at.y;
			const float lx = dx * c + dy * sn, ly = -dx * sn + dy * c;
			if (lx > mins.x - 100.0f && lx < maxs.x + 100.0f && ly > mins.y - 100.0f && ly < maxs.y + 100.0f)
				return true;
		}
	}
	return false;
}

// CEmpTeam +0x3b4 -> ledger, +0x30 float (emp_give_resources, server_srv.so 0x647610).
bool ReadTeamResources(int team, float& resources)
{
	for (int i = 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetNetworkable() == nullptr)
			continue;
		ServerClass* serverClass = e->GetNetworkable()->GetServerClass();
		if (serverClass == nullptr || std::strcmp(serverClass->GetName(), "CEmpTeam") != 0)
			continue;
		const char* raw = RawEntity(e);
		if (raw == nullptr || *reinterpret_cast<const int*>(raw + 0x3a8) != team)
			continue;
		const char* ledger = *reinterpret_cast<const char* const*>(raw + 0x3b4);
		if (ledger == nullptr)
			return false;
		resources = *reinterpret_cast<const float*>(ledger + 0x30);
		return resources >= 0.0f && resources < 1.0e7f;
	}
	return false;
}

// Research state of a team (server_srv.so, ResearchStart 0x682280 / ResearchUpdate 0x682be0):
// CEmpTeam +0x3d0 int active item (0 = none), +0x3d4 int time remaining, +0x3d8 byte research
// building built, +0x3d9 byte paused, +0x3e4 + item index: byte researched.
struct TeamResearch
{
	int active = 0;
	int remaining = 0;
	bool building = false;
	bool paused = false;
	bool done[EmpiresCommanderEconomy::kResearchItemCount] = {};
};

bool ReadTeamResearch(int team, TeamResearch& out)
{
	for (int i = 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetNetworkable() == nullptr)
			continue;
		ServerClass* serverClass = e->GetNetworkable()->GetServerClass();
		if (serverClass == nullptr || std::strcmp(serverClass->GetName(), "CEmpTeam") != 0)
			continue;
		const char* raw = RawEntity(e);
		if (raw == nullptr || *reinterpret_cast<const int*>(raw + 0x3a8) != team)
			continue;
		out.active = *reinterpret_cast<const int*>(raw + 0x3d0);
		out.remaining = *reinterpret_cast<const int*>(raw + 0x3d4);
		out.building = *reinterpret_cast<const unsigned char*>(raw + 0x3d8) != 0;
		out.paused = *reinterpret_cast<const unsigned char*>(raw + 0x3d9) != 0;
		for (int idx = 0; idx < EmpiresCommanderEconomy::kResearchItemCount; ++idx)
			out.done[idx] = *reinterpret_cast<const unsigned char*>(raw + 0x3e4 + idx) != 0;
		return true;
	}
	return false;
}
}

bool CBotEmpires::isEmpiresEngineerClass() const
{
	int classId = -1;
	bool desired = false, desiredKnown = false;
	const char* propertyName = nullptr;
	return readEmpiresClass(m_pEdict, classId, desired, desiredKnown, propertyName) && classId == EMP_CLASS_ENGINEER;
}

// The bot that should take the empty commander seat next: the team's first Engineer
// (lowest userid) when rcbot_empires_commander_engineer is on, else the lowest-slot living
// non-Engineer. nullptr while somebody of the team holds, approaches or is out of the seat.
CBotEmpires* CBotEmpires::commanderSeatCandidate(int team)
{
	CBotEmpires* chosen = nullptr;
	CBotEmpires* engineer = nullptr;
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* bot = CBots::getBot(slot);
		if (bot == nullptr || !bot->inUse() || bot->getEdict() == nullptr || !bot->isAlive() ||
			CClassInterface::getTeam(bot->getEdict()) != team)
			continue;
		if (EmpiresIsEngibot(bot->getEdict()))
			continue; // bought by the commander to stay at the base
		CBotEmpires* candidate = static_cast<CBotEmpires*>(bot);
		if (candidate->m_iCommanderEntryStage != 0 || candidate->m_iCommanderBuildStage != 0)
			return nullptr;
		if (chosen == nullptr && !candidate->isEmpiresEngineerClass())
			chosen = candidate;
		if (candidate->isEmpiresEngineerClass() && (engineer == nullptr ||
			engine->GetPlayerUserId(candidate->getEdict()) < engine->GetPlayerUserId(engineer->getEdict())))
			engineer = candidate;
	}
	if (rcbot_empires_commander_engineer.GetBool() && engineer != nullptr)
		chosen = engineer;
	return chosen;
}

// Run 121241 (production values): both seat candidates spent 20 min on refinery turrets
// (15 failed) and NF never got a commander (4 seat attempts lost to turret restarts).
bool CBotEmpires::commanderDutyBlocksSideTasks()
{
	// Stage 5 is the commander's own turret trip (out of the seat): the turret job must run.
	if (m_iCommanderBuildStage == 5 && m_bCommanderBuildTurret && m_iCommanderEntryStage == 0)
		return false;
	if (m_iCommanderEntryStage != 0 || m_iCommanderBuildStage != 0)
		return true;
	if (!rcbot_empires_auto_commander.GetBool() || gpGlobals == nullptr || engine == nullptr ||
		isControlledTestOnly())
		return false;
	const int team = getTeam();
	if ((team != 2 && team != 3) || engine->Time() < s_humanCommanderUntil[team] ||
		m_commanderEconomy.entryAttempts >= 4)
		return false;
	return commanderSeatCandidate(team) == this;
}

void CBotEmpires::tickCommanderEconomy()
{
	if (!rcbot_empires_auto_commander.GetBool() || engine == nullptr || gpGlobals == nullptr ||
		helpers == nullptr || !isAlive() || isControlledTestOnly())
		return;
	const float now = engine->Time();
	CommanderEconomyState& state = m_commanderEconomy;
	if (now < state.nextTick)
		return;
	state.nextTick = now + kTickSeconds;
	const int team = getTeam();
	if (team != 2 && team != 3)
		return;

	// --- A human wants the seat: leave it to him. ---
	edict_t* teamVehicle = nullptr;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && teamVehicle == nullptr; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (IsClass(e, team == 2 ? "emp_nf_commander" : "emp_imp_commander"))
			teamVehicle = e;
	}
	const char* humanReason = nullptr;
	if (s_testYieldRequest[team])
		humanReason = "test";
	for (int i = 1; teamVehicle != nullptr && humanReason == nullptr && i <= gpGlobals->maxClients; ++i)
	{
		edict_t* player = engine->PEntityOfEntIndex(i);
		IPlayerInfo* info = player != nullptr && !player->IsFree() && playerinfomanager != nullptr ?
			playerinfomanager->GetPlayerInfo(player) : nullptr;
		if (info == nullptr || !info->IsConnected() || info->IsFakeClient() || info->IsDead() ||
			CClassInterface::getTeam(player) != team)
			continue;
		// In the Commander view the player is not in the vehicle (m_hVehicle 4095); the
		// office is m_bCommander (player+0x1438, reverse-engineering/notes/commander.md).
		int isCommander = 0;
		int inVehicle = -1;
		if (readEmpiresEntityIntProperty(player, "m_bCommander", isCommander) && isCommander != 0)
			humanReason = "human-commander";
		else if (readEmpiresWeaponHandleProperty(player, "m_hVehicle", inVehicle) &&
			inVehicle == engine->IndexOfEdict(teamVehicle))
			humanReason = "human-inside";
		else if ((info->GetLastUserCommand().buttons & IN_USE) != 0 &&
			(CBotGlobals::entityOrigin(player) - CBotGlobals::entityOrigin(teamVehicle)).Length() < kHumanUseRadius)
			humanReason = "human-use";
	}
	if (humanReason != nullptr)
		s_humanCommanderUntil[team] = now + kHumanSeatHoldSeconds;
	if (now < s_humanCommanderUntil[team] && m_iCommanderEntryStage != 0)
	{
		int seatVehicle = -1;
		readEmpiresWeaponHandleProperty(m_pEdict, "m_hVehicle", seatVehicle);
		if (m_iCommanderEntryStage == 4)
		{
			releaseCommanderOrders(team, "human-seat");
			// Commander view: the player is outside the CV (m_hVehicle invalid). emp_comm_exit only puts him
			// back into the driver seat (still commander). Use from the driver seat leaves the CV, and
			// CEmpTank::ExitVehicle(role 0) clears the commander state. LEFT_SEAT resets the stage.
			if (seatVehicle == engine->IndexOfEdict(teamVehicle))
				m_pButtons->tap(IN_USE);
			else
				helpers->ClientCommand(m_pEdict, "emp_comm_exit\n");
		}
		else
		{
			m_iCommanderEntryStage = 0;
			cancelNavigationAttempt("commander-seat-yield");
			m_pButtons->letGo(IN_USE);
		}
		s_testYieldRequest[team] = false;
		EmpiresNativePlacement::ContextIdentity yieldContext;
		bool yieldCommander = false;
		const bool captured = EmpiresNativePlacement::CaptureContext(engine->IndexOfEdict(m_pEdict), yieldContext);
		const bool modeRead = captured && EmpiresNativePlacement::ReadCommanderMode(yieldContext, yieldCommander);
		int commanderProp = -1; // same reader as the human check above
		readEmpiresEntityIntProperty(m_pEdict, "m_bCommander", commanderProp);
		RCBOT_DIAG("[RCBOT2] commander-econ yield userid=%d team=%d stage=%d mode_read=%d commander=%d m_bCommander=%d vehicle=%d reason=%s\n",
			engine->GetPlayerUserId(m_pEdict), team, m_iCommanderEntryStage, modeRead ? 1 : 0,
			yieldCommander ? 1 : 0, commanderProp, seatVehicle,
			humanReason != nullptr ? humanReason : "hold");
		return;
	}

	// --- Commander Engineer out of the CV to build (user 2026-11-12). ---
	if (m_iCommanderBuildStage == 1)
	{
		edict_t* target = m_hCommanderBuildTarget.get();
		if (m_iCommanderEntryStage == 4)
		{
			if (now > m_fCommanderBuildDeadline || target == nullptr)
			{
				RCBOT_DIAG("[RCBOT2] commander-build result=ABORT reason=%s userid=%d\n",
					target == nullptr ? "target-gone" : "exit-deadline", engine->GetPlayerUserId(m_pEdict));
				m_iCommanderBuildStage = 0;
				return;
			}
			// Same leave sequence as the yield above: comm_exit to the driver seat, then use.
			int seatVehicle = -1;
			readEmpiresWeaponHandleProperty(m_pEdict, "m_hVehicle", seatVehicle);
			edict_t* cv = m_hCommanderEntryVehicle.get();
			if (cv != nullptr && seatVehicle == engine->IndexOfEdict(cv))
				m_pButtons->tap(IN_USE);
			else
				helpers->ClientCommand(m_pEdict, "emp_comm_exit\n");
			return;
		}
		if (m_iCommanderEntryStage != 0)
			return;
		if (m_bCommanderBuildTurret)
		{
			// The refinery-turret job (bot_empires_turret_build.cpp) runs once out of the seat.
			RCBOT_DIAG("[RCBOT2] commander-build stage=turret userid=%d refinery=%d\n", engine->GetPlayerUserId(m_pEdict),
				target != nullptr ? engine->IndexOfEdict(target) : -1);
			m_iCommanderBuildStage = 5;
			m_bCommanderTurretStarted = false;
			m_fCommanderBuildDeadline = now + 15.0f;
			state.nextEntryTry = 0.0f;
			state.entryAttempts = 0;
			return;
		}
		const bool requested = target != nullptr && requestEmpiresBuildAssignment(target);
		RCBOT_DIAG("[RCBOT2] commander-build stage=build userid=%d building=%d requested=%d\n",
			engine->GetPlayerUserId(m_pEdict), target != nullptr ? engine->IndexOfEdict(target) : -1, requested ? 1 : 0);
		m_iCommanderBuildStage = requested ? 2 : 0;
		state.nextEntryTry = 0.0f;
		state.entryAttempts = 0;
		return;
	}
	if (m_iCommanderBuildStage == 3 || m_iCommanderBuildStage == 4)
	{
		state.nextTick = now + 0.1f; // seat change and drive end react at once
		edict_t* target = m_hCommanderBuildTarget.get();
		edict_t* cv = m_hCommanderEntryVehicle.get();
		const int userId = engine->GetPlayerUserId(m_pEdict);
		if (m_bCommanderHunt)
			target = cv; // no building; the CV stands in for the checks below
		if (m_iCommanderEntryStage != 4 || cv == nullptr || target == nullptr)
		{
			m_bCommanderHunt = false;
			RCBOT_DIAG("[RCBOT2] commander-build result=ABORT reason=%s userid=%d\n",
				target == nullptr ? "target-gone" : "seat-lost", userId);
			if (m_vehicle.commanderCV)
				resetEmpiresVehicle("commander-build-abort", 0.0f);
			m_iCommanderBuildStage = 0;
			return;
		}
		int seatVehicle = -1;
		readEmpiresWeaponHandleProperty(m_pEdict, "m_hVehicle", seatVehicle);
		const bool inDriverSeat = seatVehicle == engine->IndexOfEdict(cv);
		if (m_iCommanderBuildStage == 3)
		{
			if (!inDriverSeat)
			{
				if (now > m_fCommanderBuildDeadline)
				{
					RCBOT_DIAG("[RCBOT2] commander-build result=ABORT reason=driver-seat-deadline userid=%d\n", userId);
					m_iCommanderBuildStage = 0;
				}
				else
					helpers->ClientCommand(m_pEdict, "emp_comm_exit\n"); // to the driver seat, still commander
				return;
			}
			if (startEmpiresCommanderDrive(cv, m_vCommanderBuildDriveGoal))
			{
				m_iCommanderBuildStage = 4;
				m_fCommanderBuildDeadline = now + (m_bCommanderHunt ? kCommanderHuntSeconds : kCommanderDriveSeconds);
				if (m_bCommanderHunt)
				{
					m_vehicle.cvHunt = true;
					m_vehicle.cvHome = s_cvHome[team];
					m_vehicle.cvZone = m_fCommanderHuntZone;
				}
			}
			return;
		}
		const bool driving = m_vehicle.stage == EmpiresVehicleStage::DRIVING && m_vehicle.commanderCV;
		if (driving && !m_vehicle.cvArrived && now <= m_fCommanderBuildDeadline)
			return;
		const float distance = (CBotGlobals::entityOrigin(target) - CBotGlobals::entityOrigin(cv)).Length();
		const char* end = !driving ? "drive-lost" : now > m_fCommanderBuildDeadline ? "deadline" :
			m_vehicle.cvEnd != nullptr ? m_vehicle.cvEnd : "?";
		if (m_bCommanderHunt)
		{
			RCBOT_DIAG("[RCBOT2] commander-hunt end userid=%d reason=%s targets=%d\n", userId, end, m_vehicle.cvKillsTried);
			resetEmpiresVehicle("commander-hunt-end", 0.0f);
			helpers->ClientCommand(m_pEdict, "emp_change_seat 1\n"); // back to the commander view
			m_bCommanderHunt = false;
			m_iCommanderBuildStage = 0;
			state.nextHuntCheck = now + 5.0f;
			return;
		}
		resetEmpiresVehicle("commander-drive-end", 0.0f);
		if (distance < kCommanderWalkRange)
		{
			RCBOT_DIAG("[RCBOT2] commander-build stage=exit-after-drive userid=%d end=%s building_distance=%.0f\n",
				userId, end, distance);
			m_iCommanderBuildStage = 1;
			m_fCommanderBuildDeadline = now + 10.0f;
		}
		else
		{
			RCBOT_DIAG("[RCBOT2] commander-build result=ABORT reason=drive-%s userid=%d building_distance=%.0f\n",
				end, userId, distance);
			helpers->ClientCommand(m_pEdict, "emp_change_seat 1\n"); // back to the commander view
			m_iCommanderBuildStage = 0;
		}
		return;
	}
	if (m_iCommanderBuildStage == 5)
	{
		if (m_iTurretRefinery != 0)
			m_bCommanderTurretStarted = true;
		const bool noStart = !m_bCommanderTurretStarted && now > m_fCommanderBuildDeadline;
		if (m_iTurretRefinery != 0 || m_iBuildAssignStage != 0 || (!m_bCommanderTurretStarted && !noStart))
			return;
		RCBOT_DIAG("[RCBOT2] commander-build result=%s userid=%d turret=1\n", noStart ? "NO_START" : "DONE",
			engine->GetPlayerUserId(m_pEdict));
		m_iCommanderBuildStage = 0;
	}
	if (m_iCommanderBuildStage == 2)
	{
		if (m_iBuildAssignStage != 0)
			return;
		RCBOT_DIAG("[RCBOT2] commander-build result=DONE userid=%d\n", engine->GetPlayerUserId(m_pEdict));
		m_iCommanderBuildStage = 0;
	}

	// --- Seat: the team's first Engineer bot (Repair role, lowest userid) when
	// rcbot_empires_commander_engineer is on, else the lowest-slot living non-Engineer. ---
	if (m_iCommanderEntryStage == 0)
	{
		if (now < s_humanCommanderUntil[team])
			return;
		if (state.entryAttempts >= 4 || now < state.nextEntryTry)
			return;
		if (commanderSeatCandidate(team) != this)
			return;
		// A started build, turret or revive finishes first.
		if (m_iBuildAssignStage != 0 || m_iTurretRefinery != 0 || m_iReviveTarget != 0)
			return;
		state.nextEntryTry = now + 20.0f;
		if (requestCommanderVehicleEntryOnce())
		{
			++state.entryAttempts;
			RCBOT_DIAG("[RCBOT2] commander-econ seat attempt=%d team=%d userid=%d\n", state.entryAttempts, team,
				engine->GetPlayerUserId(m_pEdict));
		}
		return;
	}
	if (m_iCommanderEntryStage != 4)
		return;
	state.entryAttempts = 0;

	edict_t* vehicle = m_hCommanderEntryVehicle.get();
	if (vehicle == nullptr)
		return;
	const Vector vehicleOrigin = CBotGlobals::entityOrigin(vehicle);

	// --- Inventory of own buildings. ---
	std::vector<BuildingInfo> buildings;
	std::vector<int> allBuildings;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (!IsClass(e, "emp_building_") || MarkedForDeletion(e))
			continue;
		allBuildings.push_back(i);
		if (CClassInterface::getTeam(e) != team)
			continue;
		BuildingInfo info;
		info.entity = i;
		info.type = BuildingType(e->GetClassName());
		if (info.type < 0 || !readEmpiresEntityIntProperty(e, "m_iHealth", info.health) ||
			!readEmpiresEntityIntProperty(e, "m_iMaxHealth", info.maxHealth))
			continue;
		buildings.push_back(info);
	}
	Inventory inventory;
	int unfinished = 0;
	for (const BuildingInfo& info : buildings)
	{
		++inventory.count[info.type];
		if (info.health < info.maxHealth)
			++unfinished;
	}

	// --- Engineers: living ones, and assignment of unfinished or damaged buildings. ---
	int livingEngineers = 0;
	std::vector<CBotEmpires*> freeEngineers;
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* bot = CBots::getBot(slot);
		if (bot == nullptr || bot == this || !bot->inUse() || bot->getEdict() == nullptr || !bot->isAlive() ||
			CClassInterface::getTeam(bot->getEdict()) != team)
			continue;
		CBotEmpires* engineer = static_cast<CBotEmpires*>(bot);
		if (!engineer->isEmpiresEngineerClass())
			continue;
		++livingEngineers;
		if (engineer->m_iBuildAssignStage == 0 && engineer->m_iTurretRefinery == 0) // a turret job finishes first
			freeEngineers.push_back(engineer);
	}
	const bool commanderEngineer = rcbot_empires_commander_engineer.GetBool() && isEmpiresEngineerClass();
	if (s_cvHomeVehicle[team] != engine->IndexOfEdict(vehicle))
	{
		s_cvHome[team] = vehicleOrigin;
		s_cvHomeVehicle[team] = engine->IndexOfEdict(vehicle);
	}
	const Vector home = s_cvHome[team];
	float zone = kCommanderZoneMax;
	{
		float nearestRefinery = -1.0f;
		for (const BuildingInfo& info : buildings)
			if (info.type == Refinery)
			{
				const float d = (CBotGlobals::entityOrigin(engine->PEntityOfEntIndex(info.entity)) - home).Length();
				if (nearestRefinery < 0.0f || d < nearestRefinery)
					nearestRefinery = d;
			}
		if (nearestRefinery >= 0.0f)
			zone = (std::max)(kCommanderZoneMin, (std::min)(kCommanderZoneMax, nearestRefinery + kCommanderZoneMargin));
	}

	// --- Boomtanking (user 2026-11-12): enemy infantry inside the base zone -> drive the CV
	// over them, then back here. Before any building work: the base comes first. ---
	// Boomtanking: 1 = on except on rainbowroad (user: the CV would fall off the road), 2 = everywhere.
	const char* huntMap = gpGlobals != nullptr ? STRING(gpGlobals->mapname) : nullptr;
	const bool boomtankHere = rcbot_empires_commander_boomtank.GetInt() >= 2 ||
		(rcbot_empires_commander_boomtank.GetInt() == 1 &&
			(huntMap == nullptr || std::strstr(huntMap, "rainbowroad") == nullptr));
	if (commanderEngineer && boomtankHere && now >= state.nextHuntCheck)
	{
		state.nextHuntCheck = now + 1.0f;
		const int enemyTeam = team == 2 ? 3 : 2;
		int prey = -1;
		float preyDistance = kCommanderHuntReach;
		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			edict_t* player = engine->PEntityOfEntIndex(i);
			if (player == nullptr || player->IsFree() || CClassInterface::getTeam(player) != enemyTeam ||
				!CBotGlobals::entityIsAlive(player) || EmpiresVehiclePlayerVehicle(this, player) >= 0)
				continue;
			const Vector at = CBotGlobals::entityOrigin(player);
			const float d = (at - vehicleOrigin).Length2D();
			if ((at - home).Length2D() < zone && std::fabs(at.z - vehicleOrigin.z) < 160.0f && d < preyDistance)
			{
				prey = i;
				preyDistance = d;
			}
		}
		if (prey > 0)
		{
			m_bCommanderHunt = true;
			m_bCommanderBuildTurret = false;
			m_hCommanderBuildTarget = nullptr;
			m_fCommanderHuntZone = zone;
			m_vCommanderBuildDriveGoal = vehicleOrigin; // come back to where the CV stood
			m_iCommanderBuildStage = 3;
			m_fCommanderBuildDeadline = now + 10.0f;
			state.nextTick = now + 0.1f;
			RCBOT_DIAG("[RCBOT2] commander-hunt start userid=%d prey=%d distance=%.0f zone=%.0f\n",
				engine->GetPlayerUserId(m_pEdict), prey, preyDistance, zone);
			return;
		}
	}
	for (const BuildingInfo& info : buildings)
	{
		if (info.health >= info.maxHealth || (freeEngineers.empty() && !commanderEngineer))
			continue;
		edict_t* building = engine->PEntityOfEntIndex(info.entity);
		bool taken = false;
		for (int slot = 0; slot < RCBOT_MAXPLAYERS && !taken; ++slot)
		{
			CBot* bot = CBots::getBot(slot);
			taken = bot != nullptr && bot != this && bot->inUse() &&
				static_cast<CBotEmpires*>(bot)->m_iBuildAssignStage != 0 &&
				static_cast<CBotEmpires*>(bot)->m_hBuildAssignBuilding.get() == building;
		}
		if (taken)
			continue;
		CommanderEconomyState::AssignTry* tries = nullptr;
		for (auto& entry : state.assignTries)
			if (entry.entity == info.entity)
				tries = &entry;
		if (tries == nullptr)
		{
			state.assignTries.push_back({info.entity, 0, 0.0f});
			tries = &state.assignTries.back();
		}
		if (tries->tries >= kMaxAssignTries || now < tries->next)
			continue;
		// The Commander Engineer builds it himself when it is in the safe zone around the CV,
		// no enemy is near it and no other Engineer is close and not fighting (user 2026-11-12).
		// Not for a scratch (run 100232: out of the CV for barracks 242/250 and armory 117/125,
		// minutes to get back in, the vehicle factory never came); free Engineers still take those.
		const bool scratch = info.health * 4 >= info.maxHealth * 3;
		if (scratch && freeEngineers.empty())
			continue;
		if (commanderEngineer && !scratch)
		{
			const Vector at = CBotGlobals::entityOrigin(building);
			const float cvDistance = (at - vehicleOrigin).Length();
			int helper = -1;
			bool enemyNear = false;
			for (int slot = 0; slot < RCBOT_MAXPLAYERS && helper < 0; ++slot)
			{
				CBot* bot = CBots::getBot(slot);
				if (bot == nullptr || bot == this || !bot->inUse() || bot->getEdict() == nullptr || !bot->isAlive() ||
					CClassInterface::getTeam(bot->getEdict()) != team || !static_cast<CBotEmpires*>(bot)->isEmpiresEngineerClass())
					continue;
				if (bot->distanceFrom(at) < kCommanderBuildHelperRange && !bot->hasEnemy())
					helper = engine->GetPlayerUserId(bot->getEdict());
			}
			for (int i = 1; i <= gpGlobals->maxClients && !enemyNear; ++i)
			{
				edict_t* player = engine->PEntityOfEntIndex(i);
				const int playerTeam = player != nullptr && !player->IsFree() ? CClassInterface::getTeam(player) : 0;
				enemyNear = (playerTeam == 2 || playerTeam == 3) && playerTeam != team &&
					CBotGlobals::entityIsAlive(player) &&
					(CBotGlobals::entityOrigin(player) - at).Length() < kCommanderBuildEnemyRange;
			}
			const float homeDistance = (at - home).Length();
			if (helper < 0 && !enemyNear && homeDistance < zone)
			{
				bool drive = cvDistance > rcbot_empires_commander_drive_from.GetFloat();
				Vector back = vehicleOrigin - at;
				back.z = 0.0f;
				back = back.Length() > 1.0f ? back / back.Length() : Vector(1, 0, 0);
				Vector goal;
				const bool goalFound = !drive || CommanderDriveGoal(at, back, cvDistance, goal);
				if (!goalFound && cvDistance <= kCommanderWalkRange)
					drive = false;
				if (!goalFound && drive && freeEngineers.empty())
				{
					tries->next = now + 10.0f;
					continue;
				}
				if (goalFound || !drive)
				{
					++tries->tries;
					tries->next = now + 10.0f;
					m_iCommanderBuildStage = drive ? 3 : 1;
					m_hCommanderBuildTarget = building;
					m_bCommanderBuildTurret = false;
					m_bCommanderHunt = false;
					m_fCommanderBuildDeadline = now + 10.0f;
					if (drive)
						m_vCommanderBuildDriveGoal = goal;
					RCBOT_DIAG("[RCBOT2] commander-build stage=%s userid=%d building=%d class=%s health=%d/%d cv_distance=%.0f "
						"home_distance=%.0f zone=%.0f try=%d\n", drive ? "drive" : "exit",
						engine->GetPlayerUserId(m_pEdict), info.entity, building->GetClassName(), info.health,
						info.maxHealth, cvDistance, homeDistance, zone, tries->tries);
					return;
				}
			}
			if (freeEngineers.empty())
			{
				if (helper < 0 && state.lastSelfBuildSkip != info.entity)
				{
					state.lastSelfBuildSkip = info.entity;
					RCBOT_DIAG("[RCBOT2] commander-build skip building=%d reason=%s home_distance=%.0f zone=%.0f\n", info.entity,
						enemyNear ? "enemy-near" : "outside-zone", homeDistance, zone);
				}
				continue;
			}
		}
		std::sort(freeEngineers.begin(), freeEngineers.end(), [&](CBotEmpires* a, CBotEmpires* b)
		{
			return a->distanceFrom(building) < b->distanceFrom(building);
		});
		++tries->tries;
		tries->next = now + 10.0f;
		const bool assigned = freeEngineers.front()->requestEmpiresBuildAssignment(building);
		RCBOT_DIAG("[RCBOT2] commander-econ assign building=%d class=%s health=%d/%d engineer_userid=%d requested=%d try=%d\n",
			info.entity, building->GetClassName(), info.health, info.maxHealth,
			engine->GetPlayerUserId(freeEngineers.front()->getEdict()), assigned ? 1 : 0, tries->tries);
		if (assigned)
			freeEngineers.erase(freeEngineers.begin());
		break; // one assignment per tick
	}

	// --- Commander Engineer: MG turret at an own refinery in the base zone (user 2026-11-12). ---
	if (commanderEngineer && rcbot_empires_engineer_turrets.GetBool() && unfinished == 0 && now >= state.nextTurretCheck)
	{
		state.nextTurretCheck = now + 5.0f;
		int built = -1, allotted = -1;
		const bool limit = readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerTurretsBuilt", built) &&
			readEmpiresDebugIntProperty(m_pEdict, "m_iEngineerTurretsAllotted", allotted) && allotted >= 0 &&
			built >= allotted;
		std::vector<Vector> turrets;
		for (int i : allBuildings)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			const char* cls = e != nullptr ? e->GetClassName() : nullptr;
			if (cls != nullptr && std::strstr(cls, "turret") != nullptr)
				turrets.push_back(CBotGlobals::entityOrigin(e));
		}
		for (const BuildingInfo& info : buildings)
		{
			if (limit || info.type != Refinery || info.health < info.maxHealth)
				continue;
			edict_t* refinery = engine->PEntityOfEntIndex(info.entity);
			const Vector at = CBotGlobals::entityOrigin(refinery);
			bool covered = false;
			for (const Vector& t : turrets)
				covered = covered || (t - at).Length2D() < 830.0f; // turret code: 500 from the spot 330 out
			bool enemyNear = false;
			for (int i = 1; i <= gpGlobals->maxClients && !enemyNear; ++i)
			{
				edict_t* player = engine->PEntityOfEntIndex(i);
				const int playerTeam = player != nullptr && !player->IsFree() ? CClassInterface::getTeam(player) : 0;
				enemyNear = (playerTeam == 2 || playerTeam == 3) && playerTeam != team &&
					CBotGlobals::entityIsAlive(player) && (CBotGlobals::entityOrigin(player) - at).Length() < kCommanderBuildEnemyRange;
			}
			if (covered || enemyNear || (at - home).Length() >= zone)
				continue;
			CommanderEconomyState::AssignTry* tries = nullptr;
			for (auto& entry : state.assignTries)
				if (entry.entity == info.entity)
					tries = &entry;
			if (tries == nullptr)
			{
				state.assignTries.push_back({info.entity, 0, 0.0f});
				tries = &state.assignTries.back();
			}
			if (tries->tries >= kMaxAssignTries || now < tries->next)
				continue;
			const float cvDistance = (at - vehicleOrigin).Length();
			bool drive = cvDistance > rcbot_empires_commander_drive_from.GetFloat();
			Vector back = vehicleOrigin - at;
			back.z = 0.0f;
			back = back.Length() > 1.0f ? back / back.Length() : Vector(1, 0, 0);
			Vector goal;
			if (drive && !CommanderDriveGoal(at, back, cvDistance, goal))
			{
				if (cvDistance > kCommanderWalkRange)
					continue;
				drive = false;
			}
			++tries->tries;
			tries->next = now + 30.0f;
			m_iCommanderBuildStage = drive ? 3 : 1;
			m_hCommanderBuildTarget = refinery;
			m_bCommanderBuildTurret = true;
			m_bCommanderHunt = false;
			m_fCommanderBuildDeadline = now + 10.0f;
			if (drive)
				m_vCommanderBuildDriveGoal = goal;
			RCBOT_DIAG("[RCBOT2] commander-build stage=%s userid=%d refinery=%d turret=1 cv_distance=%.0f home_distance=%.0f "
				"zone=%.0f try=%d\n", drive ? "drive" : "exit", engine->GetPlayerUserId(m_pEdict), info.entity,
				cvDistance, (at - home).Length(), zone, tries->tries);
			return;
		}
	}

	float resources = 0.0f;
	if (!ReadTeamResources(team, resources))
		return;

	// --- Research (TODO 5.1): the game only lets a team build a vehicle factory after the
	// research building (radar) stands and Structural_Engineering + Vehicle_Factory are researched.
	// With emp_sv_research_complete 1 (test server) everything is unlocked and this is skipped. ---
	// Before the build waits below: an unfinished building or a pending emp_build check must not
	// stop research (run 20261114-2040: one start, then never reached again).
	bool researchUnlocked = true;
	{
		ConVar* completeVar = g_pCVar != nullptr ? g_pCVar->FindVar("emp_sv_research_complete") : nullptr;
		TeamResearch research;
		if ((completeVar == nullptr || !completeVar->GetBool()) && EmpiresBinaryOffsetsTrusted() &&
			ReadTeamResearch(team, research))
		{
			researchUnlocked = research.done[kResearchVehicleFactory];
			const int next = research.done[kResearchStructural] ? kResearchVehicleFactory : kResearchStructural;
			if (!researchUnlocked && research.building && (research.active == 0 || research.paused) && now >= state.nextResearch)
			{
				EmpiresNativePlacement::ContextIdentity researchContext;
				bool researchCommander = false;
				if (EmpiresNativePlacement::CaptureContext(engine->IndexOfEdict(m_pEdict), researchContext) &&
					EmpiresNativePlacement::ReadCommanderMode(researchContext, researchCommander) && researchCommander)
				{
					// A research paused for lack of money is never resumed by the game itself
					// (ResearchResume 0x682d16 only after the radar returns, or by this command).
					if (research.active != 0 && research.paused)
					{
						helpers->ClientCommand(m_pEdict, "emp_comm_research_resume\n");
						RCBOT_DIAG("[RCBOT2] commander-econ research resume item=%d resources=%.0f\n", research.active, resources);
					}
					else
					{
						char researchCommand[48];
						snprintf(researchCommand, sizeof(researchCommand), "emp_comm_research %d\n", next);
						helpers->ClientCommand(m_pEdict, researchCommand);
						RCBOT_DIAG("[RCBOT2] commander-econ research start item=%d resources=%.0f\n", next, resources);
					}
					state.nextResearch = now + 10.0f;
				}
			}
			if (research.active != state.lastResearchActive || research.paused != state.lastResearchPaused ||
				now >= state.nextResearchLog)
			{
				state.nextResearchLog = now + 30.0f;
				state.lastResearchActive = research.active;
				state.lastResearchPaused = research.paused;
				RCBOT_DIAG("[RCBOT2] commander-econ research state active=%d remaining=%d paused=%d building=%d structural=%d factory=%d\n",
					research.active, research.remaining, int(research.paused), int(research.building),
					int(research.done[kResearchStructural]), int(research.done[kResearchVehicleFactory]));
			}
		}
	}

	// --- Orders to the team's bots (TODO 5.4). ---
	tickCommanderOrders(team, now, home);

	// --- Result of the last emp_build. ---
	if (state.verifyPending)
	{
		EmpiresTeamEconomy::KeepReserve(EmpiresTeamEconomy::Team(team), gpGlobals->curtime);
		if (now < state.verifyAt)
			return;
		state.verifyPending = false;
		int created = -1;
		for (const BuildingInfo& info : buildings)
			if (info.type == state.verifyType &&
				std::find(state.verifyBefore.begin(), state.verifyBefore.end(), info.entity) == state.verifyBefore.end())
				created = info.entity;
		if (created > 0)
		{
			state.failures[state.verifyType] = 0;
			edict_t* createdEdict = engine->PEntityOfEntIndex(created);
			const Vector at = CBotGlobals::entityOrigin(createdEdict);
			ICollideable* box = createdEdict != nullptr ? createdEdict->GetCollideable() : nullptr;
			const Vector mins = box != nullptr ? box->OBBMins() : Vector(0, 0, 0);
			const Vector maxs = box != nullptr ? box->OBBMaxs() : Vector(0, 0, 0);
			RCBOT_DIAG("[RCBOT2] commander-econ result=CREATED type=%d entity=%d class=%s pos=(%.0f %.0f %.0f) obb=(%.0f %.0f)-(%.0f %.0f) resources=%.0f->%.0f\n",
				state.verifyType, created, createdEdict != nullptr ? createdEdict->GetClassName() : "?", at.x, at.y, at.z,
				mins.x, mins.y, maxs.x, maxs.y, state.verifyResources, resources);
		}
		else
		{
			const int failures = ++state.failures[state.verifyType];
			state.blockedUntil[state.verifyType] = now + BackoffSeconds(failures);
			if (state.verifyType == Refinery && state.verifyResIndex >= 0)
				state.refineryPointBlocked.push_back({state.verifyResIndex, now + kPointBlockSeconds});
			RCBOT_DIAG("[RCBOT2] commander-econ result=REJECTED type=%d res_index=%d failures=%d retry_in=%.0fs resources=%.0f->%.0f\n",
				state.verifyType, state.verifyResIndex, failures, BackoffSeconds(failures), state.verifyResources, resources);
		}
		return;
	}

	if (unfinished >= MaxUnfinished(livingEngineers))
	{
		// Waiting for Engineers to finish: the next building is still planned (run p49931: the
		// reserve expired here and a tank was bought from the radar's money).
		EmpiresTeamEconomy::KeepReserve(EmpiresTeamEconomy::Team(team), gpGlobals->curtime);
		return;
	}

	// --- Refinery sites: enabled resource points the game would accept. ---
	struct Site { int index; int resIndex; Vector origin; float distance; };
	std::vector<Site> refinerySites;
	std::vector<Vector> friends;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		edict_t* player = engine->PEntityOfEntIndex(i);
		if (player == nullptr || player == m_pEdict || player->IsFree() || !CBotGlobals::entityIsAlive(player) ||
			CClassInterface::getTeam(player) != team)
			continue;
		friends.push_back(CBotGlobals::entityOrigin(player));
	}
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		const char* raw = RawEntity(e);
		if (raw == nullptr || !IsResourcePoint(e) || *reinterpret_cast<const unsigned char*>(raw + 0x4a8) == 0)
			continue;
		const int resIndex = *reinterpret_cast<const int*>(raw + 0x4b0);
		bool blocked = false;
		for (const auto& entry : state.refineryPointBlocked)
			blocked = blocked || (entry.first == resIndex && entry.second > now);
		if (blocked)
			continue;
		const Vector origin = CBotGlobals::entityOrigin(e);
		const float vehicleDistance = (origin - vehicleOrigin).Length();
		float nearestFriend = 1.0e9f;
		for (const Vector& friendOrigin : friends)
			nearestFriend = (std::min)(nearestFriend, (origin - friendOrigin).Length());
		const bool inReach = vehicleDistance < kNearVehicleRange || nearestFriend < kNearVehicleRange;
		if (!inReach || nearestFriend < kHullClearance)
			continue;
		refinerySites.push_back({i, resIndex, origin, vehicleDistance});
	}
	std::sort(refinerySites.begin(), refinerySites.end(), [](const Site& a, const Site& b) { return a.distance < b.distance; });

	// No resource point in reach: the game only accepts a refinery with the CV or a living
	// teammate within 4000 (streetsoffire NF: nearest point 7700 from the CV, one refinery
	// in 12 min). Walk a free Engineer toward the nearest enabled point; he then builds it.
	if (refinerySites.empty() && inventory.count[Refinery] < 4 && now >= state.escortUntil)
	{
		const Site* best = nullptr;
		std::vector<Site> candidates;
		for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
		{
			edict_t* e = engine->PEntityOfEntIndex(i);
			const char* raw = RawEntity(e);
			if (raw == nullptr || !IsResourcePoint(e) || *reinterpret_cast<const unsigned char*>(raw + 0x4a8) == 0)
				continue;
			const int resIndex = *reinterpret_cast<const int*>(raw + 0x4b0);
			bool blocked = false;
			for (const auto& entry : state.refineryPointBlocked)
				blocked = blocked || (entry.first == resIndex && entry.second > now);
			if (!blocked)
				candidates.push_back({i, resIndex, CBotGlobals::entityOrigin(e), (CBotGlobals::entityOrigin(e) - vehicleOrigin).Length()});
		}
		// Score = Engineer's walk + point's distance from the CV: a short walk wins, but a
		// point near our own base beats one deep in enemy land (run 20261111-102816: nearest
		// point to the BE CV sent the Engineer 17000 across the map).
		CBotEmpires* escort = nullptr;
		float bestScore = 1.0e9f;
		for (const Site& site : candidates)
			for (CBotEmpires* engineer : freeEngineers)
			{
				const float score = engineer->distanceFrom(site.origin) + site.distance;
				if (score < bestScore)
				{
					bestScore = score;
					best = &site;
					escort = engineer;
				}
			}
		if (best != nullptr && escort != nullptr && escort->requestEmpiresCommanderMoveTest(best->origin))
		{
			state.escortUntil = now + kEscortSeconds;
			RCBOT_DIAG("[RCBOT2] commander-econ escort engineer_userid=%d res_index=%d point=(%.0f %.0f %.0f) distance=%.0f cv_distance=%.0f candidates=%zu\n",
				engine->GetPlayerUserId(escort->getEdict()), best->resIndex, best->origin.x, best->origin.y,
				best->origin.z, escort->distanceFrom(best->origin), best->distance, candidates.size());
		}
	}

	for (int type = 0; type < TypeCount; ++type)
		inventory.siteAvailable[type] = now >= state.blockedUntil[type];
	if (refinerySites.empty())
		inventory.siteAvailable[Refinery] = false;

	if (!researchUnlocked)
		inventory.siteAvailable[VehicleFactory] = false;

	BuildType type = Refinery;
	const Decision decision = NextBuild(inventory, resources, type, rcbot_empires_commander_first.GetInt());
	{
		// TODO 5.5a: money the Commander needs next; vehicle purchases from the pool leave it.
		// Research for the factory drains Cost x Time = 240 (RE vehicle-driving.md). Game time,
		// like the vehicle code that reads it (this function's now is engine->Time()).
		float reserve = decision == Decision::Build || decision == Decision::Saving ? kCost[type] : 0.0f;
		if (!researchUnlocked)
			reserve += 240.0f;
		EmpiresTeamEconomy::PublishReserve(EmpiresTeamEconomy::Team(team), reserve, gpGlobals->curtime);
	}
	const int logKey = int(decision) * 100 + (decision == Decision::Build || decision == Decision::Saving ? int(type) : 0);
	if (decision == Decision::Build)
		state.lastLogKey = -1;
	else if (logKey != state.lastLogKey)
	{
		state.lastLogKey = logKey;
		RCBOT_DIAG("[RCBOT2] commander-econ plan decision=%s type=%d resources=%.0f refineries=%d barracks=%d factory=%d armory=%d repair=%d sites=%d\n",
			decision == Decision::Saving ? "saving" : (decision == Decision::NoSite ? "no-site" : "done"), int(type), resources,
			inventory.count[Refinery], inventory.count[Barracks], inventory.count[VehicleFactory],
			inventory.count[Armory], inventory.count[Repair], int(refinerySites.size()));
	}
	if (decision != Decision::Build)
		return;

	// --- Submit one build. ---
	EmpiresNativePlacement::ContextIdentity context;
	EmpiresNativePlacement::Diagnostics diagnostics;
	bool commander = false;
	if (!EmpiresNativePlacement::CaptureContext(engine->IndexOfEdict(m_pEdict), context, &diagnostics) ||
		!EmpiresNativePlacement::ReadCommanderMode(context, commander, &diagnostics) || !commander)
		return;

	char command[160];
	state.verifyResIndex = -1;
	if (type == Refinery)
	{
		state.verifyResIndex = refinerySites.front().resIndex;
		snprintf(command, sizeof(command), "emp_build 0 %d 0\n", state.verifyResIndex);
	}
	else
	{
		// The search radius is capped at 2048. Once the buildings already placed fill
		// that circle around the vehicle (measured: repair station on BE, all 59
		// candidates mask 0x2/0x22), search circles shifted by kShiftedSearchOffset;
		// they stay within the 4000 build range of the vehicle.
		// Search circles: around the CV, then shifted. Within each, the first
		// accepted spot that keeps the drive lanes of pads/factories free (run
		// 20261112-125413: factory on the pad entrance). Only if no circle has
		// one, the nearest valid spot (the base must grow).
		const std::vector<Lane> lanes = ReservedLanes(vehicleOrigin);
		Vector boxMins, boxMaxs;
		Footprint(int(type), team, boxMins, boxMaxs);
		auto yawAt = [&](const Vector& spot, bool quiet) {
			return type == VehicleFactory ? FactoryExitYaw(spot, team, quiet) :
				(type == Repair ? RepairPadYaw(spot, team, quiet) : 0.0f);
		};
		EmpiresSpatial::SearchResult search, first;
		bool haveFirst = false, laneFree = false;
		// Repair pad without an ideal spot in any circle: the lane-free spot with the
		// longest free axis that is not behind the CV (run 20261112-163322: BE pad
		// 800 behind the CV, axis free only 600, VF and armory in front of it).
		const Vector enemyCv = EnemyCommanderOrigin(vehicleOrigin, team);
		const float cvToEnemy = (enemyCv - vehicleOrigin).Length2D();
		bool haveFallback = false;
		EmpiresSpatial::Point fallback{};
		float fallbackScore = -1.0e9f;
		int fallbackRing = -1;
		EmpiresSpatial::Point at{};
		int usedRing = -1;
		size_t checked = 0;
		for (int ring = 0; ring < 5 && !laneFree; ++ring)
		{
			Vector center = vehicleOrigin;
			if (ring > 0)
			{
				const float angle = (ring - 1) * 1.5707963f;
				center += Vector(std::cos(angle), std::sin(angle), 0.0f) * kShiftedSearchOffset;
			}
			EmpiresSpatial::Request request;
			request.origin = {center.x, center.y, center.z};
			request.radius = kSearchRadius;
			request.buildingType = int(type);
			request.invalidSkipRadius = kInvalidSkipRadius;
			search = EmpiresSpatial::SearchBarracksCandidates(context, request);
			if (search.status == EmpiresSpatial::Status::Unavailable)
				break;
			if (search.status != EmpiresSpatial::Status::Found || !search.hasSelected)
				continue;
			if (!haveFirst)
			{
				first = search;
				haveFirst = true;
			}
			// Repair pad: within this circle prefer a spot whose drive axis runs
			// within 30 deg of the front and is clear at both ends (run 124526:
			// the first free spot only allowed a 60 deg turn). Else the first
			// lane-free spot as before.
			size_t padChecked = 0;
			for (const auto& candidate : search.accepted)
			{
				++checked;
				const Vector spot(candidate.origin.x, candidate.origin.y, candidate.origin.z);
				float turn = 0.0f, free = 0.0f;
				const float yaw = type == Repair ? RepairPadYaw(spot, team, true, &turn, &free) : yawAt(spot, true);
				if (!lanes.empty() && BlocksLane(spot, yaw, boxMins, boxMaxs, lanes))
					continue;
				const bool behind = type == Repair && (enemyCv - spot).Length2D() > cvToEnemy + 300.0f;
				if (type != Repair || (std::fabs(turn) <= 30.0f && free >= 1150.0f && !behind))
				{
					at = candidate.origin;
					laneFree = true;
					break;
				}
				const float score = free - (behind ? 800.0f : 0.0f) - std::fabs(turn) * 2.0f;
				if (!haveFallback || score > fallbackScore)
				{
					fallback = candidate.origin;
					fallbackScore = score;
					fallbackRing = ring;
					haveFallback = true;
				}
				if (++padChecked >= 40) // traces: 4 per turn, up to 6 turns
					break;
			}
			if (laneFree)
				usedRing = ring;
		}
		if (!laneFree && haveFallback)
		{
			at = fallback;
			laneFree = true;
			usedRing = fallbackRing;
		}
		if (!haveFirst)
		{
			const int failures = ++state.failures[type];
			state.blockedUntil[type] = now + BackoffSeconds(failures);
			RCBOT_DIAG("[RCBOT2] commander-econ result=NO_SPOT type=%d search=%s areas=%u queries=%u invalid=%u failures=%d\n",
				int(type), EmpiresSpatial::Name(search.status), search.areasInspected, search.nativeQueries,
				search.invalid, failures);
			return;
		}
		if (!laneFree)
			at = first.selected.origin;
		if (!lanes.empty() || type == Repair)
			RCBOT_DIAG("[RCBOT2] commander-econ lane-check type=%d lanes=%zu checked=%zu ring=%d%s\n", int(type),
				lanes.size(), checked, usedRing, laneFree ? "" : " all-block");
		const float yaw = yawAt(Vector(at.x, at.y, at.z), false);
		snprintf(command, sizeof(command), "emp_build %d %.9g %.9g %.9g %.1f\n", int(type),
			double(at.x), double(at.y), double(at.z), double(yaw));
	}
	state.verifyBefore = allBuildings;
	state.verifyType = int(type);
	state.verifyResources = resources;
	state.verifyAt = now + kVerifyDelay;
	state.verifyPending = true;
	RCBOT_DIAG("[RCBOT2] commander-econ submit type=%d cmd=%s", int(type), command);
	helpers->ClientCommand(m_pEdict, command);
}

void CBotEmpires::requestCommanderYieldTest(int team)
{
	if (team == 2 || team == 3)
		s_testYieldRequest[team] = true;
}

// Heading toward the front from a spot (see FrontYaw), for Engineer turret placement.
bool EmpiresFrontYaw(const Vector& at, int team, float& yaw)
{
	const char* source = nullptr;
	return FrontYaw(at, team, yaw, source);
}

// Team pool for other modules (vehicle purchase diagnostics, TODO 5.5a).
bool EmpiresTeamResources(int team, float& resources)
{
	return ReadTeamResources(team, resources);
}

// Adds delta to the team pool like emp_give_resources / InputModifyNFResources (0x647610 /
// 0x647530: ledger +0x30 += x; their history entry at +0x44 is skipped). Not below 0.
bool EmpiresModifyTeamResources(int team, float delta)
{
	if (!EmpiresBinaryOffsetsTrusted())
		return false;
	for (int i = 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		if (e == nullptr || e->IsFree() || e->GetNetworkable() == nullptr)
			continue;
		ServerClass* serverClass = e->GetNetworkable()->GetServerClass();
		if (serverClass == nullptr || std::strcmp(serverClass->GetName(), "CEmpTeam") != 0)
			continue;
		char* raw = const_cast<char*>(RawEntity(e));
		if (raw == nullptr || *reinterpret_cast<const int*>(raw + 0x3a8) != team)
			continue;
		char* ledger = *reinterpret_cast<char**>(raw + 0x3b4);
		if (ledger == nullptr)
			return false;
		float& pool = *reinterpret_cast<float*>(ledger + 0x30);
		if (!(pool >= 0.0f && pool < 1.0e7f) || pool + delta < 0.0f)
			return false;
		pool += delta;
		return true;
	}
	return false;
}

void EmpiresCommanderEconomyLevelReset()
{
	EmpiresTeamEconomy::Reset();
	for (int i = 0; i < 4; i++)
	{
		s_humanCommanderUntil[i] = 0.0f;
		s_testYieldRequest[i] = false;
		s_cvHome[i] = Vector();
		s_cvHomeVehicle[i] = 0;
	}
}
