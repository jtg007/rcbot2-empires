// TODO 5.4: the seated bot commander gives its bots orders the way a human does in the commander
// view (emp_unit_order_list + emp_unit_order, reverse-engineering/notes/commander-order-types.md).
// First step: "attack location" (type 2) on the capture point the team should take next, so the
// attackers go to one flag together and the order shows on the minimap. Type 3 on enemies is
// dropped by the game for a bot commander (+0x14ec); engineers already get build jobs directly.
#include "bot.h"
#include "bot_cvars.h"
#include "bot_empires.h"
#include "bot_empires_native_placement.h"
#include "bot_getprop.h"
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

bool EmpiresCapturePointOnMesh(const CEmpiresMod::CapturePointObservation& point); // bot_empires_objective.cpp

namespace
{
constexpr int kAttackLocation = 2;
constexpr float kCheckSeconds = 15.0f;
constexpr float kNoProgressSeconds = 120.0f;

struct TeamOrders
{
	int point = -1;            // capture point entity the order is on
	Vector spot;
	float issuedAt = 0.0f;     // last time the point changed or units were added
	float nextCheck = 0.0f;
	bool ordered[RCBOT_MAXPLAYERS + 1] = {};
};
TeamOrders s_orders[4];

const CEmpiresMod::CapturePointObservation* FindPoint(const CEmpiresMod* mod, int entity)
{
	for (const CEmpiresMod::CapturePointObservation& point : mod->getCapturePoints())
		if (point.entityIndex == entity)
			return &point;
	return nullptr;
}
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
	if (!rcbot_empires_commander_orders.GetBool() || team < 2 || team > 3 || helpers == nullptr ||
		playerinfomanager == nullptr || m_iCommanderEntryStage != 4)
		return;
	TeamOrders& orders = s_orders[team];
	if (now < orders.nextCheck)
		return;
	orders.nextCheck = now + kCheckSeconds;
	const CEmpiresMod* mod = static_cast<const CEmpiresMod*>(CBotGlobals::getCurrentMod());
	if (mod == nullptr)
		return;
	EmpiresNativePlacement::ContextIdentity context;
	bool seated = false;
	if (!EmpiresNativePlacement::CaptureContext(engine->IndexOfEdict(m_pEdict), context) ||
		!EmpiresNativePlacement::ReadCommanderMode(context, seated) || !seated)
		return;

	// Next point: not owned, takeable by this team, nearest to the CV home (the front line).
	// Same filter as tier 1 of pickEmpiresNavmeshGoal.
	int bestPoint = -1;
	Vector bestSpot;
	float bestDistance = 0.0f;
	int owned = 0;
	int total = 0;
	for (const CEmpiresMod::CapturePointObservation& point : mod->getCapturePoints())
	{
		if (!point.originKnown || point.entityIndex <= 0)
			continue;
		if (point.teamOwnerKnown)
		{
			++total;
			if (point.teamOwner == team)
				++owned;
		}
		const bool cappableKnown = team == EMP_TEAM_NF ? point.nfCappableKnown : point.beCappableKnown;
		const int cappable = team == EMP_TEAM_NF ? point.nfCappable : point.beCappable;
		if ((cappableKnown && cappable == 0) || (point.teamOwnerKnown && point.teamOwner == team) ||
			!EmpiresCapturePointOnMesh(point))
			continue;
		const Vector spot = point.groundApproachKnown ? point.groundApproachOrigin : point.origin;
		const float distance = (spot - home).Length2D();
		if (bestPoint < 0 || distance < bestDistance)
		{
			bestPoint = point.entityIndex;
			bestSpot = spot;
			bestDistance = distance;
		}
	}

	if (orders.point >= 0)
	{
		const CEmpiresMod::CapturePointObservation* current = FindPoint(mod, orders.point);
		const bool taken = current == nullptr || (current->teamOwnerKnown && current->teamOwner == team);
		if (taken && bestPoint < 0)
		{
			releaseCommanderOrders(team, "point-taken");
			return;
		}
		if (!taken && now - orders.issuedAt > kNoProgressSeconds)
		{
			releaseCommanderOrders(team, "no-progress");
			orders.nextCheck = now + kNoProgressSeconds; // let the bots pick their own goals for a while
			return;
		}
		if (taken)
		{
			RCBOT_DIAG("[RCBOT2] commander-orders retarget team=%d from=%d to=%d\n", team, orders.point, bestPoint);
			orders.point = -1;
		}
	}
	if (bestPoint < 0)
		return;

	// Units: alive bots of the team, no commander, no engineer, no EngiBot, not in a vehicle, without
	// an order of someone else; bots whose rank falls into the defend share stay with their flags
	// (same rank rule as pickEmpiresNavmeshGoal).
	int teamSize = 0;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		IPlayerInfo* info = e != nullptr && !e->IsFree() ? playerinfomanager->GetPlayerInfo(e) : nullptr;
		if (info != nullptr && info->IsConnected() && info->GetTeamIndex() == team)
			++teamSize;
	}
	const float defendShare = total > 0 ? static_cast<float>(owned) / total : 0.0f;
	const bool newPoint = orders.point != bestPoint;
	std::string list = "emp_unit_order_list";
	int units = 0;
	int added = 0;
	int rank = 0;
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		edict_t* e = engine->PEntityOfEntIndex(i);
		IPlayerInfo* info = e != nullptr && !e->IsFree() ? playerinfomanager->GetPlayerInfo(e) : nullptr;
		if (info == nullptr || !info->IsConnected() || info->GetTeamIndex() != team)
			continue;
		const int myRank = rank++;
		if (e == m_pEdict || !info->IsFakeClient() || info->IsDead() || EmpiresIsEngibot(e))
			continue;
		if (teamSize > 0 && (myRank + 0.5f) / teamSize < defendShare)
			continue;
		int classId = -1;
		bool desired = false, desiredKnown = false;
		const char* classProperty = nullptr;
		if (readEmpiresClass(e, classId, desired, desiredKnown, classProperty) && classId == EMP_CLASS_ENGINEER)
			continue;
		int vehicle = -1;
		if (readEmpiresWeaponHandleProperty(e, "m_hVehicle", vehicle) && vehicle > 0 && vehicle < 4095)
			continue;
		int type = -1;
		if (!readEmpiresIntProperty(e, "m_iCommandType", type))
			continue;
		if (orders.ordered[i] && type != kAttackLocation)
			orders.ordered[i] = false; // someone else ordered it, or the order ended
		const bool ours = orders.ordered[i];
		if (type != 0 && !ours)
			continue;
		if (ours && !newPoint)
			continue; // already on its way
		if (units >= 32)
			break;
		list += " " + std::to_string(i);
		++units;
		if (!ours)
			++added;
		orders.ordered[i] = true;
	}
	if (units == 0)
	{
		if (newPoint)
			orders.point = -1;
		return;
	}
	list += "\n";
	char command[96];
	std::snprintf(command, sizeof(command), "emp_unit_order %d %.0f %.0f %.0f\n", kAttackLocation,
		bestSpot.x, bestSpot.y, bestSpot.z);
	helpers->ClientCommand(m_pEdict, list.c_str());
	helpers->ClientCommand(m_pEdict, command);
	if (newPoint)
		orders.issuedAt = now;
	orders.point = bestPoint;
	orders.spot = bestSpot;
	RCBOT_DIAG("[RCBOT2] commander-orders issue type=%d team=%d point=%d spot=(%.0f %.0f %.0f) units=%d new=%d\n",
		kAttackLocation, team, bestPoint, bestSpot.x, bestSpot.y, bestSpot.z, units, added);
}
