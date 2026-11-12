/* Bounded local graph exploration for isolated Empires navigation fixtures. */

#include "bot_local_explorer.h"

#include "bot.h"
#include "bot_empires_telemetry.h"
#include "bot_globals.h"
#include "cmodel.h"
#include "const.h"
#include "edict.h"
#include "eiface.h"
#include "navmesh/util/UtilTrace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

extern IServerGameEnts *servergameents;

namespace
{
using RCBotLocalExplorer::Config;
using RCBotLocalExplorer::Result;
using RCBotLocalExplorer::Status;

constexpr float kMaxVerticalDelta = 32.0f;
constexpr float kMaxStepHeight = 18.0f;
constexpr float kGoalDistance = 20.0f;
constexpr int kMaxSessions = 8;

enum class TraversalType : std::uint8_t
{
	WALK = 0,
	STEP_UP,
	STEP_DOWN
};

struct NodeKey
{
	int x = 0;
	int y = 0;
	int z = 0;

	bool operator==(const NodeKey &other) const
	{
		return x == other.x && y == other.y && z == other.z;
	}
};

struct Node
{
	NodeKey key;
	Vector point;
	int supportEntity = -1;
	int parent = -1;
	TraversalType incoming = TraversalType::WALK;
	float g = std::numeric_limits<float>::max();
	float h = 0.0f;
	bool open = false;
	bool closed = false;
};

struct Ground
{
	bool valid = false;
	Vector feet;
	int supportEntity = -1;
};

struct Session
{
	CBot *bot = nullptr;
	Config config;
	Result result = Result::NONE;
	std::uint64_t sessionId = 0;
	std::uint64_t intentId = 0;
	std::uint64_t attemptId = 0;
	int goalId = -1;
	int team = -1;
	Vector requestedStart;
	Vector requestedGoal;
	Vector start;
	std::vector<Node> nodes;
	std::vector<int> open;
	std::vector<Vector> path;
	std::vector<TraversalType> pathEdges;
	int goalNode = -1;
	int segment = 0;
	int probes = 0;
	int expansions = 0;
	int frames = 0;
	bool active = false;
	bool graphReady = false;
	bool attemptStarted = false;
	bool segmentFailureReported = false;
	float startedAt = 0.0f;
	float lastProgressAt = 0.0f;
	Vector lastProgressPosition;
};

std::vector<Session> g_sessions;
std::uint64_t g_nextSessionId = 0;
int g_nextGoalId = 100000;

const char *supportName(const int entityIndex)
{
	return entityIndex > 0 ? "entity" : "world";
}

bool finiteVector(const Vector &value)
{
	return value.IsValid() && std::isfinite(value.x) &&
		std::isfinite(value.y) && std::isfinite(value.z);
}

Config boundedConfig(const Config &requested)
{
	Config result = requested;
	result.radius = std::clamp(result.radius, 64.0f, 1024.0f);
	result.cellSize = std::clamp(result.cellSize, 16.0f, 64.0f);
	result.maxNodes = std::clamp(result.maxNodes, 2, 256);
	result.maxProbes = std::clamp(result.maxProbes, 8, 4096);
	result.maxExpansions = std::clamp(result.maxExpansions, 1, 1024);
	result.maxWorkPerFrame = std::clamp(result.maxWorkPerFrame, 1, 32);
	result.maxDuration = std::clamp(result.maxDuration, 1.0f, 30.0f);
	return result;
}

Session *findSession(const CBot *bot)
{
	for (Session &session : g_sessions)
		if (session.bot == bot)
			return &session;
	return nullptr;
}

void event(Session &session, const char *name, const char *detail)
{
	EmpiresAnalysis::RecordPathEvent(session.bot, name, session.goalId,
		detail);
}

int entityIndexFromTrace(const trace_t &trace)
{
	if (trace.m_pEnt == nullptr || servergameents == nullptr || engine == nullptr)
		return -1;
	edict_t *entity = servergameents->BaseEntityToEdict(trace.m_pEnt);
	return entity != nullptr ? engine->IndexOfEdict(entity) : -1;
}

bool canProbe(Session &session)
{
	if (session.probes >= session.config.maxProbes)
		return false;
	++session.probes;
	return true;
}

Ground findGround(Session &session, const Vector &xy, const float referenceZ)
{
	Ground result;
	if (!finiteVector(xy) || session.bot == nullptr ||
		session.bot->getEdict() == nullptr || enginetrace == nullptr ||
		!canProbe(session))
		return result;

	const ICollideable *collideable = session.bot->getEdict()->GetCollideable();
	if (collideable == nullptr)
		return result;
	const Vector hullMins = collideable->OBBMins();
	const Vector hullMaxs = collideable->OBBMaxs();
	if (!finiteVector(hullMins) || !finiteVector(hullMaxs) ||
		hullMaxs.x <= hullMins.x || hullMaxs.y <= hullMins.y)
		return result;

	// The short first trace avoids treating a low ceiling as support. The
	// progressively higher starts still find raised floors and building decks.
	const float starts[] = { 12.0f, 32.0f, 64.0f, 96.0f };
	for (const float startOffset : starts)
	{
		trace_t trace{};
		UTIL_TraceHull(
			Vector(xy.x, xy.y, referenceZ + startOffset),
			Vector(xy.x, xy.y, referenceZ - 192.0f),
			Vector(hullMins.x, hullMins.y, 0.0f),
			Vector(hullMaxs.x, hullMaxs.y, 1.0f), MASK_PLAYERSOLID,
			session.bot->getEdict()->GetIServerEntity(),
			COLLISION_GROUP_PLAYER_MOVEMENT, &trace);
		if (trace.startsolid || trace.allsolid || trace.fraction >= 1.0f ||
			!finiteVector(trace.endpos) || trace.plane.normal.z < 0.5f ||
			std::fabs(trace.endpos.z - referenceZ) > 128.0f)
			continue;

		result.valid = true;
		result.feet = trace.endpos;
		result.supportEntity = entityIndexFromTrace(trace);
		return result;
	}
	return result;
}

bool standable(Session &session, const Vector &candidate, Ground *ground)
{
	if (ground == nullptr || !finiteVector(candidate))
		return false;
	*ground = findGround(session, candidate, candidate.z);
	if (!ground->valid || !canProbe(session))
		return false;
	const ICollideable *collideable = session.bot->getEdict()->GetCollideable();
	if (collideable == nullptr)
		return false;

	trace_t trace{};
	const Vector lifted = ground->feet + Vector(0.0f, 0.0f, 1.0f);
	UTIL_TraceHull(lifted, lifted,
		collideable->OBBMins(), collideable->OBBMaxs(), MASK_PLAYERSOLID,
		session.bot->getEdict()->GetIServerEntity(),
		COLLISION_GROUP_PLAYER_MOVEMENT, &trace);
	return !trace.startsolid && !trace.allsolid && finiteVector(trace.endpos);
}

bool groundCorridor(Session &session, const Vector &from, const Vector &to)
{
	const float distance = (to - from).Length2D();
	const int samples = std::max(1, static_cast<int>(std::ceil(distance / 16.0f)));
	for (int sample = 1; sample < samples; ++sample)
	{
		const float fraction = static_cast<float>(sample) /
			static_cast<float>(samples);
		const Vector point = from + (to - from) * fraction;
		const Ground ground = findGround(session, point, point.z);
		if (!ground.valid || std::fabs(ground.feet.z - point.z) > 24.0f)
			return false;
	}
	return true;
}

bool traversable(Session &session, const Vector &from, const Vector &to)
{
	if (!finiteVector(from) || !finiteVector(to) ||
		std::fabs(to.z - from.z) > kMaxVerticalDelta || !canProbe(session))
		return false;

	const ICollideable *collideable = session.bot->getEdict()->GetCollideable();
	if (collideable == nullptr)
		return false;
	const Vector mins = collideable->OBBMins();
	const Vector maxs = collideable->OBBMaxs();
	trace_t trace{};
	UTIL_TraceHull(from, to, mins, maxs, MASK_PLAYERSOLID,
		session.bot->getEdict()->GetIServerEntity(),
		COLLISION_GROUP_PLAYER_MOVEMENT, &trace);
	bool clear = !trace.startsolid && !trace.allsolid && trace.fraction >= 1.0f;

	if (!clear && std::fabs(to.z - from.z) <= kMaxStepHeight && canProbe(session))
	{
		Vector liftedMins = mins;
		liftedMins.z = std::max(liftedMins.z, kMaxStepHeight + 0.1f);
		trace_t liftedTrace{};
		UTIL_TraceHull(from, to, liftedMins, maxs, MASK_PLAYERSOLID,
			session.bot->getEdict()->GetIServerEntity(),
			COLLISION_GROUP_PLAYER_MOVEMENT, &liftedTrace);
		clear = !liftedTrace.startsolid && !liftedTrace.allsolid &&
			liftedTrace.fraction >= 1.0f;
	}

	return clear && groundCorridor(session, from, to);
}

TraversalType traversalType(const Vector &from, const Vector &to)
{
	if (to.z - from.z > 2.0f)
		return TraversalType::STEP_UP;
	if (from.z - to.z > 2.0f)
		return TraversalType::STEP_DOWN;
	return TraversalType::WALK;
}

const char *traversalTypeName(const TraversalType type)
{
	switch (type)
	{
	case TraversalType::STEP_UP: return "STEP_UP";
	case TraversalType::STEP_DOWN: return "STEP_DOWN";
	default: return "WALK";
	}
}

void countPathEdges(const Session &session, int &walk, int &stepUp,
	int &stepDown)
{
	walk = 0;
	stepUp = 0;
	stepDown = 0;
	for (const TraversalType type : session.pathEdges)
	{
		switch (type)
		{
		case TraversalType::STEP_UP: ++stepUp; break;
		case TraversalType::STEP_DOWN: ++stepDown; break;
		default: ++walk; break;
		}
	}
}

NodeKey makeKey(const Session &session, const Vector &point)
{
	return {
		static_cast<int>(std::lround((point.x - session.start.x) /
			session.config.cellSize)),
		static_cast<int>(std::lround((point.y - session.start.y) /
			session.config.cellSize)),
		static_cast<int>(std::lround((point.z - session.start.z) / 8.0f))
	};
}

int findNode(const Session &session, const NodeKey &key)
{
	for (std::size_t index = 0; index < session.nodes.size(); ++index)
		if (session.nodes[index].key == key)
			return static_cast<int>(index);
	return -1;
}

float score(const Node &node)
{
	return node.g + node.h;
}

int takeBestOpen(Session &session)
{
	int best = -1;
	for (const int index : session.open)
	{
		if (index < 0 || static_cast<std::size_t>(index) >= session.nodes.size() ||
			!session.nodes[index].open)
			continue;
		if (best < 0 || score(session.nodes[index]) < score(session.nodes[best]) ||
			(score(session.nodes[index]) == score(session.nodes[best]) && index < best))
			best = index;
	}
	if (best >= 0)
		session.nodes[best].open = false;
	return best;
}

int addNode(Session &session, const Vector &point, const Ground &ground,
	const int parent, const float g, const bool enqueue)
{
	const NodeKey key = makeKey(session, point);
	const int existing = findNode(session, key);
	if (existing >= 0)
	{
		Node &node = session.nodes[existing];
		if (g < node.g)
		{
			node.g = g;
			node.parent = parent;
			if (parent >= 0 && static_cast<std::size_t>(parent) < session.nodes.size())
				node.incoming = traversalType(session.nodes[parent].point, node.point);
			if (!node.open && node.closed)
			{
				node.closed = false;
				node.open = true;
				session.open.push_back(existing);
			}
		}
		return existing;
	}
	if (static_cast<int>(session.nodes.size()) >= session.config.maxNodes)
		return -1;

	Node node;
	node.key = key;
	node.point = point;
	node.supportEntity = ground.supportEntity;
	node.parent = parent;
	if (parent >= 0 && static_cast<std::size_t>(parent) < session.nodes.size())
		node.incoming = traversalType(session.nodes[parent].point, point);
	node.g = g;
	node.h = (session.requestedGoal - point).Length2D();
	node.open = enqueue;
	session.nodes.push_back(node);
	const int index = static_cast<int>(session.nodes.size()) - 1;
	if (enqueue)
		session.open.push_back(index);
	return index;
}

void finishBotMovement(Session &session, const Result result, const char *reason)
{
	if (session.bot == nullptr)
		return;
	if (session.bot->hasActiveNavigationIntent() &&
		session.bot->getNavigationIntentId() == session.intentId)
	{
		if (result == Result::LOCAL_UNREACHABLE ||
			result == Result::LOCAL_BUDGET_EXHAUSTED ||
			result == Result::LOCAL_SEGMENT_FAILED)
		{
			const bool structural = result != Result::LOCAL_SEGMENT_FAILED;
			const RCBotNavigation::FailureReason failureReason =
				result == Result::LOCAL_UNREACHABLE ?
					RCBotNavigation::FailureReason::NO_PATH :
				result == Result::LOCAL_BUDGET_EXHAUSTED ?
					RCBotNavigation::FailureReason::PROVIDER_UNAVAILABLE :
					RCBotNavigation::FailureReason::PATH_STUCK;
			const RCBotNavigation::FailureDisposition disposition =
				session.bot->handleNavigationProviderFailure(
					RCBotNavigation::Provider::LOCAL_EXPLORER,
					structural ? RCBotNavigation::FailureClass::STRUCTURAL_PLANNING :
						RCBotNavigation::FailureClass::MOVEMENT,
					failureReason,
					result == Result::LOCAL_SEGMENT_FAILED ?
						RCBotNavigation::Result::PATH_STUCK :
						result == Result::LOCAL_UNREACHABLE ?
							RCBotNavigation::Result::UNREACHABLE :
							RCBotNavigation::Result::PATH_FAILED,
					reason);
			if (disposition != RCBotNavigation::FailureDisposition::REJECTED)
				return;
		}
	}
	if (session.attemptStarted && session.bot->hasActiveNavigationAttempt() &&
		session.bot->getNavigationIntentId() == session.intentId)
	{
		const RCBotNavigation::Result navigationResult =
			result == Result::COMPLETE ? RCBotNavigation::Result::COMPLETE :
			result == Result::CANCELLED ? RCBotNavigation::Result::CANCELLED :
			RCBotNavigation::Result::PATH_FAILED;
		session.bot->finishNavigationAttempt(navigationResult, reason);
	}
	else if (session.bot->hasActiveNavigationIntent() &&
		session.bot->getNavigationIntentId() == session.intentId)
	{
		const RCBotNavigation::Result navigationResult =
			result == Result::CANCELLED ? RCBotNavigation::Result::CANCELLED :
			RCBotNavigation::Result::UNREACHABLE;
		session.bot->finishNavigationIntent(navigationResult, reason);
	}
}

void endSession(Session &session, const Result result, const char *reason)
{
	if (!session.active)
		return;
	if (result != Result::COMPLETE && result != Result::CANCELLED &&
		result != Result::LOCAL_PATH_FOUND)
	{
		char detail[320];
		std::snprintf(detail, sizeof(detail),
			"session_id=%llu intent_id=%llu result=%s reason=%s",
			static_cast<unsigned long long>(session.sessionId),
			static_cast<unsigned long long>(session.intentId),
			RCBotLocalExplorer::ResultName(result),
			reason != nullptr ? reason : "unspecified");
		event(session, "LOCAL_PATH_FAILED", detail);
	}

	char stats[512];
	int walkEdges = 0;
	int stepUpEdges = 0;
	int stepDownEdges = 0;
	countPathEdges(session, walkEdges, stepUpEdges, stepDownEdges);
	std::snprintf(stats, sizeof(stats),
		"session_id=%llu intent_id=%llu nodes=%d valid_nodes=%d edges=%d probes=%d expansions=%d frames=%d path_points=%d walk_edges=%d step_up_edges=%d step_down_edges=%d result=%s",
		static_cast<unsigned long long>(session.sessionId),
		static_cast<unsigned long long>(session.intentId),
		static_cast<int>(session.nodes.size()), static_cast<int>(session.nodes.size()),
		static_cast<int>(session.pathEdges.size()), session.probes, session.expansions,
		session.frames,
		static_cast<int>(session.path.size()),
		walkEdges, stepUpEdges, stepDownEdges,
		RCBotLocalExplorer::ResultName(result));
	event(session, "LOCAL_NODE_STATS", stats);
	finishBotMovement(session, result, reason);

	char end[320];
	std::snprintf(end, sizeof(end),
		"session_id=%llu intent_id=%llu result=%s reason=%s",
		static_cast<unsigned long long>(session.sessionId),
		static_cast<unsigned long long>(session.intentId),
		RCBotLocalExplorer::ResultName(result),
		reason != nullptr ? reason : "unspecified");
	event(session, "LOCAL_EXPLORE_END", end);
	session.result = result;
	session.active = false;
}

bool withinRadius(const Session &session, const Vector &point)
{
	return (point - session.requestedStart).Length2D() <= session.config.radius;
}

void reconstructPath(Session &session)
{
	session.path.clear();
	session.pathEdges.clear();
	int current = session.goalNode;
	std::vector<int> nodePath;
	while (current >= 0 && static_cast<std::size_t>(current) < session.nodes.size())
	{
		nodePath.push_back(current);
		if (current == 0)
			break;
		current = session.nodes[current].parent;
	}
	if (nodePath.empty() || current != 0)
	{
		return;
	}
	std::reverse(nodePath.begin(), nodePath.end());
	for (const int node : nodePath)
		session.path.push_back(session.nodes[node].point);
	for (std::size_t index = 1; index < nodePath.size(); ++index)
		session.pathEdges.push_back(session.nodes[nodePath[index]].incoming);
}

bool connectGoal(Session &session, const int from)
{
	if (session.goalNode < 0 || from < 0 || from == session.goalNode ||
		session.nodes[from].closed == false)
		return false;
	Node &goal = session.nodes[session.goalNode];
	if (!traversable(session, session.nodes[from].point, goal.point))
		return false;
	goal.parent = from;
	goal.incoming = traversalType(session.nodes[from].point, goal.point);
	goal.g = session.nodes[from].g +
		(session.nodes[from].point - goal.point).Length2D();
	reconstructPath(session);
	return !session.path.empty();
}

void pathFound(Session &session)
{
	if (session.path.empty() || (session.path.size() == 1 &&
		(session.requestedGoal - session.requestedStart).Length() > kGoalDistance))
	{
		endSession(session, Result::LOCAL_UNREACHABLE,
			session.path.empty() ? "empty-path" : "missing-transition");
		return;
	}
	session.result = Result::LOCAL_PATH_FOUND;
	int walkEdges = 0;
	int stepUpEdges = 0;
	int stepDownEdges = 0;
	countPathEdges(session, walkEdges, stepUpEdges, stepDownEdges);
	const char *firstEdge = session.pathEdges.empty() ? "NONE" :
		traversalTypeName(session.pathEdges.front());
	char detail[512];
	std::snprintf(detail, sizeof(detail),
		"session_id=%llu intent_id=%llu nodes=%d valid_nodes=%d edges=%d probes=%d expansions=%d path_points=%d walk_edges=%d step_up_edges=%d step_down_edges=%d first_edge=%s start_support=%s goal_support=%s",
		static_cast<unsigned long long>(session.sessionId),
		static_cast<unsigned long long>(session.intentId),
		static_cast<int>(session.nodes.size()), static_cast<int>(session.nodes.size()),
		static_cast<int>(session.pathEdges.size()), session.probes, session.expansions,
		static_cast<int>(session.path.size()),
		walkEdges, stepUpEdges, stepDownEdges, firstEdge,
		supportName(session.nodes.front().supportEntity),
		supportName(session.nodes[session.goalNode].supportEntity));
	event(session, "LOCAL_GRAPH_READY", detail);
	event(session, "LOCAL_PATH_FOUND", detail);

	if (!session.bot->setNavigationIntentState(
		RCBotNavigation::IntentState::PLANNING, "local-path-found"))
	{
		endSession(session, Result::LOCAL_SEGMENT_FAILED, "intent-invalidated");
		return;
	}
	const RCBotNavigation::SegmentKey firstSegment{0, 1};
	const float routeLength = session.path.size() > 1 ?
		(session.path.back() - session.path.front()).Length() : 0.0f;
	if (!session.bot->beginNavigationAttempt(
		RCBotNavigation::Provider::LOCAL_EXPLORER, session.goalId,
		session.requestedGoal, firstSegment, routeLength))
	{
		endSession(session, Result::LOCAL_SEGMENT_FAILED,
			"attempt-owner-rejected");
		return;
	}
	session.attemptStarted = true;
	session.attemptId = session.bot->getNavigationAttemptId();
	session.result = Result::LOCAL_PATH_FOUND;
}

void expand(Session &session, const int current)
{
	static const int directions[][2] = {
		{1, 0}, {0, 1}, {-1, 0}, {0, -1},
		{1, 1}, {-1, 1}, {-1, -1}, {1, -1}
	};
	Node &source = session.nodes[current];
	for (const auto &direction : directions)
	{
		if (!session.active ||
			session.probes >= session.config.maxProbes ||
			static_cast<int>(session.nodes.size()) >= session.config.maxNodes)
			return;

		const Vector candidate = source.point + Vector(
			static_cast<float>(direction[0]) * session.config.cellSize,
			static_cast<float>(direction[1]) * session.config.cellSize, 0.0f);
		if (!withinRadius(session, candidate))
			continue;
		Ground ground;
		if (!standable(session, candidate, &ground))
			continue;
		if (!withinRadius(session, ground.feet) ||
			!traversable(session, source.point, ground.feet))
			continue;

		const int node = addNode(session, ground.feet, ground, current,
			source.g + (ground.feet - source.point).Length(), true);
		if (node < 0)
			return;
		if (session.goalNode < 0 &&
			(ground.feet - session.requestedGoal).Length2D() <= kGoalDistance)
		{
			if (node == 0 &&
				(session.requestedGoal - session.start).Length() > kGoalDistance)
				continue;
			session.goalNode = node;
			reconstructPath(session);
			if (!session.path.empty())
				return;
		}
	}
}

void explore(Session &session)
{
	if (engine == nullptr || engine->Time() >
		session.startedAt + session.config.maxDuration)
	{
		endSession(session, Result::LOCAL_BUDGET_EXHAUSTED,
			"exploration-time-budget");
		return;
	}
	if (!session.graphReady)
	{
		session.graphReady = true;
		char detail[320];
		std::snprintf(detail, sizeof(detail),
			"session_id=%llu intent_id=%llu nodes=%d probes=%d direct_goal=%s",
			static_cast<unsigned long long>(session.sessionId),
			static_cast<unsigned long long>(session.intentId),
			static_cast<int>(session.nodes.size()), session.probes,
			session.goalNode >= 0 ? "candidate" : "deferred");
		event(session, "LOCAL_GRAPH_READY", detail);
	}

	for (int work = 0; work < session.config.maxWorkPerFrame && session.active;
		++work)
	{
		if (session.expansions >= session.config.maxExpansions ||
			session.probes >= session.config.maxProbes ||
			static_cast<int>(session.nodes.size()) >= session.config.maxNodes)
		{
			endSession(session, Result::LOCAL_BUDGET_EXHAUSTED,
				"exploration-budget");
			return;
		}

		const int current = takeBestOpen(session);
		if (current < 0)
		{
			endSession(session, Result::LOCAL_UNREACHABLE, "frontier-empty");
			return;
		}
		session.nodes[current].closed = true;
		++session.expansions;

		if (session.goalNode >= 0 && connectGoal(session, current))
		{
			pathFound(session);
			return;
		}
		expand(session, current);
		if (session.goalNode >= 0 && !session.path.empty())
		{
			pathFound(session);
			return;
		}
	}
}

void follow(Session &session)
{
	if (session.bot == nullptr || !session.bot->inUse() ||
		!session.bot->isAlive() || session.bot->getTeam() != session.team ||
		session.bot->getEdict() == nullptr ||
		session.bot->getEdict()->GetCollideable() == nullptr ||
		!session.bot->hasActiveNavigationIntent() ||
		session.bot->getNavigationIntentId() != session.intentId)
	{
		endSession(session, Result::CANCELLED, "lifecycle-invalidated");
		return;
	}
	if (engine == nullptr)
		return;
	if (engine->Time() > session.startedAt + session.config.maxDuration)
	{
		if (!session.segmentFailureReported)
		{
			event(session, "LOCAL_SEGMENT_FAIL", "route-timeout");
			session.segmentFailureReported = true;
		}
		endSession(session, Result::LOCAL_SEGMENT_FAILED, "route-timeout");
		return;
	}

	if (session.path.size() <= 1 || session.segment + 1 >=
		static_cast<int>(session.path.size()))
	{
		endSession(session, Result::COMPLETE, "goal-reached");
		return;
	}

	const Vector target = session.path[static_cast<std::size_t>(session.segment + 1)];
	const Vector delta = target - session.bot->getOrigin();
	if (delta.Length2D() <= 12.0f && std::fabs(delta.z) <= 24.0f)
	{
		++session.segment;
		const RCBotNavigation::SegmentKey segment{
			session.segment,
			session.segment + 1};
		const float remaining = (session.path.back() - session.bot->getOrigin()).Length();
		if (session.bot->observeNavigationAttempt(remaining, segment) ==
			RCBotNavigation::Observation::NO_PROGRESS)
		{
			if (!session.segmentFailureReported)
			{
				event(session, "LOCAL_SEGMENT_FAIL", "no-progress");
				session.segmentFailureReported = true;
			}
			endSession(session, Result::LOCAL_SEGMENT_FAILED, "no-progress");
			return;
		}
		if (session.segment + 1 >= static_cast<int>(session.path.size()))
		{
			endSession(session, Result::COMPLETE, "goal-reached");
			return;
		}
	}

	const RCBotNavigation::SegmentKey segment{
		session.segment, session.segment + 1};
	const float remaining = (session.path.back() - session.bot->getOrigin()).Length();
	if (session.bot->observeNavigationAttempt(remaining, segment) ==
		RCBotNavigation::Observation::NO_PROGRESS)
	{
		if (!session.segmentFailureReported)
		{
			event(session, "LOCAL_SEGMENT_FAIL", "no-progress");
			session.segmentFailureReported = true;
		}
		endSession(session, Result::LOCAL_SEGMENT_FAILED, "no-progress");
		return;
	}

	if ((session.bot->getOrigin() - session.lastProgressPosition).Length2D() >= 8.0f)
	{
		session.lastProgressPosition = session.bot->getOrigin();
		session.lastProgressAt = engine->Time();
	}
	if (!session.bot->setNavigationMoveTarget(session.intentId,
		RCBotNavigation::Provider::LOCAL_EXPLORER, session.attemptId, target))
	{
		if (!session.segmentFailureReported)
		{
			event(session, "LOCAL_SEGMENT_FAIL", "stale-or-rejected-target");
			session.segmentFailureReported = true;
		}
		endSession(session, Result::LOCAL_SEGMENT_FAILED,
			"stale-or-rejected-target");
	}
}

bool startSession(Session &session, CBot *bot, const Vector &goal,
	const Config &requestedConfig, const bool adoptIntent,
	const std::uint64_t intentId)
{
	if (bot == nullptr || !bot->inUse() || !bot->isAlive() ||
		bot->getEdict() == nullptr || bot->getEdict()->GetCollideable() == nullptr ||
		!finiteVector(goal) || engine == nullptr)
		return false;

	session = Session{};
	session.bot = bot;
	session.config = boundedConfig(requestedConfig);
	session.requestedStart = bot->getOrigin();
	session.requestedGoal = goal;
	session.team = bot->getTeam();
	session.sessionId = ++g_nextSessionId;
	if (session.sessionId == 0)
		session.sessionId = ++g_nextSessionId;
	if (adoptIntent)
	{
		const RCBotNavigation::NavigationIntent& intent =
			bot->getNavigationIntent();
		if (!intent.active || intent.id != intentId ||
			intent.provider != RCBotNavigation::Provider::LOCAL_EXPLORER ||
			intent.owner != RCBotNavigation::IntentOwner::LOCAL_EXPLORER ||
			intent.goalOrigin != goal)
		{
			session.active = false;
			return false;
		}
		session.goalId = intent.goalId;
		session.intentId = intentId;
	}
	else
	{
		session.goalId = g_nextGoalId++;
		if (g_nextGoalId <= 0)
			g_nextGoalId = 100000;
	}
	session.startedAt = engine->Time();
	session.lastProgressAt = session.startedAt;
	session.lastProgressPosition = session.requestedStart;
	session.result = Result::EXPLORING;
	session.active = true;
	session.nodes.reserve(static_cast<std::size_t>(session.config.maxNodes));
	session.open.reserve(static_cast<std::size_t>(session.config.maxNodes));
	session.path.reserve(static_cast<std::size_t>(session.config.maxNodes));
	session.pathEdges.reserve(static_cast<std::size_t>(session.config.maxNodes));

	if (!adoptIntent && !bot->beginLocalExplorerIntent(session.goalId, goal))
	{
		session.active = false;
		return false;
	}
	if (!adoptIntent)
		session.intentId = bot->getNavigationIntentId();
	bot->setNavigationIntentState(RCBotNavigation::IntentState::PLANNING,
		"local-explore-start");

	char startDetail[384];
	std::snprintf(startDetail, sizeof(startDetail),
		"session_id=%llu intent_id=%llu goal=(%.1f %.1f %.1f) radius=%.1f cell=%.1f max_nodes=%d max_probes=%d max_expansions=%d max_work_per_frame=%d",
		static_cast<unsigned long long>(session.sessionId),
		static_cast<unsigned long long>(session.intentId), goal.x, goal.y, goal.z,
		session.config.radius, session.config.cellSize, session.config.maxNodes,
		session.config.maxProbes, session.config.maxExpansions,
		session.config.maxWorkPerFrame);
	event(session, "LOCAL_EXPLORE_START", startDetail);
	if (!withinRadius(session, goal))
	{
		endSession(session, Result::LOCAL_UNREACHABLE, "goal-outside-radius");
		return true;
	}

	Ground startGround;
	if (!standable(session, session.requestedStart, &startGround))
	{
		endSession(session, Result::LOCAL_UNREACHABLE, "start-not-standable");
		return true;
	}
	session.start = startGround.feet;
	const int startNode = addNode(session, session.start, startGround, -1, 0.0f, true);
	if (startNode < 0)
	{
		endSession(session, Result::LOCAL_BUDGET_EXHAUSTED, "start-node-budget");
		return true;
	}

	Ground goalGround;
	if (standable(session, goal, &goalGround))
	{
		const int goalNode = addNode(session, goalGround.feet, goalGround, -1,
			0.0f, false);
		if (goalNode != startNode ||
			(session.requestedGoal - session.start).Length() <= kGoalDistance)
			session.goalNode = goalNode;
	}
	if (session.goalNode >= 0 && traversable(session, session.start,
		session.nodes[session.goalNode].point))
	{
		session.nodes[session.goalNode].parent = startNode;
		session.nodes[session.goalNode].g =
		(session.nodes[session.goalNode].point - session.start).Length();
		reconstructPath(session);
	}
	return true;
}
}

namespace RCBotLocalExplorer
{
const char *ResultName(const Result result)
{
	switch (result)
	{
	case Result::EXPLORING: return "EXPLORING";
	case Result::LOCAL_PATH_FOUND: return "LOCAL_PATH_FOUND";
	case Result::LOCAL_UNREACHABLE: return "LOCAL_UNREACHABLE";
	case Result::LOCAL_BUDGET_EXHAUSTED: return "LOCAL_BUDGET_EXHAUSTED";
	case Result::CANCELLED: return "CANCELLED";
	case Result::LOCAL_SEGMENT_FAILED: return "LOCAL_SEGMENT_FAILED";
	case Result::COMPLETE: return "COMPLETE";
	default: return "NONE";
	}
}

bool Start(CBot *bot, const Vector &goal, const Config &config)
{
	if (bot == nullptr)
		return false;
	Session *existing = findSession(bot);
	if (existing != nullptr && existing->active)
		Cancel(bot, "new-local-explorer-request");
	if (existing == nullptr)
	{
		if (static_cast<int>(g_sessions.size()) >= kMaxSessions)
			return false;
		g_sessions.emplace_back();
		existing = &g_sessions.back();
	}
	return startSession(*existing, bot, goal, config, false, 0);
}

bool StartForIntent(CBot *bot, const std::uint64_t intentId,
	const Vector &goal, const Config &config)
{
	if (bot == nullptr || !bot->hasActiveNavigationIntent())
		return false;
	const RCBotNavigation::NavigationIntent& intent =
		bot->getNavigationIntent();
	if (intent.id != intentId ||
		intent.provider != RCBotNavigation::Provider::LOCAL_EXPLORER ||
		intent.owner != RCBotNavigation::IntentOwner::LOCAL_EXPLORER ||
		intent.goalOrigin != goal)
		return false;

	Session *existing = findSession(bot);
	if (existing != nullptr && existing->active)
		Cancel(bot, "provider-route-replaced");
	if (existing == nullptr)
	{
		if (static_cast<int>(g_sessions.size()) >= kMaxSessions)
			return false;
		g_sessions.emplace_back();
		existing = &g_sessions.back();
	}
	return startSession(*existing, bot, goal, config, true, intentId);
}

bool Cancel(CBot *bot, const char *reason)
{
	Session *session = findSession(bot);
	if (session == nullptr || !session->active)
		return false;
	endSession(*session, Result::CANCELLED,
		reason != nullptr ? reason : "explicit-cancel");
	return true;
}

bool IsActive(const CBot *bot)
{
	const Session *session = findSession(bot);
	return session != nullptr && session->active;
}

bool GetStatus(const CBot *bot, Status *status)
{
	if (status == nullptr)
		return false;
	const Session *session = findSession(bot);
	if (session == nullptr)
		return false;
	status->active = session->active;
	status->result = session->result;
	status->sessionId = session->sessionId;
	status->intentId = session->intentId;
	status->attemptId = session->attemptId;
	status->goalId = session->goalId;
	status->nodes = static_cast<int>(session->nodes.size());
	status->probes = session->probes;
	status->expansions = session->expansions;
	status->pathPoints = static_cast<int>(session->path.size());
	status->segment = session->segment;
	countPathEdges(*session, status->walkEdges, status->stepUpEdges,
		status->stepDownEdges);
	status->frames = session->frames;
	return true;
}

void Frame()
{
	for (Session &session : g_sessions)
	{
		if (!session.active)
			continue;
		++session.frames;
		if (session.attemptStarted)
			follow(session);
		else
			explore(session);
	}
}

void LevelShutdown()
{
	for (Session &session : g_sessions)
		if (session.active)
			endSession(session, Result::CANCELLED, "map-shutdown");
	g_sessions.clear();
}
}
