#include "bot_navmesh_provider.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "bot.h"
#include "bot_empires_telemetry.h"
#include "bot_getprop.h"
#include "bot_globals.h"
#include "bot_navmesh_api.h"
#include "bot_cvars.h"

extern IServerGameEnts* servergameents;

extern IVEngineServer* engine;

namespace
{
constexpr float kGoalTolerance = 64.0f;
constexpr float kStepHeight = 18.0f;
constexpr float kBelowPointReach = 48.0f;

struct Route
{
	CBot* bot = nullptr;
	bool active = false;
	float lastJump = -100.0f;
	int jumps = 0;
	float lastDoorUse = -100.0f;
	std::uint64_t intentId = 0;
	std::uint64_t attemptId = 0;
	int goalId = -1;
	Vector goal = vec3_origin;
	RCBotNavMesh::Path path;
	std::size_t point = 0;
	std::size_t transitionAreaIndex = 0;
	int lastObservedArea = -1;
};

Route s_routes[RCBOT_MAXPLAYERS];

class SkipSelfAndPlayers : public CTraceFilter
{
public:
	explicit SkipSelfAndPlayers(const IHandleEntity* self) : m_self(self) {}
	bool ShouldHitEntity(IHandleEntity* entity, int) override
	{
		if (entity == m_self)
			return false;
		IServerUnknown* unknown = static_cast<IServerUnknown*>(entity);
		CBaseEntity* base = unknown != nullptr ? unknown->GetBaseEntity() : nullptr;
		edict_t* e = base != nullptr && servergameents != nullptr ? servergameents->BaseEntityToEdict(base) : nullptr;
		const char* name = e != nullptr ? e->GetClassName() : nullptr;
		return name == nullptr || strcmp(name, "player") != 0;
	}
private:
	const IHandleEntity* m_self;
};

// The route ends on the nearest mesh point, up to 600 beside the goal, but a commander MOVE only
// ends within 100 of the order spot (TODO 3.18). Add the goal as last point when the bot can walk
// there in a straight line: same height (step), hull free, ground under it.
void ExtendPathToGoal(CBot* bot, const Vector& goal, RCBotNavMesh::Path& path)
{
	if (enginetrace == nullptr || bot == nullptr || bot->getEdict() == nullptr || path.points.empty())
		return;
	const Vector last = path.points.back();
	const float gap = (goal - last).Length2D();
	if (gap <= 32.0f || gap > 600.0f || std::fabs(goal.z - last.z) > kStepHeight + 24.0f)
		return;
	SkipSelfAndPlayers skip(bot->getEdict()->GetIServerEntity());
	// Ground under the goal, at most a step below the end point's height.
	Ray_t down;
	down.Init(Vector(goal.x, goal.y, std::max(goal.z, last.z) + kStepHeight),
		Vector(goal.x, goal.y, last.z - kStepHeight - 8.0f), Vector(-16, -16, 0), Vector(16, 16, 2));
	trace_t ground;
	enginetrace->TraceRay(down, MASK_PLAYERSOLID, &skip, &ground);
	if (ground.startsolid || ground.allsolid || ground.fraction >= 1.0f || ground.plane.normal.z < 0.7f)
		return;
	const Vector end = ground.endpos;
	if (std::fabs(end.z - last.z) > kStepHeight)
		return;
	Ray_t walk;
	walk.Init(last + Vector(0, 0, kStepHeight + 2.0f), end + Vector(0, 0, kStepHeight + 2.0f),
		Vector(-16, -16, 0), Vector(16, 16, 72 - kStepHeight - 2.0f));
	trace_t hit;
	enginetrace->TraceRay(walk, MASK_PLAYERSOLID, &skip, &hit);
	if (hit.startsolid || hit.fraction < 1.0f)
		return;
	path.points.push_back(end);
	path.length += (end - last).Length();
	RCBOT_DIAG("[RCBOT2] navmesh-path-extended gap=%.0f end=(%.0f %.0f %.0f)\n", gap, end.x, end.y, end.z);
}

bool DoorDatamapInt(edict_t* door, const char* name, int& value)
{
	CBaseEntity* base = door != nullptr && !door->IsFree() && door->GetUnknown() != nullptr ?
		door->GetUnknown()->GetBaseEntity() : nullptr;
	const datamap_t* map = base != nullptr ? CBaseEntity_GetDataDescMap(base) : nullptr;
	const unsigned offset = map != nullptr ? UTIL_FindInDataMap(map, name) : 0;
	if (offset == 0)
		return false;
	value = *reinterpret_cast<int*>(reinterpret_cast<char*>(base) + offset);
	return true;
}

// func_door that neither use (SF_DOOR_PUSE 256) nor touch (SF_DOOR_PTOUCH 1024) opens,
// or that is locked: only a map input (button, timer) moves it. as_escort gate 228
// (door1, spawnflags 2080 = toggle + locked) opens from button control1 only.
bool InputOnlyDoor(edict_t* door)
{
	const char* name = door != nullptr ? door->GetClassName() : nullptr;
	int flags = 0;
	if (name == nullptr || std::strncmp(name, "func_door", 9) != 0 ||
		!DoorDatamapInt(door, "m_spawnflags", flags))
		return false;
	int locked = 0;
	const bool lockedKnown = DoorDatamapInt(door, "m_bLocked", locked);
	return (flags & (256 | 1024)) == 0 || (lockedKnown && (locked & 0xff) != 0);
}

// Closed = resting in its start position (TS_AT_BOTTOM); start-open doors are skipped.
bool DoorClosed(edict_t* door)
{
	int flags = 0, state = -1;
	return DoorDatamapInt(door, "m_spawnflags", flags) && (flags & 1) == 0 &&
		DoorDatamapInt(door, "m_toggle_state", state) && state == 1;
}

// Keep the links through closed input-only doors blocked while they stay closed,
// so routes do not lead into them at all. Refreshed every second, short expiry.
void UpdateInputOnlyDoorBlocks()
{
	static float s_next = 0.0f;
	static std::vector<int> s_closed;
	if (gpGlobals == nullptr || engine == nullptr || !RCBotNavMesh::IsLoaded())
		return;
	const float now = gpGlobals->curtime;
	if (now < s_next && now > s_next - 5.0f)
		return;
	s_next = now + 1.0f;
	s_closed.resize(gpGlobals->maxEntities, -1);
	// A trigger volume at the door opens it when walked into (as_escort bridge3_door1:
	// trigger_multiple in the doorway). Those doors stay open to routing.
	std::vector<std::pair<Vector, Vector>> triggers;
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* trigger = INDEXENT(index);
		const char* name = trigger != nullptr && !trigger->IsFree() ? trigger->GetClassName() : nullptr;
		ICollideable* collide = name != nullptr ? trigger->GetCollideable() : nullptr;
		if (collide == nullptr || (std::strcmp(name, "trigger_multiple") != 0 && std::strcmp(name, "trigger_once") != 0))
			continue;
		Vector mins, maxs;
		collide->WorldSpaceSurroundingBounds(&mins, &maxs);
		triggers.emplace_back(mins, maxs);
	}
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* door = INDEXENT(index);
		if (door == nullptr || door->IsFree() || !InputOnlyDoor(door))
			continue;
		ICollideable* collide = door->GetCollideable();
		if (collide == nullptr)
			continue;
		Vector mins, maxs;
		collide->WorldSpaceSurroundingBounds(&mins, &maxs);
		bool triggered = false;
		for (const auto& box : triggers)
			triggered = triggered || (box.first.x <= maxs.x + 96.0f && box.second.x >= mins.x - 96.0f &&
				box.first.y <= maxs.y + 96.0f && box.second.y >= mins.y - 96.0f &&
				box.first.z <= maxs.z + 96.0f && box.second.z >= mins.z - 96.0f);
		if (triggered)
			continue;
		const bool closed = DoorClosed(door);
		int links = 0;
		if (closed)
		{
			links = RCBotNavMesh::BlockEdgesThroughBox(mins - Vector(8, 8, 0), maxs + Vector(8, 8, 0), 2.5f,
				s_closed[index] != 1);
		}
		if (s_closed[index] != (closed ? 1 : 0))
			RCBOT_DIAG("[RCBOT2] navmesh-input-door door=%d state=%s links=%d\n", index,
				closed ? "closed" : "open", links);
		s_closed[index] = closed ? 1 : 0;
	}
	// Sliding gates (func_movelinear, opened by map logic only): block the links their box covers
	// right now. An open gate has moved up or aside and covers no walkway, so no state is needed.
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* gate = INDEXENT(index);
		const char* name = gate != nullptr && !gate->IsFree() ? gate->GetClassName() : nullptr;
		ICollideable* collide = name != nullptr && std::strcmp(name, "func_movelinear") == 0 ?
			gate->GetCollideable() : nullptr;
		if (collide == nullptr || collide->GetSolid() == SOLID_NONE)
			continue;
		Vector mins, maxs;
		collide->WorldSpaceSurroundingBounds(&mins, &maxs);
		const int links = RCBotNavMesh::BlockEdgesThroughBox(mins - Vector(8, 8, 0), maxs + Vector(8, 8, 0), 2.5f,
			false);
		const bool closed = links > 0;
		if (s_closed[index] != (closed ? 1 : 0))
			RCBOT_DIAG("[RCBOT2] navmesh-gate gate=%d state=%s links=%d box=(%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n", index,
				closed ? "closed" : "open", links, mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z);
		s_closed[index] = closed ? 1 : 0;
	}
}

// A closed door straight ahead, across the way to the next point. streetsoffire door *24
// (-45 505 -86) has spawnflags 288 (use opens, toggle): walking into it never opens it, an
// Engineer pushed against it for 379 s. Returns the point on the door to look at.
edict_t* DoorAhead(CBot* bot, const Vector& target, Vector& aim)
{
	if (enginetrace == nullptr || servergameents == nullptr || bot->getEdict() == nullptr)
		return nullptr;
	const Vector here = bot->getOrigin();
	Vector dir = target - here;
	dir.z = 0.0f;
	if (dir.Length2D() < 1.0f)
		return nullptr;
	dir.NormalizeInPlace();
	const Vector start = here + Vector(0, 0, 20);
	Ray_t ray;
	ray.Init(start, start + dir * 48.0f, Vector(-16, -16, 0), Vector(16, 16, 52));
	class DoorFilter : public CTraceFilter
	{
	public:
		explicit DoorFilter(const IHandleEntity* self) : m_self(self) {}
		bool ShouldHitEntity(IHandleEntity* entity, int) override
		{
			if (entity == m_self)
				return false;
			IServerUnknown* unknown = static_cast<IServerUnknown*>(entity);
			CBaseEntity* base = unknown != nullptr ? unknown->GetBaseEntity() : nullptr;
			edict_t* e = base != nullptr ? servergameents->BaseEntityToEdict(base) : nullptr;
			const char* name = e != nullptr ? e->GetClassName() : nullptr;
			return name != nullptr && (strncmp(name, "func_door", 9) == 0 || strncmp(name, "prop_door", 9) == 0);
		}
	private:
		const IHandleEntity* m_self;
	} filter(bot->getEdict()->GetIServerEntity());
	trace_t hit;
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &hit);
	if (hit.startsolid || hit.fraction >= 1.0f || hit.m_pEnt == nullptr)
		return nullptr;
	// World brushes are hit without asking the filter.
	edict_t* door = servergameents->BaseEntityToEdict(hit.m_pEnt);
	const char* name = door != nullptr ? door->GetClassName() : nullptr;
	if (name == nullptr || (strncmp(name, "func_door", 9) != 0 && strncmp(name, "prop_door", 9) != 0))
		return nullptr;
	// Use never opens it (as_escort gate 228: three uses, then stuck).
	if (InputOnlyDoor(door))
		return nullptr;
	// Only a door that stands across the way; an opened rotating door lies along it and
	// another use would close it again (toggle).
	if (-DotProduct(hit.plane.normal, dir) < 0.7f)
		return nullptr;
	aim = hit.endpos + dir * 20.0f;
	aim.z = bot->getEyePosition().z;
	return door;
}

Route* routeForBot(const CBot* bot)
{
	if (bot == nullptr || bot->getEdict() == nullptr)
		return nullptr;

	const int slot = CBots::slotOfEdict(bot->getEdict());
	if (slot < 0 || slot >= RCBOT_MAXPLAYERS)
		return nullptr;
	return &s_routes[slot];
}

bool sameIntent(const Route& route)
{
	if (!route.active || route.bot == nullptr ||
		!route.bot->hasActiveNavigationIntent())
		return false;

	const RCBotNavigation::NavigationIntent& intent =
		route.bot->getNavigationIntent();
	return intent.provider == RCBotNavigation::Provider::NAVMESH &&
		intent.owner == RCBotNavigation::IntentOwner::NAVMESH &&
		intent.id == route.intentId;
}

void resetRoute(Route& route)
{
	route = Route{};
}

void providerEvent(CBot* bot, const char* event, const char* detail)
{
	EmpiresAnalysis::RecordPathEvent(bot, event, -1, detail);
}

RCBotNavigation::SegmentKey segmentFor(const Route& route)
{
	RCBotNavigation::SegmentKey segment;
	if (route.path.areaIds.empty())
		return segment;
	const std::size_t current = route.transitionAreaIndex;
	if (current >= route.path.areaIds.size() ||
		current + 1 >= route.path.areaIds.size() ||
		route.lastObservedArea !=
			static_cast<int>(route.path.areaIds[current]))
		return segment;
	segment.currentWaypoint = static_cast<int>(route.path.areaIds[current]);
	segment.nextWaypoint = static_cast<int>(route.path.areaIds[current + 1]);
	return segment;
}

void recordUnknownTransition(CBot* bot, const char* reason)
{
	if (bot == nullptr || !bot->hasActiveNavigationAttempt())
		return;
	EmpiresAnalysis::RecordNavigationTransition(bot, bot->getNavigationIntent(),
		bot->getNavigationAttempt(), -1, -1,
		RCBotNavigationExperience::Traversal::UNKNOWN,
		RCBotNavigationExperience::TransitionResult::UNKNOWN,
		RCBotNavigationExperience::CancellationKind::NONE, reason);
}

void observeAreaTransition(Route& route, CBot* bot)
{
	if (bot == nullptr || route.path.areaIds.empty() ||
		!bot->hasActiveNavigationAttempt())
		return;
	const int currentArea =
		RCBotNavMesh::GetAreaIdAtPosition(bot->getOrigin());
	if (currentArea < 0)
	{
		route.lastObservedArea = -1;
		return;
	}
	if (route.lastObservedArea < 0)
	{
		for (std::size_t index = 0; index < route.path.areaIds.size(); ++index)
		{
			if (static_cast<int>(route.path.areaIds[index]) != currentArea)
				continue;
			route.transitionAreaIndex = index;
			route.lastObservedArea = currentArea;
			return;
		}
		recordUnknownTransition(bot, "navmesh-current-area-not-on-route");
		route.transitionAreaIndex = route.path.areaIds.size();
		route.lastObservedArea = currentArea;
		return;
	}
	if (currentArea == route.lastObservedArea)
		return;

	const std::size_t expected = route.transitionAreaIndex;
	if (expected + 1 < route.path.areaIds.size() &&
		static_cast<int>(route.path.areaIds[expected]) == route.lastObservedArea &&
		static_cast<int>(route.path.areaIds[expected + 1]) == currentArea)
	{
		EmpiresAnalysis::RecordNavigationTransition(bot,
			bot->getNavigationIntent(), bot->getNavigationAttempt(),
			route.lastObservedArea, currentArea,
			RCBotNavigationExperience::Traversal::UNKNOWN,
			RCBotNavigationExperience::TransitionResult::TRANSITION_SUCCESS,
			RCBotNavigationExperience::CancellationKind::NONE,
			"navmesh-entered-next-area");
		route.transitionAreaIndex = expected + 1;
		route.lastObservedArea = currentArea;
		return;
	}

	for (std::size_t index = 0; index < route.path.areaIds.size(); ++index)
	{
		if (static_cast<int>(route.path.areaIds[index]) != currentArea)
			continue;
		recordUnknownTransition(bot,
			"navmesh-area-transition-not-observed-in-order");
		route.transitionAreaIndex = index;
		route.lastObservedArea = currentArea;
		return;
	}
	recordUnknownTransition(bot, "navmesh-area-transition-outside-route");
	route.transitionAreaIndex = route.path.areaIds.size();
	route.lastObservedArea = currentArea;
}

float remainingDistance(const Route& route, CBot* bot)
{
	if (bot == nullptr || route.point >= route.path.points.size())
		return 0.0f;

	float distance = bot->distanceFrom(route.path.points[route.point]);
	for (std::size_t i = route.point + 1; i < route.path.points.size(); ++i)
		distance += (route.path.points[i] - route.path.points[i - 1]).Length();
	return distance;
}

void finishRoute(Route& route, const RCBotNavigation::Result result,
	const char* reason)
{
	CBot* bot = route.bot;
	if (bot != nullptr && sameIntent(route))
	{
		if (result == RCBotNavigation::Result::CANCELLED)
			bot->cancelNavigationAttempt(reason);
		else if (bot->hasActiveNavigationAttempt())
			bot->finishNavigationAttempt(result, reason);
		else
			bot->finishNavigationIntent(result, reason);
	}
	resetRoute(route);
}

RCBotNavigation::FailureReason failureReasonFor(
	const RCBotNavMesh::PathResult result)
{
	switch (result)
	{
	case RCBotNavMesh::PathResult::NAVMESH_NOT_LOADED:
		return RCBotNavigation::FailureReason::PROVIDER_UNAVAILABLE;
	case RCBotNavMesh::PathResult::START_AREA_NOT_FOUND:
		return RCBotNavigation::FailureReason::START_AREA_NOT_FOUND;
	case RCBotNavMesh::PathResult::GOAL_AREA_NOT_FOUND:
		return RCBotNavigation::FailureReason::GOAL_AREA_NOT_FOUND;
	case RCBotNavMesh::PathResult::NO_PATH:
		return RCBotNavigation::FailureReason::NO_PATH;
	default:
		return RCBotNavigation::FailureReason::PATH_FAILED;
	}
}

RCBotNavigation::Result resultFor(
	const RCBotNavMesh::PathResult result)
{
	return result == RCBotNavMesh::PathResult::NAVMESH_NOT_LOADED ?
		RCBotNavigation::Result::PATH_FAILED :
		RCBotNavigation::Result::UNREACHABLE;
}
}

namespace RCBotNavMesh
{
int ObserverTeam(CBot* bot)
{
	return bot != nullptr ? bot->getTeam() : -1;
}
}

namespace RCBotNavMeshProvider
{
bool Start(CBot* bot, const Vector& goal)
{
	if (bot == nullptr || !bot->inUse() || !bot->isAlive())
		return false;

	Route* route = routeForBot(bot);
	if (route == nullptr)
		return false;
	if (route->active)
		finishRoute(*route, RCBotNavigation::Result::CANCELLED,
			"provider-route-replaced");

	if (!bot->beginNavigationIntent(RCBotNavigation::Provider::NAVMESH,
		RCBotNavigation::IntentOwner::NAVMESH, -1, goal,
		RCBotNavigation::NavigationPolicy::NAVMESH_ONLY))
		return false;
	return StartForIntent(bot, bot->getNavigationIntentId(), goal);
}

bool StartForIntent(CBot* bot, const std::uint64_t intentId,
	const Vector& goal)
{
	if (bot == nullptr || !bot->inUse() || !bot->isAlive() ||
		!bot->hasActiveNavigationIntent())
		return false;
	const RCBotNavigation::NavigationIntent& intent =
		bot->getNavigationIntent();
	if (intent.id != intentId ||
		intent.provider != RCBotNavigation::Provider::NAVMESH ||
		intent.owner != RCBotNavigation::IntentOwner::NAVMESH ||
		intent.goalOrigin != goal)
		return false;

	Route* route = routeForBot(bot);
	if (route == nullptr)
		return false;
	if (route->active && route->intentId != intentId)
		resetRoute(*route);

	const Vector start = bot->getOrigin();

	RCBotNavMesh::Path path;
	const RCBotNavMesh::PathResult pathResult =
		RCBotNavMesh::BuildPath(start, goal, path, bot);
	if (pathResult != RCBotNavMesh::PathResult::SUCCESS ||
		path.points.empty())
	{
		char detail[256];
		std::snprintf(detail, sizeof(detail),
			"path_result=%s start=(%.1f %.1f %.1f) goal=(%.1f %.1f %.1f)",
			RCBotNavMesh::PathResultName(pathResult), start.x, start.y,
			start.z, goal.x, goal.y, goal.z);
		RCBOT_DIAG("[RCBOT2] navmesh-plan-failed %s userid=%d\n", detail,
			engine != nullptr && bot->getEdict() != nullptr ? engine->GetPlayerUserId(bot->getEdict()) : -1);
		bot->handleNavigationProviderFailure(
			RCBotNavigation::Provider::NAVMESH,
			RCBotNavigation::FailureClass::STRUCTURAL_PLANNING,
			failureReasonFor(pathResult), resultFor(pathResult), detail);
		return false;
	}
	ExtendPathToGoal(bot, goal, path);

	char pathDetail[320];
	std::snprintf(pathDetail, sizeof(pathDetail),
		"provider=NAVMESH result=SUCCESS points=%zu areas=%zu length=%.1f start_area=%d goal_area=%d",
		path.points.size(), path.areaIds.size(), path.length,
		path.startAreaId, path.goalAreaId);
	providerEvent(bot, "NAV_PROVIDER_PATH", pathDetail);

	bot->setNavigationIntentState(RCBotNavigation::IntentState::PLANNING,
		"navmesh-provider-path-ready");

	const std::size_t firstPoint = path.points.size() > 1 ? 1 : 0;
	const RCBotNavigation::SegmentKey segment{
		path.areaIds.empty() ? -1 : static_cast<int>(path.areaIds.front()),
		path.areaIds.size() > 1 ? static_cast<int>(path.areaIds[1]) : -1
	};
	if (!bot->beginNavigationAttempt(
		RCBotNavigation::Provider::NAVMESH, intent.goalId, goal, segment,
		path.length, RCBotNavigation::NavigationAttempt::kNoProgressTimeout))
	{
		bot->handleNavigationProviderFailure(
			RCBotNavigation::Provider::NAVMESH,
			RCBotNavigation::FailureClass::STRUCTURAL_PLANNING,
			RCBotNavigation::FailureReason::PROVIDER_UNAVAILABLE,
			RCBotNavigation::Result::PATH_FAILED,
			"navmesh-attempt-rejected");
		return false;
	}

	route->bot = bot;
	route->active = true;
	route->intentId = bot->getNavigationIntentId();
	route->attemptId = bot->getNavigationAttemptId();
	route->goal = goal;
	route->path = path;
	route->point = firstPoint;
	route->transitionAreaIndex = 0;
	route->lastObservedArea =
		RCBotNavMesh::GetAreaIdAtPosition(start) == path.startAreaId ?
			path.startAreaId : -1;
	return true;
}

bool Cancel(CBot* bot, const char* reason)
{
	Route* route = routeForBot(bot);
	if (route == nullptr || !route->active)
		return false;
	finishRoute(*route, RCBotNavigation::Result::CANCELLED,
		reason != nullptr ? reason : "provider-cancelled");
	return true;
}

bool StopForIntent(CBot* bot, const std::uint64_t intentId)
{
	Route* route = routeForBot(bot);
	if (route == nullptr || !route->active || route->intentId != intentId)
		return false;
	resetRoute(*route);
	return true;
}

bool IsActive(const CBot* bot)
{
	const Route* route = routeForBot(bot);
	return route != nullptr && route->active && sameIntent(*route);
}

void Frame()
{
	CBotMod* mod = CBotGlobals::getCurrentMod();
	if (mod == nullptr || mod->getBotType() != BOTTYPE_EMPIRES ||
		engine == nullptr)
		return;
	UpdateInputOnlyDoorBlocks();

	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		Route& route = s_routes[slot];
		if (!route.active)
			continue;

		CBot* bot = route.bot;
		if (bot == nullptr || !bot->inUse() || !bot->isAlive())
		{
			if (bot != nullptr && bot->inUse() && bot->hasActiveNavigationIntent())
				bot->cancelNavigationAttempt("bot-dead");
			resetRoute(route);
			continue;
		}
		if (!sameIntent(route))
		{
			resetRoute(route);
			continue;
		}
		observeAreaTransition(route, bot);

		// A point up on a ledge only counts once the bot is up there too.
		// A point just below the bot (the mesh area lies under a ledge crown) is never within 20
		// in 3D when the bot stands on the crown; it counts once the bot is above it (streetsoffire
		// (-4244 -6269 -15), bot 21 higher: stood still for 127 s).
		const auto pointReached = [&]() {
			const Vector& pt = route.path.points[route.point];
			const Vector bo = bot->getOrigin();
			if (pt.z - bo.z > kStepHeight)
				return false;
			if (route.point + 1 == route.path.points.size())
				return bot->distanceFrom(pt) <= kGoalTolerance;
			if (bot->distanceFrom(pt) <= 20.0f)
				return true;
			return bo.z - pt.z <= kBelowPointReach && Vector(pt.x - bo.x, pt.y - bo.y, 0.0f).Length() <= 20.0f;
		};
		while (route.point < route.path.points.size() && pointReached())
		{
			for (const Vector& ladderPoint : route.path.ladderPoints)
				if ((ladderPoint - route.path.points[route.point]).LengthSqr() < 1.0f)
				{
					RCBOT_DIAG("[RCBOT2] navmesh-ladder-climbed bot=(%.0f %.0f %.0f)\n",
						bot->getOrigin().x, bot->getOrigin().y, bot->getOrigin().z);
					break;
				}
			++route.point;
		}

		if (route.point >= route.path.points.size() ||
			bot->distanceFrom(route.goal) <= kGoalTolerance)
		{
			bot->finishNavigationAttempt(
				RCBotNavigation::Result::COMPLETE, "navmesh-goal-reached");
			resetRoute(route);
			continue;
		}

		const RCBotNavigation::SegmentKey segment = segmentFor(route);
		const RCBotNavigation::Observation observation =
			bot->observeNavigationAttempt(remainingDistance(route, bot), segment);
		if (observation == RCBotNavigation::Observation::NO_PROGRESS)
		{
			{
				const Vector here = bot->getOrigin();
				const Vector& target = route.path.points[route.point];
				RCBOT_DIAG("[RCBOT2] navmesh-stuck point=%zu/%zu target=(%.0f %.0f %.0f) bot=(%.0f %.0f %.0f) on_ground=%d jumps=%d since_jump=%.1f tick=%d\n",
					route.point, route.path.points.size(), target.x, target.y, target.z,
					here.x, here.y, here.z,
					(CClassInterface::getPlayerFlags(bot->getEdict()) & FL_ONGROUND) != 0 ? 1 : 0,
					route.jumps, engine->Time() - route.lastJump, gpGlobals->tickcount);
				bool worldBlocked = false;
				bool doorBlocked = false;
				edict_t* closedDoor = nullptr;
				// What blocks the bot: sweep its hull toward the target at two heights.
				if (enginetrace != nullptr && bot->getEdict() != nullptr)
				{
					for (const float lift : {2.0f, 20.0f})
					{
						Ray_t ray;
						ray.Init(here + Vector(0, 0, lift), Vector(target.x, target.y, here.z + lift),
							Vector(-16, -16, 0), Vector(16, 16, 72));
						trace_t hit;
						class SkipSelf : public CTraceFilter
						{
						public:
							explicit SkipSelf(const IHandleEntity* self) : m_self(self) {}
							bool ShouldHitEntity(IHandleEntity* entity, int) override
							{
								if (entity == m_self)
									return false;
								// Volumes the player does not collide with would hide the real blocker.
								// Players too: two bots stuck at the same wall overlap, and the
								// start-solid hit on the teammate hid the wall (crossroads (-217 -9552)).
								IServerUnknown* unknown = static_cast<IServerUnknown*>(entity);
								CBaseEntity* base = unknown != nullptr ? unknown->GetBaseEntity() : nullptr;
								edict_t* e = base != nullptr && servergameents != nullptr ?
									servergameents->BaseEntityToEdict(base) : nullptr;
								const char* name = e != nullptr ? e->GetClassName() : nullptr;
								return name == nullptr || (strcmp(name, "emp_eng_restrict") != 0 &&
									strcmp(name, "emp_comm_restrict") != 0 &&
									strcmp(name, "emp_building_vehicle_physbox") != 0 &&
									strcmp(name, "player") != 0);
							}
						private:
							const IHandleEntity* m_self;
						} skip(bot->getEdict()->GetIServerEntity());
						enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &skip, &hit);
						if (!hit.startsolid && hit.fraction < 1.0f && hit.m_pEnt != nullptr &&
							servergameents != nullptr &&
							engine->IndexOfEdict(servergameents->BaseEntityToEdict(hit.m_pEnt)) == 0)
							worldBlocked = true;
						edict_t* e = hit.m_pEnt != nullptr && servergameents != nullptr ?
							servergameents->BaseEntityToEdict(hit.m_pEnt) : nullptr;
						// A door still shut after we pressed use (as_escort BE gate 228 opens
						// by a button after a timer): plan around it for a while.
						if (!hit.startsolid && hit.fraction < 1.0f && e != nullptr && e->GetClassName() != nullptr &&
							std::strncmp(e->GetClassName(), "func_door", 9) == 0 && route.lastDoorUse > 0.0f)
						{
							doorBlocked = true;
							closedDoor = e;
						}
						// Map-placed engineer models (NF barricades, as_escort 347 at (1440 1985)) are
						// solid but unknown to the mesh: both BE bots ran into one 7 times. They can be
						// destroyed or rebuilt, so block their links for a while like a closed door.
						// Same for any other solid entity the mesh does not know: health crates
						// (as_escort BE base: emp_imp_health_crate 428 at (-413 5175)) and the like.
						// Buildings have their own detours, an unused door gets a use first.
						const char* hitName = e != nullptr ? e->GetClassName() : nullptr;
						if (!hit.startsolid && hit.fraction < 1.0f && hitName != nullptr &&
							engine->IndexOfEdict(e) != 0 && std::strcmp(hitName, "player") != 0 &&
							std::strncmp(hitName, "emp_building", 12) != 0 &&
							std::strncmp(hitName, "func_door", 9) != 0 && std::strncmp(hitName, "prop_door", 9) != 0)
						{
							doorBlocked = true;
							closedDoor = e;
						}
						RCBOT_DIAG("[RCBOT2] navmesh-stuck-trace lift=%.0f frac=%.2f start_solid=%d hit=%s index=%d end=(%.0f %.0f %.0f) normal=(%.2f %.2f %.2f)\n",
							lift, hit.fraction, hit.startsolid ? 1 : 0,
							e != nullptr && e->GetClassName() != nullptr ? e->GetClassName() : "-",
							e != nullptr ? engine->IndexOfEdict(e) : -1,
							hit.endpos.x, hit.endpos.y, hit.endpos.z,
							hit.plane.normal.x, hit.plane.normal.y, hit.plane.normal.z);
						// TODO 3.1 diagnosis: whose vehicle, where, is the bot on foot.
						if (lift == 2.0f && hit.startsolid && e != nullptr && e->GetClassName() != nullptr &&
							std::strcmp(e->GetClassName(), "emp_vehicle") == 0 && e->GetCollideable() != nullptr)
						{
							const Vector vehiclePos = e->GetCollideable()->GetCollisionOrigin();
							Vector botVelocity(0, 0, 0);
							CClassInterface::getVelocity(bot->getEdict(), &botVelocity);
							RCBOT_DIAG("[RCBOT2] navmesh-stuck-vehicle userid=%d veh=%d vehicle_pos=(%.0f %.0f %.0f) dist2d=%.0f dz=%.0f bot_speed=%.0f\n",
								engine != nullptr ? engine->GetPlayerUserId(bot->getEdict()) : -1,
								engine->IndexOfEdict(e), vehiclePos.x, vehiclePos.y, vehiclePos.z,
								(vehiclePos - here).Length2D(), vehiclePos.z - here.z, botVelocity.Length());
						}
					}
				}
				// Target straight below through a floor: the mesh dropped from an
				// upper level through solid ground (as_escort (4488 2612): bot at -16,
				// point at -206, side traces free). A world hit on the way down blocks it.
				if (!worldBlocked && enginetrace != nullptr && here.z - target.z > 64.0f &&
					(target - here).Length2D() < 64.0f)
				{
					Ray_t ray;
					ray.Init(here + Vector(0, 0, 2), target + Vector(0, 0, 2));
					CTraceFilterWorldOnly worldOnly;
					trace_t down;
					enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &worldOnly, &down);
					RCBOT_DIAG("[RCBOT2] navmesh-stuck-floor drop=%.0f frac=%.2f\n", here.z - target.z, down.fraction);
					if (!down.startsolid && down.fraction < 1.0f)
					{
						worldBlocked = true;
						// The roof edge carries several such links side by side (as_escort
						// (4488 2612) then (4500 2575)): block them all at once.
						const int blockedLinks = RCBotNavMesh::BlockDropsUnderFloor(here, 128.0f);
						RCBOT_DIAG("[RCBOT2] navmesh-drop-under-floor links=%d\n", blockedLinks);
					}
				}
				// The mesh links this step through geometry: forbid the link so the
				// retry plans around it (crossroads (3054 -6393), (-217 -9552)).
				const float blockSeconds = worldBlocked ? 0.0f : 60.0f;
				if (doorBlocked && !worldBlocked)
				{
					worldBlocked = true;
					// The gate spans several mesh links (as_escort gate 228: 39->93, 39->6792,
					// 39->967 one after the other, 12 stops in 6 min): block all of them.
					int links = 0;
					ICollideable* collide = closedDoor != nullptr ? closedDoor->GetCollideable() : nullptr;
					Vector mins(0, 0, 0), maxs(0, 0, 0);
					if (collide != nullptr)
					{
						collide->WorldSpaceSurroundingBounds(&mins, &maxs);
						links = RCBotNavMesh::BlockEdgesThroughBox(mins - Vector(8, 8, 0), maxs + Vector(8, 8, 0), blockSeconds);
						links += RCBotNavMesh::BlockEdgesBehindBox(here, mins - Vector(8, 8, 0), maxs + Vector(8, 8, 0), blockSeconds);
					}
					RCBOT_DIAG("[RCBOT2] navmesh-door-closed door=%d class=%s block_seconds=%.0f links=%d box=(%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n",
						closedDoor != nullptr ? engine->IndexOfEdict(closedDoor) : -1,
						closedDoor != nullptr ? closedDoor->GetClassName() : "-", blockSeconds, links,
						mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z);
				}
				if (worldBlocked)
				{
					const int exactArea = RCBotNavMesh::GetAreaIdAtPosition(here);
					const int nearArea = RCBotNavMesh::GetNearestAreaId(here);
					bool blocked = false;
					for (std::size_t i = 0; !blocked && i + 1 < route.path.areaIds.size(); ++i)
					{
						const int id = static_cast<int>(route.path.areaIds[i]);
						if (id != exactArea && id != nearArea)
							continue;
						RCBotNavMesh::BlockEdge(route.path.areaIds[i], route.path.areaIds[i + 1], blockSeconds);
						RCBOT_DIAG("[RCBOT2] navmesh-edge-blocked from=%u to=%u\n",
							route.path.areaIds[i], route.path.areaIds[i + 1]);
						blocked = true;
					}
					if (!blocked)
					{
						unsigned fromId = 0, toId = 0;
						if (RCBotNavMesh::BlockEdgeNearestOnPath(here, route.path.areaIds, fromId, toId, blockSeconds))
							RCBOT_DIAG("[RCBOT2] navmesh-edge-blocked from=%u to=%u (nearest)\n", fromId, toId);
						else
							RCBOT_DIAG("[RCBOT2] navmesh-edge-block-miss exact=%d near=%d areas=%zu\n",
								exactArea, nearArea, route.path.areaIds.size());
					}
				}
			}
			bot->handleNavigationProviderFailure(
				RCBotNavigation::Provider::NAVMESH,
				RCBotNavigation::FailureClass::MOVEMENT,
				RCBotNavigation::FailureReason::PATH_STUCK,
				RCBotNavigation::Result::PATH_STUCK,
				"navmesh-no-progress-timeout");
			resetRoute(route);
			continue;
		}

		{
			Vector aim;
			edict_t* door = DoorAhead(bot, route.path.points[route.point], aim);
			if (door != nullptr)
			{
				bot->setLookVector(aim);
				bot->setLookAtTask(LOOK_VECTOR, 1.0f);
				if (engine->Time() - route.lastDoorUse > 2.0f && bot->isFacing(aim))
				{
					bot->use();
					route.lastDoorUse = engine->Time();
					RCBOT_DIAG("[RCBOT2] navmesh-door-use door=%d class=%s bot=(%.0f %.0f %.0f)\n",
						engine->IndexOfEdict(door), door->GetClassName(),
						bot->getOrigin().x, bot->getOrigin().y, bot->getOrigin().z);
				}
			}
		}

		{
			// The mesh links ledges up to 64 high; above step height they need a jump.
			const Vector toPoint = route.path.points[route.point] - bot->getOrigin();
			// CBot::jump holds IN_JUMP for 0.5 s. Re-pressing every frame kept the button down for
			// good, and CGameMovement::CheckJumpButton (0x3f56a0) ignores a held jump (old-buttons
			// check): one jump per ledge, then navmesh-stuck. Let go between presses.
			bool ladderTarget = false;
			for (const Vector& ladderPoint : route.path.ladderPoints)
				ladderTarget = ladderTarget || (ladderPoint - route.path.points[route.point]).LengthSqr() < 1.0f;
			// A jump at a ladder throws the bot off it again.
			if (!ladderTarget && toPoint.z > kStepHeight && toPoint.Length2D() < 96.0f &&
				engine->Time() - route.lastJump > 0.6f &&
				(CClassInterface::getPlayerFlags(bot->getEdict()) & FL_ONGROUND) != 0)
			{
				bot->jump();
				route.lastJump = engine->Time();
				++route.jumps;
			}
		}

		// Ladder: look at the point we climb to; forward movement on a ladder
		// follows the view pitch.
		for (const Vector& ladderPoint : route.path.ladderPoints)
			if (route.point < route.path.points.size() &&
				(ladderPoint - route.path.points[route.point]).LengthSqr() < 1.0f)
			{
				bot->setLookVector(ladderPoint + Vector(0, 0, 48));
				bot->setLookAtTask(LOOK_VECTOR, 0.5f);
				break;
			}

		if (!bot->setNavigationMoveTarget(route.intentId,
			RCBotNavigation::Provider::NAVMESH, route.attemptId,
			route.path.points[route.point]))
		{
			resetRoute(route);
		}
	}
}

namespace
{
bool s_doorButtonsParsed = false;
std::vector<DoorButton> s_doorButtons;

const char* EntityTargetName(edict_t* entity)
{
	CBaseEntity* base = entity != nullptr && !entity->IsFree() && entity->GetUnknown() != nullptr ?
		entity->GetUnknown()->GetBaseEntity() : nullptr;
	const datamap_t* map = base != nullptr ? CBaseEntity_GetDataDescMap(base) : nullptr;
	const unsigned offset = map != nullptr ? UTIL_FindInDataMap(map, "m_iName") : 0;
	return offset != 0 ? *reinterpret_cast<const char* const*>(reinterpret_cast<char*>(base) + offset) :
		nullptr;
}

struct MapOutput
{
	std::string target;
	std::string input;
};

struct MapEntity
{
	std::string classname;
	std::string targetname;
	std::string filterteam;
	std::string negated;
	std::vector<std::pair<std::string, MapOutput>> outputs;
};

// "OnPressed" "door1,Toggle,,0,-1" (older maps separate with ESC 0x1b).
MapOutput ParseOutput(const std::string& value)
{
	MapOutput out;
	const char sep = value.find('\x1b') != std::string::npos ? '\x1b' : ',';
	const std::size_t a = value.find(sep);
	out.target = value.substr(0, a);
	if (a != std::string::npos)
	{
		const std::size_t b = value.find(sep, a + 1);
		out.input = value.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
	}
	return out;
}

std::vector<MapEntity> ParseMapEntities()
{
	std::vector<MapEntity> entities;
	const char* text = engine != nullptr ? engine->GetMapEntitiesString() : nullptr;
	if (text == nullptr)
		return entities;
	MapEntity current;
	bool inside = false;
	std::vector<std::string> tokens;
	for (const char* p = text; *p != '\0'; ++p)
	{
		if (*p == '{')
		{
			current = MapEntity();
			tokens.clear();
			inside = true;
		}
		else if (*p == '}' && inside)
		{
			for (std::size_t i = 0; i + 1 < tokens.size(); i += 2)
			{
				const std::string& key = tokens[i];
				const std::string& value = tokens[i + 1];
				if (key == "classname")
					current.classname = value;
				else if (key == "targetname")
					current.targetname = value;
				else if (key == "filterteam")
					current.filterteam = value;
				else if (key == "Negated")
					current.negated = value;
				else if (key.size() > 2 && key[0] == 'O' && key[1] == 'n')
					current.outputs.emplace_back(key, ParseOutput(value));
			}
			entities.push_back(current);
			inside = false;
		}
		else if (*p == '"' && inside)
		{
			const char* end = std::strchr(p + 1, '"');
			if (end == nullptr)
				break;
			tokens.emplace_back(p + 1, end);
			p = end;
		}
	}
	return entities;
}

bool OpensDoor(const std::string& input)
{
	return input == "Toggle" || input == "Open";
}

std::vector<edict_t*> EntitiesNamed(const std::string& name, const char* classPrefix)
{
	std::vector<edict_t*> found;
	if (gpGlobals == nullptr || name.empty())
		return found;
	for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities; ++index)
	{
		edict_t* entity = INDEXENT(index);
		const char* cls = entity != nullptr && !entity->IsFree() ? entity->GetClassName() : nullptr;
		if (cls == nullptr || std::strncmp(cls, classPrefix, std::strlen(classPrefix)) != 0)
			continue;
		const char* targetName = EntityTargetName(entity);
		if (targetName != nullptr && name == targetName)
			found.push_back(entity);
	}
	return found;
}

void ParseDoorButtons()
{
	s_doorButtonsParsed = true;
	s_doorButtons.clear();
	const std::vector<MapEntity> entities = ParseMapEntities();
	auto addDoors = [&](const std::string& buttonName, const std::string& doorName, int team) {
		for (edict_t* button : EntitiesNamed(buttonName, "func_button"))
			for (edict_t* door : EntitiesNamed(doorName, "func_door"))
			{
				DoorButton entry;
				entry.button = engine->IndexOfEdict(button);
				entry.door = engine->IndexOfEdict(door);
				entry.team = team;
				entry.origin = CBotGlobals::entityOrigin(button);
				s_doorButtons.push_back(entry);
				RCBOT_DIAG("[RCBOT2] door-button map button=%d(%s) door=%d(%s) team=%d at=(%.0f %.0f %.0f)\n",
					entry.button, buttonName.c_str(), entry.door, doorName.c_str(), team,
					entry.origin.x, entry.origin.y, entry.origin.z);
			}
	};
	for (const MapEntity& button : entities)
	{
		if (button.classname != "func_button" || button.targetname.empty())
			continue;
		for (const auto& output : button.outputs)
		{
			if (output.first != "OnPressed")
				continue;
			if (OpensDoor(output.second.input))
			{
				addDoors(button.targetname, output.second.target, 0);
				continue;
			}
			if (output.second.input != "TestActivator")
				continue;
			// Team filter: OnPass runs for the filter team (or every other team if negated).
			for (const MapEntity& filter : entities)
			{
				if (filter.classname != "filter_activator_team" || filter.targetname != output.second.target)
					continue;
				const int filterTeam = std::atoi(filter.filterteam.c_str());
				if (filterTeam < 2 || filterTeam > 3)
					continue;
				const int team = std::atoi(filter.negated.c_str()) != 0 ? 5 - filterTeam : filterTeam;
				for (const auto& pass : filter.outputs)
					if (pass.first == "OnPass" && OpensDoor(pass.second.input))
						addDoors(button.targetname, pass.second.target, team);
			}
		}
	}
}
}

bool DoorButtonPressable(const DoorButton& entry)
{
	edict_t* button = INDEXENT(entry.button);
	edict_t* door = INDEXENT(entry.door);
	if (button == nullptr || button->IsFree() || door == nullptr || door->IsFree())
		return false;
	int state = -1;
	return DoorDatamapInt(door, "m_toggle_state", state) && state == 1;
}

bool DoorButtonLocked(const DoorButton& entry)
{
	int locked = 0;
	return DoorDatamapInt(INDEXENT(entry.button), "m_bLocked", locked) && (locked & 0xff) != 0;
}

bool FindDoorButton(int team, const Vector& from, float maxDistance, DoorButton& out)
{
	if (engine == nullptr || gpGlobals == nullptr)
		return false;
	if (!s_doorButtonsParsed)
		ParseDoorButtons();
	float best = maxDistance;
	bool found = false;
	for (const DoorButton& entry : s_doorButtons)
	{
		if (entry.team != 0 && entry.team != team)
			continue;
		const float distance = (entry.origin - from).Length2D();
		if (distance >= best || !DoorButtonPressable(entry))
			continue;
		best = distance;
		out = entry;
		found = true;
	}
	return found;
}

void LevelShutdown()
{
	s_doorButtonsParsed = false;
	s_doorButtons.clear();
	for (Route& route : s_routes)
	{
		if (route.active)
			finishRoute(route, RCBotNavigation::Result::CANCELLED,
				"navmesh-provider-level-shutdown");
		else
			resetRoute(route);
	}
}
}
