#include "bot_navmesh_api.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <queue>
#include <string>
#include <vector>

#include "collisionutils.h"
#include "convar.h"
#include "cmodel.h"
#include "eiface.h"
#include "IEngineTrace.h"
#include "ivdebugoverlay.h"
#include "iplayerinfo.h"
#include "ientityinfo.h"
#include "navmesh/nav_mesh/nav_area.h"
#include "navmesh/nav_mesh/nav_mesh.h"
#include "navmesh/util/UtilTrace.h"
#include "bot_getprop.h"

extern CNavMesh *TheNavMesh;
extern IVEngineServer *engine;
extern CGlobalVars *gpGlobals;
extern IServerGameEnts *servergameents;
extern IEngineTrace *enginetrace;
extern IVDebugOverlay *debugoverlay;
extern IPlayerInfoManager *playerinfomanager;
extern IEntityInfoManager *entityinfomanager;
extern NavAreaVector TheNavAreas;

namespace
{
constexpr float kHumanMovementRecordInterval = 1.0f / 15.0f;
bool s_humanMovementRecordActive = false;
int s_humanMovementRecordPlayerIndex = -1;
int s_humanMovementRecordUserId = -1;
int s_humanMovementRecordSample = 0;
float s_humanMovementRecordNextSample = 0.0f;
Vector s_humanMovementRecordPreviousOrigin = vec3_origin;
bool s_humanMovementRecordHasPrevious = false;

void PrintSegment(const Vector &from, const Vector &to, const char *label)
{
	trace_t trace;
	UTIL_TraceHull(from, to, Vector(-16.0f, -16.0f, 0.0f),
		Vector(16.0f, 16.0f, 72.0f), MASK_PLAYERSOLID, nullptr,
		COLLISION_GROUP_PLAYER_MOVEMENT, &trace);
	Msg("[RCBOT2] NavMesh PoC segment=%s fraction=%.3f startsolid=%d allsolid=%d from=(%.1f %.1f %.1f) to=(%.1f %.1f %.1f)\n",
		label, trace.fraction, trace.startsolid ? 1 : 0, trace.allsolid ? 1 : 0,
		from.x, from.y, from.z, to.x, to.y, to.z);
}

void PrintSegmentVariant(const Vector &from, const Vector &to,
	const char *label, const unsigned int mask, const int collisionGroup)
{
	trace_t trace;
	UTIL_TraceHull(from, to, Vector(-16.0f, -16.0f, 0.0f),
		Vector(16.0f, 16.0f, 72.0f), mask, nullptr, collisionGroup, &trace);
	Msg("[RCBOT2] NavMesh PoC variant=%s mask=0x%x group=%d fraction=%.3f startsolid=%d allsolid=%d hit=%d\n",
		label, mask, collisionGroup, trace.fraction,
		trace.startsolid ? 1 : 0, trace.allsolid ? 1 : 0,
		trace.DidHit() ? 1 : 0);
}

bool BoxOverlaps(const Vector &aMin, const Vector &aMax,
	const Vector &bMin, const Vector &bMax)
{
	return !(aMin.x > bMax.x || aMax.x < bMin.x ||
		aMin.y > bMax.y || aMax.y < bMin.y ||
		aMin.z > bMax.z || aMax.z < bMin.z);
}

const char *SafeClassName(const edict_t *entity)
{
	return entity != nullptr && entity->GetClassName() != nullptr ?
		entity->GetClassName() : "<none>";
}

const char *SafeModelName(const edict_t *entity)
{
	if (entity == nullptr || entity->GetIServerEntity() == nullptr)
		return "<none>";
	const char *model = entity->GetIServerEntity()->GetModelName().ToCStr();
	return model != nullptr && *model != '\0' ? model : "<none>";
}

bool GetEntityWorldBounds(edict_t *entity, Vector *mins, Vector *maxs)
{
	if (entity == nullptr || entity->GetCollideable() == nullptr ||
		mins == nullptr || maxs == nullptr)
		return false;
	entity->GetCollideable()->WorldSpaceSurroundingBounds(mins, maxs);
	return maxs->x >= mins->x && maxs->y >= mins->y && maxs->z >= mins->z;
}

edict_t *FindCollisionProbeEntity(const int requestedIndex,
	const char *expectedClassname)
{
	if (engine == nullptr || gpGlobals == nullptr)
		return nullptr;

	if (requestedIndex > gpGlobals->maxClients &&
		requestedIndex < gpGlobals->maxEntities)
	{
		edict_t *requested = engine->PEntityOfEntIndex(requestedIndex);
		if (requested != nullptr && !requested->IsFree() &&
			requested->GetCollideable() != nullptr &&
			(expectedClassname == nullptr ||
				std::strcmp(SafeClassName(requested), expectedClassname) == 0))
			return requested;
	}

	if (expectedClassname == nullptr)
		return nullptr;
	for (int index = gpGlobals->maxClients + 1;
		index < gpGlobals->maxEntities; ++index)
	{
		edict_t *entity = engine->PEntityOfEntIndex(index);
		if (entity == nullptr || entity->IsFree() ||
			entity->GetCollideable() == nullptr)
			continue;
		if (std::strcmp(SafeClassName(entity), expectedClassname) == 0)
			return entity;
	}
	return nullptr;
}

void PrintCollisionProbeTarget(const char *label, const int requestedIndex,
	edict_t *entity)
{
	if (entity == nullptr)
	{
		Msg("[RCBOT2] NavMesh collision-probe target=%s requested_index=%d "
			"found=0\n", label, requestedIndex);
		return;
	}

	Vector mins = vec3_origin;
	Vector maxs = vec3_origin;
	GetEntityWorldBounds(entity, &mins, &maxs);
	const ICollideable *collideable = entity->GetCollideable();
	const Vector origin = collideable != nullptr ?
		collideable->GetCollisionOrigin() : vec3_origin;
	Msg("[RCBOT2] NavMesh collision-probe target=%s requested_index=%d "
		"found=1 entity_index=%d classname=%s model=%s "
		"origin=(%.1f %.1f %.1f) world_mins=(%.1f %.1f %.1f) "
		"world_maxs=(%.1f %.1f %.1f) solid_type=%d collision_group=%d\n",
		label, requestedIndex, engine->IndexOfEdict(entity),
		SafeClassName(entity), SafeModelName(entity), origin.x, origin.y,
		origin.z, mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z,
		collideable != nullptr ? static_cast<int>(collideable->GetSolid()) : -1,
		collideable != nullptr ? collideable->GetCollisionGroup() : -1);
}

edict_t *CollisionProbeHitEntity(const trace_t &trace)
{
	if (trace.m_pEnt == nullptr || servergameents == nullptr)
		return nullptr;
	return servergameents->BaseEntityToEdict(trace.m_pEnt);
}

edict_t *FindHumanMovementRecordPlayer()
{
	if (engine == nullptr || gpGlobals == nullptr || playerinfomanager == nullptr)
		return nullptr;

	for (int index = 1; index <= gpGlobals->maxClients; ++index)
	{
		edict_t *player = engine->PEntityOfEntIndex(index);
		if (player == nullptr || player->IsFree() ||
			player->GetCollideable() == nullptr)
			continue;
		IPlayerInfo *info = playerinfomanager->GetPlayerInfo(player);
		if (info == nullptr || !info->IsConnected() || !info->IsPlayer() ||
			info->IsFakeClient() || info->IsHLTV() || info->IsObserver())
			continue;
		return player;
	}
	return nullptr;
}

void ResetHumanMovementRecorder()
{
	s_humanMovementRecordActive = false;
	s_humanMovementRecordPlayerIndex = -1;
	s_humanMovementRecordUserId = -1;
	s_humanMovementRecordSample = 0;
	s_humanMovementRecordNextSample = 0.0f;
	s_humanMovementRecordPreviousOrigin = vec3_origin;
	s_humanMovementRecordHasPrevious = false;
}

void RecordHumanMovementFrameInternal()
{
	if (!s_humanMovementRecordActive || gpGlobals == nullptr ||
		engine == nullptr || playerinfomanager == nullptr || enginetrace == nullptr)
		return;
	if (gpGlobals->curtime + 0.0001f < s_humanMovementRecordNextSample)
		return;
	s_humanMovementRecordNextSample = gpGlobals->curtime +
		kHumanMovementRecordInterval;

	edict_t *player = engine->PEntityOfEntIndex(s_humanMovementRecordPlayerIndex);
	IPlayerInfo *info = player != nullptr && !player->IsFree() ?
		playerinfomanager->GetPlayerInfo(player) : nullptr;
	if (player == nullptr || info == nullptr || !info->IsConnected() ||
		!info->IsPlayer() || info->IsFakeClient() || info->IsHLTV() ||
		info->IsObserver() || engine->GetPlayerUserId(player) !=
			s_humanMovementRecordUserId)
	{
		Msg("[RCBOT2] Nav movement-record event=ended reason=human-unavailable "
			"samples=%d\n", s_humanMovementRecordSample);
		ResetHumanMovementRecorder();
		return;
	}

	const Vector origin = info->GetAbsOrigin();
	Vector velocity = vec3_origin;
	bool velocityKnown = CClassInterface::getVelocity(player, &velocity);
	if (!velocityKnown && entityinfomanager != nullptr)
	{
		IEntityInfo *entityInfo = entityinfomanager->GetEntityInfo(player);
		if (entityInfo != nullptr)
		{
			velocity = entityInfo->GetAbsVelocity();
			velocityKnown = velocity.IsValid();
		}
	}
	if (!velocityKnown)
		velocityKnown = CClassInterface::getVelocity(player, &velocity);

	const int flags = CClassInterface::getPlayerFlags(player);
	const int moveType = CClassInterface::getMoveType(player);
	const bool onGround = (flags & FL_ONGROUND) != 0;
	const bool ducking = (flags & FL_DUCKING) != 0;
	const Vector hullMins = info->GetPlayerMins();
	const Vector hullMaxs = info->GetPlayerMaxs();
	edict_t *groundEntity = CClassInterface::getGroundEntity(player);
	if (groundEntity != nullptr && groundEntity->IsFree())
		groundEntity = nullptr;

	const Vector groundTraceStart = origin + Vector(0.0f, 0.0f, 4.0f);
	const Vector groundTraceEnd = origin - Vector(0.0f, 0.0f, 12.0f);
	trace_t groundTrace{};
		UTIL_TraceLine(groundTraceStart, groundTraceEnd, MASK_PLAYERSOLID,
			player->GetIServerEntity(), COLLISION_GROUP_PLAYER_MOVEMENT, &groundTrace);
	edict_t *traceEntity = CollisionProbeHitEntity(groundTrace);
	const char *surfaceName = groundTrace.surface.name != nullptr &&
		*groundTrace.surface.name != '\0' ? groundTrace.surface.name : "<none>";
	const float delta = s_humanMovementRecordHasPrevious ?
		(origin - s_humanMovementRecordPreviousOrigin).Length() : 0.0f;
	const float deltaXY = s_humanMovementRecordHasPrevious ?
		(origin - s_humanMovementRecordPreviousOrigin).Length2D() : 0.0f;
	const float deltaZ = s_humanMovementRecordHasPrevious ?
		origin.z - s_humanMovementRecordPreviousOrigin.z : 0.0f;
	const float speed = velocityKnown ? velocity.Length2D() : 0.0f;
	const int groundIndex = groundEntity != nullptr ?
		engine->IndexOfEdict(groundEntity) : -1;
	const int traceIndex = traceEntity != nullptr ?
		engine->IndexOfEdict(traceEntity) : -1;
	Msg("[RCBOT2] Nav movement-record sample=%d time=%.3f userid=%d index=%d "
		"origin=(%.2f %.2f %.2f) velocity=(%.2f %.2f %.2f) "
		"velocity_known=%d speed=%.2f flags=0x%x on_ground=%d ducking=%d "
		"move_type=%d ground_entity_index=%d ground_entity_class=%s "
		"hull_mins=(%.1f %.1f %.1f) hull_maxs=(%.1f %.1f %.1f) "
		"delta=%.2f delta_xy=%.2f delta_z=%.2f "
		"ground_trace_start=(%.2f %.2f %.2f) ground_trace_end=(%.2f %.2f %.2f) "
		"ground_fraction=%.6f ground_startsolid=%d ground_allsolid=%d "
		"ground_end=(%.2f %.2f %.2f) ground_normal=(%.3f %.3f %.3f) "
		"ground_trace_entity_index=%d ground_trace_entity_class=%s surface=%s\n",
		s_humanMovementRecordSample, gpGlobals->curtime,
		s_humanMovementRecordUserId, s_humanMovementRecordPlayerIndex,
		origin.x, origin.y, origin.z, velocity.x, velocity.y, velocity.z,
		velocityKnown ? 1 : 0, speed, flags, onGround ? 1 : 0,
		ducking ? 1 : 0, moveType, groundIndex,
		groundEntity != nullptr ? SafeClassName(groundEntity) : "<none>",
		hullMins.x, hullMins.y, hullMins.z, hullMaxs.x, hullMaxs.y,
		hullMaxs.z, delta, deltaXY, deltaZ, groundTraceStart.x,
		groundTraceStart.y, groundTraceStart.z, groundTraceEnd.x,
		groundTraceEnd.y, groundTraceEnd.z, groundTrace.fraction,
		groundTrace.startsolid ? 1 : 0, groundTrace.allsolid ? 1 : 0,
		groundTrace.endpos.x, groundTrace.endpos.y, groundTrace.endpos.z,
		groundTrace.plane.normal.x, groundTrace.plane.normal.y,
		groundTrace.plane.normal.z, traceIndex,
		traceEntity != nullptr ? SafeClassName(traceEntity) : "<none>",
		surfaceName);
	s_humanMovementRecordPreviousOrigin = origin;
	s_humanMovementRecordHasPrevious = true;
	++s_humanMovementRecordSample;
}

void PrintCollisionProbeTrace(const char *label, const char *method,
	const Vector &from, const Vector &to, const trace_t &trace,
	edict_t *target)
{
	edict_t *hitEntity = CollisionProbeHitEntity(trace);
	const char *surfaceName = trace.surface.name != nullptr &&
		*trace.surface.name != '\0' ? trace.surface.name : "<none>";
	const int targetIndex = target != nullptr ? engine->IndexOfEdict(target) : -1;
	const int hitIndex = hitEntity != nullptr ? engine->IndexOfEdict(hitEntity) : -1;
	Msg("[RCBOT2] NavMesh collision-probe label=%s method=%s "
		"target_index=%d target_classname=%s start=(%.1f %.1f %.1f) "
		"end=(%.1f %.1f %.1f) fraction=%.6f startsolid=%d allsolid=%d "
		"endpos=(%.1f %.1f %.1f) hit=%d hit_entity_index=%d "
		"hit_entity_classname=%s contents=0x%x surface=%s "
		"surface_props=%d surface_flags=0x%x\n",
		label, method, targetIndex, target != nullptr ?
			SafeClassName(target) : "<none>", from.x, from.y, from.z,
		to.x, to.y, to.z, trace.fraction, trace.startsolid ? 1 : 0,
		trace.allsolid ? 1 : 0, trace.endpos.x, trace.endpos.y,
		trace.endpos.z, trace.DidHit() ? 1 : 0, hitIndex,
		hitEntity != nullptr ? SafeClassName(hitEntity) : "<none>",
		trace.contents, surfaceName, trace.surface.surfaceProps,
		trace.surface.flags);
}

void RunCollisionProbeSegment(const char *label, const Vector &from,
	const Vector &to, edict_t *barracks, edict_t *vehiclePhysbox)
{
	const Vector hullMins(-16.0f, -16.0f, 0.0f);
	const Vector hullMaxs(16.0f, 16.0f, 72.0f);
	Ray_t ray;
	ray.Init(from, to, hullMins, hullMaxs);

	trace_t normalTrace{};
	UTIL_TraceHull(from, to, hullMins, hullMaxs, MASK_PLAYERSOLID, nullptr,
		COLLISION_GROUP_PLAYER_MOVEMENT, &normalTrace);
	PrintCollisionProbeTrace(label, "normal-hull-trace", from, to,
		normalTrace, nullptr);

	struct ProbeTarget
	{
		const char *name;
		edict_t *entity;
	};
	const ProbeTarget targets[] = {
		{ "barracks-entity-94", barracks },
		{ "vehicle-physbox-506", vehiclePhysbox }
	};
	bool anyEntityHit = false;
	float earliestFraction = 1.0f;
	for (const ProbeTarget &probe : targets)
	{
		if (probe.entity == nullptr || probe.entity->GetCollideable() == nullptr)
			continue;

		trace_t entityTrace{};
		enginetrace->ClipRayToEntity(ray, MASK_ALL,
			probe.entity->GetIServerEntity(), &entityTrace);
		PrintCollisionProbeTrace(label, "ClipRayToEntity", from, to,
			entityTrace, probe.entity);
		anyEntityHit = anyEntityHit || entityTrace.DidHit();
		earliestFraction = std::min(earliestFraction, entityTrace.fraction);

		trace_t collideableTrace{};
		enginetrace->ClipRayToCollideable(ray, MASK_ALL,
			probe.entity->GetCollideable(), &collideableTrace);
		PrintCollisionProbeTrace(label, "ClipRayToCollideable", from, to,
			collideableTrace, probe.entity);
		anyEntityHit = anyEntityHit || collideableTrace.DidHit();
		earliestFraction = std::min(earliestFraction,
			collideableTrace.fraction);
	}
	Msg("[RCBOT2] NavMesh collision-probe label=%s method=both-entities "
		"entity_hit=%d earliest_fraction=%.6f\n", label,
		anyEntityHit ? 1 : 0, earliestFraction);
}

void PrintCollisionProbePointState(const char *label, const Vector &point,
	edict_t *barracks, edict_t *vehiclePhysbox)
{
	const Vector hullMins(-16.0f, -16.0f, 0.0f);
	const Vector hullMaxs(16.0f, 16.0f, 72.0f);
	Ray_t ray;
	ray.Init(point, point, hullMins, hullMaxs);
	trace_t barracksTrace{};
	trace_t physboxTrace{};
	if (barracks != nullptr && barracks->GetCollideable() != nullptr)
		enginetrace->ClipRayToCollideable(ray, MASK_ALL,
			barracks->GetCollideable(), &barracksTrace);
	if (vehiclePhysbox != nullptr &&
		vehiclePhysbox->GetCollideable() != nullptr)
		enginetrace->ClipRayToCollideable(ray, MASK_ALL,
			vehiclePhysbox->GetCollideable(), &physboxTrace);
	Msg("[RCBOT2] NavMesh collision-probe point label=%s point=(%.1f %.1f %.1f) "
		"barracks_state=%s barracks_fraction=%.6f barracks_startsolid=%d "
		"physbox_state=%s physbox_fraction=%.6f physbox_startsolid=%d\n",
		label, point.x, point.y, point.z,
		barracksTrace.startsolid || barracksTrace.allsolid ? "solid" : "free",
		barracksTrace.fraction, barracksTrace.startsolid ? 1 : 0,
		physboxTrace.startsolid || physboxTrace.allsolid ? "solid" : "free",
		physboxTrace.fraction, physboxTrace.startsolid ? 1 : 0);
}

void RunCollisionProbeScan(const Vector &referenceStart, const Vector &portal,
	edict_t *barracks, edict_t *vehiclePhysbox)
{
	if (barracks == nullptr)
		return;

	Vector mins = vec3_origin;
	Vector maxs = vec3_origin;
	if (!GetEntityWorldBounds(barracks, &mins, &maxs))
		return;
	const Vector origin = barracks->GetCollideable()->GetCollisionOrigin();
	const float halfX = (maxs.x - mins.x) * 0.5f;
	const float halfY = (maxs.y - mins.y) * 0.5f;
	const float probeZ = referenceStart.z;
	const Vector interiorPoint(origin.x, origin.y, probeZ);
	PrintCollisionProbePointState("interior-reference-height", interiorPoint,
		barracks, vehiclePhysbox);
	PrintCollisionProbePointState("known-start", referenceStart, barracks,
		vehiclePhysbox);
	PrintCollisionProbePointState("known-portal", portal, barracks,
		vehiclePhysbox);

	const float scale[] = { -1.25f, -0.75f, -0.25f, 0.25f, 0.75f, 1.25f };
	for (const float xScale : scale)
	{
		for (const float yScale : scale)
		{
			const Vector point(origin.x + halfX * xScale,
				origin.y + halfY * yScale, probeZ);
			Msg("[RCBOT2] NavMesh collision-probe scan_grid x_scale=%.2f "
				"y_scale=%.2f\n", xScale, yScale);
			PrintCollisionProbePointState("grid", point, barracks,
				vehiclePhysbox);
		}
	}

	const Vector shortSegments[][2] = {
		{ Vector(origin.x - 64.0f, origin.y, probeZ),
			Vector(origin.x + 64.0f, origin.y, probeZ) },
		{ Vector(origin.x, origin.y - 64.0f, probeZ),
			Vector(origin.x, origin.y + 64.0f, probeZ) },
		{ Vector(origin.x + halfX - 64.0f, origin.y, probeZ),
			Vector(origin.x + halfX + 64.0f, origin.y, probeZ) },
		{ Vector(origin.x, origin.y + halfY - 64.0f, probeZ),
			Vector(origin.x, origin.y + halfY + 64.0f, probeZ) }
	};
	const char *shortLabels[] = {
		"short-center-x", "short-center-y", "short-front-boundary-x",
		"short-side-boundary-y"
	};
	for (int i = 0; i < 4; ++i)
		RunCollisionProbeSegment(shortLabels[i], shortSegments[i][0],
			shortSegments[i][1], barracks, vehiclePhysbox);
}

enum class CollisionMapperCellState
{
	FREE,
	BLOCKED_BY_BUILDING,
	NO_GROUND,
	INVALID
};

const char *CollisionMapperCellStateName(
	const CollisionMapperCellState state)
{
	switch (state)
	{
	case CollisionMapperCellState::FREE:
		return "FREE";
	case CollisionMapperCellState::BLOCKED_BY_BUILDING:
		return "BLOCKED_BY_BUILDING";
	case CollisionMapperCellState::NO_GROUND:
		return "NO_GROUND";
	case CollisionMapperCellState::INVALID:
		return "INVALID";
	}
	return "INVALID";
}

struct CollisionMapperHull
{
	edict_t *player = nullptr;
	int playerIndex = -1;
	Vector feet = vec3_origin;
	Vector mins = vec3_origin;
	Vector maxs = vec3_origin;
	bool fromPlayerInfo = false;
};

bool IsValidCollisionMapperHull(const Vector &mins, const Vector &maxs)
{
	return mins.IsValid() && maxs.IsValid() && maxs.x > mins.x &&
		maxs.y > mins.y && maxs.z > mins.z;
}

bool IsCollisionMapperInsideParentBounds(const Vector &xy,
	edict_t *parentBuilding)
{
	if (parentBuilding == nullptr || !xy.IsValid())
		return false;
	Vector mins = vec3_origin;
	Vector maxs = vec3_origin;
	if (!GetEntityWorldBounds(parentBuilding, &mins, &maxs))
		return false;
	return xy.x >= mins.x && xy.x <= maxs.x && xy.y >= mins.y &&
		xy.y <= maxs.y;
}

bool IsUsableCollisionMapperPlayer(edict_t *entity, IPlayerInfo **info)
{
	if (entity == nullptr || entity->IsFree() || entity->GetCollideable() == nullptr ||
		playerinfomanager == nullptr)
		return false;
	IPlayerInfo *playerInfo = playerinfomanager->GetPlayerInfo(entity);
	if (playerInfo == nullptr || !playerInfo->IsConnected() ||
		!playerInfo->IsPlayer() || playerInfo->IsObserver())
		return false;
	if (info != nullptr)
		*info = playerInfo;
	return true;
}

bool ResolveCollisionMapperHull(const Vector &requestedPosition,
	const int requestedPlayerIndex, CollisionMapperHull *result)
{
	if (result == nullptr || engine == nullptr || gpGlobals == nullptr ||
		playerinfomanager == nullptr)
		return false;

	edict_t *bestPlayer = nullptr;
	IPlayerInfo *bestInfo = nullptr;
	float bestScore = std::numeric_limits<float>::max();

	if (requestedPlayerIndex > 0 &&
		requestedPlayerIndex <= gpGlobals->maxClients)
	{
		edict_t *requestedPlayer =
			engine->PEntityOfEntIndex(requestedPlayerIndex);
		IPlayerInfo *requestedInfo = nullptr;
		if (IsUsableCollisionMapperPlayer(requestedPlayer, &requestedInfo))
		{
			bestPlayer = requestedPlayer;
			bestInfo = requestedInfo;
		}
	}

	if (bestPlayer == nullptr)
	{
		for (int index = 1; index <= gpGlobals->maxClients; ++index)
		{
			edict_t *candidate = engine->PEntityOfEntIndex(index);
			IPlayerInfo *candidateInfo = nullptr;
			if (!IsUsableCollisionMapperPlayer(candidate, &candidateInfo))
				continue;

			const Vector candidateFeet = candidateInfo->GetAbsOrigin();
			const float dx = candidateFeet.x - requestedPosition.x;
			const float dy = candidateFeet.y - requestedPosition.y;
			const float dz = candidateFeet.z - requestedPosition.z;
			// The legacy RCBot2 Empires origin helper may expose a +32 Z
			// compatibility offset. Prefer horizontal proximity and only use Z
			// as a weak tie-breaker when resolving the actual player hull.
			const float score = dx * dx + dy * dy + dz * dz * 0.0625f;
			if (score < bestScore)
			{
				bestScore = score;
				bestPlayer = candidate;
				bestInfo = candidateInfo;
			}
		}
	}

	if (bestPlayer == nullptr || bestInfo == nullptr)
		return false;

	result->player = bestPlayer;
	result->playerIndex = engine->IndexOfEdict(bestPlayer);
	result->feet = bestInfo->GetAbsOrigin();
	result->mins = bestInfo->GetPlayerMins();
	result->maxs = bestInfo->GetPlayerMaxs();
	result->fromPlayerInfo = IsValidCollisionMapperHull(result->mins,
		result->maxs);
	if (!result->fromPlayerInfo)
	{
		const ICollideable *collideable = bestPlayer->GetCollideable();
		if (collideable == nullptr)
			return false;
		result->mins = collideable->OBBMins();
		result->maxs = collideable->OBBMaxs();
	}
	return IsValidCollisionMapperHull(result->mins, result->maxs) &&
		result->feet.IsValid();
}

struct CollisionMapperGround
{
	bool valid = false;
	Vector feet = vec3_origin;
	Vector normal = Vector(0.0f, 0.0f, 1.0f);
	std::string surface = "<none>";
	const char *source = "none";
};

void CaptureCollisionMapperGroundTrace(const trace_t &trace,
	const char *source, CollisionMapperGround *result)
{
	if (result == nullptr)
		return;
	result->valid = true;
	result->feet = trace.endpos;
	result->normal = trace.plane.normal.IsZero() ?
		Vector(0.0f, 0.0f, 1.0f) : trace.plane.normal;
	const char *surfaceName = trace.surface.name != nullptr &&
		*trace.surface.name != '\0' ? trace.surface.name : "<none>";
	result->surface = surfaceName;
	result->source = source != nullptr ? source : "unknown";
}

CollisionMapperGround FindCollisionMapperGround(const Vector &xy,
	const float referenceZ, const Vector &hullMins, const Vector &hullMaxs,
	edict_t *parentBuilding)
{
	CollisionMapperGround result;
	if (enginetrace == nullptr || !xy.IsValid() ||
		!IsValidCollisionMapperHull(hullMins, hullMaxs))
		return result;

	const Vector start(xy.x, xy.y, referenceZ + 96.0f);
	const Vector end(xy.x, xy.y, referenceZ - 192.0f);
	Ray_t ray;
	ray.Init(start, end, hullMins, hullMaxs);

	// Inside a building's world bounds, prefer the parent collideable. The
	// building floor can be above the world brush; querying brush-only first
	// would otherwise select the lower map floor and mis-project the sample.
	const auto tryParentGround = [&]() -> bool
	{
		if (parentBuilding == nullptr ||
			parentBuilding->GetCollideable() == nullptr)
			return false;
		trace_t parentTrace{};
		enginetrace->ClipRayToCollideable(ray, MASK_ALL,
			parentBuilding->GetCollideable(), &parentTrace);
		if (parentTrace.startsolid || parentTrace.allsolid ||
			parentTrace.fraction >= 1.0f || !parentTrace.endpos.IsValid() ||
			std::fabs(parentTrace.endpos.z - referenceZ) > 64.0f ||
			parentTrace.plane.normal.z < 0.5f)
			return false;
		CaptureCollisionMapperGroundTrace(parentTrace, "parent-building", &result);
		return true;
	};

	if (IsCollisionMapperInsideParentBounds(xy, parentBuilding) &&
		tryParentGround())
		return result;

	// Outside the parent bounds, use brush-only collision so the parent model
	// cannot turn a valid map floor into a false NO_GROUND result.
	trace_t trace{};
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, nullptr,
		&trace);
	if (!trace.startsolid && !trace.allsolid && trace.fraction < 1.0f &&
		trace.endpos.IsValid() &&
		std::fabs(trace.endpos.z - referenceZ) <= 64.0f)
	{
		CaptureCollisionMapperGroundTrace(trace, "world-brush", &result);
		return result;
	}

	// Also allow parent floors just outside the coarse bounds at an entrance,
	// where the actual collision can extend beyond the entity AABB used by
	// this diagnostic mapper.
	tryParentGround();
	return result;
}

bool ClipCollisionMapperHullToEntity(const Vector &from, const Vector &to,
	const Vector &hullMins, const Vector &hullMaxs, edict_t *entity,
	trace_t *trace)
{
	if (trace == nullptr)
		return false;
	*trace = trace_t{};
	if (enginetrace == nullptr || entity == nullptr ||
		entity->GetCollideable() == nullptr ||
		!IsValidCollisionMapperHull(hullMins, hullMaxs))
		return false;

	Ray_t ray;
	ray.Init(from, to, hullMins, hullMaxs);
	enginetrace->ClipRayToCollideable(ray, MASK_ALL,
		entity->GetCollideable(), trace);
	return true;
}

struct CollisionMapperCell
{
	int x = 0;
	int y = 0;
	Vector sample = vec3_origin;
	Vector feet = vec3_origin;
	Vector groundNormal = Vector(0.0f, 0.0f, 1.0f);
	const char *groundSource = "none";
	std::string groundSurface = "<none>";
	CollisionMapperCellState state = CollisionMapperCellState::INVALID;
	bool insideBuildingBounds = false;
	bool physboxSolid = false;
	float buildingFraction = 1.0f;
};

enum class CollisionMapperEdgeState
{
	WALK_FLAT,
	WALK_SLOPE,
	STEP_UP,
	DROP_DOWN,
	BLOCKED_BY_BUILDING,
	BLOCKED_BY_WORLD,
	NO_GROUND
};

const char *CollisionMapperEdgeStateName(const CollisionMapperEdgeState state)
{
	switch (state)
	{
	case CollisionMapperEdgeState::WALK_FLAT:
		return "WALK_FLAT";
	case CollisionMapperEdgeState::WALK_SLOPE:
		return "WALK_SLOPE";
	case CollisionMapperEdgeState::STEP_UP:
		return "STEP_UP";
	case CollisionMapperEdgeState::DROP_DOWN:
		return "DROP_DOWN";
	case CollisionMapperEdgeState::BLOCKED_BY_BUILDING:
		return "BLOCKED_BY_BUILDING";
	case CollisionMapperEdgeState::BLOCKED_BY_WORLD:
		return "BLOCKED_BY_WORLD";
	case CollisionMapperEdgeState::NO_GROUND:
		return "NO_GROUND";
	}
	return "NO_GROUND";
}

struct CollisionMapperEdge
{
	int from = -1;
	int to = -1;
	bool clear = false;
	CollisionMapperEdgeState state = CollisionMapperEdgeState::NO_GROUND;
	float fraction = 0.0f;
	float worldFraction = 0.0f;
	float fromGroundZ = 0.0f;
	float toGroundZ = 0.0f;
	float maxLocalGroundDelta = 0.0f;
};

struct CollisionMapperPortalSample
{
	int index = -1;
	float lateral = 0.0f;
	float inset = 0.0f;
	Vector candidate = vec3_origin;
	Vector feet = vec3_origin;
	bool insideGoalArea = false;
	bool valid = false;
	int gridCell = -1;
};

bool TraceCollisionMapperWorldHull(const Vector &from, const Vector &to,
	const Vector &hullMins, const Vector &hullMaxs, trace_t *trace)
{
	if (trace == nullptr)
		return false;
	*trace = trace_t{};
	if (enginetrace == nullptr || !IsValidCollisionMapperHull(hullMins,
		hullMaxs))
		return false;

	Ray_t ray;
	ray.Init(from, to, hullMins, hullMaxs);
	enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, nullptr, trace);
	return true;
}

bool TraceCollisionMapperWorldWallHull(const Vector &from, const Vector &to,
	const Vector &hullMins, const Vector &hullMaxs, trace_t *trace)
{
	// A hull whose feet are exactly on a brush floor can report startsolid for
	// the floor itself. Lift the diagnostic wall probe slightly so that this
	// floor contact is not mistaken for a vertical world obstruction.
	constexpr float kWallProbeLift = 4.0f;
	const Vector lift(0.0f, 0.0f, kWallProbeLift);
	return TraceCollisionMapperWorldHull(from + lift, to + lift, hullMins,
		hullMaxs, trace);
}

struct CollisionMapperEdgeEvaluation
{
	CollisionMapperEdgeState state = CollisionMapperEdgeState::NO_GROUND;
	bool traversable = false;
	float rawParentFraction = 1.0f;
	float horizontalParentFraction = 1.0f;
	float rawWorldFraction = 1.0f;
	float horizontalWorldFraction = 1.0f;
	float fromGroundZ = 0.0f;
	float toGroundZ = 0.0f;
	float maxLocalGroundDelta = 0.0f;
};

CollisionMapperEdgeEvaluation EvaluateCollisionMapperEdge(
	const CollisionMapperCell &from, const CollisionMapperCell &to,
	const Vector &hullMins, const Vector &hullMaxs, edict_t *parentBuilding)
{
	CollisionMapperEdgeEvaluation result;
	constexpr int kSurfaceSamples = 5;
	constexpr float kFlatTolerance = 4.0f;
	constexpr float kStepHeight = 18.0f;
	constexpr float kWalkableNormalZ = 0.7f;

	// If the direct 3D hull segment hits a mostly vertical parent surface, it
	// is a building wall. This check is only used for wall identification; a
	// hit with an upward-facing normal is still treated as floor/ramp contact
	// and is handled by the sampled 2.5D surface checks below.
	trace_t directParentTrace;
	const bool directParentProbe = ClipCollisionMapperHullToEntity(
		from.feet, to.feet, hullMins, hullMaxs, parentBuilding,
		&directParentTrace);
	const bool directParentWall = directParentProbe &&
		directParentTrace.DidHit() &&
		std::fabs(directParentTrace.plane.normal.z) < 0.5f;
	if (directParentWall)
	{
		result.state = CollisionMapperEdgeState::BLOCKED_BY_BUILDING;
		result.rawParentFraction = directParentTrace.fraction;
		result.horizontalParentFraction = directParentTrace.fraction;
		result.fromGroundZ = from.feet.z;
		result.toGroundZ = to.feet.z;
		result.maxLocalGroundDelta = std::fabs(to.feet.z - from.feet.z);
		return result;
	}

	std::vector<Vector> surfaceFeet;
	surfaceFeet.reserve(kSurfaceSamples);
	bool parentBlocked = false;
	bool worldBlocked = false;
	bool steepGround = false;
	for (int sampleIndex = 0; sampleIndex < kSurfaceSamples; ++sampleIndex)
	{
		const float t = static_cast<float>(sampleIndex) /
			static_cast<float>(kSurfaceSamples - 1);
		const Vector xy(from.feet.x + (to.feet.x - from.feet.x) * t,
			from.feet.y + (to.feet.y - from.feet.y) * t, 0.0f);
		const float referenceZ = from.feet.z +
			(to.feet.z - from.feet.z) * t;
		const CollisionMapperGround ground = FindCollisionMapperGround(xy,
			referenceZ, hullMins, hullMaxs, parentBuilding);
		if (!ground.valid || !ground.feet.IsValid())
		{
			result.state = CollisionMapperEdgeState::NO_GROUND;
			return result;
		}

		const Vector feet = ground.feet;
		surfaceFeet.push_back(feet);
		if (sampleIndex == 0)
			result.fromGroundZ = feet.z;
		if (sampleIndex == kSurfaceSamples - 1)
			result.toGroundZ = feet.z;
		if (ground.normal.z < kWalkableNormalZ)
			steepGround = true;

		trace_t parentPointTrace;
		if (ClipCollisionMapperHullToEntity(feet, feet, hullMins, hullMaxs,
			parentBuilding, &parentPointTrace))
		{
			parentBlocked = parentBlocked || parentPointTrace.startsolid ||
				parentPointTrace.allsolid || parentPointTrace.fraction < 1.0f;
			result.horizontalParentFraction = std::min(
				result.horizontalParentFraction, parentPointTrace.fraction);
		}

		if (sampleIndex == 0)
		{
			continue;
		}

		const Vector &previousFeet = surfaceFeet[surfaceFeet.size() - 2];
		const float localDelta = std::fabs(feet.z - previousFeet.z);
		result.maxLocalGroundDelta = std::max(result.maxLocalGroundDelta,
			localDelta);
		trace_t parentSurfaceTrace;
		if (ClipCollisionMapperHullToEntity(previousFeet, feet, hullMins,
			hullMaxs, parentBuilding, &parentSurfaceTrace))
		{
			result.rawParentFraction = std::min(result.rawParentFraction,
				parentSurfaceTrace.fraction);
			const bool rawBlocked = parentSurfaceTrace.startsolid ||
				parentSurfaceTrace.allsolid || parentSurfaceTrace.fraction < 1.0f;
			(void)rawBlocked;
		}

		// Test the horizontal hull corridor at the current surface height. A
		// raw 3D trace can report the floor/ramp's vertical transition as a
		// hit; that is not a building wall when the horizontal corridor is
		// clear and the intermediate ground samples are valid.
		const Vector horizontalFrom(previousFeet.x, previousFeet.y,
			previousFeet.z);
		const Vector horizontalTo(feet.x, feet.y, previousFeet.z);
		trace_t parentHorizontalTrace;
		if (ClipCollisionMapperHullToEntity(horizontalFrom, horizontalTo,
			hullMins, hullMaxs, parentBuilding, &parentHorizontalTrace))
		{
			result.horizontalParentFraction = std::min(
				result.horizontalParentFraction, parentHorizontalTrace.fraction);
			parentBlocked = parentBlocked || parentHorizontalTrace.startsolid ||
				parentHorizontalTrace.allsolid ||
				parentHorizontalTrace.fraction < 1.0f;
		}

		trace_t worldSurfaceTrace;
		if (TraceCollisionMapperWorldHull(previousFeet, feet, hullMins,
			hullMaxs, &worldSurfaceTrace))
		{
			result.rawWorldFraction = std::min(result.rawWorldFraction,
				worldSurfaceTrace.fraction);
			const bool rawWorldBlocked = worldSurfaceTrace.startsolid ||
				worldSurfaceTrace.allsolid || worldSurfaceTrace.fraction < 1.0f;
			(void)rawWorldBlocked;
		}

		trace_t worldHorizontalTrace;
		if (TraceCollisionMapperWorldWallHull(horizontalFrom, horizontalTo,
			hullMins, hullMaxs, &worldHorizontalTrace))
		{
			result.horizontalWorldFraction = std::min(
				result.horizontalWorldFraction, worldHorizontalTrace.fraction);
			// A brush floor can still report startsolid for a resting hull even
			// after the small vertical lift. Only a clean hit from a non-solid
			// start is evidence of a vertical world wall; valid ground samples
			// already establish that the floor itself is usable.
			worldBlocked = worldBlocked ||
				(!worldHorizontalTrace.startsolid &&
					!worldHorizontalTrace.allsolid &&
					worldHorizontalTrace.fraction < 1.0f);
		}
	}

	if (parentBlocked)
	{
		result.state = CollisionMapperEdgeState::BLOCKED_BY_BUILDING;
		return result;
	}
	if (worldBlocked)
	{
		result.state = CollisionMapperEdgeState::BLOCKED_BY_WORLD;
		return result;
	}
	if (steepGround)
	{
		result.state = CollisionMapperEdgeState::NO_GROUND;
		return result;
	}

	const float totalDelta = result.toGroundZ - result.fromGroundZ;
	if (std::fabs(totalDelta) <= kFlatTolerance)
		result.state = CollisionMapperEdgeState::WALK_FLAT;
	else if (result.maxLocalGroundDelta <= kStepHeight)
		result.state = CollisionMapperEdgeState::WALK_SLOPE;
	else if (totalDelta > 0.0f)
		result.state = CollisionMapperEdgeState::STEP_UP;
	else
		result.state = CollisionMapperEdgeState::DROP_DOWN;

	// Drops are kept directional in the diagnostic graph. This lets the
	// mapper answer whether a lower outside floor is reachable without
	// making a high-to-low transition imply the reverse is walkable.
	result.traversable = true;
	return result;
}

void DrawCollisionMapperCell(const CollisionMapperCell &cell,
	const float duration)
{
	if (debugoverlay == nullptr)
		return;

	int r = 160;
	int g = 0;
	int b = 160;
	switch (cell.state)
	{
	case CollisionMapperCellState::FREE:
		r = 0;
		g = 220;
		b = 0;
		break;
	case CollisionMapperCellState::BLOCKED_BY_BUILDING:
		r = 220;
		g = 0;
		b = 0;
		break;
	case CollisionMapperCellState::NO_GROUND:
		r = 220;
		g = 180;
		b = 0;
		break;
	case CollisionMapperCellState::INVALID:
		break;
	}
	const Vector center = cell.state == CollisionMapperCellState::FREE ?
		cell.feet : cell.sample;
	debugoverlay->AddBoxOverlay(center, Vector(-4.0f, -4.0f, -1.0f),
		Vector(4.0f, 4.0f, 1.0f), QAngle(0.0f, 0.0f, 0.0f), r, g, b,
		96, duration);
}

void PrintCollisionMapperPath(const std::vector<int> &path,
	const std::vector<CollisionMapperCell> &cells, const char *label)
{
	int turns = 0;
	int previousDX = 0;
	int previousDY = 0;
	float length = 0.0f;
	for (std::size_t i = 1; i < path.size(); ++i)
	{
		const CollisionMapperCell &previous = cells[path[i - 1]];
		const CollisionMapperCell &current = cells[path[i]];
		const int dx = current.x - previous.x;
		const int dy = current.y - previous.y;
		length += (current.feet - previous.feet).Length2D();
		if (i > 1 && (dx != previousDX || dy != previousDY))
			++turns;
		previousDX = dx;
		previousDY = dy;
	}
	Msg("[RCBOT2] NavMesh collision-mapper path label=%s cells=%d "
		"length=%.1f turns=%d s_geometry=%s\n", label,
		static_cast<int>(path.size()), length, turns,
		turns >= 2 ? "yes" : "no");
	for (std::size_t i = 0; i < path.size(); ++i)
	{
		const CollisionMapperCell &cell = cells[path[i]];
		Msg("[RCBOT2] NavMesh collision-mapper path_point label=%s step=%d "
			"grid=(%d %d) feet=(%.1f %.1f %.1f)\n", label,
			static_cast<int>(i), cell.x, cell.y, cell.feet.x, cell.feet.y,
			cell.feet.z);
	}
}

std::vector<int> BuildCollisionMapperPath(
	const std::vector<int> &predecessor, const int spawnCell, const int target)
{
	std::vector<int> result;
	if (target < 0 || target >= static_cast<int>(predecessor.size()) ||
		spawnCell < 0 || predecessor[target] < 0)
		return result;
	for (int current = target; current != spawnCell;
		current = predecessor[current])
	{
		if (current < 0 || predecessor[current] < 0)
		{
			result.clear();
			return result;
		}
		result.push_back(current);
	}
	result.push_back(spawnCell);
	std::reverse(result.begin(), result.end());
	return result;
}

struct CollisionMapperAdaptiveNode
{
	Vector feet = vec3_origin;
	int coarseCell = -1;
	int gridX = 0;
	int gridY = 0;
	bool spawnSide = false;
	bool portalSide = false;
};

struct CollisionMapperAdaptiveSample
{
	Vector xy = vec3_origin;
	int coarseCell = -1;
	bool hasGround = false;
	bool blockedByBuilding = false;
	std::vector<int> nodes;
	std::vector<float> groundZs;
	std::vector<float> freeGroundZs;
	std::vector<float> blockedGroundZs;
};

struct CollisionMapperAdaptiveBoundary
{
	Vector point = vec3_origin;
	float groundZ = 0.0f;
};

float CollisionMapperDistanceToPortal(
	const std::vector<CollisionMapperCell> &cells,
	const std::vector<CollisionMapperPortalSample> &portalSamples,
	const int cellIndex)
{
	float best = std::numeric_limits<float>::max();
	if (cellIndex < 0 || cellIndex >= static_cast<int>(cells.size()))
		return best;
	for (const CollisionMapperPortalSample &sample : portalSamples)
	{
		if (!sample.valid)
			continue;
		const float dx = cells[cellIndex].feet.x - sample.feet.x;
		const float dy = cells[cellIndex].feet.y - sample.feet.y;
		best = std::min(best, std::sqrt(dx * dx + dy * dy));
	}
	return best;
}

void PrintCollisionMapperAdaptivePath(
	const std::vector<int> &path,
	const std::vector<CollisionMapperAdaptiveNode> &nodes,
	const float resolution, const char *label)
{
	float length = 0.0f;
	int turns = 0;
	int previousDX = 0;
	int previousDY = 0;
	for (std::size_t index = 1; index < path.size(); ++index)
	{
		const CollisionMapperAdaptiveNode &from = nodes[path[index - 1]];
		const CollisionMapperAdaptiveNode &to = nodes[path[index]];
		length += (to.feet - from.feet).Length2D();
		const int dx = to.gridX - from.gridX;
		const int dy = to.gridY - from.gridY;
		if (index > 1 && (dx != previousDX || dy != previousDY))
			++turns;
		previousDX = dx;
		previousDY = dy;
	}
	Msg("[RCBOT2] NavMesh collision-mapper adaptive_path label=%s "
		"resolution=%.1f cells=%d length=%.1f turns=%d\n", label,
		resolution, static_cast<int>(path.size()), length, turns);
	for (std::size_t index = 0; index < path.size(); ++index)
	{
		const CollisionMapperAdaptiveNode &node = nodes[path[index]];
		Msg("[RCBOT2] NavMesh collision-mapper adaptive_path_point label=%s "
			"step=%d grid=(%d %d) coarse_cell=%d spawn_side=%d portal_side=%d "
			"feet=(%.1f %.1f %.1f)\n", label,
			static_cast<int>(index), node.gridX, node.gridY, node.coarseCell,
			node.spawnSide ? 1 : 0, node.portalSide ? 1 : 0, node.feet.x,
			node.feet.y, node.feet.z);
	}
}

struct CollisionMapper3DTopologyFloor
{
	Vector feet = vec3_origin;
	Vector normal = Vector(0.0f, 0.0f, 1.0f);
	std::string source = "none";
	std::string surface = "<none>";
	bool free = false;
	bool headroom = false;
	bool buildingBlocked = false;
	bool worldBlocked = false;
	bool buildingStartSolid = false;
	bool buildingAllSolid = false;
	bool worldStartSolid = false;
	bool worldAllSolid = false;
	float buildingFraction = 1.0f;
	float worldFraction = 1.0f;
};

struct CollisionMapper3DTopologySample
{
	Vector xy = vec3_origin;
	std::vector<CollisionMapper3DTopologyFloor> floors;
};

struct CollisionMapper3DTopologyNode
{
	int sample = -1;
	Vector feet = vec3_origin;
	bool lower = false;
	bool upper = false;
};

struct CollisionMapper3DTopologyEdge
{
	int from = -1;
	int to = -1;
	CollisionMapperEdgeEvaluation evaluation;
};

void DrawCollisionMapper3DTopologyFloor(
	const CollisionMapper3DTopologyFloor &floor, const float duration)
{
	if (debugoverlay == nullptr)
		return;

	int r = 220;
	int g = 180;
	int b = 0;
	if (floor.free)
	{
		if (floor.feet.z <= -240.0f)
		{
			r = 0;
			g = 220;
			b = 0;
		}
		else if (floor.feet.z >= -228.0f)
		{
			r = 0;
			g = 128;
			b = 255;
		}
		else
		{
			r = 255;
			g = 220;
			b = 0;
		}
	}
	else if (floor.buildingBlocked)
	{
		r = 220;
		g = 0;
		b = 0;
	}
	debugoverlay->AddBoxOverlay(floor.feet,
		Vector(-2.0f, -2.0f, -1.0f), Vector(2.0f, 2.0f, 1.0f),
		QAngle(0.0f, 0.0f, 0.0f), r, g, b, 128, duration);
}

bool CollisionMapper3DTopologySameXY(const Vector &a, const Vector &b)
{
	return std::fabs(a.x - b.x) <= 0.1f &&
		std::fabs(a.y - b.y) <= 0.1f;
}

void AddCollisionMapper3DTopologyFloor(
	std::vector<CollisionMapper3DTopologyFloor> *floors,
	const CollisionMapperGround &ground, const Vector &hullMins,
	const Vector &hullMaxs, edict_t *parentBuilding, edict_t *vehiclePhysbox)
{
	if (floors == nullptr || !ground.valid || !ground.feet.IsValid())
		return;

	CollisionMapper3DTopologyFloor candidate;
	candidate.feet = ground.feet;
	candidate.normal = ground.normal;
	candidate.source = ground.source != nullptr ? ground.source : "unknown";
	candidate.surface = ground.surface;

	trace_t buildingTrace;
	if (ClipCollisionMapperHullToEntity(candidate.feet, candidate.feet,
		hullMins, hullMaxs, parentBuilding, &buildingTrace))
	{
		candidate.buildingStartSolid = buildingTrace.startsolid;
		candidate.buildingAllSolid = buildingTrace.allsolid;
		candidate.buildingFraction = buildingTrace.fraction;
		candidate.buildingBlocked = buildingTrace.startsolid ||
			buildingTrace.allsolid || buildingTrace.fraction < 1.0f;
	}

	trace_t worldTrace;
	if (TraceCollisionMapperWorldWallHull(candidate.feet, candidate.feet,
		hullMins, hullMaxs, &worldTrace))
	{
		candidate.worldStartSolid = worldTrace.startsolid;
		candidate.worldAllSolid = worldTrace.allsolid;
		candidate.worldFraction = worldTrace.fraction;
		// The wall probe is lifted above the resting floor, so a hit here is
		// evidence of a world obstruction/headroom failure, not floor contact.
		candidate.worldBlocked = !worldTrace.startsolid &&
			!worldTrace.allsolid && worldTrace.fraction < 1.0f;
	}

	// The entity-specific standing-hull test includes the 72-unit player
	// height. Keep the explicit field in the diagnostic output so a floor
	// candidate is never mistaken for a point-only sample.
	candidate.headroom = !candidate.buildingBlocked &&
		!candidate.worldBlocked;
	candidate.free = candidate.normal.z >= 0.7f && candidate.headroom;

	for (std::size_t i = 0; i < floors->size(); ++i)
	{
		CollisionMapper3DTopologyFloor &existing = (*floors)[i];
		if (std::fabs(existing.feet.z - candidate.feet.z) > 2.0f)
			continue;
		// Prefer the parent collision's floor over a duplicate brush result at
		// the same Z. The physbox is intentionally never used for walkability.
		if (candidate.source == "parent-building" &&
			existing.source != "parent-building")
			existing = candidate;
		return;
	}

	floors->push_back(candidate);
	DrawCollisionMapper3DTopologyFloor(candidate, 30.0f);
	(void)vehiclePhysbox;
}

void SampleCollisionMapper3DTopologyPoint(
	const Vector &xy, const float referenceLevels[], const int levelCount,
	const Vector &hullMins, const Vector &hullMaxs, edict_t *parentBuilding,
	edict_t *vehiclePhysbox, std::vector<CollisionMapper3DTopologySample> *samples)
{
	if (samples == nullptr)
		return;

	for (std::size_t i = 0; i < samples->size(); ++i)
	{
		if (CollisionMapper3DTopologySameXY((*samples)[i].xy, xy))
			return;
	}

	CollisionMapper3DTopologySample sample;
	sample.xy = xy;
	for (int level = 0; level < levelCount; ++level)
	{
		const CollisionMapperGround parentGround = FindCollisionMapperGround(
			xy, referenceLevels[level], hullMins, hullMaxs, parentBuilding);
		AddCollisionMapper3DTopologyFloor(&sample.floors, parentGround,
			hullMins, hullMaxs, parentBuilding, vehiclePhysbox);

		const CollisionMapperGround worldGround = FindCollisionMapperGround(
			xy, referenceLevels[level], hullMins, hullMaxs, nullptr);
		AddCollisionMapper3DTopologyFloor(&sample.floors, worldGround,
			hullMins, hullMaxs, parentBuilding, vehiclePhysbox);
	}

	std::sort(sample.floors.begin(), sample.floors.end(),
		[](const CollisionMapper3DTopologyFloor &left,
			const CollisionMapper3DTopologyFloor &right)
		{
			return left.feet.z < right.feet.z;
		});

	Msg("[RCBOT2] NavMesh collision-3d sample xy=(%.1f %.1f) floors=%d\n",
		xy.x, xy.y, static_cast<int>(sample.floors.size()));
	for (std::size_t i = 0; i < sample.floors.size(); ++i)
	{
		const CollisionMapper3DTopologyFloor &floor = sample.floors[i];
		trace_t physboxTrace;
		bool physboxSolid = false;
		if (ClipCollisionMapperHullToEntity(floor.feet, floor.feet, hullMins,
			hullMaxs, vehiclePhysbox, &physboxTrace))
		{
			physboxSolid = physboxTrace.startsolid || physboxTrace.allsolid ||
				physboxTrace.fraction < 1.0f;
		}
		Msg("[RCBOT2] NavMesh collision-3d floor xy=(%.1f %.1f) feet=(%.1f %.1f %.1f) "
			"source=%s surface=%s normal=(%.3f %.3f %.3f) free=%d headroom=%d "
			"entity94_blocked=%d entity94_fraction=%.3f entity94_startsolid=%d "
			"entity94_allsolid=%d world_blocked=%d world_fraction=%.3f "
			"world_startsolid=%d world_allsolid=%d physbox_solid=%d\n",
			xy.x, xy.y, floor.feet.x, floor.feet.y, floor.feet.z,
			floor.source.c_str(), floor.surface.c_str(), floor.normal.x,
			floor.normal.y, floor.normal.z, floor.free ? 1 : 0,
			floor.headroom ? 1 : 0, floor.buildingBlocked ? 1 : 0,
			floor.buildingFraction, floor.buildingStartSolid ? 1 : 0,
			floor.buildingAllSolid ? 1 : 0, floor.worldBlocked ? 1 : 0,
			floor.worldFraction, floor.worldStartSolid ? 1 : 0,
			floor.worldAllSolid ? 1 : 0, physboxSolid ? 1 : 0);
	}
	samples->push_back(sample);
}

bool CollisionMapper3DTopologyHasLayer(const CollisionMapper3DTopologySample &sample,
	const bool lower, const bool upper)
{
	for (const CollisionMapper3DTopologyFloor &floor : sample.floors)
	{
		if (!floor.free)
			continue;
		if (lower && floor.feet.z <= -240.0f)
			return true;
		if (upper && floor.feet.z >= -228.0f)
			return true;
	}
	return false;
}

bool CollisionMapper3DTopologyNeedsRefinement(
	const CollisionMapper3DTopologySample &sample,
	const CollisionMapper3DTopologySample &other)
{
	const bool sampleHasLower = CollisionMapper3DTopologyHasLayer(sample, true,
		false);
	const bool otherHasLower = CollisionMapper3DTopologyHasLayer(other, true,
		false);
	const bool sampleHasUpper = CollisionMapper3DTopologyHasLayer(sample, false,
		true);
	const bool otherHasUpper = CollisionMapper3DTopologyHasLayer(other, false,
		true);
	return sampleHasLower != otherHasLower || sampleHasUpper != otherHasUpper ||
		sample.floors.size() != other.floors.size();
}

const CollisionMapper3DTopologyFloor *FindCollisionMapper3DTopologyFloor(
	const CollisionMapper3DTopologySample &sample, const float z,
	const float tolerance)
{
	const CollisionMapper3DTopologyFloor *best = nullptr;
	float bestDistance = std::numeric_limits<float>::max();
	for (const CollisionMapper3DTopologyFloor &floor : sample.floors)
	{
		if (!floor.free)
			continue;
		const float distance = std::fabs(floor.feet.z - z);
		if (distance <= tolerance && distance < bestDistance)
		{
			best = &floor;
			bestDistance = distance;
		}
	}
	return best;
}

int AddCollisionMapper3DTopologyNode(
	const CollisionMapper3DTopologySample &sample,
	const CollisionMapper3DTopologyFloor &floor,
	std::vector<CollisionMapper3DTopologyNode> *nodes)
{
	if (nodes == nullptr)
		return -1;
	CollisionMapper3DTopologyNode node;
	node.sample = -1;
	node.feet = floor.feet;
	node.lower = floor.feet.z <= -240.0f;
	node.upper = floor.feet.z >= -228.0f;
	nodes->push_back(node);
	(void)sample;
	return static_cast<int>(nodes->size()) - 1;
}

CollisionMapperEdgeEvaluation EvaluateCollisionMapper3DTopologyEdge(
	const CollisionMapper3DTopologyNode &from,
	const CollisionMapper3DTopologyNode &to,
	const Vector &hullMins, const Vector &hullMaxs, edict_t *parentBuilding)
{
	CollisionMapperCell fromCell;
	fromCell.feet = from.feet;
	fromCell.sample = from.feet;
	fromCell.state = CollisionMapperCellState::FREE;
	CollisionMapperCell toCell;
	toCell.feet = to.feet;
	toCell.sample = to.feet;
	toCell.state = CollisionMapperCellState::FREE;
	return EvaluateCollisionMapperEdge(fromCell, toCell, hullMins, hullMaxs,
		parentBuilding);
}

bool CollisionMapper3DTopologyEdgeAllowed(
	const CollisionMapperEdgeEvaluation &evaluation)
{
	if (!evaluation.traversable || evaluation.state ==
		CollisionMapperEdgeState::BLOCKED_BY_BUILDING || evaluation.state ==
		CollisionMapperEdgeState::BLOCKED_BY_WORLD || evaluation.state ==
		CollisionMapperEdgeState::NO_GROUND)
		return false;
	// A direct 48-unit layer jump is not a valid topology edge. A stair or
	// ramp may still be accepted when each sampled local rise stays within the
	// existing player-step envelope.
	return evaluation.maxLocalGroundDelta <= 18.0f;
}

void BuildCollisionMapper3DTopologyEdges(
	const std::vector<CollisionMapper3DTopologyNode> &nodes,
	const Vector &hullMins, const Vector &hullMaxs, edict_t *parentBuilding,
	std::vector<std::vector<int>> *adjacency,
	std::vector<CollisionMapper3DTopologyEdge> *edges)
{
	if (adjacency == nullptr || edges == nullptr)
		return;
	adjacency->assign(nodes.size(), std::vector<int>());
	for (int from = 0; from < static_cast<int>(nodes.size()); ++from)
	{
		for (int to = from + 1; to < static_cast<int>(nodes.size()); ++to)
		{
			const float dx = nodes[to].feet.x - nodes[from].feet.x;
			const float dy = nodes[to].feet.y - nodes[from].feet.y;
			const float horizontalDistance = std::sqrt(dx * dx + dy * dy);
			if (horizontalDistance < 0.1f || horizontalDistance > 11.5f ||
				std::fabs(dx) > 8.1f || std::fabs(dy) > 8.1f)
				continue;

			const CollisionMapperEdgeEvaluation forward =
				EvaluateCollisionMapper3DTopologyEdge(nodes[from], nodes[to],
					hullMins, hullMaxs, parentBuilding);
			const bool forwardAllowed =
				CollisionMapper3DTopologyEdgeAllowed(forward);
			CollisionMapper3DTopologyEdge forwardEdge;
			forwardEdge.from = from;
			forwardEdge.to = to;
			forwardEdge.evaluation = forward;
			edges->push_back(forwardEdge);
			if (forwardAllowed)
				(*adjacency)[from].push_back(to);

			const CollisionMapperEdgeEvaluation reverse =
				EvaluateCollisionMapper3DTopologyEdge(nodes[to], nodes[from],
					hullMins, hullMaxs, parentBuilding);
			const bool reverseAllowed =
				CollisionMapper3DTopologyEdgeAllowed(reverse);
			CollisionMapper3DTopologyEdge reverseEdge;
			reverseEdge.from = to;
			reverseEdge.to = from;
			reverseEdge.evaluation = reverse;
			edges->push_back(reverseEdge);
			if (reverseAllowed)
				(*adjacency)[to].push_back(from);
		}
	}
}

std::vector<int> BuildCollisionMapper3DTopologyBFS(
	const std::vector<CollisionMapper3DTopologyNode> &nodes,
	const std::vector<std::vector<int>> &adjacency, const int start,
	const int target)
{
	std::vector<int> predecessor(nodes.size(), -1);
	std::queue<int> pending;
	if (start < 0 || start >= static_cast<int>(nodes.size()))
		return std::vector<int>();
	predecessor[start] = start;
	pending.push(start);
	while (!pending.empty())
	{
		const int current = pending.front();
		pending.pop();
		if (current == target)
			break;
		for (const int next : adjacency[current])
		{
			if (next < 0 || next >= static_cast<int>(nodes.size()) ||
				predecessor[next] >= 0)
				continue;
			predecessor[next] = current;
			pending.push(next);
		}
	}
	if (target < 0 || target >= static_cast<int>(nodes.size()) ||
		predecessor[target] < 0)
		return std::vector<int>();
	std::vector<int> path;
	for (int current = target;; current = predecessor[current])
	{
		path.push_back(current);
		if (current == start)
			break;
	}
	std::reverse(path.begin(), path.end());
	return path;
}

std::vector<int> BuildCollisionMapper3DTopologyBFSToUpper(
	const std::vector<CollisionMapper3DTopologyNode> &nodes,
	const std::vector<std::vector<int>> &adjacency, const int start)
{
	std::vector<int> predecessor(nodes.size(), -1);
	std::queue<int> pending;
	if (start < 0 || start >= static_cast<int>(nodes.size()))
		return std::vector<int>();
	predecessor[start] = start;
	pending.push(start);
	int target = -1;
	while (!pending.empty())
	{
		const int current = pending.front();
		pending.pop();
		if (nodes[current].upper)
		{
			target = current;
			break;
		}
		for (const int next : adjacency[current])
		{
			if (next < 0 || next >= static_cast<int>(nodes.size()) ||
				predecessor[next] >= 0)
				continue;
			predecessor[next] = current;
			pending.push(next);
		}
	}
	if (target < 0)
		return std::vector<int>();
	std::vector<int> path;
	for (int current = target;; current = predecessor[current])
	{
		path.push_back(current);
		if (current == start)
			break;
	}
	std::reverse(path.begin(), path.end());
	return path;
}

void PrintCollisionMapper3DTopologyPath(
	const std::vector<int> &path,
	const std::vector<CollisionMapper3DTopologyNode> &nodes,
	const char *label)
{
	float length = 0.0f;
	int turns = 0;
	int previousDX = 0;
	int previousDY = 0;
	for (std::size_t i = 1; i < path.size(); ++i)
	{
		const Vector &from = nodes[path[i - 1]].feet;
		const Vector &to = nodes[path[i]].feet;
		length += (to - from).Length2D();
		const int dx = static_cast<int>(std::round(to.x - from.x));
		const int dy = static_cast<int>(std::round(to.y - from.y));
		if (i > 1 && ((dx > 0) != (previousDX > 0) ||
			(dy > 0) != (previousDY > 0)))
			++turns;
		previousDX = dx;
		previousDY = dy;
	}
	Msg("[RCBOT2] NavMesh collision-3d path label=%s nodes=%d length=%.1f "
		"turns=%d\n", label, static_cast<int>(path.size()), length,
		turns);
	for (std::size_t i = 0; i < path.size(); ++i)
	{
		const Vector &feet = nodes[path[i]].feet;
		Msg("[RCBOT2] NavMesh collision-3d path_point label=%s step=%d "
			"node=%d lower=%d upper=%d feet=(%.1f %.1f %.1f)\n", label,
			static_cast<int>(i), path[i], nodes[path[i]].lower ? 1 : 0,
			nodes[path[i]].upper ? 1 : 0, feet.x, feet.y, feet.z);
		if (debugoverlay != nullptr && i > 0)
			debugoverlay->AddLineOverlay(nodes[path[i - 1]].feet, feet, 255, 255,
				255, true, 30.0f);
	}
}

const CollisionMapper3DTopologyEdge *FindCollisionMapper3DTopologyEdge(
	const std::vector<CollisionMapper3DTopologyEdge> &edges, const int from,
	const int to)
{
	for (const CollisionMapper3DTopologyEdge &edge : edges)
	{
		if (edge.from == from && edge.to == to)
			return &edge;
	}
	return nullptr;
}

void PrintCollisionMapper3DTopologyEdgeEvidence(
	const CollisionMapper3DTopologyNode &from,
	const CollisionMapper3DTopologyNode &to,
	const Vector &hullMins, const Vector &hullMaxs, edict_t *parentBuilding)
{
	trace_t parentTrace;
	trace_t worldTrace;
	const bool parentProbe = ClipCollisionMapperHullToEntity(from.feet,
		to.feet, hullMins, hullMaxs, parentBuilding, &parentTrace);
	const bool worldProbe = TraceCollisionMapperWorldWallHull(from.feet,
		to.feet, hullMins, hullMaxs, &worldTrace);
	Msg("[RCBOT2] NavMesh collision-3d edge_evidence from=(%.1f %.1f %.1f) "
		"to=(%.1f %.1f %.1f) parent_fraction=%.6f parent_startsolid=%d "
		"parent_allsolid=%d parent_normal=(%.3f %.3f %.3f) "
		"parent_end=(%.1f %.1f %.1f) world_fraction=%.6f world_startsolid=%d "
		"world_allsolid=%d world_end=(%.1f %.1f %.1f)\n", from.feet.x,
		from.feet.y, from.feet.z, to.feet.x, to.feet.y, to.feet.z,
		parentProbe ? parentTrace.fraction : 0.0f,
		parentProbe && parentTrace.startsolid ? 1 : 0,
		parentProbe && parentTrace.allsolid ? 1 : 0,
		parentProbe ? parentTrace.plane.normal.x : 0.0f,
		parentProbe ? parentTrace.plane.normal.y : 0.0f,
		parentProbe ? parentTrace.plane.normal.z : 0.0f,
		parentProbe ? parentTrace.endpos.x : 0.0f,
		parentProbe ? parentTrace.endpos.y : 0.0f,
		parentProbe ? parentTrace.endpos.z : 0.0f,
		worldProbe ? worldTrace.fraction : 0.0f,
		worldProbe && worldTrace.startsolid ? 1 : 0,
		worldProbe && worldTrace.allsolid ? 1 : 0,
		worldProbe ? worldTrace.endpos.x : 0.0f,
		worldProbe ? worldTrace.endpos.y : 0.0f,
		worldProbe ? worldTrace.endpos.z : 0.0f);

	const float sampleFractions[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
	for (const float fraction : sampleFractions)
	{
		const Vector xy(from.feet.x + (to.feet.x - from.feet.x) * fraction,
			from.feet.y + (to.feet.y - from.feet.y) * fraction, 0.0f);
		const float referenceZ = from.feet.z +
			(to.feet.z - from.feet.z) * fraction;
		const CollisionMapperGround ground = FindCollisionMapperGround(xy,
			referenceZ, hullMins, hullMaxs, parentBuilding);
		if (!ground.valid)
		{
			Msg("[RCBOT2] NavMesh collision-3d edge_profile t=%.2f xy=(%.1f %.1f) "
				"reference_z=%.1f ground=NO\n", fraction, xy.x, xy.y,
				referenceZ);
			continue;
		}
		trace_t profileParent;
		trace_t profileWorld;
		const bool profileParentProbe = ClipCollisionMapperHullToEntity(
			ground.feet, ground.feet, hullMins, hullMaxs, parentBuilding,
			&profileParent);
		const bool profileWorldProbe = TraceCollisionMapperWorldWallHull(
			ground.feet, ground.feet, hullMins, hullMaxs, &profileWorld);
		Msg("[RCBOT2] NavMesh collision-3d edge_profile t=%.2f xy=(%.1f %.1f) "
			"reference_z=%.1f ground=(%.1f %.1f %.1f) source=%s normal_z=%.3f "
			"parent_fraction=%.6f parent_startsolid=%d world_fraction=%.6f "
			"world_startsolid=%d world_allsolid=%d\n", fraction, xy.x, xy.y,
			referenceZ, ground.feet.x, ground.feet.y, ground.feet.z,
			ground.source, ground.normal.z,
			profileParentProbe ? profileParent.fraction : 0.0f,
			profileParentProbe && profileParent.startsolid ? 1 : 0,
			profileWorldProbe ? profileWorld.fraction : 0.0f,
			profileWorldProbe && profileWorld.startsolid ? 1 : 0,
			profileWorldProbe && profileWorld.allsolid ? 1 : 0);
	}
}

void RunCollisionMapper3DTopology(const Vector &lowerPoint,
	const Vector &upperPoint, const int requestedPlayerIndex)
{
	if (engine == nullptr || gpGlobals == nullptr || enginetrace == nullptr ||
		playerinfomanager == nullptr)
	{
		Msg("[RCBOT2] NavMesh collision-3d result=D reason=trace-player-interfaces\n");
		return;
	}

	CollisionMapperHull hull;
	if (!ResolveCollisionMapperHull(lowerPoint, requestedPlayerIndex, &hull))
	{
		Msg("[RCBOT2] NavMesh collision-3d result=D reason=player-hull-unavailable\n");
		return;
	}
	edict_t *barracks = FindCollisionProbeEntity(94,
		"emp_building_nf_barracks");
	edict_t *vehiclePhysbox = FindCollisionProbeEntity(506,
		"emp_building_vehicle_physbox");
	if (barracks == nullptr)
	{
		Msg("[RCBOT2] NavMesh collision-3d result=D reason=entity94-unavailable\n");
		return;
	}

	const float referenceLevels[] = {
		-278.0f, -270.0f, -260.0f, -252.0f, -244.0f, -236.0f,
		-228.0f, -220.0f, -212.0f, -208.0f, -196.0f
	};
	const int referenceLevelCount = sizeof(referenceLevels) /
		sizeof(referenceLevels[0]);
	const float baseResolution = 8.0f;
	const float padding = 40.0f;
	const float minX = std::min(lowerPoint.x, upperPoint.x) - padding;
	const float maxX = std::max(lowerPoint.x, upperPoint.x) + padding;
	const float minY = std::min(lowerPoint.y, upperPoint.y) - padding;
	const float maxY = std::max(lowerPoint.y, upperPoint.y) + padding;
	const float alignedMinX = std::floor(minX / baseResolution) *
		baseResolution;
	const float alignedMinY = std::floor(minY / baseResolution) *
		baseResolution;
	Msg("[RCBOT2] NavMesh collision-3d begin lower=(%.1f %.1f %.1f) "
		"upper=(%.1f %.1f %.1f) bounds=(%.1f %.1f)-(%.1f %.1f) "
		"base_resolution=%.1f hull_mins=(%.1f %.1f %.1f) "
		"hull_maxs=(%.1f %.1f %.1f) player_index=%d actual_feet=(%.1f %.1f %.1f)\n",
		lowerPoint.x, lowerPoint.y, lowerPoint.z, upperPoint.x, upperPoint.y,
		upperPoint.z, alignedMinX, alignedMinY, maxX, maxY, baseResolution,
		hull.mins.x, hull.mins.y, hull.mins.z, hull.maxs.x, hull.maxs.y,
		hull.maxs.z, hull.playerIndex, hull.feet.x, hull.feet.y, hull.feet.z);
	Msg("[RCBOT2] NavMesh collision-3d entities entity94=%d classname=%s "
		"physbox506=%d classname=%s\n", engine->IndexOfEdict(barracks),
		SafeClassName(barracks), vehiclePhysbox != nullptr ?
			engine->IndexOfEdict(vehiclePhysbox) : -1,
		SafeClassName(vehiclePhysbox));

	std::vector<CollisionMapper3DTopologySample> samples;
	for (float y = alignedMinY; y <= maxY + 0.1f; y += baseResolution)
	{
		for (float x = alignedMinX; x <= maxX + 0.1f; x += baseResolution)
		{
			SampleCollisionMapper3DTopologyPoint(Vector(x, y, 0.0f),
				referenceLevels, referenceLevelCount, hull.mins, hull.maxs,
				barracks, vehiclePhysbox, &samples);
		}
	}

	const std::size_t baseSampleCount = samples.size();
	std::vector<Vector> refinementCenters;
	refinementCenters.push_back(Vector(lowerPoint.x, lowerPoint.y, 0.0f));
	refinementCenters.push_back(Vector(upperPoint.x, upperPoint.y, 0.0f));
	for (std::size_t i = 0; i < baseSampleCount; ++i)
	{
		for (std::size_t j = 0; j < baseSampleCount; ++j)
		{
			const float dx = std::fabs(samples[i].xy.x - samples[j].xy.x);
			const float dy = std::fabs(samples[i].xy.y - samples[j].xy.y);
			if ((dx == baseResolution && dy <= 0.1f) ||
				(dy == baseResolution && dx <= 0.1f))
			{
				if (CollisionMapper3DTopologyNeedsRefinement(samples[i],
					samples[j]))
					refinementCenters.push_back(samples[i].xy);
				break;
			}
		}
	}
	std::vector<Vector> uniqueCenters;
	for (const Vector &center : refinementCenters)
	{
		bool duplicate = false;
		for (const Vector &existing : uniqueCenters)
		{
			if (CollisionMapper3DTopologySameXY(center, existing))
			{
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			uniqueCenters.push_back(center);
	}
	for (const Vector &center : uniqueCenters)
	{
		for (int offsetY = -1; offsetY <= 1; ++offsetY)
		{
			for (int offsetX = -1; offsetX <= 1; ++offsetX)
			{
				const Vector xy(center.x + offsetX * 4.0f,
					center.y + offsetY * 4.0f, 0.0f);
				if (xy.x < alignedMinX - 0.1f || xy.x > maxX + 0.1f ||
					xy.y < alignedMinY - 0.1f || xy.y > maxY + 0.1f)
					continue;
				SampleCollisionMapper3DTopologyPoint(xy, referenceLevels,
					referenceLevelCount, hull.mins, hull.maxs, barracks,
					vehiclePhysbox, &samples);
			}
		}
	}
	Msg("[RCBOT2] NavMesh collision-3d sampling base_samples=%d "
		"refinement_centers=%d total_samples=%d\n",
		static_cast<int>(baseSampleCount), static_cast<int>(uniqueCenters.size()),
		static_cast<int>(samples.size()));

	std::vector<CollisionMapper3DTopologyNode> nodes;
	for (std::size_t sampleIndex = 0; sampleIndex < samples.size();
		++sampleIndex)
	{
		for (const CollisionMapper3DTopologyFloor &floor :
			samples[sampleIndex].floors)
		{
			if (!floor.free)
				continue;
			const int nodeIndex = AddCollisionMapper3DTopologyNode(
				samples[sampleIndex], floor, &nodes);
			nodes[nodeIndex].sample = static_cast<int>(sampleIndex);
		}
	}
	std::vector<std::vector<int>> adjacency;
	std::vector<CollisionMapper3DTopologyEdge> edges;
	BuildCollisionMapper3DTopologyEdges(nodes, hull.mins, hull.maxs, barracks,
		&adjacency, &edges);

	int freeNodes = 0;
	int lowerNodes = 0;
	int upperNodes = 0;
	std::vector<float> layers;
	for (const CollisionMapper3DTopologyNode &node : nodes)
	{
		++freeNodes;
		if (node.lower)
			++lowerNodes;
		if (node.upper)
			++upperNodes;
		bool knownLayer = false;
		for (const float layer : layers)
		{
			if (std::fabs(layer - node.feet.z) <= 2.0f)
			{
				knownLayer = true;
				break;
			}
		}
		if (!knownLayer)
			layers.push_back(node.feet.z);
	}
	std::sort(layers.begin(), layers.end());
	Msg("[RCBOT2] NavMesh collision-3d summary samples=%d free_nodes=%d "
		"lower_nodes=%d upper_nodes=%d directed_edges=%d layers=%d\n",
		static_cast<int>(samples.size()), freeNodes, lowerNodes, upperNodes,
		static_cast<int>(edges.size()), static_cast<int>(layers.size()));
	for (const float layer : layers)
		Msg("[RCBOT2] NavMesh collision-3d floor_layer z=%.1f\n", layer);

	int startNode = -1;
	int targetNode = -1;
	float startDistance = std::numeric_limits<float>::max();
	float targetDistance = std::numeric_limits<float>::max();
	for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
	{
		const float startDX = nodes[i].feet.x - lowerPoint.x;
		const float startDY = nodes[i].feet.y - lowerPoint.y;
		const float targetDX = nodes[i].feet.x - upperPoint.x;
		const float targetDY = nodes[i].feet.y - upperPoint.y;
		const float startXYDistance = std::sqrt(startDX * startDX +
			startDY * startDY);
		const float targetXYDistance = std::sqrt(targetDX * targetDX +
			targetDY * targetDY);
		if (nodes[i].lower && startXYDistance < startDistance)
		{
			startNode = i;
			startDistance = startXYDistance;
		}
		if (nodes[i].upper && targetXYDistance < targetDistance)
		{
			targetNode = i;
			targetDistance = targetXYDistance;
		}
	}
	Msg("[RCBOT2] NavMesh collision-3d anchors start_node=%d "
		"start_feet=(%.1f %.1f %.1f) start_xy_distance=%.1f target_node=%d "
		"target_feet=(%.1f %.1f %.1f) target_xy_distance=%.1f\n", startNode,
		startNode >= 0 ? nodes[startNode].feet.x : 0.0f,
		startNode >= 0 ? nodes[startNode].feet.y : 0.0f,
		startNode >= 0 ? nodes[startNode].feet.z : 0.0f, startDistance,
		targetNode, targetNode >= 0 ? nodes[targetNode].feet.x : 0.0f,
		targetNode >= 0 ? nodes[targetNode].feet.y : 0.0f,
		targetNode >= 0 ? nodes[targetNode].feet.z : 0.0f, targetDistance);
	if (startNode >= 0 && targetNode >= 0)
	{
		Msg("[RCBOT2] NavMesh collision-3d anchor_edge_evidence\n");
		PrintCollisionMapper3DTopologyEdgeEvidence(nodes[startNode],
			nodes[targetNode], hull.mins, hull.maxs, barracks);
	}

	std::vector<int> pathToTarget;
	if (startNode >= 0 && targetNode >= 0)
		pathToTarget = BuildCollisionMapper3DTopologyBFS(nodes, adjacency,
			startNode, targetNode);
	std::vector<int> nearbyPath;
	if (startNode >= 0)
		nearbyPath = BuildCollisionMapper3DTopologyBFSToUpper(nodes,
			adjacency, startNode);
	std::vector<int> otherLayerPath;
	int otherLayerStart = -1;
	for (int candidateStart = 0;
		candidateStart < static_cast<int>(nodes.size()); ++candidateStart)
	{
		if (candidateStart == startNode || !nodes[candidateStart].lower)
			continue;
		std::vector<int> candidatePath =
			BuildCollisionMapper3DTopologyBFSToUpper(nodes, adjacency,
				candidateStart);
		if (!candidatePath.empty())
		{
			otherLayerPath = candidatePath;
			otherLayerStart = candidateStart;
			break;
		}
	}
	if (!pathToTarget.empty())
	{
		Msg("[RCBOT2] NavMesh collision-3d transition result=A "
			"lower_to_upper=YES\n");
		PrintCollisionMapper3DTopologyPath(pathToTarget, nodes, "lower-to-upper");
	}
	else
	{
		if (!nearbyPath.empty())
		{
			Msg("[RCBOT2] NavMesh collision-3d transition result=C "
				"lower_to_upper=NEARBY_ONLY\n");
			PrintCollisionMapper3DTopologyPath(nearbyPath, nodes,
				"nearby-lower-to-upper");
		}
		else if (!otherLayerPath.empty())
		{
			Msg("[RCBOT2] NavMesh collision-3d transition result=C "
				"lower_to_upper=OTHER_LOWER_COMPONENT source_node=%d\n",
				otherLayerStart);
			PrintCollisionMapper3DTopologyPath(otherLayerPath, nodes,
				"other-lower-to-upper");
		}
		else
		{
			Msg("[RCBOT2] NavMesh collision-3d transition result=B "
			"lower_to_upper=NO\n");
		}
	}

	if (startNode >= 0)
	{
		std::vector<char> visited(nodes.size(), 0);
		std::queue<int> pending;
		visited[startNode] = 1;
		pending.push(startNode);
		while (!pending.empty())
		{
			const int current = pending.front();
			pending.pop();
			for (const int next : adjacency[current])
			{
				if (next >= 0 && next < static_cast<int>(nodes.size()) &&
					!visited[next])
				{
					visited[next] = 1;
					pending.push(next);
				}
			}
		}
		int reachable = 0;
		int reachableLower = 0;
		int reachableUpper = 0;
		for (int i = 0; i < static_cast<int>(visited.size()); ++i)
		{
			if (!visited[i])
				continue;
			++reachable;
			if (nodes[i].lower)
				++reachableLower;
			if (nodes[i].upper)
				++reachableUpper;
		}
		Msg("[RCBOT2] NavMesh collision-3d reachable_component nodes=%d "
			"lower=%d upper=%d\n", reachable, reachableLower, reachableUpper);

		float nearestGap = std::numeric_limits<float>::max();
		int gapFrom = -1;
		int gapTo = -1;
		for (int pass = 0; pass < 2 && gapFrom < 0; ++pass)
		{
			nearestGap = std::numeric_limits<float>::max();
			for (int from = 0; from < static_cast<int>(nodes.size()); ++from)
			{
				if (!visited[from])
					continue;
				for (int to = 0; to < static_cast<int>(nodes.size()); ++to)
				{
					if (visited[to] || (pass == 0 && !nodes[to].upper))
						continue;
					const float dx = nodes[to].feet.x - nodes[from].feet.x;
					const float dy = nodes[to].feet.y - nodes[from].feet.y;
					const float distance = std::sqrt(dx * dx + dy * dy);
					if (distance > 11.5f || distance >= nearestGap)
						continue;
					nearestGap = distance;
					gapFrom = from;
					gapTo = to;
				}
			}
		}
		if (gapFrom >= 0 && gapTo >= 0)
		{
			const CollisionMapper3DTopologyEdge *gapEdge =
				FindCollisionMapper3DTopologyEdge(edges, gapFrom, gapTo);
			Msg("[RCBOT2] NavMesh collision-3d nearest_frontier from=%d "
				"feet=(%.1f %.1f %.1f) to=%d feet=(%.1f %.1f %.1f) "
				"xy_distance=%.1f\n", gapFrom, nodes[gapFrom].feet.x,
				nodes[gapFrom].feet.y, nodes[gapFrom].feet.z, gapTo,
				nodes[gapTo].feet.x, nodes[gapTo].feet.y, nodes[gapTo].feet.z,
				nearestGap);
			if (gapEdge != nullptr)
			{
				Msg("[RCBOT2] NavMesh collision-3d frontier_evaluation state=%s "
					"traversable=%d parent_fraction=%.3f world_fraction=%.3f "
					"from_z=%.1f to_z=%.1f max_local_dz=%.1f\n",
					CollisionMapperEdgeStateName(gapEdge->evaluation.state),
					gapEdge->evaluation.traversable ? 1 : 0,
					gapEdge->evaluation.horizontalParentFraction,
					gapEdge->evaluation.horizontalWorldFraction,
					gapEdge->evaluation.fromGroundZ,
					gapEdge->evaluation.toGroundZ,
					gapEdge->evaluation.maxLocalGroundDelta);
				PrintCollisionMapper3DTopologyEdgeEvidence(nodes[gapFrom],
					nodes[gapTo], hull.mins, hull.maxs, barracks);
			}
		}
	}

	Msg("[RCBOT2] NavMesh collision-3d end\n");
}

void RunCollisionMapperAdaptiveBoundary(
	const CollisionMapperEdge &frontierEdge,
	const std::vector<CollisionMapperCell> &cells,
	const std::vector<CollisionMapperEdge> &coarseEdges,
	const std::vector<int> &predecessor,
	const std::vector<CollisionMapperPortalSample> &portalSamples,
	const int width, const int height, const Vector &hullMins,
	const Vector &hullMaxs, edict_t *parentBuilding, const Vector &mapperSeed,
	const Vector &portal)
{
	if (parentBuilding == nullptr || cells.empty())
		return;

	const Vector frontierMid = (cells[frontierEdge.from].feet +
		cells[frontierEdge.to].feet) * 0.5f;
	const int frontierX = (cells[frontierEdge.from].x +
		cells[frontierEdge.to].x) / 2;
	const int frontierY = (cells[frontierEdge.from].y +
		cells[frontierEdge.to].y) / 2;
	constexpr int kCoarseRadius = 12;
	std::vector<int> refinementCells;
	std::vector<char> refinementMask(cells.size(), 0);
	for (int y = std::max(0, frontierY - kCoarseRadius);
		y <= std::min(height - 1, frontierY + kCoarseRadius); ++y)
	{
		for (int x = std::max(0, frontierX - kCoarseRadius);
			x <= std::min(width - 1, frontierX + kCoarseRadius); ++x)
		{
			const int index = y * width + x;
			bool boundary = index == frontierEdge.from ||
				index == frontierEdge.to;
			for (int dy = -1; dy <= 1 && !boundary; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					if (dx == 0 && dy == 0)
						continue;
					const int nx = x + dx;
					const int ny = y + dy;
					if (nx < 0 || nx >= width || ny < 0 || ny >= height)
						continue;
					const CollisionMapperCellState a = cells[index].state;
					const CollisionMapperCellState b = cells[ny * width + nx].state;
					if ((a == CollisionMapperCellState::FREE &&
						b == CollisionMapperCellState::BLOCKED_BY_BUILDING) ||
						(a == CollisionMapperCellState::BLOCKED_BY_BUILDING &&
						b == CollisionMapperCellState::FREE))
					{
						boundary = true;
						break;
					}
				}
			}
			for (const CollisionMapperEdge &edge : coarseEdges)
			{
				if (edge.state != CollisionMapperEdgeState::BLOCKED_BY_BUILDING)
					continue;
				if (edge.from == index || edge.to == index)
				{
					boundary = true;
					break;
				}
			}
			if (boundary)
			{
				refinementMask[index] = 1;
				refinementCells.push_back(index);
			}
		}
	}

	std::vector<float> referenceLevels;
	const auto addReferenceLevel = [&referenceLevels](const float z)
	{
		if (!std::isfinite(z))
			return;
		for (const float existing : referenceLevels)
		{
			if (std::fabs(existing - z) < 2.0f)
				return;
		}
		referenceLevels.push_back(z);
	};
	addReferenceLevel(mapperSeed.z);
	addReferenceLevel(portal.z);
	for (const int index : refinementCells)
		if (cells[index].state == CollisionMapperCellState::FREE)
			addReferenceLevel(cells[index].feet.z);
	std::sort(referenceLevels.begin(), referenceLevels.end());

	Msg("[RCBOT2] NavMesh collision-mapper adaptive_begin "
		"frontier_mid=(%.1f %.1f %.1f) refinement_cells=%d "
		"reference_levels=%d\n", frontierMid.x, frontierMid.y, frontierMid.z,
		static_cast<int>(refinementCells.size()),
		static_cast<int>(referenceLevels.size()));

	for (const float resolution : { 8.0f, 4.0f })
	{
		const int subdivisions = static_cast<int>(16.0f / resolution);
		std::vector<CollisionMapperAdaptiveSample> samples;
		for (const int coarseIndex : refinementCells)
		{
			const CollisionMapperCell &coarse = cells[coarseIndex];
			for (int sampleY = 0; sampleY < subdivisions; ++sampleY)
			{
				for (int sampleX = 0; sampleX < subdivisions; ++sampleX)
				{
					const float offsetX = -8.0f + resolution * 0.5f +
						sampleX * resolution;
					const float offsetY = -8.0f + resolution * 0.5f +
						sampleY * resolution;
					const Vector xy(coarse.sample.x + offsetX,
						coarse.sample.y + offsetY, 0.0f);
					int existingSample = -1;
					for (std::size_t index = 0; index < samples.size(); ++index)
					{
						if ((samples[index].xy - xy).Length2D() < 0.1f)
						{
							existingSample = static_cast<int>(index);
							break;
						}
					}
					if (existingSample >= 0)
						continue;

					CollisionMapperAdaptiveSample sample;
					sample.xy = xy;
					sample.coarseCell = coarseIndex;
					for (const float referenceZ : referenceLevels)
					{
						const CollisionMapperGround grounds[] = {
							FindCollisionMapperGround(xy, referenceZ, hullMins,
								hullMaxs, parentBuilding),
							FindCollisionMapperGround(xy, referenceZ, hullMins,
								hullMaxs, nullptr)
						};
						for (const CollisionMapperGround &ground : grounds)
						{
							if (!ground.valid || ground.normal.z < 0.5f)
								continue;
							sample.hasGround = true;
							bool duplicateLevel = false;
							for (const float existingZ : sample.groundZs)
							{
								if (std::fabs(existingZ - ground.feet.z) < 2.0f)
								{
									duplicateLevel = true;
									break;
								}
							}
							if (duplicateLevel)
								continue;
							sample.groundZs.push_back(ground.feet.z);

							trace_t parentTrace;
							const bool parentProbe = ClipCollisionMapperHullToEntity(
								ground.feet, ground.feet, hullMins, hullMaxs,
								parentBuilding, &parentTrace);
							const bool parentBlocked = parentProbe &&
								(parentTrace.startsolid || parentTrace.allsolid ||
									parentTrace.fraction < 1.0f);
							if (parentBlocked)
							{
								sample.blockedByBuilding = true;
								sample.blockedGroundZs.push_back(ground.feet.z);
								continue;
							}
							sample.freeGroundZs.push_back(ground.feet.z);
						}
					}
					samples.push_back(sample);
				}
			}
		}

		// Build a coarse component on the far side of the selected frontier while
		// deliberately ignoring only building edges. World walls and no-ground
		// edges still terminate this diagnostic component.
		const int portalSeed = predecessor[frontierEdge.from] == -1 ?
			frontierEdge.from : frontierEdge.to;
		std::vector<char> portalComponent(cells.size(), 0);
		std::queue<int> portalPending;
		if (portalSeed >= 0 && portalSeed < static_cast<int>(cells.size()) &&
			cells[portalSeed].state == CollisionMapperCellState::FREE)
		{
			portalComponent[portalSeed] = 1;
			portalPending.push(portalSeed);
		}
		while (!portalPending.empty())
		{
			const int current = portalPending.front();
			portalPending.pop();
			for (const CollisionMapperEdge &edge : coarseEdges)
			{
				if (edge.state == CollisionMapperEdgeState::BLOCKED_BY_WORLD ||
					edge.state == CollisionMapperEdgeState::NO_GROUND)
					continue;
				int neighbor = -1;
				if (edge.from == current)
					neighbor = edge.to;
				else if (edge.to == current)
					neighbor = edge.from;
				if (neighbor < 0 || portalComponent[neighbor] ||
					cells[neighbor].state != CollisionMapperCellState::FREE)
					continue;
				portalComponent[neighbor] = 1;
				portalPending.push(neighbor);
			}
		}

		// Rebuild nodes from each sample's parent-free ground levels. The first
		// pass records all observed levels plus separate free/blocked evidence.
		std::vector<CollisionMapperAdaptiveNode> nodes;
		for (CollisionMapperAdaptiveSample &sample : samples)
		{
			sample.nodes.clear();
			for (const float groundZ : sample.freeGroundZs)
			{
				const Vector feet(sample.xy.x, sample.xy.y, groundZ);
				trace_t parentTrace;
				const bool parentProbe = ClipCollisionMapperHullToEntity(
					feet, feet, hullMins, hullMaxs, parentBuilding, &parentTrace);
				if (!parentProbe || parentTrace.startsolid || parentTrace.allsolid ||
					parentTrace.fraction < 1.0f)
					continue;
				CollisionMapperAdaptiveNode node;
				node.feet = feet;
				node.coarseCell = sample.coarseCell;
				node.gridX = static_cast<int>(std::lround(feet.x / resolution));
				node.gridY = static_cast<int>(std::lround(feet.y / resolution));
				node.spawnSide = false;
				node.portalSide = false;
				const int nodeIndex = static_cast<int>(nodes.size());
				nodes.push_back(node);
				sample.nodes.push_back(nodeIndex);
			}
			Msg("[RCBOT2] NavMesh collision-mapper adaptive_sample "
				"resolution=%.1f xy=(%.1f %.1f) ground_levels=%d free_levels=%d "
				"blocked_levels=%d blocked_by_building=%d\n", resolution,
				sample.xy.x, sample.xy.y,
				static_cast<int>(sample.groundZs.size()),
				static_cast<int>(sample.nodes.size()),
				static_cast<int>(sample.blockedGroundZs.size()),
				sample.blockedByBuilding ? 1 : 0);
		}

		// A fine wall boundary is a same-level FREE/BLOCKED adjacency, not the
		// center of a blocked sample. This lets endpoint detection resolve the
		// actual ends of a wall instead of collapsing both ends to one cell.
		std::vector<CollisionMapperAdaptiveBoundary> wallEvidence;
		const auto HasGroundLevel = [](const std::vector<float> &levels,
			const float z)
		{
			for (const float existing : levels)
				if (std::fabs(existing - z) < 2.0f)
					return true;
			return false;
		};
		for (std::size_t first = 0; first < samples.size(); ++first)
		{
			for (std::size_t second = first + 1; second < samples.size(); ++second)
			{
				const float dx = std::fabs(samples[second].xy.x -
					samples[first].xy.x);
				const float dy = std::fabs(samples[second].xy.y -
					samples[first].xy.y);
				if (dx + dy < resolution * 0.9f ||
					dx + dy > resolution * 1.1f ||
					(dx > resolution * 0.1f && dy > resolution * 0.1f))
					continue;
				for (const float firstZ : samples[first].freeGroundZs)
				{
					if (!HasGroundLevel(samples[second].blockedGroundZs, firstZ))
						continue;
					CollisionMapperAdaptiveBoundary boundary;
					boundary.point =
						(samples[first].xy + samples[second].xy) * 0.5f;
					boundary.point.z = firstZ;
					boundary.groundZ = firstZ;
					wallEvidence.push_back(boundary);
				}
				for (const float secondZ : samples[second].freeGroundZs)
				{
					if (!HasGroundLevel(samples[first].blockedGroundZs, secondZ))
						continue;
					CollisionMapperAdaptiveBoundary boundary;
					boundary.point =
						(samples[first].xy + samples[second].xy) * 0.5f;
					boundary.point.z = secondZ;
					boundary.groundZ = secondZ;
					wallEvidence.push_back(boundary);
				}
			}
		}

		// A fine node is spawn-side only when it has an actual traversable hull
		// edge from the corresponding globally reachable coarse cell. Merely
		// sharing a coarse cell is insufficient when that cell contains multiple
		// Z-layers.
		for (CollisionMapperAdaptiveNode &node : nodes)
		{
			if (node.coarseCell < 0 || predecessor[node.coarseCell] == -1)
				continue;
			const CollisionMapperCell &coarseCell = cells[node.coarseCell];
			CollisionMapperCell fineCell;
			fineCell.x = coarseCell.x;
			fineCell.y = coarseCell.y;
			fineCell.sample = node.feet;
			fineCell.feet = node.feet;
			fineCell.groundNormal = Vector(0.0f, 0.0f, 1.0f);
			fineCell.groundSource = "adaptive";
			fineCell.groundSurface = "adaptive";
			fineCell.state = CollisionMapperCellState::FREE;
			fineCell.insideBuildingBounds = true;
			fineCell.buildingFraction = 1.0f;
			const CollisionMapperEdgeEvaluation evaluation =
				EvaluateCollisionMapperEdge(coarseCell, fineCell, hullMins, hullMaxs,
					parentBuilding);
			node.spawnSide = evaluation.traversable;
		}
		for (CollisionMapperAdaptiveNode &node : nodes)
		{
			node.portalSide = !node.spawnSide &&
				portalComponent[node.coarseCell] != 0;
		}

		std::vector<std::vector<int>> adjacency(nodes.size());
		for (std::size_t first = 0; first < nodes.size(); ++first)
		{
			for (std::size_t second = first + 1; second < nodes.size(); ++second)
			{
				const Vector delta = nodes[second].feet - nodes[first].feet;
				const float xyDistance = delta.Length2D();
				if (xyDistance < resolution * 0.9f ||
					xyDistance > resolution * 1.1f)
					continue;
				CollisionMapperCell fromCell;
				fromCell.feet = nodes[first].feet;
				CollisionMapperCell toCell;
				toCell.feet = nodes[second].feet;
				const CollisionMapperEdgeEvaluation evaluation =
					EvaluateCollisionMapperEdge(fromCell, toCell, hullMins, hullMaxs,
						parentBuilding);
				if (!evaluation.traversable)
				{
					continue;
				}
				if (evaluation.state == CollisionMapperEdgeState::DROP_DOWN)
				{
					if (nodes[first].feet.z > nodes[second].feet.z)
						adjacency[first].push_back(static_cast<int>(second));
					else
						adjacency[second].push_back(static_cast<int>(first));
				}
				else
				{
					adjacency[first].push_back(static_cast<int>(second));
					adjacency[second].push_back(static_cast<int>(first));
				}
			}
		}

		std::vector<int> localPredecessor(nodes.size(), -1);
		std::queue<int> pending;
		for (std::size_t index = 0; index < nodes.size(); ++index)
		{
			if (!nodes[index].spawnSide)
				continue;
			localPredecessor[index] = static_cast<int>(index);
			pending.push(static_cast<int>(index));
		}
		int target = -1;
		while (!pending.empty())
		{
			const int current = pending.front();
			pending.pop();
			if (nodes[current].portalSide)
			{
				target = current;
				break;
			}
			for (const int neighbor : adjacency[current])
			{
				if (localPredecessor[neighbor] != -1)
					continue;
				localPredecessor[neighbor] = current;
				pending.push(neighbor);
			}
		}
		std::vector<int> localPath;
		if (target >= 0)
		{
			for (int current = target; ; current = localPredecessor[current])
			{
				localPath.push_back(current);
				if (localPredecessor[current] == current)
					break;
			}
			std::reverse(localPath.begin(), localPath.end());
		}
		bool frontierCrossed = false;
		float nearestFrontierDistance = std::numeric_limits<float>::max();
		for (const int pathNode : localPath)
		{
			nearestFrontierDistance = std::min(nearestFrontierDistance,
				(nodes[pathNode].feet - frontierMid).Length2D());
		}
		for (std::size_t index = 1; index < localPath.size(); ++index)
		{
			const int firstCell = nodes[localPath[index - 1]].coarseCell;
			const int secondCell = nodes[localPath[index]].coarseCell;
			if ((firstCell == frontierEdge.from && secondCell == frontierEdge.to) ||
				(firstCell == frontierEdge.to && secondCell == frontierEdge.from))
			{
				frontierCrossed = true;
				break;
			}
		}

		std::vector<int> evidenceIndices;
		for (std::size_t index = 0; index < wallEvidence.size(); ++index)
		{
			if ((wallEvidence[index].point - frontierMid).Length2D() <= 256.0f)
				evidenceIndices.push_back(static_cast<int>(index));
		}
		for (const int evidenceIndex : evidenceIndices)
		{
			const CollisionMapperAdaptiveBoundary &boundary =
				wallEvidence[evidenceIndex];
			Msg("[RCBOT2] NavMesh collision-mapper adaptive_wall_boundary "
				"resolution=%.1f point=(%.1f %.1f %.1f) ground_z=%.1f\n",
				resolution, boundary.point.x, boundary.point.y, boundary.point.z,
				boundary.groundZ);
		}
		std::vector<int> cluster;
		if (!evidenceIndices.empty())
		{
			std::vector<char> visited(wallEvidence.size(), 0);
			std::queue<int> evidencePending;
			int seed = evidenceIndices.front();
			float seedDistance =
				(wallEvidence[seed].point - frontierMid).Length2D();
			for (const int candidate : evidenceIndices)
			{
				const float distance =
					(wallEvidence[candidate].point - frontierMid).Length2D();
				if (distance < seedDistance)
				{
					seed = candidate;
					seedDistance = distance;
				}
			}
			visited[seed] = 1;
			evidencePending.push(seed);
			while (!evidencePending.empty())
			{
				const int current = evidencePending.front();
				evidencePending.pop();
				cluster.push_back(current);
				for (const int candidate : evidenceIndices)
				{
				if (visited[candidate] ||
					(wallEvidence[candidate].point - wallEvidence[current].point).Length2D() >
							resolution * 2.5f)
						continue;
					visited[candidate] = 1;
					evidencePending.push(candidate);
				}
			}
		}
		int endpointA = cluster.empty() ? -1 : cluster.front();
		int endpointB = endpointA;
		float endpointSpan = 0.0f;
		for (const int first : cluster)
		{
			for (const int second : cluster)
			{
				const float distance = (wallEvidence[second].point -
					wallEvidence[first].point).Length2D();
				if (distance > endpointSpan)
				{
					endpointSpan = distance;
					endpointA = first;
					endpointB = second;
				}
			}
		}
		Msg("[RCBOT2] NavMesh collision-mapper adaptive_summary "
			"resolution=%.1f samples=%d nodes=%d wall_evidence=%d "
			"wall_cluster=%d endpoint_span=%.1f local_path=%s "
			"frontier_crossed=%d nearest_frontier_distance=%.1f\n", resolution,
			static_cast<int>(samples.size()), static_cast<int>(nodes.size()),
			static_cast<int>(wallEvidence.size()), static_cast<int>(cluster.size()),
			endpointSpan, localPath.empty() ? "NO" : "YES",
			frontierCrossed ? 1 : 0, nearestFrontierDistance);
		if (endpointA >= 0 && endpointB >= 0)
		{
			const int endpointIndices[] = { endpointA, endpointB };
			for (int endpointIndex = 0; endpointIndex < 2; ++endpointIndex)
			{
				const Vector endpoint =
					wallEvidence[endpointIndices[endpointIndex]].point;
				int nearbyFree = 0;
				float nearestFreeDistance =
					std::numeric_limits<float>::max();
				std::vector<float> endpointLevels;
				for (const CollisionMapperAdaptiveNode &node : nodes)
				{
					const float distance = (node.feet - endpoint).Length2D();
					nearestFreeDistance = std::min(nearestFreeDistance, distance);
					if (distance > resolution * 2.5f)
						continue;
					++nearbyFree;
					bool duplicate = false;
					for (const float z : endpointLevels)
						if (std::fabs(z - node.feet.z) < 2.0f)
							duplicate = true;
					if (!duplicate)
						endpointLevels.push_back(node.feet.z);
				}
					Msg("[RCBOT2] NavMesh collision-mapper adaptive_wall_endpoint "
					"resolution=%.1f endpoint=%d position=(%.1f %.1f %.1f) "
					"nearby_free=%d nearest_free_distance=%.1f ground_layers=%d\n",
					resolution, endpointIndex, endpoint.x, endpoint.y, endpoint.z,
					nearbyFree, nearestFreeDistance,
					static_cast<int>(endpointLevels.size()));
				for (const float z : endpointLevels)
					Msg("[RCBOT2] NavMesh collision-mapper adaptive_wall_endpoint_layer "
						"resolution=%.1f endpoint=%d z=%.1f\n", resolution,
						endpointIndex, z);
			}
		}
		if (!localPath.empty())
			PrintCollisionMapperAdaptivePath(localPath, nodes, resolution,
				"spawn-to-portal-local-adaptive");
	}
	Msg("[RCBOT2] NavMesh collision-mapper adaptive_end\n");
}

void RunCollisionMapper(const Vector &requestedStart, const Vector &portal,
	const int requestedPlayerIndex, edict_t *barracks, edict_t *vehiclePhysbox)
{
	constexpr float kResolution = 16.0f;
	constexpr float kPadding = 128.0f;
	if (barracks == nullptr || barracks->GetCollideable() == nullptr ||
		enginetrace == nullptr)
	{
		Msg("[RCBOT2] NavMesh collision-mapper result=INVALID "
			"reason=missing-parent-collideable\n");
		return;
	}

	CollisionMapperHull hull;
	if (!ResolveCollisionMapperHull(requestedStart, requestedPlayerIndex,
		&hull))
	{
		Msg("[RCBOT2] NavMesh collision-mapper result=INVALID "
			"reason=actual-player-hull-unavailable requested_start=(%.1f %.1f %.1f)\n",
			requestedStart.x, requestedStart.y, requestedStart.z);
		return;
	}

	Vector buildingMins = vec3_origin;
	Vector buildingMaxs = vec3_origin;
	if (!GetEntityWorldBounds(barracks, &buildingMins, &buildingMaxs))
	{
		Msg("[RCBOT2] NavMesh collision-mapper result=INVALID "
			"reason=parent-world-bounds-unavailable\n");
		return;
	}

	const float minX = std::floor((buildingMins.x - kPadding) / kResolution) *
		kResolution;
	const float maxX = std::ceil((buildingMaxs.x + kPadding) / kResolution) *
		kResolution;
	const float minY = std::floor((buildingMins.y - kPadding) / kResolution) *
		kResolution;
	const float maxY = std::ceil((buildingMaxs.y + kPadding) / kResolution) *
		kResolution;
	const int width = static_cast<int>(std::round((maxX - minX) /
		kResolution)) + 1;
	const int height = static_cast<int>(std::round((maxY - minY) /
		kResolution)) + 1;
	if (width <= 0 || height <= 0 || width > 128 || height > 128)
	{
		Msg("[RCBOT2] NavMesh collision-mapper result=INVALID "
			"reason=grid-size width=%d height=%d\n", width, height);
		return;
	}

	constexpr unsigned int kPortalFromAreaId = 2;
	constexpr unsigned int kPortalToAreaId = 150;
	CNavArea *portalFromArea = TheNavMesh != nullptr ?
		TheNavMesh->GetNavAreaByID(kPortalFromAreaId) : nullptr;
	CNavArea *portalToArea = TheNavMesh != nullptr ?
		TheNavMesh->GetNavAreaByID(kPortalToAreaId) : nullptr;
	NavDirType portalDirection = NUM_DIRECTIONS;
	Vector portalCenter = vec3_origin;
	float portalHalfWidth = 0.0f;
	if (portalFromArea != nullptr && portalToArea != nullptr)
	{
		for (int direction = 0; direction < NUM_DIRECTIONS; ++direction)
		{
			const NavDirType candidate = static_cast<NavDirType>(direction);
			if (!portalFromArea->IsConnected(portalToArea, candidate))
				continue;
			portalDirection = candidate;
			portalFromArea->ComputePortal(portalToArea, portalDirection,
				&portalCenter, &portalHalfWidth);
			break;
		}
	}

	Msg("[RCBOT2] NavMesh collision-mapper begin resolution=%.1f "
		"width=%d height=%d requested_start=(%.1f %.1f %.1f) "
		"portal=(%.1f %.1f %.1f)\n", kResolution, width, height,
		requestedStart.x, requestedStart.y, requestedStart.z, portal.x,
		portal.y, portal.z);
	const Vector spawnDelta = hull.feet - requestedStart;
	Msg("[RCBOT2] NavMesh collision-mapper player index=%d "
		"requested_spawn=(%.1f %.1f %.1f) actual_spawn=(%.1f %.1f %.1f) "
		"mapper_start=(%.1f %.1f %.1f) delta=(%.1f %.1f %.1f) "
		"hull_mins=(%.1f %.1f %.1f) hull_maxs=(%.1f %.1f %.1f) source=%s\n",
		hull.playerIndex, requestedStart.x, requestedStart.y, requestedStart.z,
		hull.feet.x, hull.feet.y, hull.feet.z, hull.feet.x, hull.feet.y,
		hull.feet.z, spawnDelta.x, spawnDelta.y, spawnDelta.z, hull.mins.x,
		hull.mins.y, hull.mins.z, hull.maxs.x, hull.maxs.y, hull.maxs.z,
		hull.fromPlayerInfo ? "IPlayerInfo" : "player-collideable-fallback");
	Msg("[RCBOT2] NavMesh collision-mapper building_bounds min=(%.1f %.1f %.1f) "
		"max=(%.1f %.1f %.1f) parent_entity=%d physbox_entity=%d\n",
		buildingMins.x, buildingMins.y, buildingMins.z, buildingMaxs.x,
		buildingMaxs.y, buildingMaxs.z, engine->IndexOfEdict(barracks),
		vehiclePhysbox != nullptr ? engine->IndexOfEdict(vehiclePhysbox) : -1);
	const CollisionMapperGround actualSpawnGround = FindCollisionMapperGround(
		hull.feet, hull.feet.z, hull.mins, hull.maxs, barracks);
	const float actualGroundDelta = actualSpawnGround.valid ?
		actualSpawnGround.feet.z - hull.feet.z : 0.0f;
	const bool actualGroundStable = actualSpawnGround.valid &&
		std::fabs(actualGroundDelta) <= 4.0f;
	Msg("[RCBOT2] NavMesh collision-mapper spawn_ground actual=(%.1f %.1f %.1f) "
		"ground=(%.1f %.1f %.1f) source=%s normal=(%.3f %.3f %.3f) "
		"surface=%s delta_z=%.1f stable=%d\n", hull.feet.x, hull.feet.y,
		hull.feet.z, actualSpawnGround.feet.x, actualSpawnGround.feet.y,
		actualSpawnGround.feet.z, actualSpawnGround.source,
		actualSpawnGround.normal.x, actualSpawnGround.normal.y,
		actualSpawnGround.normal.z, actualSpawnGround.surface.c_str(),
		actualGroundDelta, actualGroundStable ? 1 : 0);
	if (!actualGroundStable)
	{
		Msg("[RCBOT2] NavMesh collision-mapper result=INVALID "
			"reason=actual-player-ground-not-stable\n");
		return;
	}
	const CollisionMapperGround requestedSeedGround = FindCollisionMapperGround(
		requestedStart, requestedStart.z, hull.mins, hull.maxs, barracks);
	const Vector mapperSeed = requestedSeedGround.valid ?
		requestedSeedGround.feet : requestedStart;
	Msg("[RCBOT2] NavMesh collision-mapper seed requested=(%.1f %.1f %.1f) "
		"projected=(%.1f %.1f %.1f) ground_source=%s normal=(%.3f %.3f %.3f) "
		"surface=%s valid=%d\n", requestedStart.x, requestedStart.y,
		requestedStart.z, mapperSeed.x, mapperSeed.y, mapperSeed.z,
		requestedSeedGround.source, requestedSeedGround.normal.x,
		requestedSeedGround.normal.y, requestedSeedGround.normal.z,
		requestedSeedGround.surface.c_str(), requestedSeedGround.valid ? 1 : 0);
	if (portalFromArea == nullptr || portalToArea == nullptr ||
		portalDirection == NUM_DIRECTIONS)
	{
		Msg("[RCBOT2] NavMesh collision-mapper portal_areas from=%u to=%u "
			"result=INVALID reason=adjacent-portal-unavailable\n",
			kPortalFromAreaId, kPortalToAreaId);
	}
	else
	{
		Extent fromExtent;
		Extent toExtent;
		portalFromArea->GetExtent(&fromExtent);
		portalToArea->GetExtent(&toExtent);
		Msg("[RCBOT2] NavMesh collision-mapper portal_areas from=%u to=%u "
			"direction=%d center=(%.1f %.1f %.1f) half_width=%.1f "
			"from_min=(%.1f %.1f %.1f) from_max=(%.1f %.1f %.1f) "
			"to_min=(%.1f %.1f %.1f) to_max=(%.1f %.1f %.1f)\n",
			kPortalFromAreaId, kPortalToAreaId, static_cast<int>(portalDirection),
			portalCenter.x, portalCenter.y, portalCenter.z, portalHalfWidth,
			fromExtent.lo.x, fromExtent.lo.y, fromExtent.lo.z, fromExtent.hi.x,
			fromExtent.hi.y, fromExtent.hi.z, toExtent.lo.x, toExtent.lo.y,
			toExtent.lo.z, toExtent.hi.x, toExtent.hi.y, toExtent.hi.z);
	}

	std::vector<CollisionMapperCell> cells;
	cells.resize(static_cast<std::size_t>(width * height));
	int freeCount = 0;
	int blockedCount = 0;
	int noGroundCount = 0;
	int invalidCount = 0;
	int physboxSolidCount = 0;
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const int index = y * width + x;
			CollisionMapperCell &cell = cells[index];
			cell.x = x;
			cell.y = y;
			cell.sample = Vector(minX + x * kResolution,
				minY + y * kResolution, mapperSeed.z);
			cell.insideBuildingBounds = cell.sample.x >= buildingMins.x &&
				cell.sample.x <= buildingMaxs.x && cell.sample.y >= buildingMins.y &&
				cell.sample.y <= buildingMaxs.y;

			CollisionMapperGround ground = FindCollisionMapperGround(cell.sample,
				mapperSeed.z, hull.mins, hull.maxs, barracks);
			cell.feet = ground.valid ? ground.feet : cell.sample;
			cell.groundNormal = ground.normal;
			cell.groundSource = ground.source;
			cell.groundSurface = ground.surface;

			trace_t buildingTrace;
			trace_t physboxTrace;
			const bool parentProbe = ClipCollisionMapperHullToEntity(cell.feet,
				cell.feet, hull.mins, hull.maxs, barracks, &buildingTrace);
			const bool physboxProbe = ClipCollisionMapperHullToEntity(cell.feet,
				cell.feet, hull.mins, hull.maxs, vehiclePhysbox, &physboxTrace);
			cell.buildingFraction = parentProbe ? buildingTrace.fraction : 0.0f;
			cell.physboxSolid = physboxProbe && (physboxTrace.startsolid ||
				physboxTrace.allsolid || physboxTrace.fraction < 1.0f);
			if (cell.physboxSolid)
				++physboxSolidCount;

			if (!parentProbe || (!ground.valid && !cell.feet.IsValid()))
				cell.state = CollisionMapperCellState::INVALID;
			else if (buildingTrace.startsolid || buildingTrace.allsolid ||
				buildingTrace.fraction < 1.0f)
				cell.state = CollisionMapperCellState::BLOCKED_BY_BUILDING;
			else if (!ground.valid)
				cell.state = CollisionMapperCellState::NO_GROUND;
			else
				cell.state = CollisionMapperCellState::FREE;

			switch (cell.state)
			{
			case CollisionMapperCellState::FREE:
				++freeCount;
				break;
			case CollisionMapperCellState::BLOCKED_BY_BUILDING:
				++blockedCount;
				break;
			case CollisionMapperCellState::NO_GROUND:
				++noGroundCount;
				break;
			case CollisionMapperCellState::INVALID:
				++invalidCount;
				break;
			}
			Msg("[RCBOT2] NavMesh collision-mapper cell grid=(%d %d) "
				"sample=(%.1f %.1f %.1f) feet=(%.1f %.1f %.1f) state=%s "
				"parent_fraction=%.6f parent_startsolid=%d parent_allsolid=%d "
				"physbox_solid=%d ground_source=%s ground_normal=(%.3f %.3f %.3f) "
				"surface=%s inside_aabb=%d\n", x, y, cell.sample.x,
				cell.sample.y, cell.sample.z, cell.feet.x, cell.feet.y,
				cell.feet.z, CollisionMapperCellStateName(cell.state),
				cell.buildingFraction, buildingTrace.startsolid ? 1 : 0,
				buildingTrace.allsolid ? 1 : 0, cell.physboxSolid ? 1 : 0,
				ground.source, cell.groundNormal.x, cell.groundNormal.y,
				cell.groundNormal.z, cell.groundSurface.c_str(),
				cell.insideBuildingBounds ? 1 : 0);
			DrawCollisionMapperCell(cell, 15.0f);
		}
	}

	const Vector criticalFirst(-7536.0f, -9984.0f, mapperSeed.z);
	const Vector criticalSecond(-7552.0f, -9984.0f, mapperSeed.z);
	auto FindCollisionMapperCellNear = [&cells](const Vector &point)
	{
		int bestIndex = -1;
		float bestDistance = std::numeric_limits<float>::max();
		for (std::size_t index = 0; index < cells.size(); ++index)
		{
			const float dx = cells[index].sample.x - point.x;
			const float dy = cells[index].sample.y - point.y;
			const float distance = dx * dx + dy * dy;
			if (distance < bestDistance)
			{
				bestDistance = distance;
				bestIndex = static_cast<int>(index);
			}
		}
		return bestIndex;
	};
	auto PrintCollisionMapperCriticalCell = [&cells](const char *label,
		const int index)
	{
		if (index < 0 || index >= static_cast<int>(cells.size()))
			return;
		const CollisionMapperCell &cell = cells[index];
		Msg("[RCBOT2] NavMesh collision-mapper critical_cell label=%s "
			"grid=(%d %d) sample=(%.1f %.1f %.1f) feet=(%.1f %.1f %.1f) "
			"state=%s ground_source=%s ground_normal=(%.3f %.3f %.3f) "
			"surface=%s parent_fraction=%.6f\n", label, cell.x, cell.y,
			cell.sample.x, cell.sample.y, cell.sample.z, cell.feet.x,
			cell.feet.y, cell.feet.z,
			CollisionMapperCellStateName(cell.state), cell.groundSource,
			cell.groundNormal.x, cell.groundNormal.y, cell.groundNormal.z,
			cell.groundSurface.c_str(), cell.buildingFraction);
	};
	const int criticalFirstCell = FindCollisionMapperCellNear(criticalFirst);
	const int criticalSecondCell = FindCollisionMapperCellNear(criticalSecond);
	PrintCollisionMapperCriticalCell("first", criticalFirstCell);
	PrintCollisionMapperCriticalCell("second", criticalSecondCell);

	// The critical transition is sampled along the ground surface rather than
	// as one diagonal endpoint-to-endpoint ray. This distinguishes a ramp from
	// a building wall that merely makes the two endpoint heights differ.
	std::vector<Vector> criticalSurfaceFeet;
	constexpr int kCriticalSurfaceSamples = 9;
	for (int sampleIndex = 0; sampleIndex < kCriticalSurfaceSamples;
		++sampleIndex)
	{
		const float t = static_cast<float>(sampleIndex) /
			static_cast<float>(kCriticalSurfaceSamples - 1);
		const Vector xy(criticalFirst.x +
			(criticalSecond.x - criticalFirst.x) * t,
			criticalFirst.y + (criticalSecond.y - criticalFirst.y) * t,
			0.0f);
		const float referenceZ = mapperSeed.z +
			(criticalFirst.z - mapperSeed.z) * t;
		const CollisionMapperGround ground = FindCollisionMapperGround(xy,
			referenceZ, hull.mins, hull.maxs, barracks);
		if (!ground.valid)
		{
			Msg("[RCBOT2] NavMesh collision-mapper critical_surface_sample "
				"index=%d t=%.3f xy=(%.1f %.1f) ground_valid=0\n",
				sampleIndex, t, xy.x, xy.y);
			continue;
		}
		criticalSurfaceFeet.push_back(ground.feet);
		trace_t parentPointTrace;
		const bool parentPointProbe = ClipCollisionMapperHullToEntity(
			ground.feet, ground.feet, hull.mins, hull.maxs, barracks,
			&parentPointTrace);
		Msg("[RCBOT2] NavMesh collision-mapper critical_surface_sample "
				"index=%d t=%.3f xy=(%.1f %.1f) ground_valid=1 feet=(%.1f %.1f %.1f) "
				"ground_source=%s ground_normal=(%.3f %.3f %.3f) surface=%s "
				"parent_fraction=%.6f parent_startsolid=%d parent_allsolid=%d\n",
				sampleIndex, t, xy.x, xy.y, ground.feet.x, ground.feet.y,
				ground.feet.z, ground.source, ground.normal.x, ground.normal.y,
				ground.normal.z, ground.surface.c_str(),
				parentPointProbe ? parentPointTrace.fraction : 0.0f,
				parentPointProbe && parentPointTrace.startsolid ? 1 : 0,
				parentPointProbe && parentPointTrace.allsolid ? 1 : 0);
	}

	if (criticalSurfaceFeet.size() >= 2)
	{
		for (std::size_t index = 1; index < criticalSurfaceFeet.size(); ++index)
		{
			const Vector &fromFeet = criticalSurfaceFeet[index - 1];
			const Vector &toFeet = criticalSurfaceFeet[index];
			const Vector horizontalFrom(fromFeet.x, fromFeet.y, fromFeet.z);
			const Vector horizontalTo(toFeet.x, toFeet.y, fromFeet.z);
			trace_t parentEqualHeightTrace;
			trace_t worldEqualHeightTrace;
			const bool parentProbe = ClipCollisionMapperHullToEntity(
				horizontalFrom, horizontalTo, hull.mins, hull.maxs, barracks,
				&parentEqualHeightTrace);
			const bool worldProbe = TraceCollisionMapperWorldHull(horizontalFrom,
				horizontalTo, hull.mins, hull.maxs, &worldEqualHeightTrace);
			const char *worldSurface = worldProbe &&
				worldEqualHeightTrace.surface.name != nullptr &&
				*worldEqualHeightTrace.surface.name != '\0' ?
				worldEqualHeightTrace.surface.name : "<none>";
			Msg("[RCBOT2] NavMesh collision-mapper critical_surface_interval "
				"index=%d from=(%.1f %.1f %.1f) to=(%.1f %.1f %.1f) "
				"dz=%.1f parent_equal_fraction=%.6f "
				"parent_equal_startsolid=%d parent_equal_allsolid=%d "
				"world_equal_fraction=%.6f world_equal_startsolid=%d "
				"world_equal_allsolid=%d world_surface=%s\n", static_cast<int>(index),
				fromFeet.x, fromFeet.y, fromFeet.z, toFeet.x, toFeet.y,
				toFeet.z, toFeet.z - fromFeet.z,
				parentProbe ? parentEqualHeightTrace.fraction : 0.0f,
				parentProbe && parentEqualHeightTrace.startsolid ? 1 : 0,
				parentProbe && parentEqualHeightTrace.allsolid ? 1 : 0,
				worldProbe ? worldEqualHeightTrace.fraction : 0.0f,
				worldProbe && worldEqualHeightTrace.startsolid ? 1 : 0,
				worldProbe && worldEqualHeightTrace.allsolid ? 1 : 0,
				worldSurface);
		}
	}

	const float criticalHeights[] = {
		cells[criticalFirstCell].feet.z,
		cells[criticalSecondCell].feet.z,
		(cells[criticalFirstCell].feet.z + cells[criticalSecondCell].feet.z) * 0.5f
	};
	for (int levelIndex = 0; levelIndex < 3; ++levelIndex)
	{
		const float level = criticalHeights[levelIndex];
		const Vector from(criticalFirst.x, criticalFirst.y, level);
		const Vector to(criticalSecond.x, criticalSecond.y, level);
		trace_t parentTrace;
		trace_t worldTrace;
		const bool parentProbe = ClipCollisionMapperHullToEntity(from, to,
			hull.mins, hull.maxs, barracks, &parentTrace);
		const bool worldProbe = TraceCollisionMapperWorldHull(from, to,
			hull.mins, hull.maxs, &worldTrace);
		const char *worldSurface = worldProbe && worldTrace.surface.name != nullptr &&
			*worldTrace.surface.name != '\0' ? worldTrace.surface.name : "<none>";
		Msg("[RCBOT2] NavMesh collision-mapper critical_equal_height "
			"level=%d z=%.1f parent_fraction=%.6f parent_startsolid=%d "
			"parent_allsolid=%d world_fraction=%.6f world_startsolid=%d "
			"world_allsolid=%d world_surface=%s\n", levelIndex, level,
			parentProbe ? parentTrace.fraction : 0.0f,
			parentProbe && parentTrace.startsolid ? 1 : 0,
			parentProbe && parentTrace.allsolid ? 1 : 0,
			worldProbe ? worldTrace.fraction : 0.0f,
			worldProbe && worldTrace.startsolid ? 1 : 0,
			worldProbe && worldTrace.allsolid ? 1 : 0, worldSurface);
	}

	std::vector<CollisionMapperPortalSample> portalSamples;
	if (portalDirection != NUM_DIRECTIONS)
	{
		Vector portalNormal = vec3_origin;
		switch (portalDirection)
		{
		case NORTH:
			portalNormal.y = -1.0f;
			break;
		case EAST:
			portalNormal.x = 1.0f;
			break;
		case SOUTH:
			portalNormal.y = 1.0f;
			break;
		case WEST:
			portalNormal.x = -1.0f;
			break;
		default:
			break;
		}

		const float lateralFractions[] = {
			-0.90f, -0.65f, -0.40f, -0.15f, 0.15f, 0.40f, 0.65f, 0.90f
		};
		const float insets[] = { 0.0f, kResolution * 0.5f, kResolution };
		int sampleIndex = 0;
		for (const float lateralFraction : lateralFractions)
		{
			for (const float inset : insets)
			{
				CollisionMapperPortalSample sample;
				sample.index = sampleIndex++;
				sample.lateral = lateralFraction * portalHalfWidth;
				sample.inset = inset;
				sample.candidate = portalCenter + portalNormal * inset;
				if (portalDirection == NORTH || portalDirection == SOUTH)
					sample.candidate.x += sample.lateral;
				else
					sample.candidate.y += sample.lateral;

				sample.insideGoalArea = portalToArea != nullptr &&
					portalToArea->IsOverlapping(sample.candidate, 0.1f);
				if (sample.insideGoalArea)
					sample.candidate.z = portalToArea->GetZ(sample.candidate.x,
						sample.candidate.y);

				const CollisionMapperGround ground = FindCollisionMapperGround(
					sample.candidate, sample.candidate.z, hull.mins, hull.maxs,
					barracks);
				if (ground.valid)
					sample.feet = ground.feet;
				else
					sample.feet = sample.candidate;

				trace_t buildingTrace;
				const bool parentProbe = ClipCollisionMapperHullToEntity(
					sample.feet, sample.feet, hull.mins, hull.maxs, barracks,
					&buildingTrace);
				sample.valid = sample.insideGoalArea && ground.valid &&
					parentProbe && !buildingTrace.startsolid &&
					!buildingTrace.allsolid && buildingTrace.fraction >= 1.0f;
				portalSamples.push_back(sample);
				Msg("[RCBOT2] NavMesh collision-mapper portal_sample index=%d "
					"lateral=%.1f inset=%.1f candidate=(%.1f %.1f %.1f) "
					"feet=(%.1f %.1f %.1f) inside_goal_area=%d ground=%d "
					"parent_fraction=%.6f parent_startsolid=%d parent_allsolid=%d "
					"valid=%d\n", sample.index, sample.lateral, sample.inset,
					sample.candidate.x, sample.candidate.y, sample.candidate.z,
					sample.feet.x, sample.feet.y, sample.feet.z,
					sample.insideGoalArea ? 1 : 0, ground.valid ? 1 : 0,
					parentProbe ? buildingTrace.fraction : 0.0f,
					parentProbe && buildingTrace.startsolid ? 1 : 0,
					parentProbe && buildingTrace.allsolid ? 1 : 0,
					sample.valid ? 1 : 0);
				if (debugoverlay != nullptr)
				{
					const int r = sample.valid ? 255 : 180;
					const int g = sample.valid ? 255 : 80;
					const int b = sample.valid ? 255 : 180;
					debugoverlay->AddBoxOverlay(sample.feet,
						Vector(-6.0f, -6.0f, 0.0f), Vector(6.0f, 6.0f, 6.0f),
						QAngle(0.0f, 0.0f, 0.0f), r, g, b, 220, 15.0f);
				}
			}
		}
	}

	std::vector<CollisionMapperEdge> edges;
	std::vector<std::vector<int>> adjacency(cells.size());
	int clearEdgeCount = 0;
	int blockedEdgeCount = 0;
	int walkFlatEdgeCount = 0;
	int walkSlopeEdgeCount = 0;
	int stepUpEdgeCount = 0;
	int dropDownEdgeCount = 0;
	int blockedBuildingEdgeCount = 0;
	int blockedWorldEdgeCount = 0;
	int noGroundEdgeCount = 0;
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const int fromIndex = y * width + x;
			if (cells[fromIndex].state != CollisionMapperCellState::FREE)
				continue;
			const int neighborX[] = { x + 1, x, x - 1, x };
			const int neighborY[] = { y, y + 1, y, y - 1 };
			for (int direction = 0; direction < 4; ++direction)
			{
				const int toX = neighborX[direction];
				const int toY = neighborY[direction];
				if (toX < 0 || toX >= width || toY < 0 || toY >= height ||
					(toY < y || (toY == y && toX <= x)))
					continue;
				const int toIndex = toY * width + toX;
				if (cells[toIndex].state != CollisionMapperCellState::FREE)
					continue;

				const CollisionMapperEdgeEvaluation evaluation =
					EvaluateCollisionMapperEdge(cells[fromIndex], cells[toIndex],
						hull.mins, hull.maxs, barracks);
				CollisionMapperEdge edge;
				edge.from = fromIndex;
				edge.to = toIndex;
				edge.clear = evaluation.traversable;
				edge.state = evaluation.state;
				edge.fraction = evaluation.horizontalParentFraction;
				edge.worldFraction = evaluation.horizontalWorldFraction;
				edge.fromGroundZ = evaluation.fromGroundZ;
				edge.toGroundZ = evaluation.toGroundZ;
				edge.maxLocalGroundDelta = evaluation.maxLocalGroundDelta;
				edges.push_back(edge);
				Msg("[RCBOT2] NavMesh collision-mapper edge from=(%d %d) "
					"to=(%d %d) state=%s traversable=%d "
					"parent_equal_fraction=%.6f world_equal_fraction=%.6f "
					"from_z=%.1f to_z=%.1f max_local_dz=%.1f\n",
					cells[fromIndex].x, cells[fromIndex].y, cells[toIndex].x,
					cells[toIndex].y, CollisionMapperEdgeStateName(edge.state),
					evaluation.traversable ? 1 : 0,
					evaluation.horizontalParentFraction,
					evaluation.horizontalWorldFraction, evaluation.fromGroundZ,
					evaluation.toGroundZ, evaluation.maxLocalGroundDelta);
				switch (edge.state)
				{
				case CollisionMapperEdgeState::WALK_FLAT:
					++walkFlatEdgeCount;
					break;
				case CollisionMapperEdgeState::WALK_SLOPE:
					++walkSlopeEdgeCount;
					break;
				case CollisionMapperEdgeState::STEP_UP:
					++stepUpEdgeCount;
					break;
				case CollisionMapperEdgeState::DROP_DOWN:
					++dropDownEdgeCount;
					break;
				case CollisionMapperEdgeState::BLOCKED_BY_BUILDING:
					++blockedBuildingEdgeCount;
					break;
				case CollisionMapperEdgeState::BLOCKED_BY_WORLD:
					++blockedWorldEdgeCount;
					break;
				case CollisionMapperEdgeState::NO_GROUND:
					++noGroundEdgeCount;
					break;
				}
				if (edge.clear)
				{
					++clearEdgeCount;
					if (edge.state == CollisionMapperEdgeState::DROP_DOWN)
					{
						if (cells[fromIndex].feet.z > cells[toIndex].feet.z)
							adjacency[fromIndex].push_back(toIndex);
						else
							adjacency[toIndex].push_back(fromIndex);
					}
					else
					{
						adjacency[fromIndex].push_back(toIndex);
						adjacency[toIndex].push_back(fromIndex);
					}
				}
				else
					++blockedEdgeCount;

				if (debugoverlay != nullptr && edge.clear)
					debugoverlay->AddLineOverlay(cells[fromIndex].feet,
						cells[toIndex].feet, 0, 220, 220, true, 15.0f);
			}
		}
	}
	Msg("[RCBOT2] NavMesh collision-mapper edge_classification_summary "
		"walk_flat=%d walk_slope=%d step_up=%d drop_down=%d "
		"blocked_building=%d blocked_world=%d no_ground=%d\n",
		walkFlatEdgeCount, walkSlopeEdgeCount, stepUpEdgeCount,
		dropDownEdgeCount, blockedBuildingEdgeCount, blockedWorldEdgeCount,
		noGroundEdgeCount);
	for (const CollisionMapperEdge &edge : edges)
	{
		if ((edge.from == criticalFirstCell && edge.to == criticalSecondCell) ||
			(edge.from == criticalSecondCell && edge.to == criticalFirstCell))
		{
			Msg("[RCBOT2] NavMesh collision-mapper critical_edge "
				"from_grid=(%d %d) to_grid=(%d %d) state=%s traversable=%d "
				"parent_equal_fraction=%.6f world_equal_fraction=%.6f "
				"from_z=%.1f to_z=%.1f max_local_dz=%.1f\n",
				cells[edge.from].x, cells[edge.from].y, cells[edge.to].x,
				cells[edge.to].y, CollisionMapperEdgeStateName(edge.state),
				edge.clear ? 1 : 0, edge.fraction, edge.worldFraction,
				edge.fromGroundZ, edge.toGroundZ, edge.maxLocalGroundDelta);
			break;
		}
	}

	int validPortalSampleCount = 0;
	int mappedPortalSampleCount = 0;
	const float maxPortalGridSnapDistance =
		std::sqrt(0.5f) * kResolution + 0.1f;
	for (CollisionMapperPortalSample &sample : portalSamples)
	{
		if (!sample.valid)
			continue;
		++validPortalSampleCount;
		float bestDistance = std::numeric_limits<float>::max();
		for (std::size_t index = 0; index < cells.size(); ++index)
		{
			const CollisionMapperCell &cell = cells[index];
			if (cell.state != CollisionMapperCellState::FREE ||
				portalToArea == nullptr ||
				!portalToArea->IsOverlapping(cell.feet, 0.1f))
				continue;
			const float dx = cell.feet.x - sample.feet.x;
			const float dy = cell.feet.y - sample.feet.y;
			const float distance = std::sqrt(dx * dx + dy * dy);
			if (distance < bestDistance)
			{
				bestDistance = distance;
				sample.gridCell = static_cast<int>(index);
			}
		}
		if (sample.gridCell >= 0 && bestDistance <= maxPortalGridSnapDistance)
		{
			++mappedPortalSampleCount;
			const CollisionMapperCell &cell = cells[sample.gridCell];
			Msg("[RCBOT2] NavMesh collision-mapper portal_sample_map index=%d "
				"grid=(%d %d) grid_feet=(%.1f %.1f %.1f) distance=%.1f\n",
				sample.index, cell.x, cell.y, cell.feet.x, cell.feet.y,
				cell.feet.z, bestDistance);
		}
		else
		{
			Msg("[RCBOT2] NavMesh collision-mapper portal_sample_map index=%d "
				"grid=(-1 -1) distance=%.1f max_distance=%.1f\n",
				sample.index, bestDistance, maxPortalGridSnapDistance);
			sample.gridCell = -1;
		}
	}

	int spawnCell = -1;
	float spawnDistance = std::numeric_limits<float>::max();
	for (std::size_t index = 0; index < cells.size(); ++index)
	{
		if (cells[index].state != CollisionMapperCellState::FREE)
			continue;
		const float dx = cells[index].feet.x - mapperSeed.x;
		const float dy = cells[index].feet.y - mapperSeed.y;
		const float distance = dx * dx + dy * dy;
		if (distance < spawnDistance)
		{
			spawnDistance = distance;
			spawnCell = static_cast<int>(index);
		}
	}

	std::vector<int> predecessor(cells.size(), -1);
	std::queue<int> pending;
	if (spawnCell >= 0)
	{
		predecessor[spawnCell] = spawnCell;
		pending.push(spawnCell);
	}
	while (!pending.empty())
	{
		const int current = pending.front();
		pending.pop();
		for (const int neighbor : adjacency[current])
		{
			if (predecessor[neighbor] != -1)
				continue;
			predecessor[neighbor] = current;
			pending.push(neighbor);
		}
	}

	int reachableFree = 0;
	int reachableInterior = 0;
	int interiorFree = 0;
	for (std::size_t index = 0; index < cells.size(); ++index)
	{
		const CollisionMapperCell &cell = cells[index];
		if (cell.state != CollisionMapperCellState::FREE)
			continue;
		if (cell.insideBuildingBounds)
			++interiorFree;
		if (predecessor[index] == -1)
			continue;
		++reachableFree;
		if (cell.insideBuildingBounds)
			++reachableInterior;
	}

	// The distribution scopes are emitted explicitly instead of deriving them
	// from the entity AABB for reachability decisions.
	int reachableGroundCount = 0;
	float reachableGroundMin = std::numeric_limits<float>::max();
	float reachableGroundMax = -std::numeric_limits<float>::max();
	float reachableGroundSum = 0.0f;
	int interiorGroundCount = 0;
	float interiorGroundMin = std::numeric_limits<float>::max();
	float interiorGroundMax = -std::numeric_limits<float>::max();
	float interiorGroundSum = 0.0f;
	int exteriorGroundCount = 0;
	float exteriorGroundMin = std::numeric_limits<float>::max();
	float exteriorGroundMax = -std::numeric_limits<float>::max();
	float exteriorGroundSum = 0.0f;
	for (std::size_t index = 0; index < cells.size(); ++index)
	{
		const CollisionMapperCell &cell = cells[index];
		if (cell.state != CollisionMapperCellState::FREE)
			continue;
		if (predecessor[index] != -1)
		{
			++reachableGroundCount;
			reachableGroundMin = std::min(reachableGroundMin, cell.feet.z);
			reachableGroundMax = std::max(reachableGroundMax, cell.feet.z);
			reachableGroundSum += cell.feet.z;
		}
		if (cell.insideBuildingBounds)
		{
			++interiorGroundCount;
			interiorGroundMin = std::min(interiorGroundMin, cell.feet.z);
			interiorGroundMax = std::max(interiorGroundMax, cell.feet.z);
			interiorGroundSum += cell.feet.z;
		}
		else
		{
			++exteriorGroundCount;
			exteriorGroundMin = std::min(exteriorGroundMin, cell.feet.z);
			exteriorGroundMax = std::max(exteriorGroundMax, cell.feet.z);
			exteriorGroundSum += cell.feet.z;
		}
	}
	Msg("[RCBOT2] NavMesh collision-mapper ground_distribution "
		"scope=reachable_component count=%d min_z=%.1f max_z=%.1f avg_z=%.1f\n",
		reachableGroundCount,
		reachableGroundCount > 0 ? reachableGroundMin : 0.0f,
		reachableGroundCount > 0 ? reachableGroundMax : 0.0f,
		reachableGroundCount > 0 ? reachableGroundSum /
			static_cast<float>(reachableGroundCount) : 0.0f);
	Msg("[RCBOT2] NavMesh collision-mapper ground_distribution "
		"scope=building_bounds_free count=%d min_z=%.1f max_z=%.1f avg_z=%.1f\n",
		interiorGroundCount, interiorGroundCount > 0 ? interiorGroundMin : 0.0f,
		interiorGroundCount > 0 ? interiorGroundMax : 0.0f,
		interiorGroundCount > 0 ? interiorGroundSum /
			static_cast<float>(interiorGroundCount) : 0.0f);
	Msg("[RCBOT2] NavMesh collision-mapper ground_distribution "
		"scope=outside_bounds_free count=%d min_z=%.1f max_z=%.1f avg_z=%.1f\n",
		exteriorGroundCount, exteriorGroundCount > 0 ? exteriorGroundMin : 0.0f,
		exteriorGroundCount > 0 ? exteriorGroundMax : 0.0f,
		exteriorGroundCount > 0 ? exteriorGroundSum /
			static_cast<float>(exteriorGroundCount) : 0.0f);

	constexpr float kLargeGroundJump = 16.0f;
	int loggedGroundJumps = 0;
	int groundJumpCount = 0;
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const int fromIndex = y * width + x;
			if (cells[fromIndex].state != CollisionMapperCellState::FREE)
				continue;
			const int neighborX[] = { x + 1, x };
			const int neighborY[] = { y, y + 1 };
			for (int direction = 0; direction < 2; ++direction)
			{
				if (neighborX[direction] >= width || neighborY[direction] >= height)
					continue;
				const int toIndex = neighborY[direction] * width +
					neighborX[direction];
				if (cells[toIndex].state != CollisionMapperCellState::FREE)
					continue;
				const float delta = cells[toIndex].feet.z -
					cells[fromIndex].feet.z;
				if (std::fabs(delta) < kLargeGroundJump)
					continue;
				++groundJumpCount;
				if (loggedGroundJumps < 256)
				{
					Msg("[RCBOT2] NavMesh collision-mapper ground_z_jump "
						"from_grid=(%d %d) to_grid=(%d %d) from_feet=(%.1f %.1f %.1f) "
						"to_feet=(%.1f %.1f %.1f) delta_z=%.1f\n",
						cells[fromIndex].x, cells[fromIndex].y, cells[toIndex].x,
						cells[toIndex].y, cells[fromIndex].feet.x,
						cells[fromIndex].feet.y, cells[fromIndex].feet.z,
						cells[toIndex].feet.x, cells[toIndex].feet.y,
						cells[toIndex].feet.z, delta);
					++loggedGroundJumps;
				}
			}
		}
	}
	Msg("[RCBOT2] NavMesh collision-mapper ground_z_jump_summary "
		"threshold=%.1f count=%d logged=%d\n", kLargeGroundJump,
		groundJumpCount, loggedGroundJumps);

	for (std::size_t index = 0; index < cells.size(); ++index)
	{
		const CollisionMapperCell &cell = cells[index];
		if (cell.state != CollisionMapperCellState::FREE ||
			predecessor[index] == -1)
			continue;
		if (debugoverlay != nullptr)
			debugoverlay->AddBoxOverlay(cell.feet, Vector(-5.0f, -5.0f, 1.0f),
				Vector(5.0f, 5.0f, 9.0f), QAngle(0.0f, 0.0f, 0.0f),
				0, 120, 255, 180, 15.0f);
	}

	std::vector<int> portalPath;
	int reachedPortalSample = -1;
	int reachedPortalCell = -1;
	float reachedPortalPathLength = std::numeric_limits<float>::max();
	for (CollisionMapperPortalSample &sample : portalSamples)
	{
		if (!sample.valid || sample.gridCell < 0)
			continue;
		const std::vector<int> candidatePath =
			BuildCollisionMapperPath(predecessor, spawnCell, sample.gridCell);
		const bool reachable = !candidatePath.empty();
		float candidateLength = 0.0f;
		for (std::size_t i = 1; i < candidatePath.size(); ++i)
			candidateLength += (cells[candidatePath[i]].feet -
				cells[candidatePath[i - 1]].feet).Length2D();
		Msg("[RCBOT2] NavMesh collision-mapper portal_sample_reachability "
			"index=%d grid=(%d %d) reachable=%d path_cells=%d path_length=%.1f\n",
			sample.index, cells[sample.gridCell].x, cells[sample.gridCell].y,
			reachable ? 1 : 0, static_cast<int>(candidatePath.size()),
			candidateLength);
		if (reachable && candidateLength < reachedPortalPathLength)
		{
			reachedPortalPathLength = candidateLength;
			reachedPortalSample = sample.index;
			reachedPortalCell = sample.gridCell;
			portalPath = candidatePath;
		}
	}
	if (!portalPath.empty())
	{
		PrintCollisionMapperPath(portalPath, cells, "spawn-to-portal");
		const CollisionMapperPortalSample *sample = nullptr;
		for (const CollisionMapperPortalSample &candidate : portalSamples)
		{
			if (candidate.index == reachedPortalSample)
			{
				sample = &candidate;
				break;
			}
		}
		if (sample != nullptr)
		{
			const CollisionMapperCell &cell = cells[reachedPortalCell];
			Msg("[RCBOT2] NavMesh collision-mapper portal_reached sample=%d "
				"sample_feet=(%.1f %.1f %.1f) grid=(%d %d) "
				"grid_feet=(%.1f %.1f %.1f)\n", sample->index, sample->feet.x,
				sample->feet.y, sample->feet.z, cell.x, cell.y, cell.feet.x,
				cell.feet.y, cell.feet.z);
		}
	}

	// Diagnostic-only 8-neighbor comparison. The production-like mapper above
	// remains 4-neighbor; this second graph rejects diagonal corner cutting
	// whenever either orthogonal side cell is not FREE and still evaluates the
	// actual player-hull segment before accepting the diagonal.
	std::vector<std::vector<int>> adjacency8 = adjacency;
	int diagonalCandidateCount = 0;
	int diagonalCornerRejectedCount = 0;
	int diagonalClearCount = 0;
	int diagonalBlockedCount = 0;
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const int fromIndex = y * width + x;
			if (cells[fromIndex].state != CollisionMapperCellState::FREE)
				continue;
			for (int dy = -1; dy <= 1; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					if (dx == 0 || dy == 0)
						continue;
					const int toX = x + dx;
					const int toY = y + dy;
					if (toX < 0 || toX >= width || toY < 0 || toY >= height)
						continue;
					const int toIndex = toY * width + toX;
					if (toIndex <= fromIndex ||
						cells[toIndex].state != CollisionMapperCellState::FREE)
						continue;
					++diagonalCandidateCount;

					const int sideA = y * width + toX;
					const int sideB = toY * width + x;
					if (cells[sideA].state != CollisionMapperCellState::FREE ||
						cells[sideB].state != CollisionMapperCellState::FREE)
					{
						++diagonalCornerRejectedCount;
						continue;
					}

					const CollisionMapperEdgeEvaluation evaluation =
						EvaluateCollisionMapperEdge(cells[fromIndex], cells[toIndex],
							hull.mins, hull.maxs, barracks);
					if (!evaluation.traversable)
					{
						++diagonalBlockedCount;
						continue;
					}
					++diagonalClearCount;
					if (evaluation.state == CollisionMapperEdgeState::DROP_DOWN)
					{
						if (cells[fromIndex].feet.z > cells[toIndex].feet.z)
							adjacency8[fromIndex].push_back(toIndex);
						else
							adjacency8[toIndex].push_back(fromIndex);
					}
					else
					{
						adjacency8[fromIndex].push_back(toIndex);
						adjacency8[toIndex].push_back(fromIndex);
					}
					if (debugoverlay != nullptr)
						debugoverlay->AddLineOverlay(cells[fromIndex].feet,
							cells[toIndex].feet, 255, 160, 0, true, 15.0f);
				}
			}
		}
	}

	std::vector<int> predecessor8(cells.size(), -1);
	std::queue<int> pending8;
	if (spawnCell >= 0)
	{
		predecessor8[spawnCell] = spawnCell;
		pending8.push(spawnCell);
	}
	while (!pending8.empty())
	{
		const int current = pending8.front();
		pending8.pop();
		for (const int neighbor : adjacency8[current])
		{
			if (predecessor8[neighbor] != -1)
				continue;
			predecessor8[neighbor] = current;
			pending8.push(neighbor);
		}
	}

	int reachableFree8 = 0;
	for (std::size_t index = 0; index < cells.size(); ++index)
	{
		if (cells[index].state == CollisionMapperCellState::FREE &&
			predecessor8[index] != -1)
			++reachableFree8;
	}
	std::vector<int> portalPath8;
	int reachedPortalSample8 = -1;
	float reachedPortalPathLength8 = std::numeric_limits<float>::max();
	for (const CollisionMapperPortalSample &sample : portalSamples)
	{
		if (!sample.valid || sample.gridCell < 0)
			continue;
		const std::vector<int> candidatePath = BuildCollisionMapperPath(
			predecessor8, spawnCell, sample.gridCell);
		if (candidatePath.empty())
			continue;
		float candidateLength = 0.0f;
		for (std::size_t i = 1; i < candidatePath.size(); ++i)
			candidateLength += (cells[candidatePath[i]].feet -
				cells[candidatePath[i - 1]].feet).Length2D();
		if (candidateLength < reachedPortalPathLength8)
		{
			reachedPortalPathLength8 = candidateLength;
			reachedPortalSample8 = sample.index;
			portalPath8 = candidatePath;
		}
	}
	Msg("[RCBOT2] NavMesh collision-mapper connectivity_compare "
		"four_reachable=%d four_spawn_to_portal=%s eight_reachable=%d "
		"eight_spawn_to_portal=%s diagonal_candidates=%d "
		"diagonal_corner_rejected=%d diagonal_clear=%d diagonal_blocked=%d\n",
		reachableFree, portalPath.empty() ? "NO" : "YES", reachableFree8,
		portalPath8.empty() ? "NO" : "YES", diagonalCandidateCount,
		diagonalCornerRejectedCount, diagonalClearCount, diagonalBlockedCount);
	if (!portalPath8.empty())
	{
		PrintCollisionMapperPath(portalPath8, cells, "spawn-to-portal-8-neighbor");
		Msg("[RCBOT2] NavMesh collision-mapper portal_reached_8 sample=%d\n",
			reachedPortalSample8);
	}

	int nearestReachableCell = -1;
	float nearestReachablePortalDistance = std::numeric_limits<float>::max();
	auto DistanceToMappedPortal = [&cells, &portalSamples](const int index)
	{
		float best = std::numeric_limits<float>::max();
		for (const CollisionMapperPortalSample &sample : portalSamples)
		{
			if (!sample.valid || sample.gridCell < 0)
				continue;
			const float dx = cells[index].feet.x - sample.feet.x;
			const float dy = cells[index].feet.y - sample.feet.y;
			best = std::min(best, std::sqrt(dx * dx + dy * dy));
		}
		return best;
	};
	for (std::size_t index = 0; index < cells.size(); ++index)
	{
		if (predecessor[index] == -1 ||
			cells[index].state != CollisionMapperCellState::FREE)
			continue;
		for (const CollisionMapperPortalSample &sample : portalSamples)
		{
			if (!sample.valid)
				continue;
			const float dx = cells[index].feet.x - sample.feet.x;
			const float dy = cells[index].feet.y - sample.feet.y;
			const float distance = std::sqrt(dx * dx + dy * dy);
			if (distance < nearestReachablePortalDistance)
			{
				nearestReachablePortalDistance = distance;
				nearestReachableCell = static_cast<int>(index);
			}
		}
	}

	const CollisionMapperEdge *frontierWallEdge = nullptr;
	float frontierWallDistance = std::numeric_limits<float>::max();
	const CollisionMapperEdge *frontierWorldEdge = nullptr;
	float frontierWorldDistance = std::numeric_limits<float>::max();
	const CollisionMapperEdge *frontierNoGroundEdge = nullptr;
	float frontierNoGroundDistance = std::numeric_limits<float>::max();
	for (const CollisionMapperEdge &edge : edges)
	{
		if (edge.clear)
			continue;
		const bool fromReachable = predecessor[edge.from] != -1;
		const bool toReachable = predecessor[edge.to] != -1;
		if (fromReachable == toReachable)
			continue;
		const int frontier = fromReachable ? edge.from : edge.to;
		const float distance = DistanceToMappedPortal(frontier);
		if (edge.state == CollisionMapperEdgeState::BLOCKED_BY_BUILDING &&
			distance < frontierWallDistance)
		{
			frontierWallDistance = distance;
			frontierWallEdge = &edge;
		}
		else if (edge.state == CollisionMapperEdgeState::BLOCKED_BY_WORLD &&
			distance < frontierWorldDistance)
		{
			frontierWorldDistance = distance;
			frontierWorldEdge = &edge;
		}
		else if (edge.state == CollisionMapperEdgeState::NO_GROUND &&
			distance < frontierNoGroundDistance)
		{
			frontierNoGroundDistance = distance;
			frontierNoGroundEdge = &edge;
		}
	}

	int frontierNonFreeCell = -1;
	int frontierNonFreeNeighbor = -1;
	float frontierNonFreeDistance = std::numeric_limits<float>::max();
	for (std::size_t index = 0; index < cells.size(); ++index)
	{
		if (predecessor[index] == -1 ||
			cells[index].state != CollisionMapperCellState::FREE)
			continue;
		const int x = cells[index].x;
		const int y = cells[index].y;
		const int neighborX[] = { x + 1, x, x - 1, x };
		const int neighborY[] = { y, y + 1, y, y - 1 };
		for (int direction = 0; direction < 4; ++direction)
		{
			if (neighborX[direction] < 0 || neighborX[direction] >= width ||
				neighborY[direction] < 0 || neighborY[direction] >= height)
				continue;
			const int neighbor = neighborY[direction] * width + neighborX[direction];
			if (cells[neighbor].state == CollisionMapperCellState::FREE)
				continue;
			const float distance = DistanceToMappedPortal(static_cast<int>(index));
			if (distance < frontierNonFreeDistance)
			{
				frontierNonFreeDistance = distance;
				frontierNonFreeCell = static_cast<int>(index);
				frontierNonFreeNeighbor = neighbor;
			}
		}
	}

	int nearestCrossReachable = -1;
	int nearestCrossPortalSide = -1;
	float nearestCrossDistance = std::numeric_limits<float>::max();
	for (std::size_t reachableIndex = 0; reachableIndex < cells.size();
		reachableIndex++)
	{
		if (predecessor[reachableIndex] == -1 ||
			cells[reachableIndex].state != CollisionMapperCellState::FREE)
			continue;
		for (std::size_t portalSideIndex = 0; portalSideIndex < cells.size();
			portalSideIndex++)
		{
			if (predecessor[portalSideIndex] != -1 ||
				cells[portalSideIndex].state != CollisionMapperCellState::FREE ||
				DistanceToMappedPortal(static_cast<int>(portalSideIndex)) ==
					std::numeric_limits<float>::max())
				continue;
			const float dx = cells[reachableIndex].feet.x -
				cells[portalSideIndex].feet.x;
			const float dy = cells[reachableIndex].feet.y -
				cells[portalSideIndex].feet.y;
			const float distance = std::sqrt(dx * dx + dy * dy);
			if (distance < nearestCrossDistance)
			{
				nearestCrossDistance = distance;
				nearestCrossReachable = static_cast<int>(reachableIndex);
				nearestCrossPortalSide = static_cast<int>(portalSideIndex);
			}
		}
	}
	if (nearestCrossReachable >= 0 && nearestCrossPortalSide >= 0)
	{
		const CollisionMapperCell &reachableCell =
			cells[nearestCrossReachable];
		const CollisionMapperCell &portalSideCell =
			cells[nearestCrossPortalSide];
		trace_t crossBuildingTrace;
		const bool crossBuildingProbe = ClipCollisionMapperHullToEntity(
			reachableCell.feet, portalSideCell.feet, hull.mins, hull.maxs,
			barracks, &crossBuildingTrace);
		Msg("[RCBOT2] NavMesh collision-mapper component_gap "
			"reachable_grid=(%d %d) reachable_feet=(%.1f %.1f %.1f) "
			"portal_side_grid=(%d %d) portal_side_feet=(%.1f %.1f %.1f) "
			"xy_distance=%.1f dz=%.1f portal_distance=%.1f "
			"entity94_fraction=%.6f entity94_startsolid=%d "
			"entity94_allsolid=%d entity94_hit=%d\n", reachableCell.x,
			reachableCell.y, reachableCell.feet.x, reachableCell.feet.y,
			reachableCell.feet.z, portalSideCell.x, portalSideCell.y,
			portalSideCell.feet.x, portalSideCell.feet.y, portalSideCell.feet.z,
			nearestCrossDistance, portalSideCell.feet.z - reachableCell.feet.z,
			DistanceToMappedPortal(nearestCrossPortalSide),
			crossBuildingProbe ? crossBuildingTrace.fraction : 0.0f,
			crossBuildingProbe && crossBuildingTrace.startsolid ? 1 : 0,
			crossBuildingProbe && crossBuildingTrace.allsolid ? 1 : 0,
			crossBuildingProbe && crossBuildingTrace.DidHit() ? 1 : 0);
	}

	if (frontierWallEdge != nullptr)
	{
		const Vector frontierMid = (cells[frontierWallEdge->from].feet +
			cells[frontierWallEdge->to].feet) * 0.5f;
		const float wallSearchRadius = 512.0f;
		std::vector<int> nearbyWallEdges;
		for (std::size_t index = 0; index < edges.size(); ++index)
		{
			const CollisionMapperEdge &edge = edges[index];
			if (edge.state != CollisionMapperEdgeState::BLOCKED_BY_BUILDING)
				continue;
			const Vector midpoint = (cells[edge.from].feet +
				cells[edge.to].feet) * 0.5f;
			if ((midpoint - frontierMid).Length2D() <= wallSearchRadius)
				nearbyWallEdges.push_back(static_cast<int>(index));
		}
		const int frontierWallEdgeIndex = static_cast<int>(frontierWallEdge -
			edges.data());
		if (nearbyWallEdges.empty())
			nearbyWallEdges.push_back(frontierWallEdgeIndex);

		// Keep only the blocked-edge cluster connected to the frontier edge.
		// A radius-only collection can include several independent walls in the
		// same building, which would produce false "endpoints" at unrelated
		// parts of the model.
		std::vector<int> wallClusterEdges;
		std::vector<char> wallEdgeVisited(edges.size(), 0);
		std::queue<int> wallEdgePending;
		wallEdgeVisited[frontierWallEdgeIndex] = 1;
		wallEdgePending.push(frontierWallEdgeIndex);
		while (!wallEdgePending.empty())
		{
			const int current = wallEdgePending.front();
			wallEdgePending.pop();
			wallClusterEdges.push_back(current);
			const Vector currentMid = (cells[edges[current].from].feet +
				cells[edges[current].to].feet) * 0.5f;
			for (const int candidate : nearbyWallEdges)
			{
				if (wallEdgeVisited[candidate])
					continue;
				const Vector candidateMid = (cells[edges[candidate].from].feet +
					cells[edges[candidate].to].feet) * 0.5f;
				if ((candidateMid - currentMid).Length2D() >
					kResolution * 1.6f)
					continue;
				wallEdgeVisited[candidate] = 1;
				wallEdgePending.push(candidate);
			}
		}

		int endA = wallClusterEdges.front();
		int endB = wallClusterEdges.front();
		float farthestEndpointDistance = 0.0f;
		for (const int first : wallClusterEdges)
		{
			const Vector firstMid = (cells[edges[first].from].feet +
				cells[edges[first].to].feet) * 0.5f;
			for (const int second : wallClusterEdges)
			{
				const Vector secondMid = (cells[edges[second].from].feet +
					cells[edges[second].to].feet) * 0.5f;
				const float distance = (secondMid - firstMid).Length2D();
				if (distance > farthestEndpointDistance)
				{
					farthestEndpointDistance = distance;
					endA = first;
					endB = second;
				}
			}
		}
		Msg("[RCBOT2] NavMesh collision-mapper wall_cluster frontier_grid=(%d %d) "
			"nearby_blocked_edges=%d connected_blocked_edges=%d "
			"search_radius=%.1f endpoint_span=%.1f\n",
			cells[frontierWallEdge->from].x, cells[frontierWallEdge->from].y,
			static_cast<int>(nearbyWallEdges.size()),
			static_cast<int>(wallClusterEdges.size()), wallSearchRadius,
			farthestEndpointDistance);

		const int wallEnds[] = { endA, endB };
		for (int endpointIndex = 0; endpointIndex < 2; ++endpointIndex)
		{
			const CollisionMapperEdge &endpointEdge = edges[wallEnds[endpointIndex]];
			const Vector endpoint = (cells[endpointEdge.from].feet +
				cells[endpointEdge.to].feet) * 0.5f;
			int centerCell = -1;
			float centerDistance = std::numeric_limits<float>::max();
			for (std::size_t index = 0; index < cells.size(); ++index)
			{
				const float distance = (cells[index].feet - endpoint).Length2D();
				if (distance < centerDistance)
				{
					centerDistance = distance;
					centerCell = static_cast<int>(index);
				}
			}
			if (centerCell < 0)
				continue;
			const int centerX = cells[centerCell].x;
			const int centerY = cells[centerCell].y;
			constexpr int kWallEndRadiusCells = 12;
			std::vector<char> localMask(cells.size(), 0);
			int localFree = 0;
			int localBlocked = 0;
			int localNoGround = 0;
			int localInvalid = 0;
			std::vector<int> localCells;
			for (int y = std::max(0, centerY - kWallEndRadiusCells);
				y <= std::min(height - 1, centerY + kWallEndRadiusCells); ++y)
			{
				for (int x = std::max(0, centerX - kWallEndRadiusCells);
					x <= std::min(width - 1, centerX + kWallEndRadiusCells); ++x)
				{
					const int index = y * width + x;
					localMask[index] = 1;
					localCells.push_back(index);
					switch (cells[index].state)
					{
					case CollisionMapperCellState::FREE:
						++localFree;
						break;
					case CollisionMapperCellState::BLOCKED_BY_BUILDING:
						++localBlocked;
						break;
					case CollisionMapperCellState::NO_GROUND:
						++localNoGround;
						break;
					case CollisionMapperCellState::INVALID:
						++localInvalid;
						break;
					}
					Msg("[RCBOT2] NavMesh collision-mapper wall_end_cell "
						"endpoint=%d grid=(%d %d) feet=(%.1f %.1f %.1f) "
						"state=%s ground_z=%.1f ground_source=%s\n", endpointIndex,
						cells[index].x, cells[index].y, cells[index].feet.x,
						cells[index].feet.y, cells[index].feet.z,
						CollisionMapperCellStateName(cells[index].state),
						cells[index].feet.z, cells[index].groundSource);
				}
			}
			int localEdgeCount = 0;
			int localClearEdgeCount = 0;
			for (const CollisionMapperEdge &edge : edges)
			{
				if (!localMask[edge.from] || !localMask[edge.to])
					continue;
				++localEdgeCount;
				if (edge.clear)
					++localClearEdgeCount;
				Msg("[RCBOT2] NavMesh collision-mapper wall_end_edge "
					"endpoint=%d from_grid=(%d %d) to_grid=(%d %d) "
					"state=%s traversable=%d parent_fraction=%.6f "
					"world_fraction=%.6f from_z=%.1f to_z=%.1f\n", endpointIndex,
					cells[edge.from].x, cells[edge.from].y, cells[edge.to].x,
					cells[edge.to].y, CollisionMapperEdgeStateName(edge.state),
					edge.clear ? 1 : 0, edge.fraction, edge.worldFraction,
					edge.fromGroundZ, edge.toGroundZ);
			}
			Msg("[RCBOT2] NavMesh collision-mapper wall_end "
				"endpoint=%d edge_from_grid=(%d %d) edge_to_grid=(%d %d) "
				"position=(%.1f %.1f %.1f) center_grid=(%d %d) "
				"free=%d blocked=%d no_ground=%d invalid=%d edges=%d "
				"clear_edges=%d\n", endpointIndex, cells[endpointEdge.from].x,
				cells[endpointEdge.from].y, cells[endpointEdge.to].x,
				cells[endpointEdge.to].y, endpoint.x, endpoint.y, endpoint.z,
				centerX, centerY, localFree, localBlocked, localNoGround,
				localInvalid, localEdgeCount, localClearEdgeCount);

			std::vector<int> localPredecessor(cells.size(), -1);
			std::queue<int> localPending;
			for (const int index : localCells)
			{
				if (cells[index].state == CollisionMapperCellState::FREE &&
					predecessor[index] != -1)
				{
					localPredecessor[index] = index;
					localPending.push(index);
				}
			}
			int localTarget = -1;
			while (!localPending.empty())
			{
				const int current = localPending.front();
				localPending.pop();
				if (predecessor[current] == -1 &&
					DistanceToMappedPortal(current) !=
						std::numeric_limits<float>::max())
				{
					localTarget = current;
					break;
				}
				for (const int neighbor : adjacency8[current])
				{
					if (!localMask[neighbor] || localPredecessor[neighbor] != -1)
						continue;
					localPredecessor[neighbor] = current;
					localPending.push(neighbor);
				}
			}
			std::vector<int> localBypass;
			if (localTarget >= 0)
			{
				for (int current = localTarget; ; current = localPredecessor[current])
				{
					localBypass.push_back(current);
					if (localPredecessor[current] == current)
						break;
				}
				std::reverse(localBypass.begin(), localBypass.end());
			}
			Msg("[RCBOT2] NavMesh collision-mapper wall_end_bypass "
				"endpoint=%d exists=%d path_cells=%d\n", endpointIndex,
				localBypass.empty() ? 0 : 1,
				static_cast<int>(localBypass.size()));
			if (!localBypass.empty())
				PrintCollisionMapperPath(localBypass, cells, "wall-end-bypass");
			if (debugoverlay != nullptr)
				debugoverlay->AddBoxOverlay(endpoint,
					Vector(-10.0f, -10.0f, -2.0f), Vector(10.0f, 10.0f, 2.0f),
					QAngle(0.0f, 0.0f, 0.0f), 255, 0, 0, 220, 15.0f);
		}
	}

	if (frontierWallEdge != nullptr)
	{
		RunCollisionMapperAdaptiveBoundary(*frontierWallEdge, cells, edges,
			predecessor, portalSamples, width, height, hull.mins, hull.maxs,
			barracks, mapperSeed, portal);
	}
	else
	{
		Msg("[RCBOT2] NavMesh collision-mapper adaptive_skipped "
			"reason=no-entity94-frontier\n");
	}

	const char *noPathReason = "NONE";
	if (portalPath.empty())
	{
		if (validPortalSampleCount == 0)
			noPathReason = "PORTAL_SAMPLING";
		else if (mappedPortalSampleCount == 0)
			noPathReason = "GRID_RESOLUTION";
		else if (frontierWallEdge != nullptr)
			noPathReason = "ENTITY_94_WALL_EDGE";
		else if (frontierNonFreeNeighbor >= 0 &&
			cells[frontierNonFreeNeighbor].state ==
				CollisionMapperCellState::BLOCKED_BY_BUILDING)
			noPathReason = "ENTITY_94_WALL_CELL";
		else if (frontierNonFreeNeighbor >= 0 &&
			cells[frontierNonFreeNeighbor].state ==
				CollisionMapperCellState::NO_GROUND)
			noPathReason = "NO_GROUND";
		else if (frontierNoGroundEdge != nullptr)
			noPathReason = "NO_GROUND_EDGE";
		else if (frontierWorldEdge != nullptr)
			noPathReason = "WORLD_WALL_EDGE";
		else
			noPathReason = "GRID_CONNECTIVITY_OR_RESOLUTION";
	}
	if (portalPath.empty() && frontierWallEdge != nullptr)
	{
		Msg("[RCBOT2] NavMesh collision-mapper component_end_wall "
			"reason=ENTITY_94_WALL_EDGE from_grid=(%d %d) to_grid=(%d %d) "
			"from_reachable=%d to_reachable=%d fraction=%.6f\n",
			cells[frontierWallEdge->from].x, cells[frontierWallEdge->from].y,
			cells[frontierWallEdge->to].x, cells[frontierWallEdge->to].y,
			predecessor[frontierWallEdge->from] != -1 ? 1 : 0,
			predecessor[frontierWallEdge->to] != -1 ? 1 : 0,
			frontierWallEdge->fraction);
	}
	else if (portalPath.empty() && frontierNoGroundEdge != nullptr)
	{
		Msg("[RCBOT2] NavMesh collision-mapper component_end_edge "
			"reason=NO_GROUND_EDGE from_grid=(%d %d) to_grid=(%d %d) "
			"from_reachable=%d to_reachable=%d from_z=%.1f to_z=%.1f\n",
			cells[frontierNoGroundEdge->from].x,
			cells[frontierNoGroundEdge->from].y,
			cells[frontierNoGroundEdge->to].x,
			cells[frontierNoGroundEdge->to].y,
			predecessor[frontierNoGroundEdge->from] != -1 ? 1 : 0,
			predecessor[frontierNoGroundEdge->to] != -1 ? 1 : 0,
			frontierNoGroundEdge->fromGroundZ,
			frontierNoGroundEdge->toGroundZ);
	}
	else if (portalPath.empty() && frontierWorldEdge != nullptr)
	{
		Msg("[RCBOT2] NavMesh collision-mapper component_end_edge "
			"reason=WORLD_WALL_EDGE from_grid=(%d %d) to_grid=(%d %d) "
			"from_reachable=%d to_reachable=%d from_z=%.1f to_z=%.1f\n",
			cells[frontierWorldEdge->from].x,
			cells[frontierWorldEdge->from].y,
			cells[frontierWorldEdge->to].x,
			cells[frontierWorldEdge->to].y,
			predecessor[frontierWorldEdge->from] != -1 ? 1 : 0,
			predecessor[frontierWorldEdge->to] != -1 ? 1 : 0,
			frontierWorldEdge->fromGroundZ, frontierWorldEdge->toGroundZ);
	}
	else if (portalPath.empty() && frontierNonFreeNeighbor >= 0)
	{
		Msg("[RCBOT2] NavMesh collision-mapper component_end_nonfree "
			"reason=%s from_grid=(%d %d) from_feet=(%.1f %.1f %.1f) "
			"to_grid=(%d %d) to_state=%s to_sample=(%.1f %.1f %.1f)\n",
			CollisionMapperCellStateName(cells[frontierNonFreeNeighbor].state),
			cells[frontierNonFreeCell].x, cells[frontierNonFreeCell].y,
			cells[frontierNonFreeCell].feet.x, cells[frontierNonFreeCell].feet.y,
			cells[frontierNonFreeCell].feet.z,
			cells[frontierNonFreeNeighbor].x,
			cells[frontierNonFreeNeighbor].y,
			CollisionMapperCellStateName(cells[frontierNonFreeNeighbor].state),
			cells[frontierNonFreeNeighbor].sample.x,
			cells[frontierNonFreeNeighbor].sample.y,
			cells[frontierNonFreeNeighbor].sample.z);
	}
	if (portalPath.empty())
		Msg("[RCBOT2] NavMesh collision-mapper no_path reason=%s "
			"valid_portal_samples=%d mapped_portal_samples=%d\n", noPathReason,
			validPortalSampleCount, mappedPortalSampleCount);
	if (portalPath.empty() && nearestReachableCell >= 0)
	{
		const CollisionMapperCell &cell = cells[nearestReachableCell];
		Msg("[RCBOT2] NavMesh collision-mapper component_end "
			"grid=(%d %d) feet=(%.1f %.1f %.1f) "
			"nearest_valid_portal_distance=%.1f\n", cell.x, cell.y,
			cell.feet.x, cell.feet.y, cell.feet.z,
			nearestReachablePortalDistance);
	}

	if (debugoverlay != nullptr)
	{
		debugoverlay->AddBoxOverlay(hull.feet, Vector(-8.0f, -8.0f, 0.0f),
			Vector(8.0f, 8.0f, 8.0f), QAngle(0.0f, 0.0f, 0.0f), 0, 0, 255,
			220, 15.0f);
		debugoverlay->AddBoxOverlay(portal, Vector(-8.0f, -8.0f, -8.0f),
			Vector(8.0f, 8.0f, 8.0f), QAngle(0.0f, 0.0f, 0.0f), 255, 255,
			255, 220, 15.0f);
		for (std::size_t i = 1; i < portalPath.size(); ++i)
			debugoverlay->AddLineOverlay(cells[portalPath[i - 1]].feet,
				cells[portalPath[i]].feet, 255, 255, 0, true, 15.0f);
	}

	Msg("[RCBOT2] NavMesh collision-mapper summary free=%d "
		"blocked_by_building=%d no_ground=%d invalid=%d physbox_solid=%d "
		"clear_edges=%d blocked_edges=%d interior_free=%d "
		"reachable_free=%d reachable_interior=%d spawn_cell=(%d %d) "
		"portal_samples=%d portal_samples_mapped=%d "
		"spawn_to_portal=%s no_path_reason=%s\n", freeCount, blockedCount,
		noGroundCount,
		invalidCount, physboxSolidCount, clearEdgeCount, blockedEdgeCount,
		interiorFree, reachableFree, reachableInterior,
		spawnCell >= 0 ? cells[spawnCell].x : -1,
		spawnCell >= 0 ? cells[spawnCell].y : -1,
		validPortalSampleCount, mappedPortalSampleCount,
		portalPath.empty() ? "NO" : "YES", noPathReason);
	Msg("[RCBOT2] NavMesh collision-mapper end\n");
}

void PrintEntityDetails(const char *prefix, edict_t *entity,
	const Vector *hitPosition, const trace_t *trace, const bool areaOverlap,
	const bool corridorOverlap)
{
	if (entity == nullptr)
	{
		Msg("[RCBOT2] NavMesh entity-sweep %s entity_index=-1 classname=<none> "
			"model=<none> area2_overlap=%d corridor_overlap=%d\n",
			prefix, areaOverlap ? 1 : 0, corridorOverlap ? 1 : 0);
		return;
	}

	const ICollideable *collideable = entity->GetCollideable();
	Vector worldMins = vec3_origin;
	Vector worldMaxs = vec3_origin;
	const bool hasBounds = GetEntityWorldBounds(entity, &worldMins, &worldMaxs);
	const Vector origin = collideable != nullptr ?
		collideable->GetCollisionOrigin() : vec3_origin;
	const int entityIndex = engine != nullptr ? engine->IndexOfEdict(entity) : -1;
	const int solid = collideable != nullptr ?
		static_cast<int>(collideable->GetSolid()) : -1;
	const int solidFlags = collideable != nullptr ?
		collideable->GetSolidFlags() : -1;
	const int collisionGroup = collideable != nullptr ?
		collideable->GetCollisionGroup() : -1;
	const int hit = hitPosition != nullptr ? 1 : 0;
	const float fraction = trace != nullptr ? trace->fraction : 1.0f;
	const int startSolid = trace != nullptr && trace->startsolid ? 1 : 0;
	const int allSolid = trace != nullptr && trace->allsolid ? 1 : 0;
	const Vector position = hitPosition != nullptr ? *hitPosition : vec3_origin;

	Msg("[RCBOT2] NavMesh entity-sweep %s entity_index=%d classname=%s "
		"model=%s origin=(%.1f %.1f %.1f) "
		"world_mins=(%.1f %.1f %.1f) world_maxs=(%.1f %.1f %.1f) "
		"solid_type=%d solid_flags=0x%x collision_group=%d "
		"hit=%d fraction=%.6f startsolid=%d allsolid=%d "
		"hit_position=(%.1f %.1f %.1f) bounds_valid=%d "
		"area2_overlap=%d corridor_overlap=%d\n",
		prefix, entityIndex, SafeClassName(entity), SafeModelName(entity),
		origin.x, origin.y, origin.z,
		worldMins.x, worldMins.y, worldMins.z,
		worldMaxs.x, worldMaxs.y, worldMaxs.z,
		solid, solidFlags, collisionGroup, hit, fraction, startSolid, allSolid,
		position.x, position.y, position.z, hasBounds ? 1 : 0,
		areaOverlap ? 1 : 0, corridorOverlap ? 1 : 0);
}

void PrintPortal(const int currentAreaId, const int nextAreaId,
	CNavArea *currentArea, CNavArea *nextArea)
{
	if (currentArea == nullptr || nextArea == nullptr)
	{
		Msg("[RCBOT2] NavMesh entity-sweep portal current_area=%d next_area=%d unavailable\n",
			currentAreaId, nextAreaId);
		return;
	}

	Vector center = vec3_origin;
	float halfWidth = 0.0f;
	const NavDirType direction = currentArea->ComputeLargestPortal(
		nextArea, &center, &halfWidth);
	Msg("[RCBOT2] NavMesh entity-sweep portal current_area=%d next_area=%d "
		"direction=%d center=(%.1f %.1f %.1f) half_width=%.1f\n",
		currentAreaId, nextAreaId, static_cast<int>(direction), center.x,
		center.y, center.z, halfWidth);
}

class CTraceFilterSkipList final : public CTraceFilterSimple
{
public:
	explicit CTraceFilterSkipList(const int collisionGroup) :
		CTraceFilterSimple(nullptr, collisionGroup) {}

	void Skip(const IHandleEntity *entity)
	{
		if (entity != nullptr)
			m_skipped.push_back(entity);
	}

	bool ShouldHitEntity(IHandleEntity *entity, int contentsMask) override
	{
		for (const IHandleEntity *skipped : m_skipped)
		{
			if (skipped == entity)
				return false;
		}
		return CTraceFilterSimple::ShouldHitEntity(entity, contentsMask);
	}

private:
	std::vector<const IHandleEntity *> m_skipped;
};

void PrintAreaBounds(const char *label, int areaId, CNavArea *area);
void PrintEntityCandidates(const Vector &sweepMin, const Vector &sweepMax,
	const Vector &areaMin, const Vector &areaMax, int botIndex);

void PrintAreaBounds(const char *label, const int areaId, CNavArea *area)
{
	if (area == nullptr)
	{
		Msg("[RCBOT2] NavMesh entity-sweep %s area_id=%d unavailable\n",
			label, areaId);
		return;
	}

	Extent extent;
	area->GetExtent(&extent);
	const Vector nw = area->GetCorner(NORTH_WEST);
	const Vector ne = area->GetCorner(NORTH_EAST);
	const Vector se = area->GetCorner(SOUTH_EAST);
	const Vector sw = area->GetCorner(SOUTH_WEST);
	Msg("[RCBOT2] NavMesh entity-sweep %s area_id=%d center=(%.1f %.1f %.1f) "
		"nw=(%.1f %.1f %.1f) ne=(%.1f %.1f %.1f) "
		"se=(%.1f %.1f %.1f) sw=(%.1f %.1f %.1f) "
		"extent_min=(%.1f %.1f %.1f) extent_max=(%.1f %.1f %.1f)\n",
		label, area->GetID(), area->GetCenter().x, area->GetCenter().y,
		area->GetCenter().z, nw.x, nw.y, nw.z, ne.x, ne.y, ne.z, se.x,
		se.y, se.z, sw.x, sw.y, sw.z, extent.lo.x, extent.lo.y, extent.lo.z,
		extent.hi.x, extent.hi.y, extent.hi.z);
}

void PrintEntityCandidates(const Vector &sweepMin, const Vector &sweepMax,
	const Vector &areaMin, const Vector &areaMax, const int botIndex)
{
	if (engine == nullptr || gpGlobals == nullptr)
		return;

	int candidateCount = 0;
	for (int index = gpGlobals->maxClients + 1;
		index < gpGlobals->maxEntities; ++index)
	{
		edict_t *entity = engine->PEntityOfEntIndex(index);
		if (entity == nullptr || entity->IsFree() || index == botIndex ||
			entity->GetCollideable() == nullptr)
			continue;

		const int solid = static_cast<int>(entity->GetCollideable()->GetSolid());
		if (solid == SOLID_NONE)
			continue;

		Vector entityMin;
		Vector entityMax;
		if (!GetEntityWorldBounds(entity, &entityMin, &entityMax))
			continue;
		const bool areaOverlap = BoxOverlaps(entityMin, entityMax,
			areaMin, areaMax);
		const bool corridorOverlap = BoxOverlaps(entityMin, entityMax,
			sweepMin, sweepMax);
		const char *className = SafeClassName(entity);
		const bool isEmpiresBuilding = std::strncmp(className, "emp_building_", 13) == 0;
		if (!areaOverlap && !corridorOverlap && !isEmpiresBuilding)
			continue;

		PrintEntityDetails("candidate", entity, nullptr, nullptr,
			areaOverlap, corridorOverlap);
		++candidateCount;
	}

	Msg("[RCBOT2] NavMesh entity-sweep candidate_count=%d\n", candidateCount);
}

struct BuildingCandidate
{
	edict_t *entity = nullptr;
	int entityIndex = -1;
	int serial = 0;
	std::string classname;
	Vector mins = vec3_origin;
	Vector maxs = vec3_origin;
	int team = -1;
	bool vehiclePhysbox = false;
};

bool IsEmpiresBuilding(const edict_t *entity)
{
	return std::strncmp(SafeClassName(entity), "emp_building_", 13) == 0;
}

bool IsVehiclePhysbox(const edict_t *entity)
{
	const char *className = SafeClassName(entity);
	return std::strstr(className, "_vehicle_physbox") != nullptr;
}

bool IsSolidEmpiresBuilding(edict_t *entity)
{
	return entity != nullptr && entity->GetCollideable() != nullptr &&
		entity->GetCollideable()->GetSolid() != SOLID_NONE &&
		IsEmpiresBuilding(entity);
}

bool BuildingIdentityOverlaps(const BuildingCandidate &a,
	const BuildingCandidate &b)
{
	if (a.vehiclePhysbox == b.vehiclePhysbox)
		return false;

	const BuildingCandidate &proxy = a.vehiclePhysbox ? a : b;
	const BuildingCandidate &parent = a.vehiclePhysbox ? b : a;
	return BoxOverlaps(proxy.mins, proxy.maxs, parent.mins, parent.maxs);
}

void FillBuildingCandidate(edict_t *entity, BuildingCandidate *candidate)
{
	if (candidate == nullptr)
		return;

	candidate->entity = entity;
	candidate->entityIndex = engine != nullptr ? engine->IndexOfEdict(entity) : -1;
	candidate->serial = entity != nullptr ? entity->m_NetworkSerialNumber : 0;
	candidate->classname = SafeClassName(entity);
	candidate->vehiclePhysbox = IsVehiclePhysbox(entity);
	candidate->team = -1;
	GetEntityWorldBounds(entity, &candidate->mins, &candidate->maxs);
}

std::vector<BuildingCandidate> ScanEmpiresBuildings()
{
	std::vector<BuildingCandidate> scanned;
	if (engine == nullptr || gpGlobals == nullptr)
		return scanned;

	for (int index = gpGlobals->maxClients + 1;
		index < gpGlobals->maxEntities; ++index)
	{
		edict_t *entity = engine->PEntityOfEntIndex(index);
		if (entity == nullptr || entity->IsFree() ||
			!IsSolidEmpiresBuilding(entity))
			continue;

		BuildingCandidate candidate;
		FillBuildingCandidate(entity, &candidate);
		if (candidate.maxs.x < candidate.mins.x ||
			candidate.maxs.y < candidate.mins.y ||
			candidate.maxs.z < candidate.mins.z)
			continue;
		scanned.push_back(candidate);
	}
	return scanned;
}

std::vector<BuildingCandidate> DeduplicateBuildingPhysboxes(
	const std::vector<BuildingCandidate> &scanned, int *suppressedCount)
{
	std::vector<BuildingCandidate> logical;
	int suppressed = 0;
	for (const BuildingCandidate &candidate : scanned)
	{
		if (!candidate.vehiclePhysbox)
		{
			logical.push_back(candidate);
			continue;
		}

		bool hasParent = false;
		for (const BuildingCandidate &other : scanned)
		{
			if (BuildingIdentityOverlaps(candidate, other))
			{
				hasParent = true;
				break;
			}
		}
		if (hasParent)
		{
			++suppressed;
			continue;
		}

		// If a server build exposes only the physical proxy, retain it as a
		// logical obstacle instead of silently losing the collision geometry.
		logical.push_back(candidate);
	}
	if (suppressedCount != nullptr)
		*suppressedCount = suppressed;
	return logical;
}

bool SameBuildingHandle(const BuildingCandidate &a,
	const BuildingCandidate &b)
{
	return a.entityIndex == b.entityIndex && a.serial == b.serial;
}

void PrintBuildingRegistryEvent(const char *event,
	const BuildingCandidate &candidate, const bool physboxDeduplicated)
{
	Msg("[RCBOT2] NavMesh building-registry event=%s entity_index=%d serial=%d "
		"classname=%s team=%d proxy_vehicle_physbox=%d physbox_deduplicated=%d "
		"world_mins=(%.1f %.1f %.1f) world_maxs=(%.1f %.1f %.1f)\n",
		event, candidate.entityIndex, candidate.serial,
		candidate.classname.c_str(), candidate.team,
		candidate.vehiclePhysbox ? 1 : 0, physboxDeduplicated ? 1 : 0,
		candidate.mins.x, candidate.mins.y, candidate.mins.z,
		candidate.maxs.x, candidate.maxs.y, candidate.maxs.z);
}

class DynamicBuildingRegistry final
{
public:
	void Reconcile()
	{
		int suppressedCount = 0;
		const std::vector<BuildingCandidate> scanned = ScanEmpiresBuildings();
		const std::vector<BuildingCandidate> logical =
			DeduplicateBuildingPhysboxes(scanned, &suppressedCount);

		for (const BuildingCandidate &old : m_active)
		{
			bool stillPresent = false;
			for (const BuildingCandidate &current : logical)
			{
				if (SameBuildingHandle(old, current))
				{
					stillPresent = true;
					break;
				}
			}
			if (!stillPresent)
				PrintBuildingRegistryEvent("remove", old, false);
		}

		for (const BuildingCandidate &current : logical)
		{
			bool wasPresent = false;
			bool proxyDeduplicated = false;
			for (const BuildingCandidate &old : m_active)
			{
				if (SameBuildingHandle(old, current))
				{
					wasPresent = true;
					break;
				}
			}
			if (!wasPresent)
			{
				for (const BuildingCandidate &proxy : scanned)
				{
					if (proxy.vehiclePhysbox &&
						BuildingIdentityOverlaps(proxy, current))
					{
						proxyDeduplicated = true;
						break;
					}
				}
				PrintBuildingRegistryEvent("spawn", current, proxyDeduplicated);
			}
		}

		m_active = logical;
		m_suppressedPhysboxes = suppressedCount;
	}

	void Reset()
	{
		m_active.clear();
		m_suppressedPhysboxes = 0;
	}

	const std::vector<BuildingCandidate> &Active() const
	{
		return m_active;
	}

	int SuppressedPhysboxes() const
	{
		return m_suppressedPhysboxes;
	}

private:
	std::vector<BuildingCandidate> m_active;
	int m_suppressedPhysboxes = 0;
};

DynamicBuildingRegistry s_buildingRegistry;

bool EntityOverlapsPathAreas(const Vector &mins, const Vector &maxs,
	const RCBotNavMesh::Path &path)
{
	if (TheNavMesh == nullptr)
		return false;

	for (std::size_t i = 0; i < path.areaIds.size(); ++i)
	{
		CNavArea *area = TheNavMesh->GetNavAreaByID(path.areaIds[i]);
		if (area == nullptr)
			continue;

		Extent areaExtent;
		area->GetExtent(&areaExtent);
		if (BoxOverlaps(mins, maxs, areaExtent.lo, areaExtent.hi))
			return true;
	}
	return false;
}

void PrintBuildingCandidate(const char *phase,
	const BuildingCandidate &candidate, const bool selected)
{
	edict_t *entity = candidate.entity;
	const ICollideable *collideable = entity != nullptr ?
		entity->GetCollideable() : nullptr;
	const int entityIndex = entity != nullptr && engine != nullptr ?
		engine->IndexOfEdict(entity) : -1;
	const int solid = collideable != nullptr ?
		static_cast<int>(collideable->GetSolid()) : -1;
	const int collisionGroup = collideable != nullptr ?
		collideable->GetCollisionGroup() : -1;
	const Vector origin = collideable != nullptr ?
		collideable->GetCollisionOrigin() : vec3_origin;

	Msg("[RCBOT2] NavMesh building-obstacle %s entity_index=%d selected=%d "
		"proxy_vehicle_physbox=%d classname=%s model=%s "
		"origin=(%.1f %.1f %.1f) world_mins=(%.1f %.1f %.1f) "
		"world_maxs=(%.1f %.1f %.1f) solid_type=%d collision_group=%d\n",
		phase, entityIndex, selected ? 1 : 0, candidate.vehiclePhysbox ? 1 : 0,
		SafeClassName(entity), SafeModelName(entity), origin.x, origin.y, origin.z,
		candidate.mins.x, candidate.mins.y, candidate.mins.z,
		candidate.maxs.x, candidate.maxs.y, candidate.maxs.z,
		solid, collisionGroup);
}

bool FindEmpiresBuildingForPath(const RCBotNavMesh::Path &path,
	BuildingCandidate *selected, int *discoveredCount)
{
	if (selected == nullptr || discoveredCount == nullptr ||
		engine == nullptr || gpGlobals == nullptr || TheNavMesh == nullptr)
		return false;

	std::vector<BuildingCandidate> candidates;
	for (int index = gpGlobals->maxClients + 1;
		index < gpGlobals->maxEntities; ++index)
	{
		edict_t *entity = engine->PEntityOfEntIndex(index);
		if (entity == nullptr || entity->IsFree() ||
			!IsEmpiresBuilding(entity) || entity->GetCollideable() == nullptr ||
			entity->GetCollideable()->GetSolid() == SOLID_NONE)
			continue;

		BuildingCandidate candidate;
		candidate.entity = entity;
		candidate.vehiclePhysbox = IsVehiclePhysbox(entity);
		if (!GetEntityWorldBounds(entity, &candidate.mins, &candidate.maxs) ||
			!EntityOverlapsPathAreas(candidate.mins, candidate.maxs, path))
			continue;
		candidates.push_back(candidate);
	}

	*discoveredCount = static_cast<int>(candidates.size());
	int selectedIndex = -1;
	for (std::size_t i = 0; i < candidates.size(); ++i)
	{
		if (!candidates[i].vehiclePhysbox)
		{
			selectedIndex = static_cast<int>(i);
			break;
		}
	}
	if (selectedIndex < 0 && !candidates.empty())
		selectedIndex = 0;

	for (std::size_t i = 0; i < candidates.size(); ++i)
		PrintBuildingCandidate("discovered", candidates[i],
			static_cast<int>(i) == selectedIndex);

	if (selectedIndex < 0)
	{
		Msg("[RCBOT2] NavMesh building-obstacle discovered_count=0\n");
		return false;
	}

	*selected = candidates[static_cast<std::size_t>(selectedIndex)];
	Msg("[RCBOT2] NavMesh building-obstacle selected_entity=%d "
		"selected_classname=%s discovered_count=%d "
		"vehicle_physbox_candidates_are_not_registered_separately=1\n",
		engine->IndexOfEdict(selected->entity), SafeClassName(selected->entity),
		*discoveredCount);
	return true;
}

void PrintPathSummary(const char *phase, const RCBotNavMesh::PathResult result,
	const RCBotNavMesh::Path &path)
{
	Msg("[RCBOT2] NavMesh building-obstacle path phase=%s result=%s "
		"start_area=%d goal_area=%d areas=%zu points=%zu length=%.1f sequence=",
		phase, RCBotNavMesh::PathResultName(result), path.startAreaId,
		path.goalAreaId, path.areaIds.size(), path.points.size(), path.length);
	for (std::size_t i = 0; i < path.areaIds.size(); ++i)
		Msg("%s%u", i == 0 ? "" : "->", path.areaIds[i]);
	Msg("\n");
	for (std::size_t i = 0; i < path.points.size(); ++i)
	{
		Msg("[RCBOT2] NavMesh building-obstacle path_%s_point[%zu]=(%.1f %.1f %.1f)\n",
			phase, i, path.points[i].x, path.points[i].y, path.points[i].z);
	}
}

void PrintPathAreaStates(const char *phase, const RCBotNavMesh::Path &path)
{
	if (TheNavMesh == nullptr)
		return;

	for (std::size_t i = 0; i < path.areaIds.size(); ++i)
	{
		CNavArea *area = TheNavMesh->GetNavAreaByID(path.areaIds[i]);
		if (area == nullptr)
			continue;
		Msg("[RCBOT2] NavMesh building-obstacle area_state phase=%s area_id=%u "
			"blocked_team_any=%d blocked_nav_blocker_ignored=%d "
			"avoidance=%d avoidance_height=%.1f\n",
			phase, path.areaIds[i], area->IsBlocked(TEAM_ANY) ? 1 : 0,
			area->IsBlocked(TEAM_ANY, true) ? 1 : 0,
			area->HasAvoidanceObstacle() ? 1 : 0,
			area->GetAvoidanceObstacleHeight());
	}
}

class CDiagnosticAvoidanceObstacle final : public INavAvoidanceObstacle
{
public:
	explicit CDiagnosticAvoidanceObstacle(edict_t *entity) :
		m_entity(entity), m_active(true) {}

	bool IsPotentiallyAbleToObstructNavAreas() const override { return true; }
	float GetNavObstructionHeight() const override { return JumpCrouchHeight; }
	bool CanObstructNavAreas() const override { return m_active; }
	edict_t *GetObstructingEntity() override { return m_entity; }
	void OnNavMeshLoaded() override {}

	void Disable() { m_active = false; }

private:
	edict_t *m_entity;
	bool m_active;
};

void UpdateDiagnosticObstacleAreas(const BuildingCandidate &candidate)
{
	if (candidate.entity == nullptr)
		return;

	int affectedCount = 0;
	for (int i = 0; i < TheNavAreas.Count(); ++i)
	{
		CNavArea *area = TheNavAreas[i];
		if (area == nullptr)
			continue;
		Extent areaExtent;
		area->GetExtent(&areaExtent);
		if (!BoxOverlaps(candidate.mins, candidate.maxs,
			areaExtent.lo, areaExtent.hi))
			continue;
		area->UpdateAvoidanceObstacles();
		Msg("[RCBOT2] NavMesh building-obstacle overlap_area phase=after-registration "
			"area_id=%u blocked_team_any=%d avoidance=%d avoidance_height=%.1f\n",
			area->GetID(), area->IsBlocked(TEAM_ANY) ? 1 : 0,
			area->HasAvoidanceObstacle() ? 1 : 0,
			area->GetAvoidanceObstacleHeight());
		++affectedCount;
	}
	Msg("[RCBOT2] NavMesh building-obstacle updated_overlapping_areas=%d\n",
		affectedCount);
}

constexpr float kPlayerHullRadius = 16.0f;
constexpr float kSafetyMargin = 4.0f;
constexpr float kCorridorSlack = 48.0f;
constexpr float kCornerEpsilon = 4.0f;
constexpr float kPointDedupDistance = 12.0f;

struct InflatedBuilding
{
	const BuildingCandidate *source = nullptr;
	Vector mins = vec3_origin;
	Vector maxs = vec3_origin;
};

struct LocalDetourCandidate
{
	Vector point = vec3_origin;
	const char *kind = "unknown";
	bool terminal = false;
};

struct LocalDetourResult
{
	bool affected = false;
	bool success = false;
	std::vector<Vector> points;
};

float DistanceSquaredToAreaXY(const Vector &point, const CNavArea *area)
{
	if (area == nullptr)
		return std::numeric_limits<float>::max();

	Extent extent;
	area->GetExtent(&extent);
	const float dx = point.x < extent.lo.x ? extent.lo.x - point.x :
		(point.x > extent.hi.x ? point.x - extent.hi.x : 0.0f);
	const float dy = point.y < extent.lo.y ? extent.lo.y - point.y :
		(point.y > extent.hi.y ? point.y - extent.hi.y : 0.0f);
	return dx * dx + dy * dy;
}

bool PointWithinLocalCorridor(const Vector &point,
	const std::vector<CNavArea *> &areas)
{
	const float slackSquared = kCorridorSlack * kCorridorSlack;
	for (CNavArea *area : areas)
	{
		if (DistanceSquaredToAreaXY(point, area) <= slackSquared)
			return true;
	}
	return false;
}

bool GroundPointWithinLocalCorridor(const Vector &point,
	const std::vector<CNavArea *> &areas, Vector *groundPoint)
{
	if (groundPoint == nullptr)
		return false;

	const float slackSquared = kCorridorSlack * kCorridorSlack;
	float bestDistance = std::numeric_limits<float>::max();
	CNavArea *bestArea = nullptr;
	Vector bestSample = vec3_origin;
	for (CNavArea *area : areas)
	{
		if (area == nullptr)
			continue;
		Extent extent;
		area->GetExtent(&extent);
		Vector sample = point;
		sample.x = std::max(extent.lo.x, std::min(extent.hi.x, sample.x));
		sample.y = std::max(extent.lo.y, std::min(extent.hi.y, sample.y));
		const float distance = (sample - point).Length2D();
		if (distance * distance > slackSquared || distance * distance >= bestDistance)
			continue;
		bestDistance = distance * distance;
		bestArea = area;
		bestSample = sample;
	}
	if (bestArea == nullptr)
		return false;

	*groundPoint = point;
	groundPoint->z = bestArea->GetZ(bestSample.x, bestSample.y);
	return std::isfinite(groundPoint->z);
}

bool PointInsideInflatedBuildingXY(const Vector &point,
	const InflatedBuilding &building)
{
	return point.x >= building.mins.x && point.x <= building.maxs.x &&
		point.y >= building.mins.y && point.y <= building.maxs.y;
}

bool SegmentIntersectsRectangleXY(const Vector &from, const Vector &to,
	const Vector &mins, const Vector &maxs, float *entry, float *exit)
{
	float tMin = 0.0f;
	float tMax = 1.0f;
	const float delta[2] = { to.x - from.x, to.y - from.y };
	const float origin[2] = { from.x, from.y };
	const float rectMin[2] = { mins.x, mins.y };
	const float rectMax[2] = { maxs.x, maxs.y };
	for (int axis = 0; axis < 2; ++axis)
	{
		if (std::fabs(delta[axis]) < 0.001f)
		{
			if (origin[axis] < rectMin[axis] || origin[axis] > rectMax[axis])
				return false;
			continue;
		}

		float nearValue = (rectMin[axis] - origin[axis]) / delta[axis];
		float farValue = (rectMax[axis] - origin[axis]) / delta[axis];
		if (nearValue > farValue)
			std::swap(nearValue, farValue);
		tMin = std::max(tMin, nearValue);
		tMax = std::min(tMax, farValue);
		if (tMin > tMax)
			return false;
	}

	if (entry != nullptr)
		*entry = tMin;
	if (exit != nullptr)
		*exit = tMax;
	return true;
}

bool SegmentClearsInflatedBuildings(const Vector &from, const Vector &to,
	const std::vector<InflatedBuilding> &buildings)
{
	for (const InflatedBuilding &building : buildings)
	{
		if (building.source == nullptr)
			continue;

		// This is intentionally a pure 2D test.  The engine hull trace is a
		// secondary diagnostic only; dynamic Empires buildings have already
		// shown that it is not a reliable source for this obstacle decision.
		if (SegmentIntersectsRectangleXY(from, to, building.mins,
			building.maxs, nullptr, nullptr))
			return false;
	}
	return true;
}

bool LogSegmentBuildingIntersections(const char *edgeKind, const int fromIndex,
	const LocalDetourCandidate &from, const int toIndex,
	const LocalDetourCandidate &to,
	const std::vector<InflatedBuilding> &buildings)
{
	bool intersectsAny = false;
	for (std::size_t buildingIndex = 0; buildingIndex < buildings.size();
		++buildingIndex)
	{
		const InflatedBuilding &building = buildings[buildingIndex];
		float entry = 0.0f;
		float exit = 0.0f;
		const bool intersects = SegmentIntersectsRectangleXY(from.point,
			to.point, building.mins, building.maxs, &entry, &exit);
		if (intersects)
			intersectsAny = true;
		Msg("[RCBOT2] NavMesh building-detour %s edge from=%d(%s) "
			"to=%d(%s) start=(%.1f %.1f %.1f) end=(%.1f %.1f %.1f) "
			"intersects_inflated_building=%s building_index=%zu entity_index=%d "
			"classname=%s entry=%.4f exit=%.4f\n",
			edgeKind, fromIndex, from.kind, toIndex, to.kind,
			from.point.x, from.point.y, from.point.z, to.point.x, to.point.y,
			to.point.z, intersects ? "yes" : "no", buildingIndex,
			building.source == nullptr ? -1 : building.source->entityIndex,
			building.source == nullptr ? "unknown" :
				building.source->classname.c_str(), entry, exit);
	}
	if (buildings.empty())
	{
		Msg("[RCBOT2] NavMesh building-detour %s edge from=%d(%s) "
			"to=%d(%s) start=(%.1f %.1f %.1f) end=(%.1f %.1f %.1f) "
			"intersects_inflated_building=no building_count=0\n",
			edgeKind, fromIndex, from.kind, toIndex, to.kind,
			from.point.x, from.point.y, from.point.z, to.point.x, to.point.y,
			to.point.z);
	}
	return intersectsAny;
}

bool SegmentWithinLocalCorridor(const Vector &from, const Vector &to,
	const std::vector<CNavArea *> &areas)
{
	for (int sampleIndex = 0; sampleIndex <= 16; ++sampleIndex)
	{
		const float fraction = static_cast<float>(sampleIndex) / 16.0f;
		const Vector sample = from + (to - from) * fraction;
		if (!PointWithinLocalCorridor(sample, areas))
			return false;
	}
	return true;
}

bool HullTraceIsClear(const Vector &from, const Vector &to, trace_t *traceOut)
{
	trace_t trace;
	UTIL_TraceHull(from, to, Vector(-16.0f, -16.0f, 0.0f),
		Vector(16.0f, 16.0f, 72.0f), MASK_PLAYERSOLID, nullptr,
		COLLISION_GROUP_PLAYER_MOVEMENT, &trace);
	if (traceOut != nullptr)
		*traceOut = trace;
	return !trace.startsolid && !trace.allsolid && trace.fraction >= 0.999f;
}

bool PointBlockedByInflatedBuildings(const Vector &point,
	const std::vector<InflatedBuilding> &buildings)
{
	for (const InflatedBuilding &building : buildings)
	{
		if (building.source == nullptr)
			continue;
		if (point.z + 72.0f < building.mins.z ||
			point.z > building.maxs.z)
			continue;
		if (PointInsideInflatedBuildingXY(point, building))
			return true;
	}
	return false;
}

void AddLocalDetourCandidate(const Vector &rawPoint, const char *kind,
	const bool terminal, const bool projectGround,
	const std::vector<CNavArea *> &corridorAreas,
	const std::vector<InflatedBuilding> &buildings,
	const Vector &start, std::vector<LocalDetourCandidate> *candidates)
{
	if (candidates == nullptr)
		return;

	Vector point = rawPoint;
	if (projectGround && !GroundPointWithinLocalCorridor(point,
		corridorAreas, &point))
		return;
	if (!PointWithinLocalCorridor(point, corridorAreas))
		return;
	if ((point - start).LengthSqr() > kPointDedupDistance * kPointDedupDistance &&
		PointBlockedByInflatedBuildings(point, buildings))
		return;

	for (const LocalDetourCandidate &existing : *candidates)
	{
		if ((existing.point - point).LengthSqr() <=
			kPointDedupDistance * kPointDedupDistance)
			return;
	}

	LocalDetourCandidate candidate;
	candidate.point = point;
	candidate.kind = kind;
	candidate.terminal = terminal;
	candidates->push_back(candidate);
}

void AddPortalEndpointCandidates(CNavArea *fromArea, CNavArea *toArea,
	const std::vector<CNavArea *> &corridorAreas,
	const std::vector<InflatedBuilding> &buildings, const Vector &start,
	const bool terminal, std::vector<LocalDetourCandidate> *candidates)
{
	if (fromArea == nullptr || toArea == nullptr)
		return;

	Vector center = vec3_origin;
	float halfWidth = 0.0f;
	const NavDirType direction = fromArea->ComputeLargestPortal(toArea,
		&center, &halfWidth);
	Vector endpoints[2] = { center, center };
	if (direction == NORTH || direction == SOUTH)
	{
		endpoints[0].x -= halfWidth;
		endpoints[1].x += halfWidth;
	}
	else if (direction == EAST || direction == WEST)
	{
		endpoints[0].y -= halfWidth;
		endpoints[1].y += halfWidth;
	}
	else
	{
		return;
	}

	for (Vector endpoint : endpoints)
	{
		// Keep the portal endpoint just inside the shared portal. Moving it
		// outward can put the player hull on the solid boundary beyond the
		// actual NavArea even though the point is within the diagnostic slack.
		if (std::fabs(endpoint.x - center.x) > 1.0f)
			endpoint.x += endpoint.x < center.x ? kCornerEpsilon : -kCornerEpsilon;
		if (std::fabs(endpoint.y - center.y) > 1.0f)
			endpoint.y += endpoint.y < center.y ? kCornerEpsilon : -kCornerEpsilon;
		AddLocalDetourCandidate(endpoint, "portal_endpoint", terminal, true,
			corridorAreas, buildings, start, candidates);
	}
}

bool ValidateLocalDetourEdge(const LocalDetourCandidate &from,
	const LocalDetourCandidate &to,
	const std::vector<CNavArea *> &corridorAreas,
	const std::vector<InflatedBuilding> &buildings, const int fromIndex,
	const int toIndex)
{
	const char *reason = "accepted";
	trace_t trace;
	const bool intersectsBuilding = LogSegmentBuildingIntersections(
		"candidate", fromIndex, from, toIndex, to, buildings);
	if (intersectsBuilding)
		reason = "cuts-inflated-building";
	else if (!SegmentWithinLocalCorridor(from.point, to.point, corridorAreas))
		reason = "outside-local-corridor";
	else if (!HullTraceIsClear(from.point, to.point, &trace))
		reason = "hull-trace-blocked";

	const bool accepted = std::strcmp(reason, "accepted") == 0;
	if (accepted)
	{
		Msg("[RCBOT2] NavMesh building-detour edge from=%d(%s) to=%d(%s) "
			"accepted=1 length=%.1f fraction=%.3f startsolid=%d allsolid=%d\n",
			fromIndex, from.kind, toIndex, to.kind,
			(from.point - to.point).Length(), trace.fraction,
			trace.startsolid ? 1 : 0, trace.allsolid ? 1 : 0);
	}
	else
	{
		Msg("[RCBOT2] NavMesh building-detour edge from=%d(%s) to=%d(%s) "
			"accepted=0 reason=%s\n",
			fromIndex, from.kind, toIndex, to.kind, reason);
	}
	return accepted;
}

LocalDetourResult BuildLocalDetour(const Vector &start, const Vector &desired,
	const std::vector<unsigned int> &areaIds, const std::size_t segmentIndex)
{
	LocalDetourResult result;
	if (TheNavMesh == nullptr || areaIds.empty())
		return result;

	std::vector<CNavArea *> corridorAreas;
	const std::size_t firstAreaIndex =
		std::min(segmentIndex, areaIds.size() - 1);
	const std::size_t secondAreaIndex =
		std::min(segmentIndex + 1, areaIds.size() - 1);
	CNavArea *firstArea = TheNavMesh->GetNavAreaByID(areaIds[firstAreaIndex]);
	CNavArea *secondArea = TheNavMesh->GetNavAreaByID(areaIds[secondAreaIndex]);
	if (firstArea != nullptr)
		corridorAreas.push_back(firstArea);
	if (secondArea != nullptr && secondArea != firstArea)
		corridorAreas.push_back(secondArea);
	if (corridorAreas.empty())
		return result;

	const float corridorMinX = std::min(start.x, desired.x) - kCorridorSlack;
	const float corridorMaxX = std::max(start.x, desired.x) + kCorridorSlack;
	const float corridorMinY = std::min(start.y, desired.y) - kCorridorSlack;
	const float corridorMaxY = std::max(start.y, desired.y) + kCorridorSlack;
	const float corridorMinZ = std::min(start.z, desired.z);
	const float corridorMaxZ = std::max(start.z, desired.z) + 72.0f;

	std::vector<InflatedBuilding> buildings;
	for (const BuildingCandidate &building : s_buildingRegistry.Active())
	{
		InflatedBuilding inflated;
		inflated.source = &building;
		inflated.mins = building.mins;
		inflated.maxs = building.maxs;
		inflated.mins.x -= kPlayerHullRadius + kSafetyMargin;
		inflated.mins.y -= kPlayerHullRadius + kSafetyMargin;
		inflated.maxs.x += kPlayerHullRadius + kSafetyMargin;
		inflated.maxs.y += kPlayerHullRadius + kSafetyMargin;
		if (inflated.maxs.x < corridorMinX || inflated.mins.x > corridorMaxX ||
			inflated.maxs.y < corridorMinY || inflated.mins.y > corridorMaxY ||
			inflated.maxs.z < corridorMinZ || inflated.mins.z > corridorMaxZ)
			continue;
		buildings.push_back(inflated);
	}

	if (buildings.empty())
	{
		result.success = true;
		result.points.push_back(desired);
		return result;
	}
	result.affected = true;

	Msg("[RCBOT2] NavMesh building-detour segment=%zu affected_buildings=%zu "
		"start=(%.1f %.1f %.1f) desired=(%.1f %.1f %.1f) "
		"hull_radius=%.1f safety_margin=%.1f corridor_slack=%.1f\n",
		segmentIndex, buildings.size(), start.x, start.y, start.z,
		desired.x, desired.y, desired.z, kPlayerHullRadius, kSafetyMargin,
		kCorridorSlack);
	for (std::size_t i = 0; i < buildings.size(); ++i)
	{
		const InflatedBuilding &building = buildings[i];
		Msg("[RCBOT2] NavMesh building-detour building[%zu] entity_index=%d "
			"serial=%d classname=%s world_mins=(%.1f %.1f %.1f) "
			"world_maxs=(%.1f %.1f %.1f) inflated_mins=(%.1f %.1f %.1f) "
			"inflated_maxs=(%.1f %.1f %.1f)\n",
			i, building.source->entityIndex, building.source->serial,
			building.source->classname.c_str(), building.source->mins.x,
			building.source->mins.y, building.source->mins.z,
			building.source->maxs.x, building.source->maxs.y,
			building.source->maxs.z, building.mins.x, building.mins.y,
			building.mins.z, building.maxs.x, building.maxs.y, building.maxs.z);
	}

	std::vector<LocalDetourCandidate> candidates;
	AddLocalDetourCandidate(start, "segment_start", false, false,
		corridorAreas, buildings, start, &candidates);
	const bool desiredBlocked = PointBlockedByInflatedBuildings(desired, buildings);
	if (!desiredBlocked)
		AddLocalDetourCandidate(desired, "segment_goal", true, false,
			corridorAreas, buildings, start, &candidates);

	for (std::size_t buildingIndex = 0; buildingIndex < buildings.size();
		++buildingIndex)
	{
		const InflatedBuilding &building = buildings[buildingIndex];
		const Vector cornerPoints[4] = {
			Vector(building.mins.x - kCornerEpsilon,
				building.mins.y - kCornerEpsilon, start.z),
			Vector(building.mins.x - kCornerEpsilon,
				building.maxs.y + kCornerEpsilon, start.z),
			Vector(building.maxs.x + kCornerEpsilon,
				building.maxs.y + kCornerEpsilon, start.z),
			Vector(building.maxs.x + kCornerEpsilon,
				building.mins.y - kCornerEpsilon, start.z)
		};
		const char *cornerKinds[4] = {
			"inflated_aabb_corner_SW", "inflated_aabb_corner_NW",
			"inflated_aabb_corner_NE", "inflated_aabb_corner_SE"
		};
		for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
		{
			AddLocalDetourCandidate(cornerPoints[cornerIndex],
				cornerKinds[cornerIndex], desiredBlocked, true,
				corridorAreas, buildings, start, &candidates);
		}
	}

	if (secondArea != firstArea)
		AddPortalEndpointCandidates(firstArea, secondArea, corridorAreas,
			buildings, start, desiredBlocked, &candidates);

	Msg("[RCBOT2] NavMesh building-detour candidates=%zu desired_blocked=%d\n",
		candidates.size(), desiredBlocked ? 1 : 0);
	for (std::size_t i = 0; i < candidates.size(); ++i)
	{
		Msg("[RCBOT2] NavMesh building-detour candidate[%zu] kind=%s terminal=%d "
			"point=(%.1f %.1f %.1f)\n", i, candidates[i].kind,
			candidates[i].terminal ? 1 : 0, candidates[i].point.x,
			candidates[i].point.y, candidates[i].point.z);
	}

	if (candidates.empty())
		return result;

	struct QueueNode
	{
		float distance;
		std::size_t index;
		bool operator<(const QueueNode &other) const
		{
			return distance > other.distance;
		}
	};
	const float infinity = std::numeric_limits<float>::max();
	std::vector<float> distances(candidates.size(), infinity);
	std::vector<int> previous(candidates.size(), -1);
	std::priority_queue<QueueNode> open;
	distances[0] = 0.0f;
	open.push({ 0.0f, 0 });
	int terminalIndex = -1;
	while (!open.empty())
	{
		const QueueNode current = open.top();
		open.pop();
		if (current.distance != distances[current.index])
			continue;
		for (std::size_t next = 0; next < candidates.size(); ++next)
		{
			if (next == current.index || !ValidateLocalDetourEdge(
				candidates[current.index], candidates[next], corridorAreas,
				buildings, static_cast<int>(current.index),
				static_cast<int>(next)))
				continue;
			const float edgeLength = (candidates[current.index].point -
				candidates[next].point).Length();
			const float nextDistance = current.distance + edgeLength;
			if (nextDistance >= distances[next])
				continue;
			distances[next] = nextDistance;
			previous[next] = static_cast<int>(current.index);
			open.push({ nextDistance, next });
		}
	}

	float bestTerminalScore = infinity;
	for (std::size_t i = 0; i < candidates.size(); ++i)
	{
		if (!candidates[i].terminal || distances[i] >= infinity)
			continue;
		const float terminalBias = 0.35f *
			(candidates[i].point - desired).Length();
		const float score = distances[i] + terminalBias;
		if (score < bestTerminalScore)
		{
			bestTerminalScore = score;
			terminalIndex = static_cast<int>(i);
		}
	}

	if (terminalIndex < 0)
	{
		Msg("[RCBOT2] NavMesh building-detour local_path=none\n");
		return result;
	}

	std::vector<int> route;
	for (int index = terminalIndex; index >= 0; index = previous[
		static_cast<std::size_t>(index)])
		route.push_back(index);
	std::reverse(route.begin(), route.end());
	if (route.empty() || route.front() != 0)
		return result;

	result.success = true;
	for (const int index : route)
		result.points.push_back(candidates[static_cast<std::size_t>(index)].point);
	Msg("[RCBOT2] NavMesh building-detour local_path=success points=%zu "
		"length=%.1f terminal=%s\n", result.points.size(),
		(distances[static_cast<std::size_t>(terminalIndex)]),
		candidates[static_cast<std::size_t>(terminalIndex)].kind);
	for (std::size_t i = 0; i < result.points.size(); ++i)
	{
		Msg("[RCBOT2] NavMesh building-detour local_point[%zu]=(%.1f %.1f %.1f)\n",
			i, result.points[i].x, result.points[i].y, result.points[i].z);
	}
	for (std::size_t i = 1; i < result.points.size(); ++i)
	{
		const LocalDetourCandidate fromCandidate = {
			result.points[i - 1], "local_path_point", false
		};
		const LocalDetourCandidate toCandidate = {
			result.points[i], "local_path_point", false
		};
		LogSegmentBuildingIntersections("local", static_cast<int>(i - 1),
			fromCandidate, static_cast<int>(i), toCandidate, buildings);
	}
	return result;
}

void AppendDetourPoint(std::vector<Vector> *points, const Vector &point)
{
	if (points == nullptr)
		return;
	if (!points->empty() &&
		(points->back() - point).LengthSqr() <= 24.0f * 24.0f)
		return;
	points->push_back(point);
}

bool BuildBuildingAwarePathInternal(const Vector &from, const Vector &to,
	RCBotNavMesh::Path *path)
{
	if (path == nullptr)
		return false;

	RCBotNavMesh::Path globalPath;
	const RCBotNavMesh::PathResult globalResult =
		RCBotNavMesh::BuildPath(from, to, globalPath);
	PrintPathSummary("global-route", globalResult, globalPath);
	if (globalResult != RCBotNavMesh::PathResult::SUCCESS)
		return false;

	RCBotNavMesh::Path detourPath = globalPath;
	detourPath.points.clear();
	detourPath.length = 0.0f;
	AppendDetourPoint(&detourPath.points, from);
	for (std::size_t i = 1; i < globalPath.points.size(); ++i)
	{
		const Vector start = detourPath.points.back();
		const LocalDetourResult local = BuildLocalDetour(start,
			globalPath.points[i], globalPath.areaIds, i - 1);
		if (!local.success)
		{
			Msg("[RCBOT2] NavMesh building-detour result=FAIL "
				"segment=%zu reason=no-valid-local-route\n", i - 1);
			return false;
		}
		for (std::size_t pointIndex = 1; pointIndex < local.points.size();
			++pointIndex)
			AppendDetourPoint(&detourPath.points, local.points[pointIndex]);
	}

	for (std::size_t i = 1; i < detourPath.points.size(); ++i)
		detourPath.length += (detourPath.points[i] -
			detourPath.points[i - 1]).Length();
	*path = detourPath;
	PrintPathSummary("building-detour-final", RCBotNavMesh::PathResult::SUCCESS,
		detourPath);
	if (debugoverlay != nullptr)
	{
		for (std::size_t i = 1; i < detourPath.points.size(); ++i)
			debugoverlay->AddLineOverlay(detourPath.points[i - 1],
				detourPath.points[i], 255, 128, 0, true, 20.0f);
	}
	return true;
}
}

namespace RCBotNavMeshPoc
{
void RecordHumanMovementFrame()
{
	RecordHumanMovementFrameInternal();
}

void ReconcileBuildings()
{
	s_buildingRegistry.Reconcile();
}

void ResetBuildings()
{
	s_buildingRegistry.Reset();
}

bool BuildBuildingAwarePath(const Vector &from, const Vector &to,
	RCBotNavMesh::Path *path)
{
	if (path == nullptr)
		return false;
	s_buildingRegistry.Reconcile();
	return BuildBuildingAwarePathInternal(from, to, path);
}
}

CON_COMMAND(rcbot_nav_record_start,
	"Diagnostic only: record raw movement samples for the connected human player")
{
	if (s_humanMovementRecordActive)
	{
		Msg("[RCBOT2] Nav movement-record event=already-active userid=%d "
			"samples=%d\n", s_humanMovementRecordUserId,
			s_humanMovementRecordSample);
		return;
	}

	if (gpGlobals == nullptr || engine == nullptr || playerinfomanager == nullptr)
	{
		Msg("[RCBOT2] Nav movement-record event=start-failed "
			"reason=player-interfaces\n");
		return;
	}
	edict_t *player = FindHumanMovementRecordPlayer();
	if (player == nullptr)
	{
		Msg("[RCBOT2] Nav movement-record event=start-failed "
			"reason=no-connected-human\n");
		return;
	}
	IPlayerInfo *info = playerinfomanager->GetPlayerInfo(player);
	s_humanMovementRecordActive = true;
	s_humanMovementRecordPlayerIndex = engine->IndexOfEdict(player);
	s_humanMovementRecordUserId = engine->GetPlayerUserId(player);
	s_humanMovementRecordSample = 0;
	s_humanMovementRecordNextSample = gpGlobals->curtime;
	s_humanMovementRecordPreviousOrigin = vec3_origin;
	s_humanMovementRecordHasPrevious = false;
	Msg("[RCBOT2] Nav movement-record event=start userid=%d index=%d "
		"name=%s origin=(%.2f %.2f %.2f)\n",
		s_humanMovementRecordUserId, s_humanMovementRecordPlayerIndex,
		info != nullptr && info->GetName() != nullptr ? info->GetName() : "<none>",
		info != nullptr ? info->GetAbsOrigin().x : 0.0f,
		info != nullptr ? info->GetAbsOrigin().y : 0.0f,
		info != nullptr ? info->GetAbsOrigin().z : 0.0f);
}

CON_COMMAND(rcbot_nav_record_stop,
	"Diagnostic only: stop raw movement sampling for the human player")
{
	if (!s_humanMovementRecordActive)
	{
		Msg("[RCBOT2] Nav movement-record event=stop-ignored reason=not-active\n");
		return;
	}
	Msg("[RCBOT2] Nav movement-record event=stop userid=%d index=%d "
		"samples=%d\n", s_humanMovementRecordUserId,
		s_humanMovementRecordPlayerIndex, s_humanMovementRecordSample);
	ResetHumanMovementRecorder();
}

CON_COMMAND(rcbot_navmesh_entity_sweep,
	"Diagnostic only: sweep player hull from userid to NavMesh target: userid x y z target_x target_y target_z")
{
	if (args.ArgC() != 8)
	{
		Msg("Usage: rcbot_navmesh_entity_sweep userid x y z target_x target_y target_z\n");
		return;
	}

	const int userId = std::atoi(args.Arg(1));
	const Vector from(
		static_cast<float>(std::atof(args.Arg(2))),
		static_cast<float>(std::atof(args.Arg(3))),
		static_cast<float>(std::atof(args.Arg(4))));
	const Vector target(
		static_cast<float>(std::atof(args.Arg(5))),
		static_cast<float>(std::atof(args.Arg(6))),
		static_cast<float>(std::atof(args.Arg(7))));
	const Vector hullMins(-16.0f, -16.0f, 0.0f);
	const Vector hullMaxs(16.0f, 16.0f, 72.0f);

	if (engine == nullptr || servergameents == nullptr || gpGlobals == nullptr)
	{
		Msg("[RCBOT2] NavMesh entity-sweep result=unavailable reason=engine-interfaces\n");
		return;
	}

	edict_t *bot = nullptr;
	for (int index = 1; index <= gpGlobals->maxClients; ++index)
	{
		edict_t *candidate = engine->PEntityOfEntIndex(index);
		if (candidate != nullptr && !candidate->IsFree() &&
			engine->GetPlayerUserId(candidate) == userId)
		{
			bot = candidate;
			break;
		}
	}
	if (bot == nullptr)
	{
		Msg("[RCBOT2] NavMesh entity-sweep result=unavailable userid=%d reason=player-not-found\n",
			userId);
		return;
	}

	RCBotNavMesh::Path path;
	const RCBotNavMesh::PathResult result =
		RCBotNavMesh::BuildPath(from, target, path);
	if (result != RCBotNavMesh::PathResult::SUCCESS)
	{
		Msg("[RCBOT2] NavMesh entity-sweep result=path-failed userid=%d reason=%s "
			"from=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f)\n",
			userId, RCBotNavMesh::PathResultName(result), from.x, from.y,
			from.z, target.x, target.y, target.z);
		return;
	}

	const Vector moveTarget = path.points.size() > 1 ? path.points[1] : target;
	const Vector sweepMin(
		std::min(from.x, moveTarget.x) + hullMins.x,
		std::min(from.y, moveTarget.y) + hullMins.y,
		std::min(from.z, moveTarget.z) + hullMins.z);
	const Vector sweepMax(
		std::max(from.x, moveTarget.x) + hullMaxs.x,
		std::max(from.y, moveTarget.y) + hullMaxs.y,
		std::max(from.z, moveTarget.z) + hullMaxs.z);

	CNavArea *currentArea = TheNavMesh != nullptr ?
		TheNavMesh->GetNearestNavArea(from) : nullptr;
	CNavArea *nextArea = nullptr;
	if (currentArea != nullptr)
	{
		for (std::size_t i = 0; i < path.areaIds.size(); ++i)
		{
			if (path.areaIds[i] != currentArea->GetID())
				continue;
			if (i + 1 < path.areaIds.size() && TheNavMesh != nullptr)
				nextArea = TheNavMesh->GetNavAreaByID(path.areaIds[i + 1]);
			break;
		}
	}
	const int currentAreaId = currentArea != nullptr ?
		static_cast<int>(currentArea->GetID()) : -1;
	const int nextAreaId = nextArea != nullptr ?
		static_cast<int>(nextArea->GetID()) : -1;
	CNavArea *area2 = TheNavMesh != nullptr ?
		TheNavMesh->GetNavAreaByID(2) : nullptr;

	Msg("[RCBOT2] NavMesh entity-sweep begin userid=%d bot_index=%d "
		"bot_origin=(%.1f %.1f %.1f) move_target=(%.1f %.1f %.1f) "
		"final_target=(%.1f %.1f %.1f) hull_mins=(%.1f %.1f %.1f) "
		"hull_maxs=(%.1f %.1f %.1f) current_area=%d next_area=%d "
		"route_start_area=%d route_goal_area=%d route_areas=%zu route_points=%zu\n",
		userId, engine->IndexOfEdict(bot), from.x, from.y, from.z,
		moveTarget.x, moveTarget.y, moveTarget.z, target.x, target.y, target.z,
		hullMins.x, hullMins.y, hullMins.z, hullMaxs.x, hullMaxs.y, hullMaxs.z,
		currentAreaId, nextAreaId, path.startAreaId, path.goalAreaId,
		path.areaIds.size(), path.points.size());

	for (std::size_t i = 0; i < path.points.size(); ++i)
	{
		Msg("[RCBOT2] NavMesh entity-sweep path_point[%zu]=(%.1f %.1f %.1f)\n",
			i, path.points[i].x, path.points[i].y, path.points[i].z);
	}
	PrintAreaBounds("current", currentAreaId, currentArea);
	PrintAreaBounds("next", nextAreaId, nextArea);
	PrintAreaBounds("area2", 2, area2);
	PrintPortal(currentAreaId, nextAreaId, currentArea, nextArea);

	Extent areaExtent;
	areaExtent.lo = vec3_origin;
	areaExtent.hi = vec3_origin;
	if (area2 != nullptr)
	{
		area2->GetExtent(&areaExtent);
		PrintEntityCandidates(sweepMin, sweepMax, areaExtent.lo, areaExtent.hi,
			engine->IndexOfEdict(bot));
	}

	CTraceFilterSkipList filter(COLLISION_GROUP_PLAYER_MOVEMENT);
	if (bot->GetIServerEntity() != nullptr)
		filter.Skip(bot->GetIServerEntity());
	int hitCount = 0;
	for (; hitCount < 32; ++hitCount)
	{
		trace_t trace;
		UTIL_TraceHull(from, moveTarget, hullMins, hullMaxs,
			MASK_PLAYERSOLID, filter, &trace);
		if (!trace.DidHit())
		{
			Msg("[RCBOT2] NavMesh entity-sweep hit=%d none fraction=%.6f\n",
				hitCount, trace.fraction);
			break;
		}

		edict_t *hitEntity = trace.m_pEnt != nullptr ?
			servergameents->BaseEntityToEdict(trace.m_pEnt) : nullptr;
		const bool hitWorld = hitEntity != nullptr &&
			engine->IndexOfEdict(hitEntity) == 0;
		bool areaOverlap = false;
		bool corridorOverlap = false;
		if (hitEntity != nullptr && area2 != nullptr)
		{
			Vector entityMin;
			Vector entityMax;
			if (GetEntityWorldBounds(hitEntity, &entityMin, &entityMax))
			{
				areaOverlap = BoxOverlaps(entityMin, entityMax,
					areaExtent.lo, areaExtent.hi);
				corridorOverlap = BoxOverlaps(entityMin, entityMax,
					sweepMin, sweepMax);
			}
		}
		PrintEntityDetails("hit", hitEntity, &trace.endpos, &trace,
			areaOverlap, corridorOverlap);
		if (hitWorld || hitEntity == nullptr ||
			hitEntity->GetIServerEntity() == nullptr)
			break;
		filter.Skip(hitEntity->GetIServerEntity());
	}
	Msg("[RCBOT2] NavMesh entity-sweep end userid=%d hits=%d\n",
		userId, hitCount);

	CTraceFilterSkipList geometryFilter(COLLISION_GROUP_NONE);
	if (bot->GetIServerEntity() != nullptr)
		geometryFilter.Skip(bot->GetIServerEntity());
	trace_t geometryTrace;
	UTIL_TraceHull(from, moveTarget, hullMins, hullMaxs,
		MASK_SOLID, geometryFilter, &geometryTrace);
	if (geometryTrace.DidHit())
	{
		edict_t *geometryEntity = geometryTrace.m_pEnt != nullptr ?
			servergameents->BaseEntityToEdict(geometryTrace.m_pEnt) : nullptr;
		bool areaOverlap = false;
		bool corridorOverlap = false;
		if (geometryEntity != nullptr && area2 != nullptr)
		{
			Vector entityMin;
			Vector entityMax;
			if (GetEntityWorldBounds(geometryEntity, &entityMin, &entityMax))
			{
				areaOverlap = BoxOverlaps(entityMin, entityMax,
					areaExtent.lo, areaExtent.hi);
				corridorOverlap = BoxOverlaps(entityMin, entityMax,
					sweepMin, sweepMax);
			}
		}
		PrintEntityDetails("geometry-hit", geometryEntity, &geometryTrace.endpos,
			&geometryTrace, areaOverlap, corridorOverlap);
	}
	else
	{
		Msg("[RCBOT2] NavMesh entity-sweep geometry-hit=none fraction=%.6f\n",
			geometryTrace.fraction);
	}

	if (debugoverlay != nullptr)
	{
		debugoverlay->AddSweptBoxOverlay(from, moveTarget, hullMins, hullMaxs,
			QAngle(0.0f, 0.0f, 0.0f), 255, 0, 0, 96, 10.0f);
		debugoverlay->AddLineOverlay(from, moveTarget, 255, 0, 0, true, 10.0f);
		if (area2 != nullptr)
		{
			const Vector center = (areaExtent.lo + areaExtent.hi) * 0.5f;
			debugoverlay->AddBoxOverlay(center, areaExtent.lo - center,
				areaExtent.hi - center, QAngle(0.0f, 0.0f, 0.0f),
				0, 255, 0, 48, 10.0f);
		}
		if (path.points.size() > 1)
		{
			const Vector &crossing = path.points[1];
			debugoverlay->AddBoxOverlay(crossing, Vector(-4.0f, -4.0f, -4.0f),
				Vector(4.0f, 4.0f, 4.0f), QAngle(0.0f, 0.0f, 0.0f),
				0, 0, 255, 192, 10.0f);
		}
	}
}

CON_COMMAND(rcbot_navmesh_building_collision_probe,
	"Diagnostic only: compare normal hull and entity-specific building collision: x1 y1 z1 x2 y2 z2")
{
	if (args.ArgC() != 7)
	{
		Msg("Usage: rcbot_navmesh_building_collision_probe x1 y1 z1 x2 y2 z2\n");
		return;
	}

	const Vector from(
		static_cast<float>(std::atof(args.Arg(1))),
		static_cast<float>(std::atof(args.Arg(2))),
		static_cast<float>(std::atof(args.Arg(3))));
	const Vector to(
		static_cast<float>(std::atof(args.Arg(4))),
		static_cast<float>(std::atof(args.Arg(5))),
		static_cast<float>(std::atof(args.Arg(6))));

	if (engine == nullptr || gpGlobals == nullptr || enginetrace == nullptr)
	{
		Msg("[RCBOT2] NavMesh collision-probe result=unavailable "
			"reason=engine-trace-interfaces\n");
		return;
	}

	// The requested indices are the known Fresh->B diagnostic entities.  The
	// classname fallback keeps this probe useful if edict allocation changes
	// between isolated server restarts; no production navigation depends on it.
	edict_t *barracks = FindCollisionProbeEntity(94,
		"emp_building_nf_barracks");
	edict_t *vehiclePhysbox = FindCollisionProbeEntity(506,
		"emp_building_vehicle_physbox");
	Msg("[RCBOT2] NavMesh collision-probe begin "
		"start=(%.1f %.1f %.1f) portal=(%.1f %.1f %.1f) mask=MASK_ALL "
		"hull_mins=(-16.0 -16.0 0.0) hull_maxs=(16.0 16.0 72.0)\n",
		from.x, from.y, from.z, to.x, to.y, to.z);
	PrintCollisionProbeTarget("barracks", 94, barracks);
	PrintCollisionProbeTarget("vehicle-physbox", 506, vehiclePhysbox);

	RunCollisionProbeSegment("known-spawn-to-portal", from, to, barracks,
		vehiclePhysbox);
	RunCollisionProbeScan(from, to, barracks, vehiclePhysbox);
	Msg("[RCBOT2] NavMesh collision-probe end\n");
}

CON_COMMAND(rcbot_navmesh_building_collision_mapper,
	"Diagnostic only: sample parent building collision on a local player-hull grid: x1 y1 z1 x2 y2 z2 [player_index]")
{
	if (args.ArgC() != 7 && args.ArgC() != 8)
	{
		Msg("Usage: rcbot_navmesh_building_collision_mapper "
			"x1 y1 z1 x2 y2 z2 [player_index]\n");
		return;
	}

	const Vector from(
		static_cast<float>(std::atof(args.Arg(1))),
		static_cast<float>(std::atof(args.Arg(2))),
		static_cast<float>(std::atof(args.Arg(3))));
	const Vector to(
		static_cast<float>(std::atof(args.Arg(4))),
		static_cast<float>(std::atof(args.Arg(5))),
		static_cast<float>(std::atof(args.Arg(6))));
	const int playerIndex = args.ArgC() == 8 ? std::atoi(args.Arg(7)) : -1;

	if (engine == nullptr || gpGlobals == nullptr || enginetrace == nullptr ||
		playerinfomanager == nullptr)
	{
		Msg("[RCBOT2] NavMesh collision-mapper result=INVALID "
			"reason=engine-player-trace-interfaces\n");
		return;
	}

	// This is intentionally a diagnostic-only parent-collision mapper.  The
	// vehicle physbox is resolved and logged per cell, but never participates
	// in the cell state or connectivity graph.
	edict_t *barracks = FindCollisionProbeEntity(94,
		"emp_building_nf_barracks");
	edict_t *vehiclePhysbox = FindCollisionProbeEntity(506,
		"emp_building_vehicle_physbox");
	RunCollisionMapper(from, to, playerIndex, barracks, vehiclePhysbox);
}

CON_COMMAND(rcbot_navmesh_building_3d_topology,
	"Diagnostic only: inspect local 3D lower-to-upper building topology: x1 y1 z1 x2 y2 z2 [player_index]")
{
	if (args.ArgC() != 7 && args.ArgC() != 8)
	{
		Msg("Usage: rcbot_navmesh_building_3d_topology "
			"x1 y1 z1 x2 y2 z2 [player_index]\n");
		return;
	}

	const Vector lowerPoint(
		static_cast<float>(std::atof(args.Arg(1))),
		static_cast<float>(std::atof(args.Arg(2))),
		static_cast<float>(std::atof(args.Arg(3))));
	const Vector upperPoint(
		static_cast<float>(std::atof(args.Arg(4))),
		static_cast<float>(std::atof(args.Arg(5))),
		static_cast<float>(std::atof(args.Arg(6))));
	const int playerIndex = args.ArgC() == 8 ? std::atoi(args.Arg(7)) : -1;
	RunCollisionMapper3DTopology(lowerPoint, upperPoint, playerIndex);
}

CON_COMMAND(rcbot_navmesh_building_obstacle_poc,
	"Diagnostic only: compare NavAreaBuildPath before/after a generic emp_building_* avoidance registration: x1 y1 z1 x2 y2 z2")
{
	if (args.ArgC() != 7)
	{
		Msg("Usage: rcbot_navmesh_building_obstacle_poc x1 y1 z1 x2 y2 z2\n");
		return;
	}

	const Vector from(
		static_cast<float>(std::atof(args.Arg(1))),
		static_cast<float>(std::atof(args.Arg(2))),
		static_cast<float>(std::atof(args.Arg(3))));
	const Vector to(
		static_cast<float>(std::atof(args.Arg(4))),
		static_cast<float>(std::atof(args.Arg(5))),
		static_cast<float>(std::atof(args.Arg(6))));

	if (TheNavMesh == nullptr || !RCBotNavMesh::IsLoaded() ||
		engine == nullptr || gpGlobals == nullptr)
	{
		Msg("[RCBOT2] NavMesh building-obstacle result=unavailable "
			"reason=navmesh-or-entity-interfaces\n");
		return;
	}

	RCBotNavMesh::Path beforePath;
	const RCBotNavMesh::PathResult beforeResult =
		RCBotNavMesh::BuildPath(from, to, beforePath);
	PrintPathSummary("before-registration", beforeResult, beforePath);
	if (beforeResult != RCBotNavMesh::PathResult::SUCCESS)
		return;
	PrintPathAreaStates("before-registration", beforePath);

	BuildingCandidate building;
	int discoveredCount = 0;
	if (!FindEmpiresBuildingForPath(beforePath, &building, &discoveredCount))
	{
		Msg("[RCBOT2] NavMesh building-obstacle result=no-building "
			"discovered_count=%d\n", discoveredCount);
		return;
	}

	Msg("[RCBOT2] NavMesh building-obstacle registration_begin "
		"entity_index=%d classname=%s area_overlap_scan=plugin-TheNavAreas\n",
		engine->IndexOfEdict(building.entity), SafeClassName(building.entity));
	for (int i = 0; i < TheNavAreas.Count(); ++i)
	{
		CNavArea *area = TheNavAreas[i];
		if (area == nullptr)
			continue;
		Extent areaExtent;
		area->GetExtent(&areaExtent);
		if (!BoxOverlaps(building.mins, building.maxs,
			areaExtent.lo, areaExtent.hi))
			continue;
		Msg("[RCBOT2] NavMesh building-obstacle overlap_area "
			"phase=before-registration area_id=%u blocked_team_any=%d "
			"avoidance=%d avoidance_height=%.1f\n",
			area->GetID(), area->IsBlocked(TEAM_ANY) ? 1 : 0,
			area->HasAvoidanceObstacle() ? 1 : 0,
			area->GetAvoidanceObstacleHeight());
	}

	CDiagnosticAvoidanceObstacle obstacle(building.entity);
	TheNavMesh->RegisterAvoidanceObstacle(&obstacle);
	UpdateDiagnosticObstacleAreas(building);
	PrintPathAreaStates("after-registration", beforePath);

	RCBotNavMesh::Path afterPath;
	const RCBotNavMesh::PathResult afterResult =
		RCBotNavMesh::BuildPath(from, to, afterPath);
	PrintPathSummary("after-registration", afterResult, afterPath);
	const bool sameAreaRoute =
		afterResult == RCBotNavMesh::PathResult::SUCCESS &&
		afterPath.areaIds == beforePath.areaIds;
	Msg("[RCBOT2] NavMesh building-obstacle comparison "
		"alternative_area_route=%d same_area_route=%d "
		"before_length=%.1f after_length=%.1f\n",
		sameAreaRoute ? 0 : 1, sameAreaRoute ? 1 : 0,
		beforePath.length,
		afterPath.length);

	obstacle.Disable();
	TheNavMesh->UnregisterAvoidanceObstacle(&obstacle);
	Msg("[RCBOT2] NavMesh building-obstacle registration_end "
		"entity_index=%d cleanup=unregistered avoidance_refresh=deferred_to_normal_mesh_update\n",
		engine->IndexOfEdict(building.entity));
}

CON_COMMAND(rcbot_navmesh_building_detour_poc,
	"Diagnostic only: build a local dynamic-building detour over the static NavMesh path: x1 y1 z1 x2 y2 z2")
{
	if (args.ArgC() != 7)
	{
		Msg("Usage: rcbot_navmesh_building_detour_poc x1 y1 z1 x2 y2 z2\n");
		return;
	}

	const Vector from(
		static_cast<float>(std::atof(args.Arg(1))),
		static_cast<float>(std::atof(args.Arg(2))),
		static_cast<float>(std::atof(args.Arg(3))));
	const Vector to(
		static_cast<float>(std::atof(args.Arg(4))),
		static_cast<float>(std::atof(args.Arg(5))),
		static_cast<float>(std::atof(args.Arg(6))));

	if (TheNavMesh == nullptr || !RCBotNavMesh::IsLoaded() ||
		engine == nullptr || gpGlobals == nullptr)
	{
		Msg("[RCBOT2] NavMesh building-detour result=unavailable "
			"reason=navmesh-or-entity-interfaces\n");
		return;
	}

	RCBotNavMesh::Path path;
	if (!RCBotNavMeshPoc::BuildBuildingAwarePath(from, to, &path))
	{
		Msg("[RCBOT2] NavMesh building-detour result=FAIL from=(%.1f %.1f %.1f) "
			"to=(%.1f %.1f %.1f)\n", from.x, from.y, from.z,
			to.x, to.y, to.z);
		return;
	}

	Msg("[RCBOT2] NavMesh building-detour result=PASS final_points=%zu "
		"final_length=%.1f route_areas=", path.points.size(), path.length);
	for (std::size_t i = 0; i < path.areaIds.size(); ++i)
		Msg("%s%u", i == 0 ? "" : "->", path.areaIds[i]);
	Msg("\n");
}

CON_COMMAND(rcbot_navmesh_poc, "Build and print an A->B NavMesh path: x1 y1 z1 x2 y2 z2")
{
	if (args.ArgC() != 7)
	{
		Msg("Usage: rcbot_navmesh_poc x1 y1 z1 x2 y2 z2\n");
		return;
	}

	const Vector from(
		static_cast<float>(std::atof(args.Arg(1))),
		static_cast<float>(std::atof(args.Arg(2))),
		static_cast<float>(std::atof(args.Arg(3))));
	const Vector to(
		static_cast<float>(std::atof(args.Arg(4))),
		static_cast<float>(std::atof(args.Arg(5))),
		static_cast<float>(std::atof(args.Arg(6))));

	RCBotNavMesh::Path path;
	const RCBotNavMesh::PathResult result =
		RCBotNavMesh::BuildPath(from, to, path);
	if (result != RCBotNavMesh::PathResult::SUCCESS)
	{
		Msg("[RCBOT2] NavMesh PoC result=%s from=(%.1f %.1f %.1f) to=(%.1f %.1f %.1f), loaded=%d areas=%u\n",
			RCBotNavMesh::PathResultName(result),
			from.x, from.y, from.z, to.x, to.y, to.z,
			RCBotNavMesh::IsLoaded() ? 1 : 0, RCBotNavMesh::GetAreaCount());
		return;
	}

	Msg("[RCBOT2] NavMesh PoC path points=%zu areas=%zu length=%.1f from=(%.1f %.1f %.1f) to=(%.1f %.1f %.1f)\n",
		path.points.size(), path.areaIds.size(), path.length,
		from.x, from.y, from.z, to.x, to.y, to.z);
	for (std::size_t i = 0; i < path.points.size(); ++i)
	{
		Msg("[RCBOT2] NavMesh PoC point[%zu]=(%.1f %.1f %.1f)\n",
			i, path.points[i].x, path.points[i].y, path.points[i].z);
	}
	if (path.points.size() > 1)
	{
		const Vector &segmentFrom = path.points[0];
		const Vector &segmentTo = path.points[1];
		PrintSegmentVariant(segmentFrom, segmentTo, "p0-p1-solid-none",
			MASK_SOLID, COLLISION_GROUP_NONE);
		PrintSegmentVariant(segmentFrom, segmentTo, "p0-p1-player-none",
			MASK_PLAYERSOLID, COLLISION_GROUP_NONE);
		PrintSegmentVariant(segmentFrom, segmentTo, "p0-p1-player-movement",
			MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT);
		PrintSegmentVariant(segmentFrom, segmentTo, "p0-p1-player-player",
			MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER);
	}
	CNavArea *startArea = TheNavMesh != nullptr ?
		TheNavMesh->GetNavAreaByID(static_cast<unsigned int>(path.startAreaId)) : nullptr;
	CNavArea *goalArea = TheNavMesh != nullptr ?
		TheNavMesh->GetNavAreaByID(static_cast<unsigned int>(path.goalAreaId)) : nullptr;
	if (startArea != nullptr)
	{
		Msg("[RCBOT2] NavMesh PoC start_area=%d center=(%.1f %.1f %.1f) "
			"nw=(%.1f %.1f %.1f) ne=(%.1f %.1f %.1f) se=(%.1f %.1f %.1f) "
			"sw=(%.1f %.1f %.1f)\n",
			path.startAreaId,
			startArea->GetCenter().x, startArea->GetCenter().y, startArea->GetCenter().z,
			startArea->GetCorner(NORTH_WEST).x,
			startArea->GetCorner(NORTH_WEST).y,
			startArea->GetCorner(NORTH_WEST).z,
			startArea->GetCorner(NORTH_EAST).x,
			startArea->GetCorner(NORTH_EAST).y,
			startArea->GetCorner(NORTH_EAST).z,
			startArea->GetCorner(SOUTH_EAST).x,
			startArea->GetCorner(SOUTH_EAST).y,
			startArea->GetCorner(SOUTH_EAST).z,
			startArea->GetCorner(SOUTH_WEST).x,
			startArea->GetCorner(SOUTH_WEST).y,
			startArea->GetCorner(SOUTH_WEST).z);
	}
	if (goalArea != nullptr)
	{
		Msg("[RCBOT2] NavMesh PoC goal_area=%d center=(%.1f %.1f %.1f) "
			"nw=(%.1f %.1f %.1f) ne=(%.1f %.1f %.1f) se=(%.1f %.1f %.1f) "
			"sw=(%.1f %.1f %.1f)\n",
			path.goalAreaId,
			goalArea->GetCenter().x, goalArea->GetCenter().y, goalArea->GetCenter().z,
			goalArea->GetCorner(NORTH_WEST).x,
			goalArea->GetCorner(NORTH_WEST).y,
			goalArea->GetCorner(NORTH_WEST).z,
			goalArea->GetCorner(NORTH_EAST).x,
			goalArea->GetCorner(NORTH_EAST).y,
			goalArea->GetCorner(NORTH_EAST).z,
			goalArea->GetCorner(SOUTH_EAST).x,
			goalArea->GetCorner(SOUTH_EAST).y,
			goalArea->GetCorner(SOUTH_EAST).z,
			goalArea->GetCorner(SOUTH_WEST).x,
			goalArea->GetCorner(SOUTH_WEST).y,
			goalArea->GetCorner(SOUTH_WEST).z);
		const Vector startInset[] = {
			Vector(startArea->GetCorner(NORTH_WEST).x + 64.0f,
				startArea->GetCorner(NORTH_WEST).y + 64.0f,
				startArea->GetZ(startArea->GetCorner(NORTH_WEST).x + 64.0f,
					startArea->GetCorner(NORTH_WEST).y + 64.0f)),
			Vector(startArea->GetCorner(NORTH_EAST).x - 64.0f,
				startArea->GetCorner(NORTH_EAST).y + 64.0f,
				startArea->GetZ(startArea->GetCorner(NORTH_EAST).x - 64.0f,
					startArea->GetCorner(NORTH_EAST).y + 64.0f)),
			Vector(startArea->GetCorner(SOUTH_EAST).x - 64.0f,
				startArea->GetCorner(SOUTH_EAST).y - 64.0f,
				startArea->GetZ(startArea->GetCorner(SOUTH_EAST).x - 64.0f,
					startArea->GetCorner(SOUTH_EAST).y - 64.0f)),
			Vector(startArea->GetCorner(SOUTH_WEST).x + 64.0f,
				startArea->GetCorner(SOUTH_WEST).y - 64.0f,
				startArea->GetZ(startArea->GetCorner(SOUTH_WEST).x + 64.0f,
					startArea->GetCorner(SOUTH_WEST).y - 64.0f))
		};
		const Vector goalInset[] = {
			Vector(goalArea->GetCorner(NORTH_WEST).x + 64.0f,
				goalArea->GetCorner(NORTH_WEST).y + 64.0f,
				goalArea->GetZ(goalArea->GetCorner(NORTH_WEST).x + 64.0f,
					goalArea->GetCorner(NORTH_WEST).y + 64.0f)),
			Vector(goalArea->GetCorner(NORTH_EAST).x - 64.0f,
				goalArea->GetCorner(NORTH_EAST).y + 64.0f,
				goalArea->GetZ(goalArea->GetCorner(NORTH_EAST).x - 64.0f,
					goalArea->GetCorner(NORTH_EAST).y + 64.0f)),
			Vector(goalArea->GetCorner(SOUTH_EAST).x - 64.0f,
				goalArea->GetCorner(SOUTH_EAST).y - 64.0f,
				goalArea->GetZ(goalArea->GetCorner(SOUTH_EAST).x - 64.0f,
					goalArea->GetCorner(SOUTH_EAST).y - 64.0f)),
			Vector(goalArea->GetCorner(SOUTH_WEST).x + 64.0f,
				goalArea->GetCorner(SOUTH_WEST).y - 64.0f,
				goalArea->GetZ(goalArea->GetCorner(SOUTH_WEST).x + 64.0f,
					goalArea->GetCorner(SOUTH_WEST).y - 64.0f))
		};
		for (int i = 0; i < 4; ++i)
		{
			char label[32];
			V_snprintf(label, sizeof(label), "from-start-%d", i);
			PrintSegment(from, startInset[i], label);
			V_snprintf(label, sizeof(label), "start-to-goal-%d", i);
			PrintSegment(from, goalInset[i], label);
		}
	}
}
