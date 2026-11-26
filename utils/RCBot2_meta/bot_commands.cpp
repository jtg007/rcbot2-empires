// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
/*
 *    This file is part of RCBot.
 *
 *    RCBot by Paul Murphy adapted from Botman's HPB Bot 2 template.
 *
 *    RCBot is free software; you can redistribute it and/or modify it
 *    under the terms of the GNU General Public License as published by the
 *    Free Software Foundation; either version 2 of the License, or (at
 *    your option) any later version.
 *
 *    RCBot is distributed in the hope that it will be useful, but
 *    WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with RCBot; if not, write to the Free Software Foundation,
 *    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    In addition, as a special exception, the author gives permission to
 *    link the code of this program with the Half-Life Game Engine ("HL
 *    Engine") and Modified Game Libraries ("MODs") developed by Valve,
 *    L.L.C ("Valve").  You must obey the GNU General Public License in all
 *    respects for all of the code used other than the HL Engine and MODs
 *    from Valve.  If you modify this file, you may extend this exception
 *    to your version of the file, but you are not obligated to do so.  If
 *    you do not wish to do so, delete this exception statement from your
 *    version.
 *
 */
#include "bot_commands.h"
#include "bot.h"
#include "bot_accessclient.h"
#include "bot_client.h"
#include "bot_cvars.h"
#include "bot_empires.h"
#include "bot_empires_team_director.h"
#include "bot_empires_barracks_execution.h"
#include "bot_commander_move_test_fixture.h"
#include "bot_empires_telemetry.h"
#include "bot_getprop.h"
#include "bot_globals.h"
#include "bot_menu.h"
#include "bot_navigator.h"
#include "bot_navmesh_provider.h"
#include "bot_navmesh_api.h"
#include "bot_navigation_arbitration.h"
#include "bot_local_explorer.h"
#include "bot_schedule.h"
#include "bot_strings.h"
#include "bot_waypoint.h"           // for waypoint commands
#include "bot_waypoint_locations.h" // for waypoint commands
#include "bot_waypoint_visibility.h"
#include "bot_weapons.h"
#include "ndebugoverlay.h"
#include "server_class.h"
#include "tier1/convar.h"

#include "bot_tf2_points.h"

#include <cctype>
#ifdef __linux__
#include <dlfcn.h>
#endif
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

extern IVDebugOverlay* debugoverlay;
extern IServerTools* servertools;
float EmpiresFactoryExitYaw(const Vector& at, int team, bool quiet);

// include our subcommands
#include "rcbot_subcmds/config.cpp"
#include "rcbot_subcmds/debug.cpp"
#include "rcbot_subcmds/pathwaypoint.cpp"
#include "rcbot_subcmds/users.cpp"
#include "rcbot_subcmds/util.cpp"
#include "rcbot_subcmds/waypoint.cpp"

// temporarily declared at the bottom
// CBotSubcommands *CBotGlobals :: m_pCommands;

namespace
{
	bool parseEmpiresPathWaypointArgument(const char* value, int& waypointId)
	{
		if (value == nullptr || *value == '\0')
			return false;

		char* end = nullptr;
		const long parsed = std::strtol(value, &end, 10);
		if (end == value || *end != '\0' || parsed < 0 || parsed > INT_MAX)
			return false;

		waypointId = static_cast<int>(parsed);
		return true;
	}

	bool parseNavigationPolicyArgument(const char* value,
		RCBotNavigation::NavigationPolicy& policy)
	{
		if (value == nullptr)
			return false;
		if (std::strcmp(value, "AUTO") == 0 ||
			std::strcmp(value, "auto") == 0)
		{
			policy = RCBotNavigation::NavigationPolicy::AUTO;
			return true;
		}
		if (std::strcmp(value, "WAYPOINT_ONLY") == 0 ||
			std::strcmp(value, "waypoint_only") == 0)
		{
			policy = RCBotNavigation::NavigationPolicy::WAYPOINT_ONLY;
			return true;
		}
		if (std::strcmp(value, "NAVMESH_ONLY") == 0 ||
			std::strcmp(value, "navmesh_only") == 0)
		{
			policy = RCBotNavigation::NavigationPolicy::NAVMESH_ONLY;
			return true;
		}
		return false;
	}

	CBot* findNormalEmpiresBotByUserId(const int userId)
	{
		if (userId < 0)
			return nullptr;

		edict_t* pEdict = CBotGlobals::playerByUserId(userId);
		CBot* pBot = pEdict != nullptr ? CBots::getBotPointer(pEdict) : nullptr;
		return pBot != nullptr && !pBot->isControlledTestOnly() ? pBot : nullptr;
	}

	CBot* findActiveNavigationTestBot()
	{
		for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
		{
			CBot* pBot = CBots::getBot(slot);
			if (pBot != nullptr && !pBot->isControlledTestOnly() &&
				pBot->hasActiveNavigationIntent())
				return pBot;
		}
		return nullptr;
	}

	edict_t* findNearestEmpiresBuilding(CBot* bot, const char* className)
	{
		if (bot == nullptr || className == nullptr || servertools == nullptr ||
			engine == nullptr || bot->getTeam() != EMP_TEAM_NF)
			return nullptr;

		edict_t* nearest = nullptr;
		float nearestDistance = (std::numeric_limits<float>::max)();
		for (CBaseEntity* entity = servertools->FirstEntity(); entity != nullptr;
			entity = servertools->NextEntity(entity))
		{
			IServerUnknown* unknown = reinterpret_cast<IServerUnknown*>(entity);
			IServerNetworkable* networkable = unknown != nullptr ?
				unknown->GetNetworkable() : nullptr;
			edict_t* candidate = networkable != nullptr ?
				networkable->GetEdict() : nullptr;
			if (candidate == nullptr || candidate->IsFree() ||
				candidate->GetClassName() == nullptr ||
				std::strcmp(candidate->GetClassName(), className) != 0)
				continue;
			const ICollideable* collideable = candidate->GetCollideable();
			if (collideable == nullptr || collideable->GetSolid() == SOLID_NONE)
				continue;
			const float distance =
				(collideable->GetCollisionOrigin() -
					bot->getOrigin()).LengthSqr();
			if (distance < nearestDistance)
			{
				nearestDistance = distance;
				nearest = candidate;
			}
		}
		return nearest;
	}

	bool localBuildingGoal(edict_t* building, Vector& goal)
	{
		if (building == nullptr || building->GetCollideable() == nullptr)
			return false;
		const ICollideable* collideable = building->GetCollideable();
		const Vector origin = collideable->GetCollisionOrigin();
		const Vector mins = origin + collideable->OBBMins();
		const Vector maxs = origin + collideable->OBBMaxs();
		if (!mins.IsValid() || !maxs.IsValid() || maxs.x <= mins.x ||
			maxs.y <= mins.y || maxs.z <= mins.z)
			return false;
		// Geometry comes from the live entity. The command selects only the
		// building class; it never embeds a map coordinate or entity index.
		goal = Vector((mins.x + maxs.x) * 0.5f,
			(mins.y + maxs.y) * 0.5f, mins.z + 1.0f);
		return goal.IsValid();
	}

	CBot* localExplorerBot(const int userId, const bool activeOnly)
	{
		CBot* bot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) : nullptr;
		if (bot == nullptr && !activeOnly)
			bot = CBots::getFirstNormalBot();
		if (bot == nullptr && activeOnly)
		{
			for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
			{
				CBot* candidate = CBots::getBot(slot);
				if (candidate != nullptr && RCBotLocalExplorer::IsActive(candidate))
				{
					bot = candidate;
					break;
				}
			}
		}
		return bot != nullptr && !bot->isControlledTestOnly() ? bot : nullptr;
	}

	int normalEmpiresBotUserId(const CBot* pBot)
	{
		return pBot != nullptr && engine != nullptr && pBot->getEdict() != nullptr
			? engine->GetPlayerUserId(pBot->getEdict()) : -1;
	}

	std::size_t meaningfulBotCommandArgCount(const BotCommandArgs& args)
	{
		std::size_t count = 0;
		for (const char* value : args)
			if (value != nullptr && *value != '\0')
				++count;
		return count;
	}

	bool parseEmpiresFixtureUserIdArgument(const char* value, int& userId)
	{
		if (value == nullptr || *value == '\0')
			return false;

		char* end = nullptr;
		const long parsed = std::strtol(value, &end, 10);
		if (end == value || *end != '\0' || parsed < 0 || parsed > INT_MAX)
			return false;

		userId = static_cast<int>(parsed);
		return true;
	}

	bool parseEmpiresFixtureDelayArgument(const char* value, float& seconds)
	{
		if (value == nullptr || *value == '\0')
			return false;

		char* end = nullptr;
		const float parsed = std::strtof(value, &end);
		if (end == value || *end != '\0' || !std::isfinite(parsed) ||
			parsed < 0.0f || parsed > 8.0f)
		{
			return false;
		}

		seconds = parsed;
		return true;
	}

	bool parseEmpiresCommanderMoveCoordinate(const char* value, float& coordinate)
	{
		if (value == nullptr || *value == '\0')
			return false;

		char* end = nullptr;
		const float parsed = std::strtof(value, &end);
		if (end == value || *end != '\0' || !std::isfinite(parsed) ||
			parsed < -32768.0f || parsed > 32768.0f)
		{
			return false;
		}

		coordinate = parsed;
		return true;
	}
	std::string analysisMarkText(const char* raw)
	{
		std::string text = raw != nullptr ? raw : "";
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
			text.erase(text.begin());
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
			text.pop_back();
		if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
			text = text.substr(1, text.size() - 2);
		return text;
	}

	std::string analysisMarkText(const BotCommandArgs& args)
	{
		std::string text;
		for (const char* arg : args)
		{
			if (arg == nullptr)
				continue;
			if (!text.empty())
				text += ' ';
			text += arg;
		}
		return analysisMarkText(text.c_str());
	}

	edict_t* analysisOutputEntity(const CClient* client)
	{
		return client != nullptr ? client->getPlayer() : nullptr;
	}

	const char* analysisMapName()
	{
		return gpGlobals != nullptr ? gpGlobals->mapname.ToCStr() : "unknown";
	}

	eBotCommandResult analysisStart(CClient* client)
	{
		const bool started = EmpiresAnalysis::Start(analysisMapName());
		CBotGlobals::botMessage(analysisOutputEntity(client), 0,
			started ? "Empires analysis started session=%s path=%s" :
			"Empires analysis start failed: %s",
			started ? EmpiresAnalysis::GetSessionId() : EmpiresAnalysis::GetLastError(),
			started ? EmpiresAnalysis::GetSessionPath() : "");
		return started ? COMMAND_ACCESSED : COMMAND_ERROR;
	}

	eBotCommandResult analysisStop(CClient* client)
	{
		const bool stopped = EmpiresAnalysis::Stop();
		CBotGlobals::botMessage(analysisOutputEntity(client), 0,
			stopped ? "Empires analysis stopped session=%s duration=%.2f events=%d bots=%d path=%s" :
			"Empires analysis stop failed: %s",
			stopped ? EmpiresAnalysis::GetSessionId() : EmpiresAnalysis::GetLastError(),
			stopped ? EmpiresAnalysis::GetDurationSeconds() : 0.0,
			stopped ? EmpiresAnalysis::GetEventCount() : 0,
			stopped ? EmpiresAnalysis::GetBotCount() : 0,
			stopped ? EmpiresAnalysis::GetSessionPath() : "");
		return stopped ? COMMAND_ACCESSED : COMMAND_ERROR;
	}

	eBotCommandResult analysisMark(CClient* client, const std::string& text)
	{
		if (text.empty())
		{
			CBotGlobals::botMessage(analysisOutputEntity(client), 0,
				"usage: rcbot_analysis_mark <text>");
			return COMMAND_ERROR;
		}
		if (!EmpiresAnalysis::IsActive())
		{
			CBotGlobals::botMessage(analysisOutputEntity(client), 0,
				"Empires analysis mark failed: analysis session is not active");
			return COMMAND_ERROR;
		}
		EmpiresAnalysis::Mark(text.c_str());
		CBotGlobals::botMessage(analysisOutputEntity(client), 0,
			"Empires analysis marker recorded: %s", text.c_str());
		return COMMAND_ACCESSED;
	}

	eBotCommandResult analysisStatus(CClient* client)
	{
		CBotGlobals::botMessage(analysisOutputEntity(client), 0,
			"Empires analysis active=%d session=%s path=%s duration=%.2f events=%d bots=%d%s%s",
			EmpiresAnalysis::IsActive() ? 1 : 0,
			EmpiresAnalysis::GetSessionId(), EmpiresAnalysis::GetSessionPath(),
			EmpiresAnalysis::GetDurationSeconds(), EmpiresAnalysis::GetEventCount(),
			EmpiresAnalysis::GetBotCount(),
			EmpiresAnalysis::GetLastError()[0] != '\0' ? " error=" : "",
			EmpiresAnalysis::GetLastError());
		return COMMAND_ACCESSED;
	}
}

eBotCommandResult CBotCommandInline::execute(CClient* pClient, const BotCommandArgs& args) {
	// fire off callback function
	return m_Callback(pClient, args);
}

CON_COMMAND(rcbot_analysis_start, "start an Empires gameplay analysis session")
{
	analysisStart(nullptr);
}

CON_COMMAND(rcbot_analysis_stop, "stop the Empires gameplay analysis session")
{
	analysisStop(nullptr);
}

CON_COMMAND(rcbot_analysis_mark, "record a marker in the Empires analysis session")
{
	analysisMark(nullptr, analysisMarkText(args.ArgS()));
}

CON_COMMAND(rcbot_analysis_status, "show Empires gameplay analysis status")
{
	analysisStatus(nullptr);
}

CBotCommandInline EmpiresAnalysisStartCommand("analysis_start", CMD_ACCESS_CONSOLE_ONLY,
	[](CClient* client, const BotCommandArgs&) { return analysisStart(client); },
	"start an Empires gameplay analysis session");

CBotCommandInline EmpiresAnalysisStopCommand("analysis_stop", CMD_ACCESS_CONSOLE_ONLY,
	[](CClient* client, const BotCommandArgs&) { return analysisStop(client); },
	"stop the Empires gameplay analysis session");

CBotCommandInline EmpiresAnalysisMarkCommand("analysis_mark", CMD_ACCESS_CONSOLE_ONLY,
	[](CClient* client, const BotCommandArgs& args)
	{
		return analysisMark(client, analysisMarkText(args));
	}, "record a marker in the Empires analysis session");

CBotCommandInline EmpiresAnalysisStatusCommand("analysis_status", CMD_ACCESS_CONSOLE_ONLY,
	[](CClient* client, const BotCommandArgs&) { return analysisStatus(client); },
	"show Empires gameplay analysis status");

CBotCommandInline ControlCommand("control", CMD_ACCESS_BOT | CMD_ACCESS_DEDICATED, [](const CClient* pClient,
	const BotCommandArgs& args)
	{
		edict_t* pEntity = nullptr;

		if (pClient)
			pEntity = pClient->getPlayer();
		if (args[0] && *args[0])
		{
			if (CBots::controlBot(args[0], args[0], args[2], args[3]))
				CBotGlobals::botMessage(pEntity, 0, "bot added");
			else
				CBotGlobals::botMessage(pEntity, 0, "error: couldn't control bot '%s'", args[0]);

			return COMMAND_ACCESSED;

		}
		return COMMAND_ERROR;
	});

CBotCommandInline AddBotCommand("addbot", CMD_ACCESS_BOT | CMD_ACCESS_DEDICATED, [](const CClient* pClient,
	const BotCommandArgs& args)
{
//	bool bOkay = false;

	edict_t* pEntity = nullptr;

	if (pClient)
		pEntity = pClient->getPlayer();

	// That breaks the bot quota system? [APG]RoboCop[CL]
	/*if (rcbot_bot_quota_interval.GetFloat() > 0) {
		CBotGlobals::botMessage(pEntity, 0, "error: cannot manually add bot while rcbot_bot_quota_interval is active");
		return COMMAND_ACCESSED;
	}*/
	
	//if ( !bot_sv_cheat_warning.GetBool() || bot_sv_cheats_auto.GetBool() || (!sv_cheats || sv_cheats->GetBool()) )
	//{
		//if ( !args[0] || !*args[0] )
		//	bOkay = CBots::createBot();
		//else
		//bOkay = CBots::createBot();

	if (CBots::createBot(args[0], args[1], args[2])) {
		CBotGlobals::botMessage(pEntity, 0, "bot adding...");
	}
	else {
		CBotGlobals::botMessage(pEntity, 0, "error: couldn't create bot! (Check maxplayers)");
	}

	//}
	//else
	//	CBotGlobals::botMessage(pEntity,0,"error: sv_cheats must be 1 to add bots");

	return COMMAND_ACCESSED;
	});

CBotCommandInline KickBotCommand("kickbot", CMD_ACCESS_BOT | CMD_ACCESS_DEDICATED, [](CClient* pClient,
	const BotCommandArgs& args)
{
	if (!args[0] || !*args[0])
	{
		if (rcbot_nonrandom_kicking.GetBool()) {
			CBots::kickChosenBot();
		} else {
			CBots::kickRandomBot();
		}
	}
	else
	{
		const int team = std::atoi(args[0]);

		if (rcbot_nonrandom_kicking.GetBool()) {
			CBots::kickChosenBotOnTeam(team);
		} else {
			CBots::kickRandomBotOnTeam(team);
		}
	}

	return COMMAND_ACCESSED;
}, R"(usage "kickbot" or "kickbot <team>" : kicks random bot or bot on team: <team>)");

CBotCommandInline EmpiresBootstrapCreateCommand("empires_bootstrap_create", CMD_ACCESS_BOT | CMD_ACCESS_DEDICATED,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if (CBots::createEmpiresBootstrapBot())
			CBotGlobals::botMessage(pEntity, 0, "Empires bootstrap fake client created and attached");
		else
			CBotGlobals::botMessage(pEntity, 0, "error: Empires bootstrap fake client was not created");

		return COMMAND_ACCESSED;
	}, R"(usage "empires_bootstrap_create" : creates one non-gameplay Empires bootstrap fake client)");

CBotCommandInline EmpiresBootstrapRemoveCommand("empires_bootstrap_remove", CMD_ACCESS_BOT | CMD_ACCESS_DEDICATED,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if (CBots::removeEmpiresBootstrapBot())
			CBotGlobals::botMessage(pEntity, 0, "Empires bootstrap fake client removal requested");
		else
			CBotGlobals::botMessage(pEntity, 0, "error: no removable Empires bootstrap fake client");

		return COMMAND_ACCESSED;
	}, R"(usage "empires_bootstrap_remove" : disconnects the controlled Empires bootstrap fake client)");

CBotCommandInline EmpiresNormalPathOnceCommand("empires_normal_path_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotMod* pMod = CBotGlobals::getCurrentMod();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_normal_path_once requires the Empires mod");
			return COMMAND_ACCESSED;
		}

		const bool noArguments = args[0] == nullptr || *args[0] == '\0';
		const bool trailingParserSentinel = args.size() == 3 &&
			(args[2] == nullptr || *args[2] == '\0');
		const bool validArgumentCount = noArguments ?
			(args.size() == 0 || args.size() == 1 || (args.size() == 2 &&
				(args[1] == nullptr || *args[1] == '\0'))) :
			(args.size() == 2 || trailingParserSentinel);
		int startWaypoint = -1;
		int destinationWaypoint = -1;
		if (!validArgumentCount || (!noArguments &&
			(!parseEmpiresPathWaypointArgument(args[0], startWaypoint) ||
			 !parseEmpiresPathWaypointArgument(args[1], destinationWaypoint))))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_normal_path_once [start_waypoint destination_waypoint]");
			return COMMAND_ACCESSED;
		}

		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_normal_path_once requires one normal RCBot2 bot; controlled probes are ignored");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestNormalPathOnce(startWaypoint, destinationWaypoint, true))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires normal path_once armed: normal CBot::think/RunPlayerMove route %s to %d; no generic roam after stop",
				startWaypoint >= 0 ? args[0] : "nearest current waypoint",
				destinationWaypoint >= 0 ? destinationWaypoint : 155);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_normal_path_once was rejected; the normal bot must be alive after Empires spawn");
		}

		return COMMAND_ACCESSED;
	}, R"(usage "empires_normal_path_once [start_waypoint destination_waypoint]" : arms one normal Empires bot for a bounded existing A* route using regular CBot::think and RunPlayerMove; no arguments use the nearest current waypoint to 155)");

	CBotCommandInline EmpiresAnchorResolverProbeCommand(
	"empires_anchor_resolver_probe", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool pairArguments = args.size() == 2 ||
			(args.size() == 3 && (args[2] == nullptr || *args[2] == '\0'));
		if (pairArguments)
		{
			int startWaypoint = -1;
			int goalWaypoint = -1;
			if (!parseEmpiresPathWaypointArgument(args[0], startWaypoint) ||
				!parseEmpiresPathWaypointArgument(args[1], goalWaypoint))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_anchor_resolver_probe <start_waypoint> <goal_waypoint>");
				return COMMAND_ACCESSED;
			}

			CBotMod* pMod = CBotGlobals::getCurrentMod();
			CBot* pBaseBot = CBots::getFirstNormalBot();
			CWaypoint* pStart = CWaypoints::getWaypoint(startWaypoint);
			CWaypoint* pGoal = CWaypoints::getWaypoint(goalWaypoint);
			if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
				pBaseBot == nullptr || pBaseBot->getNavigator() == nullptr ||
				pStart == nullptr || pGoal == nullptr)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: empires_anchor_resolver_probe pair requires one normal Empires bot and two loaded waypoints");
				return COMMAND_ACCESSED;
			}

			CWaypointNavigator* pNavigator = static_cast<CWaypointNavigator*>(
				pBaseBot->getNavigator());
			const RouteAnchorStatus status = pNavigator->probeDisconnectedAnchorsForTest(
				startWaypoint, goalWaypoint);
			CBotGlobals::botMessage(pEntity, 0,
				"Empires anchor resolver probe status=%s start_waypoint=%d goal_waypoint=%d start=(%.1f %.1f %.1f) goal=(%.1f %.1f %.1f) movement_started=0",
				routeAnchorStatusName(status), startWaypoint, goalWaypoint,
				pStart->getOrigin().x, pStart->getOrigin().y,
				pStart->getOrigin().z, pGoal->getOrigin().x,
				pGoal->getOrigin().y, pGoal->getOrigin().z);
			return COMMAND_ACCESSED;
		}

		const bool validArgumentCount = args.size() == 3 || args.size() == 4 ||
			(args.size() == 5 && (args[4] == nullptr || *args[4] == '\0'));
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		int goalWaypoint = -1;
		if (!validArgumentCount ||
			!parseEmpiresCommanderMoveCoordinate(args[0], x) ||
			!parseEmpiresCommanderMoveCoordinate(args[1], y) ||
			!parseEmpiresCommanderMoveCoordinate(args[2], z) ||
			(args.size() >= 4 && args[3] != nullptr && *args[3] != '\0' &&
				!parseEmpiresPathWaypointArgument(args[3], goalWaypoint)))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_anchor_resolver_probe <x> <y> <z> [goal_waypoint]");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr || pBaseBot->getNavigator() == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_anchor_resolver_probe requires one normal Empires bot with a navigator");
			return COMMAND_ACCESSED;
		}

		IBotNavigator* pNavigator = pBaseBot->getNavigator();
		const RouteAnchorStatus status = pNavigator->prepareRouteAnchors(
			pBaseBot->getOrigin(), Vector(x, y, z), goalWaypoint);
		CBotGlobals::botMessage(pEntity, 0,
			"Empires anchor resolver probe status=%s goal_request=%d target=(%.1f %.1f %.1f) movement_started=0",
			routeAnchorStatusName(status), goalWaypoint, x, y, z);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_anchor_resolver_probe <x> <y> <z> [goal_waypoint]" : test-only bounded waypoint anchor resolution; never starts movement)");

	CBotCommandInline EmpiresNavMeshProviderOnceCommand(
		"empires_navmesh_provider_once", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			if (argumentCount < 3 || argumentCount > 4)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navmesh_provider_once <x> <y> <z> [bot_userid]");
				return COMMAND_ACCESSED;
			}

			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			int userId = -1;
			if (!parseEmpiresCommanderMoveCoordinate(args[0], x) ||
				!parseEmpiresCommanderMoveCoordinate(args[1], y) ||
				!parseEmpiresCommanderMoveCoordinate(args[2], z) ||
				(argumentCount == 4 &&
					!parseEmpiresFixtureUserIdArgument(args[3], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navmesh_provider_once <x> <y> <z> [bot_userid]");
				return COMMAND_ACCESSED;
			}

			CBotMod* pMod = CBotGlobals::getCurrentMod();
			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				CBots::getFirstNormalBot();
			if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
				pBot == nullptr || !pBot->isAlive())
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: empires_navmesh_provider_once requires one alive normal Empires bot");
				return COMMAND_ACCESSED;
			}

			const Vector goal(x, y, z);
			const bool started = RCBotNavMeshProvider::Start(pBot, goal);
			CBotGlobals::botMessage(pEntity, 0,
				started ?
					"Empires NavMesh provider started bot_userid=%d goal=(%.1f %.1f %.1f)" :
					"Empires NavMesh provider rejected bot_userid=%d goal=(%.1f %.1f %.1f)",
				normalEmpiresBotUserId(pBot), goal.x, goal.y, goal.z);
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navmesh_provider_once <x> <y> <z> [bot_userid]" : test-only native Source NavMesh route through NavigationIntent ownership and normal CBot locomotion)");

	CBotCommandInline EmpiresNavMeshProviderStatusCommand(
		"empires_navmesh_provider_status", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navmesh_provider_status [bot_userid]");
				return COMMAND_ACCESSED;
			}

			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				CBots::getFirstNormalBot();
			if (pBot == nullptr)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: no normal Empires bot available");
				return COMMAND_ACCESSED;
			}

			const RCBotNavigation::NavigationIntent& intent =
				pBot->getNavigationIntent();
			CBotGlobals::botMessage(pEntity, 0,
				"Empires NavMesh provider status active=%d intent_active=%d intent_id=%llu attempt_id=%llu state=%d move_to_valid=%d bot_userid=%d",
				RCBotNavMeshProvider::IsActive(pBot) ? 1 : 0,
				intent.active ? 1 : 0,
				static_cast<unsigned long long>(intent.id),
				static_cast<unsigned long long>(intent.attemptId),
				static_cast<int>(intent.state), pBot->moveToIsValid() ? 1 : 0,
				normalEmpiresBotUserId(pBot));
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navmesh_provider_status [bot_userid]" : test-only reports the native NavMesh provider and shared navigation ownership state)");

	CBotCommandInline EmpiresNavMeshProviderCancelCommand(
		"empires_navmesh_provider_cancel", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navmesh_provider_cancel [bot_userid]");
				return COMMAND_ACCESSED;
			}

			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				findActiveNavigationTestBot();
			if (pBot == nullptr)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: no active NavMesh provider bot available");
				return COMMAND_ACCESSED;
			}

			const bool cancelled = RCBotNavMeshProvider::Cancel(
				pBot, "test-command-cancel");
			CBotGlobals::botMessage(pEntity, 0,
				"Empires NavMesh provider cancel bot_userid=%d cancelled=%d",
				normalEmpiresBotUserId(pBot), cancelled ? 1 : 0);
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navmesh_provider_cancel [bot_userid]" : test-only cancels the native NavMesh route and releases shared navigation ownership)");

	CBotCommandInline EmpiresNavigationRequestCommand(
		"empires_navigation_request", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			const bool validArgumentCount = argumentCount >= 3 && argumentCount <= 5;
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			RCBotNavigation::NavigationPolicy policy =
				RCBotNavigation::NavigationPolicy::AUTO;
			int userId = -1;
			if (!validArgumentCount ||
				!parseEmpiresCommanderMoveCoordinate(args[0], x) ||
				!parseEmpiresCommanderMoveCoordinate(args[1], y) ||
				!parseEmpiresCommanderMoveCoordinate(args[2], z) ||
				(argumentCount >= 4 &&
					!parseNavigationPolicyArgument(args[3], policy)) ||
				(argumentCount == 5 &&
					!parseEmpiresFixtureUserIdArgument(args[4], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navigation_request <x> <y> <z> [AUTO|WAYPOINT_ONLY|NAVMESH_ONLY] [bot_userid]");
				return COMMAND_ACCESSED;
			}

			CBotMod* pMod = CBotGlobals::getCurrentMod();
			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				CBots::getFirstNormalBot();
			if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
				pBot == nullptr || !pBot->isAlive())
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: empires_navigation_request requires one alive normal Empires bot");
				return COMMAND_ACCESSED;
			}

			const Vector goal(x, y, z);
			const bool accepted = pBot->requestNavigation(goal, policy);
			CBotGlobals::botMessage(pEntity, 0,
				"Empires navigation request policy=%s accepted=%d bot_userid=%d goal=(%.1f %.1f %.1f)",
				RCBotNavigation::NavigationPolicyName(policy), accepted ? 1 : 0,
				normalEmpiresBotUserId(pBot), goal.x, goal.y, goal.z);
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navigation_request <x> <y> <z> [AUTO|WAYPOINT_ONLY|NAVMESH_ONLY] [bot_userid]" : test-only starts the provider-arbitrated normal navigation intent)");

	CBotCommandInline EmpiresNavigationNavMeshFixtureCommand(
		"empires_navigation_navmesh_fixture_once", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navigation_navmesh_fixture_once [bot_userid]");
				return COMMAND_ACCESSED;
			}

			CBotMod* pMod = CBotGlobals::getCurrentMod();
			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				findActiveNavigationTestBot();
			CWaypointNavigator* pWaypointNavigator = pBot != nullptr &&
				pBot->getNavigator() != nullptr ?
				static_cast<CWaypointNavigator*>(pBot->getNavigator()) : nullptr;
			if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
				pBot == nullptr || pWaypointNavigator == nullptr || !pBot->isAlive())
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: empires_navigation_navmesh_fixture_once requires one alive normal Empires bot with a Waypoint navigator");
				return COMMAND_ACCESSED;
			}

			const Vector goal = pBot->getOrigin() + Vector(128.0f, 0.0f, 0.0f);
			const bool accepted = pBot->requestNavigation(goal,
				RCBotNavigation::NavigationPolicy::AUTO);
			if (accepted)
				pWaypointNavigator->armTestStructuralFailureForNextRoute();
			CBotGlobals::botMessage(pEntity, 0,
				"Empires navigation NavMesh fixture accepted=%d bot_userid=%d goal=(%.1f %.1f %.1f) waypoint_fixture_pending=%d",
				accepted ? 1 : 0, normalEmpiresBotUserId(pBot),
				goal.x, goal.y, goal.z,
				pWaypointNavigator->isTestStructuralFailureFixtureArmed() ? 1 : 0);
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navigation_navmesh_fixture_once [bot_userid]" : test-only forces one structural WAYPOINT failure before a real short AUTO NAVMESH route)");

	CBotCommandInline EmpiresNavigationBarracksOnceCommand(
		"empires_navigation_barracks_once", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navigation_barracks_once [bot_userid]");
				return COMMAND_ACCESSED;
			}

			CBotMod* pMod = CBotGlobals::getCurrentMod();
			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				CBots::getFirstNormalBot();
			edict_t* pBarracks = findNearestEmpiresBuilding(pBot,
			"emp_building_nf_barracks");
			Vector goal;
			if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
				pBot == nullptr || !pBot->isAlive() ||
				!localBuildingGoal(pBarracks, goal))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: empires_navigation_barracks_once requires one alive normal Empires bot and a live team NF Barracks");
				return COMMAND_ACCESSED;
			}

			const bool accepted = pBot->requestNavigation(goal,
				RCBotNavigation::NavigationPolicy::AUTO);
			CBotGlobals::botMessage(pEntity, 0,
				"Empires navigation Barracks request accepted=%d bot_userid=%d building_index=%d building_class=%s goal=(%.1f %.1f %.1f) policy=AUTO source=live-collision-bounds",
				accepted ? 1 : 0, normalEmpiresBotUserId(pBot),
				engine->IndexOfEdict(pBarracks), pBarracks->GetClassName(),
				goal.x, goal.y, goal.z);
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navigation_barracks_once [bot_userid]" : test-only starts AUTO arbitration toward the nearest live team NF Barracks selected by class and live collision bounds)");

	CBotCommandInline EmpiresNavigationIntentOnceCommand(
		"empires_navigation_intent_once", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const bool validArgumentCount = args.size() == 1 ||
				(args.size() == 2 && (args[1] == nullptr || *args[1] == '\0'));
			int goalWaypoint = -1;
			if (!validArgumentCount ||
				!parseEmpiresPathWaypointArgument(args[0], goalWaypoint))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navigation_intent_once <goal_waypoint>");
				return COMMAND_ACCESSED;
			}

			CBotMod* pMod = CBotGlobals::getCurrentMod();
			CBot* pBot = CBots::getFirstNormalBot();
			if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
				pBot == nullptr || pBot->getNavigator() == nullptr)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: empires_navigation_intent_once requires one normal Empires bot with a navigator");
				return COMMAND_ACCESSED;
			}

			pBot->forceGotoWaypoint(goalWaypoint);
			CBotGlobals::botMessage(pEntity, 0,
				"Empires navigation intent test armed: goal_waypoint=%d bot_userid=%d source=CFindPathTask",
				goalWaypoint, normalEmpiresBotUserId(pBot));
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navigation_intent_once <goal_waypoint>" : test-only real normal Waypoint CFindPathTask intent lifecycle)");

	CBotCommandInline EmpiresNavigationIntentWriterProbeCommand(
		"empires_navigation_intent_writer_probe", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navigation_intent_writer_probe [bot_userid]");
				return COMMAND_ACCESSED;
			}

			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				findActiveNavigationTestBot();
		if (pBot == nullptr)
		{
				CBotGlobals::botMessage(pEntity, 0,
					"error: no normal bot available");
				return COMMAND_ACCESSED;
			}

			const bool attempted = pBot->probeLegacyNavigationMoveWriterForTest();
			CBotGlobals::botMessage(pEntity, 0,
				"Empires navigation writer probe attempted=%d owner_active=%d bot_userid=%d",
				attempted ? 1 : 0, pBot->hasActiveNavigationIntent() ? 1 : 0,
				normalEmpiresBotUserId(pBot));
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navigation_intent_writer_probe [bot_userid]" : test-only attempts a conflicting legacy setMoveTo while Waypoint navigation owns movement)");

	CBotCommandInline EmpiresNavigationIntentStaleWriterProbeCommand(
		"empires_navigation_stale_writer_probe", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			const bool validArgumentCount = argumentCount == 1 || argumentCount == 2;
			char* end = nullptr;
			unsigned long long intentId = 0;
			int userId = -1;
			bool validUserId = true;
			if (validArgumentCount && args[0] != nullptr && *args[0] != '\0')
				intentId = std::strtoull(args[0], &end, 10);
			if (validArgumentCount && argumentCount == 2 &&
				!parseEmpiresFixtureUserIdArgument(args[1], userId))
				validUserId = false;
			if (!validArgumentCount || end == args[0] || *end != '\0' || intentId == 0 ||
				!validUserId)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navigation_stale_writer_probe <intent_id> [bot_userid]");
				return COMMAND_ACCESSED;
			}

			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				CBots::getFirstNormalBot();
			if (pBot == nullptr)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: no normal bot available");
				return COMMAND_ACCESSED;
			}

			const bool accepted = pBot->setNavigationMoveTarget(
			static_cast<std::uint64_t>(intentId),
				pBot->getOrigin() + Vector(64.0f, 0.0f, 0.0f));
			CBotGlobals::botMessage(pEntity, 0,
				"Empires navigation stale writer probe intent_id=%llu accepted=%d active=%d bot_userid=%d",
				intentId, accepted ? 1 : 0,
				pBot->hasActiveNavigationIntent() ? 1 : 0,
				normalEmpiresBotUserId(pBot));
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navigation_stale_writer_probe <intent_id> [bot_userid]" : test-only verifies an old owner token cannot write after terminal cleanup)");

	CBotCommandInline EmpiresNavigationIntentStatusCommand(
		"empires_navigation_intent_status", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navigation_intent_status [bot_userid]");
				return COMMAND_ACCESSED;
			}
			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				CBots::getFirstNormalBot();
			if (pBot == nullptr)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: no normal bot available");
				return COMMAND_ACCESSED;
			}

			const RCBotNavigation::NavigationIntent& intent =
				pBot->getNavigationIntent();
			const Vector moveTo = pBot->getMoveTo();
			const RCBotNavigation::ProviderFailureRecord& waypointFailure =
				intent.failureReasonPerProvider[0];
			const RCBotNavigation::ProviderFailureRecord& navmeshFailure =
				intent.failureReasonPerProvider[1];
			const RCBotNavigation::ProviderFailureRecord& localExplorerFailure =
				intent.failureReasonPerProvider[2];
			const CWaypointNavigator* pWaypointNavigator = pBot->getNavigator() != nullptr ?
				static_cast<const CWaypointNavigator*>(pBot->getNavigator()) : nullptr;
			CBotGlobals::botMessage(pEntity, 0,
				"Empires navigation intent status active=%d policy=%s provider=%d owner=%d intent_id=%llu attempt_id=%llu goal=%d state=%d providers_attempted=%u provider_switch_count=%u waypoint_failure=%s/%s navmesh_failure=%s/%s local_explorer_failure=%s/%s attempt_active=%d move_to_valid=%d move_to=(%.1f %.1f %.1f) waypoint_fixture_pending=%d bot_userid=%d",
				intent.active ? 1 : 0,
				RCBotNavigation::NavigationPolicyName(intent.policy),
				static_cast<int>(intent.provider),
				static_cast<int>(intent.owner),
				static_cast<unsigned long long>(intent.id),
				static_cast<unsigned long long>(intent.attemptId), intent.goalId,
				static_cast<int>(intent.state),
				static_cast<unsigned int>(intent.providersAttempted),
				static_cast<unsigned int>(intent.providerSwitchCount),
				waypointFailure.recorded ?
					RCBotNavigation::FailureClassName(waypointFailure.classification) : "NONE",
				waypointFailure.recorded ?
					RCBotNavigation::FailureReasonName(waypointFailure.reason) : "NONE",
				navmeshFailure.recorded ?
					RCBotNavigation::FailureClassName(navmeshFailure.classification) : "NONE",
				navmeshFailure.recorded ?
					RCBotNavigation::FailureReasonName(navmeshFailure.reason) : "NONE",
				localExplorerFailure.recorded ?
					RCBotNavigation::FailureClassName(localExplorerFailure.classification) : "NONE",
				localExplorerFailure.recorded ?
					RCBotNavigation::FailureReasonName(localExplorerFailure.reason) : "NONE",
				pBot->hasActiveNavigationAttempt() ? 1 : 0,
				pBot->moveToIsValid() ? 1 : 0, moveTo.x, moveTo.y, moveTo.z,
				pWaypointNavigator != nullptr &&
					pWaypointNavigator->isTestStructuralFailureFixtureArmed() ? 1 : 0,
				normalEmpiresBotUserId(pBot));
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navigation_intent_status [bot_userid]" : test-only reports intent, owner, attempt and target cleanup state)");

	CBotCommandInline EmpiresNavigationIntentCancelCommand(
		"empires_navigation_intent_cancel", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_navigation_intent_cancel [bot_userid]");
				return COMMAND_ACCESSED;
			}
			CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
				findActiveNavigationTestBot();
			if (pBot == nullptr)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: no normal bot available");
				return COMMAND_ACCESSED;
			}

			pBot->cancelNavigationAttempt("test-command-cancel");
			CBotGlobals::botMessage(pEntity, 0,
				"Empires navigation intent test cancel issued bot_userid=%d",
				normalEmpiresBotUserId(pBot));
			return COMMAND_ACCESSED;
		}, R"(usage "empires_navigation_intent_cancel [bot_userid]" : test-only cancels the active navigation intent and releases movement)");

	CBotCommandInline EmpiresLocalExplorerRequestCommand(
		"empires_local_explorer_request", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			int userId = -1;
			if (argumentCount < 3 || argumentCount > 4 ||
				!parseEmpiresCommanderMoveCoordinate(args[0], x) ||
				!parseEmpiresCommanderMoveCoordinate(args[1], y) ||
				!parseEmpiresCommanderMoveCoordinate(args[2], z) ||
				(argumentCount == 4 &&
					!parseEmpiresFixtureUserIdArgument(args[3], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_local_explorer_request <x> <y> <z> [bot_userid]");
				return COMMAND_ACCESSED;
			}
			CBotMod* pMod = CBotGlobals::getCurrentMod();
			CBot* pBot = localExplorerBot(userId, false);
			if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
				pBot == nullptr || !pBot->isAlive())
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: local explorer requires one alive normal Empires bot");
				return COMMAND_ACCESSED;
			}
			const bool accepted = RCBotLocalExplorer::Start(pBot, Vector(x, y, z));
		CBotGlobals::botMessage(pEntity, 0,
				"Empires local explorer request accepted=%d bot_userid=%d goal=(%.1f %.1f %.1f)",
				accepted ? 1 : 0, normalEmpiresBotUserId(pBot), x, y, z);
			return COMMAND_ACCESSED;
		}, R"(usage "empires_local_explorer_request <x> <y> <z> [bot_userid]" : test-only starts a bounded local hull/ground graph explorer; it is not an AUTO provider)");

	CBotCommandInline EmpiresLocalExplorerBarracksCommand(
		"empires_local_explorer_barracks_once", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_local_explorer_barracks_once [bot_userid]");
				return COMMAND_ACCESSED;
			}
			CBot* pBot = localExplorerBot(userId, false);
			edict_t* pBarracks = findNearestEmpiresBuilding(pBot,
				"emp_building_nf_barracks");
			Vector goal;
			if (pBot == nullptr || !pBot->isAlive() ||
				!localBuildingGoal(pBarracks, goal))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: local explorer Barracks test requires an alive normal bot and a live team NF Barracks");
				return COMMAND_ACCESSED;
			}
			const bool accepted = RCBotLocalExplorer::Start(pBot, goal);
			CBotGlobals::botMessage(pEntity, 0,
				"Empires local explorer Barracks accepted=%d bot_userid=%d building_index=%d building_class=%s goal=(%.1f %.1f %.1f)",
				accepted ? 1 : 0, normalEmpiresBotUserId(pBot),
				engine->IndexOfEdict(pBarracks), pBarracks->GetClassName(),
				goal.x, goal.y, goal.z);
			return COMMAND_ACCESSED;
		}, R"(usage "empires_local_explorer_barracks_once [bot_userid]" : test-only selects the nearest live team NF Barracks by class and derives its live collision bounds as the local-explorer goal)");

	CBotCommandInline EmpiresLocalExplorerBudgetCommand(
		"empires_local_explorer_budget_once", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			int userId = -1;
			if (argumentCount < 3 || argumentCount > 4 ||
				!parseEmpiresCommanderMoveCoordinate(args[0], x) ||
				!parseEmpiresCommanderMoveCoordinate(args[1], y) ||
				!parseEmpiresCommanderMoveCoordinate(args[2], z) ||
				(argumentCount == 4 &&
					!parseEmpiresFixtureUserIdArgument(args[3], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_local_explorer_budget_once <x> <y> <z> [bot_userid]");
				return COMMAND_ACCESSED;
			}
			CBot* pBot = localExplorerBot(userId, false);
			CBotMod* pMod = CBotGlobals::getCurrentMod();
			if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
				pBot == nullptr || !pBot->isAlive())
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: local explorer budget test requires one alive normal Empires bot");
				return COMMAND_ACCESSED;
			}
			RCBotLocalExplorer::Config config;
			config.maxNodes = 2;
			config.maxProbes = 8;
			config.maxExpansions = 1;
			config.maxWorkPerFrame = 1;
			const bool accepted = RCBotLocalExplorer::Start(
				pBot, Vector(x, y, z), config);
			CBotGlobals::botMessage(pEntity, 0,
				"Empires local explorer budget test accepted=%d bot_userid=%d goal=(%.1f %.1f %.1f) max_nodes=%d max_probes=%d max_expansions=%d",
				accepted ? 1 : 0, normalEmpiresBotUserId(pBot), x, y, z,
				config.maxNodes, config.maxProbes, config.maxExpansions);
			return COMMAND_ACCESSED;
		}, R"(usage "empires_local_explorer_budget_once <x> <y> <z> [bot_userid]" : test-only runs the local explorer with deliberately tiny hard budgets and expects LOCAL_BUDGET_EXHAUSTED)");

	CBotCommandInline EmpiresLocalExplorerUnreachableCommand(
		"empires_local_explorer_unreachable", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_local_explorer_unreachable [bot_userid]");
				return COMMAND_ACCESSED;
			}
			CBot* pBot = localExplorerBot(userId, false);
			if (pBot == nullptr || !pBot->isAlive())
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: local explorer unreachable test requires one alive normal bot");
				return COMMAND_ACCESSED;
			}
			const Vector goal = pBot->getOrigin() + Vector(2048.0f, 0.0f, 0.0f);
			const bool accepted = RCBotLocalExplorer::Start(pBot, goal);
			CBotGlobals::botMessage(pEntity, 0,
				"Empires local explorer unreachable test accepted=%d bot_userid=%d goal=(%.1f %.1f %.1f) reason=outside-bounded-radius",
				accepted ? 1 : 0, normalEmpiresBotUserId(pBot), goal.x, goal.y,
				goal.z);
			return COMMAND_ACCESSED;
		}, R"(usage "empires_local_explorer_unreachable [bot_userid]" : test-only requests a deterministic goal beyond the explorer radius and expects LOCAL_UNREACHABLE)");

	CBotCommandInline EmpiresLocalExplorerStatusCommand(
		"empires_local_explorer_status", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_local_explorer_status [bot_userid]");
				return COMMAND_ACCESSED;
			}
			CBot* pBot = localExplorerBot(userId, userId < 0);
			RCBotLocalExplorer::Status status;
			if (pBot == nullptr || !RCBotLocalExplorer::GetStatus(pBot, &status))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: no local explorer session available");
				return COMMAND_ACCESSED;
			}
			CBotGlobals::botMessage(pEntity, 0,
				"Empires local explorer status active=%d result=%s session_id=%llu intent_id=%llu attempt_id=%llu goal_id=%d nodes=%d valid_nodes=%d path_edges=%d probes=%d expansions=%d frames=%d path_points=%d segment=%d walk_edges=%d step_up_edges=%d step_down_edges=%d bot_userid=%d",
				status.active ? 1 : 0, RCBotLocalExplorer::ResultName(status.result),
				static_cast<unsigned long long>(status.sessionId),
				static_cast<unsigned long long>(status.intentId),
				static_cast<unsigned long long>(status.attemptId), status.goalId,
				status.nodes, status.nodes, status.pathPoints > 0 ? status.pathPoints - 1 : 0,
				status.probes, status.expansions, status.frames, status.pathPoints,
				status.segment, status.walkEdges, status.stepUpEdges,
				status.stepDownEdges, normalEmpiresBotUserId(pBot));
			return COMMAND_ACCESSED;
		}, R"(usage "empires_local_explorer_status [bot_userid]" : test-only reports bounded graph, lifecycle, intent and attempt state)");

	CBotCommandInline EmpiresLocalExplorerCancelCommand(
		"empires_local_explorer_cancel", CMD_ACCESS_CONSOLE_ONLY,
		[](const CClient* pClient, const BotCommandArgs& args)
		{
			edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
			const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
			int userId = -1;
			if (argumentCount > 1 || (argumentCount == 1 &&
				!parseEmpiresFixtureUserIdArgument(args[0], userId)))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"usage: rcbotd empires_local_explorer_cancel [bot_userid]");
				return COMMAND_ACCESSED;
			}
			CBot* pBot = localExplorerBot(userId, true);
			const bool cancelled = pBot != nullptr &&
				RCBotLocalExplorer::Cancel(pBot, "test-command-cancel");
			CBotGlobals::botMessage(pEntity, 0,
				"Empires local explorer cancel cancelled=%d bot_userid=%d",
				cancelled ? 1 : 0, normalEmpiresBotUserId(pBot));
			return COMMAND_ACCESSED;
		}, R"(usage "empires_local_explorer_cancel [bot_userid]" : test-only cancels exploration and releases the shared CBot movement owner)");

CBotCommandInline EmpiresVehicleScaleHpCommand("empires_vehicle_scalehp", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const float factor = args[0] != nullptr && *args[0] != '\0' ? std::atof(args[0]) : 0.3f;
		// Optional: four side factors for the armor (front rear left right).
		float sides[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		bool haveSides = true;
		for (int k = 0; k < 4; ++k)
		{
			if (args[k + 1] == nullptr || *args[k + 1] == '\0')
			{
				haveSides = false;
				break;
			}
			sides[k] = std::atof(args[k + 1]);
		}
		const int changed = EmpiresVehicleDebugScaleHealth(factor, haveSides ? sides : nullptr);
		CBotGlobals::botMessage(pEntity, 0, "empires_vehicle_scalehp factor=%.2f vehicles=%d", factor, changed);
		return COMMAND_ACCESSED;
	});

int EmpiresVehicleDebugSetAmmo(int magazine, int reserve); // bot_empires_vehicle.cpp

CBotCommandInline EmpiresVehicleSetAmmoCommand("empires_vehicle_setammo", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const int magazine = args[0] != nullptr && *args[0] != '\0' ? std::atoi(args[0]) : 0;
		const int reserve = args[1] != nullptr && *args[1] != '\0' ? std::atoi(args[1]) : 0;
		const int changed = EmpiresVehicleDebugSetAmmo(magazine, reserve);
		CBotGlobals::botMessage(pEntity, 0, "empires_vehicle_setammo magazine=%d reserve=%d vehicles=%d", magazine, reserve, changed);
		return COMMAND_ACCESSED;
	});

int EmpiresVehicleDebugCrate(float distance); // bot_empires_vehicle.cpp

CBotCommandInline EmpiresVehicleCrateCommand("empires_vehicle_crate", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const float distance = args[0] != nullptr && *args[0] != '\0' ? static_cast<float>(std::atof(args[0])) : 1500.0f;
		const int crate = EmpiresVehicleDebugCrate(distance);
		CBotGlobals::botMessage(pEntity, 0, "empires_vehicle_crate distance=%.0f crate=%d", distance, crate);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_vehicle_crate <distance>" : test-only, puts a team supply crate beside the first vehicle)");

bool EmpiresVehicleDebugObstacle(float ahead, float lateral, float entrySpeed, const char* model); // bot_empires_vehicle.cpp

CBotCommandInline EmpiresVehicleObstacleCommand("empires_vehicle_obstacle", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		auto number = [&](int index, float fallback)
		{
			return args[index] != nullptr && *args[index] != '\0' ? static_cast<float>(std::atof(args[index])) : fallback;
		};
		const float ahead = number(0, 700.0f);
		const float lateral = number(1, 0.0f);
		const float entry = number(2, 450.0f);
		const char* model = args[3] != nullptr && *args[3] != '\0' ? args[3] : nullptr;
		const bool armed = EmpiresVehicleDebugObstacle(ahead, lateral, entry, model);
		CBotGlobals::botMessage(pEntity, 0, "empires_vehicle_obstacle armed=%d ahead=%.0f lateral=%.0f entry=%.0f",
			armed ? 1 : 0, ahead, lateral, entry);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_vehicle_obstacle [ahead] [lateral] [entry_speed] [model]" : test-only, the next NF Light/BE AFV driver at entry_speed gets a solid prop (default models/props/concretebarrier.mdl) on its route, ahead along the route and lateral to its left; logs vehicle obstacle placed/passed/timeout/abort)");

int EmpiresTestWagesChat(int speakerUserId, float wages); // bot_empires_vehicle.cpp

CBotCommandInline EmpiresTestWagesCommand("empires_test_wages", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const int userId = args[0] != nullptr && *args[0] != '\0' ? std::atoi(args[0]) : 2;
		const float wages = args[1] != nullptr && *args[1] != '\0' ? static_cast<float>(std::atof(args[1])) : 50.0f;
		const int set = EmpiresTestWagesChat(userId, wages);
		CBotGlobals::botMessage(pEntity, 0, "empires_test_wages speaker=%d wages=%.0f bots=%d", userId, wages, set);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_wages <speaker_userid> <wages>" : test-only, sets the teammates' wages and runs the wages chat request)");

bool EmpiresTestEngibotChat(int speakerUserId, char prefix); // bot_empires_engibot.cpp

CBotCommandInline EmpiresTestEngibotCommand("empires_test_engibot", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const int userId = args[0] != nullptr && *args[0] != '\0' ? std::atoi(args[0]) : 2;
		const char prefix = args[1] != nullptr && args[1][0] == '!' ? '!' : '/';
		const bool said = EmpiresTestEngibotChat(userId, prefix);
		CBotGlobals::botMessage(pEntity, 0, "empires_test_engibot speaker=%d said=%d", userId, said ? 1 : 0);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_engibot <speaker_userid> [!]" : test-only, the speaker says /buildengibot (or !buildengibot) in chat)");

// Test only: the commander gives an order the way the game's commander UI does: select the unit
// (emp_unit_order_list <entindex>), then emp_unit_order <type> <x> <y> <z>, or with a target
// emp_unit_order <type> <target entindex>
// (server 0x64cdf0 / 0x64e9a0, reverse-engineering/notes/commander-order-types.md).
// Type 0 abort, 1 move, 2 attack location, 3 attack target, 4 guard. target = player userid.
CBotCommandInline EmpiresTestUnitOrderCommand("empires_test_unit_order", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		auto byUserId = [](int userId) -> edict_t* {
			for (int i = 1; gpGlobals != nullptr && i <= gpGlobals->maxClients; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				if (e != nullptr && !e->IsFree() && engine->GetPlayerUserId(e) == userId)
					return e;
			}
			return nullptr;
		};
		auto arg = [&args](std::size_t i) { return i < args.size() && args[i] != nullptr && *args[i] != '\0' ? args[i] : nullptr; };
		if (arg(5) == nullptr || helpers == nullptr)
			return COMMAND_ERROR;
		edict_t* commander = byUserId(std::atoi(arg(0)));
		edict_t* unit = byUserId(std::atoi(arg(2)));
		edict_t* target = nullptr;
		if (commander == nullptr || unit == nullptr)
			return COMMAND_ERROR;
		const bool refinery = arg(6) != nullptr && arg(6)[0] == 'r';
		if (arg(6) != nullptr && (arg(6)[0] == 'b' || refinery))
		{
			// "b": the unit's nearest own building below full health; "bd": nearest full one, damaged to 100.
			float best = 0.0f;
			const Vector from = CBotGlobals::entityOrigin(unit);
			for (int i = gpGlobals->maxClients + 1; i < engine->GetEntityCount(); ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				if (e == nullptr || e->IsFree() || e->GetClassName() == nullptr ||
					std::strncmp(e->GetClassName(), "emp_building", 12) != 0 ||
					(refinery && std::strstr(e->GetClassName(), "refinery") == nullptr) ||
					CClassInterface::getTeam(e) != CClassInterface::getTeam(unit))
					continue;
				int hp = -1, maxHp = -1;
				if (e->GetNetworkable() != nullptr)
				{
					CClassInterfaceValue h(e->GetNetworkable()->GetServerClass()->m_pNetworkName, "m_iHealth", 0);
					CClassInterfaceValue m(e->GetNetworkable()->GetServerClass()->m_pNetworkName, "m_iMaxHealth", 0);
					h.findOffset();
					m.findOffset();
					if (int* p = h.getIntPointer(e)) hp = *p;
					if (int* p = m.getIntPointer(e)) maxHp = *p;
				}
				const bool full = hp >= maxHp && maxHp > 0;
				if (!refinery && full != (arg(6)[1] == 'd')) // b: not full (new or damaged); bd: full, damaged below
					continue;
				const float d = (CBotGlobals::entityOrigin(e) - from).Length();
				if (target == nullptr || d < best)
				{
					target = e;
					best = d;
				}
			}
			if (target != nullptr && (arg(6)[1] == 'd' || refinery) && target->GetNetworkable() != nullptr)
			{
				CClassInterfaceValue health(target->GetNetworkable()->GetServerClass()->m_pNetworkName, "m_iHealth", 0);
				health.findOffset();
				if (int* h = health.getIntPointer(target))
					*h = 100;
			}
			if (target == nullptr)
				return COMMAND_ERROR;
		}
		else if (arg(6) != nullptr)
			target = byUserId(std::atoi(arg(6)));
		const int type = std::atoi(arg(1));
		char command[160];
		snprintf(command, sizeof(command), "emp_unit_order_list %d\n", engine->IndexOfEdict(unit));
		helpers->ClientCommand(commander, command);
		if (target != nullptr && engine->IndexOfEdict(target) <= gpGlobals->maxClients &&
			CClassInterface::getTeam(target) != CClassInterface::getTeam(commander) &&
			target->GetUnknown() != nullptr && target->GetUnknown()->GetBaseEntity() != nullptr)
		{
			// An enemy player is accepted only with byte +0x14ec set, which only ShouldTransmit to a
			// human commander's client writes (0x673173; commander-order-types.md). A bot commander gets
			// no snapshots, so set it here like a human commander's view would.
			reinterpret_cast<unsigned char*>(target->GetUnknown()->GetBaseEntity())[0x14ec] = 1;
		}
		if (target != nullptr)
			snprintf(command, sizeof(command), "emp_unit_order %d %d\n", type, engine->IndexOfEdict(target)); // entity form: 2 args (0x650114)
		else
			snprintf(command, sizeof(command), "emp_unit_order %d %s %s %s\n", type, arg(3), arg(4), arg(5));
		helpers->ClientCommand(commander, command);
		int commandType = -999;
		ServerClass* sc = unit->GetNetworkable() != nullptr ? unit->GetNetworkable()->GetServerClass() : nullptr;
		if (sc != nullptr)
		{
			CClassInterfaceValue v(sc->m_pNetworkName, "m_iCommandType", 0);
			v.findOffset();
			if (int* p = v.getIntPointer(unit))
				commandType = *p;
		}
		CBotGlobals::botMessage(pEntity, 0, "empires_test_unit_order commander=%s unit=%d type=%d target=%d sent=\"%s\" unit_command_type_now=%d",
			arg(0), engine->IndexOfEdict(unit), type, target != nullptr ? engine->IndexOfEdict(target) : -1, command, commandType);
		RCBOT_DIAG("[RCBOT2] test-unit-order unit=%d type=%d unit_command_type_now=%d\n", engine->IndexOfEdict(unit), type, commandType);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_unit_order <commander_userid> <type> <unit_userid> <x> <y> <z> [target_userid|b|bd|r]" : test-only (b = nearest own building below full health, bd = nearest full one, damaged to 100 first, r = nearest own refinery, damaged to 100; an enemy player target gets the commander-view byte +0x14ec), the commander orders one unit through emp_unit_order_list + emp_unit_order)");

extern int g_empiresTestHumanCommanderTeam; // bot_empires_engibot.cpp

CBotCommandInline EmpiresTestHelpHumanCommand("empires_test_help_human", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		g_empiresTestHumanCommanderTeam = args.size() > 0 && args[0] != nullptr ? std::atoi(args[0]) : 0;
		CBotGlobals::botMessage(pClient != nullptr ? pClient->getPlayer() : nullptr, 0, "empires_test_help_human team=%d", g_empiresTestHumanCommanderTeam);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_help_human <team|0>" : test-only, the team's seated bot commander counts as a human commander for rcbot_empires_engineer_help_human)");

CBotCommandInline EmpiresCommanderMoveTestCommand("empires_test_commander_move", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		std::vector<std::string> fixtureArguments;
		for (const char* argument : args)
			if (argument != nullptr && *argument != '\0')
				fixtureArguments.emplace_back(argument);
		RCBotCommanderMoveTestFixture::Request request;
		if (!RCBotCommanderMoveTestFixture::ParseArguments(fixtureArguments,
			request))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_test_commander_move bot <bot_userid> <x> <y> <z> | squad <team> <squad_id> <x> <y> <z>");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_test_commander_move requires the Empires mod");
			return COMMAND_ACCESSED;
		}

		const Vector target(request.target.x, request.target.y,
			request.target.z);
		if (request.mode == RCBotCommanderMoveTestFixture::Mode::BOT)
		{
			const int userId = static_cast<int>(request.botUserId);
			edict_t* pBotEdict = CBotGlobals::playerByUserId(userId);
			CBot* pBaseBot = pBotEdict != nullptr ?
				CBots::getBotPointer(pBotEdict) : nullptr;
			if (pBaseBot == nullptr || pBaseBot->isControlledTestOnly() ||
				!static_cast<CBotEmpires*>(pBaseBot)->requestEmpiresCommanderMoveTest(target))
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: bot MOVE requires one alive normal Empires bot in a normal NF/BE round");
				return COMMAND_ACCESSED;
			}
			CBotGlobals::botMessage(pEntity, 0,
				"Empires commander move test armed bot_userid=%d target=(%.1f %.1f %.1f)",
				userId, target.x, target.y, target.z);
		}
		else
		{
			bool issued = false;
			for (int slot = 0; slot < RCBOT_MAXPLAYERS && !issued; ++slot)
			{
				CBot* pBaseBot = CBots::getBot(slot);
				if (pBaseBot == nullptr || pBaseBot->isControlledTestOnly())
					continue;
				issued = static_cast<CBotEmpires*>(pBaseBot)->
					requestEmpiresSquadMoveTestFixture(request.team,
						static_cast<int>(request.squadId), target);
			}
			if (!issued)
			{
				CBotGlobals::botMessage(pEntity, 0,
					"error: squad MOVE requires an existing live bot-owned Empires Squad and its leader");
				return COMMAND_ACCESSED;
			}
			CBotGlobals::botMessage(pEntity, 0,
				"Empires commander squad move test issued team=%d squad_id=%llu target=(%.1f %.1f %.1f)",
				request.team, static_cast<unsigned long long>(request.squadId),
				target.x, target.y, target.z);
		}

		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_commander_move bot <bot_userid> <x> <y> <z> | squad <team> <squad_id> <x> <y> <z>" : test-only MOVE fixture; bot mode preserves Commander BOT path, squad mode issues one Phase-10.6 SQUAD parent/child order from existing live bot-owned membership without joining or moving any member)");

CBotCommandInline EmpiresGuardTestCommand("empires_test_guard", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if (args.size() < 2 || args[0] == nullptr || args[1] == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0, "usage: rcbotd empires_test_guard <bot_userid> <building_entindex|0>");
			return COMMAND_ACCESSED;
		}
		edict_t* pBotEdict = CBotGlobals::playerByUserId(std::atoi(args[0]));
		CBot* pBaseBot = pBotEdict != nullptr ? CBots::getBotPointer(pBotEdict) : nullptr;
		CBotMod* pMod = CBotGlobals::getCurrentMod();
		if (pBaseBot == nullptr || pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES)
		{
			CBotGlobals::botMessage(pEntity, 0, "error: no Empires bot with that userid");
			return COMMAND_ACCESSED;
		}
		static_cast<CBotEmpires*>(pBaseBot)->setEmpiresTestGuardTarget(std::atoi(args[1]));
		if (args.size() > 2 && args[2] != nullptr && std::strcmp(args[2], "damage") == 0)
			static_cast<CBotEmpires*>(pBaseBot)->damageEmpiresTestGuardBuilding(std::atoi(args[1]));
		Msg("[RCBOT2] guard-order test armed\n");
		CBotGlobals::botMessage(pEntity, 0, "Empires guard test armed");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_guard <bot_userid> <building_entindex|0>" : test-only, makes the bot act as if a Commander gave Guard on that building)");

CBotCommandInline EmpiresBotClientCommandTest("empires_test_botcmd", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if (args.size() < 3 || args[0] == nullptr || args[1] == nullptr || args[2] == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0, "usage: rcbotd empires_test_botcmd <bot_userid> <repeat> <command> [args]");
			return COMMAND_ACCESSED;
		}
		constexpr int maxRepeat = 32;
		char* repeatEnd = nullptr;
		const long repeat = std::strtol(args[1], &repeatEnd, 10);
		if (repeatEnd == args[1] || *repeatEnd != '\0' || repeat < 1 || repeat > maxRepeat)
		{
			CBotGlobals::botMessage(pEntity, 0, "error: repeat must be an integer from 1 to %d", maxRepeat);
			return COMMAND_ACCESSED;
		}
		edict_t* pBotEdict = CBotGlobals::playerByUserId(std::atoi(args[0]));
		if (pBotEdict == nullptr || CBots::getBotPointer(pBotEdict) == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0, "error: no bot with that userid");
			return COMMAND_ACCESSED;
		}
		std::string command;
		for (std::size_t i = 2; i < args.size() && args[i] != nullptr; ++i)
		{
			if (!command.empty())
				command += ' ';
			command += args[i];
		}
		command += '\n';
		for (long i = 0; i < repeat; ++i)
			helpers->ClientCommand(pBotEdict, command.c_str());
		Msg("[RCBOT2] botcmd userid=%s repeat=%ld cmd=%s", args[0], repeat, command.c_str());
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_botcmd <bot_userid> <repeat> <command> [args]" : test-only, the bot runs a client command)");

CBotCommandInline EmpiresKillZoneTestCommand("empires_test_kill_zone", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		for (int i = 0; i < 4; ++i)
			if (args[i] == nullptr || *args[i] == '\0')
				return COMMAND_ERROR;
		const int team = args[4] != nullptr && *args[4] != '\0' ? std::atoi(args[4]) : 0;
		EmpiresTestKillZoneArm(static_cast<float>(std::atof(args[0])), static_cast<float>(std::atof(args[1])),
			static_cast<float>(std::atof(args[2])), static_cast<float>(std::atof(args[3])), team);
		Msg("[RCBOT2] test-kill-zone armed\n");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_kill_zone <x> <y> <z> <radius> [team]" : test-only (TODO 8.1), every bot (of team 2/3, 0 = all) on foot that enters the circle (2D, +-200 height) is killed once per life by 'kill' and counted as a death for the hot zones; radius 0 disarms)");

CBotCommandInline EmpiresNearDeathTestCommand("empires_test_near_death", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		if (args[0] == nullptr || *args[0] == '\0' || playerinfomanager == nullptr)
			return COMMAND_ERROR;
		edict_t* bot = CBotGlobals::playerByUserId(std::atoi(args[0]));
		IPlayerInfo* info = bot != nullptr ? playerinfomanager->GetPlayerInfo(bot) : nullptr;
		if (info == nullptr)
			return COMMAND_ERROR;
		const int deaths = args[1] != nullptr && *args[1] != '\0' ? std::atoi(args[1]) : 2;
		const float threatDist = args[2] != nullptr && *args[2] != '\0' ? static_cast<float>(std::atof(args[2])) : 800.0f;
		Vector fwd, right;
		QAngle angles = info->GetAbsAngles();
		angles.x = 0.0f;
		AngleVectors(angles, &fwd, &right, nullptr);
		const Vector at = CBotGlobals::entityOrigin(bot);
		const Vector threat = at + fwd * threatDist;
		for (int i = 0; i < deaths; ++i)
			EmpiresInjectNearDeath(info->GetTeamIndex(), at + right * (i % 2 == 0 ? 150.0f : -150.0f) + fwd * 100.0f, threat);
		Msg("[RCBOT2] test-near-death userid=%s deaths=%d threat=(%.0f %.0f %.0f)\n", args[0], deaths, threat.x, threat.y, threat.z);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_near_death <bot_userid> [deaths=2] [threat_dist=800]" : test-only (TODO 8.2), records teammate deaths 150 HU beside the bot, killed from threat_dist ahead of its facing)");

extern int g_exitBlockUserId; // bot_empires_vehicle.cpp
extern bool g_exitBlockPassenger;

CBotCommandInline EmpiresExitBlockTestCommand("empires_test_exit_block", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		g_exitBlockUserId = args[0] != nullptr && *args[0] != '\0' ? std::atoi(args[0]) : -1;
		g_exitBlockPassenger = args[1] != nullptr && std::strcmp(args[1], "passenger") == 0;
		Msg("[RCBOT2] exit-block armed userid=%d passenger=%d\n", g_exitBlockUserId, g_exitBlockPassenger ? 1 : 0);
		CBotGlobals::botMessage(pEntity, 0, "empires_test_exit_block armed userid=%d", g_exitBlockUserId);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_exit_block <bot_userid|0> [passenger]" : test-only, once that bot (0 = the first APC driver) drives a vehicle, solid barriers box the vehicle in and the bot tries to get out; with passenger the driver keeps driving and a teammate on foot respawns into the vehicle (logs vehicle exit-block placed, stall-escalate who=vehicle-exit))");

CBotCommandInline EmpiresCommanderYieldTestCommand("empires_test_commander_yield", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if (args.size() < 1 || args[0] == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0, "usage: rcbotd empires_test_commander_yield <team 2|3>");
			return COMMAND_ACCESSED;
		}
		CBotEmpires::requestCommanderYieldTest(std::atoi(args[0]));
		Msg("[RCBOT2] commander-yield test armed team=%d\n", std::atoi(args[0]));
		return COMMAND_ACCESSED;
	}, R"(usage "empires_test_commander_yield <team>" : test-only, the auto Commander of that team acts as if a human wanted the seat)");

CBotCommandInline EmpiresRecoveryCaptureAcceptanceCommand(
	"empires_recovery_acceptance_capture", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
		int userId = -1;
		if (argumentCount < 1 || argumentCount > 2 ||
			(args[0] == nullptr || (std::strcmp(args[0], "prepare") != 0 &&
				std::strcmp(args[0], "progress") != 0 &&
				std::strcmp(args[0], "select") != 0)) ||
			(argumentCount == 2 &&
				!parseEmpiresFixtureUserIdArgument(args[1], userId)))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_recovery_acceptance_capture <prepare|progress|select> [bot_userid]");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
			CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBot == nullptr || pBot->isControlledTestOnly())
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: recovery capture acceptance requires one alive normal Empires bot");
			return COMMAND_ACCESSED;
		}

		const bool accepted = static_cast<CBotEmpires*>(pBot)->
			requestEmpiresRecoveryCaptureAcceptance(args[0]);
		CBotGlobals::botMessage(pEntity, 0,
			"Empires recovery acceptance capture command phase=%s accepted=%d bot_userid=%d",
			args[0], accepted ? 1 : 0, normalEmpiresBotUserId(pBot));
		return COMMAND_ACCESSED;
	}, R"(usage "empires_recovery_acceptance_capture <prepare|progress|select> [bot_userid]" : test-only deterministic Recovery Episode capture acceptance fixture using the real capture selector, domain exhaustion, progress and suppression hooks)"
);

CBotCommandInline EmpiresRecoverySquadAcceptanceCommand(
	"empires_recovery_acceptance_squad", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const std::size_t argumentCount = meaningfulBotCommandArgCount(args);
		int userId = -1;
		if (argumentCount > 1 || (argumentCount == 1 &&
			!parseEmpiresFixtureUserIdArgument(args[0], userId)))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_recovery_acceptance_squad [bot_userid]");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBot = userId >= 0 ? findNormalEmpiresBotByUserId(userId) :
			CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBot == nullptr || pBot->isControlledTestOnly())
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: recovery squad acceptance requires one alive normal Empires bot");
			return COMMAND_ACCESSED;
		}

		const bool accepted = static_cast<CBotEmpires*>(pBot)->
			requestEmpiresRecoverySquadAcceptance();
		CBotGlobals::botMessage(pEntity, 0,
			"Empires recovery acceptance squad command accepted=%d bot_userid=%d",
			accepted ? 1 : 0, normalEmpiresBotUserId(pBot));
		return COMMAND_ACCESSED;
	}, R"(usage "empires_recovery_acceptance_squad [bot_userid]" : test-only deterministic SQUAD recovery shadow acceptance fixture using the existing shared squad goal and real recovery hooks)"
);

CBotCommandInline EmpiresScoutBuildingVisibilityScanCommand(
	"empires_scout_building_visibility_scan", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool validArgumentCount = args.size() == 2 ||
			(args.size() == 3 && (args[2] == nullptr || *args[2] == '\0'));
		int buildingIndex = -1;
		if (!validArgumentCount ||
			!parseEmpiresPathWaypointArgument(args[0], buildingIndex))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_scout_building_visibility_scan <building_index>");
			return COMMAND_ACCESSED;
		}

		edict_t* pBuilding = INDEXENT(buildingIndex);
		const char* buildingClass = pBuilding != nullptr && !pBuilding->IsFree() ?
			pBuilding->GetClassName() : nullptr;
		if (pBuilding == nullptr || pBuilding->IsFree() || buildingClass == nullptr ||
			std::strncmp(buildingClass, "emp_building_", 13) != 0)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Scout building visibility scan requires a live Empires building entity");
			return COMMAND_ACCESSED;
		}

		const Vector targetCenter = CBotGlobals::worldCenter(pBuilding);
		int visibleCount = 0;
		int nearestVisibleWaypoint = -1;
		float nearestVisibleDistance = 1.0e30f;
		Vector nearestVisibleSource = vec3_origin;
		const int waypointCount = (std::min)(CWaypoints::numWaypoints(),
			CWaypoints::MAX_WAYPOINTS);
		for (int waypointIndex = 0; waypointIndex < waypointCount; ++waypointIndex)
		{
			CWaypoint* pWaypoint = CWaypoints::getWaypoint(waypointIndex);
			if (pWaypoint == nullptr || !pWaypoint->isUsed())
				continue;

			// Match the normal player eye-height convention used by the Scout
			// binocular action.  This is a trace-only observer; it never moves an
			// actor or changes a building/property state.
			const Vector source = pWaypoint->getOrigin() +
				Vector(0.0f, 0.0f, 64.0f);
			if (!CBotGlobals::isVisible(nullptr, source, pBuilding))
				continue;

			++visibleCount;
			const float distance = (targetCenter - source).Length();
			if (distance < nearestVisibleDistance)
			{
				nearestVisibleDistance = distance;
				nearestVisibleWaypoint = waypointIndex;
				nearestVisibleSource = source;
			}
		}

		if (logger != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires scout building visibility_scan target_index=%d target_class=%s waypoint_count=%d visible_count=%d nearest_visible_waypoint=%d nearest_visible_distance=%.1f source=(%.1f %.1f %.1f) target_center=(%.1f %.1f %.1f)",
				buildingIndex, buildingClass, waypointCount, visibleCount,
				nearestVisibleWaypoint,
				nearestVisibleWaypoint >= 0 ? nearestVisibleDistance : -1.0f,
				nearestVisibleSource.x, nearestVisibleSource.y,
				nearestVisibleSource.z, targetCenter.x, targetCenter.y,
				targetCenter.z);
		}
		CBotGlobals::botMessage(pEntity, 0,
			"Empires Scout building visibility scan: target=%d visible_waypoints=%d nearest_waypoint=%d",
			buildingIndex, visibleCount, nearestVisibleWaypoint);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_scout_building_visibility_scan <building_index>" : read-only trace from loaded waypoint eye-height positions to one live Empires building; does not move actors or write gameplay state)");

CBotCommandInline EmpiresEnemyLosFixtureCommand("empires_enemy_los_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool naturalExpiry =
			(args[2] != nullptr && std::strcmp(args[2], "expire") == 0) ||
			(args[3] != nullptr && std::strcmp(args[3], "expire") == 0);
		const bool validArguments = args.size() == 2 || naturalExpiry ||
			(args.size() == 3 && (args[2] == nullptr || *args[2] == '\0'));
		int observerUserId = -1;
		int opponentUserId = -1;
		if (!validArguments ||
			!parseEmpiresFixtureUserIdArgument(args[0], observerUserId) ||
			!parseEmpiresFixtureUserIdArgument(args[1], opponentUserId))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_enemy_los_once <observer_userid> <opponent_userid> [expire]");
			return COMMAND_ACCESSED;
		}

		if (CBotEmpires::requestEmpiresEnemyLosFixture(observerUserId,
			opponentUserId, naturalExpiry))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires enemy LOS fixture armed observer_userid=%d opponent_userid=%d",
				observerUserId, opponentUserId);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires enemy LOS fixture was rejected; use two alive opposing normal bot userids");
		}
		return COMMAND_ACCESSED;
		}, R"(usage "empires_enemy_los_once <observer_userid> <opponent_userid> [expire]" : runs one bounded two-normal-bot LOS/last-known fixture; optional expire mode uses the same geometry fixture with opponent route 155 to 157 and requires natural search expiry without reacquisition)");

CBotCommandInline EmpiresEnemyLosFixtureCleanupCommand("empires_enemy_los_cleanup", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotEmpires::clearEmpiresEnemyLosFixture("operator-cleanup");
		CBotGlobals::botMessage(pEntity, 0,
			"Empires enemy LOS fixture cleanup result=idempotent");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_enemy_los_cleanup" : releases any active debug LOS fixture without changing normal enemy memory)");

CBotCommandInline EmpiresEnemyCombatFixtureCommand("empires_enemy_combat_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		int observerUserId = -1;
		int opponentUserId = -1;
		const bool trailingParserSentinel = args.size() == 3 &&
			(args[2] == nullptr || *args[2] == '\0');
		const bool validArguments = (args.size() == 2 || trailingParserSentinel) &&
			parseEmpiresFixtureUserIdArgument(args[0], observerUserId) &&
			parseEmpiresFixtureUserIdArgument(args[1], opponentUserId) &&
			observerUserId != opponentUserId;
		if (!validArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_enemy_combat_once <observer_userid> <opponent_userid>");
			return COMMAND_ACCESSED;
		}

		if (CBotEmpires::requestEmpiresEnemyLosFixture(observerUserId,
			opponentUserId, false, true))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires enemy combat fixture armed observer_userid=%d opponent_userid=%d positioning=known-visible-waypoints attack_input=NORMAL",
				observerUserId, opponentUserId);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires enemy combat fixture was rejected; use two alive opposing normal bot userids");
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_enemy_combat_once <observer_userid> <opponent_userid>" : positions two alive opposing normal bots at known visible waypoints, holds movement only, and leaves normal combat input active for one bounded telemetry run)");

CBotCommandInline EmpiresScoutPriorityFixtureCommand("empires_scout_priority_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		int scoutUserId = -1;
		int targetAUserId = -1;
		int targetBUserId = -1;
		int firingTargetUserId = -1;
		const bool trailingParserSentinel = args.size() == 5 &&
			(args[4] == nullptr || *args[4] == '\0');
		const bool validArguments = (args.size() == 4 || trailingParserSentinel) &&
			parseEmpiresFixtureUserIdArgument(args[0], scoutUserId) &&
			parseEmpiresFixtureUserIdArgument(args[1], targetAUserId) &&
			parseEmpiresFixtureUserIdArgument(args[2], targetBUserId) &&
			parseEmpiresFixtureUserIdArgument(args[3], firingTargetUserId);
		if (!validArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_scout_priority_once <scout_userid> <target_a_userid> <target_b_userid> <firing_target_userid>");
			return COMMAND_ACCESSED;
		}

		if (CBotEmpires::requestEmpiresScoutPriorityFixture(
			scoutUserId, targetAUserId, targetBUserId, firingTargetUserId))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Scout priority fixture armed scout_userid=%d target_a_userid=%d target_b_userid=%d firing_target_userid=%d",
				scoutUserId, targetAUserId, targetBUserId, firingTargetUserId);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Scout priority fixture was rejected; use one alive Scout and two alive opposing normal bots");
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_scout_priority_once <scout_userid> <target_a_userid> <target_b_userid> <firing_target_userid>" : positions one Scout and two opposing normal players at known visible waypoints, varies only the public last-command IN_ATTACK state and observes normal target selection)");

CBotCommandInline EmpiresScoutPriorityFixtureCleanupCommand("empires_scout_priority_cleanup", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotEmpires::clearEmpiresScoutPriorityFixture("operator-cleanup");
		CBotGlobals::botMessage(pEntity, 0,
			"Empires Scout priority fixture cleanup result=idempotent");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_scout_priority_cleanup" : releases the bounded Scout target-priority fixture)");

CBotCommandInline EmpiresEnemyApproachFixtureCommand("empires_enemy_approach_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		int observerUserId = -1;
		int opponentUserId = -1;
		const bool trailingParserSentinel = args.size() == 3 &&
			(args[2] == nullptr || *args[2] == '\0');
		const bool validArguments = (args.size() == 2 || trailingParserSentinel) &&
			parseEmpiresFixtureUserIdArgument(args[0], observerUserId) &&
			parseEmpiresFixtureUserIdArgument(args[1], opponentUserId) &&
			observerUserId != opponentUserId;
		if (!validArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_enemy_approach_once <observer_userid> <opponent_userid>");
			return COMMAND_ACCESSED;
		}

		if (CBotEmpires::requestEmpiresEnemyLosFixture(observerUserId,
			opponentUserId, false, true, true))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires enemy approach fixture armed observer_userid=%d opponent_userid=%d positioning=known-visible-waypoints release_after_acquisition=TRUE attack_input=NORMAL",
				observerUserId, opponentUserId);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires enemy approach fixture was rejected; use two alive opposing normal bot userids");
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_enemy_approach_once <observer_userid> <opponent_userid>" : positions two normal bots, releases the observer after acquisition and proves existing approach movement with the target held)");

CBotCommandInline EmpiresScoutBinocularOnceCommand("empires_scout_binocular_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		int scoutUserId = -1;
		int targetUserId = -1;
		const bool trailingParserSentinel = args.size() == 3 &&
			(args[2] == nullptr || *args[2] == '\0');
		const bool validArguments = (args.size() == 2 || trailingParserSentinel) &&
			parseEmpiresFixtureUserIdArgument(args[0], scoutUserId) &&
			parseEmpiresFixtureUserIdArgument(args[1], targetUserId) &&
			scoutUserId != targetUserId;
		if (!validArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_scout_binocular_once <scout_userid> <target_userid>");
			return COMMAND_ACCESSED;
		}

		if (CBotEmpires::requestEmpiresScoutBinocularOnce(scoutUserId, targetUserId))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Scout binocular action armed scout_userid=%d target_userid=%d protocol=select-aim-primary-attack",
				scoutUserId, targetUserId);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Scout binocular action rejected; use an alive normal Scout with an opposing alive target");
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_scout_binocular_once <scout_userid> <target_userid>" : bounded Scout binocular primary-input proof; no generic scoring or role policy)");

CBotCommandInline EmpiresScoutBinocularBuildingOnceCommand("empires_scout_binocular_building_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		int scoutUserId = -1;
		int buildingIndex = -1;
		const bool trailingParserSentinel = args.size() == 3 &&
			(args[2] == nullptr || *args[2] == '\0');
		const bool validArguments = (args.size() == 2 || trailingParserSentinel) &&
			parseEmpiresFixtureUserIdArgument(args[0], scoutUserId) &&
			parseEmpiresFixtureUserIdArgument(args[1], buildingIndex);
		if (!validArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_scout_binocular_building_once <scout_userid> <building_entity_index>");
			return COMMAND_ACCESSED;
		}

		if (CBotEmpires::requestEmpiresScoutBinocularBuildingOnce(
			scoutUserId, buildingIndex))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Scout building binocular action armed scout_userid=%d building_index=%d protocol=select-aim-primary-attack",
				scoutUserId, buildingIndex);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Scout building binocular action rejected; use an alive normal Scout and an opposing Empires building entity index");
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_scout_binocular_building_once <scout_userid> <building_entity_index>" : bounded Scout binocular building-output proof; no generic role policy)");

CBotCommandInline EmpiresScoutSkillObservationCommand("empires_scout_skill_observation", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		int scoutUserId = -1;
		const bool trailingParserSentinel = args.size() == 2 &&
			(args[1] == nullptr || *args[1] == '\0');
		const bool validArguments = (args.size() == 1 || trailingParserSentinel) &&
			parseEmpiresFixtureUserIdArgument(args[0], scoutUserId);
		if (!validArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_scout_skill_observation <scout_userid>");
			return COMMAND_ACCESSED;
		}

		if (CBotEmpires::observeEmpiresScoutSkillsOnce(scoutUserId))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Scout personal-skill observation completed scout_userid=%d source=dynamic-public-property",
				scoutUserId);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Scout personal-skill observation rejected; use an alive normal Scout");
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_scout_skill_observation <scout_userid>" : development-only read-only Scout personal class-skill-slot observation through dynamic public properties; not Squad-Leader skill observation; no state writes)");

CBotCommandInline EmpiresSquadObservationCommand("empires_squad_observation", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		int targetUserId = -1;
		const bool trailingParserSentinel = (args.size() == 1 &&
			(args[0] == nullptr || *args[0] == '\0')) || (args.size() == 2 &&
			(args[1] == nullptr || *args[1] == '\0'));
		const bool validArguments = args.size() == 0 || trailingParserSentinel ||
			(args.size() == 1 && parseEmpiresFixtureUserIdArgument(args[0], targetUserId));
		if (!validArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_squad_observation [userid]");
			return COMMAND_ACCESSED;
		}

		if (CBotEmpires::observeEmpiresSquadProbeOnce(targetUserId))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires squad observation completed target_userid=%d source=dynamic-public-property",
				targetUserId);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires squad observation found no players; use a running Empires server with connected players");
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_squad_observation [userid]" : development-only read-only Empires squad membership/leader/class/points observation through dynamic public properties; dumps raw m_iSquad/m_iSquadPosition/m_bSquadLeader/m_iEmpSquadXP with resolution status; no state writes)");

CBotCommandInline EmpiresSquadMakeLeadCommand("empires_squad_make_lead", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		int issuerUserId = -1;
		int targetUserId = -1;
		const bool validArguments = meaningfulBotCommandArgCount(args) == 2 &&
			parseEmpiresFixtureUserIdArgument(args[0], issuerUserId) &&
			parseEmpiresFixtureUserIdArgument(args[1], targetUserId);
		if (!validArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_squad_make_lead <issuer_userid> <target_userid>");
			return COMMAND_ACCESSED;
		}

		if (CBotEmpires::requestEmpiresSquadLeadActuation(issuerUserId,
			targetUserId))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires squad lead actuation submitted issuer_userid=%d target_userid=%d; confirm with empires_squad_make_lead_status",
				issuerUserId, targetUserId);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires squad lead actuation rejected issuer_userid=%d target_userid=%d; see server log reason",
				issuerUserId, targetUserId);
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_squad_make_lead <issuer_userid> <target_userid>" : development-only native Empires squad leader change through the production client-command path (emp_make_lead with the live target entity index); submission is not success, confirm with empires_squad_make_lead_status; no tactical policy)");

CBotCommandInline EmpiresSquadMakeLeadStatusCommand("empires_squad_make_lead_status", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const RCBotEmpiresLeadActuation::LeadActuationStatus status =
			CBotEmpires::pollEmpiresSquadLeadActuation();
		CBotGlobals::botMessage(pEntity, 0,
			"Empires squad lead actuation status=%s",
			RCBotEmpiresLeadActuation::LeadActuationStatusName(status));
		return COMMAND_ACCESSED;
	}, R"(usage "empires_squad_make_lead_status" : development-only readback of the pending native squad leader change from public squad state; reports NOT_REQUESTED/REQUESTED/CONFIRMED/FAILED/INVALIDATED)");

CBotCommandInline EmpiresMortarSmokeCommand("empires_mortar_smoke_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES || pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_mortar_smoke_once requires one alive normal Empires bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresMortarSmoke())
			CBotGlobals::botMessage(pEntity, 0,
				"Empires mortar smoke probe armed: NF Grenadier, owned mortar, one normal IN_ATTACK pulse");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires mortar smoke probe was rejected");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_mortar_smoke_once" : bounded NF mortar projectile proof using normal bot input; no target required)");

CBotCommandInline EmpiresMortarFireOnceCommand("empires_mortar_fire_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_mortar_fire_once requires one normal Empires bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresMortarFireOnce())
			CBotGlobals::botMessage(pEntity, 0,
				"Empires normal mortar action armed: current-team Grenadier, normal handleWeapons/RunPlayerMove path");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires normal mortar action was rejected; use an alive NF/BE Grenadier with its mortar equipped");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_mortar_fire_once" : bounded current-team Grenadier mortar first shot through the normal weapon/input path; no tactical selection)");

CBotCommandInline EmpiresMineFireOnceCommand("empires_mine_fire_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_mine_fire_once requires one normal Empires bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresMineFireOnce())
			CBotGlobals::botMessage(pEntity, 0,
				"Empires normal mine action armed: current-team Grenadier, one normal IN_ATTACK pulse");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires normal mine action was rejected; use an alive NF/BE Grenadier with its mine equipped");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_mine_fire_once" : bounded current-team Grenadier mine throw through the normal weapon/input path; no tactical placement)");

CBotCommandInline EmpiresMineTriggerOnceCommand("empires_mine_trigger_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if (args[0] == nullptr || *args[0] == '\0' ||
			(args.size() > 2 && args[2] != nullptr && *args[2] != '\0'))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_mine_trigger_once <target_userid> [target_hold_seconds 0..8]");
			return COMMAND_ACCESSED;
		}

		int targetUserId = -1;
		if (!parseEmpiresFixtureUserIdArgument(args[0], targetUserId))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_mine_trigger_once <target_userid> [target_hold_seconds 0..8]");
			return COMMAND_ACCESSED;
		}
		float targetHoldSeconds = 0.0f;
		if (args.size() > 1 && args[1] != nullptr && *args[1] != '\0' &&
			!parseEmpiresFixtureDelayArgument(args[1], targetHoldSeconds))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_mine_trigger_once <target_userid> [target_hold_seconds 0..8]");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_mine_trigger_once requires a normal Empires Grenadier and opposing normal target");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresMineTriggerOnce(targetUserId, targetHoldSeconds))
			CBotGlobals::botMessage(pEntity, 0,
				"Empires mine trigger fixture armed: normal mine deployment plus opposing target engine-trigger observation");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires mine trigger fixture was rejected; use an alive Grenadier and opposing normal bot userid");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_mine_trigger_once <target_userid> [target_hold_seconds]" : debug-only bounded mine deployment and normal engine trigger observation; optional target hold delays fixture-only placement for activation observation)");

CBotCommandInline EmpiresMortarAimOnceCommand("empires_mortar_aim_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool noArguments = args[0] == nullptr || *args[0] == '\0';
		const bool trailingParserSentinel = args.size() == 2 &&
			(args[1] == nullptr || *args[1] == '\0');
		if (!noArguments && !(args.size() == 1 || trailingParserSentinel))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_mortar_aim_once [target_waypoint]");
			return COMMAND_ACCESSED;
		}

		int targetWaypoint = 149;
		if (!noArguments && !parseEmpiresPathWaypointArgument(args[0], targetWaypoint))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_mortar_aim_once [target_waypoint]");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_mortar_aim_once requires one normal Empires bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresMortarAimOnce(targetWaypoint))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"Empires mortar aim-once armed: NF Grenadier start waypoint 147 target waypoint %d; normal mortar action/input path",
				targetWaypoint);
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires mortar aim-once was rejected; use an alive NF Grenadier and loaded waypoints");
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_mortar_aim_once [target_waypoint]" : debug-only one-shot NF mortar aim test from waypoint 147, default target 149, using normal controller input)");

CBotCommandInline EmpiresEngineerRepairFixturePrepareCommand("empires_engineer_repair_fixture_prepare", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES || pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_repair_fixture_prepare requires one normal Empires bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->prepareEmpiresEngineerRepairFixture())
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Engineer repair fixture prepared: NF Barracks targetname assigned");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: could not prepare the NF Barracks repair fixture");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_repair_fixture_prepare" : development-only names the current NF Barracks for the isolated repair fixture)");

CBotCommandInline EmpiresEngineerRepairFixtureDamageCommand("empires_engineer_repair_fixture_damage", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES || pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_repair_fixture_damage requires one normal Empires bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		// Optional: building class name and damage (default NF Barracks, 100).
		const char* buildingClass = args[0] != nullptr && *args[0] != '\0' ?
			args[0] : "emp_building_nf_barracks";
		const int damage = args.size() > 1 && args[1] != nullptr && *args[1] != '\0' ?
			std::atoi(args[1]) : 100;
		if (pBot->damageEmpiresEngineerRepairFixture(damage, buildingClass))
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Engineer repair fixture damage applied through dynamic m_iHealth test fixture");
		else
			CBotGlobals::botMessage(pEntity, 0,
			"error: could not apply the Engineer repair fixture damage");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_repair_fixture_damage [class] [damage]" : development-only lowers m_iHealth through the resolved NF Barracks property for one repair test)");

CBotCommandInline EmpiresEngineerRepairVisibilityScanCommand("empires_engineer_repair_visibility_scan", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool noArguments = args[0] == nullptr || *args[0] == '\0';
		const bool trailingParserSentinel = args.size() == 2 &&
			(args[1] == nullptr || *args[1] == '\0');
		if (!noArguments && !(args.size() == 1 || trailingParserSentinel))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_repair_visibility_scan [building_index]");
			return COMMAND_ACCESSED;
		}

		int buildingIndex = -1;
		if (!noArguments && !parseEmpiresPathWaypointArgument(args[0], buildingIndex))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_repair_visibility_scan [building_index]");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_repair_visibility_scan requires one normal Empires bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->scanEmpiresEngineerRepairVisibility(buildingIndex))
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Engineer repair visibility scan completed: input-free waypoint traces logged");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Engineer repair visibility scan could not resolve the current team Barracks");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_repair_visibility_scan [building_index]" : development-only input-free MASK_SHOT scan from all current-team waypoints to Barracks target points)");

CBotCommandInline EmpiresEngineerRepairVisibilityAllBuildingsCommand("empires_engineer_repair_visibility_scan_all", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if ((args[0] != nullptr && *args[0] != '\0') ||
			(args.size() > 1 && args[1] != nullptr && *args[1] != '\0'))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_repair_visibility_scan_all");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_repair_visibility_scan_all requires one normal Empires bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->scanEmpiresEngineerRepairVisibilityAllBuildings())
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Engineer all-building visibility scan completed: input-free dynamic traces logged");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Engineer all-building visibility scan found no current-team building");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_repair_visibility_scan_all" : development-only input-free MASK_SHOT scan from all current-team buildings to all current-team waypoints)");

CBotCommandInline EmpiresEngineerRepairOnceCommand("empires_engineer_repair_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool noArguments = args[0] == nullptr || *args[0] == '\0';
		const bool trailingParserSentinel = args.size() == 2 &&
			(args[1] == nullptr || *args[1] == '\0');
		const bool requireFullHealth = args.size() > 1 && args[1] != nullptr &&
			std::strcmp(args[1], "full") == 0;
		if (!noArguments && !(args.size() == 1 || trailingParserSentinel ||
			requireFullHealth))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_repair_once [building_index] [full]");
			return COMMAND_ACCESSED;
		}

		int buildingIndex = -1;
		if (!noArguments && !parseEmpiresPathWaypointArgument(args[0], buildingIndex))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_repair_once [building_index] [full]");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_repair_once requires one alive normal Empires Engineer bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresEngineerRepairOnce(buildingIndex,
			requireFullHealth))
		{
			CBotGlobals::botMessage(pEntity, 0,
				requireFullHealth ?
					"Empires Engineer repair action armed: full-health mode, friendly building, Engineer kit, normal primary input" :
					"Empires Engineer repair action armed: friendly building, Engineer kit, normal primary input");
		}
		else
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Engineer repair action was rejected; use an alive Engineer near a damaged friendly building");
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_repair_once [building_index] [full]" : development-only bounded friendly-building repair through the normal Engineer kit primary input path; optional full waits for max health)");

CBotCommandInline EmpiresEngineerResupplyOnceCommand("empires_engineer_resupply_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if ((args[0] != nullptr && *args[0] != '\0') ||
			(args.size() > 1 && args[1] != nullptr && *args[1] != '\0'))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_resupply_once");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_resupply_once requires one alive normal Empires Engineer bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresEngineerResupplyOnce())
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Engineer resupply action armed: nearest current-team ammo crate, normal IN_USE");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Engineer resupply action was rejected; use an alive Engineer on a map with a same-team ammo crate");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_resupply_once" : development-only bounded same-team ammo-crate interaction through normal Engineer/IN_USE input; no ammo or player-state writes)");

CBotCommandInline EmpiresEngineerResupplyGoalOnceCommand("empires_engineer_resupply_goal_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if ((args[0] != nullptr && *args[0] != '\0') ||
			(args.size() > 1 && args[1] != nullptr && *args[1] != '\0'))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_resupply_goal_once");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_resupply_goal_once requires one alive normal Empires Engineer bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresEngineerResupplyOnce(false, true))
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Engineer resupply-goal depletion armed: normal primary input, then normal out-of-ammo goal selection");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Engineer resupply-goal depletion was rejected; use an alive Engineer on a map with a same-team ammo crate");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_resupply_goal_once" : development-only drains the current Engineer primary through normal input, then returns to normal out-of-ammo resupply goal selection; no ammo or player-state writes)");

CBotCommandInline EmpiresEngineerBuildSelectionOnceCommand("empires_engineer_build_select_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if ((args[0] != nullptr && *args[0] != '\0') ||
			(args.size() > 1 && args[1] != nullptr && *args[1] != '\0'))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_build_select_once");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_build_select_once requires one alive normal Empires Engineer bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresEngineerBuildSelectionOnce())
			CBotGlobals::botMessage(pEntity, 0,
				"Empires Engineer build selection armed: one normal secondary-input pulse, no building placement");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Engineer build selection rejected; use an alive Engineer with a resolvable build state");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_build_select_once" : development-only one-shot Engineer-kit secondary input; observes build-item state without placing a building)");

CBotCommandInline EmpiresEngineerBuildOnceCommand("empires_engineer_build_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool firstArgumentPresent = args[0] != nullptr && *args[0] != '\0';
		const bool secondArgumentPresent = args.size() > 1 && args[1] != nullptr &&
			*args[1] != '\0';
		const bool thirdArgumentPresent = args.size() > 2 && args[2] != nullptr &&
			*args[2] != '\0';
		const bool fullMode = secondArgumentPresent &&
			std::strcmp(args[1], "full") == 0;
		const bool validArguments = !thirdArgumentPresent &&
			(!firstArgumentPresent ? !secondArgumentPresent :
				(!secondArgumentPresent || fullMode));
		if (!validArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_build_once [target_waypoint] [full]");
			return COMMAND_ACCESSED;
		}

		int targetWaypoint = 147;
		if (firstArgumentPresent &&
			!parseEmpiresPathWaypointArgument(args[0], targetWaypoint))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_build_once [target_waypoint] [full]");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_build_once requires one alive normal Empires Engineer bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresEngineerBuildOnce(targetWaypoint, nullptr, fullMode))
			CBotGlobals::botMessage(pEntity, 0,
				fullMode ?
				"Empires Engineer build action armed: engSelect 1, deterministic waypoint aim, normal primary input, full-health mode" :
				"Empires Engineer build action armed: engSelect 1, deterministic waypoint aim, normal primary input");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Engineer build action rejected; use an alive Engineer and a valid team waypoint");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_build_once [target_waypoint] [full]" : development-only bounded Engineer MG-turret placement through current engSelect and normal primary input; optional full continues the normal build input until max health)");

CBotCommandInline EmpiresEngineerBuildAtCommand("empires_engineer_build_at", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool fullMode = args.size() > 3 && args[3] != nullptr &&
			std::strcmp(args[3], "full") == 0;
		const bool optionalArgumentPresent = args.size() > 3 && args[3] != nullptr &&
			*args[3] != '\0';
		const bool trailingParserArgumentPresent = args.size() > 4 &&
			args[4] != nullptr && *args[4] != '\0';
		if ((optionalArgumentPresent && !fullMode) ||
			trailingParserArgumentPresent ||
			args[0] == nullptr || args[1] == nullptr || args[2] == nullptr ||
			*args[0] == '\0' || *args[1] == '\0' || *args[2] == '\0')
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_build_at <x> <y> <z> [full]");
			return COMMAND_ACCESSED;
		}

		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		auto parseCoordinate = [](const char* value, float& result) -> bool
		{
			char* end = nullptr;
			const float parsed = std::strtof(value, &end);
			if (end == value || *end != '\0' || !std::isfinite(parsed))
				return false;
			result = parsed;
			return true;
		};
		if (!parseCoordinate(args[0], x) || !parseCoordinate(args[1], y) ||
			!parseCoordinate(args[2], z))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_engineer_build_at <x> <y> <z> [full]");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_engineer_build_at requires one alive normal Empires Engineer bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		const Vector target(x, y, z);
		if (pBot->requestEmpiresEngineerBuildOnce(-1, &target, fullMode))
			CBotGlobals::botMessage(pEntity, 0,
				fullMode ?
				"Empires Engineer build action armed: explicit debug target, normal primary input, full-health mode" :
				"Empires Engineer build action armed: explicit debug target, normal primary input");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires Engineer build action rejected; use an alive Engineer and finite target");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_engineer_build_at <x> <y> <z> [full]" : development-only bounded Engineer MG-turret placement at an explicit fixture target through normal input; optional full continues the normal build input until max health)");

CBotCommandInline EmpiresVehicleInventoryCommand("empires_vehicle_inventory", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool noArguments = args[0] == nullptr || *args[0] == '\0';
		if (!noArguments)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_vehicle_inventory");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_vehicle_inventory requires the Empires mod");
			return COMMAND_ACCESSED;
		}

		static_cast<CEmpiresMod*>(pMod)->logVehicleInventory();
		CBotGlobals::botMessage(pEntity, 0,
			"Empires vehicle inventory audit executed; see the RCBot2 log");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_vehicle_inventory" : development-only read-only inventory of live Empires vehicle entities, spawners and presets)");

CBotCommandInline EmpiresNavScanCommand("empires_nav_scan", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)pClient;
		RCBotNavMesh::DebugNavScan(Vector(std::atof(args[0]), std::atof(args[1]), std::atof(args[2])),
			Vector(std::atof(args[3]), std::atof(args[4]), std::atof(args[5])), 256.0f, 16.0f,
			args[6] != nullptr ? std::atof(args[6]) : 0.0f);
		return COMMAND_ACCESSED;
	}, "dev scan x y z x2 y2 z2 [vehicle-width]");

CBotCommandInline EmpiresCommanderEntryCommand("empires_commander_entry_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		CBot* bot = CBots::getFirstNormalBot();
		const bool armed = bot != nullptr && CBotGlobals::getCurrentMod() != nullptr &&
			CBotGlobals::getCurrentMod()->getBotType() == BOTTYPE_EMPIRES &&
			static_cast<CBotEmpires*>(bot)->requestCommanderVehicleEntryOnce();
		CBotGlobals::botMessage(pClient != nullptr ? pClient->getPlayer() : nullptr, 0,
			"commander_entry armed=%d", armed ? 1 : 0);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_commander_entry_once" : first normal bot walks to its Command Vehicle and takes the Commander seat)");

// Empires removes a rejected building with UTIL_Remove: the edict stays in use until the end of the frame,
// only EFL_KILLME (m_iEFlags 0x118, bit 0x1 in server_srv.so, CServerNetworkProperty::MarkForDeletion) is set.
static bool EmpiresEntityMarkedForDeletion(edict_t* e)
{
	IServerUnknown* unknown = e != nullptr ? e->GetUnknown() : nullptr;
	CBaseEntity* entity = unknown != nullptr ? unknown->GetBaseEntity() : nullptr;
	return entity == nullptr || (*reinterpret_cast<const int*>(reinterpret_cast<const char*>(entity) + 0x118) & 0x1) != 0;
}

CBotCommandInline EmpiresCommanderPlaceBarracksCommand("empires_commander_place_barracks", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* output = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBot* base = CBots::getFirstNormalBot();
		if (base == nullptr || engine == nullptr || gpGlobals == nullptr ||
			CBotGlobals::getCurrentMod() == nullptr ||
			CBotGlobals::getCurrentMod()->getBotType() != BOTTYPE_EMPIRES)
			return COMMAND_ERROR;
		CBotEmpires* bot = static_cast<CBotEmpires*>(base);
		edict_t* vehicle = bot->m_hCommanderEntryVehicle.get();
		if (bot->m_iCommanderEntryStage != 4 || vehicle == nullptr)
		{
			CBotGlobals::botMessage(output, 0, "commander_place result=NOT_COMMANDER stage=%d", bot->m_iCommanderEntryStage);
			return COMMAND_ACCESSED;
		}

		EmpiresNativePlacement::ContextIdentity context;
		EmpiresNativePlacement::Diagnostics diagnostics;
		if (!EmpiresNativePlacement::CaptureContext(engine->IndexOfEdict(bot->getEdict()), context, &diagnostics))
		{
			CBotGlobals::botMessage(output, 0, "commander_place result=NO_CONTEXT");
			return COMMAND_ACCESSED;
		}

		const Vector center = CBotGlobals::entityOrigin(vehicle);
		EmpiresSpatial::Request request;
		request.origin = {center.x, center.y, center.z};
		const auto search = EmpiresSpatial::SearchBarracksCandidates(context, request);

		const auto listBarracks = []()
		{
			std::vector<int> indices;
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				if (e == nullptr || e->IsFree() || e->GetUnknown() == nullptr || EmpiresEntityMarkedForDeletion(e))
					continue;
				const char* name = e->GetClassName();
				if (name != nullptr && (std::strcmp(name, "emp_building_nf_barracks") == 0 ||
					std::strcmp(name, "emp_building_imp_barracks") == 0 ||
					std::strcmp(name, "emp_building_barracks") == 0))
					indices.push_back(i);
			}
			return indices;
		};
		const auto before = listBarracks();
		EmpiresBarracksExecution::Attempt attempt;
		const auto status = EmpiresBarracksExecution::ExecuteNative(attempt, context, search, diagnostics);
		const auto after = listBarracks();

		int created = -1;
		for (const int index : after)
			if (std::find(before.begin(), before.end(), index) == before.end())
				created = index;
		edict_t* building = created > 0 ? engine->PEntityOfEntIndex(created) : nullptr;
		const Vector actual = building != nullptr ? CBotGlobals::entityOrigin(building) : Vector(0, 0, 0);
		Msg("[RCBOT2] commander-place result=%s search=%s areas=%u queries=%u valid=%u invalid=%u selected=(%.0f %.0f %.0f) created=%d class=%s origin=(%.0f %.0f %.0f) team=%d\n",
			EmpiresBarracksExecution::Name(status), EmpiresSpatial::Name(search.status),
			search.areasInspected, search.nativeQueries, search.valid, search.invalid,
			search.selected.origin.x, search.selected.origin.y, search.selected.origin.z,
			created, building != nullptr ? building->GetClassName() : "-",
			actual.x, actual.y, actual.z, building != nullptr ? CClassInterface::getTeam(building) : -1);

		if (building != nullptr)
		{
			bool assigned = false;
			for (int i = 1; i <= gpGlobals->maxClients && !assigned; ++i)
			{
				edict_t* player = engine->PEntityOfEntIndex(i);
				CBot* candidate = player != nullptr ? CBots::getBotPointer(player) : nullptr;
				if (candidate == nullptr || candidate == base || candidate->getEdict() == nullptr ||
					CClassInterface::getTeam(candidate->getEdict()) != CClassInterface::getTeam(building))
					continue;
				assigned = static_cast<CBotEmpires*>(candidate)->requestEmpiresBuildAssignment(building);
				// Test aid: start the Engineer from its spawn rather than wherever it roamed to.
				if (assigned && helpers != nullptr)
					helpers->ClientCommand(candidate->getEdict(), "kill\n");
			}
			Msg("[RCBOT2] build-assign requested=%d building=%d\n", assigned ? 1 : 0, created);
		}
		return COMMAND_ACCESSED;
	}, R"(usage "empires_commander_place_barracks" : seated Commander bot searches a valid spot near its vehicle and submits one emp_build)");

// Probe: type 0 Refinery, 1 Barracks, 2 Radar, 3 Armory, 4 Repair, 5 VehicleFactory (GetBuildingType order).
// Server side (Command_emp_build 0x651890): "emp_build <type> x y z yaw" never builds a refinery (jump table
// entry for type 0 is a no-op); a refinery needs "emp_build 0 <m_iResIndex> <yaw>" and is placed on that
// enabled emp_resource_point. A rejected building is removed with the deferred UTIL_Remove: it is still
// listed in the same frame (rejected=1 via EFL_KILLME); "empires_commander_build list" checks later.
// Raw offsets (server_srv.so): entity team 0x208, CResourcePoint enabled 0x4a8 / m_iResIndex 0x4b0.
CBotCommandInline EmpiresCommanderBuildProbeCommand("empires_commander_build", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* output = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBot* base = CBots::getFirstNormalBot();
		if (base == nullptr || args.size() < 1 || CBotGlobals::getCurrentMod() == nullptr ||
			CBotGlobals::getCurrentMod()->getBotType() != BOTTYPE_EMPIRES)
			return COMMAND_ERROR;
		const auto rawEntity = [](edict_t* e) -> const char*
		{
			IServerUnknown* unknown = e != nullptr ? e->GetUnknown() : nullptr;
			CBaseEntity* entity = unknown != nullptr ? unknown->GetBaseEntity() : nullptr;
			return reinterpret_cast<const char*>(entity);
		};
		const auto isClass = [](edict_t* e, const char* name, size_t n)
		{
			return e != nullptr && !e->IsFree() && e->GetClassName() != nullptr && std::strncmp(e->GetClassName(), name, n) == 0;
		};
		if (std::strcmp(args[0], "list") == 0)
		{
			// CEmpTeam is never a player slot; a client slot punted while the server hibernated keeps a
			// dangling networkable (crash in ReadTeamTickets on wake-up, run 20261126-114539).
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				const char* raw = rawEntity(e);
				if (raw == nullptr)
					continue;
				ServerClass* serverClass = e->GetNetworkable() != nullptr ? e->GetNetworkable()->GetServerClass() : nullptr;
				if (serverClass != nullptr && std::strcmp(serverClass->GetName(), "CEmpTeam") == 0)
				{
					// CEmpTeam +0x3b4 -> resource ledger, +0x30 float = team resources (emp_give_resources 0x647610).
					const char* ledger = *reinterpret_cast<const char* const*>(raw + 0x3b4);
					Msg("[RCBOT2] commander-build list team_entity=%d team=%d resources=%.1f\n", i,
						*reinterpret_cast<const int*>(raw + 0x3a8), ledger != nullptr ? *reinterpret_cast<const float*>(ledger + 0x30) : -1.0f);
					continue;
				}
				const Vector o = CBotGlobals::entityOrigin(e);
				if (isClass(e, "emp_building_", 13))
					Msg("[RCBOT2] commander-build list building=%d class=%s team=%d pos=(%.0f %.0f %.0f)\n", i, e->GetClassName(),
						*reinterpret_cast<const int*>(raw + 0x208), o.x, o.y, o.z);
				else if (isClass(e, "emp_resource_point", 19))
					Msg("[RCBOT2] commander-build list resource_point=%d res_index=%d enabled=%d pos=(%.0f %.0f %.0f)\n", i,
						*reinterpret_cast<const int*>(raw + 0x4b0), int(*reinterpret_cast<const unsigned char*>(raw + 0x4a8)), o.x, o.y, o.z);
			}
			return COMMAND_ACCESSED;
		}
		if (std::strcmp(args[0], "helper") == 0 && args.size() >= 4)
		{
			// Moves the first other bot (needs sv_cheats 1) to test the "friendly player within 4000" rule.
			for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
			{
				CBot* other = CBots::getBot(slot);
				if (other == nullptr || other == base || !other->inUse() || other->getEdict() == nullptr)
					continue;
				char setpos[160];
				snprintf(setpos, sizeof(setpos), "setpos %s %s %s\n", args[1], args[2], args[3]);
				helpers->ClientCommand(other->getEdict(), setpos);
				const Vector o = CBotGlobals::entityOrigin(other->getEdict());
				Msg("[RCBOT2] commander-build helper slot=%d pos=(%.0f %.0f %.0f)\n", slot, o.x, o.y, o.z);
				break;
			}
			return COMMAND_ACCESSED;
		}
		CBotEmpires* bot = static_cast<CBotEmpires*>(base);
		edict_t* vehicle = bot->m_hCommanderEntryVehicle.get();
		if (bot->m_iCommanderEntryStage != 4 || vehicle == nullptr)
		{
			CBotGlobals::botMessage(output, 0, "commander_build result=NOT_COMMANDER");
			return COMMAND_ACCESSED;
		}
#ifdef __linux__
		if (std::strcmp(args[0], "obstruct_grid") == 0 && args.size() >= 4)
		{
			// Diagnosis only: GetAllObstructions on a ground grid around the CV (obstruct_grid <type> <radius> <step>);
			// prints a flag histogram and every position without any flag except 0x8 (too far from the CV).
			Dl_info info{};
			const unsigned char expected[8] = {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec};
			extern IServerGameDLL* servergamedll;
			if (servergamedll == nullptr || dladdr(*reinterpret_cast<void**>(servergamedll), &info) == 0 || info.dli_fbase == nullptr || enginetrace == nullptr)
				return COMMAND_ERROR;
			const unsigned char* fn = static_cast<const unsigned char*>(info.dli_fbase) + 0x387ed0;
			if (std::memcmp(fn, expected, sizeof(expected)) != 0)
				return COMMAND_ACCESSED;
			using Fn = int (*)(int, const Vector&, int, int, const void*, const Vector&, void*);
			const float radius = static_cast<float>(std::atof(args[2]));
			const float step = std::max(50.0f, static_cast<float>(std::atof(args[3])));
			const Vector cv = CBotGlobals::entityOrigin(vehicle);
			// Optional scan centre (obstruct_grid <type> <radius> <step> [x y]); distances are still measured from the CV.
			const Vector centre = args.size() >= 6 ? Vector(static_cast<float>(std::atof(args[4])), static_cast<float>(std::atof(args[5])), cv.z) : cv;
			const int team = CClassInterface::getTeam(bot->getEdict());
			CTraceFilterWorldOnly filter;
			int total = 0, clear = 0, noGround = 0;
			std::map<int, int> histogram;
			for (float dx = -radius; dx <= radius; dx += step)
				for (float dy = -radius; dy <= radius; dy += step)
				{
					if (dx * dx + dy * dy > radius * radius)
						continue;
					const Vector top(centre.x + dx, centre.y + dy, cv.z + 800.0f);
					Ray_t ray;
					ray.Init(top, top - Vector(0, 0, 1500.0f));
					trace_t hit;
					enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &hit);
					if (hit.startsolid || hit.fraction >= 1.0f)
					{
						++noGround;
						continue;
					}
					const Vector at = hit.endpos + Vector(0, 0, 1.0f);
					const int flags = reinterpret_cast<Fn>(const_cast<unsigned char*>(fn))(100000, at, std::atoi(args[1]), team, nullptr, cv, nullptr);
					++total;
					++histogram[flags];
					if ((flags & ~0x8) == 0)
					{
						++clear;
						Msg("[RCBOT2] commander-build obstruct_grid clear pos=(%.0f %.0f %.0f) cv_dist=%.0f flags=0x%x\n", at.x, at.y, at.z, (at - cv).Length(), flags);
					}
				}
			Msg("[RCBOT2] commander-build obstruct_grid done type=%s radius=%.0f step=%.0f total=%d no_ground=%d clear_except_far=%d\n", args[1], radius, step, total, noGround, clear);
			for (const auto& entry : histogram)
				Msg("[RCBOT2] commander-build obstruct_grid flags=0x%x count=%d\n", entry.first, entry.second);
			return COMMAND_ACCESSED;
		}
		if (std::strcmp(args[0], "obstruct") == 0 && args.size() >= 5)
		{
			// Diagnosis only: calls EmpBuildingPlacementRules::GetAllObstructions (server_srv.so 0x387ed0) like
			// Command_emp_build does (resources, origin, type, team, ignore, CV origin, resource point).
			Dl_info info{};
			const unsigned char expected[8] = {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec};
			extern IServerGameDLL* servergamedll;
			if (servergamedll == nullptr || dladdr(*reinterpret_cast<void**>(servergamedll), &info) == 0 || info.dli_fbase == nullptr)
				return COMMAND_ERROR;
			const unsigned char* fn = static_cast<const unsigned char*>(info.dli_fbase) + 0x387ed0;
			if (std::memcmp(fn, expected, sizeof(expected)) != 0)
			{
				Msg("[RCBOT2] commander-build obstruct result=BINARY_MISMATCH\n");
				return COMMAND_ACCESSED;
			}
			using Fn = int (*)(int, const Vector&, int, int, const void*, const Vector&, void*);
			const Vector at(static_cast<float>(std::atof(args[2])), static_cast<float>(std::atof(args[3])), static_cast<float>(std::atof(args[4])));
			const Vector cv = CBotGlobals::entityOrigin(vehicle);
			const int flags = reinterpret_cast<Fn>(const_cast<unsigned char*>(fn))(100000, at, std::atoi(args[1]), CClassInterface::getTeam(bot->getEdict()), nullptr, cv, nullptr);
			// Same box as GetBuildingHullTrace for type 5 (radius 600, height 500 NF / 300 BE, mask 0x4600402b):
			// names the entity that makes flag 0x2 appear (worldspawn = terrain/map geometry).
			extern IServerGameEnts* servergameents;
			const char* hitClass = "none";
			int hitIndex = -1;
			float hitFrac = 1.0f, hitNz = 0.0f;
			bool hitStart = false;
			if (enginetrace != nullptr && servergameents != nullptr)
			{
				class AllFilter : public CTraceFilter
				{
				public:
					bool ShouldHitEntity(IHandleEntity*, int) override { return true; }
				} filter;
				const float height = CClassInterface::getTeam(bot->getEdict()) == 2 ? 500.0f : 300.0f;
				Ray_t ray;
				ray.Init(at, at, Vector(-600, -600, 0), Vector(600, 600, height));
				trace_t hit;
				enginetrace->TraceRay(ray, 0x4600402b, &filter, &hit);
				if (hit.startsolid || hit.fraction < 1.0f)
				{
					edict_t* e = hit.m_pEnt != nullptr ? servergameents->BaseEntityToEdict(hit.m_pEnt) : nullptr;
					hitClass = e != nullptr ? e->GetClassName() : "unknown";
					hitIndex = e != nullptr ? engine->IndexOfEdict(e) : -1;
					hitFrac = hit.fraction;
					hitNz = hit.plane.normal.z;
					hitStart = hit.startsolid;
				}
			}
			char variants[8] = "----";
			if (enginetrace != nullptr)
			{
				class VarFilter : public CTraceFilter
				{
				public:
					bool ShouldHitEntity(IHandleEntity*, int) override { return true; }
				} vfilter;
				const float h = CClassInterface::getTeam(bot->getEdict()) == 2 ? 500.0f : 300.0f;
				// A full box from z+40 (above floor roughness), B floor slab 0..40, C half-width box from z+40, D box from z+200 (ceiling check)
				const float vmins[4][3] = {{-600, -600, 40}, {-600, -600, 0}, {-300, -300, 40}, {-600, -600, 200}};
				const float vmaxs[4][3] = {{600, 600, h}, {600, 600, 40}, {300, 300, h}, {600, 600, h}};
				for (int v = 0; v < 4; ++v)
				{
					Ray_t vr;
					vr.Init(at, at, Vector(vmins[v][0], vmins[v][1], vmins[v][2]), Vector(vmaxs[v][0], vmaxs[v][1], vmaxs[v][2]));
					trace_t vh;
					enginetrace->TraceRay(vr, 0x4600402b, &vfilter, &vh);
					variants[v] = (vh.startsolid || vh.fraction < 1.0f) ? 'X' : '-';
				}
			}
			Msg("[RCBOT2] commander-build obstruct type=%s pos=(%.0f %.0f %.0f) cv_dist=%.0f flags=0x%x hull_hit=%s#%d variants=%s\n", args[1], at.x, at.y, at.z, (at - cv).Length(), flags, hitClass, hitIndex, variants);
			return COMMAND_ACCESSED;
		}
#endif
		const int type = std::atoi(args[0]);
		Vector pos = CBotGlobals::entityOrigin(vehicle);
		int resIndex = -1;
		if (args.size() >= 4)
			pos = Vector(static_cast<float>(std::atof(args[1])), static_cast<float>(std::atof(args[2])), static_cast<float>(std::atof(args[3])));
		else if (type != 0 && args.size() >= 2 && args[1] != nullptr && std::strcmp(args[1], "spot") == 0)
		{
			// Test aid: a valid spot for this type near the CV (same native search as the commander economy).
			EmpiresNativePlacement::ContextIdentity context;
			EmpiresSpatial::SearchResult search;
			const bool captured = EmpiresNativePlacement::CaptureContext(engine->IndexOfEdict(bot->getEdict()), context);
			for (int ring = 0; captured && ring < 5 && !(search.status == EmpiresSpatial::Status::Found && search.hasSelected); ++ring)
			{
				Vector center = pos;
				if (ring > 0)
				{
					const float angle = (ring - 1) * 1.5707963f;
					center += Vector(std::cos(angle), std::sin(angle), 0.0f) * 1500.0f; // as the commander economy
				}
				EmpiresSpatial::Request request;
				request.origin = {center.x, center.y, center.z};
				request.radius = 2048.0f;
				request.buildingType = type;
				request.invalidSkipRadius = 250.0f;
				search = EmpiresSpatial::SearchBarracksCandidates(context, request);
			}
			if (search.status != EmpiresSpatial::Status::Found || !search.hasSelected)
			{
				Msg("[RCBOT2] commander-build type=%d spot=none search=%s\n", type, EmpiresSpatial::Name(search.status));
				return COMMAND_ACCESSED;
			}
			pos = Vector(search.selected.origin.x, search.selected.origin.y, search.selected.origin.z);
		}
		else if (type == 0)
		{
			float best = 1e30f;
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
			{
				edict_t* e = engine->PEntityOfEntIndex(i);
				const char* raw = rawEntity(e);
				if (raw == nullptr || !isClass(e, "emp_resource_point", 19) || *reinterpret_cast<const unsigned char*>(raw + 0x4a8) == 0)
					continue;
				const Vector o = CBotGlobals::entityOrigin(e);
				const float d = (o - CBotGlobals::entityOrigin(vehicle)).Length();
				if (d < best) { best = d; pos = o; resIndex = *reinterpret_cast<const int*>(raw + 0x4b0); }
			}
		}
		const auto listBuildings = [&]()
		{
			std::vector<int> indices;
			for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
				if (isClass(engine->PEntityOfEntIndex(i), "emp_building_", 13))
					indices.push_back(i);
			return indices;
		};
		const auto before = listBuildings();
		char command[160];
		if (type == 0 && resIndex >= 0)
			snprintf(command, sizeof(command), "emp_build 0 %d 0\n", resIndex);
		else
		{
			const float yaw = type == 5 && args[4] != nullptr && std::strcmp(args[4], "auto") == 0 ?
				EmpiresFactoryExitYaw(pos, CClassInterface::getTeam(bot->getEdict()), false) : 0.0f;
			snprintf(command, sizeof(command), "emp_build %d %.9g %.9g %.9g %.1f\n",
				type, double(pos.x), double(pos.y), double(pos.z), double(yaw));
		}
		helpers->ClientCommand(bot->getEdict(), command);
		const auto after = listBuildings();
		for (const int index : after)
			if (std::find(before.begin(), before.end(), index) == before.end())
			{
				edict_t* e = engine->PEntityOfEntIndex(index);
				Msg("[RCBOT2] commander-build created=%d class=%s rejected=%d\n", index, e->GetClassName(), EmpiresEntityMarkedForDeletion(e) ? 1 : 0);
			}
		Msg("[RCBOT2] commander-build type=%d res_index=%d pos=(%.0f %.0f %.0f) cv_dist=%.0f before=%d after=%d\n",
			type, resIndex, pos.x, pos.y, pos.z, (pos - CBotGlobals::entityOrigin(vehicle)).Length(), int(before.size()), int(after.size()));
		return COMMAND_ACCESSED;
}, R"(usage "empires_commander_build <type> [x y z [auto] | spot] | list" : seated Commander bot submits emp_build (probe); type 5 with auto uses the automatic factory yaw, and list shows buildings and resource points)");

CBotCommandInline EmpiresVehicleFactoryDriveOnceCommand("empires_vehicle_vf_drive_once", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if ((args[0] != nullptr && *args[0] != '\0') ||
			(args.size() > 1 && args[1] != nullptr && *args[1] != '\0'))
		{
			CBotGlobals::botMessage(pEntity, 0,
				"usage: rcbotd empires_vehicle_vf_drive_once");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_vehicle_vf_drive_once requires one alive normal Empires bot");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (pBot->requestEmpiresVehicleFactoryDriveOnce())
			CBotGlobals::botMessage(pEntity, 0,
				"Empires vehicle VF action armed: complete tutorial normally, enter constructed team VF, purchase through normal commands, then RunPlayerMove drive/stop/exit");
		else
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires vehicle VF action rejected; use an alive normal bot and a constructed team VF");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_vehicle_vf_drive_once" : development-only bounded bot tutorial completion, VF purchase, vehicle entry, normal UserCmd drive/stop/exit; no vehicle state writes)");

CBotCommandInline EmpiresNormalRespawnProbeKillCommand("empires_normal_respawn_probe_kill", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getFirstNormalBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES ||
			pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_normal_respawn_probe_kill requires one normal Empires bot");
			return COMMAND_ACCESSED;
		}

		pBaseBot->kill();
		CBotGlobals::botMessage(pEntity, 0,
			"Empires normal respawn probe kill issued through the bot client command path");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_normal_respawn_probe_kill" : development-only kill of the first normal Empires bot to test one bounded respawn cycle)");

CBotCommandInline EmpiresBootstrapCountCheckCommand("empires_bootstrap_countcheck", CMD_ACCESS_CONSOLE_ONLY,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		(void)args;
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		CBotMod* pMod = CBotGlobals::getCurrentMod();

		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES || !CBots::hasBootstrapOnlyBot())
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires countcheck requires an active Empires bootstrap fake client");
			return COMMAND_ACCESSED;
		}

		/* This is a one-shot developer diagnostic; it does not reconcile quota
		 * and it does not create, remove, or otherwise alter a client. */
		CBotGlobals::setCountCheckDiagnostics(true);
		const int iClients = CBotGlobals::numClients();
		const int iPlayersPlaying = CBotGlobals::numPlayersPlaying();
		CBotGlobals::setCountCheckDiagnostics(false);

		const bool bAssertionsPassed = iClients >= 0 &&
			iClients <= CBotGlobals::maxClients() &&
			iPlayersPlaying >= 0 &&
			iPlayersPlaying <= iClients;

		logger->Log(LogLevel::INFO,
			"Empires countcheck result: numClients=%d numPlayersPlaying=%d assertions=%s",
			iClients, iPlayersPlaying, bAssertionsPassed ? "PASS" : "FAIL");
		CBotGlobals::botMessage(pEntity, bAssertionsPassed ? 0 : 1,
			"Empires countcheck: numClients=%d numPlayersPlaying=%d assertions=%s",
			iClients, iPlayersPlaying, bAssertionsPassed ? "PASS" : "FAIL");

		return COMMAND_ACCESSED;
		}, R"(usage "empires_bootstrap_countcheck" : developer-only client-count safety diagnostic; requires the bootstrap fake)");

CBotCommandInline EmpiresBootstrapJoinTeamCommand("empires_bootstrap_jointeam", CMD_ACCESS_BOT | CMD_ACCESS_DEDICATED,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		const bool hasOnlyParserSentinel = args.size() == 2 &&
			(args[1] == nullptr || *args[1] == '\0');
		if ((args.size() != 1 && !hasOnlyParserSentinel) ||
			args[0] == nullptr || *args[0] == '\0')
		{
			CBotGlobals::botMessage(pEntity, 0, "usage: rcbotd empires_bootstrap_jointeam <nf|be>");
			return COMMAND_ACCESSED;
		}

		const char* requestedName = args[0];
		auto equalsIgnoreCase = [](const char* value, const char* expected)
		{
			while (*value != '\0' && *expected != '\0')
			{
				if (std::tolower(static_cast<unsigned char>(*value)) !=
					std::tolower(static_cast<unsigned char>(*expected)))
				{
					return false;
				}
				++value;
				++expected;
			}
			return *value == '\0' && *expected == '\0';
		};

		int requestedTeam = -1;
		if (equalsIgnoreCase(requestedName, "nf"))
		{
			requestedTeam = EMP_TEAM_NF;
		}
		else if (equalsIgnoreCase(requestedName, "be"))
		{
			requestedTeam = EMP_TEAM_BE;
		}

		if (requestedTeam < 0)
		{
			CBotGlobals::botMessage(pEntity, 0, "error: team must be exactly nf or be");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getBootstrapOnlyBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES || pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_bootstrap_jointeam requires exactly one active Empires bootstrap fake client");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		EmpiresBootstrapPlayerState before;
		if (!pBot->readBootstrapPlayerState(before))
		{
			CBotGlobals::botMessage(pEntity, 0, "error: bootstrap client state is unavailable");
			return COMMAND_ACCESSED;
		}

		if (before.team == requestedTeam)
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: bootstrap client is already on requested team %d", requestedTeam);
			return COMMAND_ACCESSED;
		}

		if (!pBot->requestBootstrapTeam(requestedTeam))
		{
			CBotGlobals::botMessage(pEntity, 0, "error: Empires team request was rejected");
			return COMMAND_ACCESSED;
		}

		CBotGlobals::botMessage(pEntity, 0,
			"Empires bootstrap jointeam %s issued for team %d; observing callbacks/frames",
			requestedName, requestedTeam);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_bootstrap_jointeam <nf|be>" : assigns the controlled bootstrap client to NF or BE without class/spawn commands)");

CBotCommandInline EmpiresBootstrapJoinClassCommand("empires_bootstrap_joinclass", CMD_ACCESS_BOT | CMD_ACCESS_DEDICATED,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		const bool hasOnlyParserSentinel = args.size() == 2 &&
			(args[1] == nullptr || *args[1] == '\0');
		const edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		if ((args.size() != 1 && !hasOnlyParserSentinel) ||
			args[0] == nullptr || *args[0] == '\0')
		{
			CBotGlobals::botMessage(const_cast<edict_t*>(pEntity), 0,
				"usage: rcbotd empires_bootstrap_joinclass <scout|rifleman|grenadier|engineer|0..3>");
			return COMMAND_ACCESSED;
		}

		int requestedClass = -1;
		const char* canonicalName = nullptr;
		if (!parseEmpiresBootstrapClass(args[0], requestedClass, canonicalName))
		{
			CBotGlobals::botMessage(const_cast<edict_t*>(pEntity), 0,
				"error: class must be one of scout, rifleman, grenadier, engineer or numeric 0..3");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getBootstrapOnlyBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES || pBaseBot == nullptr)
		{
			CBotGlobals::botMessage(const_cast<edict_t*>(pEntity), 0,
				"error: empires_bootstrap_joinclass requires exactly one active Empires bootstrap fake client");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		EmpiresBootstrapPlayerState before;
		if (!pBot->readBootstrapPlayerState(before))
		{
			CBotGlobals::botMessage(const_cast<edict_t*>(pEntity), 0,
				"error: bootstrap client state is unavailable");
			return COMMAND_ACCESSED;
		}

		if (before.team != EMP_TEAM_NF && before.team != EMP_TEAM_BE)
		{
			CBotGlobals::botMessage(const_cast<edict_t*>(pEntity), 0,
				"error: bootstrap client must already be on NF or BE");
			return COMMAND_ACCESSED;
		}

		if (before.classKnown && before.classId == requestedClass)
		{
			CBotGlobals::botMessage(const_cast<edict_t*>(pEntity), 0,
				"error: bootstrap client already has class %d (%s)", requestedClass, canonicalName);
			return COMMAND_ACCESSED;
		}

		CBotGlobals::botMessage(const_cast<edict_t*>(pEntity), 0,
			"Empires bootstrap class before team=%d class=%d class_readback=%d class_desired=%d class_desired_readback=%d alive=%d dead=%d",
			before.team, before.classId, before.classKnown ? 1 : 0,
			before.classDesired ? 1 : 0, before.classDesiredKnown ? 1 : 0,
			before.alive ? 1 : 0, before.dead ? 1 : 0);

		if (!pBot->requestBootstrapClass(requestedClass))
		{
			CBotGlobals::botMessage(const_cast<edict_t*>(pEntity), 0,
				"error: Empires class request was rejected");
			return COMMAND_ACCESSED;
		}

		CBotGlobals::botMessage(const_cast<edict_t*>(pEntity), 0,
			"Empires bootstrap changeclass %s (%d) issued; observing class readback and spawn state",
			canonicalName, requestedClass);
		return COMMAND_ACCESSED;
	}, R"(usage "empires_bootstrap_joinclass <scout|rifleman|grenadier|engineer|0..3>" : selects one verified infantry class without an explicit spawn command)");

	CBotCommandInline EmpiresBootstrapSpawnCommand("empires_bootstrap_spawn", CMD_ACCESS_BOT | CMD_ACCESS_DEDICATED,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		bool hasNonEmptyArgument = false;
		for (const char* argument : args)
		{
			if (argument != nullptr && *argument != '\0')
			{
				hasNonEmptyArgument = true;
				break;
			}
		}
		if (hasNonEmptyArgument)
		{
			CBotGlobals::botMessage(pEntity, 0, "usage: rcbotd empires_bootstrap_spawn");
			return COMMAND_ACCESSED;
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getBootstrapOnlyBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES || pBaseBot == nullptr ||
			!pBaseBot->isBootstrapOnly())
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_bootstrap_spawn requires exactly one active Empires bootstrap fake client");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (!pBot->requestBootstrapSpawn())
		{
			CBotGlobals::botMessage(pEntity, 0, "error: Empires bootstrap spawn was rejected");
			return COMMAND_ACCESSED;
		}

		CBotGlobals::botMessage(pEntity, 0,
			"Empires bootstrap spawn issued through the public emp_spawn_select path; observing state transition");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_bootstrap_spawn" : selects one live public Empires spawn point for the verified infantry bootstrap client)");

CBotCommandInline EmpiresBootstrapSpawnConfirmCommand("empires_bootstrap_spawnconfirm", CMD_ACCESS_BOT | CMD_ACCESS_DEDICATED,
	[](const CClient* pClient, const BotCommandArgs& args)
	{
		edict_t* pEntity = pClient != nullptr ? pClient->getPlayer() : nullptr;
		for (const char* argument : args)
		{
			if (argument != nullptr && *argument != '\0')
			{
				CBotGlobals::botMessage(pEntity, 0, "usage: rcbotd empires_bootstrap_spawnconfirm");
				return COMMAND_ACCESSED;
			}
		}

		CBotMod* pMod = CBotGlobals::getCurrentMod();
		CBot* pBaseBot = CBots::getBootstrapOnlyBot();
		if (pMod == nullptr || pMod->getBotType() != BOTTYPE_EMPIRES || pBaseBot == nullptr ||
			!pBaseBot->isBootstrapOnly())
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: empires_bootstrap_spawnconfirm requires exactly one active Empires bootstrap fake client");
			return COMMAND_ACCESSED;
		}

		CBotEmpires* pBot = static_cast<CBotEmpires*>(pBaseBot);
		if (!pBot->requestBootstrapSpawnConfirmation())
		{
			CBotGlobals::botMessage(pEntity, 0,
				"error: Empires bootstrap spawn confirmation was rejected");
			return COMMAND_ACCESSED;
		}

		CBotGlobals::botMessage(pEntity, 0,
			"Empires bootstrap spawn confirmation armed; exactly one IN_ATTACK press and one neutral release will be submitted");
		return COMMAND_ACCESSED;
	}, R"(usage "empires_bootstrap_spawnconfirm" : submits exactly one controlled IN_ATTACK press followed by one neutral release)");

bool CBotCommand::hasAccess(const CClient* pClient) const
{
	if (m_iAccessLevel & CMD_ACCESS_CONSOLE_ONLY)
		return false;

	// check access level excluding dedicated server flag
	const int iClientAccessLevel = this->m_iAccessLevel & ~CMD_ACCESS_DEDICATED;
	return (iClientAccessLevel & pClient->accessLevel()) == iClientAccessLevel;
}

bool CBotCommand::isCommand(const char* szCommand) const
{
	return FStrEq(szCommand, m_szCommand);
}

eBotCommandResult CBotCommand::execute(CClient* pClient, const BotCommandArgs& args) {
	return COMMAND_NOT_FOUND;
}

eBotCommandResult CBotSubcommands::execute(CClient* pClient, const BotCommandArgs& args) {
	BotCommandArgs mutableArgs = args; // Make a mutable copy
	const char* subcmd = mutableArgs[0];
	mutableArgs.pop_front();

	for (CBotCommand* const cmd : m_theCommands) {
		if (!cmd->isCommand(subcmd)) {
			continue;
		}

		if (pClient && !cmd->hasAccess(pClient)) {
			return COMMAND_REQUIRE_ACCESS;
		}
		if (!pClient && !cmd->canbeUsedDedicated()) {
			CBotGlobals::botMessage(nullptr, 0, "Sorry, this command cannot be used on a dedicated server");
			return COMMAND_ERROR;
		}

		// shift arguments and call
		const eBotCommandResult result = cmd->execute(pClient, mutableArgs);
		if (result == COMMAND_ERROR) {
			cmd->printHelp(pClient ? pClient->getPlayer() : nullptr);
		}
		return COMMAND_ACCESSED;
	}

	printHelp(pClient ? pClient->getPlayer() : nullptr);
	return COMMAND_NOT_FOUND;
}

void CBotSubcommands::printCommand(edict_t* pPrintTo, const int indent)
{
	if (indent)
	{
		constexpr int maxIndent = 64;
		char szIndent[maxIndent] = {};

		for (int i = 0; (i < (indent * 2)) && (i < maxIndent - 1); i++)
			szIndent[i] = ' ';

		CBotGlobals::botMessage(pPrintTo, 0, "%s[%s]", szIndent, m_szCommand);
	}
	else
		CBotGlobals::botMessage(pPrintTo, 0, "[%s]", m_szCommand);

	for (CBotCommand* const& m_theCommand : m_theCommands)
	{
		m_theCommand->printCommand(pPrintTo, indent + 1);
	}
}

void CBotSubcommands::printHelp(edict_t* pPrintTo) {
	this->printCommand(pPrintTo);
}

CBotCommandInline PrintCommands("printcommands", CMD_ACCESS_DEDICATED, [](const CClient* pClient, const BotCommandArgs& args)
{
	if ( pClient != nullptr)
	{
		CBotGlobals::botMessage(pClient->getPlayer(),0,"All bot commands:");
		CBotGlobals::m_pCommands->printCommand(pClient->getPlayer());
	}
	else
	{
		CBotGlobals::botMessage(nullptr,0,"All bot commands:");
		CBotGlobals::m_pCommands->printCommand(nullptr);
	}

	return COMMAND_ACCESSED;
});

///////////////////////////////////////////

void CBotCommand::printCommand(edict_t* pPrintTo, const int indent)
{
	if (indent)
	{
		constexpr int maxIndent = 64;
		char szIndent[maxIndent];
		int i;

		for (i = 0; (i < (indent * 2)) && (i < maxIndent - 1); i++)
			szIndent[i] = ' ';

		szIndent[maxIndent - 1] = 0;
		szIndent[i] = 0;

		if (!pPrintTo && !canbeUsedDedicated())
			CBotGlobals::botMessage(pPrintTo, 0, "%s%s [can't use]", szIndent, m_szCommand);
		else
			CBotGlobals::botMessage(pPrintTo, 0, "%s%s", szIndent, m_szCommand);
	}
	else
	{
		if (!pPrintTo && !canbeUsedDedicated())
			CBotGlobals::botMessage(pPrintTo, 0, "%s [can't use]", m_szCommand);
		else
			CBotGlobals::botMessage(pPrintTo, 0, m_szCommand);
	}
}

void CBotCommand::printHelp(edict_t* pPrintTo)
{
	if (m_szHelp)
		CBotGlobals::botMessage(pPrintTo, 0, m_szHelp);
	else
		CBotGlobals::botMessage(pPrintTo, 0, "Sorry, no help for this command (yet)");

	//return;
}

CBotCommandInline CTestCommand("test", 0, [](CClient* pClient, const BotCommandArgs& args)
{
	// for developers
	// first argument is at args[0]
	return COMMAND_NOT_FOUND;
});

CBotSubcommands* CBotGlobals::m_pCommands = new CBotSubcommands("rcbot", CMD_ACCESS_DEDICATED, {
	&WaypointSubcommands,
	&AddBotCommand,
	&ControlCommand,
	&PathWaypointSubcommands,
	&DebugSubcommands,
	&PrintCommands,
	&ConfigSubcommands,
	&KickBotCommand,
	&EmpiresAnalysisStartCommand,
	&EmpiresAnalysisStopCommand,
	&EmpiresAnalysisMarkCommand,
	&EmpiresAnalysisStatusCommand,
	&EmpiresBootstrapCreateCommand,
	&EmpiresBootstrapRemoveCommand,
	&EmpiresNormalPathOnceCommand,
	&EmpiresAnchorResolverProbeCommand,
	&EmpiresNavMeshProviderOnceCommand,
	&EmpiresNavMeshProviderStatusCommand,
	&EmpiresNavMeshProviderCancelCommand,
	&EmpiresNavigationRequestCommand,
	&EmpiresNavigationNavMeshFixtureCommand,
	&EmpiresNavigationBarracksOnceCommand,
	&EmpiresNavigationIntentOnceCommand,
	&EmpiresNavigationIntentWriterProbeCommand,
	&EmpiresNavigationIntentStaleWriterProbeCommand,
	&EmpiresNavigationIntentStatusCommand,
	&EmpiresNavigationIntentCancelCommand,
	&EmpiresLocalExplorerRequestCommand,
	&EmpiresLocalExplorerBarracksCommand,
	&EmpiresLocalExplorerBudgetCommand,
	&EmpiresLocalExplorerUnreachableCommand,
	&EmpiresLocalExplorerStatusCommand,
	&EmpiresLocalExplorerCancelCommand,
	&EmpiresCommanderMoveTestCommand,
	&EmpiresVehicleScaleHpCommand,
	&EmpiresVehicleSetAmmoCommand,
	&EmpiresVehicleCrateCommand,
	&EmpiresVehicleObstacleCommand,
	&EmpiresTestWagesCommand,
	&EmpiresTestEngibotCommand,
	&EmpiresTestUnitOrderCommand,
	&EmpiresTestHelpHumanCommand,
	&EmpiresGuardTestCommand,
	&EmpiresCommanderYieldTestCommand,
	&EmpiresBotClientCommandTest,
	&EmpiresExitBlockTestCommand,
	&EmpiresKillZoneTestCommand,
	&EmpiresNearDeathTestCommand,
	&EmpiresRecoveryCaptureAcceptanceCommand,
	&EmpiresRecoverySquadAcceptanceCommand,
	&EmpiresScoutBuildingVisibilityScanCommand,
	&EmpiresEnemyLosFixtureCommand,
	&EmpiresEnemyLosFixtureCleanupCommand,
	&EmpiresEnemyCombatFixtureCommand,
	&EmpiresScoutPriorityFixtureCommand,
	&EmpiresScoutPriorityFixtureCleanupCommand,
	&EmpiresEnemyApproachFixtureCommand,
	&EmpiresScoutBinocularOnceCommand,
	&EmpiresScoutBinocularBuildingOnceCommand,
	&EmpiresScoutSkillObservationCommand,
	&EmpiresSquadObservationCommand,
	&EmpiresSquadMakeLeadCommand,
	&EmpiresSquadMakeLeadStatusCommand,
	&EmpiresMortarSmokeCommand,
	&EmpiresMortarFireOnceCommand,
	&EmpiresMineFireOnceCommand,
	&EmpiresMineTriggerOnceCommand,
	&EmpiresMortarAimOnceCommand,
	&EmpiresEngineerRepairFixturePrepareCommand,
	&EmpiresEngineerRepairFixtureDamageCommand,
	&EmpiresEngineerRepairVisibilityScanCommand,
	&EmpiresEngineerRepairVisibilityAllBuildingsCommand,
	&EmpiresEngineerRepairOnceCommand,
	&EmpiresEngineerResupplyOnceCommand,
	&EmpiresEngineerResupplyGoalOnceCommand,
	&EmpiresEngineerBuildSelectionOnceCommand,
	&EmpiresEngineerBuildOnceCommand,
	&EmpiresEngineerBuildAtCommand,
	&EmpiresVehicleInventoryCommand,
	&EmpiresVehicleFactoryDriveOnceCommand,
	&EmpiresNavScanCommand,
	&EmpiresCommanderEntryCommand,
	&EmpiresCommanderPlaceBarracksCommand,
	&EmpiresCommanderBuildProbeCommand,
	&EmpiresNormalRespawnProbeKillCommand,
	&EmpiresBootstrapCountCheckCommand,
	&EmpiresBootstrapJoinTeamCommand,
	&EmpiresBootstrapJoinClassCommand,
	&EmpiresBootstrapSpawnCommand,
	&EmpiresBootstrapSpawnConfirmCommand,
	&UserSubcommands,
	&UtilSubcommands
});
