// TODO 5.4 / 5.8: the seated bot commander gives its bots orders the way a human does in the commander
// view (emp_unit_order_list + emp_unit_order, reverse-engineering/notes/commander-order-types.md).
// The team director (bot_empires_team_director.cpp) splits the free infantry into push / hold / defense
// and names the goals; here each role group gets "attack location" (type 2) on its goal, so the bots
// spread over up to two push goals, hold near a front building and defend the base. Type 3 on enemies is
// dropped by the game for a bot commander (+0x14ec); engineers already get build jobs directly. Bots
// with an order of someone else (a human) are never touched.
#include "bot.h"
#include "bot_cvars.h"
#include "bot_empires.h"
#include "bot_empires_native_placement.h"
#include "bot_empires_team_director.h"
#include "bot_getprop.h"
#include "server_class.h"
#include "bot_globals.h"
#include "bot_mods.h"
#include "bot_plugin_meta.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern IVEngineServer* engine;
extern IPlayerInfoManager* playerinfomanager;
extern IServerPluginHelpers* helpers;


namespace
{
constexpr int kAttackLocation = 2;
constexpr float kCheckSeconds = 2.0f;
constexpr float kSameSpot = 200.0f; // a goal that moved less than this keeps the order

struct TeamOrders
{
	int point = -1;            // entity of the last push goal (for the release log)
	float nextCheck = 0.0f;
	bool ordered[RCBOT_MAXPLAYERS + 1] = {};
	Vector spot[RCBOT_MAXPLAYERS + 1];
};
TeamOrders s_orders[4];

// Send prop of the player's server class (m_iCommandType / m_vCommandLocation of CEmpPlayer). The
// offset is looked up once per name and class (called every frame from the vehicle gate).
const char* PlayerProp(edict_t* e, const char* name, const ServerClass*& cachedClass, unsigned& cachedOffset)
{
	if (e == nullptr || e->IsFree() || e->GetUnknown() == nullptr || e->GetNetworkable() == nullptr)
		return nullptr;
	CBaseEntity* base = e->GetUnknown()->GetBaseEntity();
	ServerClass* serverClass = e->GetNetworkable()->GetServerClass();
	if (base == nullptr || serverClass == nullptr)
		return nullptr;
	if (serverClass != cachedClass)
	{
		cachedOffset = 0;
		if (!UTIL_FindSendPropInfo(serverClass, name, &cachedOffset))
			cachedOffset = 0;
		cachedClass = serverClass;
	}
	return cachedOffset != 0 ? reinterpret_cast<const char*>(base) + cachedOffset : nullptr;
}

// Our order is still the one the unit carries: type 2 at the spot we sent. A human squad leader also
// gives type 2 (GiveSquadOrder 0x65c1db), and a human commander may take over after our commander
// died; then the location differs and the order is not ours any more (expert review 21.11.).
bool StillOurs(const TeamOrders& orders, int client)
{
	if (!orders.ordered[client] || engine == nullptr)
		return false;
	edict_t* e = engine->PEntityOfEntIndex(client);
	static const ServerClass* typeClass = nullptr;
	static const ServerClass* locationClass = nullptr;
	static unsigned typeOffset = 0, locationOffset = 0;
	const char* type = PlayerProp(e, "m_iCommandType", typeClass, typeOffset);
	const char* location = PlayerProp(e, "m_vCommandLocation", locationClass, locationOffset);
	if (type == nullptr || location == nullptr || *reinterpret_cast<const int*>(type) != kAttackLocation)
		return false;
	const float* xyz = reinterpret_cast<const float*>(location);
	const float dx = xyz[0] - orders.spot[client].x;
	const float dy = xyz[1] - orders.spot[client].y;
	return dx * dx + dy * dy < 60.0f * 60.0f;
}
}

bool EmpiresCommanderOrderIsOurs(int team, int client)
{
	return team >= 0 && team <= 3 && client >= 1 && client <= RCBOT_MAXPLAYERS && StillOurs(s_orders[team], client);
}

void EmpiresCommanderOrdersLevelReset()
{
	for (TeamOrders& orders : s_orders)
		orders = TeamOrders();
}

// Abort (type 0) for every unit this commander ordered; used when the point is taken without a
// next one, after no progress, and before a human gets the seat.
void CBotEmpires::releaseCommanderOrders(int team, const char* reason)
{
	if (team < 0 || team > 3 || helpers == nullptr || engine == nullptr)
		return;
	TeamOrders& orders = s_orders[team];
	std::string list = "emp_unit_order_list";
	int count = 0;
	for (int i = 1; i <= gpGlobals->maxClients && count < 32; ++i)
	{
		if (!orders.ordered[i])
			continue;
		orders.ordered[i] = false;
		edict_t* unit = engine->PEntityOfEntIndex(i);
		int type = -1;
		if (unit == nullptr || unit->IsFree() || !readEmpiresIntProperty(unit, "m_iCommandType", type) ||
			type != kAttackLocation)
			continue; // someone else gave it a new order meanwhile
		list += " " + std::to_string(i);
		++count;
	}
	if (count == 0 && orders.point < 0)
		return;
	if (count > 0)
	{
		list += "\n";
		helpers->ClientCommand(m_pEdict, list.c_str());
		helpers->ClientCommand(m_pEdict, "emp_unit_order 0 0 0 0\n");
	}
	RCBOT_DIAG("[RCBOT2] commander-orders release team=%d point=%d units=%d reason=%s\n", team, orders.point, count, reason);
	orders.point = -1;
}

void CBotEmpires::tickCommanderOrders(int team, float now, const Vector& home)
{
	(void)home;
	if (!rcbot_empires_commander_orders.GetBool() || team < 2 || team > 3 || helpers == nullptr ||
		playerinfomanager == nullptr || m_iCommanderEntryStage != 4)
		return;
	TeamOrders& orders = s_orders[team];
	if (now < orders.nextCheck)
		return;
	orders.nextCheck = now + kCheckSeconds;
	const EmpiresTeamDirector::Board* board = EmpiresTeamDirectorBoard(team);
	if (board == nullptr)
		return;
	EmpiresNativePlacement::ContextIdentity context;
	bool seated = false;
	if (!EmpiresNativePlacement::CaptureContext(engine->IndexOfEdict(m_pEdict), context) ||
		!EmpiresNativePlacement::ReadCommanderMode(context, seated) || !seated)
		return;

	// One order per goal: units whose role goal differs from the spot they were last sent to.
	struct Group
	{
		Vector spot;
		const EmpiresTeamDirector::Goal* goal;
		int role;
		std::vector<int> units;
	};
	std::vector<Group> groups;
	std::string abortList = "emp_unit_order_list";
	int aborts = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i <= RCBOT_MAXPLAYERS && i < EmpiresTeamDirector::kMaxClients; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		IPlayerInfo* info = e != nullptr && !e->IsFree() ? playerinfomanager->GetPlayerInfo(e) : nullptr;
		if (info == nullptr || !info->IsConnected() || info->GetTeamIndex() != team || e == m_pEdict)
		{
			orders.ordered[i] = false;
			continue;
		}
		int type = -1;
		if (!readEmpiresIntProperty(e, "m_iCommandType", type))
			continue;
		if (orders.ordered[i] && !StillOurs(orders, i))
			orders.ordered[i] = false; // someone else ordered it (other type or spot), or the order ended
		const EmpiresTeamDirector::Goal* goal = EmpiresTeamDirector::RoleGoal(*board, i);
		if (goal == nullptr)
		{
			// No role any more (driver, engineer, foreign order): our old order goes.
			if (orders.ordered[i] && !info->IsDead() && aborts < 32)
			{
				abortList += " " + std::to_string(i);
				++aborts;
				orders.ordered[i] = false;
			}
			continue;
		}
		if (info->IsDead() || (type != 0 && !orders.ordered[i]))
			continue;
		const Vector spot(goal->pos[0], goal->pos[1], goal->pos[2]);
		if (orders.ordered[i] && (orders.spot[i] - spot).Length2D() < kSameSpot)
			continue; // already on its way
		Group* group = nullptr;
		for (Group& g : groups)
			if ((g.spot - spot).Length2D() < 1.0f)
				group = &g;
		if (group == nullptr)
		{
			groups.push_back({ spot, goal, board->role[i], {} });
			group = &groups.back();
		}
		if (group->units.size() < 32)
			group->units.push_back(i);
	}
	if (aborts > 0)
	{
		abortList += "\n";
		helpers->ClientCommand(m_pEdict, abortList.c_str());
		helpers->ClientCommand(m_pEdict, "emp_unit_order 0 0 0 0\n");
		RCBOT_DIAG("[RCBOT2] commander-orders release team=%d units=%d reason=role-ended\n", team, aborts);
	}
	for (const Group& group : groups)
	{
		std::string list = "emp_unit_order_list";
		for (const int i : group.units)
			list += " " + std::to_string(i);
		list += "\n";
		char command[96];
		snprintf(command, sizeof(command), "emp_unit_order %d %.0f %.0f %.0f\n", kAttackLocation,
			group.spot.x, group.spot.y, group.spot.z);
		helpers->ClientCommand(m_pEdict, list.c_str());
		helpers->ClientCommand(m_pEdict, command);
		for (const int i : group.units)
		{
			orders.ordered[i] = true;
			orders.spot[i] = group.spot;
			RCBOT_DIAG("[RCBOT2] commander-sector team=%d userid=%d role=%s goal=%s:%d spot=(%.0f %.0f %.0f) phase=%s\n",
				team, engine->GetPlayerUserId(engine->PEntityOfEntIndex(i)), EmpiresTeamDirector::RoleName(group.role),
				group.goal->kind, group.goal->entity, group.spot.x, group.spot.y, group.spot.z,
				EmpiresTeamDirector::PhaseName(board->phase));
		}
		if (group.role == EmpiresTeamDirector::ROLE_PUSH)
			orders.point = group.goal->entity;
		RCBOT_DIAG("[RCBOT2] commander-orders issue type=%d team=%d role=%s goal=%s:%d spot=(%.0f %.0f %.0f) units=%zu\n",
			kAttackLocation, team, EmpiresTeamDirector::RoleName(group.role), group.goal->kind, group.goal->entity,
			group.spot.x, group.spot.y, group.spot.z, group.units.size());
	}
}
