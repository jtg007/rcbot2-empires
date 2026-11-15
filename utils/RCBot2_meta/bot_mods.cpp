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
#include "engine_wrappers.h"

#include "bot.h"

#include "in_buttons.h"

#include "bot_mods.h"
#include "bot_globals.h"
#include "bot_fortress.h"
#include "bot_weapons.h"
#include "bot_configfile.h"
#include "bot_getprop.h"
#include "bot_dod_bot.h"
#include "bot_navigator.h"
#include "bot_waypoint.h"
#include "bot_waypoint_locations.h"
#include "bot_perceptron.h"
#include "bot_empires.h"
#include "bot_empires_telemetry.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "rcbot/logging.h"

#include "dt_send.h"
#include "server_class.h"
#include "bot_cvars.h"

#if defined(_WIN64) || defined(_WIN32)
#define strcmpi _strcmpi
#endif 

extern IServerGameDLL* servergamedll;
extern IServerTools* servertools;

namespace
{
	bool readEmpiresEntityIntProperty(const edict_t* pEdict,
		const char* propertyName, int& value)
	{
		value = -1;
		if (pEdict == nullptr || pEdict->IsFree() ||
			servergamedll == nullptr || propertyName == nullptr)
			return false;

		edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
		IServerUnknown* pUnknown = pCurrentEdict->GetUnknown();
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		const ServerClass* pClass = pNetworkable != nullptr ?
			pNetworkable->GetServerClass() : nullptr;
		if (pClass != nullptr && pClass->m_pTable != nullptr)
		{
			SendProp* pProp = UTIL_FindSendProp(pClass->m_pTable, propertyName);
			unsigned offset = 0;
			if (pProp != nullptr && pProp->GetType() == DPT_Int &&
				pProp->GetNumElements() == 1 &&
				UTIL_FindSendPropInfo(pClass, propertyName, &offset) && offset != 0)
			{
				CClassInterfaceValue propertyValue(pClass->m_pNetworkName,
					propertyName, 0);
				propertyValue.findOffset();
				int* pValue = propertyValue.getIntPointer(pCurrentEdict);
				if (pValue != nullptr)
				{
					value = *pValue;
					return true;
				}
			}
		}

		CBaseEntity* pEntity = pUnknown != nullptr ? pUnknown->GetBaseEntity() : nullptr;
		if (pEntity == nullptr)
			return false;

		datamap_t* pMap = CBaseEntity_GetDataDescMap(pEntity);
		const unsigned offset = pMap != nullptr ?
			UTIL_FindInDataMap(pMap, propertyName) : 0;
		if (offset == 0)
			return false;

		value = *reinterpret_cast<int*>(reinterpret_cast<char*>(pEntity) + offset);
		return true;
	}

	bool readEmpiresEntityBoolProperty(const edict_t* pEdict,
		const char* propertyName, bool& value)
	{
		value = false;
		if (pEdict == nullptr || pEdict->IsFree() ||
			servergamedll == nullptr || propertyName == nullptr)
			return false;

		edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
		IServerUnknown* pUnknown = pCurrentEdict->GetUnknown();
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		const ServerClass* pClass = pNetworkable != nullptr ?
			pNetworkable->GetServerClass() : nullptr;
		if (pClass != nullptr && pClass->m_pTable != nullptr)
		{
			SendProp* pProp = UTIL_FindSendProp(pClass->m_pTable, propertyName);
			unsigned offset = 0;
			if (pProp != nullptr && pProp->GetType() == DPT_Int &&
				pProp->GetNumElements() == 1 &&
				UTIL_FindSendPropInfo(pClass, propertyName, &offset) && offset != 0)
			{
				CClassInterfaceValue propertyValue(pClass->m_pNetworkName,
					propertyName, 0);
				propertyValue.findOffset();
				bool* pValue = propertyValue.getBoolPointer(pCurrentEdict);
				if (pValue != nullptr)
				{
					value = *pValue;
					return true;
				}
			}
		}

		CBaseEntity* pEntity = pUnknown != nullptr ? pUnknown->GetBaseEntity() : nullptr;
		if (pEntity == nullptr)
			return false;

		datamap_t* pMap = CBaseEntity_GetDataDescMap(pEntity);
		const unsigned offset = pMap != nullptr ?
			UTIL_FindInDataMap(pMap, propertyName) : 0;
		if (offset == 0)
			return false;

		value = *reinterpret_cast<bool*>(reinterpret_cast<char*>(pEntity) + offset);
		return true;
	}

	bool readEmpiresEntityFloatProperty(const edict_t* pEdict,
		const char* propertyName, float& value)
	{
		value = 0.0f;
		if (pEdict == nullptr || pEdict->IsFree() || servergamedll == nullptr ||
			propertyName == nullptr)
			return false;
		edict_t* entity = const_cast<edict_t*>(pEdict);
		IServerUnknown* unknown = entity->GetUnknown();
		CBaseEntity* base = unknown != nullptr ? unknown->GetBaseEntity() : nullptr;
		IServerNetworkable* networkable = unknown != nullptr ?
			unknown->GetNetworkable() : nullptr;
		const ServerClass* serverClass = networkable != nullptr ?
			networkable->GetServerClass() : nullptr;
		unsigned offset = 0;
		SendProp* prop = serverClass != nullptr && serverClass->m_pTable != nullptr ?
			UTIL_FindSendProp(serverClass->m_pTable, propertyName) : nullptr;
		if (base != nullptr && prop != nullptr && prop->GetType() == DPT_Float &&
			UTIL_FindSendPropInfo(serverClass, propertyName, &offset) && offset != 0)
		{
			value = *reinterpret_cast<const float*>(
				reinterpret_cast<const char*>(base) + offset);
			return true;
		}
		datamap_t* map = base != nullptr ? CBaseEntity_GetDataDescMap(base) : nullptr;
		const unsigned dataOffset = map != nullptr ?
			UTIL_FindInDataMap(map, propertyName) : 0;
		if (dataOffset == 0)
			return false;
		value = *reinterpret_cast<const float*>(
			reinterpret_cast<const char*>(base) + dataOffset);
		return true;
	}

	bool readEmpiresEntityVectorProperty(const edict_t* pEdict,
		const char* propertyName, Vector& value)
	{
		if (pEdict == nullptr || pEdict->IsFree() || propertyName == nullptr ||
			const_cast<edict_t*>(pEdict)->GetUnknown() == nullptr ||
			const_cast<edict_t*>(pEdict)->GetNetworkable() == nullptr)
			return false;
		edict_t* entity = const_cast<edict_t*>(pEdict);
		CBaseEntity* base = entity->GetUnknown()->GetBaseEntity();
		ServerClass* serverClass = entity->GetNetworkable()->GetServerClass();
		unsigned offset = 0;
		if (base == nullptr || serverClass == nullptr ||
			!UTIL_FindSendPropInfo(serverClass, propertyName, &offset) || offset == 0)
			return false;
		const float* components = reinterpret_cast<const float*>(
			reinterpret_cast<const char*>(base) + offset);
		value = Vector(components[0], components[1], components[2]);
		return value.IsValid();
	}

	// m_iCommandTarget is a 32-int array; Guard stores the target entity in
	// element 0 (reverse-engineering/notes/commander-order-types.md). The int
	// reader above rejects arrays, so read element 0 relative to
	// m_iCommandType (+0x1458 -> +0x1468 in server_srv.so), guarded by the
	// known m_vCommandLocation layout (+0x145c) so a changed binary fails.
	bool readEmpiresCommandTargetElement0(const edict_t* pEdict, int& value)
	{
		value = -1;
		if (pEdict == nullptr || pEdict->IsFree() ||
			const_cast<edict_t*>(pEdict)->GetUnknown() == nullptr ||
			const_cast<edict_t*>(pEdict)->GetNetworkable() == nullptr)
			return false;
		edict_t* entity = const_cast<edict_t*>(pEdict);
		CBaseEntity* base = entity->GetUnknown()->GetBaseEntity();
		ServerClass* serverClass = entity->GetNetworkable()->GetServerClass();
		unsigned typeOffset = 0;
		unsigned locationOffset = 0;
		if (base == nullptr || serverClass == nullptr ||
			!UTIL_FindSendPropInfo(serverClass, "m_iCommandType", &typeOffset) ||
			!UTIL_FindSendPropInfo(serverClass, "m_vCommandLocation", &locationOffset) ||
			typeOffset == 0 || locationOffset != typeOffset + 4)
			return false;
		value = *reinterpret_cast<const int*>(
			reinterpret_cast<const char*>(base) + typeOffset + 0x10);
		return true;
	}

	bool readEmpiresKeyValueInt(CBaseEntity* pEntity, const char* key,
		int& value)
	{
		value = -1;
		if (servertools == nullptr || pEntity == nullptr || key == nullptr)
			return false;

		char rawValue[32] = {};
		if (!servertools->GetKeyValue(pEntity, key, rawValue, sizeof(rawValue)))
			return false;

		char* end = nullptr;
		const long parsed = std::strtol(rawValue, &end, 10);
		if (end == rawValue || *end != '\0' ||
			parsed < static_cast<long>((std::numeric_limits<int>::min)()) ||
			parsed > static_cast<long>((std::numeric_limits<int>::max)()))
			return false;

		value = static_cast<int>(parsed);
		return true;
	}

	std::string formatEmpiresCaptureOccupants(const std::vector<int>& userIds)
	{
		if (userIds.empty())
			return "-";

		std::string result;
		for (const int userId : userIds)
		{
			if (!result.empty())
				result += ',';
			result += std::to_string(userId);
		}
		return result;
	}

	bool isEmpiresSpotQuickCommand(const int argc, const char* pcmd,
		const char* arg1, const char* arg2, int& button)
	{
		if (pcmd == nullptr || std::strcmp(pcmd, "emp_menu_quickcmd") != 0 ||
			argc < 3 || arg1 == nullptr || arg2 == nullptr)
			return false;

		const int menu = std::atoi(arg1);
		button = std::atoi(arg2);
		// Empires' default Q/T bindings are the player-spot quick commands:
		// menu 2, button 0 = infantry and button 4 = armour.  Both are
		// resolved to an enemy player here; vehicle targets remain deferred.
		return menu == 2 && (button == 0 || button == 4);
	}

	edict_t* findEmpiresSpotTarget(edict_t* pSource)
	{
		if (pSource == nullptr || gpGlobals == nullptr || engine == nullptr ||
			playerinfomanager == nullptr || gameclients == nullptr)
			return nullptr;

		IPlayerInfo* pSourceInfo = playerinfomanager->GetPlayerInfo(pSource);
		if (pSourceInfo == nullptr || !pSourceInfo->IsConnected() ||
			!pSourceInfo->IsPlayer() ||
			pSourceInfo->IsDead() || pSourceInfo->GetHealth() <= 0)
			return nullptr;

		const int sourceTeam = pSourceInfo->GetTeamIndex();
		if (sourceTeam != EMP_TEAM_NF && sourceTeam != EMP_TEAM_BE)
			return nullptr;

		Vector sourceEye;
		gameclients->ClientEarPosition(pSource, &sourceEye);

		Vector vAim;
		AngleVectors(pSourceInfo->GetLastUserCommand().viewangles, &vAim);
		const float aimLength = vAim.Length();
		if (aimLength <= 0.001f)
			return nullptr;
		vAim = vAim / aimLength;

		edict_t* pBestTarget = nullptr;
		float bestDot = 0.97f;
		float bestDistance = 1.0e30f;
		const int maxClients = (std::min)(gpGlobals->maxClients, RCBOT_MAXPLAYERS - 1);

		for (int index = 1; index <= maxClients; ++index)
		{
			edict_t* pCandidate = engine->PEntityOfEntIndex(index);
			if (pCandidate == nullptr || pCandidate == pSource || pCandidate->IsFree() ||
				!CBotGlobals::entityIsValid(pCandidate) ||
				!CBotGlobals::isPlayer(pCandidate))
				continue;

			IPlayerInfo* pCandidateInfo = playerinfomanager->GetPlayerInfo(pCandidate);
			if (pCandidateInfo == nullptr || !pCandidateInfo->IsConnected() ||
				!pCandidateInfo->IsPlayer() || pCandidateInfo->IsObserver() ||
				pCandidateInfo->IsDead() || pCandidateInfo->GetHealth() <= 0 ||
				pCandidateInfo->GetTeamIndex() == sourceTeam)
				continue;

			const int candidateTeam = pCandidateInfo->GetTeamIndex();
			if (candidateTeam != EMP_TEAM_NF && candidateTeam != EMP_TEAM_BE)
				continue;

			const ICollideable* pCollideable = pCandidate->GetCollideable();
			if (pCollideable == nullptr)
				continue;

			// Use the same collision-origin convention as the generic RCBot2
			// visibility code, with the player's collision-box midpoint as the
			// aim target.  The public shot trace rejects world/entity occlusion.
			const Vector targetPoint = pCollideable->GetCollisionOrigin() +
				Vector(0.0f, 0.0f, pCollideable->OBBMaxs().z * 0.5f);
			Vector vDirection = targetPoint - sourceEye;
			const float distance = vDirection.Length();
			if (distance <= 1.0f)
				continue;
			vDirection = vDirection / distance;

			const float dot = DotProduct(vAim, vDirection);
			if (dot < bestDot || (dot == bestDot && distance >= bestDistance))
				continue;

			if (!CBotGlobals::isShotVisible(pSource, sourceEye, targetPoint, pCandidate))
				continue;

			bestDot = dot;
			bestDistance = distance;
			pBestTarget = pCandidate;
		}

		return pBestTarget;
	}
}

void CEmpiresMod::initMod()
{
	if (logger != nullptr)
		logger->Log(LogLevel::INFO, "Empires objective=observer-init");

	// Load the native Empires classnames into the existing RCBot2 weapon
	// registry.  If weapons.ini has no EMPIRES section, loadWeapons() uses
	// EMPWeaps as the verified fallback table.
	CWeapons::loadWeapons(m_szWeaponListName == nullptr ? "EMPIRES" : m_szWeaponListName, EMPWeaps);

	// Empires is currently in the native bootstrap phase. Keep the normal
	// quota/min-max reconciler from creating unrelated bots during initial
	// load and late plugin reloads. Explicit bootstrap commands remain valid.
	CBots::setEmpiresBootstrapTestMode(true);
}

void CEmpiresMod::mapInit()
{
	CBotMod::mapInit();
	EmpiresTurretBuildLevelReset();
	EmpiresReviveLevelReset();
	EmpiresVehicleLevelReset();
	EmpiresCommanderEconomyLevelReset();
	EmpiresObjectiveLevelReset();
	EmpiresEngibotLevelReset();
	EmpiresCommanderOrdersLevelReset();
	RCBOT_DIAG("[RCBOT2] empires level-reset\n");
	m_CapturePoints.clear();
	m_flNextCapturePointScan = 0.0f;
	m_bCapturePointInventoryLogged = false;
	m_bCapturePointScanUnavailableLogged = false;
}

void CEmpiresMod::modFrame()
{
	if (gpGlobals == nullptr || gpGlobals->curtime < m_flNextCapturePointScan)
		return;

	m_flNextCapturePointScan = gpGlobals->curtime + 0.25f;
	updateCommanderOrders();
	updateCapturePoints();
}

void CEmpiresMod::updateCommanderOrders()
{
	if (engine == nullptr || playerinfomanager == nullptr || gpGlobals == nullptr)
		return;
	int commanderUserIds[4] = { -1, -1, -1, -1 };
	const int maxClients = (std::min)(gpGlobals->maxClients, RCBOT_MAXPLAYERS - 1);
	for (int index = 1; index <= maxClients; ++index)
	{
		edict_t* player = engine->PEntityOfEntIndex(index);
		if (player == nullptr || player->IsFree())
			continue;
		IPlayerInfo* info = playerinfomanager->GetPlayerInfo(player);
		if (info == nullptr || !info->IsConnected() || !info->IsPlayer())
			continue;
		const int team = info->GetTeamIndex();
		if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
			continue;
		bool isCommander = false;
		if (!info->IsFakeClient() &&
			readEmpiresEntityBoolProperty(player, "m_bCommander", isCommander) &&
			isCommander)
		{
			const int userId = engine->GetPlayerUserId(player);
			if (userId > 0)
				commanderUserIds[team] = userId;
		}
	}

	for (int index = 1; index <= maxClients; ++index)
	{
		edict_t* player = engine->PEntityOfEntIndex(index);
		if (player == nullptr || player->IsFree())
			continue;
		IPlayerInfo* info = playerinfomanager->GetPlayerInfo(player);
		if (info == nullptr || !info->IsConnected() || !info->IsPlayer())
			continue;
		const int team = info->GetTeamIndex();
		if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
			continue;
		EmpiresAnalysis::CommanderOrderObservation observation;
		observation.recipientUserId = engine->GetPlayerUserId(player);
		if (observation.recipientUserId <= 0)
			continue;
		observation.commanderUserId = commanderUserIds[team];
		observation.team = team;
		(void)readEmpiresEntityIntProperty(player, "m_iSquad", observation.squad);
		if (!readEmpiresEntityIntProperty(player, "m_iCommandType",
			observation.commandType))
			continue;
		observation.targetEntityKnown = readEmpiresEntityIntProperty(player,
			"m_iCommandTarget", observation.targetEntityIndex) ||
			readEmpiresCommandTargetElement0(player, observation.targetEntityIndex);
		Vector target;
		observation.targetPositionKnown = readEmpiresEntityVectorProperty(player,
			"m_vCommandLocation", target);
		if (observation.targetPositionKnown)
		{
			observation.targetX = target.x;
			observation.targetY = target.y;
			observation.targetZ = target.z;
		}
		observation.cancelTimeKnown = readEmpiresEntityFloatProperty(player,
			"m_flOrderCancelTime", observation.cancelTime);
		EmpiresAnalysis::ObserveCommanderOrder(observation);
	}
}

void CEmpiresMod::logVehicleInventory() const
{
	if (servertools == nullptr || engine == nullptr)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires vehicle-audit unavailable servertools=%d engine=%d",
				servertools != nullptr ? 1 : 0, engine != nullptr ? 1 : 0);
		return;
	}

	int vehicleCount = 0;
	int spawnerCount = 0;
	int presetCount = 0;
	int spawnBrushCount = 0;
	int vehicleFactoryCount = 0;

	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pEdict = pNetworkable != nullptr ?
			pNetworkable->GetEdict() : nullptr;
		if (pEdict == nullptr || pEdict->IsFree() || pEdict->GetClassName() == nullptr)
			continue;

		const char* className = pEdict->GetClassName();
		const bool isVehicle = std::strcmp(className, "emp_vehicle") == 0;
		const bool isSpawner = std::strcmp(className, "emp_vehicle_spawner") == 0;
		const bool isPreset = std::strcmp(className, "emp_vehicle_preset") == 0;
		const bool isSpawnBrush = std::strcmp(className, "emp_vehicle_spawn_brush") == 0;
		const bool isVehicleFactory =
			std::strcmp(className, "emp_building_vehiclefactory") == 0 ||
			std::strcmp(className, "emp_building_nf_vehiclefactory") == 0 ||
			std::strcmp(className, "emp_building_imp_vehiclefactory") == 0;
		if (!isVehicle && !isSpawner && !isPreset && !isSpawnBrush && !isVehicleFactory)
			continue;

		if (isVehicle)
			++vehicleCount;
		else if (isSpawner)
			++spawnerCount;
		else if (isPreset)
			++presetCount;
		else if (isSpawnBrush)
			++spawnBrushCount;
		else
			++vehicleFactoryCount;

		const Vector origin = CBotGlobals::entityOrigin(pEdict);
		if (logger != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires vehicle-audit entity_index=%d classname=%s origin=(%.1f %.1f %.1f)",
				engine->IndexOfEdict(pEdict), className,
				origin.x, origin.y, origin.z);

			if (isVehicleFactory)
			{
				int team = -1;
				int health = -1;
				int maxHealth = -1;
				bool fullyBuilt = false;
				const bool teamKnown = readEmpiresEntityIntProperty(
					pEdict, "m_iTeamNum", team);
				const bool healthKnown = readEmpiresEntityIntProperty(
					pEdict, "m_iHealth", health);
				const bool maxHealthKnown = readEmpiresEntityIntProperty(
					pEdict, "m_iMaxHealth", maxHealth);
				const bool fullyBuiltKnown = readEmpiresEntityBoolProperty(
					pEdict, "m_bFullyBuilt", fullyBuilt);
				logger->Log(LogLevel::INFO,
					"Empires vehicle-audit vf-state entity_index=%d classname=%s team_known=%d team=%d health_known=%d health=%d max_health_known=%d max_health=%d fully_built_known=%d fully_built=%d",
					engine->IndexOfEdict(pEdict), className,
					teamKnown ? 1 : 0, team, healthKnown ? 1 : 0, health,
					maxHealthKnown ? 1 : 0, maxHealth,
					fullyBuiltKnown ? 1 : 0, fullyBuilt ? 1 : 0);
			}
		}
	}

	if (logger != nullptr)
		logger->Log(LogLevel::INFO,
			"Empires vehicle-audit summary vehicles=%d spawners=%d presets=%d spawn_brushes=%d vehicle_factories=%d",
			vehicleCount, spawnerCount, presetCount, spawnBrushCount, vehicleFactoryCount);
}

void CEmpiresMod::updateCapturePoints()
{
	if (servertools == nullptr || engine == nullptr || gpGlobals == nullptr)
	{
		if (!m_bCapturePointScanUnavailableLogged && logger != nullptr)
		{
			logger->Log(LogLevel::WARN,
				"Empires objective=observer-unavailable servertools=%d engine=%d globals=%d",
				servertools != nullptr ? 1 : 0,
				engine != nullptr ? 1 : 0,
				gpGlobals != nullptr ? 1 : 0);
			m_bCapturePointScanUnavailableLogged = true;
		}
		return;
	}

	std::vector<CapturePointObservation> current;
	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pEdict = pNetworkable != nullptr ? pNetworkable->GetEdict() : nullptr;
		if (pEdict == nullptr || pEdict->IsFree() || pEdict->GetClassName() == nullptr ||
			std::strcmp(pEdict->GetClassName(), "emp_cap_point") != 0)
			continue;

		CapturePointObservation observation;
		observation.entityIndex = engine->IndexOfEdict(pEdict);
		const CapturePointObservation* previous = nullptr;
		for (const CapturePointObservation& prior : m_CapturePoints)
		{
			if (prior.entityIndex == observation.entityIndex)
			{
				previous = &prior;
				break;
			}
		}
		observation.pointIndexKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iCapPointIndex", observation.pointIndex);
		if (!observation.pointIndexKnown)
		{
			observation.pointIndexFromKeyValue = readEmpiresKeyValueInt(
				pEntity, "pointNumber", observation.pointIndex);
			observation.pointIndexKnown = observation.pointIndexFromKeyValue;
		}
		if (!observation.pointIndexKnown && previous != nullptr)
		{
			observation.pointIndex = previous->pointIndex;
			observation.pointIndexKnown = previous->pointIndexKnown;
			observation.pointIndexFromKeyValue = previous->pointIndexFromKeyValue;
		}
		observation.nfStatusKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iNFStatus", observation.nfStatus);
		observation.beStatusKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iBEStatus", observation.beStatus);
		observation.progressKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iProgress", observation.progress);
		observation.teamOwnerKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iTeamOwner", observation.teamOwner);
		observation.timeToCapKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iTimeToCap", observation.timeToCap);
		observation.timeToNeutralKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iTimeToNeutral", observation.timeToNeutral);
		observation.nfCappableKnown = readEmpiresKeyValueInt(
			pEntity, "canNFCap", observation.nfCappable);
		observation.beCappableKnown = readEmpiresKeyValueInt(
			pEntity, "canImpCap", observation.beCappable);
		if (!observation.nfCappableKnown && previous != nullptr)
		{
			observation.nfCappable = previous->nfCappable;
			observation.nfCappableKnown = previous->nfCappableKnown;
		}
		if (!observation.beCappableKnown && previous != nullptr)
		{
			observation.beCappable = previous->beCappable;
			observation.beCappableKnown = previous->beCappableKnown;
		}
		int linearMapValue = -1;
		if (readEmpiresKeyValueInt(pEntity, "linearMap", linearMapValue) &&
			(linearMapValue == 0 || linearMapValue == 1))
		{
			observation.linearMap = linearMapValue != 0;
			observation.linearMapKnown = true;
		}
		if (!observation.linearMapKnown && previous != nullptr)
		{
			observation.linearMap = previous->linearMap;
			observation.linearMapKnown = previous->linearMapKnown;
		}
		observation.nfEnabledKnown = readEmpiresEntityBoolProperty(
			pEdict, "m_bNFEnabled", observation.nfEnabled);
		observation.beEnabledKnown = readEmpiresEntityBoolProperty(
			pEdict, "m_bBEEnabled", observation.beEnabled);
		observation.origin = CBotGlobals::entityOrigin(pEdict);
		observation.originKnown = pEdict->GetIServerEntity() != nullptr &&
			pEdict->GetIServerEntity()->GetCollideable() != nullptr;
		// emp_cap_point is a world brush (model *N). Its collision origin is
		// commonly (0, 0, 0), while the world-space bounds contain the actual
		// capture volume. Prefer the public bounds for brush points rather than
		// introducing a map-specific coordinate or a fixed memory offset.
		ICollideable* pCollideable = observation.originKnown ?
			pEdict->GetIServerEntity()->GetCollideable() : nullptr;
		Vector captureMins;
		Vector captureMaxs;
		bool captureBoundsKnown = false;
		if (pCollideable != nullptr && pCollideable->GetCollisionModelIndex() >= 0)
		{
			Vector worldMins;
			Vector worldMaxs;
			pCollideable->WorldSpaceSurroundingBounds(&worldMins, &worldMaxs);
			if (worldMins.IsValid() && worldMaxs.IsValid() &&
				worldMaxs.x >= worldMins.x && worldMaxs.y >= worldMins.y &&
				worldMaxs.z >= worldMins.z)
			{
				observation.origin = (worldMins + worldMaxs) * 0.5f;
				observation.originFromCollisionBounds = true;
				captureMins = worldMins;
				captureMaxs = worldMaxs;
				captureBoundsKnown = true;
			}
		}
		// A capture brush's bounds center is not necessarily a walkable player
		// origin.  Resolve a grounded candidate from the same public bounds using
		// the normal world trace.  This is observation only; it never moves the
		// entity or writes Empires state.
		if (captureBoundsKnown)
		{
			CTraceFilterWorldAndPropsOnly groundFilter;
			const float tracePadding =
				static_cast<float>(CWaypointLocations::REACHABLE_RANGE);
			Vector traceStart = observation.origin + Vector(0.0f, 0.0f, tracePadding);
			const Vector traceEnd = observation.origin - Vector(0.0f, 0.0f, tracePadding);
			// Probe from the volume center first: a trace from above stops on a
			// roof when the zone holds a building (emp_coast "Bunker": roof 824
			// has no way up, floor 664 inside is the reachable part of the zone).
			for (float z = observation.origin.z; z > captureMins.z; z -= 16.0f)
			{
				const Vector probe(observation.origin.x, observation.origin.y, z);
				CBotGlobals::traceLine(probe, traceEnd,
					MASK_NPCSOLID_BRUSHONLY, &groundFilter);
				const trace_t* probeTrace = CBotGlobals::getTraceResult();
				if (probeTrace == nullptr)
					break;
				if (!probeTrace->startsolid && !probeTrace->allsolid)
				{
					traceStart = probe;
					break;
				}
			}
			CBotGlobals::traceLine(traceStart, traceEnd,
				MASK_NPCSOLID_BRUSHONLY, &groundFilter);
			const trace_t* groundTrace = CBotGlobals::getTraceResult();
			if (groundTrace != nullptr && !groundTrace->startsolid &&
				!groundTrace->allsolid && groundTrace->fraction < 1.0f)
			{
				const Vector candidate = groundTrace->endpos + Vector(0.0f, 0.0f, 1.0f);
				if (candidate.IsValid() &&
					candidate.x >= captureMins.x && candidate.x <= captureMaxs.x &&
					candidate.y >= captureMins.y && candidate.y <= captureMaxs.y &&
					candidate.z >= captureMins.z && candidate.z <= captureMaxs.z)
				{
					observation.groundApproachOrigin = candidate;
					observation.groundApproachKnown = true;
				}
			}
		}

		// Read-only attribution aid: record alive NF/BE players whose current
		// player origin is inside the public capture brush bounds.  This does
		// not decide capture ownership and does not write any game state; it
		// only makes a later status transition attributable to a real actor.
		if (captureBoundsKnown && engine != nullptr &&
			playerinfomanager != nullptr && gpGlobals != nullptr)
		{
			const int maxClients = (std::min)(gpGlobals->maxClients,
				RCBOT_MAXPLAYERS - 1);
			for (int clientIndex = 1; clientIndex <= maxClients; ++clientIndex)
			{
				edict_t* pClient = engine->PEntityOfEntIndex(clientIndex);
				if (pClient == nullptr || pClient->IsFree())
					continue;

				IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(pClient);
				if (pInfo == nullptr || !pInfo->IsConnected() ||
					!pInfo->IsPlayer() || pInfo->IsObserver() || pInfo->IsDead() ||
					pInfo->GetHealth() <= 0)
					continue;

				const int team = pInfo->GetTeamIndex();
				if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
					continue;

				const Vector origin = pInfo->GetAbsOrigin();
				if (origin.x < captureMins.x || origin.x > captureMaxs.x ||
					origin.y < captureMins.y || origin.y > captureMaxs.y ||
					origin.z < captureMins.z || origin.z > captureMaxs.z)
					continue;

				const int userId = engine->GetPlayerUserId(pClient);
				if (userId <= 0)
					continue;
				if (team == EMP_TEAM_NF)
					observation.nfOccupantUserIds.push_back(userId);
				else
					observation.beOccupantUserIds.push_back(userId);
			}
			std::sort(observation.nfOccupantUserIds.begin(),
				observation.nfOccupantUserIds.end());
			std::sort(observation.beOccupantUserIds.begin(),
				observation.beOccupantUserIds.end());
		}
		const int waypointCount = CWaypoints::numWaypoints();
		for (int waypointIndex = 0; waypointIndex < waypointCount; ++waypointIndex)
		{
			CWaypoint* pWaypoint = CWaypoints::getWaypoint(waypointIndex);
			if (pWaypoint == nullptr || !pWaypoint->isUsed())
				continue;

			const Vector waypointOrigin = pWaypoint->getOrigin();
			const float distance = (waypointOrigin - observation.origin).Length();
			if (!observation.waypointAssociationKnown ||
				distance < observation.nearestWaypointDistance)
			{
				observation.nearestWaypoint = waypointIndex;
				observation.nearestWaypointDistance = distance;
				observation.waypointAssociationKnown = true;
			}
			if (captureBoundsKnown &&
				waypointOrigin.x >= captureMins.x && waypointOrigin.x <= captureMaxs.x &&
				waypointOrigin.y >= captureMins.y && waypointOrigin.y <= captureMaxs.y &&
				waypointOrigin.z >= captureMins.z && waypointOrigin.z <= captureMaxs.z)
			{
				const float captureDistance =
					(waypointOrigin - observation.origin).Length();
				if (!observation.captureWaypointKnown ||
					captureDistance <
						(observation.captureWaypointOrigin - observation.origin).Length())
				{
					observation.captureWaypoint = waypointIndex;
					observation.captureWaypointOrigin = waypointOrigin;
					observation.captureWaypointKnown = true;
				}
			}
		}
		if (EmpiresAnalysis::IsActive())
		{
			const std::string nfOccupants =
				formatEmpiresCaptureOccupants(observation.nfOccupantUserIds);
			const std::string beOccupants =
				formatEmpiresCaptureOccupants(observation.beOccupantUserIds);
			char state[1024] = {};
			snprintf(state, sizeof(state),
				"origin=(%.1f,%.1f,%.1f);origin_known=%d;ground_approach=(%.1f,%.1f,%.1f);ground_approach_known=%d;nf_status=%d;nf_status_known=%d;be_status=%d;be_status_known=%d;progress=%d;progress_known=%d;team_owner=%d;team_owner_known=%d;time_to_cap=%d;time_to_neutral=%d;nf_cappable=%d;be_cappable=%d;nf_enabled=%d;be_enabled=%d;nf_occupants=%s;be_occupants=%s",
				observation.origin.x, observation.origin.y, observation.origin.z,
				observation.originKnown ? 1 : 0,
				observation.groundApproachOrigin.x,
				observation.groundApproachOrigin.y,
				observation.groundApproachOrigin.z,
				observation.groundApproachKnown ? 1 : 0,
				observation.nfStatus, observation.nfStatusKnown ? 1 : 0,
				observation.beStatus, observation.beStatusKnown ? 1 : 0,
				observation.progress, observation.progressKnown ? 1 : 0,
				observation.teamOwner, observation.teamOwnerKnown ? 1 : 0,
				observation.timeToCap, observation.timeToNeutral,
				observation.nfCappable, observation.beCappable,
				observation.nfEnabled ? 1 : 0, observation.beEnabled ? 1 : 0,
				nfOccupants.c_str(), beOccupants.c_str());
			EmpiresAnalysis::RecordCapturePointState(observation.entityIndex,
				observation.pointIndex, state);
		}
		current.push_back(observation);
	}

	std::sort(current.begin(), current.end(),
		[](const CapturePointObservation& left, const CapturePointObservation& right)
		{
			return left.entityIndex < right.entityIndex;
		});

	bool changed = current.size() != m_CapturePoints.size();
	if (!changed)
	{
		for (std::size_t i = 0; i < current.size(); ++i)
		{
			const CapturePointObservation& left = current[i];
			const CapturePointObservation& right = m_CapturePoints[i];
			if (left.entityIndex != right.entityIndex ||
				left.pointIndex != right.pointIndex ||
				left.pointIndexKnown != right.pointIndexKnown ||
				left.pointIndexFromKeyValue != right.pointIndexFromKeyValue ||
				left.nfStatus != right.nfStatus ||
				left.nfStatusKnown != right.nfStatusKnown ||
				left.beStatus != right.beStatus ||
				left.beStatusKnown != right.beStatusKnown ||
				left.progress != right.progress ||
				left.progressKnown != right.progressKnown ||
				left.teamOwner != right.teamOwner ||
				left.teamOwnerKnown != right.teamOwnerKnown ||
				left.timeToCap != right.timeToCap ||
				left.timeToCapKnown != right.timeToCapKnown ||
				left.timeToNeutral != right.timeToNeutral ||
				left.timeToNeutralKnown != right.timeToNeutralKnown ||
				left.nfCappable != right.nfCappable ||
				left.nfCappableKnown != right.nfCappableKnown ||
				left.beCappable != right.beCappable ||
				left.beCappableKnown != right.beCappableKnown ||
				left.linearMap != right.linearMap ||
				left.linearMapKnown != right.linearMapKnown ||
				left.nfEnabled != right.nfEnabled ||
				left.nfEnabledKnown != right.nfEnabledKnown ||
				left.beEnabled != right.beEnabled ||
				left.beEnabledKnown != right.beEnabledKnown ||
				left.originKnown != right.originKnown ||
				left.originFromCollisionBounds != right.originFromCollisionBounds ||
				left.groundApproachKnown != right.groundApproachKnown ||
				(left.groundApproachOrigin - right.groundApproachOrigin).LengthSqr() > 0.01f ||
				left.captureWaypoint != right.captureWaypoint ||
				left.captureWaypointKnown != right.captureWaypointKnown ||
				(left.captureWaypointOrigin - right.captureWaypointOrigin).LengthSqr() > 0.01f ||
				left.nearestWaypoint != right.nearestWaypoint ||
				left.waypointAssociationKnown != right.waypointAssociationKnown ||
				left.nfOccupantUserIds != right.nfOccupantUserIds ||
				left.beOccupantUserIds != right.beOccupantUserIds ||
				std::fabs(left.nearestWaypointDistance - right.nearestWaypointDistance) > 0.01f ||
				(left.origin - right.origin).LengthSqr() > 0.01f)
			{
				changed = true;
				break;
			}
		}
	}

	if (!m_bCapturePointInventoryLogged || changed)
	{
		logger->Log(LogLevel::INFO,
			"Empires objective=inventory map=%s cap_points=%d changed=%d curtime=%.3f",
			gpGlobals->mapname.ToCStr(),
			static_cast<int>(current.size()), changed ? 1 : 0, gpGlobals->curtime);

		for (const CapturePointObservation& observation : current)
		{
			logger->Log(LogLevel::INFO,
			"Empires objective=cap-point entity_index=%d point_index=%d point_index_known=%d point_index_source=%s linear_map=%d linear_map_known=%d origin=(%.1f %.1f %.1f) origin_known=%d origin_source=%s ground_approach=(%.1f %.1f %.1f) ground_approach_known=%d waypoint_count=%d nearest_waypoint=%d nearest_waypoint_distance=%.1f waypoint_association_known=%d capture_waypoint=%d capture_waypoint_known=%d capture_waypoint_origin=(%.1f %.1f %.1f) nf_status=%d nf_status_known=%d be_status=%d be_status_known=%d progress=%d progress_known=%d team_owner=%d team_owner_known=%d time_to_cap=%d time_to_cap_known=%d time_to_neutral=%d time_to_neutral_known=%d nf_cappable=%d nf_cappable_known=%d be_cappable=%d be_cappable_known=%d nf_enabled=%d nf_enabled_known=%d be_enabled=%d be_enabled_known=%d nf_occupants=%s be_occupants=%s",
			observation.entityIndex, observation.pointIndex,
			observation.pointIndexKnown ? 1 : 0,
			observation.pointIndexFromKeyValue ? "keyvalue" :
				(observation.pointIndexKnown ? "sendprop" : "unknown"),
			observation.linearMap ? 1 : 0,
			observation.linearMapKnown ? 1 : 0,
			observation.origin.x, observation.origin.y, observation.origin.z,
				observation.originKnown ? 1 : 0,
				observation.originFromCollisionBounds ? "collision-bounds" : "entity-origin",
				observation.groundApproachOrigin.x,
				observation.groundApproachOrigin.y,
				observation.groundApproachOrigin.z,
				observation.groundApproachKnown ? 1 : 0,
				CWaypoints::numWaypoints(),
				observation.nearestWaypoint,
				observation.nearestWaypointDistance,
				observation.waypointAssociationKnown ? 1 : 0,
				observation.captureWaypoint,
				observation.captureWaypointKnown ? 1 : 0,
				observation.captureWaypointOrigin.x,
				observation.captureWaypointOrigin.y,
				observation.captureWaypointOrigin.z,
				observation.nfStatus, observation.nfStatusKnown ? 1 : 0,
				observation.beStatus, observation.beStatusKnown ? 1 : 0,
				observation.progress, observation.progressKnown ? 1 : 0,
				observation.teamOwner, observation.teamOwnerKnown ? 1 : 0,
				observation.timeToCap, observation.timeToCapKnown ? 1 : 0,
				observation.timeToNeutral, observation.timeToNeutralKnown ? 1 : 0,
				observation.nfCappable, observation.nfCappableKnown ? 1 : 0,
				observation.beCappable, observation.beCappableKnown ? 1 : 0,
				observation.nfEnabled ? 1 : 0,
				observation.nfEnabledKnown ? 1 : 0,
				observation.beEnabled ? 1 : 0,
				observation.beEnabledKnown ? 1 : 0,
				formatEmpiresCaptureOccupants(observation.nfOccupantUserIds).c_str(),
				formatEmpiresCaptureOccupants(observation.beOccupantUserIds).c_str());
		}

		m_bCapturePointInventoryLogged = true;
	}

	m_CapturePoints.swap(current);
}

void CEmpiresMod::clientCommand(edict_t* pEntity, const int argc,
	const char* pcmd, const char* arg1, const char* arg2)
{
	int button = -1;
	if (!isEmpiresSpotQuickCommand(argc, pcmd, arg1, arg2, button))
		return;

	if (pEntity == nullptr || playerinfomanager == nullptr || engine == nullptr)
		return;

	IPlayerInfo* pSourceInfo = playerinfomanager->GetPlayerInfo(pEntity);
	if (pSourceInfo == nullptr || !pSourceInfo->IsConnected() ||
		!pSourceInfo->IsPlayer())
		return;

	const int sourceTeam = pSourceInfo->GetTeamIndex();
	edict_t* pTarget = findEmpiresSpotTarget(pEntity);
	if (pTarget == nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires gameplay player-spot=received source_index=%d source_team=%d button=%d target=none reason=no-visible-enemy-player-in-aim-cone",
			engine->IndexOfEdict(pEntity), sourceTeam, button);
		return;
	}

	int teammateCount = 0;
	for (std::size_t i = 0; i < static_cast<std::size_t>(RCBOT_MAXPLAYERS); ++i)
	{
		CBot* pCandidate = CBots::get(i);
		if (pCandidate == nullptr || !pCandidate->inUse() ||
			pCandidate->isControlledTestOnly() || pCandidate->getEdict() == nullptr)
			continue;

		// The current Empires bot type is CBotEmpires for every normal slot;
		// use the established mod-specific slot invariant rather than RTTI.
		CBotEmpires* pTeammate = static_cast<CBotEmpires*>(pCandidate);
		if (pTeammate->getEdict() == pEntity || pTeammate->getTeam() != sourceTeam)
			continue;

		pTeammate->receiveEmpiresSpottedEnemy(pTarget, engine->IndexOfEdict(pEntity));
		++teammateCount;
	}

	IPlayerInfo* pTargetInfo = playerinfomanager->GetPlayerInfo(pTarget);
	logger->Log(LogLevel::INFO,
		"Empires gameplay player-spot=accepted source_index=%d source_userid=%d source_team=%d button=%d target_index=%d target_userid=%d target_team=%d teammates=%d response=existing-last-enemy-memory",
		engine->IndexOfEdict(pEntity), engine->GetPlayerUserId(pEntity), sourceTeam, button,
		engine->IndexOfEdict(pTarget), engine->GetPlayerUserId(pTarget),
		pTargetInfo != nullptr ? pTargetInfo->GetTeamIndex() : -1, teammateCount);
}

std::vector<edict_wpt_pair_t> CHalfLifeDeathmatchMod::m_LiftWaypoints;

void CBotMods::parseFile()
{
	char buffer[1024];
	char key[64];
	char val[256];

	eModId modtype = MOD_CUSTOM;
	eBotType bottype = BOTTYPE_GENERIC;
	
	char gamefolder[256];
	char weaponlist[64];

	CBotGlobals::buildFileName(buffer, BOT_MOD_FILE, BOT_CONFIG_FOLDER, BOT_CONFIG_EXTENSION);

	std::fstream fp = CBotGlobals::openFile(buffer, std::fstream::in);

	CBotMod* curmod = nullptr;

	if (!fp)
	{
		logger->Log(LogLevel::ERROR, "Failed to open file '%s' for reading", buffer);
		return;
	}

	while (fp.getline(buffer, 1023))
	{
		if (buffer[0] == '#')
			continue;

		std::size_t len = std::strlen(buffer);

		if (len == 0)
			continue;

		if (buffer[len - 1] == '\n')
			buffer[--len] = 0;

		std::size_t i = 0;
		std::size_t j = 0;

		while (i < len && buffer[i] != '=')
		{
			if (buffer[i] != ' ')
				key[j++] = buffer[i];
			i++;
		}

		i++;

		key[j] = 0;

		j = 0;

		while (i < len && buffer[i] != '\n' && buffer[i] != '\r')
		{
			if (j || buffer[i] != ' ')
				val[j++] = buffer[i];
			i++;
		}

		val[j] = 0;

		if (!std::strcmp(key, "mod"))
		{
			if (curmod)
			{
				curmod->setup(gamefolder, modtype, bottype, weaponlist);
				m_Mods.emplace_back(curmod);
			}

			curmod = nullptr;
			weaponlist[0] = 0;

			bottype = BOTTYPE_GENERIC;

			//modtype = MOD_CUSTOM;
			//TODO: Add Black Mesa Source support [APG]RoboCop[CL]

			if (!strcmpi("CUSTOM", val))
			{
				modtype = MOD_CUSTOM;
				curmod = new CBotMod();
			}
			else if (!strcmpi("CSS", val))
			{
				modtype = MOD_CSS;
				curmod = new CCounterStrikeSourceMod();
			}
			else if (!strcmpi("HL1DM", val))
			{
				modtype = MOD_HL1DMSRC;
				curmod = new CHLDMSourceMod();
			}
			else if (!strcmpi("HL2DM", val))
			{
				modtype = MOD_HLDM2;
				curmod = new CHalfLifeDeathmatchMod();
			}
			else if (!strcmpi("FF", val))
			{
				modtype = MOD_FF;
				curmod = new CFortressForeverMod();
			}
			else if (!strcmpi("TF2", val))
			{
				modtype = MOD_TF2;
				curmod = new CTeamFortress2Mod();
			}
			else if (!strcmpi("SVENCOOP2", val))
			{
				modtype = MOD_SVENCOOP2;
				curmod = new CBotMod();
			}
			else if (!strcmpi("TIMCOOP", val))
			{
				modtype = MOD_TIMCOOP;
				curmod = new CBotMod();
			}
			else if (!strcmpi("NS2", val))
			{
				modtype = MOD_NS2;
				curmod = new CBotMod();
			}
			else if (!strcmpi("SYNERGY", val))
			{
				modtype = MOD_SYNERGY;
				curmod = new CSynergyMod();
			}
			else if (!strcmpi("DOD", val))
			{
				modtype = MOD_DOD;
				curmod = new CDODMod();
			}
			else
				curmod = new CBotMod();
		}
		else if (curmod && !std::strcmp(key, "bot"))
		{
			if (!strcmpi("GENERIC", val))
				bottype = BOTTYPE_GENERIC;
			else if (!strcmpi("CSS", val))
				bottype = BOTTYPE_CSS;
			else if (!strcmpi("HL1DM", val))
				bottype = BOTTYPE_HL1DM;
			else if (!strcmpi("HL2DM", val))
				bottype = BOTTYPE_HL2DM;
			else if (!strcmpi("FF", val))
				bottype = BOTTYPE_FF;
			else if (!strcmpi("TF2", val))
				bottype = BOTTYPE_TF2;
			else if (!strcmpi("COOP", val))
				bottype = BOTTYPE_COOP;
			else if (!strcmpi("ZOMBIE", val))
				bottype = BOTTYPE_ZOMBIE;
			else if (!strcmpi("DOD", val))
				bottype = BOTTYPE_DOD;
			else if (!strcmpi("SYNERGY", val))
				bottype = BOTTYPE_SYN;
		}
		else if (curmod && !strcmpi(key, "gamedir"))
		{
			std::strncpy(gamefolder, val, 255);
			gamefolder[255] = '\0';
		}
		else if (curmod && !strcmpi(key, "weaponlist"))
		{
			std::strncpy(weaponlist, val, 63);
			weaponlist[63] = '\0';
		}
	}

	if (curmod)
	{
		curmod->setup(gamefolder, modtype, bottype, weaponlist);
		m_Mods.emplace_back(curmod);
	}
}

void CBotMods::readMods()
{
#if SOURCE_ENGINE == SE_TF2
	m_Mods.emplace_back(new CTeamFortress2Mod());
	m_Mods.emplace_back(new CTF2ClassifiedMod());
#elif SOURCE_ENGINE == SE_DODS
	m_Mods.emplace_back(new CDODMod());
#elif SOURCE_ENGINE == SE_CSS
	m_Mods.emplace_back(new CCounterStrikeSourceMod());
#elif SOURCE_ENGINE == SE_HL2DM
	m_Mods.emplace_back(new CHalfLifeDeathmatchMod());
#elif SOURCE_ENGINE == SE_SDK2013
	m_Mods.emplace_back(new CFortressForeverMod());
	m_Mods.emplace_back(new CSynergyMod());
	m_Mods.emplace_back(new CTF2ClassifiedMod());
	m_Mods.emplace_back(new CEmpiresMod());
#else
	//TODO: Add Black Mesa Source support [APG]RoboCop[CL]
	m_Mods.emplace_back(new CHLDMSourceMod());

	// Look for extra MODs

	parseFile();
#endif
}

//////////////////////////////////////////////////////////////////////////////

void CBotMod::setup(const char* szModFolder, const eModId iModId, const eBotType iBotType, const char* szWeaponListName)
{
	m_szModFolder = CStrings::getString(szModFolder);
	m_iModId = iModId;
	m_iBotType = iBotType;

	if (szWeaponListName && *szWeaponListName)
		m_szWeaponListName = CStrings::getString(szWeaponListName);
}

/*CBot *CBotMod :: makeNewBots ()
{
	return NULL;
}*/

bool CBotMod::isModFolder(const char* szModFolder) const
{
	return !strcmpi(m_szModFolder, szModFolder);
}

char* CBotMod::getModFolder() const
{
	return m_szModFolder;
}

eModId CBotMod::getModId() const
{
	return m_iModId;
}

//
// MOD LIST

std::vector<CBotMod*> CBotMods::m_Mods;

void CBotMods::freeMemory()
{
	for (CBotMod*& m_Mod : m_Mods)
	{
		m_Mod->freeMemory();
		delete m_Mod;
		m_Mod = nullptr;
	}

	m_Mods.clear();
}

CBotMod* CBotMods::getMod(char* szModFolder)
{
	for (CBotMod* const& m_Mod : m_Mods)
	{
		if (m_Mod->isModFolder(szModFolder))
		{
			logger->Log(LogLevel::INFO, "HL2 MOD ID %d (Game Folder = %s) FOUND", m_Mod->getModId(), szModFolder);

			return m_Mod;
		}
	}

	logger->Log(LogLevel::FATAL, "HL2 MODIFICATION \"%s\" NOT FOUND, EXITING... see bot_mods.ini in bot config folder", szModFolder);

	for (CBotMod* const& m_Mod : m_Mods)
	{
		logger->Log(LogLevel::FATAL, "Registered mod: \"%s\" (id: %d)", m_Mod->getModFolder(), m_Mod->getModId());
	}

	return nullptr;
}

void CBotMod::initMod()
{
	m_bPlayerHasSpawned = false;

	CWeapons::loadWeapons(m_szWeaponListName, nullptr);
}

void CBotMod::mapInit()
{
	m_bPlayerHasSpawned = false;
}

bool CBotMod::playerSpawned(edict_t* pPlayer)
{
	if (m_bPlayerHasSpawned)
		return false;

	m_bPlayerHasSpawned = true;

	return true;
}

bool CHalfLifeDeathmatchMod::playerSpawned(edict_t* pPlayer)
{
	if (CBotMod::playerSpawned(pPlayer))
	{
		m_LiftWaypoints.clear();

		CWaypoints::updateWaypointPairs(&m_LiftWaypoints, CWaypointTypes::W_FL_LIFT, "func_button");
	}

	return true;
}

void CFortressForeverMod::initMod()
{
	CWeapons::loadWeapons(m_szWeaponListName == nullptr ? "FF" : m_szWeaponListName, FFWeaps.data());
}

void CFortressForeverMod::getTeamOnlyWaypointFlags(const int iTeam, int *iOn, int *iOff)
{
	if (iTeam == FF_TEAM_BLUE)
	{
		*iOn = CWaypointTypes::W_FL_NORED;
		*iOff = CWaypointTypes::W_FL_NOBLU;
	}
	else if (iTeam == FF_TEAM_RED)
	{
		*iOn = CWaypointTypes::W_FL_NOBLU;
		*iOff = CWaypointTypes::W_FL_NORED;
	}
}

bool CFortressForeverMod::checkWaypointForTeam(CWaypoint *pWpt, const int iTeam)
{
	return (!pWpt->hasFlag(CWaypointTypes::W_FL_NOBLU) || iTeam != FF_TEAM_BLUE) &&
		   (!pWpt->hasFlag(CWaypointTypes::W_FL_NORED) || iTeam != FF_TEAM_RED);
}

void CHalfLifeDeathmatchMod::initMod()
{
	CWeapons::loadWeapons(m_szWeaponListName == nullptr ? "HL2DM" : m_szWeaponListName, HL2DMWeaps.data());

	//	for ( i = 0; i < HL2DM_WEAPON_MAX; i ++ )
	//	CWeapons::addWeapon(new CWeapon(HL2DMWeaps[i]));//.iSlot,HL2DMWeaps[i].szWeaponName,HL2DMWeaps[i].iId,HL2DMWeaps[i].m_iFlags,HL2DMWeaps[i].m_iAmmoIndex,HL2DMWeaps[i].minPrimDist,HL2DMWeaps[i].maxPrimDist,HL2DMWeaps[i].m_iPreference,HL2DMWeaps[i].m_fProjSpeed));
}

void CHalfLifeDeathmatchMod::mapInit()
{
	CBotMod::mapInit();

	m_LiftWaypoints.clear();
}
