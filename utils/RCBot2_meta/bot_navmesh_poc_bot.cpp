/* Isolated test-only Bot runner for the Empires NavMesh spike. */
#include "bot_navmesh_api.h"
#include "bot_navmesh_poc.h"

#include <cstdlib>
#include <vector>

#include "bot.h"
#include "bot_schedule.h"
#include "bot_task.h"

namespace
{
int s_barracksBotUserId = -1;
Vector s_barracksBotTarget = vec3_origin;
bool s_barracksBotRouteActive = false;
std::vector<Vector> s_barracksBotPoints;
std::size_t s_barracksBotPoint = 0;
float s_barracksBotStartTime = 0.0f;
float s_barracksBotNextProgressLog = 0.0f;

CBot *FindBotByUserId(const int userId)
{
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot *bot = CBots::get(slot);
		if (bot == nullptr || !bot->inUse() || bot->getEdict() == nullptr)
			continue;

		if (engine->GetPlayerUserId(bot->getEdict()) == userId)
			return bot;
	}

	return nullptr;
}

void ResetBarracksBotRoute()
{
	s_barracksBotUserId = -1;
	s_barracksBotTarget = vec3_origin;
	s_barracksBotRouteActive = false;
	s_barracksBotPoints.clear();
	s_barracksBotPoint = 0;
	s_barracksBotStartTime = 0.0f;
	s_barracksBotNextProgressLog = 0.0f;
}
}

namespace RCBotNavMeshPoc
{
void Frame()
{
	RecordHumanMovementFrame();

	// Keep the PoC-only dynamic building snapshot synchronized independently
	// of Commander/task execution. It is only consumed by the diagnostic path
	// builder and does not register with the native TheNavMesh.
	ReconcileBuildings();

	if (!s_barracksBotRouteActive)
		return;

	CBot *bot = FindBotByUserId(s_barracksBotUserId);
	if (bot == nullptr || !bot->isAlive())
	{
		Msg("[RCBOT2] NavMesh Barracks bot result=FAILED userid=%d reason=bot-not-alive\n",
			s_barracksBotUserId);
		ResetBarracksBotRoute();
		return;
	}

	if (s_barracksBotPoint + 1 < s_barracksBotPoints.size() &&
		bot->distanceFrom(s_barracksBotPoints[s_barracksBotPoint]) <= 64.0f)
	{
		++s_barracksBotPoint;
	}

	const float distance = bot->distanceFrom(s_barracksBotTarget);
	if (engine->Time() >= s_barracksBotNextProgressLog)
	{
		const Vector origin = bot->getOrigin();
		Msg("[RCBOT2] NavMesh Barracks bot progress userid=%d point=%zu/%zu origin=(%.1f %.1f %.1f) distance=%.1f\n",
			s_barracksBotUserId, s_barracksBotPoint,
			s_barracksBotPoints.size(), origin.x, origin.y, origin.z,
			distance);
		s_barracksBotNextProgressLog = engine->Time() + 5.0f;
	}

	if (distance <= 64.0f)
	{
		Msg("[RCBOT2] NavMesh Barracks bot result=COMPLETED userid=%d final_distance=%.1f\n",
			s_barracksBotUserId, distance);
		ResetBarracksBotRoute();
		return;
	}

	if (engine->Time() - s_barracksBotStartTime > 90.0f)
	{
		Msg("[RCBOT2] NavMesh Barracks bot result=FAILED userid=%d reason=timeout final_distance=%.1f\n",
			s_barracksBotUserId, distance);
		ResetBarracksBotRoute();
		return;
	}

	// The runner remains inside the spike, but uses the normal bot command
	// path. Override priority is limited to the currently selected PoC point.
	bot->setMoveLookPriority(MOVELOOK_OVERRIDE);
	bot->setMoveTo(s_barracksBotPoints[s_barracksBotPoint]);
}

void LevelShutdown()
{
	ResetBarracksBotRoute();
	ResetBuildings();
}
}

CON_COMMAND(rcbot_navmesh_barracks_bot,
	"Test-only: route a live bot to a target using NavMesh centers: userid x y z")
{
	if (args.ArgC() != 5)
	{
		Msg("Usage: rcbot_navmesh_barracks_bot userid x y z\n");
		return;
	}

	const int userId = std::atoi(args.Arg(1));
	CBot *bot = FindBotByUserId(userId);
	if (bot == nullptr || !bot->isAlive())
	{
		Msg("[RCBOT2] NavMesh Barracks bot result=FAILED userid=%d reason=bot-not-alive\n",
			userId);
		return;
	}

	const Vector target(
		static_cast<float>(std::atof(args.Arg(2))),
		static_cast<float>(std::atof(args.Arg(3))),
		static_cast<float>(std::atof(args.Arg(4))));
	RCBotNavMesh::Path path;
	const RCBotNavMesh::PathResult result =
		RCBotNavMesh::BuildPath(bot->getOrigin(), target, path);
	if (result != RCBotNavMesh::PathResult::SUCCESS)
	{
		Msg("[RCBOT2] NavMesh Barracks bot result=FAILED userid=%d reason=%s\n",
			userId, RCBotNavMesh::PathResultName(result));
		return;
	}

	CBotSchedules *schedules = bot->getSchedule();
	if (schedules == nullptr)
	{
		Msg("[RCBOT2] NavMesh Barracks bot result=FAILED userid=%d reason=no-schedule\n",
			userId);
		return;
	}

	CBotSchedule *route = new CBotSchedule();
	for (std::size_t i = 1; i < path.points.size(); ++i)
		route->addTask(new CMoveToTask(path.points[i]));

	schedules->addFront(route);
	s_barracksBotUserId = userId;
	s_barracksBotTarget = target;
	s_barracksBotPoints = path.points;
	s_barracksBotPoint = path.points.size() > 1 ? 1 : 0;
	s_barracksBotStartTime = engine->Time();
	s_barracksBotNextProgressLog = s_barracksBotStartTime;
	s_barracksBotRouteActive = true;

	Msg("[RCBOT2] NavMesh Barracks bot result=ARMED userid=%d path_points=%zu from=(%.1f %.1f %.1f) to=(%.1f %.1f %.1f)\n",
		userId, path.points.size(), bot->getOrigin().x, bot->getOrigin().y,
		bot->getOrigin().z, target.x, target.y, target.z);
	for (std::size_t i = 0; i < path.points.size(); ++i)
	{
		Msg("[RCBOT2] NavMesh Barracks bot point[%zu]=(%.1f %.1f %.1f)\n",
			i, path.points[i].x, path.points[i].y, path.points[i].z);
	}
}

CON_COMMAND(rcbot_navmesh_building_bot,
	"Test-only: route a live bot with the dynamic-building NavMesh PoC: userid x y z")
{
	if (args.ArgC() != 5)
	{
		Msg("Usage: rcbot_navmesh_building_bot userid x y z\n");
		return;
	}

	const int userId = std::atoi(args.Arg(1));
	CBot *bot = FindBotByUserId(userId);
	if (bot == nullptr || !bot->isAlive())
	{
		Msg("[RCBOT2] NavMesh building bot result=FAILED userid=%d reason=bot-not-alive\n",
			userId);
		return;
	}

	const Vector target(
		static_cast<float>(std::atof(args.Arg(2))),
		static_cast<float>(std::atof(args.Arg(3))),
		static_cast<float>(std::atof(args.Arg(4))));
	RCBotNavMesh::Path path;
	if (!RCBotNavMeshPoc::BuildBuildingAwarePath(bot->getOrigin(), target, &path))
	{
		Msg("[RCBOT2] NavMesh building bot result=FAILED userid=%d "
			"reason=no-building-aware-path\n", userId);
		return;
	}

	CBotSchedules *schedules = bot->getSchedule();
	if (schedules == nullptr)
	{
		Msg("[RCBOT2] NavMesh building bot result=FAILED userid=%d reason=no-schedule\n",
			userId);
		return;
	}

	CBotSchedule *route = new CBotSchedule();
	for (std::size_t i = 1; i < path.points.size(); ++i)
		route->addTask(new CMoveToTask(path.points[i]));
	schedules->addFront(route);

	s_barracksBotUserId = userId;
	s_barracksBotTarget = target;
	s_barracksBotPoints = path.points;
	s_barracksBotPoint = path.points.size() > 1 ? 1 : 0;
	s_barracksBotStartTime = engine->Time();
	s_barracksBotNextProgressLog = s_barracksBotStartTime;
	s_barracksBotRouteActive = true;

	Msg("[RCBOT2] NavMesh building bot result=ARMED userid=%d path_points=%zu "
		"path_length=%.1f from=(%.1f %.1f %.1f) to=(%.1f %.1f %.1f)\n",
		userId, path.points.size(), path.length, bot->getOrigin().x,
		bot->getOrigin().y, bot->getOrigin().z, target.x, target.y, target.z);
	for (std::size_t i = 0; i < path.points.size(); ++i)
	{
		Msg("[RCBOT2] NavMesh building bot point[%zu]=(%.1f %.1f %.1f)\n",
			i, path.points[i].x, path.points[i].y, path.points[i].z);
	}
}
