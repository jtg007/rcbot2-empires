/* Minimal Valve-inspired path following for the RCBot2 NavMesh spike. */
#include "bot_navmesh_follower.h"

#include <algorithm>
#include <cmath>

#include "bot.h"
#include "bot_globals.h"
#include "cmodel.h"
#include "const.h"
#include "edict.h"
#include "navmesh/util/UtilTrace.h"

extern CGlobalVars *gpGlobals;

namespace
{
constexpr float kGoalTolerance = 25.0f;
constexpr float kGoalLookAhead = 50.0f;
constexpr float kPlayerStepHeight = 18.0f;
constexpr float kProgressDistance = 8.0f;
constexpr float kStuckSeconds = 3.0f;
constexpr float kAvoidRange = 50.0f;
constexpr float kAvoidGoalDistance = 100.0f;

float Now()
{
	return gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
}

Vector BotFeet(CBot *bot)
{
	if (bot == nullptr || bot->getEdict() == nullptr)
		return vec3_origin;
	return CBotGlobals::entityOrigin(bot->getEdict());
}

bool GetPlayerHull(CBot *bot, Vector *mins, Vector *maxs)
{
	if (bot == nullptr || bot->getEdict() == nullptr ||
		bot->getEdict()->GetCollideable() == nullptr)
		return false;

	const ICollideable *collideable = bot->getEdict()->GetCollideable();
	*mins = collideable->OBBMins();
	*maxs = collideable->OBBMaxs();
	return maxs->x > mins->x && maxs->y > mins->y && maxs->z > mins->z;
}

void TracePlayerHull(CBot *bot, const Vector &from, const Vector &to,
	const Vector &mins, const Vector &maxs, trace_t *trace)
{
	const IHandleEntity *ignore = nullptr;
	if (bot != nullptr && bot->getEdict() != nullptr)
		ignore = bot->getEdict()->GetIServerEntity();
	UTIL_TraceHull(from, to, mins, maxs, MASK_PLAYERSOLID, ignore,
		COLLISION_GROUP_PLAYER_MOVEMENT, trace);
}
}

namespace RCBotNavMesh
{
const char *FollowStatusName(const FollowStatus status)
{
	switch (status)
	{
	case FollowStatus::INACTIVE:
		return "inactive";
	case FollowStatus::RUNNING:
		return "running";
	case FollowStatus::COMPLETE:
		return "complete";
	case FollowStatus::STUCK:
		return "stuck";
	case FollowStatus::FAILED:
		return "failed";
	}
	return "unknown";
}

void PathFollower::Reset()
{
	m_path.clear();
	m_goalIndex = 0;
	m_active = false;
	m_lastProgressPosition = vec3_origin;
	m_lastProgressTime = 0.0f;
	m_lastStatus = FollowStatus::INACTIVE;
}

void PathFollower::SetPath(const Path &path)
{
	m_path = path;
	m_goalIndex = m_path.points.size() > 1 ? 1 : m_path.points.size();
	m_active = !m_path.points.empty();
	m_lastProgressPosition = m_path.points.empty() ? vec3_origin :
		m_path.points.front();
	m_lastProgressTime = Now();
	m_lastStatus = FollowStatus::INACTIVE;
}

bool PathFollower::IsInPathArea(const Vector &point) const
{
	return IsPointInPathAreas(m_path, point);
}

bool PathFollower::IsPotentiallyTraversable(CBot *bot, const Vector &from,
	const Vector &to, float *fraction) const
{
	Vector hullMins;
	Vector hullMaxs;
	if (!GetPlayerHull(bot, &hullMins, &hullMaxs))
		return false;

	trace_t direct;
	TracePlayerHull(bot, from, to, hullMins, hullMaxs, &direct);
	if (fraction != nullptr)
		*fraction = direct.fraction;
	if (!direct.startsolid && !direct.allsolid && direct.fraction >= 1.0f)
		return true;

	// Match the useful part of Valve locomotion's step test: recheck the
	// segment above the step height, but never turn this into a detour search.
	Vector stepMins = hullMins;
	stepMins.z = (std::max)(stepMins.z, kPlayerStepHeight + 0.1f);
	trace_t stepped;
	TracePlayerHull(bot, from, to, stepMins, hullMaxs, &stepped);
	return !stepped.startsolid && !stepped.allsolid && stepped.fraction >= 1.0f;
}

bool PathFollower::HasPotentialGap(CBot *bot, const Vector &from,
	const Vector &to) const
{
	Vector hullMins;
	Vector hullMaxs;
	if (!GetPlayerHull(bot, &hullMins, &hullMaxs))
		return true;

	const Vector delta = to - from;
	const float distance = delta.Length2D();
	const int sampleCount = (std::max)(1, (std::min)(32,
		static_cast<int>(std::ceil(distance / 32.0f))));
	float firstGround = 0.0f;
	bool firstGroundKnown = false;
	for (int sample = 0; sample <= sampleCount; ++sample)
	{
		const float t = static_cast<float>(sample) /
			static_cast<float>(sampleCount);
		Vector feet = from + delta * t;
		Vector groundMins(hullMins.x, hullMins.y, 0.0f);
		Vector groundMaxs(hullMaxs.x, hullMaxs.y, 1.0f);
		trace_t ground;
		TracePlayerHull(bot,
			feet + Vector(0.0f, 0.0f, kPlayerStepHeight + 4.0f),
			feet - Vector(0.0f, 0.0f, 96.0f), groundMins, groundMaxs,
			&ground);
		if (ground.startsolid || ground.allsolid || !ground.DidHit())
			return true;

		if (!firstGroundKnown)
		{
			firstGround = ground.endpos.z;
			firstGroundKnown = true;
		}
		else if (firstGround - ground.endpos.z >
			kPlayerStepHeight + 8.0f)
		{
			return true;
		}
	}
	return false;
}

bool PathFollower::IsAtGoal(CBot *bot) const
{
	if (m_goalIndex == 0 || m_goalIndex >= m_path.points.size())
		return m_goalIndex >= m_path.points.size();

	const Vector feet = BotFeet(bot);
	const Vector &goal = m_path.points[m_goalIndex];
	const Vector toGoal = goal - feet;
	if (m_goalIndex + 1 < m_path.points.size())
	{
		const Vector &prior = m_path.points[m_goalIndex - 1];
		const Vector &next = m_path.points[m_goalIndex + 1];
		Vector dividingPlane = (goal - prior) + (next - goal);
		dividingPlane.z = 0.0f;
		if (DotProduct2D(toGoal.AsVector2D(), dividingPlane.AsVector2D()) < 0.0001f &&
			std::fabs(toGoal.z) < 72.0f && toGoal.z < kPlayerStepHeight &&
			IsPotentiallyTraversable(bot, feet, next) &&
			!HasPotentialGap(bot, feet, next))
			return true;
	}
	return toGoal.Length2D() < kGoalTolerance;
}

Vector PathFollower::Avoid(CBot *bot, const Vector &goal) const
{
	const Vector feet = BotFeet(bot);
	Vector forward = goal - feet;
	forward.z = 0.0f;
	if (forward.NormalizeInPlace() <= 0.001f)
		return goal;

	Vector left(-forward.y, forward.x, 0.0f);
	Vector hullMins;
	Vector hullMaxs;
	if (!GetPlayerHull(bot, &hullMins, &hullMaxs))
		return goal;

	const float halfWidth = 0.5f * (hullMaxs.x - hullMins.x);
	const float sensorHalf = (std::max)(4.0f, halfWidth * 0.25f);
	const float sensorOffset = halfWidth + 2.0f;
	Vector sensorMins(-sensorHalf, -sensorHalf,
		(std::max)(hullMins.z, kPlayerStepHeight + 0.1f));
	Vector sensorMaxs(sensorHalf, sensorHalf,
		(std::min)(hullMaxs.z, 36.0f));
	if (sensorMaxs.z <= sensorMins.z)
		sensorMaxs.z = hullMaxs.z;

	trace_t leftTrace;
	trace_t rightTrace;
	TracePlayerHull(bot, feet + sensorOffset * left,
		feet + sensorOffset * left + kAvoidRange * forward,
		sensorMins, sensorMaxs, &leftTrace);
	TracePlayerHull(bot, feet - sensorOffset * left,
		feet - sensorOffset * left + kAvoidRange * forward,
		sensorMins, sensorMaxs, &rightTrace);
	const bool leftClear = !leftTrace.startsolid && leftTrace.fraction >= 1.0f;
	const bool rightClear = !rightTrace.startsolid && rightTrace.fraction >= 1.0f;
	if (leftClear && rightClear)
		return goal;

	Vector side = vec3_origin;
	if (leftClear && !rightClear)
		side = left;
	else if (!leftClear && rightClear)
		side = -left;
	else if (leftTrace.fraction > rightTrace.fraction + 0.01f)
		side = left;
	else if (rightTrace.fraction > leftTrace.fraction + 0.01f)
		side = -left;
	else
		return goal;

	Vector adjustedDirection = 0.5f * forward + side;
	if (adjustedDirection.NormalizeInPlace() <= 0.001f)
		return goal;
	Vector adjustedGoal = feet + kAvoidGoalDistance * adjustedDirection;
	adjustedGoal.z = feet.z;
	if (!IsInPathArea(adjustedGoal) ||
		!IsPotentiallyTraversable(bot, feet, adjustedGoal))
		return goal;
	return adjustedGoal;
}

FollowStatus PathFollower::Update(CBot *bot)
{
	if (!m_active)
		return FollowStatus::INACTIVE;
	if (bot == nullptr || bot->getEdict() == nullptr ||
		bot->getEdict()->IsFree())
	{
		m_active = false;
		m_lastStatus = FollowStatus::FAILED;
		return FollowStatus::FAILED;
	}

	const Vector feet = BotFeet(bot);
	const float now = Now();
	if ((feet - m_lastProgressPosition).Length2D() >= kProgressDistance)
	{
		m_lastProgressPosition = feet;
		m_lastProgressTime = now;
	}
	else if (now - m_lastProgressTime >= kStuckSeconds)
	{
		m_active = false;
		m_lastStatus = FollowStatus::STUCK;
		return FollowStatus::STUCK;
	}

	while (m_goalIndex < m_path.points.size() && IsAtGoal(bot))
	{
		++m_goalIndex;
		if (m_goalIndex >= m_path.points.size())
		{
			m_active = false;
			m_lastStatus = FollowStatus::COMPLETE;
			return FollowStatus::COMPLETE;
		}
	}

	// Valve's look-ahead only skips nearby ground goals when the later goal is
	// directly traversable and does not cross a gap.
	while (m_goalIndex + 1 < m_path.points.size() &&
		(m_path.points[m_goalIndex] - feet).Length2D() < kGoalLookAhead)
	{
		const Vector &next = m_path.points[m_goalIndex + 1];
		if (!IsPotentiallyTraversable(bot, feet, next) ||
			HasPotentialGap(bot, feet, next))
			break;
		++m_goalIndex;
	}

	if (m_goalIndex >= m_path.points.size())
	{
		m_active = false;
		m_lastStatus = FollowStatus::COMPLETE;
		return FollowStatus::COMPLETE;
	}

	// Keep the spike on Valve's area-path geometry. Local side-steering is
	// intentionally disabled until the raw area crossing is proven on its own.
	const Vector moveGoal = m_path.points[m_goalIndex];
	bot->setMoveLookPriority(MOVELOOK_OVERRIDE);
	bot->setMoveTo(moveGoal);
	bot->setMoveLookPriority(MOVELOOK_TASK);
	m_lastStatus = FollowStatus::RUNNING;
	return FollowStatus::RUNNING;
}
}
