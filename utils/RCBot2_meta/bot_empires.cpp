#include "bot_empires.h"
#include "bot_empires_binary.h"
#include "bot_empires_combat_fixture_waypoints.h"
#include "bot_empires_squad_join.h"
#include "bot_empires_squad_observation.h"
#include "bot_weapons.h"
#include "in_buttons.h"
#include "bot_buttons.h"

#include "bot_getprop.h"
#include "bot_cvars.h"
#include "bot_globals.h"
#include "bot_kv.h"
#include "bot_navigator.h"
#include "bot_navmesh_api.h"
#include "bot_empires_native_placement.h"
#include "bot_profile.h"
#include "bot_schedule.h"
#include "bot_waypoint.h"
#include "bot_waypoint_locations.h"
#include "bot_empires_telemetry.h"
#include "bot_local_yielding.h"
#include "bot_empires_squad_runtime.h"
#include "bot_squad_spatial_movement.h"
#include "bot_commander_move_test_fixture.h"
#include "rcbot/logging.h"

#include "dt_send.h"
#include "server_class.h"
#include "const.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreorder"
#endif
#include "ientityinfo.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

extern IServerGameDLL* servergamedll;
extern IServerGameEnts* servergameents;
extern IServerTools* servertools;
void getGrenadeAngle(double v, double g, double desx, double desy,
	float* fa1, float* fa2);

namespace
{
	class CCommanderEntryTraceFilter : public CTraceFilter
	{
	public:
		explicit CCommanderEntryTraceFilter(const IHandleEntity* passEntity)
			: m_passEntity(passEntity) {}
		bool ShouldHitEntity(IHandleEntity* entity, int) override
		{
			return entity != m_passEntity;
		}
	private:
		const IHandleEntity* m_passEntity;
	};

	// The Use press only works when the eye ray actually reaches the vehicle;
	// distance to its center says nothing about that.
	bool TraceCommanderEntryUse(edict_t* actor, edict_t* vehicle,
		const Vector& eye, trace_t& hit)
	{
		hit = trace_t{};
		if (!enginetrace || !actor || !vehicle || !actor->GetUnknown() ||
			!vehicle->GetUnknown() || !eye.IsValid())
			return false;
		Vector forward = CBotGlobals::entityOrigin(vehicle) - eye;
		if (!forward.IsValid() || VectorNormalize(forward) <= 0.0f)
			return false;
		Ray_t ray;
		ray.Init(eye, eye + forward * 96.0f);
		CCommanderEntryTraceFilter filter(actor->GetUnknown());
		enginetrace->TraceRay(ray, MASK_SOLID, &filter, &hit);
		return !hit.startsolid && !hit.allsolid && hit.fraction < 1.0f &&
			hit.m_pEnt == vehicle->GetUnknown()->GetBaseEntity();
	}

	class CEmpiresEngineerShotTraceFilter : public CTraceFilter
	{
	public:
		explicit CEmpiresEngineerShotTraceFilter(const IHandleEntity* passEntity)
			: m_passEntity(passEntity)
		{
		}

		bool ShouldHitEntity(IHandleEntity* entity, int) override
		{
			if (entity == m_passEntity)
				return false;

			// This filter is used only by the bounded Engineer repair diagnostic.
			// The current Empires kit uses the game-side trace collision group
			// 21, which rejects vehicle-clip proxies (group 12).  Reproduce that
			// known, public collision distinction here without calling private
			// game code.  This is not a production repair-target resolver.
			if (enginetrace != nullptr)
			{
				const ICollideable* collideable =
					enginetrace->GetCollideable(entity);
				if (collideable != nullptr &&
					collideable->GetCollisionGroup() ==
						COLLISION_GROUP_VEHICLE_CLIP)
				{
					return false;
				}
			}

			return true;
		}

		TraceType_t GetTraceType() const override
		{
			return TRACE_EVERYTHING;
		}

	private:
		const IHandleEntity* m_passEntity;
	};

	// Empires 2.39.4 scripts/weapon_emp_{nf,imp}_mortar.txt both define
	// Velocity=1800.  This is used only by the explicit one-shot aim probe;
	// normal weapon scoring remains unchanged.
	constexpr float kEmpiresMortarAimProjectileSpeed = 1800.0f;
	// CEmpPlayer::Command_emp_unit_order maps Commander MOVE to type 1.
	constexpr int kEmpiresCommanderMoveType = 1;
	constexpr float kEmpiresCommanderMoveTargetChangeRadius = 16.0f;
	constexpr float kEmpiresWorldPositionAnchorSearchRange = 1024.0f;
	constexpr RCBotOrder::IssuerId kEmpiresCommanderIssuerId = 1;
	RCBotEmpiresSquadRuntime::Adapter g_empireSquadRuntimeShadow;
	RCBotSquadOrder::SquadIdentityTracker g_empireSquadMoveIdentityTracker;
	RCBotSquadOrder::SquadMoveRecipient g_empireSquadMoveRecipient;
	RCBotOrder::OrderId g_nextEmpiresSquadMoveOrderId = 900000;
	RCBotOrder::Generation g_nextEmpiresSquadMoveOrderGeneration = 0;
	std::uint64_t g_nextEmpiresSquadSpatialRequestToken = 1;

	void RecordEmpiresSquadSpatialMoveEvent(CBot* bot, const char* event,
		const RCBotSquadOrder::NavigationIntentRequest& request,
		const std::uint64_t navigationIntentId = 0,
		const std::uint64_t navigationAttemptId = 0,
		const char* provider = nullptr, const char* terminal = nullptr,
		const char* outcome = nullptr)
	{
		if (bot == nullptr || event == nullptr)
			return;
		char detail[768];
		std::snprintf(detail, sizeof(detail),
			"parent_order_id=%llu parent_generation=%llu child_order_id=%llu child_generation=%llu squad_generation=%llu decision_revision=%llu formation_generation=%llu member_id=%llu slot_id=%llu claim_key=%u:%llu:%llu navigation_intent_id=%llu navigation_attempt_id=%llu provider=%s terminal=%s child_outcome=%s target=(%.1f %.1f %.1f)",
			static_cast<unsigned long long>(request.parentOrder.orderId),
			static_cast<unsigned long long>(request.parentOrder.generation),
			static_cast<unsigned long long>(request.childExecution.orderId),
			static_cast<unsigned long long>(request.childExecution.generation),
			static_cast<unsigned long long>(request.squadGeneration),
			static_cast<unsigned long long>(request.decisionRevision),
			static_cast<unsigned long long>(request.formationGeneration),
			static_cast<unsigned long long>(request.memberId),
			static_cast<unsigned long long>(request.slotId),
			static_cast<unsigned>(request.claimResourceKind),
			static_cast<unsigned long long>(request.claimResourceId),
			static_cast<unsigned long long>(request.claimResourceGeneration),
			static_cast<unsigned long long>(navigationIntentId),
			static_cast<unsigned long long>(navigationAttemptId),
			provider != nullptr ? provider : "none",
			terminal != nullptr ? terminal : "none",
			outcome != nullptr ? outcome : "none", request.target.x,
			request.target.y, request.target.z);
		EmpiresAnalysis::RecordBotEvent(bot, event, nullptr, detail);
	}

	bool StartEmpiresSquadSpatialNavigation(void*,
		const RCBotSquadOrder::NavigationIntentRequest& request,
		std::uint64_t* requestToken)
	{
		if (requestToken == nullptr || engine == nullptr || gpGlobals == nullptr)
			return false;
		for (int index = 1; index <= gpGlobals->maxClients; ++index)
		{
			edict_t* player = engine->PEntityOfEntIndex(index);
			if (player == nullptr || player->IsFree() ||
				engine->GetPlayerUserId(player) !=
					static_cast<int>(request.memberId))
				continue;
			CBot* base = CBots::getBotPointer(player);
			if (base == nullptr || base->isControlledTestOnly())
				return false;
			CBotEmpires* bot = static_cast<CBotEmpires*>(base);
			if (g_nextEmpiresSquadSpatialRequestToken == 0)
				++g_nextEmpiresSquadSpatialRequestToken;
			std::uint64_t token =
				g_nextEmpiresSquadSpatialRequestToken++;
			if (!bot->requestEmpiresSquadSpatialNavigation(request, &token))
				return false;
			*requestToken = token;
			RecordEmpiresSquadSpatialMoveEvent(bot,
				"SQUAD_SPATIAL_MOVE_REQUESTED", request);
			return true;
		}
		return false;
	}

	void CancelEmpiresSquadSpatialNavigation(void*,
		const RCBotSquadOrder::NavigationIntentRequest& request,
		const std::uint64_t requestToken)
	{
		if (engine == nullptr || gpGlobals == nullptr)
			return;
		for (int index = 1; index <= gpGlobals->maxClients; ++index)
		{
			edict_t* player = engine->PEntityOfEntIndex(index);
			if (player == nullptr || player->IsFree() ||
				engine->GetPlayerUserId(player) !=
					static_cast<int>(request.memberId))
				continue;
			CBot* base = CBots::getBotPointer(player);
			if (base == nullptr)
				return;
			CBotEmpires* bot = static_cast<CBotEmpires*>(base);
			bot->cancelEmpiresSquadSpatialNavigation(request, requestToken);
			RecordEmpiresSquadSpatialMoveEvent(bot,
				"SQUAD_SPATIAL_MOVE_CANCELLED", request,
				requestToken);
			return;
		}
	}

	void RecordEmpiresSquadRuntimeTelemetry(void* const context,
		const RCBotEmpiresSquadRuntime::Telemetry& telemetry)
	{
		CBot* bot = static_cast<CBot*>(context);
		if (bot != nullptr)
			EmpiresAnalysis::RecordSquadShadowEvent(bot, telemetry);
	}

	void RecordCommanderMoveShadowTelemetry(void* const context,
		const RCBotOrderTelemetry::TelemetryEvent& event)
	{
		CBotEmpires* bot = static_cast<CBotEmpires*>(context);
		if (bot != nullptr)
			EmpiresAnalysis::RecordOrderProtocolEvent(bot, event);
	}

	RCBotOrder::DetailReason CommanderMoveAbortReason(const char* reason)
	{
		if (reason != nullptr &&
			(std::strcmp(reason, "bot-died") == 0 ||
				std::strcmp(reason, "bot-dead") == 0 ||
				std::strcmp(reason, "spawn-reset") == 0 ||
				std::strcmp(reason, "respawn") == 0 ||
				std::strcmp(reason, "map-change") == 0 ||
				std::strcmp(reason, "lifecycle-cleanup") == 0 ||
				std::strcmp(reason, "lifecycle-invalidated") == 0 ||
				std::strcmp(reason, "team-changed") == 0))
			return RCBotOrder::DetailReason::DEATH_OR_LIFECYCLE;
		return RCBotOrder::DetailReason::CANCELLED;
	}

	RCBotOrderTelemetry::NavigationProvider CommanderNavigationProvider(
		const RCBotNavigation::Provider provider)
	{
		switch (provider)
		{
		case RCBotNavigation::Provider::WAYPOINT:
			return RCBotOrderTelemetry::NavigationProvider::WAYPOINT;
		case RCBotNavigation::Provider::NAVMESH:
			return RCBotOrderTelemetry::NavigationProvider::NAVMESH;
		case RCBotNavigation::Provider::LOCAL_EXPLORER:
			return RCBotOrderTelemetry::NavigationProvider::LOCAL_EXPLORER;
		case RCBotNavigation::Provider::UNKNOWN:
		case RCBotNavigation::Provider::BUILDING_RELATIVE:
			return RCBotOrderTelemetry::NavigationProvider::UNKNOWN;
		}
		return RCBotOrderTelemetry::NavigationProvider::UNKNOWN;
	}

	RCBotOrderTelemetry::NavigationTerminalResult CommanderNavigationResult(
		const RCBotNavigation::Result result)
	{
		switch (result)
		{
		case RCBotNavigation::Result::COMPLETE:
			return RCBotOrderTelemetry::NavigationTerminalResult::COMPLETE;
		case RCBotNavigation::Result::CANCELLED:
			return RCBotOrderTelemetry::NavigationTerminalResult::CANCELLED;
		case RCBotNavigation::Result::PATH_FAILED:
			return RCBotOrderTelemetry::NavigationTerminalResult::PATH_FAILED;
		case RCBotNavigation::Result::PATH_STUCK:
			return RCBotOrderTelemetry::NavigationTerminalResult::PATH_STUCK;
		case RCBotNavigation::Result::UNREACHABLE:
			return RCBotOrderTelemetry::NavigationTerminalResult::UNREACHABLE;
		case RCBotNavigation::Result::NONE:
			return RCBotOrderTelemetry::NavigationTerminalResult::NONE;
		}
		return RCBotOrderTelemetry::NavigationTerminalResult::NONE;
	}

	RCBotOrderTelemetry::RuntimeContext CommanderNavigationContext(
		const RCBotNavigation::NavigationIntent& intent,
		const RCBotNavigation::Result result = RCBotNavigation::Result::NONE)
	{
		RCBotOrderTelemetry::RuntimeContext context;
		context.hasNavigationIntentId = intent.id != 0;
		context.navigationIntentId = intent.id;
		context.hasNavigationAttemptId = intent.attemptId != 0;
		context.navigationAttemptId = intent.attemptId;
		context.hasProvider = intent.provider != RCBotNavigation::Provider::UNKNOWN;
		context.provider = CommanderNavigationProvider(intent.provider);
		context.hasNavigationTerminalResult = result != RCBotNavigation::Result::NONE;
		context.navigationTerminalResult = CommanderNavigationResult(result);
		return context;
	}

	constexpr float kEmpiresEngineerRepairActionSeconds = 12.0f;
	// Empires' server binary contains the static sv_gravity ConVar with the
	// default string "800".  The plugin-side ConVarRef is not registered by
	// this game (as logged at startup), so use the same 800 fallback already
	// used by CBotGlobals::grenadeWillLand().
	constexpr float kEmpiresMortarAimDefaultGravity = 800.0f;
	// Keep the first production Grenadier tactic deliberately conservative:
	// close targets stay on the normal combat path, while very distant targets
	// are not claimed until a broader map/range proof exists.
	constexpr float kEmpiresGrenadierMortarMinRange = 900.0f;
	constexpr float kEmpiresGrenadierMortarMaxRange = 3000.0f;
	constexpr float kEmpiresGrenadierMortarCooldown = 30.0f;
	// A newly placed MG turret starts at 25/100.  The normal Engineer-kit
	// repair path advances it over time; keep this debug-only action bounded,
	// but long enough to observe the already-proven construction effect reach
	// the authoritative completion state.
	constexpr float kEmpiresEngineerBuildActionSeconds = 20.0f;
	constexpr float kEmpiresVehicleFactoryInsideRadius = 64.0f;
	constexpr float kEmpiresVehicleApproachRadius = 96.0f;
	constexpr float kEmpiresVehicleActionSeconds = 150.0f;
	constexpr float kEmpiresVehicleDriveSeconds = 2.5f;
	constexpr float kEmpiresVehicleMovementThreshold = 48.0f;
	constexpr float kEmpiresVehicleDriveSpeed = 400.0f;
	constexpr float kEmpiresVehiclePurchaseCommandInterval = 0.15f;
	constexpr float kEmpiresVehicleTutorialExitRadius = 128.0f;
	constexpr float kEmpiresVehicleTutorialTeleportDistance = 1024.0f;
	constexpr float kEmpiresVehicleTutorialTeleportWait = 5.0f;
	constexpr float kEmpiresVehicleRouteSearchRange = 16384.0f;

	int countEmpiresEngineerMGTurrets(int* highestIndex = nullptr)
	{
		if (highestIndex != nullptr)
			*highestIndex = -1;
		if (servertools == nullptr || engine == nullptr)
			return 0;

		int count = 0;
		for (CBaseEntity* pEntity = servertools->FirstEntity();
			pEntity != nullptr; pEntity = servertools->NextEntity(pEntity))
		{
			IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
			IServerNetworkable* pNetworkable = pUnknown != nullptr ?
				pUnknown->GetNetworkable() : nullptr;
			const char* className = pNetworkable != nullptr ?
				pNetworkable->GetClassName() : nullptr;
			edict_t* pEdict = pNetworkable != nullptr ?
				pNetworkable->GetEdict() : nullptr;
			if (className == nullptr || pEdict == nullptr || pEdict->IsFree() ||
				std::strcmp(className, "emp_building_mgturret") != 0)
			{
				continue;
			}

			++count;
			if (highestIndex != nullptr)
				*highestIndex = std::max(*highestIndex, engine->IndexOfEdict(pEdict));
		}
		return count;
	}
	// Current Empires CWeaponMortar::PrimaryAttack converts the player's
	// EyeVectors to angles and subtracts this verified launch-pitch offset
	// before creating the shell.  The helper stores the compensated player
	// view pitch, while the ballistic solver works in launch-pitch space.
	constexpr float kEmpiresMortarAimLaunchPitchOffset = 16.4f;
	constexpr float kEmpiresMortarLaunchForwardOffset = 14.0f;
	constexpr float kEmpiresMortarLaunchUpOffset = -3.0f;
	bool readEmpiresEntityVectorProperty(edict_t* pEdict, const char* propertyName,
		Vector& value)
	{
		if (pEdict == nullptr || propertyName == nullptr ||
			pEdict->GetUnknown() == nullptr ||
			pEdict->GetNetworkable() == nullptr)
		{
			return false;
		}

		CBaseEntity* pEntity = pEdict->GetUnknown()->GetBaseEntity();
		ServerClass* pServerClass = pEdict->GetNetworkable()->GetServerClass();
		unsigned offset = 0;
		if (pEntity == nullptr || pServerClass == nullptr ||
			!UTIL_FindSendPropInfo(pServerClass, propertyName, &offset) ||
			offset == 0)
		{
			return false;
		}

		const float* components = reinterpret_cast<const float*>(
			reinterpret_cast<const char*>(pEntity) + offset);
		value = Vector(components[0], components[1], components[2]);
		return value.IsValid();
	}

	bool readConfiguredEmpiresSpawnId(const int team, int& spawnId)
	{
		spawnId = -1;
		if (filesystem == nullptr || gpGlobals == nullptr ||
			(team != EMP_TEAM_NF && team != EMP_TEAM_BE))
		{
			return false;
		}

		const char* mapName = gpGlobals->mapname.ToCStr();
		if (mapName == nullptr || mapName[0] == '\0')
		{
			return false;
		}

		char filename[512] = {};
		CBotGlobals::buildFileName(filename, "emp_map_spawnpoints", BOT_CONFIG_FOLDER, "ini");
		std::fstream spawnFile(filename, std::fstream::in);
		if (!spawnFile)
		{
			return false;
		}

		CRCBotKeyValueList spawnConfig;
		spawnConfig.parseFile(spawnFile);

		char key[128] = {};
		std::snprintf(key, sizeof(key), "%s_%s", mapName,
			team == EMP_TEAM_NF ? "nf" : "be");
		int configuredId = -1;
		if (!spawnConfig.getInt(key, &configuredId) || configuredId < 0)
		{
			return false;
		}

		spawnId = configuredId;
		return true;
	}

	struct EmpiresClassSpec
	{
		int id;
		const char* name;
	};

	constexpr EmpiresClassSpec kEmpiresInfantryClasses[] =
	{
		{ EMP_CLASS_SCOUT, "scout" },
		{ EMP_CLASS_RIFLEMAN, "rifleman" },
		{ EMP_CLASS_GRENADIER, "grenadier" },
		{ EMP_CLASS_ENGINEER, "engineer" }
	};

	struct EmpiresLoadoutSpec
	{
		int team;
		int classId;
		int sidearm;
		int primary;
		int grenade;
		int equipment;
	};

	// Current Empires 2.39.4 command tuples, applied in the verified order:
	// changeclass, changesidearm, changeprimary, changegrenade,
	// changeequipment.  The final command is the server's
	// ValidateLoadout(true) commit trigger. Personal skill selection remains
	// gated by the game's progression/unlock rules and is not part of this
	// normal loadout tuple. Squad-Leader skills are a separate squad-point
	// system and are not represented here either.
	constexpr EmpiresLoadoutSpec kEmpiresNormalLoadouts[] =
	{
		{ EMP_TEAM_NF, EMP_CLASS_SCOUT, 1, 4, 17, 20 },
		{ EMP_TEAM_NF, EMP_CLASS_RIFLEMAN, 1, 6, 12, 21 },
		{ EMP_TEAM_NF, EMP_CLASS_GRENADIER, 1, 10, 18, 22 },
		{ EMP_TEAM_NF, EMP_CLASS_ENGINEER, 1, 3, 14, 19 },
		{ EMP_TEAM_BE, EMP_CLASS_SCOUT, 23, 26, 38, 41 },
		{ EMP_TEAM_BE, EMP_CLASS_RIFLEMAN, 23, 28, 33, 42 },
		{ EMP_TEAM_BE, EMP_CLASS_GRENADIER, 23, 31, 39, 43 },
		{ EMP_TEAM_BE, EMP_CLASS_ENGINEER, 23, 25, 35, 40 }
	};

	const EmpiresLoadoutSpec* findEmpiresNormalLoadout(const int team, const int classId)
	{
		for (const EmpiresLoadoutSpec& loadout : kEmpiresNormalLoadouts)
		{
			if (loadout.team == team && loadout.classId == classId)
				return &loadout;
		}

		return nullptr;
	}

	const char* empiresMortarProbeStateName(const EmpiresMortarProbeState state)
	{
		switch (state)
		{
		case EmpiresMortarProbeState::IDLE: return "IDLE";
		case EmpiresMortarProbeState::WAITING_FOR_ACTIVE: return "WAITING_FOR_ACTIVE";
		case EmpiresMortarProbeState::WAITING_FOR_READY: return "WAITING_FOR_READY";
		case EmpiresMortarProbeState::ATTACK_ARMED: return "ATTACK_ARMED";
		case EmpiresMortarProbeState::ATTACK_SENT: return "ATTACK_SENT";
		case EmpiresMortarProbeState::OBSERVING_SHELL: return "OBSERVING_SHELL";
		case EmpiresMortarProbeState::COUCH_PREP: return "COUCH_PREP";
		case EmpiresMortarProbeState::RELOAD_WAIT: return "RELOAD_WAIT";
		case EmpiresMortarProbeState::COMPLETE: return "COMPLETE";
		case EmpiresMortarProbeState::FAILED: return "FAILED";
		}

		return "UNKNOWN";
	}

	const char* empiresMortarClassName(const int team)
	{
		if (team == EMP_TEAM_NF)
			return "weapon_emp_nf_mortar";
		if (team == EMP_TEAM_BE)
			return "weapon_emp_imp_mortar";
		return nullptr;
	}

	const char* empiresMineClassName(const int team)
	{
		if (team == EMP_TEAM_NF)
			return "weapon_emp_nf_mine_grenade";
		if (team == EMP_TEAM_BE)
			return "weapon_emp_imp_mine_grenade";
		return nullptr;
	}

	const char* empiresBinocularClassName(const int team)
	{
		if (team == EMP_TEAM_NF)
			return "weapon_emp_nf_binoculars";
		if (team == EMP_TEAM_BE)
			return "weapon_emp_imp_binoculars";
		return nullptr;
	}

	const char* empiresMineProjectileClassName(const int team)
	{
		if (team == EMP_TEAM_NF)
			return "emp_nf_mine_grenade_projectile";
		if (team == EMP_TEAM_BE)
			return "emp_imp_mine_grenade_projectile";
		return nullptr;
	}

	int countEmpiresMortarShells(int* firstIndex = nullptr,
		const char** firstClassName = nullptr)
	{
		if (firstIndex != nullptr)
			*firstIndex = -1;
		if (firstClassName != nullptr)
			*firstClassName = nullptr;

		if (engine == nullptr || gpGlobals == nullptr)
			return 0;

		int count = 0;
		for (int index = gpGlobals->maxClients + 1; index <= gpGlobals->maxEntities; ++index)
		{
			edict_t* pEntity = engine->PEntityOfEntIndex(index);
			const char* className = pEntity != nullptr && !pEntity->IsFree() ?
				pEntity->GetClassName() : nullptr;
			if (className == nullptr ||
				(std::strcmp(className, "emp_shell") != 0 &&
					std::strcmp(className, "nf_mortar_shell") != 0 &&
					std::strcmp(className, "imp_mortar_shell") != 0))
			{
				continue;
			}

			if (firstIndex != nullptr && *firstIndex < 0)
				*firstIndex = index;
			if (firstClassName != nullptr && *firstClassName == nullptr)
				*firstClassName = className;
			++count;
		}

		return count;
	}

	int countEmpiresMineProjectiles(const int team, int* firstIndex = nullptr,
		const char** firstClassName = nullptr)
	{
		if (firstIndex != nullptr)
			*firstIndex = -1;
		if (firstClassName != nullptr)
			*firstClassName = nullptr;

		const char* expected = empiresMineProjectileClassName(team);
		if (expected == nullptr || engine == nullptr || gpGlobals == nullptr)
			return 0;

		int count = 0;
		for (int index = gpGlobals->maxClients + 1;
			index <= gpGlobals->maxEntities; ++index)
		{
			edict_t* pEntity = engine->PEntityOfEntIndex(index);
			const char* className = pEntity != nullptr && !pEntity->IsFree() ?
				pEntity->GetClassName() : nullptr;
			if (className == nullptr || std::strcmp(className, expected) != 0)
				continue;

			if (firstIndex != nullptr && *firstIndex < 0)
				*firstIndex = index;
			if (firstClassName != nullptr && *firstClassName == nullptr)
				*firstClassName = className;
			++count;
		}

		return count;
	}

	bool equalsIgnoreCase(const char* value, const char* expected)
	{
		if (value == nullptr || expected == nullptr)
			return false;

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
	}

bool parseClassId(const char* value, int& classId)
	{
		if (value == nullptr || *value == '\0')
			return false;

		int parsed = 0;
		for (const char* cursor = value; *cursor != '\0'; ++cursor)
		{
			if (*cursor < '0' || *cursor > '9')
				return false;
			parsed = parsed * 10 + (*cursor - '0');
			if (parsed > EMP_CLASS_ENGINEER)
				return false;
		}

		classId = parsed;
		return true;
}

	bool parseSpawnPointId(const char* value, int& spawnId)
	{
		if (value == nullptr || *value == '\0')
			return false;

		int parsed = 0;
		for (const char* cursor = value; *cursor != '\0'; ++cursor)
		{
			if (*cursor < '0' || *cursor > '9')
				return false;
			parsed = parsed * 10 + (*cursor - '0');
			if (parsed < 0)
				return false;
		}

		spawnId = parsed;
		return true;
	}

	const char* spawnConfirmationStateName(const EmpiresBootstrapSpawnConfirmationState state)
	{
		switch (state)
		{
		case EmpiresBootstrapSpawnConfirmationState::IDLE:
			return "IDLE";
		case EmpiresBootstrapSpawnConfirmationState::ATTACK_PRESS_PENDING:
			return "ATTACK_PRESS_PENDING";
		case EmpiresBootstrapSpawnConfirmationState::ATTACK_RELEASE_PENDING:
			return "ATTACK_RELEASE_PENDING";
		case EmpiresBootstrapSpawnConfirmationState::OBSERVING_RESULT:
			return "OBSERVING_RESULT";
		case EmpiresBootstrapSpawnConfirmationState::COMPLETE:
			return "COMPLETE";
		case EmpiresBootstrapSpawnConfirmationState::FAILED:
			return "FAILED";
		}

		return "UNKNOWN";
	}

	const char* normalSpawnStateName(const EmpiresNormalSpawnState state)
	{
		switch (state)
		{
		case EmpiresNormalSpawnState::IDLE:
			return "IDLE";
		case EmpiresNormalSpawnState::SPAWNPOINT_REQUESTED:
			return "SPAWNPOINT_REQUESTED";
		case EmpiresNormalSpawnState::WAITING_FOR_RESPAWNABLE:
			return "WAITING_FOR_RESPAWNABLE";
		case EmpiresNormalSpawnState::ATTACK_FRAME_1:
			return "ATTACK_FRAME_1";
		case EmpiresNormalSpawnState::ATTACK_FRAME_2:
			return "ATTACK_FRAME_2";
		case EmpiresNormalSpawnState::RELEASE_FRAME:
			return "RELEASE_FRAME";
		case EmpiresNormalSpawnState::OBSERVING_RESULT:
			return "OBSERVING_RESULT";
		case EmpiresNormalSpawnState::COMPLETE:
			return "COMPLETE";
		case EmpiresNormalSpawnState::FAILED:
			return "FAILED";
		}

		return "UNKNOWN";
	}

	const char* lifeStateName(const int state)
	{
		switch (state)
		{
		case LIFE_ALIVE:
			return "LIFE_ALIVE";
		case LIFE_DYING:
			return "LIFE_DYING";
		case LIFE_DEAD:
			return "LIFE_DEAD";
		case LIFE_RESPAWNABLE:
			return "LIFE_RESPAWNABLE";
		case LIFE_DISCARDBODY:
			return "LIFE_DISCARDBODY";
		}

		return "LIFE_UNKNOWN";
	}

	enum class EmpiresPlayerObservation
	{
		UNKNOWN = -1,
		EMPTY = 0,
		SELF,
		FRIENDLY,
		ENEMY,
		UNASSIGNED,
		SPECTATOR,
		DEAD
	};

	const char* empiresPlayerObservationName(const int observation)
	{
		switch (static_cast<EmpiresPlayerObservation>(observation))
		{
		case EmpiresPlayerObservation::UNKNOWN:
			return "UNKNOWN";
		case EmpiresPlayerObservation::EMPTY:
			return "EMPTY";
		case EmpiresPlayerObservation::SELF:
			return "SELF";
		case EmpiresPlayerObservation::FRIENDLY:
			return "FRIENDLY";
		case EmpiresPlayerObservation::ENEMY:
			return "ENEMY";
		case EmpiresPlayerObservation::UNASSIGNED:
			return "UNASSIGNED";
		case EmpiresPlayerObservation::SPECTATOR:
			return "SPECTATOR";
		case EmpiresPlayerObservation::DEAD:
			return "DEAD";
		}

		return "UNKNOWN";
	}

bool controllerProbeRiflemanAcknowledged(const EmpiresBootstrapPlayerState& state)
	{
		// Empires exposes the pending class acknowledgement as a bool. The
		// command that produced it is fixed to class 1 in this probe, so a
		// current class readback of 1 is also accepted when the game has already
		// applied the request and cleared the pending flag.
		return (state.classDesiredKnown && state.classDesired) ||
			(state.classKnown && state.classId == EMP_CLASS_RIFLEMAN);
}

constexpr float kControllerProbeReadinessSafetySeconds = 20.0f;
constexpr float kControllerProbeNeutralReadinessSafetySeconds = 20.0f;
constexpr int kControllerProbeNeutralReadinessHardCommandCap = 2048;
constexpr int kControllerProbeNeutralReadinessSetupSamples = 2;
constexpr int kControllerProbeNeutralReadinessProgressInterval = 64;
constexpr float kControllerProbeLoadoutReadinessSafetySeconds = 30.0f;
constexpr float kControllerProbeLoadoutNeutralSafetySeconds = 20.0f;
constexpr int kControllerProbeLoadoutReadinessSetupSamples = 2;
constexpr int kControllerProbeLoadoutReadinessProgressInterval = 64;
constexpr int kControllerProbeAppliedLoadoutObservationFrames = 10;
constexpr int kControllerProbeSpawnConfirmReadyObservationFrames = 120;
// Keep this as a bounded one-shot proof, but long enough to make the
// forward run visible in-game and reach nearby collision geometry.
constexpr int kControllerProbeMovementActiveFrames = 120;
constexpr int kControllerProbeMovementFinalObservationFrames = 2;
constexpr int kControllerProbeMovementSafetyFrames = 256;
constexpr float kControllerProbeMovementForwardSpeed = 320.0f;
constexpr float kControllerProbeMovementMinimumDisplacement = 8.0f;
constexpr float kControllerProbeWaypointMinimumTargetDistance = 64.0f;
constexpr float kControllerProbeWaypointMaximumTargetDistance = 512.0f;
constexpr float kControllerProbeWaypointArrivalDistance = 48.0f;
constexpr float kControllerProbeWaypointMaximumSeconds = 10.0f;
constexpr int kControllerProbeWaypointNoProgressFrames = 90;
constexpr float kControllerProbeWaypointNoProgressEpsilon = 2.0f;
constexpr int kControllerProbePathStartWaypoint = 145;
constexpr int kControllerProbePathDestinationWaypoint = 157;
constexpr int kControllerProbePathMinimumNodes = 5;
constexpr int kControllerProbePathMaximumNodes = 10;
constexpr int kControllerProbePathSearchMaximumFrames = 32;
constexpr int kControllerProbePathMaximumFrames = 1536;
constexpr float kControllerProbePathMaximumSeconds = 45.0f;
constexpr int kControllerProbeWaypointMaximumFrames = 768;
constexpr int kControllerProbeWaypointRouteMaximumTargets = 4;
constexpr int kControllerProbeWaypointRouteSegmentFrames = 30;
constexpr int kControllerProbeLoadoutSidearm = 1;
constexpr int kControllerProbeLoadoutPrimary = 6;
constexpr int kControllerProbeLoadoutGrenade = 12;
constexpr int kControllerProbeLoadoutEquipment = 21;
constexpr float kNormalSpawnSelectionTimeout = 4.0f;
constexpr float kNormalSpawnReadinessTimeout = 20.0f;
// The successful controller path leaves the client in LIFE_RESPAWNABLE for
// about three seconds before submitting the attack edge. Preserve that
// observed server-side settling interval for normal bots as well.
constexpr float kNormalSpawnConfirmDelaySeconds = 3.0f;
constexpr float kNormalSpawnResultTimeout = 4.0f;
constexpr int kNormalPathMaximumFrames = 1536;
constexpr float kNormalPathMaximumSeconds = 45.0f;
constexpr int kNormalPathSearchMaximumFrames = 64;
constexpr int kNormalPathNoProgressFrames = 180;
constexpr float kNormalPathNoProgressEpsilon = 2.0f;
constexpr int kNormalPathStartWaypoint = 145;
constexpr int kNormalPathDestinationWaypoint = 155;

bool appliedLoadoutFieldsKnown(const EmpiresBootstrapPlayerState& state)
{
	return state.classKnown && state.classDesiredKnown &&
		state.appliedSidearmKnown && state.sidearmDesiredKnown &&
		state.appliedPrimaryKnown && state.primaryDesiredKnown &&
		state.appliedGrenadeKnown && state.grenadeDesiredKnown &&
		state.appliedEquipmentKnown && state.equipmentDesiredKnown;
}

bool appliedLoadoutMatches(const EmpiresBootstrapPlayerState& state)
{
	return appliedLoadoutFieldsKnown(state) && state.team == EMP_TEAM_NF &&
		state.classId == EMP_CLASS_RIFLEMAN &&
		state.appliedSidearm == kControllerProbeLoadoutSidearm &&
		state.appliedPrimary == kControllerProbeLoadoutPrimary &&
		state.appliedGrenade == kControllerProbeLoadoutGrenade &&
		state.appliedEquipment == kControllerProbeLoadoutEquipment &&
		!state.classDesired && !state.sidearmDesired &&
		!state.primaryDesired && !state.grenadeDesired &&
		!state.equipmentDesired;
}

bool controllerProbeSpawnConfirmReadyPredicate(
	const EmpiresBootstrapPlayerState& state)
{
	// LIFE_RESPAWNABLE plus zero health is the authoritative dead/waiting
	// state for this Empires build. IPlayerInfo::IsDead() is not guaranteed to
	// mirror that intermediate engine state, so it is logged separately below.
	return state.connected && state.active && state.fakeClient && !state.observer &&
		state.team == EMP_TEAM_NF && appliedLoadoutMatches(state) &&
		state.desiredSpawnKnown && state.desiredSpawn == 32 &&
		state.lifeStateKnown && state.lifeState == LIFE_RESPAWNABLE &&
		state.healthKnown && state.health == 0 && state.lastUserCommandKnown &&
		state.lastUserCommandButtons == 0 &&
		std::fabs(state.lastUserCommandForwardMove) <= 0.01f &&
		std::fabs(state.lastUserCommandSideMove) <= 0.01f &&
		std::fabs(state.lastUserCommandUpMove) <= 0.01f &&
		state.lastUserCommandImpulse == 0 &&
		state.lastUserCommandWeaponSelect == 0;
}

bool appliedValueChangedToExpected(const int before, const int after,
	const int expected)
{
	return before != expected && after == expected;
}

const char* classifyAppliedLoadout(const EmpiresBootstrapPlayerState& before,
	const EmpiresBootstrapPlayerState& after)
{
	if (!appliedLoadoutFieldsKnown(after))
		return "unknown";
	if (appliedLoadoutMatches(after))
		return "exact-application";

	const bool partial =
		appliedValueChangedToExpected(before.classId, after.classId,
			EMP_CLASS_RIFLEMAN) ||
		appliedValueChangedToExpected(before.appliedSidearm, after.appliedSidearm,
			kControllerProbeLoadoutSidearm) ||
		appliedValueChangedToExpected(before.appliedPrimary, after.appliedPrimary,
			kControllerProbeLoadoutPrimary) ||
		appliedValueChangedToExpected(before.appliedGrenade, after.appliedGrenade,
			kControllerProbeLoadoutGrenade) ||
		appliedValueChangedToExpected(before.appliedEquipment, after.appliedEquipment,
			kControllerProbeLoadoutEquipment);
	if (partial)
		return "partial-application";

	const bool anyPending = after.classDesired || after.sidearmDesired ||
		after.primaryDesired || after.grenadeDesired || after.equipmentDesired;
	if (!anyPending)
		return "reset-or-corrected";
	return "pending";
}

// This is a deliberately inactive, server-command-owned fixture. It uses
// normal bots and normal Think/perception; it only owns temporary positioning
// and movement neutralization at the final command boundary.
enum class EmpiresEnemyLosFixturePhase
{
	INACTIVE,
	SETTLING,
	WAITING_FOR_ACQUISITION,
	WAITING_FOR_ROUTE,
	TRAVERSING,
	OBSERVING_LAST_KNOWN,
	COMBAT_OBSERVING
};

struct EmpiresEnemyLosFixtureState
{
	bool active = false;
	EmpiresEnemyLosFixturePhase phase = EmpiresEnemyLosFixturePhase::INACTIVE;
	int observerUserId = -1;
	int opponentUserId = -1;
	int observerIndex = -1;
	int opponentIndex = -1;
	const edict_t* observerEdict = nullptr;
	const edict_t* opponentEdict = nullptr;
	bool holdObserver = false;
	bool holdOpponent = false;
	bool routeRequested = false;
	bool initialVisibilityLogged = false;
	bool visibilityDiagnosticLogged = false;
	bool enemyAcquiredLogged = false;
	bool routeValidatedLogged = false;
	bool opponentMovedLogged = false;
	bool observerMovedLogged = false;
	bool observerCommandHoldLogged = false;
	bool opponentCommandHoldLogged = false;
	bool geometryBlockedLogged = false;
	bool botVisibilityLostLogged = false;
	bool opponentHeldAfterOcclusionLogged = false;
	bool requireNaturalExpiry = false;
	bool combatMode = false;
	bool releaseObserverForCombatMovement = false;
	Vector observerAnchor;
	Vector opponentAnchor;
	Vector observerLastOrigin;
	Vector opponentLastOrigin;
	bool observerLastOriginKnown = false;
	bool opponentLastOriginKnown = false;
	int settleStableFrames = 0;
	int lastProcessTick = -1;
	float deadline = 0.0f;
};

EmpiresEnemyLosFixtureState g_empireEnemyLosFixture;

// Development-only Scout target-priority fixture. It positions one normal
// Scout and two opposing normal players at a known visible waypoint corridor,
// then varies only which target's delivered UserCmd contains IN_ATTACK. This
// never writes enemy state or gameplay properties; normal visibility and
// CFindEnemyFunc selection remain the observed path.
struct EmpiresScoutPriorityFixtureState
{
	bool active = false;
	int scoutUserId = -1;
	int targetAUserId = -1;
	int targetBUserId = -1;
	int firingTargetUserId = -1;
	int scoutIndex = -1;
	int targetAIndex = -1;
	int targetBIndex = -1;
	const edict_t* scoutEdict = nullptr;
	const edict_t* targetAEdict = nullptr;
	const edict_t* targetBEdict = nullptr;
	Vector scoutAnchor;
	Vector targetAAnchor;
	Vector targetBAnchor;
	Vector lastScoutOrigin;
	Vector lastTargetAOrigin;
	Vector lastTargetBOrigin;
	bool lastOriginsKnown = false;
	bool commandHoldLogged = false;
	bool stableLogged = false;
	bool visibilityDiagnosticLogged = false;
	int stableFrames = 0;
	int lastProcessTick = -1;
	float deadline = 0.0f;
};

EmpiresScoutPriorityFixtureState g_empireScoutPriorityFixture;

// Production squad coordination is intentionally bounded to one shared
// waypoint goal per team/squad.  The game-owned membership tuple remains the
// authority; this state only coordinates normal fake clients after that tuple
// has been observed.  It is reset with the map lifecycle and never touches
// Commander or squad-power properties.
struct EmpiresSquadGoalState
{
	bool active = false;
	int team = -1;
	int squad = -1;
	int leaderUserId = -1;
	int waypoint = -1;
	Vector origin = vec3_origin;
	int generation = 0;
};

EmpiresSquadGoalState g_empireSquadGoals[2][64];

int empSquadTeamSlot(const int team)
{
	if (team == EMP_TEAM_NF)
		return 0;
	if (team == EMP_TEAM_BE)
		return 1;
	return -1;
}

EmpiresSquadGoalState* empSquadGoalState(const int team, const int squad)
{
	const int teamSlot = empSquadTeamSlot(team);
	if (teamSlot < 0 || squad <= 0 || squad >= 64)
		return nullptr;
	return &g_empireSquadGoals[teamSlot][squad];
}

void clearEmpiresSquadGoals()
{
	for (int teamSlot = 0; teamSlot < 2; ++teamSlot)
	{
		for (int squad = 0; squad < 64; ++squad)
			g_empireSquadGoals[teamSlot][squad] = EmpiresSquadGoalState();
	}
}

CBotEmpires* fixtureBotForUserId(const int userId)
{
	if (userId < 0)
		return nullptr;

	edict_t* pEdict = CBotGlobals::playerByUserId(userId);
	if (pEdict == nullptr || pEdict->IsFree())
		return nullptr;

	CBot* pBot = CBots::getBotPointer(pEdict);
	if (pBot == nullptr || pBot->isControlledTestOnly())
		return nullptr;

	return static_cast<CBotEmpires*>(pBot);
}

float fixtureDistance(const Vector& a, const Vector& b)
{
	return std::sqrt((a - b).LengthSqr());
}

bool fixtureActorValid(CBotEmpires* pBot, const int expectedUserId,
	const int expectedIndex, const edict_t* expectedEdict)
{
	if (pBot == nullptr || pBot->getEdict() == nullptr ||
		pBot->getController() == nullptr || engine == nullptr ||
		gpGlobals == nullptr)
		return false;

	edict_t* pEdict = pBot->getEdict();
	return pEdict == expectedEdict && !pEdict->IsFree() &&
		CBotGlobals::entityIsValid(pEdict) &&
		engine->IndexOfEdict(pEdict) == expectedIndex &&
		engine->GetPlayerUserId(pEdict) == expectedUserId &&
		pBot->isAlive();
}

void fixtureLog(const char* format, ...)
{
	if (logger == nullptr)
		return;

	char message[1024] = {};
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	logger->Log(LogLevel::INFO, "%s", message);
}

}

bool parseEmpiresBootstrapClass(const char* requested, int& classId, const char*& canonicalName)
{
	canonicalName = nullptr;
	classId = -1;

	for (const EmpiresClassSpec& spec : kEmpiresInfantryClasses)
	{
		if (equalsIgnoreCase(requested, spec.name))
		{
			classId = spec.id;
			canonicalName = spec.name;
			return true;
		}
	}

	if (!parseClassId(requested, classId))
		return false;

	canonicalName = kEmpiresInfantryClasses[classId].name;
	return true;
}

CBotEmpires::CBotEmpires()
{
	m_commanderMoveShadow.SetTelemetrySink(
		RecordCommanderMoveShadowTelemetry, this);
	for (int i = 0; i < RCBOT_MAXPLAYERS; ++i)
		m_iLastEmpiresPlayerObservation[i] =
			static_cast<int>(EmpiresPlayerObservation::UNKNOWN);
}

std::string CBotEmpires::getEmpiresCombatCorrelation(
	const edict_t* target)
{
	RCBotEmpiresCombatTelemetry::Correlation correlation;
	correlation.curtime = gpGlobals != nullptr ? gpGlobals->curtime : 0.0;
	correlation.lifeGeneration = getRecoveryLifeGeneration();
	correlation.captureGeneration = m_iEmpiresCaptureObjectiveGeneration;
	correlation.captureAttempt = m_iEmpiresCaptureRouteAttemptGeneration;
	correlation.ownerUserId = engine != nullptr && m_pEdict != nullptr ?
		engine->GetPlayerUserId(m_pEdict) : -1;
	correlation.ownerTeam = getTeam();
	correlation.capturePoint = m_iEmpiresCaptureApproachPoint;
	if (target != nullptr && engine != nullptr)
	{
		correlation.targetIndex = engine->IndexOfEdict(target);
		correlation.targetUserId = engine->GetPlayerUserId(target);
		IPlayerInfo* targetInfo = playerinfomanager != nullptr ?
			playerinfomanager->GetPlayerInfo(const_cast<edict_t*>(target)) :
			nullptr;
		correlation.targetTeam = targetInfo != nullptr ?
			targetInfo->GetTeamIndex() : -1;
	}
	return RCBotEmpiresCombatTelemetry::FormatCorrelation(correlation);
}

void CBotEmpires::setup()
{
	CBot::setup();
	m_hEmpiresTurretHitBy = nullptr;
	m_hEmpiresTurretFireTarget = nullptr;
	m_flEmpiresTurretRocketUntil = 0.0f;
	m_bEmpiresTurretRocketApproach = false;
	m_iEmpiresLastHealth = 0;
	m_commanderMoveShadow.Reset();
	m_commanderMoveShadow.SetTelemetrySink(
		RecordCommanderMoveShadowTelemetry, this);

	// The engine can reuse the same edict address after a fake client is
	// kicked.  setup() is called for every normal createBotFromEdict() call,
	// so clear the Empires-only test and lifecycle state before the new bot
	// enters suppressBotThink()/startGame().  Controlled probes do not call
	// setup(); they attach through their separate, explicit path.
	resetControlledTestState();
	m_pNormalTeamEdict = nullptr;
	m_iNormalLifecycleUserId = -1;
	m_bNormalStartGameEntryLogged = false;
	m_bNormalThinkObserverEntryLogged = false;
	m_eNormalPathState = EmpiresNormalPathState::IDLE;
	m_bNormalPathTerminalAfterStop = false;
	m_bNormalPathAutoStarted = false;
	m_bNormalDeathEventPending = false;
	m_NormalPathRoute.clear();
	m_eEmpiresVehicleActionState = EmpiresVehicleActionState::IDLE;
	m_iEmpiresVehicleActionExpectedIndex = -1;
	m_iEmpiresVehicleActionExpectedUserId = -1;
	m_pEmpiresVehicleActionExpectedEdict = nullptr;
	m_pEmpiresVehicleActionFactory = nullptr;
	m_pEmpiresVehicleActionTutorialExit = nullptr;
	m_iEmpiresVehicleActionTutorialExitIndex = -1;
	m_vEmpiresVehicleActionTutorialExitOrigin = vec3_origin;
	m_vEmpiresVehicleActionTutorialEntryOrigin = vec3_origin;
	m_flEmpiresVehicleActionTutorialTeleportDeadline = 0.0f;
	m_iEmpiresVehicleActionFactoryIndex = -1;
	m_iEmpiresVehicleActionFactoryTeam = -1;
	m_vEmpiresVehicleActionFactoryOrigin = vec3_origin;
	m_iEmpiresVehicleActionPurchaseStep = 0;
	m_flEmpiresVehicleActionNextPurchaseCommand = 0.0f;
	m_EmpiresVehicleActionBaselineVehicleIndices.clear();
	m_pEmpiresVehicleActionVehicle = nullptr;
	m_iEmpiresVehicleActionVehicleIndex = -1;
	m_iEmpiresVehicleActionVehicleTeam = -1;
	m_iEmpiresVehicleActionPassenger0 = -1;
	m_iEmpiresVehicleActionFrames = 0;
	m_flEmpiresVehicleActionDeadline = 0.0f;
	m_flEmpiresVehicleActionDriveStart = 0.0f;
	m_vEmpiresVehicleActionStartOrigin = vec3_origin;
	m_vEmpiresVehicleActionLastOrigin = vec3_origin;
	m_qEmpiresVehicleActionDriveAngles = vec3_angle;
	m_bEmpiresVehicleActionMoved = false;
	m_bEmpiresVehicleActionUseLogged = false;
	m_bEmpiresVehicleActionExitUseLogged = false;
	m_pEmpiresApproachTarget = nullptr;
	m_bEmpiresApproachTaskIssued = false;
	m_flEmpiresRelevanceIgnoreLogTime = 0.0f;
	m_pEmpiresLastSpotTarget = nullptr;
	m_flEmpiresNextSpotTime = 0.0f;
	m_bEmpiresLastEnemyObscured = false;
	m_bEmpiresLastEnemySearchActive = false;
	m_bEmpiresLastEnemySearchTaskLogged = false;
	m_iEmpiresSquadObserved = -1;
	m_iEmpiresSquadPositionObserved = -1;
	m_bEmpiresSquadLeaderObserved = false;
	m_bEmpiresSquadLeaderKnownObserved = false;
	m_flEmpiresNextSquadActionTime = 0.0f;
	m_iEmpiresSquadJoinAttempts = 0;
	m_bEmpiresSquadSpatialChildActive = false;
	m_EmpiresSquadSpatialParentOrder = RCBotOrder::OrderKey();
	m_EmpiresSquadSpatialChildExecution = RCBotOrder::OrderKey();
	m_iEmpiresSquadSpatialMemberId = 0;
	m_iEmpiresSquadSpatialRequestToken = 0;
	m_iEmpiresSquadSpatialNavigationIntentId = 0;
	m_iEmpiresSquadSpatialSlotId = 0;
	m_iEmpiresSquadSpatialGeneration = 0;
	m_iEmpiresSquadSpatialDecisionRevision = 0;
	m_iEmpiresSquadSpatialFormationGeneration = 0;
	m_iEmpiresSquadSpatialClaimKind = 0;
	m_iEmpiresSquadSpatialClaimId = 0;
	m_iEmpiresSquadSpatialClaimGeneration = 0;
	m_vEmpiresSquadSpatialTarget = vec3_origin;
	m_empireRecoveryEpisodeTracker.Reset();
	m_empireRecoveryEpisodePolicy.Reset();
	m_iEmpiresCaptureObjectiveGeneration = 0;
	m_iEmpiresCaptureRouteAttemptGeneration = 0;
	m_iEmpiresRecoveryCapturePoint = -1;
	m_iEmpiresRecoveryCaptureProgress = -1;
	m_iEmpiresRecoveryLastTeam = -1;
	m_iEmpiresRecoverySquadTeam = -1;
	m_iEmpiresRecoverySquad = -1;
	m_iEmpiresRecoverySquadGeneration = -1;
	m_iEmpiresRecoverySquadWaypoint = -1;
	resetEmpiresSquadGoalState();
	m_bEmpiresCombatEngaged = false;
	m_bEmpiresCoverScheduleActive = false;
	m_bEmpiresCaptureApproachIssued = false;
	m_bEmpiresCaptureHoldIssued = false;
	m_bEmpiresEngineerSupportTaskIssued = false;
	m_bEmpiresEngineerSupportNearby = false;
	m_bEmpiresEngineerResupplyTaskIssued = false;
	m_iEmpiresCaptureApproachPoint = -1;
	m_iEmpiresCaptureRouteWaypoint = -1;
	m_iEmpiresCaptureApproachRetries = 0;
	m_iEmpiresCaptureRouteAttemptGeneration = 0;
	m_captureInterruptionLatch.reset();
	clearEmpiresStartAnchorRecovery();
	m_flEmpiresCaptureRouteIssuedTime = -1.0f;
	m_iEmpiresCaptureRejectedPoint = -1;
	m_iEmpiresEngineerSupportBuildingIndex = -1;
	m_iEmpiresEngineerSupportWaypoint = -1;
	m_iEmpiresEngineerSupportRejectedBuildingIndex = -1;
	m_iEmpiresEngineerSupportRejectedWaypoint = -1;
	m_iEmpiresEngineerResupplyWaypoint = -1;
	m_vEmpiresCaptureApproachOrigin = vec3_origin;
	m_vEmpiresCaptureApproachStartOrigin = vec3_origin;
	m_vEmpiresEngineerSupportWaypointOrigin = vec3_origin;
	m_vEmpiresEngineerResupplyWaypointOrigin = vec3_origin;
	m_flEmpiresNextCaptureApproachTime = 0.0f;
	m_flEmpiresNextCaptureApproachProgressLogTime = 0.0f;
	m_flEmpiresCaptureHoldDeadline = 0.0f;
	m_flEmpiresCaptureRejectedUntil = 0.0f;
	m_flEmpiresNextEngineerSupportTime = 0.0f;
	m_flEmpiresEngineerSupportRejectedUntil = 0.0f;
	m_flEmpiresNextEngineerResupplyTime = 0.0f;
	m_flEmpiresNextLastKnownMineTime = 0.0f;
	m_flEmpiresNextVisibleMortarTime = 0.0f;
	m_flEmpiresReactionUntil = 0.0f;
	m_eEmpiresMortarActionState = EmpiresMortarActionState::IDLE;
	m_iEmpiresMortarActionTeam = -1;
	m_iEmpiresMortarActionExpectedIndex = -1;
	m_iEmpiresMortarActionExpectedUserId = -1;
	m_pEmpiresMortarActionExpectedEdict = nullptr;
	m_pEmpiresMortarActionWeapon = nullptr;
	m_iEmpiresMortarActionShellBaseline = 0;
	m_iEmpiresMortarActionShellIndex = -1;
	m_iEmpiresMortarActionAmmoBefore = -1;
	m_iEmpiresMortarActionAmmoAfter = -1;
	m_iEmpiresMortarActionFrames = 0;
	m_bEmpiresMortarActionPostureLogged = false;
	m_bEmpiresMortarActionAttackLogged = false;
	m_bEmpiresMortarActionReleaseLogged = false;
	m_flEmpiresMortarActionDeadline = 0.0f;
	m_flEmpiresMortarActionShellDeadline = 0.0f;
	m_eEmpiresEngineerRepairActionState = EmpiresEngineerRepairActionState::IDLE;
	m_iEmpiresEngineerRepairActionTeam = -1;
	m_iEmpiresEngineerRepairActionExpectedIndex = -1;
	m_iEmpiresEngineerRepairActionExpectedUserId = -1;
	m_pEmpiresEngineerRepairActionExpectedEdict = nullptr;
	m_pEmpiresEngineerRepairActionBuilding = nullptr;
	m_iEmpiresEngineerRepairActionBuildingIndex = -1;
	m_iEmpiresEngineerRepairActionHealthBefore = -1;
	m_iEmpiresEngineerRepairActionHealthLast = -1;
	m_iEmpiresEngineerRepairActionMaxHealth = -1;
	m_iEmpiresEngineerRepairActionFrames = 0;
	m_flEmpiresEngineerRepairActionDeadline = 0.0f;
	m_bEmpiresEngineerRepairActionRequireFullHealth = false;
	m_bEmpiresEngineerRepairActionProgressLogged = false;
	m_eEmpiresEngineerBuildSelectionState = EmpiresEngineerBuildSelectionState::IDLE;
	m_iEmpiresEngineerBuildSelectionTeam = -1;
	m_iEmpiresEngineerBuildSelectionExpectedIndex = -1;
	m_iEmpiresEngineerBuildSelectionExpectedUserId = -1;
	m_pEmpiresEngineerBuildSelectionExpectedEdict = nullptr;
	m_pEmpiresEngineerBuildSelectionWeapon = nullptr;
	m_iEmpiresEngineerBuildSelectionFrames = 0;
	m_iEmpiresEngineerBuildSelectionBaselineItem = -1;
	m_iEmpiresEngineerBuildSelectionLastItem = -1;
	m_iEmpiresEngineerBuildSelectionBaselineState = -1;
	m_iEmpiresEngineerBuildSelectionLastState = -1;
	m_bEmpiresEngineerBuildSelectionItemKnown = false;
	m_bEmpiresEngineerBuildSelectionStateKnown = false;
	m_bEmpiresEngineerBuildSelectionInputRequested = false;
	m_bEmpiresEngineerBuildSelectionInputSubmitted = false;
	m_bEmpiresEngineerBuildSelectionBaselineLogged = false;
	m_bEmpiresEngineerBuildSelectionTransitionLogged = false;
	m_flEmpiresEngineerBuildSelectionDeadline = 0.0f;
	m_eEmpiresEngineerBuildActionState = EmpiresEngineerBuildActionState::IDLE;
	m_iEmpiresEngineerBuildActionTeam = -1;
	m_iEmpiresEngineerBuildActionExpectedIndex = -1;
	m_iEmpiresEngineerBuildActionExpectedUserId = -1;
	m_pEmpiresEngineerBuildActionExpectedEdict = nullptr;
	m_iEmpiresEngineerBuildActionTargetWaypoint = -1;
	m_iEmpiresEngineerBuildActionBaselineTurrets = 0;
	m_iEmpiresEngineerBuildActionBaselineMaxIndex = -1;
	m_iEmpiresEngineerBuildActionResultIndex = -1;
	m_iEmpiresEngineerBuildActionFrames = 0;
	m_bEmpiresEngineerBuildActionCommandRequested = false;
	m_bEmpiresEngineerBuildActionCompletionCommandRequested = false;
	m_bEmpiresEngineerBuildActionAttackSubmitted = false;
	m_bEmpiresEngineerBuildActionBaselineLogged = false;
	m_bEmpiresEngineerBuildActionSelectionLogged = false;
	m_bEmpiresEngineerBuildActionResultObservedLogged = false;
	m_bEmpiresEngineerBuildActionRequireFullHealth = false;
	m_vEmpiresEngineerBuildActionTarget = vec3_origin;
	m_flEmpiresEngineerBuildActionDeadline = 0.0f;
	m_bEmpiresCommanderMoveTestActive = false;
	m_vEmpiresCommanderMoveTestTarget = vec3_origin;
	m_bEmpiresCommanderMoveRouteIssued = false;
	m_bEmpiresCommanderMoveFinished = false;
	m_bEmpiresCommanderMoveFailed = false;
	m_vEmpiresCommanderMoveTarget = vec3_origin;
	m_iEmpiresCommanderMoveNavigationIntentId = 0;
	m_iEmpiresCommanderMoveGeneration = 0;
	m_vEmpiresCommanderMovePreviousTarget = vec3_origin;
	m_flEmpiresCommanderMoveNextDiagnosticLog = 0.0f;
	m_flEmpiresCommanderMoveNextCommandDiagnosticLog = 0.0f;
	m_bEmpiresCommanderMoveDiagnosticCommandPending = false;
	m_iEmpiresCommanderMoveDiagnosticCommandNumber = -1;
	m_eEmpiresMineActionState = EmpiresMineActionState::IDLE;
	m_iEmpiresMineActionTeam = -1;
	m_iEmpiresMineActionExpectedIndex = -1;
	m_iEmpiresMineActionExpectedUserId = -1;
	m_pEmpiresMineActionExpectedEdict = nullptr;
	m_pEmpiresMineActionWeapon = nullptr;
	m_iEmpiresMineActionProjectileBaseline = 0;
	m_iEmpiresMineActionProjectileIndex = -1;
	m_iEmpiresMineActionAmmoBefore = -1;
	m_iEmpiresMineActionAmmoAfter = -1;
	m_iEmpiresMineActionFrames = 0;
	m_iEmpiresMineActionDeployObservations = 0;
	m_iEmpiresMineActionSettledFrames = 0;
	m_iEmpiresMineActionTriggerTargetUserId = -1;
	m_iEmpiresMineActionTriggerTargetIndex = -1;
	m_iEmpiresMineActionTriggerTargetHealthBefore = -1;
	m_iEmpiresMineActionTriggerTargetHealthAfter = -1;
	m_pEmpiresMineActionTriggerTargetEdict = nullptr;
	m_bEmpiresMineActionTriggerMode = false;
	m_flEmpiresMineActionTriggerDelaySeconds = 0.0f;
	m_bEmpiresMineActionTriggerPositionLogged = false;
	m_bEmpiresMineActionTriggerHoldLogged = false;
	m_bEmpiresMineActionTriggerImpactLogged = false;
	m_bEmpiresMineActionTriggerNaturalDeathLogged = false;
	m_bEmpiresMineActionAttackLogged = false;
	m_bEmpiresMineActionReleaseLogged = false;
	m_bEmpiresMineActionDeployLogged = false;
	m_iEmpiresMineActionActivationLast = -1;
	m_bEmpiresMineActionActivationUnavailableLogged = false;
	m_bEmpiresMineActionActivationLogged = false;
	m_flEmpiresMineActionDeployedTime = 0.0f;
	m_flEmpiresMineActionDeadline = 0.0f;
	m_flEmpiresMineActionProjectileDeadline = 0.0f;
	m_eEmpiresScoutBinocularActionState =
		EmpiresScoutBinocularActionState::IDLE;
	m_iEmpiresScoutBinocularActionExpectedIndex = -1;
	m_iEmpiresScoutBinocularActionExpectedUserId = -1;
	m_pEmpiresScoutBinocularActionExpectedEdict = nullptr;
	m_pEmpiresScoutBinocularActionTargetEdict = nullptr;
	m_iEmpiresScoutBinocularActionTargetIndex = -1;
	m_iEmpiresScoutBinocularActionTargetUserId = -1;
	m_iEmpiresScoutBinocularActionTargetTeam = -1;
	m_bEmpiresScoutBinocularActionTargetBuilding = false;
	m_pEmpiresScoutBinocularActionWeapon = nullptr;
	m_iEmpiresScoutBinocularActionFrames = 0;
	m_flEmpiresScoutBinocularActionDeadline = 0.0f;
	m_flEmpiresScoutBinocularActionAimUntil = 0.0f;
	m_bEmpiresScoutBinocularActionAttackLogged = false;
	m_bEmpiresScoutBinocularActionReleaseLogged = false;
	m_bEmpiresScoutBinocularActionPropertyBaselineLogged = false;
	m_bEmpiresScoutBinocularActionSpotTransitionLogged = false;
	m_iEmpiresScoutBinocularActionBaselineSpotter = -1;
	m_bEmpiresScoutBinocularActionBaselineSpotterKnown = false;
	m_bEmpiresScoutBinocularActionBaselineSpotted = false;
	m_bEmpiresScoutBinocularActionBaselineSpottedKnown = false;
	m_iEmpiresScoutBinocularActionBaselineVisibilityState = -1;
	m_bEmpiresScoutBinocularActionBaselineVisibilityKnown = false;
	m_bEmpiresMortarAimRequested = false;
	m_iEmpiresMortarAimTargetWaypoint = -1;
	m_vEmpiresMortarAimTarget = Vector(0.0f, 0.0f, 0.0f);
	m_qEmpiresMortarAimAngles = QAngle(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLaunchOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_bEmpiresMortarAimComputed = false;
	m_bEmpiresMortarAimViewApplied = false;
	m_bEmpiresMortarAimProcessedLogged = false;
	m_iEmpiresMortarAimTrajectorySamples = 0;
	m_flEmpiresMortarAimLastSampleTime = 0.0f;
	m_vEmpiresMortarAimFirstShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLastShellOrigin = Vector(0.0f, 0.0f, 0.0f);
}

int CBotEmpires::classifyEmpiresPlayer(edict_t* pEdict) const
{
	if (pEdict == nullptr || pEdict->IsFree())
		return static_cast<int>(EmpiresPlayerObservation::EMPTY);

	if (gpGlobals == nullptr || playerinfomanager == nullptr)
		return static_cast<int>(EmpiresPlayerObservation::UNKNOWN);

	const int index = ENTINDEX(pEdict);
	if (index <= 0 || index > gpGlobals->maxClients ||
		!CBotGlobals::entityIsValid(pEdict))
		return static_cast<int>(EmpiresPlayerObservation::UNKNOWN);

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(pEdict);
	if (pInfo == nullptr || !pInfo->IsConnected() || !pInfo->IsPlayer() ||
		pInfo->IsHLTV())
		return static_cast<int>(EmpiresPlayerObservation::EMPTY);

	if (pEdict == m_pEdict)
		return static_cast<int>(EmpiresPlayerObservation::SELF);

	const int team = pInfo->GetTeamIndex();
	if (pInfo->IsObserver() || team == 1)
		return static_cast<int>(EmpiresPlayerObservation::SPECTATOR);
	if (team == 0)
		return static_cast<int>(EmpiresPlayerObservation::UNASSIGNED);
	if (pInfo->IsDead() || pInfo->GetHealth() <= 0)
		return static_cast<int>(EmpiresPlayerObservation::DEAD);

	const int ownTeam = m_pPlayerInfo != nullptr ?
		m_pPlayerInfo->GetTeamIndex() : -1;
	if (ownTeam != EMP_TEAM_NF && ownTeam != EMP_TEAM_BE)
		return static_cast<int>(EmpiresPlayerObservation::UNKNOWN);

	if (team == ownTeam)
		return static_cast<int>(EmpiresPlayerObservation::FRIENDLY);
	if ((team == EMP_TEAM_NF || team == EMP_TEAM_BE) &&
		(team != ownTeam))
		return static_cast<int>(EmpiresPlayerObservation::ENEMY);

	return static_cast<int>(EmpiresPlayerObservation::UNKNOWN);
}

bool CBotEmpires::isEnemy(edict_t* pEdict, const bool bCheckWeapons)
{
	// Empires has two playable teams. The old donor also rejected self and
	// non-player/dead entities before its team comparison. Keep that behavior,
	// but obtain all state from current SDK2013 IPlayerInfo instead of old
	// class-name, offset, signature or GameRules assumptions.
	(void)bCheckWeapons;
	return classifyEmpiresPlayer(pEdict) ==
		static_cast<int>(EmpiresPlayerObservation::ENEMY);
}

float CBotEmpires::getEnemyFactor(edict_t* pEnemy)
{
	// Keep distance as the primary factor, then add small, observable priorities
	// that are available through the public SDK2013 API.  CFindEnemyFunc still
	// performs the actual selection over the existing visible set; this method
	// only makes the role-specific preference explicit.
	float factor = CBot::getEnemyFactor(pEnemy);
	if (pEnemy == nullptr || playerinfomanager == nullptr)
		return factor;

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(pEnemy);
	if (pInfo == nullptr || !pInfo->IsConnected() || !pInfo->IsPlayer())
		return factor + 100000.0f;

	if (!pInfo->IsFakeClient())
		factor -= 128.0f;

	const bool enemyPressingAttack =
		(pInfo->GetLastUserCommand().buttons & IN_ATTACK) != 0;
	if (getDesiredClass() == EMP_CLASS_SCOUT)
	{
		// A Scout's first useful role distinction is reconnaissance: among
		// otherwise comparable visible players, prefer the quieter target so the
		// public spot command can report a flanker or observer instead of merely
		// duplicating the shared combat urgency.  The existing distance and
		// real-player terms remain in force, and an actively firing enemy is
		// still a valid target when it is materially closer.
		factor += enemyPressingAttack ? 256.0f : -256.0f;
	}
	else if (enemyPressingAttack)
	{
		factor -= 512.0f;
	}

	return factor;
}

void CBotEmpires::enemyFound(edict_t* pEnemy)
{
	CBot::enemyFound(pEnemy);
	m_flEmpiresReactionUntil = engine != nullptr ?
		engine->Time() + randomFloat(0.20f, 0.55f) : 0.0f;

	if (logger == nullptr || pEnemy == nullptr || playerinfomanager == nullptr)
		return;

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(pEnemy);
	const CBotCmd lastCommand = pInfo != nullptr ?
		pInfo->GetLastUserCommand() : CBotCmd();
	const bool scoutReconPriority =
		getDesiredClass() == EMP_CLASS_SCOUT && pInfo != nullptr &&
		(lastCommand.buttons & IN_ATTACK) == 0;
	logger->Log(LogLevel::INFO,
		"Empires gameplay target=selected index=%d userid=%d team=%d fake=%d distance=%.1f attacking=%d scout_recon_priority=%d policy=%s reaction=%.2f",
		engine->IndexOfEdict(pEnemy), engine->GetPlayerUserId(pEnemy),
		pInfo != nullptr ? pInfo->GetTeamIndex() : -1,
		pInfo != nullptr && pInfo->IsFakeClient() ? 1 : 0,
		distanceFrom(pEnemy), (lastCommand.buttons & IN_ATTACK) != 0 ? 1 : 0,
		scoutReconPriority ? 1 : 0,
		getDesiredClass() == EMP_CLASS_SCOUT ?
			(scoutReconPriority ? "scout-quiet-target" : "scout-combat-target") :
			"shared-combat-priority",
		std::max(0.0f, m_flEmpiresReactionUntil - engine->Time()));

	// A bot only sees its own visibility set.  Share this one verified sighting
	// with active Empires teammates through the existing last-enemy memory so a
	// teammate can approach the known position without inventing a second
	// perception or navigation system.
	const int sourceIndex = engine->IndexOfEdict(m_pEdict);
	const int sourceTeam = getTeam();
	for (std::size_t i = 0; i < static_cast<std::size_t>(RCBOT_MAXPLAYERS); ++i)
	{
		CBot* pCandidate = CBots::get(i);
		if (pCandidate == nullptr || pCandidate == this || !pCandidate->inUse() ||
			pCandidate->isControlledTestOnly() || pCandidate->getEdict() == nullptr)
			continue;

		CBotEmpires* pTeammate = static_cast<CBotEmpires*>(pCandidate);
		if (pTeammate->getTeam() == sourceTeam)
			pTeammate->receiveEmpiresSpottedEnemy(pEnemy, sourceIndex);
	}
}

void CBotEmpires::receiveEmpiresSpottedEnemy(edict_t* pEnemy, const int sourceIndex)
{
	if (pEnemy == nullptr || pEnemy == m_pEdict || m_pPlayerInfo == nullptr ||
		isControlledTestOnly() || m_bEmpiresCoverScheduleActive ||
		m_bEmpiresLastEnemySearchActive ||
		(m_pEnemy != nullptr && hasSomeConditions(CONDITION_SEE_CUR_ENEMY)))
		return;

	if (!CBotGlobals::entityIsAlive(pEnemy) || !isEnemy(pEnemy, false))
		return;

	setLastEnemy(pEnemy);
	m_bLookedForEnemyLast = false;
	logger->Log(LogLevel::INFO,
		"Empires gameplay team-alert=accepted source_index=%d target_index=%d target_userid=%d target_team=%d response=last-known-approach",
		sourceIndex,
		engine != nullptr ? engine->IndexOfEdict(pEnemy) : -1,
		engine != nullptr ? engine->GetPlayerUserId(pEnemy) : -1,
		playerinfomanager != nullptr && playerinfomanager->GetPlayerInfo(pEnemy) != nullptr ?
		playerinfomanager->GetPlayerInfo(pEnemy)->GetTeamIndex() : -1);
}

void CBotEmpires::maybeSpotEmpiresEnemy()
{
	// Scout spotting is deliberately a small role action layered on the
	// existing Empires quick-command -> last-enemy-memory path. It does not
	// alter perception, target selection, combat, or navigation.
	if (getDesiredClass() != EMP_CLASS_SCOUT || m_pEdict == nullptr ||
		m_pPlayerInfo == nullptr || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0 || helpers == nullptr || engine == nullptr)
		return;

	edict_t* pEnemy = m_pEnemy.get();
	if (pEnemy == nullptr || !hasSomeConditions(CONDITION_SEE_CUR_ENEMY) ||
		!CBotGlobals::entityIsValid(pEnemy) ||
		!CBotGlobals::entityIsAlive(pEnemy) || !isEnemy(pEnemy, false) ||
		!isVisible(pEnemy))
		return;

	const float now = engine->Time();
	if (m_pEmpiresLastSpotTarget.get() == pEnemy && now < m_flEmpiresNextSpotTime)
		return;

	// Spot only what matters, like a human: not every enemy in sight. Cheap checks
	// first; one spot per team every 20 s; nothing already spotted; and only an
	// enemy in a vehicle or one close to our flags or buildings.
	static float s_nextTeamSpot[4] = {};
	const int team = getTeam();
	if (team < 0 || team >= 4 || now < s_nextTeamSpot[team])
		return;
	bool alreadySpotted = false;
	if (readEmpiresEntityBoolProperty(pEnemy, "m_bSpotted", alreadySpotted) && alreadySpotted)
		return;
	const Vector enemyOrigin = CBotGlobals::entityOrigin(pEnemy);
	constexpr float kSpotNearOwn = 1500.0f;
	const char* reason = nullptr;
	int vehicle = -1;
	// Weapon-handle read returns the entity index; 4095 or less than 1 means on foot.
	if (readEmpiresWeaponHandleProperty(pEnemy, "m_hVehicle", vehicle) && vehicle > 0 && vehicle < 4095)
		reason = "vehicle";
	const CBotMod* pMod = CBotGlobals::getCurrentMod();
	if (reason == nullptr && pMod != nullptr && pMod->getBotType() == BOTTYPE_EMPIRES)
	{
		for (const CEmpiresMod::CapturePointObservation& point :
			static_cast<const CEmpiresMod*>(pMod)->getCapturePoints())
		{
			if (point.originKnown && point.teamOwnerKnown && point.teamOwner == team &&
				(point.origin - enemyOrigin).Length() < kSpotNearOwn)
			{
				reason = "near-own-flag";
				break;
			}
		}
	}
	if (reason == nullptr && gpGlobals != nullptr)
	{
		for (int index = gpGlobals->maxClients + 1; index < gpGlobals->maxEntities && reason == nullptr; ++index)
		{
			edict_t* pBuilding = INDEXENT(index);
			if (pBuilding == nullptr || pBuilding->IsFree() ||
				!isEmpiresRepairableBuildingClass(pBuilding->GetClassName(), team))
				continue;
			const int buildingTeam = CClassInterface::getTeam(pBuilding);
			if (buildingTeam > 0 && buildingTeam != team)
				continue;
			if ((CBotGlobals::entityOrigin(pBuilding) - enemyOrigin).Length() < kSpotNearOwn)
				reason = "near-own-building";
		}
	}
	if (reason == nullptr)
		return;
	s_nextTeamSpot[team] = now + 20.0f;

	// The existing command handler resolves the target from the source's last
	// user-command view. Keep normal look scheduling intact and issue only the
	// public Empires quick command; no enemy-memory fields are injected here.
	setLookAtTask(LOOK_ENEMY);
	helpers->ClientCommand(m_pEdict, "emp_menu_quickcmd 2 0\n");
	m_pEmpiresLastSpotTarget = pEnemy;
	m_flEmpiresNextSpotTime = now + 10.0f;
	EmpiresAnalysis::RecordBotEvent(this, "SCOUT_SPOT", pEnemy,
		"existing-quick-command;cooldown=10.0");

	logger->Log(LogLevel::INFO,
		"Empires gameplay scout-spot=requested source_index=%d source_userid=%d target_index=%d target_userid=%d reason=%s command=emp_menu_quickcmd 2 0 cooldown=10.0 team_cooldown=20.0",
		engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
		engine->IndexOfEdict(pEnemy), engine->GetPlayerUserId(pEnemy), reason);
}

void CBotEmpires::observeEmpiresPlayerConditions()
{
	if (gpGlobals == nullptr || playerinfomanager == nullptr || engine == nullptr)
		return;

	int maxClients = gpGlobals->maxClients;
	const int configuredMaxClients = CBotGlobals::maxClients();
	if (configuredMaxClients > 0 && configuredMaxClients < maxClients)
		maxClients = configuredMaxClients;
	if (maxClients >= RCBOT_MAXPLAYERS)
		maxClients = RCBOT_MAXPLAYERS - 1;

	for (int index = 1; index <= maxClients; ++index)
	{
		edict_t* pEdict = INDEXENT(index);
		IPlayerInfo* pInfo = nullptr;
		if (pEdict != nullptr && !pEdict->IsFree())
			pInfo = playerinfomanager->GetPlayerInfo(pEdict);

		const int observation = classifyEmpiresPlayer(pEdict);
		const int userId = pInfo != nullptr && pInfo->IsConnected() ?
			pInfo->GetUserID() : -1;
		const bool identityChanged =
			m_pLastEmpiresPlayerEdict[index] != pEdict ||
			m_iLastEmpiresPlayerUserId[index] != userId;
		const int previousObservation = m_iLastEmpiresPlayerObservation[index];
		const bool wasObserved = previousObservation !=
			static_cast<int>(EmpiresPlayerObservation::UNKNOWN);
		const bool shouldLog =
			(observation != static_cast<int>(EmpiresPlayerObservation::EMPTY) &&
				observation != static_cast<int>(EmpiresPlayerObservation::UNKNOWN) &&
				(!wasObserved || observation != previousObservation || identityChanged)) ||
			(wasObserved && observation != previousObservation);

		if (shouldLog)
		{
			const bool hasPlayerInfo = pInfo != nullptr && pInfo->IsConnected() &&
				pInfo->IsPlayer() && !pInfo->IsHLTV();
			const int team = hasPlayerInfo ? pInfo->GetTeamIndex() : -1;
			const int health = hasPlayerInfo ? pInfo->GetHealth() : -1;
			const bool dead = hasPlayerInfo ? pInfo->IsDead() : true;
			const bool observer = hasPlayerInfo ? pInfo->IsObserver() : false;
			const char* name = hasPlayerInfo && pInfo->GetName() != nullptr ?
				pInfo->GetName() : "<none>";
			const bool enemy = isEnemy(pEdict, false);
			const bool visible = pEdict != nullptr && pEdict != m_pEdict &&
				m_pVisibles != nullptr && isVisible(pEdict);

			logger->Log(LogLevel::INFO,
				"Empires conditions player index=%d userid=%d name=\"%s\" team=%d state=%s is_enemy=%d alive=%d health=%d observer=%d fake=%d visible=%d",
				index, userId, name, team,
				empiresPlayerObservationName(observation), enemy ? 1 : 0,
				(hasPlayerInfo && !dead && health > 0) ? 1 : 0, health,
				observer ? 1 : 0,
				(hasPlayerInfo && pInfo->IsFakeClient()) ? 1 : 0,
				visible ? 1 : 0);
		}

		m_iLastEmpiresPlayerObservation[index] = observation;
		m_pLastEmpiresPlayerEdict[index] = pEdict;
		m_iLastEmpiresPlayerUserId[index] = userId;
	}
}

void CBotEmpires::updateConditions()
{
	// Preserve the generic RCBot2 condition machinery. It clears stale/dead
	// enemies and updates visibility conditions after findEnemy() has used this
	// class's isEnemy() filter.
	const bool wasEnemyObscured = m_bEmpiresLastEnemyObscured;
	CBot::updateConditions();

	// getTasks() runs before findEnemy() in the generic Think loop.  A normal
	// Engineer resupply action can therefore still own IN_USE for one complete
	// frame when the new visible enemy is acquired.  Release that normal input
	// action here, after the authoritative visibility update, so combat can take
	// over on the next Think instead of waiting for the resupply timeout.
	const bool validVisibleEnemyForResupplyInterrupt =
		m_pEnemy != nullptr &&
		hasSomeConditions(CONDITION_SEE_CUR_ENEMY) &&
		CBotGlobals::entityIsValid(m_pEnemy) &&
		CBotGlobals::entityIsAlive(m_pEnemy) &&
		isEnemy(m_pEnemy, false) && isVisible(m_pEnemy);
	if (validVisibleEnemyForResupplyInterrupt)
		interruptEmpiresEngineerResupplyForCombat("enemy-acquired");

	// CBot::think calls getTasks() before findEnemy() and updateConditions().
	// On the first lost-LOS frame findEnemy() has already cleared m_pEnemy, so
	// the generic obscured-enemy branch cannot record the target.  The active
	// Empires approach target is still represented by m_pOldEnemy; feed that
	// already-acquired target through the existing generic last-enemy storage.
	edict_t* pPreviouslyVisibleEnemy = m_pOldEnemy.get();
	if (m_pEnemy == nullptr && m_bEmpiresApproachTaskIssued &&
		pPreviouslyVisibleEnemy != nullptr &&
		!isVisible(pPreviouslyVisibleEnemy) &&
		CBotGlobals::entityIsValid(pPreviouslyVisibleEnemy) &&
		CBotGlobals::entityIsAlive(pPreviouslyVisibleEnemy) &&
		isEnemy(pPreviouslyVisibleEnemy, false))
	{
		if (m_pLastEnemy != pPreviouslyVisibleEnemy || m_fLastSeeEnemy <= 0.0f)
			setLastEnemy(pPreviouslyVisibleEnemy);

		removeCondition(CONDITION_SEE_CUR_ENEMY);
		removeCondition(CONDITION_SEE_ENEMY_HEAD);
		updateCondition(CONDITION_ENEMY_OBSCURED);
	}

	observeEmpiresPlayerConditions();

	// Keep the normal generic condition set aligned with the Empires-specific
	// resupply selector.  The selector still owns the route/action decision;
	// this only exposes the already-proven out-of-ammo fact to existing RCBot2
	// scheduling/diagnostic consumers.
	if (isEmpiresEngineerResupplyNeeded())
		updateCondition(CONDITION_NEED_AMMO);
	else
		removeCondition(CONDITION_NEED_AMMO);

	const bool enemyObscured =
		hasSomeConditions(CONDITION_ENEMY_OBSCURED) &&
		m_pLastEnemy != nullptr &&
		m_fLastSeeEnemy > 0.0f &&
		CBotGlobals::entityIsValid(m_pLastEnemy) &&
		CBotGlobals::entityIsAlive(m_pLastEnemy) &&
		isEnemy(m_pLastEnemy, false);

	if (!wasEnemyObscured && enemyObscured)
	{
		logger->Log(LogLevel::INFO,
			"Empires gameplay threat=enemy los=lost last_known_recorded=1 target_index=%d target_userid=%d last_known=(%.1f %.1f %.1f)",
			engine != nullptr ? engine->IndexOfEdict(m_pLastEnemy) : -1,
			engine != nullptr ? engine->GetPlayerUserId(m_pLastEnemy) : -1,
			m_vLastSeeEnemy.x, m_vLastSeeEnemy.y, m_vLastSeeEnemy.z);
	}
	else if (wasEnemyObscured && !enemyObscured &&
		hasSomeConditions(CONDITION_SEE_CUR_ENEMY))
	{
		logger->Log(LogLevel::INFO,
			"Empires gameplay threat=enemy reacquired=1 los=visible");
	}

	m_bEmpiresLastEnemyObscured = enemyObscured;
}

CBotEmpires::~CBotEmpires()
{
	clearEmpiresEnemyLosFixture("bot-destructor");
	clearEmpiresScoutPriorityFixture("bot-destructor");
}

bool CBotEmpires::hurt(edict_t* pAttacker, const int iHealthNow,
	const bool bDontHide)
{
	// A Commander MOVE is authoritative even when damage would normally make
	// the generic bot schedule a hide task.  Keep the normal damage bookkeeping
	// but suppress the autonomous cover interruption until the order completes.
	if (isEmpiresCommanderMoveActive())
	{
		(void)CBot::hurt(pAttacker, iHealthNow, true);
		return false;
	}

	noteEmpiresTurretThreat(pAttacker);
	const bool hideScheduled = CBot::hurt(pAttacker, iHealthNow, bDontHide);
	// Engineer support is interruptible by real damage. Release the kit and
	// route immediately; the next normal think cycle can reacquire the attacker
	// and use the ordinary combat path.
	interruptEmpiresEngineerSupportForCombat("damage-interrupt");
	if (!hideScheduled || bDontHide || m_pSchedules == nullptr)
		return hideScheduled;

	CBotSchedule* pHideSchedule = m_pSchedules->getCurrentSchedule();
	if (pHideSchedule == nullptr || !pHideSchedule->isID(SCHED_GOOD_HIDE_SPOT))
	{
		logger->Log(LogLevel::WARN,
			"Empires gameplay cover=not-extended reason=hide-schedule-not-current");
		return hideScheduled;
	}

	// The generic RCBot2 hurt path already chooses a hide spot and routes to it.
	// Empires only extends that existing schedule with one bounded wait task;
	// this is deliberately not a second cover or navigation implementation.
	const float waitSeconds = randomFloat(4.0f, 7.0f);
	pHideSchedule->addTask(new CBotWaitTask(waitSeconds));
	m_bEmpiresCoverScheduleActive = true;
	logger->Log(LogLevel::INFO,
		"Empires gameplay cover=started wait=%.1f reason=damage-threshold route=existing-CGotoHideSpotSched",
		waitSeconds);
	return hideScheduled;
}

void CBotEmpires::handleWeapons()
{
	// Commander movement owns the bot's objective and input boundary.  Do not
	// let combat, support or diagnostic action handlers replace it.
	if (isEmpiresCommanderMoveActive())
	{
		logEmpiresCommanderMoveDiagnostic("handleWeapons-commander", nullptr, false);
		return;
	}

	// The bounded vehicle proof owns movement and interaction input after the
	// route reaches the vehicle. Do not let generic combat select a weapon or
	// overwrite the drive/entry/exit command window.
	tickEmpiresGuardOrder();
	tickEmpiresBuildAssignment();
	tickCommanderEconomy();

	if (commanderVehicleEntryActive())
	{
		handleCommanderVehicleEntry();
		return;
	}

	if (isEmpiresVehicleActionActive())
	{
		handleEmpiresVehicleAction();
		return;
	}

	// The mortar smoke probe owns only its temporary input window.  Keeping
	// this guard here prevents the ordinary target-driven weapon path from
	// cancelling the probe's one attack pulse or selecting another weapon.
	if (isEmpiresMortarProbeActive())
		return;

	// Grenadier actions own the weapon until the shot has been observed.
	if (m_flEmpiresTurretRocketUntil > 0.0f)
	{
		tickEmpiresTurretRocket();
		return;
	}
	if (isEmpiresMortarActionActive())
	{
		handleEmpiresMortarAction();
		return;
	}

	// The binocular proof is explicit and one-shot.  It must not participate
	// in generic weapon scoring or allow the ordinary combat handler to replace
	// its selected weapon or attack pulse.
	if (isEmpiresScoutBinocularActionActive())
	{
		handleEmpiresScoutBinocularAction();
		return;
	}

	// Mine firing is available either through the explicit bounded diagnostic
	// command or the narrow last-known-area Grenadier action. Recognition stays
	// outside generic weapon scoring; both callers use this same state machine.
	if (isEmpiresMineActionActive())
	{
		handleEmpiresMineAction();
		return;
	}

	// Engineer repair owns the input while a normal support pulse is active.
	// Nearby support is selected by getTasks(); explicit repair commands use the
	// same normal kit action for bounded diagnostics.
	if (isEmpiresEngineerRepairActionActive())
	{
		handleEmpiresEngineerRepairAction();
		return;
	}

	// Engineer resupply is currently an explicit bounded proof only. It owns
	// the input while it selects a normal weapon and uses a real team crate.
	if (isEmpiresEngineerResupplyActionActive())
	{
		handleEmpiresEngineerResupplyAction();
		return;
	}

	// Engineer build-item selection is a bounded diagnostic only. It owns one
	// normal secondary-input pulse and must not fall through to generic combat
	// or weapon scoring while the result is being observed.
	if (isEmpiresEngineerBuildSelectionActive())
	{
		handleEmpiresEngineerBuildSelection();
		return;
	}

	// Engineer construction is currently a bounded diagnostic only. It owns
	// the normal kit/input path while a real building result is observed.
	if (isEmpiresEngineerBuildActionActive())
	{
		handleEmpiresEngineerBuildAction();
		return;
	}

	const bool coverScheduleActive =
		m_bEmpiresCoverScheduleActive &&
		m_pSchedules != nullptr &&
		m_pSchedules->isCurrentSchedule(SCHED_GOOD_HIDE_SPOT);
	if (coverScheduleActive)
	{
		if (m_bEmpiresCombatEngaged || (m_iButtons & IN_ATTACK) != 0)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay cover=holding-fire movement=existing-hide-route");
		}

		if (m_pButtons != nullptr)
			m_pButtons->letGo(IN_ATTACK);
		m_iButtons &= ~IN_ATTACK;
		wantToShoot(false);
		return;
	}

	edict_t* pEnemy = m_pEnemy.get();
	const bool validTarget =
		pEnemy != nullptr &&
		!hasSomeConditions(CONDITION_ENEMY_DEAD) &&
		hasSomeConditions(CONDITION_SEE_CUR_ENEMY) &&
		CBotGlobals::entityIsValid(pEnemy) &&
		CBotGlobals::entityIsAlive(pEnemy) &&
		isEnemy(pEnemy, false) &&
		isVisible(pEnemy) &&
		m_pWeapons != nullptr;

	if (!validTarget)
	{
		if (m_bEmpiresCombatEngaged || (m_iButtons & IN_ATTACK) != 0)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay combat=cleared reason=target-invalid-dead-out-of-sight-or-range");
		}

		if (m_pButtons != nullptr)
			m_pButtons->letGo(IN_ATTACK);

		m_iButtons &= ~IN_ATTACK;
		m_bEmpiresCombatEngaged = false;
		return;
	}

	CBotWeapon* pWeapon = getBestWeapon(pEnemy);
	if (pWeapon == nullptr)
	{
		// Do not fall back to generic primaryAttack() when the target is not
		// covered by an Empires weapon definition.  The active approach task
		// keeps moving through the normal input path until a valid weapon is
		// available or the target is lost.
		if (m_bEmpiresCombatEngaged)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay combat=cleared reason=no-compatible-weapon-in-range movement_continues=1");
		}

		if (m_pButtons != nullptr)
			m_pButtons->letGo(IN_ATTACK);

		m_iButtons &= ~IN_ATTACK;
		m_bEmpiresCombatEngaged = false;
		wantToShoot(false);
		setLookAtTask(LOOK_ENEMY);
		return;
	}

	if (engine != nullptr && engine->Time() < m_flEmpiresReactionUntil)
	{
		if (m_pButtons != nullptr)
			m_pButtons->letGo(IN_ATTACK);
		m_iButtons &= ~IN_ATTACK;
		wantToShoot(false);
		setLookAtTask(LOOK_ENEMY);
		return;
	}

	if (!m_bEmpiresCombatEngaged)
	{
		logger->Log(LogLevel::INFO,
			"Empires gameplay combat=engaged target_index=%d target_userid=%d distance=%.1f weapon=%s weapon_max_range=%.1f movement_continues=1 input=generic_handleWeapons",
			engine != nullptr ? engine->IndexOfEdict(pEnemy) : -1,
			engine != nullptr ? engine->GetPlayerUserId(pEnemy) : -1,
			distanceFrom(pEnemy),
			pWeapon->getWeaponInfo() != nullptr ? pWeapon->getWeaponInfo()->getWeaponName() : "unknown",
			pWeapon->getPrimaryMaxRange());
		m_bEmpiresCombatEngaged = true;
	}

	wantToShoot(true);
	CBot::handleWeapons();
}

bool CBotEmpires::isEmpiresScoutBinocularActionActive() const
{
	return m_eEmpiresScoutBinocularActionState !=
		EmpiresScoutBinocularActionState::IDLE &&
		m_eEmpiresScoutBinocularActionState !=
		EmpiresScoutBinocularActionState::COMPLETE &&
		m_eEmpiresScoutBinocularActionState !=
		EmpiresScoutBinocularActionState::FAILED;
}

void CBotEmpires::failEmpiresScoutBinocularAction(const char* reason)
{
	if (!isEmpiresScoutBinocularActionActive())
		return;

	if (m_pButtons != nullptr)
	{
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_USE);
		m_pButtons->letGo(IN_FORWARD);
		m_pButtons->letGo(IN_BACK);
		m_pButtons->letGo(IN_MOVELEFT);
		m_pButtons->letGo(IN_MOVERIGHT);
	}
	m_iButtons = 0;
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	setLookAtTask(LOOK_NONE);
	if (logger != nullptr)
	{
		logger->Log(LogLevel::WARN,
			"Empires scout binocular action=result=FAIL index=%d userid=%d target_index=%d target_userid=%d frames=%d reason=%s",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			m_iEmpiresScoutBinocularActionTargetIndex,
			m_iEmpiresScoutBinocularActionTargetUserId,
			m_iEmpiresScoutBinocularActionFrames,
			reason != nullptr ? reason : "unspecified");
	}
	m_eEmpiresScoutBinocularActionState =
		EmpiresScoutBinocularActionState::FAILED;
}

void CBotEmpires::handleEmpiresScoutBinocularAction()
{
	if (!isEmpiresScoutBinocularActionActive())
		return;

	if (engine == nullptr || gpGlobals == nullptr || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		m_pController == nullptr || m_pButtons == nullptr)
	{
		failEmpiresScoutBinocularAction("runtime-prerequisite-invalid");
		return;
	}

	++m_iEmpiresScoutBinocularActionFrames;
	const float now = gpGlobals->curtime;
	const int index = engine->IndexOfEdict(m_pEdict);
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if (index != m_iEmpiresScoutBinocularActionExpectedIndex ||
		userId != m_iEmpiresScoutBinocularActionExpectedUserId ||
		m_pEdict != m_pEmpiresScoutBinocularActionExpectedEdict)
	{
		failEmpiresScoutBinocularAction("client-identity-changed");
		return;
	}

	edict_t* pTarget = const_cast<edict_t*>(
		m_pEmpiresScoutBinocularActionTargetEdict);
	const bool targetBuilding = m_bEmpiresScoutBinocularActionTargetBuilding;
	IPlayerInfo* pTargetInfo = !targetBuilding && pTarget != nullptr &&
		playerinfomanager != nullptr ? playerinfomanager->GetPlayerInfo(pTarget) : nullptr;
	if (m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		(m_pPlayerInfo->GetTeamIndex() != EMP_TEAM_NF &&
		 m_pPlayerInfo->GetTeamIndex() != EMP_TEAM_BE))
	{
		failEmpiresScoutBinocularAction("scout-no-longer-alive-or-team-changed");
		return;
	}
	const char* targetClassName = pTarget != nullptr && !pTarget->IsFree() ?
		pTarget->GetClassName() : nullptr;
	int targetTeam = -1;
	const bool targetTeamKnown = pTarget != nullptr &&
		readEmpiresEntityIntProperty(pTarget, "m_iTeamNum", targetTeam);
	const bool targetIdentityValid = targetBuilding ?
		(pTarget != nullptr && !pTarget->IsFree() && targetClassName != nullptr &&
		 std::strncmp(targetClassName, "emp_building_", 13) == 0 &&
		 targetTeamKnown && targetTeam == m_iEmpiresScoutBinocularActionTargetTeam) :
		(pTarget != nullptr && !pTarget->IsFree() && pTargetInfo != nullptr &&
		 pTargetInfo->IsConnected() && !pTargetInfo->IsDead() &&
		 pTargetInfo->GetHealth() > 0 &&
		 pTargetInfo->GetUserID() == m_iEmpiresScoutBinocularActionTargetUserId);
	if (!targetIdentityValid || engine->IndexOfEdict(pTarget) !=
		m_iEmpiresScoutBinocularActionTargetIndex)
	{
		failEmpiresScoutBinocularAction("target-identity-or-alive-state-invalid");
		return;
	}

	const char* expectedClassName = empiresBinocularClassName(
		m_pPlayerInfo->GetTeamIndex());
	const edict_t* pCurrentWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool binocularsActive = pCurrentWeapon != nullptr &&
		!pCurrentWeapon->IsFree() && pCurrentWeapon->GetClassName() != nullptr &&
		expectedClassName != nullptr &&
		std::strcmp(pCurrentWeapon->GetClassName(), expectedClassName) == 0;

	// The debug action owns no movement or unrelated buttons.  The view is
	// still driven through the normal LOOK_EDICT/doLook path below.
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons &= ~(IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_FORWARD | IN_BACK |
		IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK);

	if (m_eEmpiresScoutBinocularActionState ==
		EmpiresScoutBinocularActionState::WAITING_FOR_ACTIVE)
	{
		if (!binocularsActive)
		{
			selectWeaponName(expectedClassName);
			if (now >= m_flEmpiresScoutBinocularActionDeadline)
				failEmpiresScoutBinocularAction("binocular-selection-timeout");
			return;
		}

		m_pEmpiresScoutBinocularActionWeapon = pCurrentWeapon;
		m_eEmpiresScoutBinocularActionState =
			EmpiresScoutBinocularActionState::AIMING;
		m_flEmpiresScoutBinocularActionAimUntil = now + 0.50f;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires scout binocular action=selected index=%d userid=%d target_index=%d target_userid=%d active_weapon=%s weapon_index=%d aim_path=normal_LOOK_EDICT",
				index, userId, m_iEmpiresScoutBinocularActionTargetIndex,
				m_iEmpiresScoutBinocularActionTargetUserId,
				pCurrentWeapon->GetClassName(),
				engine->IndexOfEdict(const_cast<edict_t*>(pCurrentWeapon)));
		return;
	}

	if (!binocularsActive)
	{
		failEmpiresScoutBinocularAction("binoculars-no-longer-active");
		return;
	}

	lookAtEdict(pTarget);
	setLookAtTask(LOOK_EDICT);

	if (!m_bEmpiresScoutBinocularActionPropertyBaselineLogged)
	{
		bool spotted = false;
		const bool spottedKnown = readEmpiresEntityBoolProperty(
			pTarget, "m_bSpotted", spotted);
		int spotter = -1;
		const bool spotterKnown = readEmpiresWeaponHandleProperty(
			pTarget, "m_hSpotter", spotter);
		int visibilityState = -1;
		const bool visibilityKnown = readEmpiresEntityIntProperty(
			pTarget, "m_VisibilityState", visibilityState);
		m_bEmpiresScoutBinocularActionBaselineSpottedKnown = spottedKnown;
		m_bEmpiresScoutBinocularActionBaselineSpotted = spotted;
		m_bEmpiresScoutBinocularActionBaselineSpotterKnown = spotterKnown;
		m_iEmpiresScoutBinocularActionBaselineSpotter = spotter;
		m_bEmpiresScoutBinocularActionBaselineVisibilityKnown = visibilityKnown;
		m_iEmpiresScoutBinocularActionBaselineVisibilityState = visibilityState;
		m_bEmpiresScoutBinocularActionPropertyBaselineLogged = true;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires scout binocular action=spot-state-baseline target_index=%d target_userid=%d target_building=%d target_class=%s target_team=%d m_bSpotted_known=%d m_bSpotted=%d m_hSpotter_known=%d m_hSpotter=%d m_VisibilityState_known=%d m_VisibilityState=%d observation=dynamic-only",
				m_iEmpiresScoutBinocularActionTargetIndex,
				m_iEmpiresScoutBinocularActionTargetUserId,
				targetBuilding ? 1 : 0,
				targetClassName != nullptr ? targetClassName : "<none>",
				targetTeam,
				spottedKnown ? 1 : 0, spotted ? 1 : 0,
				spotterKnown ? 1 : 0, spotter,
				visibilityKnown ? 1 : 0, visibilityState);
	}

	if (m_eEmpiresScoutBinocularActionState ==
		EmpiresScoutBinocularActionState::AIMING)
	{
		if (now < m_flEmpiresScoutBinocularActionAimUntil)
			return;

		if (logger != nullptr)
		{
			const bool geometryVisible = CBotGlobals::isVisible(
				m_pEdict, getEyePosition(), pTarget);
			logger->Log(LogLevel::INFO,
				"Empires scout binocular action=ready index=%d userid=%d target_index=%d target_userid=%d curtime=%.3f geometry_visible=%d view_angles=(%.2f %.2f %.2f)",
				index, userId, m_iEmpiresScoutBinocularActionTargetIndex,
				m_iEmpiresScoutBinocularActionTargetUserId, now,
				geometryVisible ? 1 : 0, m_vViewAngles.x, m_vViewAngles.y,
				m_vViewAngles.z);
		}
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->tap(IN_ATTACK);
		m_eEmpiresScoutBinocularActionState =
			EmpiresScoutBinocularActionState::ATTACK_SUBMITTED;
		m_flEmpiresScoutBinocularActionDeadline = now + 3.0f;
		return;
	}

	if (m_eEmpiresScoutBinocularActionState ==
		EmpiresScoutBinocularActionState::ATTACK_SUBMITTED)
	{
		m_iButtons &= ~IN_ATTACK;
		m_pButtons->letGo(IN_ATTACK);
		m_eEmpiresScoutBinocularActionState =
			EmpiresScoutBinocularActionState::OBSERVING_RESULT;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires scout binocular action=attack-release index=%d userid=%d target_index=%d target_userid=%d curtime=%.3f",
				index, userId, m_iEmpiresScoutBinocularActionTargetIndex,
				m_iEmpiresScoutBinocularActionTargetUserId, now);
		return;
	}

	if (m_eEmpiresScoutBinocularActionState ==
		EmpiresScoutBinocularActionState::OBSERVING_RESULT)
	{
		bool spotted = false;
		const bool spottedKnown = readEmpiresEntityBoolProperty(
			pTarget, "m_bSpotted", spotted);
		int spotter = -1;
		const bool spotterKnown = readEmpiresWeaponHandleProperty(
			pTarget, "m_hSpotter", spotter);
		const bool spottedTransition = spottedKnown && spotted &&
			(!m_bEmpiresScoutBinocularActionBaselineSpottedKnown ||
				!m_bEmpiresScoutBinocularActionBaselineSpotted);
		const bool spotterTransition = spotterKnown && spotter >= 0 &&
			(!m_bEmpiresScoutBinocularActionBaselineSpotterKnown ||
				m_iEmpiresScoutBinocularActionBaselineSpotter != spotter);
		int visibilityState = -1;
		const bool visibilityKnown = readEmpiresEntityIntProperty(
			pTarget, "m_VisibilityState", visibilityState);
		const bool visibilityTransition = visibilityKnown &&
			m_bEmpiresScoutBinocularActionBaselineVisibilityKnown &&
			visibilityState != m_iEmpiresScoutBinocularActionBaselineVisibilityState;
		if ((visibilityTransition || spottedTransition || spotterTransition) &&
			!m_bEmpiresScoutBinocularActionSpotTransitionLogged)
		{
			m_bEmpiresScoutBinocularActionSpotTransitionLogged = true;
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires scout binocular action=result=PASS target_index=%d target_userid=%d target_building=%d target_class=%s target_team=%d spot_transition=1 transition_source=%s m_bSpotted_known=%d m_bSpotted=%d m_hSpotter_known=%d m_hSpotter=%d m_VisibilityState_known=%d m_VisibilityState_baseline=%d m_VisibilityState=%d",
					m_iEmpiresScoutBinocularActionTargetIndex,
					m_iEmpiresScoutBinocularActionTargetUserId,
					targetBuilding ? 1 : 0,
					targetClassName != nullptr ? targetClassName : "<none>",
					targetTeam,
					visibilityTransition ? "m_VisibilityState" :
						(spottedTransition ? "m_bSpotted" : "m_hSpotter"),
					spottedKnown ? 1 : 0, spotted ? 1 : 0,
					spotterKnown ? 1 : 0, spotter,
					visibilityKnown ? 1 : 0,
					m_iEmpiresScoutBinocularActionBaselineVisibilityState,
					visibilityState);
			m_iButtons = 0;
			m_fForwardSpeed = 0.0f;
			m_fSideSpeed = 0.0f;
			m_fUpSpeed = 0.0f;
			m_pButtons->letGo(IN_ATTACK);
			m_eEmpiresScoutBinocularActionState =
				EmpiresScoutBinocularActionState::COMPLETE;
			return;
		}

		if (now >= m_flEmpiresScoutBinocularActionDeadline)
		{
			failEmpiresScoutBinocularAction(
				(m_bEmpiresScoutBinocularActionBaselineSpottedKnown ||
				 m_bEmpiresScoutBinocularActionBaselineSpotterKnown ||
				 m_bEmpiresScoutBinocularActionBaselineVisibilityKnown) ?
				"no-dynamic-spot-transition" :
				"input-processed-effect-unknown-no-public-property");
		}
	}
}

bool CBotEmpires::readEmpiresEntityIntProperty(const edict_t* pEdict,
	const char* propertyName, int& value) const
{
	value = 0;
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree() ||
		servergamedll == nullptr || propertyName == nullptr)
	{
		return false;
	}

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

bool CBotEmpires::readEmpiresEntityBoolProperty(const edict_t* pEdict,
	const char* propertyName, bool& value) const
{
	value = false;
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree() ||
		servergamedll == nullptr || propertyName == nullptr)
	{
		return false;
	}

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

bool CBotEmpires::writeEmpiresEntityIntProperty(const edict_t* pEdict,
	const char* propertyName, const int value) const
{
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree() ||
		servergamedll == nullptr || propertyName == nullptr)
	{
		return false;
	}

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
				*pValue = value;
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

	*reinterpret_cast<int*>(reinterpret_cast<char*>(pEntity) + offset) = value;
	return true;
}


void CBotEmpires::killed(edict_t* pVictim, char* weapon)
{
	CBot::killed(pVictim, weapon);
	if (EmpiresAnalysis::IsActive())
		EmpiresAnalysis::RecordBotEvent(this, "KILL", pVictim, weapon);
}

void CBotEmpires::died(edict_t* pKiller, const char* pszWeapon)
{
	m_flEmpiresTurretRocketUntil = 0.0f;
	m_bEmpiresTurretRocketApproach = false;
	m_flVoidNextCheck = 0.0f;
	m_flVoidSince = -1.0f;
	m_bVoidKilled = false;
	m_iEmpiresLastHealth = 0;
	if (m_bEmpiresNavGoalActive)
		endEmpiresNavmeshGoal("drop", "died", 0.0f);
	m_flEmpiresNavGoalHoldUntil = 0.0f;

	// The debug mine-trigger fixture positions the expected opposing bot on
	// the deployed mine.  Empires can apply the blast to the target and the
	// placer in the same simulation step, so the placer may die before its
	// next Think() can observe the removed projectile.  Record only this
	// identity-checked natural death; no damage, entity state or kill result is
	// injected by the fixture.
	const bool mineDeathWeapon = pszWeapon != nullptr &&
		std::strstr(pszWeapon, "mine") != nullptr;
	if (mineDeathWeapon && pKiller != nullptr && m_pEdict != nullptr &&
		engine != nullptr && pKiller != m_pEdict)
	{
		CBot* pKillerBase = CBots::getBotPointer(pKiller);
		CBotEmpires* pMinePlacer = pKillerBase != nullptr &&
			pKillerBase != this ? static_cast<CBotEmpires*>(pKillerBase) : nullptr;
		const bool expectedTargetDeath = pMinePlacer != nullptr &&
			pMinePlacer->m_eEmpiresMineActionState ==
				EmpiresMineActionState::OBSERVING_TRIGGER &&
			pMinePlacer->m_bEmpiresMineActionTriggerMode &&
			pMinePlacer->m_pEmpiresMineActionTriggerTargetEdict == m_pEdict &&
			pMinePlacer->m_pEmpiresMineActionExpectedEdict == pKiller &&
			engine->IndexOfEdict(m_pEdict) ==
				pMinePlacer->m_iEmpiresMineActionTriggerTargetIndex &&
			engine->GetPlayerUserId(m_pEdict) ==
				pMinePlacer->m_iEmpiresMineActionTriggerTargetUserId;
		if (expectedTargetDeath)
		{
			pMinePlacer->m_bEmpiresMineActionTriggerNaturalDeathLogged = true;
			if (!pMinePlacer->m_bEmpiresMineActionTriggerImpactLogged)
			{
				pMinePlacer->m_bEmpiresMineActionTriggerImpactLogged = true;
				int healthAfter = -1;
				if (playerinfomanager != nullptr)
				{
					IPlayerInfo* pVictimInfo =
						playerinfomanager->GetPlayerInfo(m_pEdict);
					if (pVictimInfo != nullptr)
						healthAfter = pVictimInfo->GetHealth();
				}
				pMinePlacer->m_iEmpiresMineActionTriggerTargetHealthAfter = healthAfter;
				if (logger != nullptr)
					logger->Log(LogLevel::INFO,
						"Empires mine trigger=target-affected index=%d userid=%d target_index=%d target_userid=%d health_before=%d health_after=%d target_dead=1 reason=death-callback",
						engine->IndexOfEdict(pMinePlacer->m_pEdict),
						engine->GetPlayerUserId(pMinePlacer->m_pEdict),
						pMinePlacer->m_iEmpiresMineActionTriggerTargetIndex,
						pMinePlacer->m_iEmpiresMineActionTriggerTargetUserId,
						pMinePlacer->m_iEmpiresMineActionTriggerTargetHealthBefore,
						healthAfter);
			}
		}
	}

	if (isEmpiresEngineerRepairActionActive())
		failEmpiresEngineerRepairAction("bot-died-before-action-complete");

	if (isEmpiresVehicleActionActive())
		failEmpiresVehicleAction("bot-died-before-action-complete");

	if (isEmpiresEngineerResupplyActionActive())
		failEmpiresEngineerResupplyAction("bot-died-before-action-complete");

	if (isEmpiresMortarActionActive())
		failEmpiresMortarAction("bot-died-before-action-complete");

	if (isEmpiresScoutBinocularActionActive())
		failEmpiresScoutBinocularAction("bot-died-before-action-complete");

	const bool keepMineTriggerObservation = isEmpiresMineActionActive() &&
		m_bEmpiresMineActionTriggerMode &&
		m_bEmpiresMineActionTriggerNaturalDeathLogged;
	if (isEmpiresMineActionActive() && !keepMineTriggerObservation)
		failEmpiresMineAction("bot-died-before-action-complete");

	if (isEmpiresMortarProbeActive())
		failEmpiresMortarSmoke("bot-died-before-probe-complete");

	if (g_empireEnemyLosFixture.active &&
		(m_pEdict == g_empireEnemyLosFixture.observerEdict ||
			m_pEdict == g_empireEnemyLosFixture.opponentEdict))
	{
		fixtureLog("Empires enemy LOS fixture=result=FAIL reason=fixture-actor-died");
		clearEmpiresEnemyLosFixture("actor-died");
	}
	if (g_empireScoutPriorityFixture.active &&
		(m_pEdict == g_empireScoutPriorityFixture.scoutEdict ||
			m_pEdict == g_empireScoutPriorityFixture.targetAEdict ||
			m_pEdict == g_empireScoutPriorityFixture.targetBEdict))
	{
		fixtureLog("Empires scout-priority fixture=result=FAIL reason=actor-died");
		clearEmpiresScoutPriorityFixture("actor-died");
	}
	if (m_bEmpiresCommanderMoveRouteIssued || m_bEmpiresCommanderMoveTestActive)
		clearEmpiresCommanderMoveState("bot-died");
	if (m_bEmpiresSquadGoalTaskIssued)
	{
		EmpiresAnalysis::RecordBotEvent(this, "SQUAD_GOAL_RECOVERY", nullptr,
			"reason=bot-died;shared-goal-retained-for-survivors");
		logger->Log(LogLevel::INFO,
			"Empires gameplay squad-goal=recovery reason=bot-died owner_userid=%d generation=%d shared_goal_retained=1",
			engine != nullptr && m_pEdict != nullptr ?
				engine->GetPlayerUserId(m_pEdict) : -1,
			m_iEmpiresSquadGoalGeneration);
	}
	resetEmpiresSquadGoalState();
	endEmpiresRecoveryEpisode("DEATH");
	m_iCommanderEntryStage = 0;
	m_iCommanderBuildStage = 0; // the seat choice waits for the resumed build-assign
	if (m_iBuildAssignStage > 0)
	{
		m_iBuildAssignStage = 1; // resumes from the walk after respawn
		Msg("[RCBOT2] build-assign event=died resuming-after-respawn\n");
	}
	CBot::died(pKiller, pszWeapon);
	if (EmpiresAnalysis::IsActive())
		EmpiresAnalysis::RecordBotEvent(this, "BOT_DEATH", pKiller, pszWeapon);
	if (m_pButtons != nullptr)
		m_pButtons->letGo(IN_ATTACK);
	m_iButtons &= ~IN_ATTACK;
	m_pEmpiresLastSpotTarget = nullptr;
	m_flEmpiresNextSpotTime = 0.0f;
	m_bEmpiresLastEnemyObscured = false;
	m_bEmpiresLastEnemySearchActive = false;
	m_bEmpiresLastEnemySearchTaskLogged = false;
	m_bEmpiresCombatEngaged = false;
	m_bEmpiresCoverScheduleActive = false;
	m_bEmpiresCaptureApproachIssued = false;
	m_bEmpiresCaptureHoldIssued = false;
	m_bEmpiresEngineerSupportTaskIssued = false;
	m_bEmpiresEngineerSupportNearby = false;
	m_bEmpiresEngineerResupplyTaskIssued = false;
	m_iEmpiresCaptureApproachPoint = -1;
	m_iEmpiresCaptureRouteWaypoint = -1;
	m_iEmpiresCaptureApproachRetries = 0;
	m_iEmpiresCaptureRouteAttemptGeneration = 0;
	m_captureInterruptionLatch.reset();
	clearEmpiresStartAnchorRecovery();
	m_flEmpiresCaptureRouteIssuedTime = -1.0f;
	m_iEmpiresCaptureRejectedPoint = -1;
	m_iEmpiresEngineerSupportBuildingIndex = -1;
	m_iEmpiresEngineerSupportWaypoint = -1;
	m_iEmpiresEngineerSupportRejectedBuildingIndex = -1;
	m_iEmpiresEngineerSupportRejectedWaypoint = -1;
	m_iEmpiresEngineerResupplyWaypoint = -1;
	m_vEmpiresCaptureApproachOrigin = vec3_origin;
	m_vEmpiresCaptureApproachStartOrigin = vec3_origin;
	m_vEmpiresEngineerSupportWaypointOrigin = vec3_origin;
	m_vEmpiresEngineerResupplyWaypointOrigin = vec3_origin;
	m_flEmpiresNextCaptureApproachTime = 0.0f;
	m_flEmpiresNextCaptureApproachProgressLogTime = 0.0f;
	m_flEmpiresCaptureHoldDeadline = 0.0f;
	m_flEmpiresCaptureRejectedUntil = 0.0f;
	m_flEmpiresNextEngineerSupportTime = 0.0f;
	m_flEmpiresEngineerSupportRejectedUntil = 0.0f;
	m_flEmpiresNextEngineerResupplyTime = 0.0f;
	m_flEmpiresNextLastKnownMineTime = 0.0f;
	m_flEmpiresNextVisibleMortarTime = 0.0f;
	m_flEmpiresReactionUntil = 0.0f;
	if (!isControlledTestOnly())
	{
		m_pEmpiresApproachTarget = nullptr;
		m_bEmpiresApproachTaskIssued = false;
		m_flEmpiresRelevanceIgnoreLogTime = 0.0f;
		if (m_pSchedules != nullptr)
			m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
	}

	// A player_death during the initial team/class/spawn sequence is not a
	// respawn of an already-running bot. Only mark a normal, spawned bot for
	// the repeatable normal respawn preparation path.
	if (isControlledTestOnly() || m_eNormalSpawnState != EmpiresNormalSpawnState::COMPLETE ||
		m_bNormalDeathEventPending)
		return;

	m_bNormalDeathEventPending = true;
	m_flReviveDeathTime = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	logger->Log(LogLevel::INFO,
		"Empires normal lifecycle=death-event-pending index=%d userid=%d path_state=%s",
		engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
		engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
		normalPathStateName());
}

const char* CBotEmpires::normalPathStateName() const
{
	switch (m_eNormalPathState)
	{
	case EmpiresNormalPathState::IDLE:
		return "IDLE";
	case EmpiresNormalPathState::SEARCHING:
		return "SEARCHING";
	case EmpiresNormalPathState::ARMED:
		return "ARMED";
	case EmpiresNormalPathState::ACTIVE:
		return "ACTIVE";
	case EmpiresNormalPathState::COMPLETE:
		return "COMPLETE";
	case EmpiresNormalPathState::FAILED:
		return "FAILED";
	}

	return "UNKNOWN";
}

bool CBotEmpires::normalPathRouteMatches(const int startWaypoint,
	const int destinationWaypoint) const
{
	return m_NormalPathRoute.size() == 2 &&
		m_NormalPathRoute[0] == startWaypoint &&
		m_NormalPathRoute[1] == destinationWaypoint;
}

bool CBotEmpires::normalPathRouteMatches(
	const std::vector<int>& expectedRoute) const
{
	return m_NormalPathRoute == expectedRoute;
}

void CBotEmpires::prepareEmpiresEnemyLosFixture()
{
	const bool normalPathWasStarted = m_bNormalPathAutoStarted;
	const char* previousPathState = normalPathStateName();
	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		static_cast<CWaypointNavigator*>(m_pNavigator)->clear();

	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	stopMoving();

	// The fixture owns both actors' temporary placement. Clear only the
	// normal path bookkeeping that could overwrite that placement. Preserve
	// the automatic-start marker so a cancelled startup route is not restarted
	// underneath the fixture. Enemy handles and last-known-enemy fields are
	// deliberately untouched; normal Think/perception must own those states.
	m_eNormalPathState = EmpiresNormalPathState::IDLE;
	m_bNormalPathAutoStarted = normalPathWasStarted;
	m_bNormalPathTerminalAfterStop = false;
	m_bNormalPathSearchStarted = false;
	m_iNormalPathSearchFrames = 0;
	m_iNormalPathStartWaypoint = -1;
	m_iNormalPathDestinationWaypoint = -1;
	m_iNormalPathLastWaypoint = -1;
	m_iNormalPathActiveFrames = 0;
	m_iNormalPathNoProgressFrames = 0;
	m_iNormalPathExpectedIndex = -1;
	m_iNormalPathExpectedUserId = -1;
	m_pNormalPathExpectedEdict = nullptr;
	m_flNormalPathStartTime = 0.0f;
	m_flNormalPathDeadline = 0.0f;
	m_bNormalPathLastOriginKnown = false;
	m_NormalPathRoute.clear();
	m_pEmpiresApproachTarget = nullptr;
	m_bEmpiresApproachTaskIssued = false;
	m_bEmpiresCoverScheduleActive = false;
	m_bEmpiresCaptureApproachIssued = false;
	m_bEmpiresCaptureHoldIssued = false;
	m_iEmpiresCaptureApproachPoint = -1;
	m_iEmpiresCaptureRouteWaypoint = -1;
	m_iEmpiresCaptureApproachRetries = 0;
	m_captureInterruptionLatch.reset();
	clearEmpiresStartAnchorRecovery();
	m_flEmpiresCaptureRouteIssuedTime = -1.0f;
	m_iEmpiresCaptureRejectedPoint = -1;
	m_vEmpiresCaptureApproachOrigin = vec3_origin;
	m_vEmpiresCaptureApproachStartOrigin = vec3_origin;
	m_flEmpiresNextCaptureApproachTime = 0.0f;
	m_flEmpiresNextCaptureApproachProgressLogTime = 0.0f;
	m_flEmpiresCaptureHoldDeadline = 0.0f;
	m_flEmpiresCaptureRejectedUntil = 0.0f;

	if (std::strcmp(previousPathState, "IDLE") != 0 && logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires enemy LOS fixture=cancelled-preexisting-path state=%s memory-preserved=1",
			previousPathState);
	}
}

bool CBotEmpires::hasEmpiresFixtureEnemy(const edict_t* pTarget)
{
	return pTarget != nullptr && m_pEnemy.get() == pTarget &&
		hasSomeConditions(CONDITION_SEE_CUR_ENEMY) &&
		CBotGlobals::entityIsValid(const_cast<edict_t*>(pTarget)) &&
		CBotGlobals::entityIsAlive(const_cast<edict_t*>(pTarget)) &&
		isVisible(pTarget);
}

bool CBotEmpires::requestEmpiresEnemyLosFixture(const int observerUserId,
	const int opponentUserId, const bool requireNaturalExpiry,
	const bool combatMode, const bool releaseObserverForCombatMovement)
{
	if (g_empireEnemyLosFixture.active || observerUserId < 0 ||
		opponentUserId < 0 || observerUserId == opponentUserId ||
		engine == nullptr || gpGlobals == nullptr)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires enemy LOS fixture rejected: already active, invalid userids or engine state");
		return false;
	}

	CBotEmpires* pObserver = fixtureBotForUserId(observerUserId);
	CBotEmpires* pOpponent = fixtureBotForUserId(opponentUserId);
	if (!fixtureActorValid(pObserver, observerUserId,
		pObserver != nullptr && pObserver->getEdict() != nullptr ?
			engine->IndexOfEdict(pObserver->getEdict()) : -1,
		pObserver != nullptr ? pObserver->getEdict() : nullptr) ||
		!fixtureActorValid(pOpponent, opponentUserId,
		pOpponent != nullptr && pOpponent->getEdict() != nullptr ?
			engine->IndexOfEdict(pOpponent->getEdict()) : -1,
		pOpponent != nullptr ? pOpponent->getEdict() : nullptr))
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires enemy LOS fixture rejected: both addressed normal bots must be alive with controllers");
		return false;
	}

	const int observerTeam = pObserver->getTeam();
	const int opponentTeam = pOpponent->getTeam();
	if ((observerTeam != EMP_TEAM_NF && observerTeam != EMP_TEAM_BE) ||
		(observerTeam == opponentTeam) ||
		(opponentTeam != EMP_TEAM_NF && opponentTeam != EMP_TEAM_BE))
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires enemy LOS fixture rejected: observer_team=%d opponent_team=%d must be opposing NF/BE teams",
				observerTeam, opponentTeam);
		return false;
	}

	const int waypointCount = CWaypoints::numWaypoints();
	constexpr int kObserverFixtureWaypoint = 160;
	// The normal LOS fixture and the movement-only approach fixture use the
	// already proven close visibility pair. The standard combat fixture picks
	// a known set for the loaded con_district402 waypoint graph below.
	constexpr int kCloseObserverFixtureWaypoint = 160;
	constexpr int kCombatObserverFixtureWaypoint = 147;
	const bool movementFixture = combatMode && releaseObserverForCombatMovement;
	int observerFixtureWaypoint = combatMode ?
		(movementFixture ? kCloseObserverFixtureWaypoint :
			kCombatObserverFixtureWaypoint) : kObserverFixtureWaypoint;
	int opponentStartWaypoint = combatMode ?
		(movementFixture ? 155 : 149) : 155;
	int opponentDestinationWaypoint = requireNaturalExpiry ? 157 : 156;
	const char* mapName = CBotGlobals::getMapName();
	const bool districtCombatFixture = combatMode && !movementFixture &&
		!requireNaturalExpiry && mapName != nullptr &&
		std::strcmp(mapName, "con_district402") == 0;
	if (districtCombatFixture)
	{
		EmpiresCombatFixture::WaypointSet waypointSet;
		if (!EmpiresCombatFixture::selectWaypointSet(waypointCount, &waypointSet))
		{
			if (logger != nullptr)
				logger->Log(LogLevel::WARN,
					"Empires enemy combat fixture rejected: unsupported waypoint dataset map=%s waypoint_count=%d",
					mapName, waypointCount);
			return false;
		}

		observerFixtureWaypoint = waypointSet.observer;
		opponentStartWaypoint = waypointSet.opponentStart;
		opponentDestinationWaypoint = waypointSet.opponentDestination;
	}
	CWaypoint* pObserverWaypoint = CWaypoints::getWaypoint(observerFixtureWaypoint);
	CWaypoint* pOpponentStart = CWaypoints::getWaypoint(opponentStartWaypoint);
	CWaypoint* pOpponentDestination = CWaypoints::getWaypoint(opponentDestinationWaypoint);
	if (pObserverWaypoint == nullptr || pOpponentStart == nullptr ||
		pOpponentDestination == nullptr || !pObserverWaypoint->isUsed() ||
		!pOpponentStart->isUsed() || !pOpponentDestination->isUsed() ||
		!pOpponentStart->forTeam(opponentTeam) ||
		!pOpponentDestination->forTeam(opponentTeam))
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires enemy %s fixture rejected: selected waypoints %d, %d or %d are unavailable for opponent_team=%d waypoint_count=%d",
				combatMode ? "combat" : "LOS", observerFixtureWaypoint, opponentStartWaypoint,
				opponentDestinationWaypoint, opponentTeam, waypointCount);
		return false;
	}

	pObserver->prepareEmpiresEnemyLosFixture();
	pOpponent->prepareEmpiresEnemyLosFixture();

	Vector observerOrigin = pObserverWaypoint->getOrigin();
	Vector opponentOrigin = pOpponentStart->getOrigin();
	Vector observerToOpponent = opponentOrigin - observerOrigin;
	if (observerToOpponent.LengthSqr() < 1.0f)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires enemy LOS fixture rejected: observer and opponent waypoints overlap");
		return false;
	}

	const float initialSeparation = observerToOpponent.Length();
	if (combatMode && !movementFixture && initialSeparation < 450.0f)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires enemy combat fixture rejected: initial_separation=%.1f below RPG-safe minimum=450.0",
				initialSeparation);
		return false;
	}

	constexpr float kRadiansToDegrees = 57.29577951308232f;
	const float observerYaw = std::atan2(observerToOpponent.y,
		observerToOpponent.x) * kRadiansToDegrees;
	Vector opponentToDestination = pOpponentDestination->getOrigin() - opponentOrigin;
	const float opponentYaw = std::atan2(opponentToDestination.y,
		opponentToDestination.x) * kRadiansToDegrees;
	QAngle observerAngles(0.0f, observerYaw, 0.0f);
	QAngle opponentAngles(0.0f, opponentYaw, 0.0f);
	IBotController* pObserverController = pObserver->getController();
	IBotController* pOpponentController = pOpponent->getController();
	if (pObserverController == nullptr || pOpponentController == nullptr)
		return false;

	// Position and orient each actor exactly once. Outgoing command
	// neutralization below prevents stale normal movement from immediately
	// undoing the fixture setup while the engine settles the actors.
	pObserverController->SetAbsOrigin(observerOrigin);
	pObserverController->SetAbsAngles(observerAngles);
	pOpponentController->SetAbsOrigin(opponentOrigin);
	pOpponentController->SetAbsAngles(opponentAngles);

	g_empireEnemyLosFixture = EmpiresEnemyLosFixtureState();
	g_empireEnemyLosFixture.active = true;
	g_empireEnemyLosFixture.phase = EmpiresEnemyLosFixturePhase::SETTLING;
	g_empireEnemyLosFixture.observerUserId = observerUserId;
	g_empireEnemyLosFixture.opponentUserId = opponentUserId;
	g_empireEnemyLosFixture.observerIndex = engine->IndexOfEdict(pObserver->getEdict());
	g_empireEnemyLosFixture.opponentIndex = engine->IndexOfEdict(pOpponent->getEdict());
	g_empireEnemyLosFixture.observerEdict = pObserver->getEdict();
	g_empireEnemyLosFixture.opponentEdict = pOpponent->getEdict();
	g_empireEnemyLosFixture.requireNaturalExpiry = requireNaturalExpiry;
	g_empireEnemyLosFixture.combatMode = combatMode;
	g_empireEnemyLosFixture.releaseObserverForCombatMovement =
		releaseObserverForCombatMovement;
	g_empireEnemyLosFixture.holdObserver = true;
	g_empireEnemyLosFixture.holdOpponent = true;
	g_empireEnemyLosFixture.observerAnchor = observerOrigin;
	g_empireEnemyLosFixture.opponentAnchor = opponentOrigin;
	g_empireEnemyLosFixture.observerLastOrigin = observerOrigin;
	g_empireEnemyLosFixture.observerLastOriginKnown = true;
	g_empireEnemyLosFixture.deadline = engine->Time() + 12.0f;

	fixtureLog(
		"Empires enemy LOS fixture=armed observer_userid=%d observer_index=%d opponent_userid=%d opponent_index=%d observer_waypoint=%d opponent_route=%d->%d waypoint_count=%d initial_separation=%.1f mode=%s positioning=SetAbsOrigin/SetAbsAngles once movement_hold=command-boundary combat_input=%s",
		observerUserId, g_empireEnemyLosFixture.observerIndex, opponentUserId,
		g_empireEnemyLosFixture.opponentIndex, observerFixtureWaypoint,
		opponentStartWaypoint, opponentDestinationWaypoint, waypointCount, initialSeparation,
		combatMode ? (releaseObserverForCombatMovement ?
			"COMBAT_MOVEMENT" : "COMBAT") :
			(requireNaturalExpiry ? "NATURAL_EXPIRY" : "REACQUISITION"),
		combatMode && !releaseObserverForCombatMovement ? "NORMAL" :
			(combatMode ? "NORMAL_AFTER_ACQUISITION" : "IN_ATTACK_SUPPRESSED"));
	return true;
}

void CBotEmpires::clearEmpiresEnemyLosFixture(const char* reason)
{
	if (!g_empireEnemyLosFixture.active)
		return;

	fixtureLog("Empires enemy LOS fixture=cleanup reason=%s", reason != nullptr ? reason : "unspecified");
	g_empireEnemyLosFixture = EmpiresEnemyLosFixtureState();
}

bool CBotEmpires::requestEmpiresScoutPriorityFixture(
	const int scoutUserId, const int targetAUserId, const int targetBUserId,
	const int firingTargetUserId)
{
	if (g_empireScoutPriorityFixture.active ||
		scoutUserId < 0 || targetAUserId < 0 || targetBUserId < 0 ||
		targetAUserId == targetBUserId || scoutUserId == targetAUserId ||
		scoutUserId == targetBUserId ||
		(firingTargetUserId != targetAUserId &&
			firingTargetUserId != targetBUserId) ||
		engine == nullptr || gpGlobals == nullptr)
	{
		return false;
	}

	CBotEmpires* pScout = fixtureBotForUserId(scoutUserId);
	CBotEmpires* pTargetA = fixtureBotForUserId(targetAUserId);
	CBotEmpires* pTargetB = fixtureBotForUserId(targetBUserId);
	if (!fixtureActorValid(pScout, scoutUserId,
		pScout != nullptr && pScout->getEdict() != nullptr ?
			engine->IndexOfEdict(pScout->getEdict()) : -1,
		pScout != nullptr ? pScout->getEdict() : nullptr) ||
		!fixtureActorValid(pTargetA, targetAUserId,
		pTargetA != nullptr && pTargetA->getEdict() != nullptr ?
			engine->IndexOfEdict(pTargetA->getEdict()) : -1,
		pTargetA != nullptr ? pTargetA->getEdict() : nullptr) ||
		!fixtureActorValid(pTargetB, targetBUserId,
		pTargetB != nullptr && pTargetB->getEdict() != nullptr ?
			engine->IndexOfEdict(pTargetB->getEdict()) : -1,
		pTargetB != nullptr ? pTargetB->getEdict() : nullptr))
	{
		return false;
	}

	const int scoutTeam = pScout->getTeam();
	const int targetTeamA = pTargetA->getTeam();
	const int targetTeamB = pTargetB->getTeam();
	if ((scoutTeam != EMP_TEAM_NF && scoutTeam != EMP_TEAM_BE) ||
		targetTeamA != targetTeamB || targetTeamA == scoutTeam ||
		(targetTeamA != EMP_TEAM_NF && targetTeamA != EMP_TEAM_BE))
	{
		return false;
	}

	// Keep both opposing candidates in the already proven visible corridor.
	// The target positions are existing map waypoints; normal visibility and
	// CFindEnemyFunc selection remain the observed production path.
	constexpr int kScoutWaypoint = 160;
	constexpr int kTargetAWaypoint = 149;
	constexpr int kTargetBWaypoint = 150;
	CWaypoint* pScoutWaypoint = CWaypoints::getWaypoint(kScoutWaypoint);
	CWaypoint* pTargetAWaypoint = CWaypoints::getWaypoint(kTargetAWaypoint);
	CWaypoint* pTargetBWaypoint = CWaypoints::getWaypoint(kTargetBWaypoint);
	if (pScoutWaypoint == nullptr || pTargetAWaypoint == nullptr ||
		pTargetBWaypoint == nullptr || !pScoutWaypoint->isUsed() ||
		!pTargetAWaypoint->isUsed() || !pTargetBWaypoint->isUsed() ||
		!pScoutWaypoint->forTeam(scoutTeam) ||
		!pTargetAWaypoint->forTeam(targetTeamA) ||
		!pTargetBWaypoint->forTeam(targetTeamB))
	{
		return false;
	}

	pScout->prepareEmpiresEnemyLosFixture();
	pTargetA->prepareEmpiresEnemyLosFixture();
	pTargetB->prepareEmpiresEnemyLosFixture();
	for (CBotEmpires* pBot : {pScout, pTargetA, pTargetB})
	{
		pBot->setEnemy(nullptr);
		pBot->setLastEnemy(nullptr);
		pBot->removeCondition(CONDITION_SEE_CUR_ENEMY |
			CONDITION_ENEMY_OBSCURED | CONDITION_ENEMY_DEAD |
			CONDITION_SEE_ENEMY_HEAD);
	}

	Vector scoutOrigin = pScoutWaypoint->getOrigin();
	Vector targetAOrigin = pTargetAWaypoint->getOrigin();
	Vector targetBOrigin = pTargetBWaypoint->getOrigin();
	const Vector midpoint = (targetAOrigin + targetBOrigin) * 0.5f;
	QAngle scoutAngles;
	VectorAngles(midpoint - scoutOrigin, scoutAngles);
	QAngle targetAAngles;
	VectorAngles(scoutOrigin - targetAOrigin, targetAAngles);
	QAngle targetBAngles;
	VectorAngles(scoutOrigin - targetBOrigin, targetBAngles);
	pScout->getController()->SetAbsOrigin(scoutOrigin);
	pScout->getController()->SetAbsAngles(scoutAngles);
	pTargetA->getController()->SetAbsOrigin(targetAOrigin);
	pTargetA->getController()->SetAbsAngles(targetAAngles);
	pTargetB->getController()->SetAbsOrigin(targetBOrigin);
	pTargetB->getController()->SetAbsAngles(targetBAngles);

	g_empireScoutPriorityFixture = EmpiresScoutPriorityFixtureState();
	g_empireScoutPriorityFixture.active = true;
	g_empireScoutPriorityFixture.scoutUserId = scoutUserId;
	g_empireScoutPriorityFixture.targetAUserId = targetAUserId;
	g_empireScoutPriorityFixture.targetBUserId = targetBUserId;
	g_empireScoutPriorityFixture.firingTargetUserId = firingTargetUserId;
	g_empireScoutPriorityFixture.scoutIndex = engine->IndexOfEdict(
		pScout->getEdict());
	g_empireScoutPriorityFixture.targetAIndex = engine->IndexOfEdict(
		pTargetA->getEdict());
	g_empireScoutPriorityFixture.targetBIndex = engine->IndexOfEdict(
		pTargetB->getEdict());
	g_empireScoutPriorityFixture.scoutEdict = pScout->getEdict();
	g_empireScoutPriorityFixture.targetAEdict = pTargetA->getEdict();
	g_empireScoutPriorityFixture.targetBEdict = pTargetB->getEdict();
	g_empireScoutPriorityFixture.scoutAnchor = scoutOrigin;
	g_empireScoutPriorityFixture.targetAAnchor = targetAOrigin;
	g_empireScoutPriorityFixture.targetBAnchor = targetBOrigin;
	g_empireScoutPriorityFixture.lastScoutOrigin = scoutOrigin;
	g_empireScoutPriorityFixture.lastTargetAOrigin = targetAOrigin;
	g_empireScoutPriorityFixture.lastTargetBOrigin = targetBOrigin;
	g_empireScoutPriorityFixture.lastOriginsKnown = true;
	// Normal CBot visibility is scanned incrementally across client slots. Keep
	// this bounded, but allow that public sight-update path to complete before
	// judging target selection on this three-bot fixture.
	g_empireScoutPriorityFixture.deadline = engine->Time() + 30.0f;

	fixtureLog(
		"Empires scout-priority fixture=armed scout_userid=%d target_a_userid=%d target_b_userid=%d firing_target_userid=%d scout_waypoint=%d target_a_waypoint=%d target_b_waypoint=%d target_a_distance=%.1f target_b_distance=%.1f positioning=known-visible-waypoints input=public-last-user-command",
		scoutUserId, targetAUserId, targetBUserId, firingTargetUserId,
		kScoutWaypoint, kTargetAWaypoint, kTargetBWaypoint,
		fixtureDistance(scoutOrigin, targetAOrigin),
		fixtureDistance(scoutOrigin, targetBOrigin));
	return true;
}

void CBotEmpires::clearEmpiresScoutPriorityFixture(const char* reason)
{
	if (!g_empireScoutPriorityFixture.active)
		return;

	fixtureLog("Empires scout-priority fixture=cleanup reason=%s",
		reason != nullptr ? reason : "unspecified");
	g_empireScoutPriorityFixture = EmpiresScoutPriorityFixtureState();
}

void processEmpiresScoutPriorityFixture(CBotEmpires* pCaller)
{
	EmpiresScoutPriorityFixtureState& fixture =
		g_empireScoutPriorityFixture;
	if (!fixture.active || pCaller == nullptr || gpGlobals == nullptr ||
		engine == nullptr || fixture.lastProcessTick == gpGlobals->tickcount)
		return;
	fixture.lastProcessTick = gpGlobals->tickcount;

	CBotEmpires* pScout = fixtureBotForUserId(fixture.scoutUserId);
	CBotEmpires* pTargetA = fixtureBotForUserId(fixture.targetAUserId);
	CBotEmpires* pTargetB = fixtureBotForUserId(fixture.targetBUserId);
	if (!fixtureActorValid(pScout, fixture.scoutUserId, fixture.scoutIndex,
		fixture.scoutEdict) ||
		!fixtureActorValid(pTargetA, fixture.targetAUserId,
			fixture.targetAIndex, fixture.targetAEdict) ||
		!fixtureActorValid(pTargetB, fixture.targetBUserId,
			fixture.targetBIndex, fixture.targetBEdict))
	{
		CBotEmpires::clearEmpiresScoutPriorityFixture("actor-invalid");
		return;
	}

	const Vector scoutOrigin = pScout->getOrigin();
	const Vector targetAOrigin = pTargetA->getOrigin();
	const Vector targetBOrigin = pTargetB->getOrigin();
	const float scoutDrift = fixtureDistance(pScout->getOrigin(),
		fixture.scoutAnchor);
	const float targetADrift = fixtureDistance(pTargetA->getOrigin(),
		fixture.targetAAnchor);
	const float targetBDrift = fixtureDistance(pTargetB->getOrigin(),
		fixture.targetBAnchor);
	if (scoutDrift > 256.0f || targetADrift > 256.0f || targetBDrift > 256.0f)
	{
		fixtureLog(
			"Empires scout-priority fixture=result=FAIL reason=settling-drift scout_drift=%.1f target_a_drift=%.1f target_b_drift=%.1f",
			scoutDrift, targetADrift, targetBDrift);
		CBotEmpires::clearEmpiresScoutPriorityFixture("settling-drift");
		return;
	}

	const float scoutStep = fixture.lastOriginsKnown ? fixtureDistance(
		scoutOrigin, fixture.lastScoutOrigin) : 0.0f;
	const float targetAStep = fixture.lastOriginsKnown ? fixtureDistance(
		targetAOrigin, fixture.lastTargetAOrigin) : 0.0f;
	const float targetBStep = fixture.lastOriginsKnown ? fixtureDistance(
		targetBOrigin, fixture.lastTargetBOrigin) : 0.0f;
	fixture.lastScoutOrigin = scoutOrigin;
	fixture.lastTargetAOrigin = targetAOrigin;
	fixture.lastTargetBOrigin = targetBOrigin;
	fixture.lastOriginsKnown = true;
	if (scoutStep <= 3.0f && targetAStep <= 3.0f && targetBStep <= 3.0f)
		++fixture.stableFrames;
	else
		fixture.stableFrames = 0;

	if (fixture.stableFrames >= 8 && !fixture.stableLogged)
	{
		fixture.scoutAnchor = scoutOrigin;
		fixture.targetAAnchor = targetAOrigin;
		fixture.targetBAnchor = targetBOrigin;
		fixture.stableLogged = true;
		fixtureLog(
			"Empires scout-priority fixture=settled stable_frames=%d scout_userid=%d target_a_userid=%d target_b_userid=%d firing_target_userid=%d movement_hold=command-boundary scout_drift=%.1f target_a_drift=%.1f target_b_drift=%.1f",
			fixture.stableFrames, fixture.scoutUserId, fixture.targetAUserId,
			fixture.targetBUserId, fixture.firingTargetUserId, scoutDrift,
			targetADrift, targetBDrift);
	}

	if (fixture.stableLogged && !fixture.visibilityDiagnosticLogged)
	{
		const bool targetAGeometryVisible = CBotGlobals::isVisible(
			pScout->getEdict(), pScout->getEyePosition(), pTargetA->getEdict());
		const bool targetBGeometryVisible = CBotGlobals::isVisible(
			pScout->getEdict(), pScout->getEyePosition(), pTargetB->getEdict());
		const bool targetABotVisible = pScout->isVisible(pTargetA->getEdict());
		const bool targetBBotVisible = pScout->isVisible(pTargetB->getEdict());
		const bool targetAFovVisible = pScout->FInViewCone(pTargetA->getEdict());
		const bool targetBFovVisible = pScout->FInViewCone(pTargetB->getEdict());
		fixture.visibilityDiagnosticLogged = true;
		fixtureLog(
			"Empires scout-priority fixture=visibility target_a_userid=%d target_b_userid=%d target_a_geometry=%d target_a_bot_visibility=%d target_a_fov=%d target_b_geometry=%d target_b_bot_visibility=%d target_b_fov=%d target_a_distance=%.1f target_b_distance=%.1f",
			fixture.targetAUserId, fixture.targetBUserId,
			targetAGeometryVisible ? 1 : 0, targetABotVisible ? 1 : 0,
			targetAFovVisible ? 1 : 0, targetBGeometryVisible ? 1 : 0,
			targetBBotVisible ? 1 : 0, targetBFovVisible ? 1 : 0,
			fixtureDistance(pScout->getOrigin(), pTargetA->getOrigin()),
			fixtureDistance(pScout->getOrigin(), pTargetB->getOrigin()));
	}

	if (engine->Time() >= fixture.deadline)
	{
		fixtureLog(
			"Empires scout-priority fixture=result=FAIL reason=selection-deadline scout_userid=%d target_a_userid=%d target_b_userid=%d firing_target_userid=%d",
			fixture.scoutUserId, fixture.targetAUserId,
			fixture.targetBUserId, fixture.firingTargetUserId);
		CBotEmpires::clearEmpiresScoutPriorityFixture("deadline");
	}
}

void CBotEmpires::alterOutgoingUserCmd(CBotCmd& command)
{
	// A bot inside (or entering/leaving) a vehicle owns the whole command.
	if (applyEmpiresVehicleCommand(command))
		return;

	// This is the final command boundary. A competing action may have left
	// stale attack or movement buttons in the generic command, but it must never
	// cancel or redirect an active Commander MOVE. Movement comes from the
	// route task's move vector.
	if (isEmpiresCommanderMoveActive())
	{
		command.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT |
			IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_JUMP | IN_SPEED | IN_WALK);
		if (logger != nullptr && engine != nullptr && gpGlobals != nullptr &&
			m_pEdict != nullptr && !m_pEdict->IsFree() &&
			gpGlobals->curtime >= m_flEmpiresCommanderMoveNextCommandDiagnosticLog)
		{
			logEmpiresCommanderMoveDiagnostic("alter-outgoing-final",
				&command, true);
			m_flEmpiresCommanderMoveNextCommandDiagnosticLog =
				gpGlobals->curtime + 0.5f;
			m_bEmpiresCommanderMoveDiagnosticCommandPending = true;
			m_iEmpiresCommanderMoveDiagnosticCommandNumber =
				command.command_number;
		}
		return;
	}

	// Everything after the walk (stage 1) belongs to the entry itself.
	if (commanderVehicleEntryActive() && m_iCommanderEntryStage != 1)
	{
		command.forwardmove = command.sidemove = command.upmove = 0.0f;
		command.buttons &= ~(IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_JUMP | IN_FORWARD | IN_BACK);
		edict_t* vehicle = m_hCommanderEntryVehicle.get();
		if (vehicle != nullptr)
			VectorAngles(CBotGlobals::entityOrigin(vehicle) - getEyePosition(), command.viewangles);
		if (m_iCommanderEntryStage == 2 && (command.buttons & IN_USE) != 0)
		{
			trace_t hit;
			if (!TraceCommanderEntryUse(m_pEdict, vehicle, getEyePosition(), hit))
				command.buttons &= ~IN_USE;
		}
		return;
	}

	const bool vehicleActionIsApproaching =
		m_eEmpiresVehicleActionState ==
			EmpiresVehicleActionState::APPROACHING_TUTORIAL_EXIT ||
		m_eEmpiresVehicleActionState ==
			EmpiresVehicleActionState::APPROACHING_FACTORY ||
		m_eEmpiresVehicleActionState ==
			EmpiresVehicleActionState::APPROACHING_VEHICLE;
	if (isEmpiresVehicleActionActive() && !vehicleActionIsApproaching)
	{
		command.forwardmove = 0.0f;
		command.sidemove = 0.0f;
		command.upmove = 0.0f;
		command.buttons &= ~(IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_JUMP |
			IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_SPEED | IN_WALK);

		if (m_eEmpiresVehicleActionState == EmpiresVehicleActionState::DRIVING)
		{
			command.forwardmove = kEmpiresVehicleDriveSpeed;
			command.buttons |= IN_FORWARD;
			command.viewangles = m_qEmpiresVehicleActionDriveAngles;
		}

		if (logger != nullptr && (command.buttons & IN_USE) != 0 &&
			(m_eEmpiresVehicleActionState ==
				EmpiresVehicleActionState::WAITING_FOR_ENTRY ||
			 m_eEmpiresVehicleActionState ==
				EmpiresVehicleActionState::WAITING_FOR_EXIT))
		{
			logger->Log(LogLevel::INFO,
				"Empires vehicle action=use-input state=%s index=%d userid=%d vehicle_index=%d buttons=0x%x path=normal-RunPlayerMove",
				m_eEmpiresVehicleActionState ==
					EmpiresVehicleActionState::WAITING_FOR_ENTRY ? "entry" : "exit",
				m_iEmpiresVehicleActionExpectedIndex,
				m_iEmpiresVehicleActionExpectedUserId,
				m_iEmpiresVehicleActionVehicleIndex, command.buttons);
		}
		return;
	}

	// The resupply action is a bounded fixture only.  Its interaction target
	// is selected by the normal E trace below; this merely makes the view input
	// deterministic so the command delivered through RunPlayerMove points at
	// the same current-team crate that the fixture placed the bot beside.
	if (isEmpiresEngineerResupplyActionActive() && m_pEdict != nullptr)
	{
		// The bounded resupply fixture owns no movement.  Enforce that ownership
		// at the final command boundary as well as in the action handler, because
		// the normal Think path may have prepared a stale movement command before
		// the fixture was armed.  This is inactive outside the explicit fixture.
		command.forwardmove = 0.0f;
		command.sidemove = 0.0f;
		command.upmove = 0.0f;
		command.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT |
			IN_MOVERIGHT);

		const Vector eye = getEyePosition();
		VectorAngles((m_vEmpiresEngineerResupplyActionSourceOrigin +
			Vector(0.0f, 0.0f, 24.0f)) - eye, command.viewangles);
		if (m_bEmpiresEngineerResupplyActionUseSubmitted &&
			(command.buttons & IN_USE) != 0 &&
			!m_bEmpiresEngineerResupplyActionUseInputLogged)
		{
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires engineer resupply=use-input outgoing index=%d userid=%d source_index=%d buttons=0x%x view=(%.2f %.2f) path=normal-RunPlayerMove",
					engine != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
					engine != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
					m_iEmpiresEngineerResupplyActionSourceIndex, command.buttons,
					command.viewangles.x, command.viewangles.y);
			m_bEmpiresEngineerResupplyActionUseInputLogged = true;
		}
	}

	if (m_flEmpiresTurretRocketUntil > 0.0f)
	{
		command.forwardmove = command.sidemove = command.upmove = 0.0f;
		command.viewangles = m_qEmpiresTurretRocketAim;
		command.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT |
			IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK | IN_ATTACK2);
		return;
	}

	// Aimed mortar actions hold the calculated view and keep the bot still
	// until the normal weapon command has been processed.
	if (isEmpiresMortarActionActive() && m_pEdict != nullptr &&
		engine != nullptr)
	{
		if (m_bEmpiresMortarAimRequested && m_bEmpiresMortarAimComputed)
		{
			command.viewangles = m_qEmpiresMortarAimAngles;
			// The aim-once fixture owns no movement.  Keep the actor at the
			// measured launch position while the normal mortar state machine waits
			// for posture, loading and attack timers.
			command.forwardmove = 0.0f;
			command.sidemove = 0.0f;
			command.upmove = 0.0f;
			command.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT |
				IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK);
			if (logger != nullptr && !m_bEmpiresMortarAimViewApplied)
			{
				logger->Log(LogLevel::INFO,
					"Empires mortar aim=view-applied index=%d userid=%d angles=(%.2f %.2f %.2f) path=normal-RunPlayerMove",
					engine->IndexOfEdict(m_pEdict),
					engine->GetPlayerUserId(m_pEdict),
					m_qEmpiresMortarAimAngles.x,
					m_qEmpiresMortarAimAngles.y,
					m_qEmpiresMortarAimAngles.z);
				m_bEmpiresMortarAimViewApplied = true;
			}
		}

		if (logger == nullptr)
			return;

		const edict_t* pWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
		const char* weaponName = pWeapon != nullptr && !pWeapon->IsFree() &&
			pWeapon->GetClassName() != nullptr ? pWeapon->GetClassName() : "<none>";
		if (m_eEmpiresMortarActionState == EmpiresMortarActionState::ATTACK_SUBMITTED &&
			(command.buttons & IN_ATTACK) != 0 &&
			!m_bEmpiresMortarActionAttackLogged)
		{
			logger->Log(LogLevel::INFO,
				"Empires mortar action=attack-input index=%d userid=%d weapon=%s buttons=0x%x path=normal-RunPlayerMove",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				weaponName, command.buttons);
			m_bEmpiresMortarActionAttackLogged = true;
		}
		else if (m_eEmpiresMortarActionState == EmpiresMortarActionState::OBSERVING_RESULT &&
			(command.buttons & IN_ATTACK) == 0 &&
			!m_bEmpiresMortarActionReleaseLogged)
		{
			logger->Log(LogLevel::INFO,
				"Empires mortar action=attack-release index=%d userid=%d weapon=%s buttons=0x%x path=normal-RunPlayerMove",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				weaponName, command.buttons);
			m_bEmpiresMortarActionReleaseLogged = true;
		}
		return;
	}

	if (isEmpiresMineActionActive() && m_pEdict != nullptr &&
		engine != nullptr)
	{
		command.forwardmove = 0.0f;
		command.sidemove = 0.0f;
		command.upmove = 0.0f;
		command.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT |
			IN_MOVERIGHT | IN_JUMP | IN_USE | IN_SPEED | IN_WALK | IN_DUCK);

		const edict_t* pWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
		const char* weaponName = pWeapon != nullptr && !pWeapon->IsFree() &&
			pWeapon->GetClassName() != nullptr ? pWeapon->GetClassName() : "<none>";
		if (logger != nullptr &&
			m_eEmpiresMineActionState == EmpiresMineActionState::ATTACK_SUBMITTED &&
			(command.buttons & IN_ATTACK) != 0 && !m_bEmpiresMineActionAttackLogged)
		{
			logger->Log(LogLevel::INFO,
				"Empires mine action=attack-input index=%d userid=%d weapon=%s buttons=0x%x path=normal-RunPlayerMove",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				weaponName, command.buttons);
			m_bEmpiresMineActionAttackLogged = true;
		}
		else if (logger != nullptr &&
			m_eEmpiresMineActionState == EmpiresMineActionState::OBSERVING_RESULT &&
			(command.buttons & IN_ATTACK) == 0 &&
			!m_bEmpiresMineActionReleaseLogged)
		{
			logger->Log(LogLevel::INFO,
				"Empires mine action=attack-release index=%d userid=%d weapon=%s buttons=0x%x path=normal-RunPlayerMove",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				weaponName, command.buttons);
			m_bEmpiresMineActionReleaseLogged = true;
		}
		return;
	}

	if (isEmpiresScoutBinocularActionActive() && m_pEdict != nullptr &&
		engine != nullptr)
	{
		command.forwardmove = 0.0f;
		command.sidemove = 0.0f;
		command.upmove = 0.0f;
		command.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT |
			IN_JUMP | IN_ATTACK2 | IN_USE | IN_SPEED | IN_WALK);

		const edict_t* pWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
		const char* weaponName = pWeapon != nullptr && !pWeapon->IsFree() &&
			pWeapon->GetClassName() != nullptr ? pWeapon->GetClassName() : "<none>";
		if (logger != nullptr &&
			m_eEmpiresScoutBinocularActionState ==
				EmpiresScoutBinocularActionState::ATTACK_SUBMITTED &&
			(command.buttons & IN_ATTACK) != 0 &&
			!m_bEmpiresScoutBinocularActionAttackLogged)
		{
			logger->Log(LogLevel::INFO,
				"Empires scout binocular action=attack-input index=%d userid=%d target_index=%d target_userid=%d weapon=%s buttons=0x%x path=normal-RunPlayerMove",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresScoutBinocularActionTargetIndex,
				m_iEmpiresScoutBinocularActionTargetUserId, weaponName,
				command.buttons);
			m_bEmpiresScoutBinocularActionAttackLogged = true;
		}
		else if (logger != nullptr &&
			m_eEmpiresScoutBinocularActionState ==
				EmpiresScoutBinocularActionState::OBSERVING_RESULT &&
			(command.buttons & IN_ATTACK) == 0 &&
			!m_bEmpiresScoutBinocularActionReleaseLogged)
		{
			logger->Log(LogLevel::INFO,
				"Empires scout binocular action=attack-release index=%d userid=%d target_index=%d target_userid=%d weapon=%s buttons=0x%x path=normal-RunPlayerMove",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				m_iEmpiresScoutBinocularActionTargetIndex,
				m_iEmpiresScoutBinocularActionTargetUserId, weaponName,
				command.buttons);
			m_bEmpiresScoutBinocularActionReleaseLogged = true;
		}
		return;
	}

	if (isEmpiresMortarProbeActive() && m_pEdict != nullptr && engine != nullptr)
	{
		// Keep the probe stationary and prevent normal task input from leaking
		// into the bounded test.  IN_DUCK is retained only for the explicit
		// posture diagnostic attempt.
		command.forwardmove = 0.0f;
		command.sidemove = 0.0f;
		command.upmove = 0.0f;
		command.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT |
			IN_JUMP | IN_ATTACK2 | IN_USE | IN_SPEED | IN_WALK);

		const bool attackPulse =
			m_eEmpiresMortarProbeState == EmpiresMortarProbeState::ATTACK_ARMED &&
			(command.buttons & IN_ATTACK) != 0;
		if (attackPulse)
		{
			const edict_t* pWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
			logger->Log(LogLevel::INFO,
				"Empires mortar probe=attack-input pulse=1 attempt=%d index=%d userid=%d weapon=%s buttons=0x%x flags=0x%x",
				m_iEmpiresMortarProbeAttempt, engine->IndexOfEdict(m_pEdict),
				engine->GetPlayerUserId(m_pEdict),
				pWeapon != nullptr && pWeapon->GetClassName() != nullptr ?
					pWeapon->GetClassName() : "<none>", command.buttons,
				CClassInterface::getPlayerFlags(m_pEdict));
			m_eEmpiresMortarProbeState = EmpiresMortarProbeState::ATTACK_SENT;
			if (m_pButtons != nullptr)
				m_pButtons->letGo(IN_ATTACK);
		}
		else if (m_eEmpiresMortarProbeState == EmpiresMortarProbeState::ATTACK_SENT)
		{
			logger->Log(LogLevel::INFO,
				"Empires mortar probe=attack-release pulse=1 index=%d userid=%d buttons_before_clear=0x%x",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				command.buttons);
			m_eEmpiresMortarProbeState = EmpiresMortarProbeState::OBSERVING_SHELL;
			m_iEmpiresMortarProbeFrames = 0;
			m_flEmpiresMortarProbeShellDeadline = gpGlobals->curtime + 4.0f;
		}

		// After the one command containing IN_ATTACK, every subsequent probe
		// command is forced neutral until shell observation completes.
		if (!attackPulse)
			command.buttons &= ~IN_ATTACK;

		return;
	}

	if (g_empireScoutPriorityFixture.active && m_pEdict != nullptr &&
		engine != nullptr)
	{
		const bool isScout =
			g_empireScoutPriorityFixture.scoutEdict == m_pEdict &&
			engine->IndexOfEdict(m_pEdict) ==
				g_empireScoutPriorityFixture.scoutIndex &&
			engine->GetPlayerUserId(m_pEdict) ==
				g_empireScoutPriorityFixture.scoutUserId;
		const bool isTargetA =
			g_empireScoutPriorityFixture.targetAEdict == m_pEdict &&
			engine->IndexOfEdict(m_pEdict) ==
				g_empireScoutPriorityFixture.targetAIndex &&
			engine->GetPlayerUserId(m_pEdict) ==
				g_empireScoutPriorityFixture.targetAUserId;
		const bool isTargetB =
			g_empireScoutPriorityFixture.targetBEdict == m_pEdict &&
			engine->IndexOfEdict(m_pEdict) ==
				g_empireScoutPriorityFixture.targetBIndex &&
			engine->GetPlayerUserId(m_pEdict) ==
				g_empireScoutPriorityFixture.targetBUserId;
		if (isScout || isTargetA || isTargetB)
		{
			if (!g_empireScoutPriorityFixture.commandHoldLogged)
			{
				g_empireScoutPriorityFixture.commandHoldLogged = true;
				fixtureLog(
					"Empires scout-priority fixture=command-hold applied=TRUE scout_userid=%d target_a_userid=%d target_b_userid=%d firing_target_userid=%d",
					g_empireScoutPriorityFixture.scoutUserId,
					g_empireScoutPriorityFixture.targetAUserId,
					g_empireScoutPriorityFixture.targetBUserId,
					g_empireScoutPriorityFixture.firingTargetUserId);
			}
			command.forwardmove = 0.0f;
			command.sidemove = 0.0f;
			command.upmove = 0.0f;
			command.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT |
				IN_MOVERIGHT | IN_JUMP | IN_DUCK | IN_SPEED | IN_WALK);

			if (isScout)
			{
				command.buttons &= ~IN_ATTACK;
				Vector aimTarget =
					(g_empireScoutPriorityFixture.targetAAnchor +
						g_empireScoutPriorityFixture.targetBAnchor) * 0.5f +
					Vector(0.0f, 0.0f, 32.0f);
				if (getEnemy() != nullptr &&
					hasSomeConditions(CONDITION_SEE_CUR_ENEMY) &&
					CBotGlobals::entityIsAlive(getEnemy()) &&
					isEnemy(getEnemy(), false) && isVisible(getEnemy()))
				{
					aimTarget = CBotGlobals::entityOrigin(getEnemy()) +
						Vector(0.0f, 0.0f, 32.0f);
				}
				Vector aimDirection = aimTarget - getEyePosition();
				if (aimDirection.LengthSqr() > 1.0f)
				{
					QAngle fixtureView;
					VectorAngles(aimDirection, fixtureView);
					CBotGlobals::fixFloatAngle(&fixtureView.x);
					CBotGlobals::fixFloatAngle(&fixtureView.y);
					fixtureView.x = clamp(fixtureView.x, -89.0f, 89.0f);
					command.viewangles = fixtureView;
				}
			}
			else
			{
				const int userId = engine->GetPlayerUserId(m_pEdict);
				command.buttons &= ~IN_ATTACK;
				if (userId == g_empireScoutPriorityFixture.firingTargetUserId)
					command.buttons |= IN_ATTACK;
				const Vector awayDirection = getOrigin() -
					g_empireScoutPriorityFixture.scoutAnchor;
				if (awayDirection.LengthSqr() > 1.0f)
				{
					QAngle fixtureView;
					VectorAngles(awayDirection, fixtureView);
					CBotGlobals::fixFloatAngle(&fixtureView.x);
					CBotGlobals::fixFloatAngle(&fixtureView.y);
					fixtureView.x = clamp(fixtureView.x, -89.0f, 89.0f);
					command.viewangles = fixtureView;
				}
			}
		}
	}

	if (!g_empireEnemyLosFixture.active || m_pEdict == nullptr || engine == nullptr)
		return;

	const bool isObserver = g_empireEnemyLosFixture.observerEdict == m_pEdict &&
		engine->IndexOfEdict(m_pEdict) == g_empireEnemyLosFixture.observerIndex &&
		engine->GetPlayerUserId(m_pEdict) == g_empireEnemyLosFixture.observerUserId;
	const bool isOpponent = g_empireEnemyLosFixture.opponentEdict == m_pEdict &&
		engine->IndexOfEdict(m_pEdict) == g_empireEnemyLosFixture.opponentIndex &&
		engine->GetPlayerUserId(m_pEdict) == g_empireEnemyLosFixture.opponentUserId;
	if ((isObserver && g_empireEnemyLosFixture.holdObserver) ||
		(isOpponent && g_empireEnemyLosFixture.holdOpponent))
	{
		if (isObserver && !g_empireEnemyLosFixture.observerCommandHoldLogged)
		{
			g_empireEnemyLosFixture.observerCommandHoldLogged = true;
			fixtureLog("Empires enemy LOS fixture=command-hold observer_applied=TRUE userid=%d index=%d",
				g_empireEnemyLosFixture.observerUserId,
				g_empireEnemyLosFixture.observerIndex);
		}
		if (isOpponent && !g_empireEnemyLosFixture.opponentCommandHoldLogged)
		{
			g_empireEnemyLosFixture.opponentCommandHoldLogged = true;
			fixtureLog("Empires enemy LOS fixture=command-hold opponent_applied=TRUE userid=%d index=%d",
				g_empireEnemyLosFixture.opponentUserId,
				g_empireEnemyLosFixture.opponentIndex);
		}
		command.forwardmove = 0.0f;
		command.sidemove = 0.0f;
		command.upmove = 0.0f;
		command.buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT |
			IN_MOVERIGHT | IN_JUMP | IN_DUCK | IN_SPEED | IN_WALK);
	}

	// The LOS fixture suppresses attack so it can observe geometry safely. The
	// opt-in combat fixture keeps attack untouched. The movement-only variant
	// suppresses both actors' attack so the held target cannot die before the
	// approach displacement threshold is observed. Both paths use normal
	// command delivery after this narrow, test-only adjustment.
	if ((isObserver || isOpponent) && (!g_empireEnemyLosFixture.combatMode ||
		g_empireEnemyLosFixture.releaseObserverForCombatMovement))
		command.buttons &= ~IN_ATTACK;

	// While the observer is deliberately held for acquisition, keep the
	// normal outgoing view directed at the fixture opponent.  CBotVisibles
	// requires the target to be inside the generic view cone before normal
	// enemy selection can acquire it; ordinary Think may otherwise turn back
	// toward a roaming waypoint between the one-shot placement and the next
	// visibility pass.  This is fixture-only and is disabled immediately when
	// the movement variant releases the observer.
	if (isObserver && g_empireEnemyLosFixture.holdObserver &&
		g_empireEnemyLosFixture.opponentEdict != nullptr &&
		CBotGlobals::entityIsValid(const_cast<edict_t *>(
			g_empireEnemyLosFixture.opponentEdict)))
	{
		const Vector aimOrigin = CBotGlobals::entityOrigin(
			const_cast<edict_t *>(g_empireEnemyLosFixture.opponentEdict)) +
			Vector(0.0f, 0.0f, 32.0f);
		Vector aimDirection = aimOrigin - getEyePosition();
		if (aimDirection.LengthSqr() > 1.0f)
		{
			QAngle fixtureView;
			VectorAngles(aimDirection, fixtureView);
			CBotGlobals::fixFloatAngle(&fixtureView.x);
			CBotGlobals::fixFloatAngle(&fixtureView.y);
			fixtureView.x = clamp(fixtureView.x, -89.0f, 89.0f);
			command.viewangles = fixtureView;
		}
	}

	// The movement variant keeps the target stationary and suppresses only the
	// target's return fire. The observer's command remains untouched after the
	// fixture releases it, so approach, aiming and attack still use normal bot
	// behavior.
	if (isOpponent && g_empireEnemyLosFixture.combatMode &&
		g_empireEnemyLosFixture.releaseObserverForCombatMovement)
		command.buttons &= ~IN_ATTACK;
}

void processEmpiresEnemyLosFixture(CBotEmpires* pCaller)
{
	EmpiresEnemyLosFixtureState& fixture = g_empireEnemyLosFixture;
	if (!fixture.active || pCaller == nullptr || gpGlobals == nullptr ||
		engine == nullptr || fixture.lastProcessTick == gpGlobals->tickcount)
		return;
	fixture.lastProcessTick = gpGlobals->tickcount;

	CBotEmpires* pObserver = fixtureBotForUserId(fixture.observerUserId);
	CBotEmpires* pOpponent = fixtureBotForUserId(fixture.opponentUserId);
	if (!fixtureActorValid(pObserver, fixture.observerUserId,
		fixture.observerIndex, fixture.observerEdict) ||
		!fixtureActorValid(pOpponent, fixture.opponentUserId,
		fixture.opponentIndex, fixture.opponentEdict))
	{
		fixtureLog("Empires enemy LOS fixture=result=FAIL reason=actor-identity-controller-or-alive-state-invalid");
		CBotEmpires::clearEmpiresEnemyLosFixture("actor-invalid");
		return;
	}

	const float now = engine->Time();
	const Vector observerOrigin = pObserver->getOrigin();
	const Vector opponentOrigin = pOpponent->getOrigin();
	if (now >= fixture.deadline &&
		fixture.phase != EmpiresEnemyLosFixturePhase::OBSERVING_LAST_KNOWN &&
		fixture.phase != EmpiresEnemyLosFixturePhase::COMBAT_OBSERVING)
	{
		fixtureLog("Empires enemy LOS fixture=result=FAIL reason=server-deadline phase=%d", static_cast<int>(fixture.phase));
		CBotEmpires::clearEmpiresEnemyLosFixture("server-deadline");
		return;
	}

	switch (fixture.phase)
	{
	case EmpiresEnemyLosFixturePhase::SETTLING:
	{
		const float observerDrift = fixtureDistance(observerOrigin, fixture.observerAnchor);
		const float opponentDrift = fixtureDistance(opponentOrigin, fixture.opponentAnchor);
		// SetAbsOrigin is intentionally performed only once. Empires/Source may
		// resolve ground contact or residual velocity over several frames, so
		// judge settling by consecutive-frame motion rather than requiring the
		// final physics origin to equal the requested waypoint origin.
		if (observerDrift > 256.0f || opponentDrift > 256.0f)
		{
			fixtureLog("Empires enemy LOS fixture=result=FAIL reason=settling-drift observer_drift=%.1f opponent_drift=%.1f",
				observerDrift, opponentDrift);
			CBotEmpires::clearEmpiresEnemyLosFixture("settling-drift");
			return;
		}

		const float observerStep = fixture.observerLastOriginKnown ?
			fixtureDistance(observerOrigin, fixture.observerLastOrigin) : 0.0f;
		const float opponentStep = fixture.opponentLastOriginKnown ?
			fixtureDistance(opponentOrigin, fixture.opponentLastOrigin) : 0.0f;
		fixture.observerLastOrigin = observerOrigin;
		fixture.observerLastOriginKnown = true;
		fixture.opponentLastOrigin = opponentOrigin;
		fixture.opponentLastOriginKnown = true;
		if (observerStep <= 3.0f && opponentStep <= 3.0f)
			++fixture.settleStableFrames;
		else
			fixture.settleStableFrames = 0;

		if (fixture.settleStableFrames >= 8)
		{
			// Physics may resolve the one-shot placement a short distance away
			// from the requested waypoint. Once the actor has been stable for the
			// required consecutive frames, subsequent hold validation must compare
			// against that verified physical anchor rather than the request point.
			fixture.observerAnchor = observerOrigin;
			fixture.phase = EmpiresEnemyLosFixturePhase::WAITING_FOR_ACQUISITION;
			fixture.deadline = now + 12.0f;
			fixtureLog("Empires enemy LOS fixture=settled stable_frames=%d observer_origin=(%.1f %.1f %.1f) opponent_origin=(%.1f %.1f %.1f)",
				fixture.settleStableFrames, observerOrigin.x, observerOrigin.y,
				observerOrigin.z, opponentOrigin.x, opponentOrigin.y,
				opponentOrigin.z);
		}
		return;
	}

	case EmpiresEnemyLosFixturePhase::WAITING_FOR_ACQUISITION:
	{
		const bool geometryVisible = CBotGlobals::isVisible(
			pObserver->getEdict(), pObserver->getEyePosition(),
			pOpponent->getEdict());
		const bool botVisible = pObserver->isVisible(pOpponent->getEdict());
		if (!fixture.visibilityDiagnosticLogged)
		{
			const Vector eyePosition = pObserver->getEyePosition();
			const Vector targetOrigin = CBotGlobals::entityOrigin(
				pOpponent->getEdict());
			Vector targetDirection = targetOrigin - eyePosition;
			const float targetDistance = targetDirection.Length();
			float viewDot = -1.0f;
			if (targetDistance > 1.0f)
			{
				targetDirection /= targetDistance;
				Vector viewForward;
				AngleVectors(CBotGlobals::playerAngles(pObserver->getEdict()),
					&viewForward);
				viewDot = DotProduct(targetDirection, viewForward);
			}
			const QAngle actualAngles = CBotGlobals::playerAngles(
				pObserver->getEdict());
			fixture.visibilityDiagnosticLogged = true;
			fixtureLog(
				"Empires enemy LOS fixture=visibility-diagnostic geometry=%d bot_visibility=%d fov=%d dot=%.3f distance=%.1f observer_angles=(%.1f %.1f %.1f) observer_origin=(%.1f %.1f %.1f) eye=(%.1f %.1f %.1f) target_origin=(%.1f %.1f %.1f)",
				geometryVisible ? 1 : 0, botVisible ? 1 : 0,
				pObserver->FInViewCone(pOpponent->getEdict()) ? 1 : 0,
				viewDot, targetDistance, actualAngles.x, actualAngles.y,
				actualAngles.z, observerOrigin.x, observerOrigin.y,
				observerOrigin.z, eyePosition.x, eyePosition.y, eyePosition.z,
				targetOrigin.x, targetOrigin.y, targetOrigin.z);
		}
		if (geometryVisible && botVisible && !fixture.initialVisibilityLogged)
		{
			fixture.initialVisibilityLogged = true;
			fixtureLog("Empires enemy LOS fixture=initial-visibility geometry=CLEAR bot_visibility=TRUE observer_userid=%d opponent_userid=%d",
				fixture.observerUserId, fixture.opponentUserId);
		}

		if (geometryVisible && botVisible &&
			pObserver->hasEmpiresFixtureEnemy(pOpponent->getEdict()))
		{
			fixture.enemyAcquiredLogged = true;
			if (fixture.combatMode)
			{
				if (fixture.releaseObserverForCombatMovement)
				{
					fixture.holdObserver = false;
					fixture.holdOpponent = true;
					fixture.observerAnchor = observerOrigin;
					fixture.observerLastOrigin = observerOrigin;
					fixture.observerLastOriginKnown = true;
					fixture.observerMovedLogged = false;
					fixtureLog("Empires enemy combat movement fixture=observer-translation=RELEASED after=enemy-acquired target-held=TRUE normal_approach=ACTIVE");
				}
				fixture.phase = EmpiresEnemyLosFixturePhase::COMBAT_OBSERVING;
				fixture.deadline = now +
					(fixture.releaseObserverForCombatMovement ? 45.0f : 90.0f);
				fixtureLog("Empires enemy combat fixture=enemy-acquired observer_userid=%d opponent_userid=%d visibility=TRUE normal_combat=ACTIVE",
					fixture.observerUserId, fixture.opponentUserId);
				return;
			}
			fixture.phase = EmpiresEnemyLosFixturePhase::WAITING_FOR_ROUTE;
			fixture.deadline = now + 12.0f;
			fixtureLog("Empires enemy LOS fixture=enemy-acquired observer_userid=%d opponent_userid=%d visibility=TRUE",
				fixture.observerUserId, fixture.opponentUserId);
			const int destinationWaypoint = fixture.requireNaturalExpiry ? 157 : 156;
			if (!pOpponent->requestNormalPathOnce(155, destinationWaypoint, false))
			{
				fixtureLog("Empires enemy LOS fixture=result=FAIL reason=opponent-route-request-rejected");
				CBotEmpires::clearEmpiresEnemyLosFixture("route-request-rejected");
			}
			else
			{
				fixture.routeRequested = true;
				fixtureLog("Empires enemy LOS fixture=opponent-route-requested route=155->%d",
					destinationWaypoint);
			}
		}
		return;
	}

	case EmpiresEnemyLosFixturePhase::WAITING_FOR_ROUTE:
	{
		if (!fixture.routeRequested)
			return;
		const char* pathState = pOpponent->normalPathStateName();
		const std::vector<int> expectedRoute = fixture.requireNaturalExpiry ?
			std::vector<int>{ 155, 156, 157 } : std::vector<int>{ 155, 156 };
		if (pOpponent->normalPathRouteMatches(expectedRoute) &&
			(std::strcmp(pathState, "ARMED") == 0 ||
				std::strcmp(pathState, "ACTIVE") == 0))
		{
			fixture.routeValidatedLogged = true;
			fixture.phase = EmpiresEnemyLosFixturePhase::TRAVERSING;
			fixture.deadline = now + 20.0f;
			fixture.opponentLastOrigin = opponentOrigin;
			fixture.opponentLastOriginKnown = true;
			fixture.holdOpponent = false;
			fixtureLog("Empires enemy LOS fixture=opponent-route-validated route=155->%d route_nodes=%d translation=RELEASED",
				fixture.requireNaturalExpiry ? 157 : 156,
				static_cast<int>(expectedRoute.size()));
		}
		else if (std::strcmp(pathState, "FAILED") == 0 ||
			std::strcmp(pathState, "COMPLETE") == 0)
		{
			fixtureLog("Empires enemy LOS fixture=result=FAIL reason=opponent-route-not-exact state=%s", pathState);
			CBotEmpires::clearEmpiresEnemyLosFixture("route-not-exact");
		}
		return;
	}

	case EmpiresEnemyLosFixturePhase::TRAVERSING:
	{
		if (fixtureDistance(observerOrigin, fixture.observerAnchor) > 48.0f)
		{
			fixtureLog("Empires enemy LOS fixture=result=FAIL reason=held-observer-drift");
			CBotEmpires::clearEmpiresEnemyLosFixture("held-observer-drift");
			return;
		}

		const float opponentDisplacement = fixtureDistance(opponentOrigin,
			fixture.opponentAnchor);
		if (opponentDisplacement > 8.0f && !fixture.opponentMovedLogged)
		{
			fixture.opponentMovedLogged = true;
				fixtureLog("Empires enemy LOS fixture=opponent-transition=observed route=155->%d displacement=%.1f physical_translation=TRUE",
					fixture.requireNaturalExpiry ? 157 : 156, opponentDisplacement);
		}

		const bool geometryVisible = CBotGlobals::isVisible(
			pObserver->getEdict(), pObserver->getEyePosition(),
			pOpponent->getEdict());
		const bool botVisible = pObserver->isVisible(pOpponent->getEdict());
		if (!geometryVisible && !fixture.geometryBlockedLogged)
		{
			fixture.geometryBlockedLogged = true;
				fixtureLog("Empires enemy LOS fixture=geometry=BLOCKED route=155->%d opponent_displacement=%.1f",
					fixture.requireNaturalExpiry ? 157 : 156, opponentDisplacement);
		}
		if (fixture.geometryBlockedLogged && !botVisible &&
			!fixture.botVisibilityLostLogged)
		{
			fixture.botVisibilityLostLogged = true;
			fixture.holdObserver = false;
			if (fixture.requireNaturalExpiry && !fixture.opponentHeldAfterOcclusionLogged)
			{
				// Move the opponent away only after the observer's normal visibility
				// check has reported LOS loss. The observer keeps its existing
				// last-known position and CFindLastEnemy search; this actor placement
				// only prevents the observer from seeing the opponent after reaching
				// that area, without changing enemy memory or search completion state.
				CWaypoint* pExpiryFixtureWaypoint = CWaypoints::getWaypoint(1);
				if (pExpiryFixtureWaypoint == nullptr || !pExpiryFixtureWaypoint->isUsed() ||
					pOpponent->getController() == nullptr)
				{
					fixtureLog("Empires enemy LOS fixture=result=FAIL reason=expiry-actor-position-unavailable");
					CBotEmpires::clearEmpiresEnemyLosFixture("expiry-actor-position-unavailable");
					return;
				}

				Vector expiryFixtureOrigin = pExpiryFixtureWaypoint->getOrigin();
				pOpponent->getController()->SetAbsOrigin(expiryFixtureOrigin);
				fixture.holdOpponent = true;
				fixture.opponentHeldAfterOcclusionLogged = true;
				fixtureLog("Empires enemy LOS fixture=opponent-position=RELOCATED_AFTER_LOS_LOSS waypoint=1 origin=(%.1f %.1f %.1f) normal_think=ACTIVE reason=prevent-reacquisition",
					expiryFixtureOrigin.x, expiryFixtureOrigin.y, expiryFixtureOrigin.z);
			}
			fixture.phase = EmpiresEnemyLosFixturePhase::OBSERVING_LAST_KNOWN;
			fixture.deadline = now + 10.0f;
			fixtureLog("Empires enemy LOS fixture=bot-visibility=LOST observer_translation=RELEASED natural_last_known=ENABLED");
		}

		const char* pathState = pOpponent->normalPathStateName();
		if (std::strcmp(pathState, "FAILED") == 0 && !fixture.geometryBlockedLogged)
		{
			fixtureLog("Empires enemy LOS fixture=result=FAIL reason=opponent-steering-failed-before-occlusion moved=%d",
				fixture.opponentMovedLogged ? 1 : 0);
			CBotEmpires::clearEmpiresEnemyLosFixture("opponent-steering-failed");
		}
		else if (std::strcmp(pathState, "COMPLETE") == 0 &&
			!fixture.geometryBlockedLogged)
		{
			fixtureLog("Empires enemy LOS fixture=result=FAIL reason=route-completed-without-geometry-block moved=%d",
				fixture.opponentMovedLogged ? 1 : 0);
			CBotEmpires::clearEmpiresEnemyLosFixture("route-completed-without-occlusion");
		}
		return;
	}

	case EmpiresEnemyLosFixturePhase::OBSERVING_LAST_KNOWN:
	{
		if (pObserver->hasEmpiresFixtureEnemy(pOpponent->getEdict()))
		{
			if (fixture.requireNaturalExpiry)
			{
				fixtureLog("Empires enemy LOS fixture=result=FAIL reason=unexpected-reacquisition mode=NATURAL_EXPIRY");
				CBotEmpires::clearEmpiresEnemyLosFixture("unexpected-reacquisition");
				return;
			}
			fixtureLog("Empires enemy LOS fixture=complete last_known_result=REACQUIRED");
			CBotEmpires::clearEmpiresEnemyLosFixture("reacquired");
			return;
		}
		if (now >= fixture.deadline)
		{
			fixtureLog("Empires enemy LOS fixture=complete last_known_result=%s existing_search=expected reacquired=0",
				fixture.requireNaturalExpiry ? "EXPIRED_NATURALLY" : "OBSERVED_UNTIL_DEADLINE");
			CBotEmpires::clearEmpiresEnemyLosFixture("observation-deadline");
		}
		return;
	}

	case EmpiresEnemyLosFixturePhase::COMBAT_OBSERVING:
		if (fixture.releaseObserverForCombatMovement)
		{
			const float observerDisplacement = fixtureDistance(
				observerOrigin, fixture.observerAnchor);
			if (observerDisplacement > 8.0f && !fixture.observerMovedLogged)
			{
				fixture.observerMovedLogged = true;
				fixtureLog("Empires enemy combat movement fixture=observer-transition=observed displacement=%.1f physical_translation=TRUE",
					observerDisplacement);
			}
			if (observerDisplacement >= 96.0f)
			{
				fixtureLog("Empires enemy combat movement fixture=complete result=APPROACH_MOVED displacement=%.1f target_held=TRUE normal_combat=ACTIVE",
					observerDisplacement);
				CBotEmpires::clearEmpiresEnemyLosFixture("combat-movement-complete");
				return;
			}
		}
		if (now >= fixture.deadline)
		{
			fixtureLog("Empires enemy combat fixture=complete result=%s observer_moved=%d",
				fixture.releaseObserverForCombatMovement ? "APPROACH_MOVEMENT_DEADLINE" : "OBSERVATION_DEADLINE",
				fixture.observerMovedLogged ? 1 : 0);
			CBotEmpires::clearEmpiresEnemyLosFixture("combat-observation-deadline");
		}
		return;

	case EmpiresEnemyLosFixturePhase::INACTIVE:
		return;
	}
}

void CBotEmpires::continueNormalPathSearch(const bool restart)
{
	CWaypointNavigator* pNavigator = static_cast<CWaypointNavigator*>(m_pNavigator);
	CWaypoint* pDestination = CWaypoints::getWaypoint(m_iNormalPathDestinationWaypoint);
	if (pNavigator == nullptr || pDestination == nullptr ||
		m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		m_pController == nullptr || engine == nullptr ||
		engine->IndexOfEdict(m_pEdict) != m_iNormalPathExpectedIndex ||
		engine->GetPlayerUserId(m_pEdict) != m_iNormalPathExpectedUserId ||
		m_pEdict != m_pNormalPathExpectedEdict || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0)
	{
		if (pNavigator != nullptr)
			pNavigator->clear();
		m_NormalPathRoute.clear();
		m_eNormalPathState = EmpiresNormalPathState::FAILED;
		logger->Log(LogLevel::ERROR,
			"Empires normal path_once result=PATH_SEARCH_FAILED reason=client identity, controller, destination or alive state changed search_frames=%d",
			m_iNormalPathSearchFrames);
		return;
	}

	bool searchFailed = false;
	const bool searchFinished = pNavigator->workRoute(
		m_vNormalPathStartOrigin, m_vNormalPathDestinationOrigin, &searchFailed,
		restart, true, m_iNormalPathDestinationWaypoint, 0, -1);
	++m_iNormalPathSearchFrames;
	m_bNormalPathSearchStarted = true;

	if (!searchFinished)
	{
		logger->Log(LogLevel::INFO,
			"Empires normal path_once search-pending start=%d destination=%d search_frame=%d search_finished=0 search_failed=0",
			m_iNormalPathStartWaypoint, m_iNormalPathDestinationWaypoint,
			m_iNormalPathSearchFrames);
		if (m_iNormalPathSearchFrames >= kNormalPathSearchMaximumFrames)
		{
			pNavigator->clear();
			m_NormalPathRoute.clear();
			m_eNormalPathState = EmpiresNormalPathState::FAILED;
			logger->Log(LogLevel::ERROR,
				"Empires normal path_once result=PATH_SEARCH_FAILED reason=bounded search timeout start=%d destination=%d search_frames=%d",
				m_iNormalPathStartWaypoint, m_iNormalPathDestinationWaypoint,
				m_iNormalPathSearchFrames);
		}
		return;
	}

	if (searchFailed || !pNavigator->routeFound() ||
		!pNavigator->copyRoute(&m_NormalPathRoute) || m_NormalPathRoute.empty() ||
		m_NormalPathRoute.front() != m_iNormalPathStartWaypoint ||
		m_NormalPathRoute.back() != m_iNormalPathDestinationWaypoint)
	{
		logger->Log(LogLevel::ERROR,
			"Empires normal path_once result=PATH_SEARCH_FAILED start=%d destination=%d route_nodes=%d search_finished=1 search_failed=%d route_found=%d search_frames=%d",
			m_iNormalPathStartWaypoint, m_iNormalPathDestinationWaypoint,
			static_cast<int>(m_NormalPathRoute.size()), searchFailed ? 1 : 0,
			pNavigator->routeFound() ? 1 : 0, m_iNormalPathSearchFrames);
		m_NormalPathRoute.clear();
		m_eNormalPathState = EmpiresNormalPathState::FAILED;
		return;
	}

	m_eNormalPathState = EmpiresNormalPathState::ARMED;
	m_iNormalPathLastWaypoint = m_NormalPathRoute.front();
	m_iNormalPathActiveFrames = 0;
	m_iNormalPathNoProgressFrames = 0;
	m_flNormalPathStartTime = engine->Time();
	m_flNormalPathDeadline = m_flNormalPathStartTime + kNormalPathMaximumSeconds;
	m_vNormalPathLastOrigin = m_vNormalPathStartOrigin;
	m_bNormalPathLastOriginKnown = true;

	logger->Log(LogLevel::INFO,
		"Empires normal path_once route-found=1 start=%d destination=%d route_nodes=%d search_frames=%d index=%d userid=%d start_origin=(%.1f %.1f %.1f)",
		m_iNormalPathStartWaypoint, m_iNormalPathDestinationWaypoint,
		static_cast<int>(m_NormalPathRoute.size()), m_iNormalPathSearchFrames,
		m_iNormalPathExpectedIndex, m_iNormalPathExpectedUserId,
		m_vNormalPathStartOrigin.x, m_vNormalPathStartOrigin.y,
		m_vNormalPathStartOrigin.z);
	for (int i = 0; i < static_cast<int>(m_NormalPathRoute.size()); ++i)
	{
		logger->Log(LogLevel::INFO,
			"Empires normal path_once route-node index=%d waypoint=%d",
			i, m_NormalPathRoute[i]);
	}
	logger->Log(LogLevel::INFO,
		"Empires normal path_once armed state=%s input=normal_CBot_think_and_runPlayerMove exactly_once=1",
		normalPathStateName());
}

bool CBotEmpires::requestNormalPathOnce(const int startWaypoint,
	const int destinationWaypoint, const bool terminalAfterStop)
{
	if (m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr || m_pController == nullptr ||
		engine == nullptr || gpGlobals == nullptr || m_pNavigator == nullptr)
	{
		logger->Log(LogLevel::WARN,
			"Empires normal path_once rejected: normal bot, player info, controller, navigator or engine state unavailable");
		return false;
	}

	if (m_eNormalSpawnState != EmpiresNormalSpawnState::COMPLETE ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0)
	{
		logger->Log(LogLevel::WARN,
			"Empires normal path_once rejected: normal Empires spawn is not complete or bot is not alive state=%s dead=%d health=%d",
			normalPathStateName(), m_pPlayerInfo->IsDead() ? 1 : 0,
			m_pPlayerInfo->GetHealth());
		return false;
	}

	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
	{
		logger->Log(LogLevel::WARN,
			"Empires normal path_once rejected: unsupported team=%d", team);
		return false;
	}

	const bool explicitStart = startWaypoint >= 0;
	int selectedDestination = destinationWaypoint >= 0 ? destinationWaypoint :
		kNormalPathDestinationWaypoint;
	const int nearestStart = CWaypointLocations::NearestWaypoint(
		getOrigin(), CWaypointLocations::REACHABLE_RANGE, -1, true, false,
		true, nullptr, false, team);
	const int selectedStart = explicitStart ? startWaypoint : nearestStart;
	if (!explicitStart && destinationWaypoint < 0 &&
		selectedStart == selectedDestination)
	{
		selectedDestination = selectedDestination == kNormalPathDestinationWaypoint ?
			kNormalPathStartWaypoint : kNormalPathDestinationWaypoint;
		logger->Log(LogLevel::INFO,
			"Empires normal path_once adjusted default destination=%d because start=%d; alternate=%d",
			kNormalPathDestinationWaypoint, selectedStart, selectedDestination);
	}
	logger->Log(LogLevel::INFO,
		"Empires normal path_once request origin=(%.1f %.1f %.1f) team=%d requested_start=%d selected_start=%d requested_destination=%d selected_destination=%d nearest_waypoint=%d",
		getOrigin().x, getOrigin().y, getOrigin().z, team, startWaypoint,
		selectedStart, destinationWaypoint, selectedDestination, nearestStart);
	CWaypoint* pStart = CWaypoints::getWaypoint(selectedStart);
	CWaypoint* pDestination = CWaypoints::getWaypoint(selectedDestination);
	if (pStart == nullptr || pDestination == nullptr || !pStart->isUsed() ||
		!pDestination->isUsed() || !pStart->forTeam(team) ||
		!pDestination->forTeam(team) || selectedStart == selectedDestination)
	{
		logger->Log(LogLevel::WARN,
			"Empires normal path_once rejected: unsuitable fixed route start=%d destination=%d team=%d",
			selectedStart, selectedDestination, team);
		return false;
	}

	CWaypointNavigator* pNavigator = static_cast<CWaypointNavigator*>(m_pNavigator);
	if (pNavigator == nullptr)
		return false;

	// Retire the prior attempt before freeing the route-once schedule, so
	// CBot::think's ownership guard cannot cancel the new waypoint cursor.
	cancelNavigationAttempt("normal-path-once-replaced");
	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	pNavigator->clear();
	pNavigator->setRouteStartAnchor(selectedStart);
	m_bNormalPathTerminalAfterStop = terminalAfterStop;
	m_pEmpiresApproachTarget = nullptr;
	m_bEmpiresApproachTaskIssued = false;

	m_iNormalPathStartWaypoint = selectedStart;
	m_iNormalPathDestinationWaypoint = selectedDestination;
	m_iNormalPathExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iNormalPathExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pNormalPathExpectedEdict = m_pEdict;
	// An explicit start waypoint is part of the path contract. Feed its
	// origin to the existing navigator so physics settling of a fixture actor
	// cannot make A* silently select a different nearest start node.
	m_vNormalPathStartOrigin = explicitStart ? pStart->getOrigin() : getOrigin();
	m_vNormalPathDestinationOrigin = pDestination->getOrigin();
	m_iNormalPathSearchFrames = 0;
	m_bNormalPathSearchStarted = false;
	m_eNormalPathState = EmpiresNormalPathState::SEARCHING;
	continueNormalPathSearch(true);
	return m_eNormalPathState == EmpiresNormalPathState::SEARCHING ||
		m_eNormalPathState == EmpiresNormalPathState::ARMED;
}

bool CBotEmpires::readEmpiresCommanderMove(Vector& target, bool& synthetic,
	bool& expired, int& commandType) const
{
	target = vec3_origin;
	synthetic = false;
	expired = false;
	commandType = -1;

	if (m_bEmpiresCommanderMoveTestActive)
	{
		target = m_vEmpiresCommanderMoveTestTarget;
		synthetic = true;
		commandType = kEmpiresCommanderMoveType;
		return target.IsValid();
	}

	if (m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		!m_pPlayerInfo->IsFakeClient() || engine == nullptr)
		return false;

	if (!readEmpiresIntProperty(m_pEdict, "m_iCommandType", commandType) ||
		commandType != kEmpiresCommanderMoveType ||
		!readEmpiresEntityVectorProperty(m_pEdict, "m_vCommandLocation", target) ||
		!target.IsValid())
	{
		return false;
	}

	float cancelTime = 0.0f;
	if (readEmpiresFloatProperty(m_pEdict, "m_flOrderCancelTime", cancelTime) &&
		cancelTime > 0.0f && cancelTime <= engine->Time())
		expired = true;

	return true;
}

void CBotEmpires::onNavigationIntentStarted(
	const RCBotNavigation::NavigationIntent& intent)
{
	if (m_bEmpiresSquadSpatialChildActive &&
		intent.goalId == static_cast<int>(
			m_EmpiresSquadSpatialChildExecution.orderId) &&
		intent.goalOrigin.x == m_vEmpiresSquadSpatialTarget.x &&
		intent.goalOrigin.y == m_vEmpiresSquadSpatialTarget.y &&
		intent.goalOrigin.z == m_vEmpiresSquadSpatialTarget.z)
	{
		if (g_empireSquadMoveRecipient.ObserveNavigationStarted(
			m_EmpiresSquadSpatialParentOrder,
			m_EmpiresSquadSpatialChildExecution,
			m_iEmpiresSquadSpatialMemberId,
			m_iEmpiresSquadSpatialRequestToken, intent.id))
		{
			m_iEmpiresSquadSpatialNavigationIntentId = intent.id;
			RCBotSquadOrder::NavigationIntentRequest request;
			request.parentOrder = m_EmpiresSquadSpatialParentOrder;
			request.childExecution = m_EmpiresSquadSpatialChildExecution;
			request.memberId = m_iEmpiresSquadSpatialMemberId;
			request.target = { m_vEmpiresSquadSpatialTarget.x,
				m_vEmpiresSquadSpatialTarget.y,
				m_vEmpiresSquadSpatialTarget.z };
			request.slotId = m_iEmpiresSquadSpatialSlotId;
			request.squadGeneration = m_iEmpiresSquadSpatialGeneration;
			request.decisionRevision = m_iEmpiresSquadSpatialDecisionRevision;
			request.formationGeneration =
				m_iEmpiresSquadSpatialFormationGeneration;
			request.claimResourceKind = m_iEmpiresSquadSpatialClaimKind;
			request.claimResourceId = m_iEmpiresSquadSpatialClaimId;
			request.claimResourceGeneration =
				m_iEmpiresSquadSpatialClaimGeneration;
			RecordEmpiresSquadSpatialMoveEvent(this,
				"SQUAD_SPATIAL_MOVE_STARTED", request, intent.id);
		}
		return;
	}
	if (!m_commanderMoveShadow.HasActiveOrder() ||
		!m_bEmpiresCommanderMoveRouteIssued)
		return;

	const RCBotOrder::OrderKey order = m_commanderMoveShadow.ActiveKey();
	if (intent.goalId != static_cast<int>(order.orderId) ||
		intent.goalOrigin != m_vEmpiresCommanderMoveTarget)
		return;

	m_iEmpiresCommanderMoveNavigationIntentId = intent.id;
	const double now = engine != nullptr ? engine->Time() : 0.0;
	m_commanderMoveShadow.ObservePlanning(now,
		CommanderNavigationContext(intent));
}

void CBotEmpires::onNavigationAttemptStarted(
	const RCBotNavigation::NavigationIntent& intent)
{
	if (m_bEmpiresSquadSpatialChildActive &&
		intent.id == m_iEmpiresSquadSpatialNavigationIntentId)
	{
		RCBotSquadOrder::NavigationIntentRequest request;
		request.parentOrder = m_EmpiresSquadSpatialParentOrder;
		request.childExecution = m_EmpiresSquadSpatialChildExecution;
		request.memberId = m_iEmpiresSquadSpatialMemberId;
		request.target = { m_vEmpiresSquadSpatialTarget.x,
			m_vEmpiresSquadSpatialTarget.y,
			m_vEmpiresSquadSpatialTarget.z };
		request.slotId = m_iEmpiresSquadSpatialSlotId;
		request.squadGeneration = m_iEmpiresSquadSpatialGeneration;
		request.decisionRevision = m_iEmpiresSquadSpatialDecisionRevision;
		request.formationGeneration = m_iEmpiresSquadSpatialFormationGeneration;
		request.claimResourceKind = m_iEmpiresSquadSpatialClaimKind;
		request.claimResourceId = m_iEmpiresSquadSpatialClaimId;
		request.claimResourceGeneration =
			m_iEmpiresSquadSpatialClaimGeneration;
		RecordEmpiresSquadSpatialMoveEvent(this,
			"SQUAD_SPATIAL_MOVE_ATTEMPT", request, intent.id,
			intent.attemptId, RCBotOrderTelemetry::NavigationProviderName(
				CommanderNavigationProvider(intent.provider)));
		return;
	}
	if (!m_commanderMoveShadow.HasActiveOrder() ||
		intent.id != m_iEmpiresCommanderMoveNavigationIntentId)
		return;

	const RCBotOrder::OrderKey order = m_commanderMoveShadow.ActiveKey();
	if (intent.goalId != static_cast<int>(order.orderId) ||
		intent.goalOrigin != m_vEmpiresCommanderMoveTarget)
		return;

	const double now = engine != nullptr ? engine->Time() : 0.0;
	m_commanderMoveShadow.ObserveMovementStarted(now,
		CommanderNavigationContext(intent));
	if (logger != nullptr && engine != nullptr && m_pEdict != nullptr &&
		!m_pEdict->IsFree())
	{
		logger->Log(LogLevel::INFO,
			"Empires commander-move=movement-started owner_userid=%d order_id=%llu generation=%llu navigation_intent_id=%llu navigation_attempt_id=%llu provider=%s",
			engine->GetPlayerUserId(m_pEdict),
			static_cast<unsigned long long>(order.orderId),
			static_cast<unsigned long long>(order.generation),
			static_cast<unsigned long long>(intent.id),
			static_cast<unsigned long long>(intent.attemptId),
			RCBotOrderTelemetry::NavigationProviderName(
				CommanderNavigationProvider(intent.provider)));
	}
}

void CBotEmpires::onNavigationIntentTerminal(
	const RCBotNavigation::NavigationIntent& intent,
	const RCBotNavigation::Result result, const char* reason)
{
	if (m_bEmpiresSquadSpatialChildActive &&
		intent.id == m_iEmpiresSquadSpatialNavigationIntentId &&
		intent.goalId == static_cast<int>(
			m_EmpiresSquadSpatialChildExecution.orderId) &&
		intent.goalOrigin.x == m_vEmpiresSquadSpatialTarget.x &&
		intent.goalOrigin.y == m_vEmpiresSquadSpatialTarget.y &&
		intent.goalOrigin.z == m_vEmpiresSquadSpatialTarget.z)
	{
		RCBotSquadOrder::NavigationIntentRequest request;
		request.parentOrder = m_EmpiresSquadSpatialParentOrder;
		request.childExecution = m_EmpiresSquadSpatialChildExecution;
		request.memberId = m_iEmpiresSquadSpatialMemberId;
		request.target = { m_vEmpiresSquadSpatialTarget.x,
			m_vEmpiresSquadSpatialTarget.y,
			m_vEmpiresSquadSpatialTarget.z };
		request.slotId = m_iEmpiresSquadSpatialSlotId;
		request.squadGeneration = m_iEmpiresSquadSpatialGeneration;
		request.decisionRevision = m_iEmpiresSquadSpatialDecisionRevision;
		request.formationGeneration = m_iEmpiresSquadSpatialFormationGeneration;
		request.claimResourceKind = m_iEmpiresSquadSpatialClaimKind;
		request.claimResourceId = m_iEmpiresSquadSpatialClaimId;
		request.claimResourceGeneration =
			m_iEmpiresSquadSpatialClaimGeneration;
		const auto terminal = CommanderNavigationResult(result);
		const char* outcome = "ABORTED";
		switch (terminal)
		{
		case RCBotOrderTelemetry::NavigationTerminalResult::COMPLETE:
			outcome = "COMPLETE";
			break;
		case RCBotOrderTelemetry::NavigationTerminalResult::PATH_STUCK:
			outcome = "STUCK";
			break;
		case RCBotOrderTelemetry::NavigationTerminalResult::UNREACHABLE:
			outcome = "UNREACHABLE";
			break;
		case RCBotOrderTelemetry::NavigationTerminalResult::PATH_FAILED:
			outcome = "BLOCKED";
			break;
		case RCBotOrderTelemetry::NavigationTerminalResult::CANCELLED:
		case RCBotOrderTelemetry::NavigationTerminalResult::NONE:
			break;
		}
		(void)g_empireSquadMoveRecipient.ObserveNavigationTerminal(
			request.parentOrder, request.childExecution, request.memberId,
			intent.id, terminal, engine != nullptr ? engine->Time() : 0.0);
		RecordEmpiresSquadSpatialMoveEvent(this,
			"SQUAD_SPATIAL_MOVE_TERMINAL", request, intent.id,
			intent.attemptId, RCBotOrderTelemetry::NavigationProviderName(
				CommanderNavigationProvider(intent.provider)),
			RCBotOrderTelemetry::NavigationTerminalResultName(terminal), outcome);
		m_bEmpiresSquadSpatialChildActive = false;
		m_iEmpiresSquadSpatialNavigationIntentId = 0;
		m_iEmpiresSquadSpatialRequestToken = 0;
		return;
	}
	if (!m_commanderMoveShadow.HasActiveOrder() ||
		intent.id != m_iEmpiresCommanderMoveNavigationIntentId)
		return;

	const RCBotOrder::OrderKey order = m_commanderMoveShadow.ActiveKey();
	if (intent.goalId != static_cast<int>(order.orderId) ||
		intent.goalOrigin != m_vEmpiresCommanderMoveTarget)
		return;

	const double now = engine != nullptr ? engine->Time() : 0.0;
	const RCBotOrderTelemetry::RuntimeContext context =
		CommanderNavigationContext(intent, result);
	bool terminalApplied = false;
	if (result == RCBotNavigation::Result::CANCELLED &&
		CommanderMoveAbortReason(reason) ==
			RCBotOrder::DetailReason::DEATH_OR_LIFECYCLE)
	{
		terminalApplied = m_commanderMoveShadow.ObserveAbort(
			RCBotOrder::DetailReason::DEATH_OR_LIFECYCLE, now, context);
	}
	else
	{
		terminalApplied = m_commanderMoveShadow.ObserveNavigationTerminal(context, now);
	}
	if (!terminalApplied)
		return;

	Msg("[RCBOT2] commander-move result=%s reason=%s retries=%d\n",
		result == RCBotNavigation::Result::COMPLETE ? "COMPLETE" : "FAIL",
		reason != nullptr ? reason : "-", m_iEmpiresCommanderMoveRetries);
	if (result == RCBotNavigation::Result::COMPLETE)
	{
		m_bEmpiresCommanderMoveFinished = true;
		m_bEmpiresCommanderMoveFailed = false;
		EmpiresAnalysis::RecordCommanderEvent(this,
			"COMMANDER_ORDER_COMPLETED", m_vEmpiresCommanderMoveTarget,
			"navigation-complete");
	}
	else
	{
		m_bEmpiresCommanderMoveFailed = true;
		// Only a route that got stuck is worth another try; an unreachable
		// goal fails the same way from anywhere.
		m_bEmpiresCommanderMoveStuck = result == RCBotNavigation::Result::PATH_STUCK;
	}
	if (m_bEmpiresCommanderMoveTestActive)
		m_bEmpiresCommanderMoveTestActive = false;
}

void CBotEmpires::clearEmpiresCommanderMoveState(const char* reason)
{
	if (!m_bEmpiresCommanderMoveRouteIssued && !m_bEmpiresCommanderMoveTestActive)
		return;
	logEmpiresCommanderMoveDiagnostic("state-clear-before-cleanup", nullptr, true);
	if (!m_bEmpiresCommanderMoveFinished && !m_bEmpiresCommanderMoveFailed)
	{
		const Vector& target = m_bEmpiresCommanderMoveRouteIssued ?
			m_vEmpiresCommanderMoveTarget : m_vEmpiresCommanderMoveTestTarget;
		if (target.IsValid())
			EmpiresAnalysis::RecordCommanderEvent(this,
				"COMMANDER_ORDER_CANCELLED", target, reason);
	}
	if (hasActiveNavigationIntent())
		cancelNavigationAttempt(reason != nullptr ? reason : "commander-cleared");
	if (m_commanderMoveShadow.HasActiveOrder())
	{
		const double now = engine != nullptr ? engine->Time() : 0.0;
		m_commanderMoveShadow.ObserveAbort(
			CommanderMoveAbortReason(reason), now);
	}

	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
	{
		m_pNavigator->clear();
		m_pNavigator->clearCurrentPoint();
	}

	if (logger != nullptr && engine != nullptr && m_pEdict != nullptr &&
		!m_pEdict->IsFree())
	{
		logger->Log(LogLevel::INFO,
			"Empires commander-move=cleared owner_userid=%d reason=%s",
			engine->GetPlayerUserId(m_pEdict), reason != nullptr ? reason : "unknown");
	}

	m_bEmpiresCommanderMoveTestActive = false;
	m_vEmpiresCommanderMoveTestTarget = vec3_origin;
	m_bEmpiresCommanderMoveRouteIssued = false;
	m_bEmpiresCommanderMoveFinished = false;
	m_bEmpiresCommanderMoveFailed = false;
	m_vEmpiresCommanderMoveTarget = vec3_origin;
	m_iEmpiresCommanderMoveGeneration = 0;
	m_iEmpiresCommanderMoveNavigationIntentId = 0;
	m_vEmpiresCommanderMovePreviousTarget = vec3_origin;
	m_flEmpiresCommanderMoveNextDiagnosticLog = 0.0f;
	m_flEmpiresCommanderMoveNextCommandDiagnosticLog = 0.0f;
	m_bEmpiresCommanderMoveDiagnosticCommandPending = false;
	m_iEmpiresCommanderMoveDiagnosticCommandNumber = -1;
	logEmpiresCommanderMoveDiagnostic("state-clear-after-cleanup", nullptr, true);
}

bool CBotEmpires::requestEmpiresCommanderMoveTest(const Vector& target)
{
	if (m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr || m_pController == nullptr ||
		engine == nullptr || gpGlobals == nullptr ||
		!m_pPlayerInfo->IsFakeClient() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0 || !target.IsValid())
	{
		return false;
	}

	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	m_bEmpiresCommanderMoveTestActive = true;
	m_vEmpiresCommanderMoveTestTarget = target;
	return true;
}

bool CBotEmpires::requestEmpiresSquadMoveTestFixture(const int team,
	const int squadId, const Vector& target)
{
	if (m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		!m_pPlayerInfo->IsConnected() || !m_pPlayerInfo->IsFakeClient() ||
		m_pPlayerInfo->IsObserver() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0 || engine == nullptr ||
		playerinfomanager == nullptr || gpGlobals == nullptr ||
		(team != EMP_TEAM_NF && team != EMP_TEAM_BE) || squadId <= 0 ||
		squadId > static_cast<int>(
			RCBotEmpiresSquadRuntime::Adapter::kMaxSquadsPerTeam) ||
		!target.IsValid() || m_pPlayerInfo->GetTeamIndex() != team)
	{
		return false;
	}

	int ownSquad = -1;
	int ownPosition = -1;
	if (!readEmpiresEntityIntProperty(m_pEdict, "m_iSquad", ownSquad) ||
		!readEmpiresEntityIntProperty(m_pEdict, "m_iSquadPosition", ownPosition) ||
		ownSquad != squadId || ownPosition != 0)
	{
		return false;
	}

	std::vector<RCBotOrder::RecipientId> memberIds;
	std::vector<int> memberPositions;
	int leaderCount = 0;
	for (int index = 1; index <= gpGlobals->maxClients; ++index)
	{
		edict_t* pPlayer = engine->PEntityOfEntIndex(index);
		IPlayerInfo* pInfo = pPlayer != nullptr && !pPlayer->IsFree() ?
			playerinfomanager->GetPlayerInfo(pPlayer) : nullptr;
		if (pInfo == nullptr || !pInfo->IsConnected() ||
			pInfo->GetTeamIndex() != team)
		{
			continue;
		}

		int observedSquad = -1;
		if (!readEmpiresEntityIntProperty(pPlayer, "m_iSquad", observedSquad))
			return false;
		if (observedSquad != squadId)
			continue;

		int position = -1;
		if (!pInfo->IsFakeClient() || pInfo->IsObserver() || pInfo->IsDead() ||
			pInfo->GetHealth() <= 0 ||
			!readEmpiresEntityIntProperty(pPlayer, "m_iSquadPosition", position) ||
			position < 0)
		{
			return false;
		}

		CBot* memberBot = CBots::getBotPointer(pPlayer);
		const int userId = engine->GetPlayerUserId(pPlayer);
		if (memberBot == nullptr || memberBot->isControlledTestOnly() || userId <= 0)
			return false;

		memberIds.push_back(static_cast<RCBotOrder::RecipientId>(userId));
		memberPositions.push_back(position);
		if (position == 0)
		{
			++leaderCount;
			if (memberBot != this)
				return false;
		}
	}

	if (memberIds.size() < 2 || memberIds.size() > 64 || leaderCount != 1)
		return false;
	std::sort(memberPositions.begin(), memberPositions.end());
	if (std::adjacent_find(memberPositions.begin(), memberPositions.end()) !=
		memberPositions.end())
	{
		return false;
	}

	const double now = engine->Time();
	if (!std::isfinite(now))
		return false;
	RCBotCommanderMoveTestFixture::Request request;
	request.mode = RCBotCommanderMoveTestFixture::Mode::SQUAD;
	request.team = team;
	request.squadId = static_cast<RCBotOrder::RecipientId>(squadId);
	request.target = { target.x, target.y, target.z };
	g_empireSquadMoveRecipient.SetNavigationSinks(
		StartEmpiresSquadSpatialNavigation,
		CancelEmpiresSquadSpatialNavigation);
	g_empireSquadMoveRecipient.SetTelemetrySink(
		RecordCommanderMoveShadowTelemetry, this);
	RCBotSquadOrder::SquadMembershipObservation membership;
	const RCBotOrder::OrderId orderId =
		++g_nextEmpiresSquadMoveOrderId;
	const RCBotOrder::Generation orderGeneration =
		++g_nextEmpiresSquadMoveOrderGeneration;
	if (!RCBotCommanderMoveTestFixture::IssueSquadMove(request, memberIds,
		g_empireSquadMoveIdentityTracker, g_empireSquadMoveRecipient,
		orderId, orderGeneration, kEmpiresCommanderIssuerId, now, now + 300.0,
		&membership))
	{
		return false;
	}

	const RCBotOrder::OrderRecord parent =
		g_empireSquadMoveRecipient.ParentSnapshot();
	std::string memberList;
	for (const RCBotOrder::RecipientId memberId : membership.members)
	{
		if (!memberList.empty())
			memberList += ",";
		memberList += std::to_string(memberId);
	}
	const int leaderUserId = engine->GetPlayerUserId(m_pEdict);
	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires commander squad move test issued team=%d squad_id=%d squad_recipient_id=%llu squad_generation=%llu parent_order_id=%llu parent_generation=%llu leader_userid=%d member_userids=%s target=(%.1f %.1f %.1f) navigation_sink=normal-auto-spatial-slots",
			team, squadId,
			static_cast<unsigned long long>(membership.recipient.id),
			static_cast<unsigned long long>(membership.recipient.generation),
			static_cast<unsigned long long>(parent.orderId),
			static_cast<unsigned long long>(parent.generation), leaderUserId,
			memberList.c_str(), target.x, target.y, target.z);
	}
	return true;
}

bool CBotEmpires::requestEmpiresRecoveryCaptureAcceptance(const char* phase)
{
	if (phase == nullptr ||
		(std::strcmp(phase, "prepare") != 0 &&
			std::strcmp(phase, "progress") != 0 &&
			std::strcmp(phase, "select") != 0) ||
		m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		!m_pPlayerInfo->IsFakeClient() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0 || m_pSchedules == nullptr ||
		engine == nullptr || gpGlobals == nullptr)
	{
		return false;
	}

	CBotMod* pCurrentMod = CBotGlobals::getCurrentMod();
	if (pCurrentMod == nullptr || pCurrentMod->getBotType() != BOTTYPE_EMPIRES)
		return false;
	const CEmpiresMod* pEmpiresMod = static_cast<const CEmpiresMod*>(pCurrentMod);
	if (pEmpiresMod->getCapturePoints().empty())
		return false;

	const auto clearCaptureAttempt = [this]()
	{
		if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
			m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
		m_bEmpiresCaptureApproachIssued = false;
		m_bEmpiresCaptureHoldIssued = false;
		m_iEmpiresCaptureApproachPoint = -1;
		m_iEmpiresCaptureRouteWaypoint = -1;
		m_iEmpiresCaptureApproachRetries = 0;
		m_captureInterruptionLatch.reset();
		clearEmpiresStartAnchorRecovery();
		m_flEmpiresCaptureRouteIssuedTime = -1.0f;
		m_flEmpiresCaptureHoldDeadline = 0.0f;
		m_flEmpiresNextCaptureApproachTime = 0.0f;
		m_flEmpiresNextCaptureApproachProgressLogTime = 0.0f;
		m_iEmpiresCaptureRejectedPoint = -1;
		m_flEmpiresCaptureRejectedUntil = 0.0f;
	};

	if (std::strcmp(phase, "select") == 0)
	{
		clearCaptureAttempt();
		const bool selected = requestEmpiresCaptureApproach();
		logger->Log(LogLevel::INFO,
			"Empires recovery acceptance capture=selector phase=select bot_userid=%d active=%d selected=%d subject=%d result=%s",
			engine->GetPlayerUserId(m_pEdict), eta_recovery_active.GetBool() ? 1 : 0,
			selected ? 1 : 0, m_iEmpiresCaptureApproachPoint,
			selected ? "CAPTURE_SELECTED" : "NO_CAPTURE_CANDIDATE");
		return true;
	}

	// Start from a fresh practical goal, then use the real selector to create
	// the CAPTURE episode and its identity.  The only forced input below is a
	// test-only invocation of the already-integrated domain terminal/progress
	// observation hook; no suppression bit or counter is written here.
	clearCaptureAttempt();
	endEmpiresRecoveryEpisode("TEST_FIXTURE_RESET");
	clearCaptureAttempt();
	if (!requestEmpiresCaptureApproach())
		return false;

	const RCBotRecovery::RecoveryEpisodeIdentity selectedIdentity =
		getRecoveryEpisodeIdentity();
	if (!selectedIdentity.IsActive() ||
		selectedIdentity.domain != RCBotRecovery::Domain::CAPTURE)
	{
		return false;
	}
	const std::string subject = selectedIdentity.subjectId;
	const std::uint64_t domainGeneration = selectedIdentity.domainGeneration;
	const std::uint64_t episodeId = selectedIdentity.episodeId;
	const std::uint64_t lifeGeneration = selectedIdentity.lifeGeneration;
	const int selectedPoint = m_iEmpiresCaptureApproachPoint;
	clearCaptureAttempt();

	observeEmpiresRecoveryDomainExhausted(
		"acceptance-capture-domain-exhausted-1");
	ensureEmpiresRecoveryEpisode(RCBotRecovery::Domain::CAPTURE,
		subject.c_str(), domainGeneration,
		"acceptance-capture-same-practical-goal-reuse");
	if (std::strcmp(phase, "progress") == 0)
	{
		observeEmpiresRecoveryProgress(
			"acceptance-meaningful-capture-progress");
		ensureEmpiresRecoveryEpisode(RCBotRecovery::Domain::CAPTURE,
			subject.c_str(), domainGeneration,
			"acceptance-capture-post-progress-reuse");
	}
	observeEmpiresRecoveryDomainExhausted(
		"acceptance-capture-domain-exhausted-2");
	logger->Log(LogLevel::INFO,
		"Empires recovery acceptance capture=prepared phase=%s bot_userid=%d active=%d subject=%s point=%d episode_id=%llu life_generation=%llu domain_generation=%llu",
		phase, engine->GetPlayerUserId(m_pEdict), eta_recovery_active.GetBool() ? 1 : 0,
		subject.c_str(), selectedPoint,
		static_cast<unsigned long long>(episodeId),
		static_cast<unsigned long long>(lifeGeneration),
		static_cast<unsigned long long>(domainGeneration));
	return true;
}

bool CBotEmpires::requestEmpiresRecoverySquadAcceptance()
{
	if (m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		!m_pPlayerInfo->IsFakeClient() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0 || m_pSchedules == nullptr ||
		engine == nullptr || gpGlobals == nullptr)
	{
		return false;
	}

	CBotMod* pCurrentMod = CBotGlobals::getCurrentMod();
	if (pCurrentMod == nullptr || pCurrentMod->getBotType() != BOTTYPE_EMPIRES)
		return false;

	const auto clearSquadAttempt = [this]()
	{
		if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
			m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
	};

	// Reuse the normal shared-goal creation path if this leader has not yet
	// observed one.  The two failures below then enter handleEmpiresSquadGoal
	// itself, including the existing retry threshold and shadow hook.
	clearSquadAttempt();
	if (!hasActiveEmpiresSquadRecoveryGoal() && !handleEmpiresSquadGoal())
		return false;

	clearSquadAttempt();
	m_iEmpiresSquadGoalRecoveryAttempts = 2;
	const bool firstFailureReturned = !handleEmpiresSquadGoal();
	if (!firstFailureReturned || !hasActiveEmpiresSquadRecoveryGoal())
		return false;

	// The first failure resets only local route bookkeeping. Re-entering the
	// same shared goal emits REUSE; the second forced route terminal then uses
	// the existing domain exhaustion hook and emits shadow-only telemetry.
	clearSquadAttempt();
	if (!handleEmpiresSquadGoal())
		return false;
	clearSquadAttempt();
	m_iEmpiresSquadGoalRecoveryAttempts = 2;
	const bool secondFailureReturned = !handleEmpiresSquadGoal();
	const bool sharedGoalActive = hasActiveEmpiresSquadRecoveryGoal();
	logger->Log(LogLevel::INFO,
		"Empires recovery acceptance squad=fixture bot_userid=%d first_failure=%d second_failure=%d shared_goal_active=%d active=%d",
		engine->GetPlayerUserId(m_pEdict), firstFailureReturned ? 1 : 0,
		secondFailureReturned ? 1 : 0, sharedGoalActive ? 1 : 0,
		eta_recovery_active.GetBool() ? 1 : 0);
	return secondFailureReturned && sharedGoalActive;
}

bool CBotEmpires::requestEmpiresSquadSpatialNavigation(
	const RCBotSquadOrder::NavigationIntentRequest& request,
	std::uint64_t* requestToken)
{
	if (requestToken == nullptr || m_bEmpiresSquadSpatialChildActive ||
		m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		!m_pPlayerInfo->IsConnected() || !m_pPlayerInfo->IsFakeClient() ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		engine == nullptr || engine->GetPlayerUserId(m_pEdict) !=
			static_cast<int>(request.memberId) ||
		request.parentOrder.orderId == 0 || request.parentOrder.generation == 0 ||
		request.childExecution.orderId == 0 ||
		request.childExecution.generation == 0 || request.slotId == 0 ||
		request.squadGeneration == 0 || request.decisionRevision == 0 ||
		request.formationGeneration == 0 ||
		!std::isfinite(request.target.x) || !std::isfinite(request.target.y) ||
		!std::isfinite(request.target.z))
	{
		return false;
	}
	const Vector target(request.target.x, request.target.y, request.target.z);
	if (!target.IsValid() || !requestNavigation(target,
		RCBotNavigation::NavigationPolicy::AUTO,
		static_cast<int>(request.childExecution.orderId)))
	{
		return false;
	}
	m_bEmpiresSquadSpatialChildActive = true;
	m_EmpiresSquadSpatialParentOrder = request.parentOrder;
	m_EmpiresSquadSpatialChildExecution = request.childExecution;
	m_iEmpiresSquadSpatialMemberId = request.memberId;
	m_iEmpiresSquadSpatialRequestToken = *requestToken;
	m_iEmpiresSquadSpatialNavigationIntentId = 0;
	m_iEmpiresSquadSpatialSlotId = request.slotId;
	m_iEmpiresSquadSpatialGeneration = request.squadGeneration;
	m_iEmpiresSquadSpatialDecisionRevision = request.decisionRevision;
	m_iEmpiresSquadSpatialFormationGeneration = request.formationGeneration;
	m_iEmpiresSquadSpatialClaimKind = request.claimResourceKind;
	m_iEmpiresSquadSpatialClaimId = request.claimResourceId;
	m_iEmpiresSquadSpatialClaimGeneration =
		request.claimResourceGeneration;
	m_vEmpiresSquadSpatialTarget = target;
	return true;
}

void CBotEmpires::cancelEmpiresSquadSpatialNavigation(
	const RCBotSquadOrder::NavigationIntentRequest& request,
	const std::uint64_t requestToken)
{
	if (!m_bEmpiresSquadSpatialChildActive ||
		m_EmpiresSquadSpatialParentOrder.orderId != request.parentOrder.orderId ||
		m_EmpiresSquadSpatialParentOrder.generation !=
			request.parentOrder.generation ||
		m_EmpiresSquadSpatialChildExecution.orderId !=
			request.childExecution.orderId ||
		m_EmpiresSquadSpatialChildExecution.generation !=
			request.childExecution.generation ||
		m_iEmpiresSquadSpatialMemberId != request.memberId ||
		(m_iEmpiresSquadSpatialRequestToken != requestToken &&
			m_iEmpiresSquadSpatialNavigationIntentId != requestToken))
	{
		return;
	}
	const std::uint64_t intentId = m_iEmpiresSquadSpatialNavigationIntentId;
	m_bEmpiresSquadSpatialChildActive = false;
	m_iEmpiresSquadSpatialNavigationIntentId = 0;
	m_iEmpiresSquadSpatialRequestToken = 0;
	if (intentId != 0 && hasActiveNavigationIntent() &&
		m_navigationIntent.id == intentId)
		cancelNavigationAttempt("squad-spatial-replaced");
	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
}

bool CBotEmpires::ownsEmpiresSquadSpatialMovement() const
{
	if (engine == nullptr || m_pEdict == nullptr || m_pEdict->IsFree() ||
		m_pPlayerInfo == nullptr || !g_empireSquadMoveRecipient.HasActiveParent())
		return false;
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if (userId <= 0)
		return false;
	int squad = -1;
	const int team = m_pPlayerInfo->GetTeamIndex();
	if (!readEmpiresEntityIntProperty(m_pEdict, "m_iSquad", squad) ||
		squad <= 0)
		return false;
	const RCBotOrder::RecipientId scopedId =
		RCBotCommanderMoveTestFixture::TeamScopedSquadId(team,
			static_cast<RCBotOrder::RecipientId>(squad));
	const RCBotOrder::OrderRecord parent =
		g_empireSquadMoveRecipient.ParentSnapshot();
	RCBotSquadOrder::SquadMembershipObservation currentIdentity;
	const bool identityMatches =
		g_empireSquadMoveIdentityTracker.Current(scopedId, currentIdentity) &&
		currentIdentity.recipient == parent.recipient;
	return RCBotSquadMovement::ShouldSuppressLegacyMovement(
		parent.valid && parent.recipient.kind ==
			RCBotOrder::RecipientKind::SQUAD && parent.recipient.id == scopedId &&
			identityMatches,
		g_empireSquadMoveRecipient.HasActiveMovementForMember(
			static_cast<RCBotOrder::RecipientId>(userId)));
}

bool CBotEmpires::isEmpiresCommanderMoveActive() const
{
	// A failed NavMesh route releases the bot back to normal AI. The last target
	// remains recorded so the same Commander order is not retried every frame;
	// a changed target starts a fresh generation.
	return m_bEmpiresCommanderMoveRouteIssued &&
		!m_bEmpiresCommanderMoveFinished &&
		!m_bEmpiresCommanderMoveFailed;
}

void CBotEmpires::logEmpiresCommanderMoveDiagnostic(const char* phase,
	const CBotCmd* outgoing, const bool force)
{
	if (logger == nullptr || engine == nullptr || gpGlobals == nullptr ||
		m_pEdict == nullptr || m_pEdict->IsFree())
		return;

	const float now = engine->Time();
	if (!force && now < m_flEmpiresCommanderMoveNextDiagnosticLog)
		return;
	if (!force)
		m_flEmpiresCommanderMoveNextDiagnosticLog = now + 0.5f;

	const CBotSchedule* schedule = nullptr;
	CBotTask* task = nullptr;
	const bool scheduleEmpty = m_pSchedules == nullptr ||
		m_pSchedules->isEmpty();
	if (!scheduleEmpty)
	{
		schedule = m_pSchedules->getCurrentSchedule();
		task = m_pSchedules->getCurrentTask();
	}
	char taskText[256] = {};
	if (task != nullptr)
		task->debugString(taskText, sizeof(taskText));

	const bool navHasNext = m_pNavigator != nullptr &&
		m_pNavigator->hasNextPoint();
	const int navWaypoint = navHasNext ?
		m_pNavigator->getCurrentWaypointID() : -1;
	const int navGoalId = navHasNext ?
		m_pNavigator->getCurrentGoalID() : -1;
	const int navRouteSize = m_pNavigator != nullptr ?
		m_pNavigator->getRouteSize() : -1;
	const Vector navNext = navHasNext && m_pNavigator != nullptr ?
		m_pNavigator->getNextPoint() : vec3_origin;
	const Vector navPrevious = m_pNavigator != nullptr ?
		m_pNavigator->getPreviousPoint() : vec3_origin;
	const Vector navGoalOrigin = m_pNavigator != nullptr ?
		m_pNavigator->getGoalOrigin() : vec3_origin;
	Vector velocity = vec3_origin;
	const bool velocityKnown = m_pEdict != nullptr &&
		CClassInterface::getVelocity(m_pEdict, &velocity);
	const Vector origin = getOrigin();

	logger->Log(LogLevel::INFO,
		"Empires commander-diag phase=%s curtime=%.3f owner_userid=%d generation=%d route_issued=%d test_active=%d finished=%d failed=%d target=(%.1f %.1f %.1f) previous_order_target=(%.1f %.1f %.1f) origin=(%.1f %.1f %.1f) schedule_empty=%d schedule=%s task=%s nav_has_next=%d nav_waypoint=%d nav_goal_id=%d nav_route_size=%d nav_next=(%.1f %.1f %.1f) nav_previous=(%.1f %.1f %.1f) nav_goal=(%.1f %.1f %.1f) move_to_valid=%d move_to=(%.1f %.1f %.1f) move_priority=%d move_look_priority=%d desired_move=(%.1f %.1f %.1f) buttons=0x%x view=(%.1f %.1f %.1f) velocity_known=%d velocity=(%.1f %.1f %.1f)",
		phase != nullptr ? phase : "unknown", now,
		engine->GetPlayerUserId(m_pEdict), m_iEmpiresCommanderMoveGeneration,
		m_bEmpiresCommanderMoveRouteIssued ? 1 : 0,
		m_bEmpiresCommanderMoveTestActive ? 1 : 0,
		m_bEmpiresCommanderMoveFinished ? 1 : 0,
		m_bEmpiresCommanderMoveFailed ? 1 : 0,
		m_vEmpiresCommanderMoveTarget.x, m_vEmpiresCommanderMoveTarget.y,
		m_vEmpiresCommanderMoveTarget.z,
		m_vEmpiresCommanderMovePreviousTarget.x,
		m_vEmpiresCommanderMovePreviousTarget.y,
		m_vEmpiresCommanderMovePreviousTarget.z,
		origin.x, origin.y, origin.z,
		scheduleEmpty ? 1 : 0,
		schedule != nullptr && schedule->getIDString() != nullptr ?
			schedule->getIDString() : "none",
		taskText[0] != 0 ? taskText : "none",
		navHasNext ? 1 : 0, navWaypoint, navGoalId, navRouteSize,
		navNext.x, navNext.y, navNext.z,
		navPrevious.x, navPrevious.y, navPrevious.z,
		navGoalOrigin.x, navGoalOrigin.y, navGoalOrigin.z,
		m_bMoveToIsValid ? 1 : 0, m_vMoveTo.x, m_vMoveTo.y, m_vMoveTo.z,
		m_iMovePriority, m_iMoveLookPriority,
		m_fForwardSpeed, m_fSideSpeed, m_fUpSpeed, m_iButtons,
		m_vViewAngles.x, m_vViewAngles.y, m_vViewAngles.z,
		velocityKnown ? 1 : 0, velocity.x, velocity.y, velocity.z);

	if (outgoing != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires commander-diag-cmd phase=%s owner_userid=%d command_number=%d tick_count=%d final_forwardmove=%.1f final_sidemove=%.1f final_upmove=%.1f final_buttons=0x%x final_view=(%.1f %.1f %.1f) alter_owner=%s",
			phase != nullptr ? phase : "unknown",
			engine->GetPlayerUserId(m_pEdict), outgoing->command_number,
			outgoing->tick_count, outgoing->forwardmove, outgoing->sidemove,
			outgoing->upmove, outgoing->buttons, outgoing->viewangles.x,
			outgoing->viewangles.y, outgoing->viewangles.z,
			isEmpiresCommanderMoveActive() ? "commander-navigation" : "none");
	}
}

bool CBotEmpires::handleEmpiresCommanderMoveProviderNeutral()
{
	const double now = engine != nullptr ? engine->Time() : 0.0;
	if (m_commanderMoveShadow.HasActiveOrder() &&
		m_commanderMoveShadow.Update(now))
	{
		m_bEmpiresCommanderMoveFailed = true;
		if (m_vEmpiresCommanderMoveTarget.IsValid())
			EmpiresAnalysis::RecordCommanderEvent(this,
				"COMMANDER_ORDER_CANCELLED", m_vEmpiresCommanderMoveTarget,
				"deadline-expired");
		// The order deadline is independent of NavigationAttempt timeout.  Once
		// the order is terminal, release the still-active navigation owner but do
		// not create a retry or a replacement order.
		if (hasActiveNavigationIntent())
			cancelNavigationAttempt("commander-order-deadline");
		return false;
	}

	Vector target;
	bool synthetic = false;
	bool expired = false;
	int commandType = -1;

	// One stuck attempt (3 s without progress) used to drop the order for
	// good. While the same order stands, plan again from where the bot is.
	if (m_bEmpiresCommanderMoveRouteIssued && m_bEmpiresCommanderMoveFailed &&
		m_bEmpiresCommanderMoveStuck && m_iEmpiresCommanderMoveRetries < 10 && engine != nullptr &&
		engine->Time() >= m_fEmpiresCommanderMoveRetryAt && isAlive())
	{
		const bool read = readEmpiresCommanderMove(target, synthetic, expired, commandType);
		const bool sameOrder = read ? !expired &&
			(target - m_vEmpiresCommanderMoveTarget).LengthSqr() <=
				kEmpiresCommanderMoveTargetChangeRadius * kEmpiresCommanderMoveTargetChangeRadius :
			m_bEmpiresCommanderMoveSynthetic;
		if (sameOrder)
		{
			++m_iEmpiresCommanderMoveRetries;
			m_fEmpiresCommanderMoveRetryAt = engine->Time() + 2.0f;
			clearNavigationAttemptCooldownForStartAnchorRetry();
			m_bEmpiresCommanderMoveRouteIssued = false;
			m_bEmpiresCommanderMoveRetrying = true;
			if (m_bEmpiresCommanderMoveSynthetic)
			{
				m_bEmpiresCommanderMoveTestActive = true;
				m_vEmpiresCommanderMoveTestTarget = m_vEmpiresCommanderMoveTarget;
			}
			Msg("[RCBOT2] commander-move retry=%d from=(%.0f %.0f %.0f)\n",
				m_iEmpiresCommanderMoveRetries, getOrigin().x, getOrigin().y, getOrigin().z);
		}
	}

	if (!readEmpiresCommanderMove(target, synthetic, expired, commandType))
	{
		// A cleared public sendprop is not a cancellation. Keep the active order
		// alive until the provider-neutral navigation path reports a result.
		if (!m_bEmpiresCommanderMoveRouteIssued ||
			m_bEmpiresCommanderMoveFinished || m_bEmpiresCommanderMoveFailed)
			return false;
		target = m_vEmpiresCommanderMoveTarget;
		synthetic = m_bEmpiresCommanderMoveTestActive;
	}

	if (engine != nullptr)
	{
		// Keep the generic legacy stuck sentinel from competing with the active
		// NavigationAttempt. Provider progress and failure remain authoritative.
		m_bFailNextMove = false;
		m_fLastWaypointVisible = engine->Time();
	}

	const int userId = engine != nullptr && m_pEdict != nullptr ?
		engine->GetPlayerUserId(m_pEdict) : -1;
	const bool targetChanged = !m_bEmpiresCommanderMoveRouteIssued ||
		(m_vEmpiresCommanderMoveTarget - target).LengthSqr() >
			kEmpiresCommanderMoveTargetChangeRadius *
			kEmpiresCommanderMoveTargetChangeRadius;

	if (targetChanged)
	{
		const bool replacement = m_bEmpiresCommanderMoveRouteIssued &&
			!m_bEmpiresCommanderMoveFinished && !m_bEmpiresCommanderMoveFailed;
		if (replacement)
		{
			EmpiresAnalysis::RecordCommanderEvent(this,
				"COMMANDER_ORDER_REPLACED", m_vEmpiresCommanderMoveTarget,
				"navigation-intent-replaced");
			// The protocol is terminal before the new order is issued. The old
			// intent is then cancelled while no adapter order is active, so its
			// terminal callback cannot mutate the replacement.
			m_commanderMoveShadow.ObserveReplacement(engine->Time());
			if (hasActiveNavigationIntent())
				cancelNavigationAttempt("commander-order-replaced");
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires commander-move=route-reset owner_userid=%d generation=%d replacement=1 reason=new-target",
					userId, m_iEmpiresCommanderMoveGeneration);
		}

		if (!m_bEmpiresCommanderMoveRetrying)
			m_iEmpiresCommanderMoveRetries = 0;
		m_bEmpiresCommanderMoveRetrying = false;
		m_bEmpiresCommanderMoveStuck = false;
		m_bEmpiresCommanderMoveSynthetic = synthetic;
		m_bEmpiresCommanderMoveRouteIssued = true;
		m_bEmpiresCommanderMoveFinished = false;
		m_bEmpiresCommanderMoveFailed = false;
		m_vEmpiresCommanderMovePreviousTarget = m_vEmpiresCommanderMoveTarget;
		m_vEmpiresCommanderMoveTarget = target;
		++m_iEmpiresCommanderMoveGeneration;
		m_iEmpiresCommanderMoveNavigationIntentId = 0;

		const char* source = synthetic ? "synthetic-test" : "EmpiresSendProp";
		const std::string commanderDetail = std::string("source=") + source +
			" command_type=" + std::to_string(commandType) +
			" order_expired=" + std::to_string(expired ? 1 : 0);
		const int legacyOrderId = EmpiresAnalysis::RecordCommanderEvent(this,
			"COMMANDER_ORDER_RECEIVED", target, commanderDetail.c_str());
		const int recipientIndex = userId > 0 ? userId :
			(engine != nullptr && m_pEdict != nullptr ?
				engine->IndexOfEdict(m_pEdict) : 0);
		const RCBotOrder::TargetPosition shadowTarget = {
			target.x, target.y, target.z
		};
		const double now = engine != nullptr ? engine->Time() : 0.0;
		m_commanderMoveShadow.ObserveNewMove(shadowTarget,
			legacyOrderId > 0 ?
				static_cast<RCBotOrder::OrderId>(legacyOrderId) : 0,
			kEmpiresCommanderIssuerId,
			recipientIndex > 0 ?
				static_cast<RCBotOrder::RecipientId>(recipientIndex) : 0,
			now, now + 300.0);

		const RCBotOrder::OrderKey order = m_commanderMoveShadow.ActiveKey();
		if (!requestNavigation(target,
			RCBotNavigation::NavigationPolicy::AUTO,
			static_cast<int>(order.orderId)))
		{
			m_bEmpiresCommanderMoveFailed = true;
			m_commanderMoveShadow.ObserveFailure(
				RCBotOrder::OrderOutcome::BLOCKED,
				RCBotOrder::DetailReason::FAILURE_REPORTED, now);
			EmpiresAnalysis::RecordCommanderEvent(this,
				"COMMANDER_ORDER_CANCELLED", target,
				"navigation-request-rejected");
			return false;
		}

		m_bFailNextMove = false;
		m_fWaypointStuckTime = 0.0f;
		m_fLastWaypointVisible = now;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires commander-move=started source=%s owner_userid=%d generation=%d target=(%.1f %.1f %.1f) navigation=provider-neutral-auto replacement=%d order_expired=%d",
				synthetic ? "synthetic-test" : "EmpiresSendProp", userId,
				m_iEmpiresCommanderMoveGeneration, target.x, target.y, target.z,
				replacement ? 1 : 0, expired ? 1 : 0);
	}

	if (m_bEmpiresCommanderMoveRouteIssued &&
		!m_bEmpiresCommanderMoveFinished &&
		!m_bEmpiresCommanderMoveFailed)
		setMoveLookPriority(MOVELOOK_TASK);

	logEmpiresCommanderMoveDiagnostic("commander-handler", nullptr, false);
	return m_bEmpiresCommanderMoveRouteIssued &&
		!m_bEmpiresCommanderMoveFinished && !m_bEmpiresCommanderMoveFailed;
}

void CBotEmpires::getTasks(const unsigned iIgnore)
{
	// A current Phase-10.6 parent owns its snapshot members.  This gate also
	// covers the brief Parent->Decision dispatch window so legacy squad offsets
	// cannot acquire movement while validated child slots are being resolved.
	if (ownsEmpiresSquadSpatialMovement())
		return;

	// A real Commander MOVE (or the test hook that drives this same consumer)
	// owns the objective until provider-neutral navigation reaches it, replaces
	// it, or reports a terminal failure. Normal bot navigation remains unchanged.
	if (handleEmpiresCommanderMoveProviderNeutral())
	{
		logEmpiresCommanderMoveDiagnostic("getTasks-commander-return", nullptr, false);
		return;
	}

	// The bounded vehicle action owns the route and interaction lifecycle until
	// it completes or fails. Its normal UserCmd is emitted from handleWeapons
	// after the schedule has had a chance to execute the approach task.
	if (commanderVehicleEntryActive() || m_iBuildAssignStage > 0)
		return;

	if (isEmpiresVehicleActionActive())
		return;

	// Buying, walking to and driving a vehicle replaces the normal goals.
	if (empiresVehicleOwnsBot())
		return;

	if (m_eNormalPathState == EmpiresNormalPathState::SEARCHING ||
		m_eNormalPathState == EmpiresNormalPathState::ARMED ||
		m_eNormalPathState == EmpiresNormalPathState::ACTIVE)
		return;

	// Keep navigation from replacing the Grenadier's firing position while
	// a mortar or RPG action owns the input.
	if (isEmpiresMortarActionActive() || m_flEmpiresTurretRocketUntil > 0.0f)
		return;

	// The explicit Engineer repair proof owns the input window until it
	// completes or times out.  Do not recreate a normal Goal-Roam schedule
	// while that debug-only action is active.
	if (isEmpiresEngineerRepairActionActive())
		return;

	if (isEmpiresEngineerResupplyActionActive())
		return;

	// A normal mine action owns its bounded input window. Do not recreate a
	// Goal-Roam schedule while the existing action waits for selection,
	// readiness or the engine's delayed deployment state.
	if (isEmpiresMineActionActive())
		return;

	// The bounded Engineer build probes own the bot's position and input
	// window after their route/setup phase.  Do not install a normal Goal-Roam
	// schedule underneath them while they wait for selection, aim convergence
	// or the real placement result.
	if (isEmpiresEngineerBuildSelectionActive() ||
		isEmpiresEngineerBuildActionActive())
		return;

	// Keep the Scout's normal schedule neutral while the comparative fixture
	// waits for both target-selection observations. The fixture still lets the
	// real perception/selection and outgoing UserCmd paths run; after explicit
	// fixture cleanup, normal spot and approach tasks resume for verification.
	if (g_empireScoutPriorityFixture.active &&
		m_pEdict == g_empireScoutPriorityFixture.scoutEdict)
		return;

	const bool coverScheduleActive =
		m_pSchedules != nullptr &&
		m_pSchedules->isCurrentSchedule(SCHED_GOOD_HIDE_SPOT);
	if (coverScheduleActive)
	{
		if (!m_bEmpiresCoverScheduleActive)
		{
			// Defensive recovery for a schedule created by the generic hurt path.
			// The Empires override normally marks this before the next Think.
			m_bEmpiresCoverScheduleActive = true;
			logger->Log(LogLevel::INFO,
				"Empires gameplay cover=started route=existing-CGotoHideSpotSched");
		}
		return;
	}

	if (m_bEmpiresCoverScheduleActive)
	{
		m_bEmpiresCoverScheduleActive = false;
		m_pEmpiresApproachTarget = nullptr;
		m_bEmpiresApproachTaskIssued = false;
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
		logger->Log(LogLevel::INFO,
			"Empires gameplay cover=ended next=normal-ai");
	}

	bool lastEnemySearchEnded = false;
	bool lastEnemySearchReachedArea = false;
	const bool lastEnemySearchScheduleActive =
		m_pSchedules != nullptr &&
		m_pSchedules->isCurrentSchedule(SCHED_FOLLOW_LAST_ENEMY);

	if (m_bEmpiresLastEnemySearchActive && !lastEnemySearchScheduleActive)
	{
		lastEnemySearchReachedArea = m_bEmpiresLastEnemySearchTaskLogged;
		const bool enemyReacquired =
			hasSomeConditions(CONDITION_SEE_CUR_ENEMY) &&
			m_pEnemy != nullptr &&
			CBotGlobals::entityIsValid(m_pEnemy) &&
			CBotGlobals::entityIsAlive(m_pEnemy) &&
			isEnemy(m_pEnemy, false);

		logger->Log(LogLevel::INFO,
			"Empires gameplay last-known-search=ended owner_userid=%d outcome=%s",
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			enemyReacquired ? "reacquired" :
				(m_bEmpiresLastEnemySearchTaskLogged ? "expired-or-completed" :
					"path-failed-or-interrupted"));
		m_bEmpiresLastEnemySearchActive = false;
		m_bEmpiresLastEnemySearchTaskLogged = false;
		lastEnemySearchEnded = true;
	}

	if (m_bEmpiresLastEnemySearchActive)
	{
		if (!m_bEmpiresLastEnemySearchTaskLogged &&
			lastEnemySearchScheduleActive && m_pSchedules->getCurrentTask() != nullptr)
		{
			char taskName[128] = {};
			m_pSchedules->getCurrentTask()->debugString(taskName, sizeof(taskName));
			if (std::strncmp(taskName, "CFindLastEnemy", 14) == 0)
			{
				m_bEmpiresLastEnemySearchTaskLogged = true;
				logger->Log(LogLevel::INFO,
					"Empires gameplay last-known-search=area-reached owner_userid=%d search_task=CFindLastEnemy position=(%.1f %.1f %.1f)",
					engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
					m_vLastSeeEnemy.x, m_vLastSeeEnemy.y, m_vLastSeeEnemy.z);
			}
		}

		return;
	}

	const bool validLastEnemy =
		!hasSomeConditions(CONDITION_SEE_CUR_ENEMY) &&
		!m_bLookedForEnemyLast &&
		m_pLastEnemy != nullptr &&
		m_fLastSeeEnemy > 0.0f &&
		m_fLastSeeEnemy + 10.0f > engine->Time() &&
		CBotGlobals::entityIsValid(m_pLastEnemy) &&
		CBotGlobals::entityIsAlive(m_pLastEnemy) &&
		isEnemy(m_pLastEnemy, false) &&
		wantToFollowEnemy();

	if (validLastEnemy && m_pSchedules != nullptr)
	{
		if (getRecoveryEpisodeIdentity().IsActive())
			endEmpiresRecoveryEpisode("GOAL_REPLACED");
		if (m_bEmpiresApproachTaskIssued)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay task=APPROACH_ENEMY cleared reason=los-lost-last-known");
		}

		// Empires currently has only the normal approach/goal-roam schedules.
		// Discard the stale direct-enemy route before installing the bounded
		// last-known route, so goal-roam cannot resume underneath it.
		// Tactical combat takes schedule ownership here; a live capture
		// records the cause so its resume does not spend retry budget.
		if (m_bEmpiresCaptureApproachIssued)
			m_captureInterruptionLatch.noteCombatInterruption();
		m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
		m_pEmpiresApproachTarget = nullptr;
		m_bEmpiresApproachTaskIssued = false;

		Vector vVelocity(0.0f, 0.0f, 0.0f);
		(void)CClassInterface::getVelocity(m_pLastEnemy, &vVelocity);

		CBotSchedule* pSchedule = new CBotSchedule();
		CFindPathTask* pFindPath =
			new CFindPathTask(m_vLastSeeEnemy, LOOK_LAST_ENEMY);
		if (CWaypoints::numWaypoints() == 0)
			pFindPath->setNavigationPolicy(RCBotNavigation::NavigationPolicy::NAVMESH_ONLY);
		pFindPath->setCompleteInterrupt(CONDITION_SEE_CUR_ENEMY);
		pFindPath->setNoInterruptions();
		pSchedule->addTask(pFindPath);
		pSchedule->addTask(new CFindLastEnemy(m_vLastSeeEnemy, vVelocity));
		pSchedule->setID(SCHED_FOLLOW_LAST_ENEMY);
		m_pSchedules->addFront(pSchedule);

		m_bLookedForEnemyLast = true;
		m_bEmpiresLastEnemySearchActive = true;
		m_bEmpiresLastEnemySearchTaskLogged = false;
		logger->Log(LogLevel::INFO,
			"Empires gameplay last-known-search=started owner_userid=%d target_index=%d target_userid=%d position=(%.1f %.1f %.1f) route=CFindPathTask search=CFindLastEnemy bounded_seconds=2-4",
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			engine != nullptr ? engine->IndexOfEdict(m_pLastEnemy) : -1,
			engine != nullptr ? engine->GetPlayerUserId(m_pLastEnemy) : -1,
			m_vLastSeeEnemy.x, m_vLastSeeEnemy.y, m_vLastSeeEnemy.z);
		return;
	}

	edict_t* pApproachTarget = m_pEmpiresApproachTarget.get();
	const bool validApproachTarget =
		m_bEmpiresApproachTaskIssued &&
		pApproachTarget != nullptr &&
		CBotGlobals::entityIsValid(pApproachTarget) &&
		CBotGlobals::entityIsAlive(pApproachTarget) &&
		isEnemy(pApproachTarget, false);
	if (validApproachTarget)
		maybeSpotEmpiresEnemy();

	// Set when the relevance gate ends an active pursuit. The think below
	// then routes into the normal no-enemy decisions (capture resume) instead
	// of re-issuing the approach for the still-visible enemy.
	bool pursuitDroppedForCapture = false;

	// Keep an already-issued route alive while an enemy temporarily leaves the
	// bot's view cone.  Visibility is required to acquire a target, but it is
	// not a safe reason to discard an active approach route.  Death, disconnect,
	// self/team changes still invalidate it through the checks below.
	if (m_bEmpiresApproachTaskIssued)
	{
		if (validApproachTarget)
		{
			// Bound pursuit while a valid capture owns the objective. A chase
			// whose enemy left the relevant area (or that dragged the bot too
			// far from the objective) ends here; the still-valid capture
			// resumes through the existing drain path without spending retry
			// budget (the latch recorded at the original wipe is consumed
			// there, exactly once). Without an owned capture this block is
			// untouched legacy behavior.
			if (m_bEmpiresCaptureApproachIssued &&
				isEmpiresCaptureApproachPointStillValid())
			{
				namespace CombatRelevance = RCBotEmpiresCombatRelevance;
				const float pursuitEnemyDistance =
					distanceFrom(pApproachTarget);
				const float pursuitEnemyObjectiveDistance =
					(CBotGlobals::entityOrigin(pApproachTarget) -
						m_vEmpiresCaptureApproachOrigin).Length();
				const float pursuitBotObjectiveDistance =
					distanceFrom(m_vEmpiresCaptureApproachOrigin);
				const bool pursuitRecentlyHurt = recentlyHurt(
					CombatRelevance::kSelfDefenseHurtWindowSeconds);
				CombatRelevance::PursuitDropReason dropReason =
					CombatRelevance::PursuitDropReason::NONE;
				if (CombatRelevance::DecidePursuitContinuation(
					CombatRelevance::CaptureOwnership{true, true},
					pursuitEnemyDistance, pursuitEnemyObjectiveDistance,
					pursuitBotObjectiveDistance, pursuitRecentlyHurt,
					&dropReason) == CombatRelevance::PursuitDecision::DROP)
				{
					const std::string correlation =
						getEmpiresCombatCorrelation(pApproachTarget);
					const std::string telemetryDetail =
						RCBotEmpiresCombatTelemetry::FormatDecision(
							"CONTINUE", "DROP",
							dropReason == CombatRelevance::PursuitDropReason::PURSUIT_DRAGGED_TOO_FAR ?
								"pursuit-dragged-too-far" : "enemy-left-relevant-area",
							"PURSUIT_TERMINATED") + ";" + correlation;
					EmpiresAnalysis::RecordBotEvent(this,
						"APPROACH_ENEMY_DROPPED", pApproachTarget,
						telemetryDetail.c_str());
					logger->Log(LogLevel::INFO,
						"Empires gameplay task=APPROACH_ENEMY dropped reason=%s decision_phase=CONTINUE decision=DROP caller_outcome=PURSUIT_TERMINATED owner_userid=%d owner_team=%d target_index=%d target_userid=%d target_team=%d capture_point=%d capture_generation=%llu capture_attempt=%llu curtime=%.3f enemy_distance=%.1f enemy_objective_distance=%.1f objective_remaining=%.1f",
						dropReason == CombatRelevance::PursuitDropReason::PURSUIT_DRAGGED_TOO_FAR ?
							"pursuit-dragged-too-far" : "enemy-left-relevant-area",
						engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
						getTeam(),
						engine != nullptr ? engine->IndexOfEdict(pApproachTarget) : -1,
						engine != nullptr ? engine->GetPlayerUserId(pApproachTarget) : -1,
						playerinfomanager != nullptr && playerinfomanager->GetPlayerInfo(pApproachTarget) != nullptr ? playerinfomanager->GetPlayerInfo(pApproachTarget)->GetTeamIndex() : -1,
						m_iEmpiresCaptureApproachPoint,
						static_cast<unsigned long long>(m_iEmpiresCaptureObjectiveGeneration),
						static_cast<unsigned long long>(m_iEmpiresCaptureRouteAttemptGeneration),
						gpGlobals != nullptr ? gpGlobals->curtime : 0.0f,
						pursuitEnemyDistance, pursuitEnemyObjectiveDistance,
						pursuitBotObjectiveDistance);
					m_pEmpiresApproachTarget = nullptr;
					m_bEmpiresApproachTaskIssued = false;
					if (m_pSchedules != nullptr)
						m_pSchedules->freeMemory();
					if (m_pNavigator != nullptr)
						m_pNavigator->clear();
					pursuitDroppedForCapture = true;
				}
			}

			// A failed or completed approach schedule can leave the target state
			// latched while the visible enemy is still well outside the existing
			// CMoveToTask completion radius.  Re-arm only this Empires-specific
			// approach route; do not alter the generic task or weapon systems.
			const float approachDistance = distanceFrom(pApproachTarget);
			if (!pursuitDroppedForCapture && m_pSchedules != nullptr &&
				m_pSchedules->isEmpty() &&
				approachDistance > 64.0f && isVisible(pApproachTarget))
			{
				if (logger != nullptr)
				{
					const std::string correlation =
						getEmpiresCombatCorrelation(pApproachTarget);
					const std::string telemetryDetail =
						RCBotEmpiresCombatTelemetry::FormatDecision(
							"CONTINUE", "REARM", "schedule-empty",
							"REISSUE_PENDING") + ";" + correlation;
					EmpiresAnalysis::RecordBotEvent(this,
						"APPROACH_ENEMY_REARMED", pApproachTarget,
						telemetryDetail.c_str());
					logger->Log(LogLevel::INFO,
						"Empires gameplay task=APPROACH_ENEMY rearmed reason=schedule-empty decision_phase=CONTINUE decision=REARM caller_outcome=REISSUE_PENDING owner_userid=%d owner_team=%d target_index=%d target_userid=%d capture_point=%d capture_generation=%llu capture_attempt=%llu curtime=%.3f distance=%.1f",
						engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
						getTeam(),
						engine != nullptr ? engine->IndexOfEdict(pApproachTarget) : -1,
						engine != nullptr ? engine->GetPlayerUserId(pApproachTarget) : -1,
						m_iEmpiresCaptureApproachPoint,
						static_cast<unsigned long long>(m_iEmpiresCaptureObjectiveGeneration),
						static_cast<unsigned long long>(m_iEmpiresCaptureRouteAttemptGeneration),
						gpGlobals != nullptr ? gpGlobals->curtime : 0.0f,
						approachDistance);
				}

				m_pEmpiresApproachTarget = nullptr;
				m_bEmpiresApproachTaskIssued = false;
				if (m_pNavigator != nullptr)
					m_pNavigator->clear();
			}
			else if (!pursuitDroppedForCapture)
			{
				return;
			}
			// Dropped pursuits fall through to the no-enemy decisions below so
			// the still-valid capture resumes instead of re-arming.
		}
		else
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay task=APPROACH_ENEMY cleared reason=target-invalid-dead-or-team-changed");
			m_pEmpiresApproachTarget = nullptr;
			m_bEmpiresApproachTaskIssued = false;
			if (m_pSchedules != nullptr)
				m_pSchedules->freeMemory();
			if (m_pNavigator != nullptr)
				m_pNavigator->clear();
		}
	}

	edict_t* pEnemy = m_pEnemy.get();
	const bool validVisibleEnemy =
		pEnemy != nullptr &&
		hasSomeConditions(CONDITION_SEE_CUR_ENEMY) &&
		CBotGlobals::entityIsValid(pEnemy) &&
		CBotGlobals::entityIsAlive(pEnemy) &&
		isEnemy(pEnemy, false) &&
		isVisible(pEnemy);
	if (validVisibleEnemy)
		maybeSpotEmpiresEnemy();

	// Objective-aware combat relevance: a distant, irrelevant enemy must not
	// displace a valid active capture objective. When the gate declines, the
	// think below runs the normal no-enemy decisions (capture resume) with
	// the capture schedule untouched. Weapon handling is a separate existing
	// path and still engages this enemy; spotting above still shares it.
	bool deferVisibleEnemyToObjective = false;
	if (validVisibleEnemy && !m_bEmpiresApproachTaskIssued &&
		m_bEmpiresCaptureApproachIssued &&
		isEmpiresCaptureApproachPointStillValid())
	{
		namespace CombatRelevance = RCBotEmpiresCombatRelevance;
		const float engageEnemyDistance = distanceFrom(pEnemy);
		const float engageEnemyObjectiveDistance =
			(CBotGlobals::entityOrigin(pEnemy) -
				m_vEmpiresCaptureApproachOrigin).Length();
		const bool engageRecentlyHurt = recentlyHurt(
			CombatRelevance::kSelfDefenseHurtWindowSeconds);
		if (CombatRelevance::DecideVisibleEnemyEngagement(
			CombatRelevance::CaptureOwnership{true, true},
			engageEnemyDistance, engageEnemyObjectiveDistance,
			engageRecentlyHurt) == CombatRelevance::EngagementDecision::IGNORE)
		{
			deferVisibleEnemyToObjective = true;
			// Throttled: this evaluation runs every think while the enemy
			// stays visible. The timestamp never feeds a decision (REL-X1).
			if (engine->Time() >= m_flEmpiresRelevanceIgnoreLogTime)
			{
				m_flEmpiresRelevanceIgnoreLogTime = engine->Time() + 10.0f;
				const std::string correlation =
					getEmpiresCombatCorrelation(pEnemy);
				const std::string telemetryDetail =
					RCBotEmpiresCombatTelemetry::FormatDecision(
						"ACQUIRE", "DEFER", "enemy-irrelevant-to-capture",
						"CAPTURE_PRESERVED") + ";" + correlation;
				EmpiresAnalysis::RecordBotEvent(this,
					"APPROACH_ENEMY_DEFERRED", pEnemy,
					telemetryDetail.c_str());
				logger->Log(LogLevel::INFO,
					"Empires gameplay task=APPROACH_ENEMY deferred reason=enemy-irrelevant-to-capture decision_phase=ACQUIRE decision=DEFER caller_outcome=CAPTURE_PRESERVED owner_userid=%d owner_team=%d target_index=%d target_userid=%d target_team=%d capture_point=%d capture_generation=%llu capture_attempt=%llu curtime=%.3f enemy_distance=%.1f enemy_objective_distance=%.1f",
					engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
					getTeam(),
					engine != nullptr ? engine->IndexOfEdict(pEnemy) : -1,
					engine != nullptr ? engine->GetPlayerUserId(pEnemy) : -1,
					playerinfomanager != nullptr && playerinfomanager->GetPlayerInfo(pEnemy) != nullptr ? playerinfomanager->GetPlayerInfo(pEnemy)->GetTeamIndex() : -1,
					m_iEmpiresCaptureApproachPoint,
					static_cast<unsigned long long>(m_iEmpiresCaptureObjectiveGeneration),
					static_cast<unsigned long long>(m_iEmpiresCaptureRouteAttemptGeneration),
					gpGlobals != nullptr ? gpGlobals->curtime : 0.0f,
					engageEnemyDistance, engageEnemyObjectiveDistance);
			}
		}
	}

	if (!validVisibleEnemy || pursuitDroppedForCapture ||
		deferVisibleEnemyToObjective)
	{
		if (m_bEmpiresApproachTaskIssued || m_pEmpiresApproachTarget.get() != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay task=APPROACH_ENEMY cleared reason=target-invalid-dead-or-not-visible");
			m_pEmpiresApproachTarget = nullptr;
			m_bEmpiresApproachTaskIssued = false;
			if (m_pSchedules != nullptr)
				m_pSchedules->freeMemory();
			if (m_pNavigator != nullptr)
				m_pNavigator->clear();
		}

		// A normal Engineer resupply route owns only the trip to a current-team
		// ammo crate.  Once the route arrives, the existing bounded action performs
		// the real IN_USE interaction; after completion/failure, normal goal choice
		// resumes.
		if (m_bEmpiresEngineerResupplyTaskIssued)
		{
			if (m_eEmpiresEngineerResupplyActionState ==
				EmpiresEngineerResupplyActionState::COMPLETE ||
				m_eEmpiresEngineerResupplyActionState ==
				EmpiresEngineerResupplyActionState::FAILED)
			{
				if (m_eEmpiresEngineerResupplyActionState ==
					EmpiresEngineerResupplyActionState::COMPLETE)
					EmpiresAnalysis::RecordBotEvent(this,
						"ENGINEER_RESUPPLY_COMPLETED", nullptr,
						"normal-goal-action-complete");
				m_bEmpiresEngineerResupplyTaskIssued = false;
				m_iEmpiresEngineerResupplyWaypoint = -1;
				m_vEmpiresEngineerResupplyWaypointOrigin = vec3_origin;
				m_flEmpiresNextEngineerResupplyTime = engine->Time() + 30.0f;
			}
			else if (isEmpiresEngineerResupplyActionActive())
			{
				return;
			}
			else if (m_pSchedules != nullptr && m_pSchedules->isEmpty())
			{
				const float waypointDistance =
					distanceFrom(m_vEmpiresEngineerResupplyWaypointOrigin);
				if (waypointDistance > 96.0f)
				{
					const bool navigatorHasNextPoint =
						m_pNavigator != nullptr && m_pNavigator->hasNextPoint();
					const int navigatorCurrentWaypoint = navigatorHasNextPoint ?
						m_pNavigator->getCurrentWaypointID() : -1;
					const int navigatorGoalWaypoint = m_pNavigator != nullptr ?
						m_pNavigator->getCurrentGoalID() : -1;
					const int navigatorRouteSize = m_pNavigator != nullptr ?
						m_pNavigator->getRouteSize() : -1;
					const Vector currentOrigin = getOrigin();
					logger->Log(LogLevel::WARN,
						"Empires gameplay engineer-resupply=result=FAILED reason=route-did-not-reach-waypoint waypoint=%d distance=%.1f origin=(%.1f %.1f %.1f) navigator_has_next=%d navigator_current=%d navigator_goal=%d navigator_route_size=%d",
						m_iEmpiresEngineerResupplyWaypoint, waypointDistance,
						currentOrigin.x, currentOrigin.y, currentOrigin.z,
						navigatorHasNextPoint ? 1 : 0, navigatorCurrentWaypoint,
						navigatorGoalWaypoint, navigatorRouteSize);
					m_bEmpiresEngineerResupplyTaskIssued = false;
					m_iEmpiresEngineerResupplyWaypoint = -1;
					m_vEmpiresEngineerResupplyWaypointOrigin = vec3_origin;
					m_flEmpiresNextEngineerResupplyTime = engine->Time() + 15.0f;
				}
				else if (requestEmpiresEngineerResupplyOnce(false))
				{
					logger->Log(LogLevel::INFO,
						"Empires gameplay engineer-resupply=action-started waypoint=%d mode=normal-goal",
						m_iEmpiresEngineerResupplyWaypoint);
					return;
				}
				else
				{
					logger->Log(LogLevel::WARN,
						"Empires gameplay engineer-resupply=result=FAILED reason=action-rejected waypoint=%d",
						m_iEmpiresEngineerResupplyWaypoint);
					m_bEmpiresEngineerResupplyTaskIssued = false;
					m_iEmpiresEngineerResupplyWaypoint = -1;
					m_vEmpiresEngineerResupplyWaypointOrigin = vec3_origin;
					m_flEmpiresNextEngineerResupplyTime = engine->Time() + 15.0f;
				}
			}
			else
			{
				return;
			}
		}

		const bool engineerResupplyCanReplaceRoam =
			m_pSchedules != nullptr &&
			(m_pSchedules->isEmpty() ||
				m_pSchedules->isCurrentSchedule(SCHED_GOTO_ORIGIN));
		const bool engineerResupplyNeeded = isEmpiresEngineerResupplyNeeded();
		if (m_bEmpiresEngineerResupplyActionDepletionOnly &&
			m_eEmpiresEngineerResupplyActionState ==
				EmpiresEngineerResupplyActionState::COMPLETE &&
			!m_bEmpiresEngineerResupplyHandoffCheckLogged)
		{
			const int engineerTeam = getTeam();
			const char* primaryName =
				m_pPrimaryWeapon != nullptr &&
				m_pPrimaryWeapon->getWeaponInfo() != nullptr &&
				m_pPrimaryWeapon->getWeaponInfo()->getWeaponName() != nullptr ?
				m_pPrimaryWeapon->getWeaponInfo()->getWeaponName() : "<none>";
			const int primaryClip = m_pPrimaryWeapon != nullptr ?
				m_pPrimaryWeapon->getClip1(this) : -1;
			const int primaryAmmo = m_pPrimaryWeapon != nullptr ?
				m_pPrimaryWeapon->getAmmo(this) : -1;
			const bool primaryOutOfAmmo = m_pPrimaryWeapon != nullptr &&
				m_pPrimaryWeapon->outOfAmmo(this);
			const edict_t* activeWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
			int activeClip = -1;
			int activeAmmoType = -1;
			int activeAmmo = -1;
			if (activeWeapon != nullptr && !activeWeapon->IsFree())
			{
				int activeClip2 = -1;
				int activeAmmoType2 = -1;
				CClassInterface::getWeaponClip(const_cast<edict_t*>(activeWeapon),
					&activeClip, &activeClip2);
				CClassInterface::getAmmoTypes(const_cast<edict_t*>(activeWeapon),
					&activeAmmoType, &activeAmmoType2);
				if (activeAmmoType >= 0)
					activeAmmo = getAmmo(static_cast<std::size_t>(activeAmmoType));
			}
			edict_t* pSource = nullptr;
			Vector sourceOrigin = vec3_origin;
			const bool sourceFound = findEmpiresEngineerAmmoSource(engineerTeam,
				pSource, sourceOrigin);
			const int selectedWaypoint = sourceFound ?
				CWaypoints::nearestWaypointGoal(-1, sourceOrigin, 2048.0f,
					engineerTeam) : -1;
			// The closest waypoint is not necessarily a usable handoff point:
			// on emp_streetsoffire the ammo crate sits behind map geometry from
			// waypoint 184. Prefer a team-usable waypoint with a direct trace to
			// the source, even when it is slightly farther from the crate.
			const int selectedWaypointWithVisibility = sourceFound ?
				CWaypointLocations::NearestWaypoint(sourceOrigin, 4096.0f, -1,
					true, false, true, nullptr, false, engineerTeam) : -1;
			CWaypoint* pWaypoint = selectedWaypoint >= 0 ?
				CWaypoints::getWaypoint(selectedWaypoint) : nullptr;
			logger->Log(LogLevel::INFO,
				"Empires gameplay engineer-resupply=handoff-check team=%d primary=%s primary_has=%d primary_clip=%d primary_ammo=%d primary_out_of_ammo=%d active_weapon=%s active_clip=%d active_ammo_type=%d active_ammo=%d needed=%d schedule_can_replace_roam=%d schedule_empty=%d now=%.3f next_time=%.3f source_found=%d source_index=%d source_origin=(%.1f %.1f %.1f) selected_waypoint=%d selected_waypoint_with_visibility=%d waypoint_used=%d waypoint_for_team=%d",
				engineerTeam, primaryName, m_pPrimaryWeapon != nullptr &&
				m_pPrimaryWeapon->hasWeapon() ? 1 : 0, primaryClip, primaryAmmo,
				primaryOutOfAmmo ? 1 : 0,
				activeWeapon != nullptr && !activeWeapon->IsFree() &&
				activeWeapon->GetClassName() != nullptr ? activeWeapon->GetClassName() : "<none>",
				activeClip, activeAmmoType, activeAmmo,
				engineerResupplyNeeded ? 1 : 0,
				engineerResupplyCanReplaceRoam ? 1 : 0,
				m_pSchedules != nullptr && m_pSchedules->isEmpty() ? 1 : 0,
				engine->Time(), m_flEmpiresNextEngineerResupplyTime,
				sourceFound ? 1 : 0,
				pSource != nullptr ? engine->IndexOfEdict(pSource) : -1,
				sourceOrigin.x, sourceOrigin.y, sourceOrigin.z, selectedWaypoint,
				selectedWaypointWithVisibility,
				pWaypoint != nullptr && pWaypoint->isUsed() ? 1 : 0,
				pWaypoint != nullptr && pWaypoint->forTeam(engineerTeam) ? 1 : 0);
			m_bEmpiresEngineerResupplyHandoffCheckLogged = true;
		}
		if (!m_bEmpiresEngineerResupplyTaskIssued &&
			engineerResupplyCanReplaceRoam &&
			engine->Time() >= m_flEmpiresNextEngineerResupplyTime &&
			engineerResupplyNeeded)
		{
			const int engineerTeam = getTeam();
			edict_t* pSource = nullptr;
			Vector sourceOrigin = vec3_origin;
			if (findEmpiresEngineerAmmoSource(engineerTeam, pSource, sourceOrigin))
			{
				const int nearestWaypoint = CWaypoints::nearestWaypointGoal(
					-1, sourceOrigin, 2048.0f, engineerTeam);
				const int visibleWaypoint = CWaypointLocations::NearestWaypoint(
					sourceOrigin, 4096.0f, -1, true, false, true, nullptr, false,
					engineerTeam);
				const int selectedWaypoint = visibleWaypoint >= 0 ? visibleWaypoint :
					nearestWaypoint;
				CWaypoint* pWaypoint = selectedWaypoint >= 0 ?
					CWaypoints::getWaypoint(selectedWaypoint) : nullptr;
				if (pWaypoint != nullptr && pWaypoint->isUsed() &&
					pWaypoint->forTeam(engineerTeam))
				{
					// A new support route must not inherit the one-frame movement
					// failure sentinel or the expiry boundary from the route that
					// just ended. CFindPathTask consumes moveFailed() after it has
					// built a valid route; carrying the old sentinel would therefore
					// discard this fresh schedule while its navigator still owns a
					// valid path.
					m_bFailNextMove = false;
					m_fWaypointStuckTime = 0.0f;
					m_fLastWaypointVisible = 0.0f;
					if (!m_pSchedules->isEmpty())
						m_pSchedules->freeMemory();
					// A fresh resupply handoff must also reset navigator state when
					// the previous schedule already completed and the list is empty.
					// Otherwise a stale failed edge can reject the new route before
					// the Engineer gets a chance to leave its current waypoint.
					if (m_pNavigator != nullptr)
						m_pNavigator->clear();
					CBotSchedule* pResupplySchedule = new CBotSchedule();
					CFindPathTask* pResupplyPath = new CFindPathTask(
						selectedWaypoint, LOOK_WAYPOINT);
					pResupplySchedule->addTask(pResupplyPath);
					// addTask() initializes the task and clears its flags, so this
					// must be set afterward. This route is longer than the default
					// bot_pathrevs budget on emp_streetsoffire; use the existing
					// bounded non-interrupting path mode used by other long routes.
					pResupplyPath->setNoInterruptions();
					pResupplySchedule->addTask(new CMoveToTask(pWaypoint->getOrigin()));
					CMoveToTask* pSourceApproach = new CMoveToTask(sourceOrigin);
					// The source is not itself represented by a nearby waypoint on
					// emp_streetsoffire. The final direct leg can briefly increase
					// Euclidean distance while the player clears the waypoint
					// geometry; do not let the generic looping-origin guard end this
					// one deliberate ammo-crate approach early.
					pSourceApproach->ignoreDistanceIncrease();
					pResupplySchedule->addTask(pSourceApproach);
					m_pSchedules->addFront(pResupplySchedule);
					m_bEmpiresEngineerResupplyTaskIssued = true;
					// The normal-goal handoff follows the bounded depletion probe.
					// That probe legitimately ends in COMPLETE; the route must begin
					// with a fresh action state so the completed precondition cannot
					// clear the newly issued schedule on the next Think.
					m_eEmpiresEngineerResupplyActionState =
						EmpiresEngineerResupplyActionState::IDLE;
					m_iEmpiresEngineerResupplyWaypoint = selectedWaypoint;
					m_vEmpiresEngineerResupplyWaypointOrigin = sourceOrigin;
					EmpiresAnalysis::RecordBotEvent(this,
						"ENGINEER_SUPPORT_SELECTED", pSource,
						"ammo-crate;normal-goal");
					EmpiresAnalysis::RecordBotEvent(this,
						"ENGINEER_RESUPPLY_STARTED", pSource,
						"normal-goal-route");
					logger->Log(LogLevel::INFO,
						"Empires gameplay engineer-resupply=route-started source_index=%d source_class=%s waypoint=%d waypoint_selection=%s nearest_waypoint=%d visible_waypoint=%d anchor=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f) reason=primary-out-of-ammo route=existing-CFindPathTask+CMoveToTask+direct-source-approach",
						engine->IndexOfEdict(pSource), pSource->GetClassName(),
						selectedWaypoint, visibleWaypoint >= 0 ? "visible" : "nearest",
						nearestWaypoint, visibleWaypoint, pWaypoint->getOrigin().x,
						pWaypoint->getOrigin().y, pWaypoint->getOrigin().z,
						sourceOrigin.x, sourceOrigin.y, sourceOrigin.z);
					return;
				}
			}

			m_flEmpiresNextEngineerResupplyTime = engine->Time() + 5.0f;
		}

		// An Engineer support action owns the current building until the normal
		// kit action reports progress or failure. Nearby support is reselected
		// quickly so a newly built structure can be worked on over multiple normal
		// repair pulses; routed support keeps its existing conservative cooldown.
		if (m_bEmpiresEngineerSupportTaskIssued)
		{
			const bool supportActionCompleted =
				m_eEmpiresEngineerRepairActionState ==
				EmpiresEngineerRepairActionState::COMPLETE;
			if (m_eEmpiresEngineerRepairActionState ==
				EmpiresEngineerRepairActionState::COMPLETE ||
				m_eEmpiresEngineerRepairActionState ==
				EmpiresEngineerRepairActionState::FAILED)
			{
				if (supportActionCompleted)
					EmpiresAnalysis::RecordBotEvent(this,
						"ENGINEER_REPAIR_COMPLETED", nullptr,
						"normal-goal-action-complete");
				if (supportActionCompleted)
				{
					// A completed repair proves this route anchor is usable again.
					m_iEmpiresEngineerSupportRejectedBuildingIndex = -1;
					m_iEmpiresEngineerSupportRejectedWaypoint = -1;
					m_flEmpiresEngineerSupportRejectedUntil = 0.0f;
				}
				m_bEmpiresEngineerSupportTaskIssued = false;
				m_bEmpiresEngineerSupportNearby = false;
				m_iEmpiresEngineerSupportBuildingIndex = -1;
				m_iEmpiresEngineerSupportWaypoint = -1;
				m_vEmpiresEngineerSupportWaypointOrigin = vec3_origin;
				m_flEmpiresNextEngineerSupportTime = engine->Time() +
					(supportActionCompleted ? 0.5f : 5.0f);
			}
			else if (m_pSchedules != nullptr && m_pSchedules->isEmpty())
			{
				const float waypointDistance =
					distanceFrom(m_vEmpiresEngineerSupportWaypointOrigin);
				if (waypointDistance > 96.0f)
				{
					m_iEmpiresEngineerSupportRejectedBuildingIndex =
						m_iEmpiresEngineerSupportBuildingIndex;
					m_iEmpiresEngineerSupportRejectedWaypoint =
						m_iEmpiresEngineerSupportWaypoint;
					m_flEmpiresEngineerSupportRejectedUntil = engine->Time() + 60.0f;
					logger->Log(LogLevel::WARN,
						"Empires gameplay engineer-support=result=FAILED reason=route-did-not-reach-waypoint building_index=%d waypoint=%d distance=%.1f rejected_until=%.1f",
						m_iEmpiresEngineerSupportBuildingIndex,
						m_iEmpiresEngineerSupportWaypoint, waypointDistance,
						m_flEmpiresEngineerSupportRejectedUntil);
					m_bEmpiresEngineerSupportTaskIssued = false;
					m_bEmpiresEngineerSupportNearby = false;
					m_iEmpiresEngineerSupportBuildingIndex = -1;
					m_iEmpiresEngineerSupportWaypoint = -1;
					m_vEmpiresEngineerSupportWaypointOrigin = vec3_origin;
					m_flEmpiresNextEngineerSupportTime = engine->Time() + 15.0f;
				}
				else if (requestEmpiresEngineerRepairOnce(
					m_iEmpiresEngineerSupportBuildingIndex))
				{
					logger->Log(LogLevel::INFO,
						"Empires gameplay engineer-support=repair-started building_index=%d waypoint=%d",
						m_iEmpiresEngineerSupportBuildingIndex,
						m_iEmpiresEngineerSupportWaypoint);
					return;
				}
				else
				{
					if (m_iEmpiresEngineerSupportWaypoint >= 0)
					{
						m_iEmpiresEngineerSupportRejectedBuildingIndex =
							m_iEmpiresEngineerSupportBuildingIndex;
						m_iEmpiresEngineerSupportRejectedWaypoint =
							m_iEmpiresEngineerSupportWaypoint;
						m_flEmpiresEngineerSupportRejectedUntil = engine->Time() + 60.0f;
					}
					logger->Log(LogLevel::WARN,
						"Empires gameplay engineer-support=result=FAILED reason=repair-action-rejected building_index=%d waypoint=%d rejected_until=%.1f",
						m_iEmpiresEngineerSupportBuildingIndex,
						m_iEmpiresEngineerSupportWaypoint,
						m_iEmpiresEngineerSupportRejectedWaypoint >= 0 ?
							m_flEmpiresEngineerSupportRejectedUntil : 0.0f);
					m_bEmpiresEngineerSupportTaskIssued = false;
					m_bEmpiresEngineerSupportNearby = false;
					m_iEmpiresEngineerSupportBuildingIndex = -1;
					m_iEmpiresEngineerSupportWaypoint = -1;
					m_vEmpiresEngineerSupportWaypointOrigin = vec3_origin;
					m_flEmpiresNextEngineerSupportTime = engine->Time() + 15.0f;
				}
			}
			else
			{
				return;
			}
		}

		const bool engineerSupportCanReplaceRoam =
			m_pSchedules != nullptr &&
			(m_pSchedules->isEmpty() ||
				m_pSchedules->isCurrentSchedule(SCHED_GOTO_ORIGIN));
		if (!m_bEmpiresEngineerSupportTaskIssued &&
			engineerSupportCanReplaceRoam &&
			engine->Time() >= m_flEmpiresNextEngineerSupportTime)
		{
			int nearbyBuildingIndex = -1;
			if (findEmpiresEngineerNearbySupportTarget(nearbyBuildingIndex) &&
				requestEmpiresEngineerRepairOnce(nearbyBuildingIndex, true))
			{
				m_bEmpiresEngineerSupportTaskIssued = true;
				m_bEmpiresEngineerSupportNearby = true;
				m_iEmpiresEngineerSupportBuildingIndex = nearbyBuildingIndex;
				m_iEmpiresEngineerSupportWaypoint = -1;
				m_vEmpiresEngineerSupportWaypointOrigin = getOrigin();
				EmpiresAnalysis::RecordBotEvent(this,
					"ENGINEER_SUPPORT_SELECTED", nullptr,
					"nearby-building;normal-goal");
				EmpiresAnalysis::RecordBotEvent(this,
					"ENGINEER_REPAIR_STARTED", nullptr,
					"nearby-building;normal-goal");
				logger->Log(LogLevel::INFO,
					"Empires gameplay engineer-support=nearby-repair-started index=%d userid=%d building_index=%d mode=normal-goal",
					engine->IndexOfEdict(m_pEdict),
					engine->GetPlayerUserId(m_pEdict), nearbyBuildingIndex);
				return;
			}

			int buildingIndex = -1;
			int supportWaypoint = -1;
			Vector supportWaypointOrigin = vec3_origin;
			if (findEmpiresEngineerSupportTarget(buildingIndex, supportWaypoint,
				supportWaypointOrigin))
			{
				if (!m_pSchedules->isEmpty())
				{
					m_pSchedules->freeMemory();
					if (m_pNavigator != nullptr)
						m_pNavigator->clear();
				}
				CBotSchedule* pSupportSchedule = new CBotSchedule();
				pSupportSchedule->addTask(new CFindPathTask(supportWaypoint,
					LOOK_WAYPOINT));
				pSupportSchedule->addTask(new CMoveToTask(supportWaypointOrigin));
				m_pSchedules->addFront(pSupportSchedule);
				m_bEmpiresEngineerSupportTaskIssued = true;
				m_bEmpiresEngineerSupportNearby = false;
				m_iEmpiresEngineerSupportBuildingIndex = buildingIndex;
				m_iEmpiresEngineerSupportWaypoint = supportWaypoint;
				m_vEmpiresEngineerSupportWaypointOrigin = supportWaypointOrigin;
				EmpiresAnalysis::RecordBotEvent(this,
					"ENGINEER_SUPPORT_SELECTED", nullptr,
					"routed-building;normal-goal");
				logger->Log(LogLevel::INFO,
					"Empires gameplay engineer-support=route-started building_index=%d waypoint=%d origin=(%.1f %.1f %.1f) route=existing-CFindPathTask+CMoveToTask",
					buildingIndex, supportWaypoint, supportWaypointOrigin.x,
					supportWaypointOrigin.y, supportWaypointOrigin.z);
				return;
			}

			m_flEmpiresNextEngineerSupportTime = engine->Time() + 5.0f;
		}

		// A Grenadier gets one meaningful role action after it has actually
		// reached the last-known enemy area and the bounded search ended without
		// reacquisition. The request validates the current class/inventory and
		// hands all input plus dynamic deployment observation to the existing
		// mine state machine. There is deliberately no arbitrary roam-endpoint
		// or timer-driven mine placement.
		if (lastEnemySearchEnded && lastEnemySearchReachedArea &&
			engine != nullptr &&
			m_flEmpiresNextLastKnownMineTime <= engine->Time() &&
			requestEmpiresMineFireOnce())
		{
			m_flEmpiresNextLastKnownMineTime = engine->Time() + 30.0f;
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires gameplay role=GRENADIER_MINE_AFTER_LAST_KNOWN started owner_userid=%d context=last-known-area search=expired-without-reacquisition cooldown=30.0 action=existing-mine-input-state-machine",
					engine->GetPlayerUserId(m_pEdict));
			return;
		}

		if (handleEmpiresTurretThreat())
			return;

		if (handleEmpiresRevive())
			return;

		if (handleEmpiresVehicleRepairRequest())
			return;

		if (handleEmpiresRefineryTurret())
			return;

		if (handleEmpiresDoorButton())
			return;

		if (handleEmpiresGrenadierTurretAttack())
			return;

		if (requestEmpiresCaptureApproach())
			return;

		// Once the authoritative squad membership tuple is visible, let the
		// elected bot leader choose one shared waypoint and keep followers on
		// bounded offsets.  This sits below combat/objective/action ownership and
		// above generic Goal-Roam, so it cannot replace a meaningful higher-level
		// task with formation movement.
		if (handleEmpiresSquadGoal())
			return;

		// Maps without waypoints: pick targets from map entities and walk
		// there on the navmesh.  Maps with waypoints keep Goal-Roam below.
		if (CWaypoints::numWaypoints() == 0 && handleEmpiresNavmeshGoal())
			return;

		// Port the donor's active goal-roam behavior through the modern
		// waypoint/A* schedule.  This is deliberately only navigation: no
		// objective, combat, or game-specific state is introduced here.
		if (m_pSchedules != nullptr && m_pSchedules->isEmpty())
		{
			if (CWaypoint* pGoal = CWaypoints::randomWaypointGoal(-1))
			{
				// Replacing a schedule is not a practical-goal transition.  In
				// particular, a bounded squad retry may temporarily leave the
				// schedule empty while the same authoritative shared goal remains
				// active; keep its episode identity across this Goal-Roam bridge.
				if (getRecoveryEpisodeIdentity().IsActive() &&
					!hasActiveEmpiresSquadRecoveryGoal())
					endEmpiresRecoveryEpisode("GOAL_REPLACED");
				const int iGoalWaypoint = CWaypoints::getWaypointIndex(pGoal);
				m_pSchedules->add(new CBotGotoOriginSched(pGoal->getOrigin()));
				logger->Log(LogLevel::INFO,
					"Empires gameplay task=GOAL_ROAM waypoint=%d owner_userid=%d route=CBotGotoOriginSched donor=goal-waypoint reason=%s",
					iGoalWaypoint,
					engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
					lastEnemySearchEnded ? "last-known-search-ended" : "no-visible-enemy");
				return;
			}
		}

		CBot::getTasks(iIgnore);
		return;
	}

	if (m_pEmpiresApproachTarget.get() != pEnemy)
	{
		m_pEmpiresApproachTarget = nullptr;
		m_bEmpiresApproachTaskIssued = false;
	}

	if (!m_bEmpiresApproachTaskIssued && m_pSchedules != nullptr)
	{
		if (getRecoveryEpisodeIdentity().IsActive())
			endEmpiresRecoveryEpisode("GOAL_REPLACED");
		// The first production Grenadier role policy is intentionally narrow. A
		// visible enemy in the conservative mortar range is handled by the
		// already verified normal mortar state machine; close combat, missing
		// inventory and failed aim all fall back to the existing approach/weapon
		// path. No teleport or entity state write is used here.
		if (engine != nullptr && engine->Time() >= m_flEmpiresNextVisibleMortarTime &&
			requestEmpiresMortarAtEnemyOnce(pEnemy))
		{
			m_flEmpiresNextVisibleMortarTime =
				engine->Time() + kEmpiresGrenadierMortarCooldown;
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires gameplay role=GRENADIER_MORTAR_LONG_RANGE started owner_userid=%d target_userid=%d distance=%.1f range=%.1f-%.1f context=visible-enemy action=existing-mortar-input-state-machine cooldown=%.1f engine_time=%.3f",
					engine->GetPlayerUserId(m_pEdict),
					engine->GetPlayerUserId(pEnemy), distanceFrom(pEnemy),
					kEmpiresGrenadierMortarMinRange,
					kEmpiresGrenadierMortarMaxRange,
					kEmpiresGrenadierMortarCooldown, engine->Time());
			return;
		}

		if (m_bEmpiresEngineerResupplyTaskIssued)
		{
			interruptEmpiresEngineerResupplyForCombat("enemy-acquired");
		}

		if (m_bEmpiresEngineerSupportTaskIssued)
		{
			logger->Log(LogLevel::INFO,
				"Empires gameplay engineer-support=cancelled reason=enemy-acquired building_index=%d waypoint=%d",
				m_iEmpiresEngineerSupportBuildingIndex,
				m_iEmpiresEngineerSupportWaypoint);
			m_bEmpiresEngineerSupportTaskIssued = false;
			m_bEmpiresEngineerSupportNearby = false;
			m_iEmpiresEngineerSupportBuildingIndex = -1;
			m_iEmpiresEngineerSupportWaypoint = -1;
			m_vEmpiresEngineerSupportWaypointOrigin = vec3_origin;
			m_flEmpiresNextEngineerSupportTime = engine->Time() + 15.0f;
		}
		// Tactical combat takes schedule ownership here; a live capture
		// records the cause so its resume does not spend retry budget.
		if (m_bEmpiresCaptureApproachIssued)
			m_captureInterruptionLatch.noteCombatInterruption();
		m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();

		// Use the existing graph route first, then its existing direct-move
		// task for the final leg to the live player entity.  The direct task
		// is deliberately not a new navigation system; it is the same task
		// already used by other RCBot2 schedules.  Resolve the visible enemy
		// through the same broader world-position anchor used for commander
		// orders; CFindPathTask(edict_t*) otherwise repeats the generic 400-unit
		// target lookup and can end the approach before CMoveToTask runs.
		const int approachAnchor = CWaypointLocations::NearestWaypoint(
			CBotGlobals::entityOrigin(pEnemy),
			kEmpiresWorldPositionAnchorSearchRange, -1, false, false, true,
			nullptr, false, getTeam());
		CBotSchedule* pApproachSchedule = new CBotSchedule();
		if (approachAnchor >= 0)
		{
			pApproachSchedule->addTask(new CFindPathTask(approachAnchor,
				LOOK_WAYPOINT));
		}
		else
		{
			// The target is visible, so a direct task is a valid bounded fallback
			// when the map has no usable team waypoint near that player.  Do not
			// install a route task that is guaranteed to fail and leave the bot
			// without a movement command.
			logger->Log(LogLevel::INFO,
				"Empires gameplay task=APPROACH_ENEMY route=direct-visible-target-no-anchor owner_userid=%d target_index=%d target_userid=%d distance=%.1f",
				engine != nullptr && m_pEdict != nullptr ?
					engine->GetPlayerUserId(m_pEdict) : -1,
				engine != nullptr ? engine->IndexOfEdict(pEnemy) : -1,
				engine != nullptr ? engine->GetPlayerUserId(pEnemy) : -1,
				distanceFrom(pEnemy));
		}
		pApproachSchedule->addTask(new CMoveToTask(pEnemy));
		m_pSchedules->addFront(pApproachSchedule);
		m_pEmpiresApproachTarget = pEnemy;
		m_bEmpiresApproachTaskIssued = true;
		IPlayerInfo* pEnemyInfo = playerinfomanager != nullptr ?
			playerinfomanager->GetPlayerInfo(pEnemy) : nullptr;
		logger->Log(LogLevel::INFO,
			"Empires gameplay task=APPROACH_ENEMY issued decision_phase=ACQUIRE decision=ISSUE decision_reason=visible-enemy caller_outcome=PURSUIT_ACTIVE owner_userid=%d owner_team=%d target_index=%d target_userid=%d target_team=%d capture_point=%d capture_generation=%llu capture_attempt=%llu curtime=%.3f distance=%.1f route=CBot2_CFindPathTask+CBot2_CMoveToTask",
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			getTeam(),
			engine != nullptr ? engine->IndexOfEdict(pEnemy) : -1,
			engine != nullptr ? engine->GetPlayerUserId(pEnemy) : -1,
			pEnemyInfo != nullptr ? pEnemyInfo->GetTeamIndex() : -1,
			m_iEmpiresCaptureApproachPoint,
			static_cast<unsigned long long>(m_iEmpiresCaptureObjectiveGeneration),
			static_cast<unsigned long long>(m_iEmpiresCaptureRouteAttemptGeneration),
			gpGlobals != nullptr ? gpGlobals->curtime : 0.0f,
			distanceFrom(pEnemy));
		const std::string telemetryDetail =
			RCBotEmpiresCombatTelemetry::FormatDecision(
				"ACQUIRE", "ISSUE", "visible-enemy", "PURSUIT_ACTIVE") + ";" +
			getEmpiresCombatCorrelation(pEnemy);
		EmpiresAnalysis::RecordBotEvent(this, "APPROACH_ENEMY_ISSUED", pEnemy,
			telemetryDetail.c_str());
	}
	return;
}

// Engineer skill plan (user request 2026-11-11): with two engineers one
// takes Repair Upgrade first, the other Revive; the second slot gets the
// other one, the third Turret Upgrade, the fourth Healing Upgrade. Skill codes
// from scripts EmpiresSkills (SkillCode); slots unlock with rank, the game
// keeps the wish (CEmpPlayer::ChangeSkill 0x65a530 stores slot 1-4).
void CBotEmpires::updateEmpiresEngineerSkills()
{
	const float now = gpGlobals->curtime;
	if (now < m_flEmpiresSkillsCheck)
		return;
	m_flEmpiresSkillsCheck = now + 5.0f;
	if (!isEmpiresEngineerClass())
		return;
	constexpr int kRepair = 131072, kRevive = 262144, kTurret = 524288, kHealing = 65536;
	// Rank among this team's engineer bots by userid: even = repair first.
	const int team = getTeam();
	const int myId = engine->GetPlayerUserId(m_pEdict);
	int lower = 0;
	for (int slot = 0; slot < RCBOT_MAXPLAYERS; ++slot)
	{
		CBot* bot = CBots::getBot(slot);
		if (bot == nullptr || bot == this || !bot->inUse() || bot->getEdict() == nullptr ||
			CClassInterface::getTeam(bot->getEdict()) != team)
			continue;
		const CBotEmpires* other = static_cast<const CBotEmpires*>(bot);
		if (other->isEmpiresEngineerClass() && engine->GetPlayerUserId(bot->getEdict()) < myId)
			++lower;
	}
	const bool repairFirst = (lower % 2) == 0;
	m_bEmpiresRepairRole = repairFirst;
	const int wanted[4] = {repairFirst ? kRepair : kRevive, repairFirst ? kRevive : kRepair, kTurret, kHealing};
	bool changed = false;
	for (int i = 0; i < 4; ++i)
	{
		if (m_iEmpiresSkillsSent[i] == wanted[i])
			continue;
		char command[64];
		std::snprintf(command, sizeof(command), "changeskill %d %d\n", i + 1, wanted[i]);
		helpers->ClientCommand(m_pEdict, command);
		m_iEmpiresSkillsSent[i] = wanted[i];
		changed = true;
	}
	bool repair = false, revive = false, turret = false;
	readEmpiresBoolProperty(m_pEdict, "m_bSkillIncreasedRepair", repair);
	readEmpiresBoolProperty(m_pEdict, "m_bSkillRevive", revive);
	readEmpiresBoolProperty(m_pEdict, "m_bSkillTurretUpgrade", turret);
	const int flags = (repair ? 1 : 0) | (revive ? 2 : 0) | (turret ? 4 : 0);
	// Rank points (CEmpPlayer+0x12c0); slots unlock at >0, >9, >29, >59.
	CBaseEntity* base = m_pEdict->GetUnknown() != nullptr ? m_pEdict->GetUnknown()->GetBaseEntity() : nullptr;
	if (!EmpiresBinaryOffsetsTrusted())
		base = nullptr; // rank/skill/gate offsets unverified: read as unavailable
	const int rank = base != nullptr ? *reinterpret_cast<const int*>(reinterpret_cast<const char*>(base) + 0x12c0) : -1;
	const int gates = base != nullptr ? (reinterpret_cast<const char*>(base)[0x121e] != 0 ? 8 : 0) |
		(reinterpret_cast<const char*>(base)[0x121f] != 0 ? 16 : 0) : 0;
	if (changed || (flags | gates) != m_iEmpiresSkillFlags || rank != m_iEmpiresSkillRank)
	{
		m_iEmpiresSkillFlags = flags | gates;
		m_iEmpiresSkillRank = rank;
		// Raw: wanted skills +0x1200..+0x120c, active +0x11f0..+0x11fc (UpdateSkills 0x65b220),
		// command gates +0x121e (emp_eng_upgrade) and +0x121f (emp_revive).
		const char* b = base != nullptr ? reinterpret_cast<const char*>(base) : nullptr;
		auto at = [b](int off) { return b != nullptr ? *reinterpret_cast<const int*>(b + off) : -1; };
		Msg("[RCBOT2] engineer-skills userid=%d first=%s sent=%d rank=%d active repair=%d revive=%d turret=%d "
			"want=%d,%d,%d,%d have=%d,%d,%d,%d gate_upgrade=%d gate_revive=%d\n",
			myId, repairFirst ? "repair" : "revive", changed ? 1 : 0, rank, repair ? 1 : 0, revive ? 1 : 0, turret ? 1 : 0,
			at(0x1200), at(0x1204), at(0x1208), at(0x120c), at(0x11f0), at(0x11f4), at(0x11f8), at(0x11fc),
			b != nullptr ? b[0x121e] : -1, b != nullptr ? b[0x121f] : -1);
	}
}

void CBotEmpires::modThink()
{
	updateEmpiresEngineerSkills();
	tickEmpiresVehicle();
	tickEmpiresVoidSuicide();
	if (!empiresVehicleOwnsBot() && !commanderVehicleEntryActive())
	{
		handleEmpiresRevive();
		handleEmpiresRefineryTurret();
	}
	if (m_vehicle.stage == EmpiresVehicleStage::DRIVING ||
		m_vehicle.stage == EmpiresVehicleStage::ENTERING ||
		m_vehicle.stage == EmpiresVehicleStage::STUN_EXIT ||
		m_vehicle.stage == EmpiresVehicleStage::STUN_THROW)
		return;
	// Read the authoritative Empires squad tuple and Commander MOVE SendProps,
	// then feed one canonical value snapshot into the headless squad stack.
	// An accepted Phase-10.6 parent may dispatch validated final slot targets
	// through the ordinary NavigationIntent(AUTO) path; this adapter never
	// creates a parallel path or directly manipulates movement tasks.
	// Map turrets hurt without a usable player_hurt (as_escort: no event reached
	// hurt() in three runs); watch our own health instead.
	if (m_pPlayerInfo != nullptr && !m_pPlayerInfo->IsDead())
	{
		const int health = m_pPlayerInfo->GetHealth();
		if (health > 0 && health < m_iEmpiresLastHealth)
		{
			Vector damageOrigin;
			IServerUnknown* unknown = m_pEdict->GetUnknown();
			CBaseEntity* entity = unknown != nullptr ? unknown->GetBaseEntity() : nullptr;
			const datamap_t* map = entity != nullptr ? CBaseEntity_GetDataDescMap(entity) : nullptr;
			const unsigned offset = map != nullptr ? UTIL_FindInDataMap(map, "m_DmgOrigin") : 0;
			if (offset != 0)
			{
				damageOrigin = *reinterpret_cast<const Vector*>(
					reinterpret_cast<const char*>(entity) + offset);
			}
			if (offset != 0 || !hasSomeConditions(CONDITION_SEE_CUR_ENEMY))
				noteEmpiresTurretThreat(nullptr, offset != 0 ? &damageOrigin : nullptr);
		}
		m_iEmpiresLastHealth = health;
	}
	observeEmpiresSquadRuntimeShadow();
	observeEmpiresSquadGameplay();
	updateEmpiresVacancyReviveIntervention();
	observeEmpiresRecoveryEpisode();
	if (m_bEmpiresCaptureApproachIssued && gpGlobals != nullptr &&
		gpGlobals->curtime >= m_flEmpiresNextCaptureApproachProgressLogTime)
	{
		const Vector currentOrigin = getOrigin();
		logger->Log(LogLevel::INFO,
			"Empires gameplay objective=CAPTURE_APPROACH progress point=%d owner_userid=%d origin=(%.1f %.1f %.1f) travelled=%.1f remaining=%.1f",
			m_iEmpiresCaptureApproachPoint,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			currentOrigin.x, currentOrigin.y, currentOrigin.z,
			distanceFrom(m_vEmpiresCaptureApproachStartOrigin),
			distanceFrom(m_vEmpiresCaptureApproachOrigin));
			m_flEmpiresNextCaptureApproachProgressLogTime = gpGlobals->curtime + 3.0f;
		EmpiresAnalysis::RecordRecoveryEpisodeProgress(this,
			"capture-progress");
	}

	observeEmpiresMortarSmoke();
	if (isEmpiresMortarProbeActive())
		return;

	processEmpiresEnemyLosFixture(this);
	processEmpiresScoutPriorityFixture(this);

	// The normal-path state machine runs after the schedule and navigator in
	// CBot::think(). Leave it dormant while a Commander order owns the bot, or
	// it can overwrite the freshly issued movement command.
	if (isEmpiresCommanderMoveActive())
	{
		logEmpiresCommanderMoveDiagnostic("modThink-commander", nullptr, false);
		return;
	}

	if (m_eNormalPathState == EmpiresNormalPathState::SEARCHING)
	{
		continueNormalPathSearch(false);
		return;
	}

	if (m_eNormalPathState != EmpiresNormalPathState::ARMED &&
		m_eNormalPathState != EmpiresNormalPathState::ACTIVE)
		return;

	if (m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		m_pController == nullptr || engine == nullptr || m_pNavigator == nullptr ||
		engine->IndexOfEdict(m_pEdict) != m_iNormalPathExpectedIndex ||
		engine->GetPlayerUserId(m_pEdict) != m_iNormalPathExpectedUserId ||
		m_pEdict != m_pNormalPathExpectedEdict)
	{
		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		m_iButtons = 0;
		stopMoving();
		m_eNormalPathState = EmpiresNormalPathState::FAILED;
		logger->Log(LogLevel::ERROR,
			"Empires normal path_once result=PATH_FOUND_STEERING_BLOCKED reason=client identity, controller or navigator changed");
		return;
	}

	if (m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0)
	{
		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		m_iButtons = 0;
		stopMoving();
		m_eNormalPathState = EmpiresNormalPathState::FAILED;
		logger->Log(LogLevel::ERROR,
			"Empires normal path_once result=PATH_FOUND_STEERING_BLOCKED reason=bot became dead active_frames=%d",
			m_iNormalPathActiveFrames);
		return;
	}

	if (m_eNormalPathState == EmpiresNormalPathState::ARMED)
	{
		m_eNormalPathState = EmpiresNormalPathState::ACTIVE;
		logger->Log(LogLevel::INFO,
			"Empires normal path_once movement-start start=%d destination=%d origin=(%.1f %.1f %.1f)",
			m_iNormalPathStartWaypoint, m_iNormalPathDestinationWaypoint,
			m_vNormalPathStartOrigin.x, m_vNormalPathStartOrigin.y,
			m_vNormalPathStartOrigin.z);
	}

	++m_iNormalPathActiveFrames;
	const Vector currentOrigin = getOrigin();
	const float frameDisplacement = std::sqrt(
		(currentOrigin - m_vNormalPathLastOrigin).LengthSqr());
	if (frameDisplacement > kNormalPathNoProgressEpsilon)
		m_iNormalPathNoProgressFrames = 0;
	else
		++m_iNormalPathNoProgressFrames;
	m_vNormalPathLastOrigin = currentOrigin;

	CWaypointNavigator* pNavigator = static_cast<CWaypointNavigator*>(m_pNavigator);
	const int currentWaypoint = pNavigator->hasNextPoint() ?
		pNavigator->getCurrentWaypointID() : -1;
	if (m_iNormalPathActiveFrames <= 5 ||
		m_iNormalPathActiveFrames % 30 == 0)
	{
		const CBotCmd lastCommand = m_pPlayerInfo->GetLastUserCommand();
		logger->Log(LogLevel::INFO,
			"Empires normal path_once telemetry active_frames=%d waypoint=%d origin=(%.1f %.1f %.1f) move_to_valid=%d move_to=(%.1f %.1f %.1f) ideal_speed=%.1f forward=%.1f side=%.1f up=%.1f buttons=0x%x view=(%.1f %.1f %.1f) lastcmd=(%d %d %.1f %.1f %.1f 0x%x)",
			m_iNormalPathActiveFrames, currentWaypoint,
			currentOrigin.x, currentOrigin.y, currentOrigin.z,
			moveToIsValid() ? 1 : 0, m_vMoveTo.x, m_vMoveTo.y,
			m_vMoveTo.z, m_fIdealMoveSpeed, m_fForwardSpeed,
			m_fSideSpeed, m_fUpSpeed, m_iButtons, m_vViewAngles.x,
			m_vViewAngles.y, m_vViewAngles.z, lastCommand.command_number,
			lastCommand.tick_count, lastCommand.forwardmove,
			lastCommand.sidemove, lastCommand.upmove, lastCommand.buttons);
	}
	if (currentWaypoint != m_iNormalPathLastWaypoint && currentWaypoint >= 0)
	{
		logger->Log(LogLevel::INFO,
			"Empires normal path_once waypoint-transition from=%d to=%d active_frames=%d origin=(%.1f %.1f %.1f)",
			m_iNormalPathLastWaypoint, currentWaypoint, m_iNormalPathActiveFrames,
			currentOrigin.x, currentOrigin.y, currentOrigin.z);
		m_iNormalPathLastWaypoint = currentWaypoint;
	}

	if (!pNavigator->hasNextPoint())
	{
		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		m_iButtons = 0;
		stopMoving();
		m_eNormalPathState = EmpiresNormalPathState::COMPLETE;
		const float displacement = std::sqrt(
			(currentOrigin - m_vNormalPathStartOrigin).LengthSqr());
		logger->Log(LogLevel::INFO,
			"Empires normal path_once destination-reached waypoint=%d route_nodes=%d active_frames=%d final_origin=(%.1f %.1f %.1f) displacement=%.2f movement_stopped=1 alive=1 result=PATH_FOUND_AND_TRAVERSED",
			m_iNormalPathDestinationWaypoint, static_cast<int>(m_NormalPathRoute.size()),
			m_iNormalPathActiveFrames, currentOrigin.x, currentOrigin.y, currentOrigin.z,
			displacement);
		return;
	}

	if (engine->Time() >= m_flNormalPathDeadline ||
		m_iNormalPathActiveFrames >= kNormalPathMaximumFrames)
	{
		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		m_iButtons = 0;
		stopMoving();
		m_eNormalPathState = EmpiresNormalPathState::FAILED;
		logger->Log(LogLevel::ERROR,
			"Empires normal path_once result=PATH_FOUND_STEERING_BLOCKED reason=bounded timeout active_frames=%d origin=(%.1f %.1f %.1f)",
			m_iNormalPathActiveFrames, currentOrigin.x, currentOrigin.y, currentOrigin.z);
		return;
	}

	if (m_iNormalPathNoProgressFrames >= kNormalPathNoProgressFrames)
	{
		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		m_iButtons = 0;
		stopMoving();
		m_eNormalPathState = EmpiresNormalPathState::FAILED;
		logger->Log(LogLevel::ERROR,
			"Empires normal path_once result=PATH_FOUND_STEERING_BLOCKED reason=no-progress waypoint=%d no_progress_frames=%d active_frames=%d origin=(%.1f %.1f %.1f)",
			currentWaypoint, m_iNormalPathNoProgressFrames, m_iNormalPathActiveFrames,
			currentOrigin.x, currentOrigin.y, currentOrigin.z);
	}
}

bool CBotEmpires::beginNormalRespawn()
{
	if (m_bBootstrapOnly || m_bControllerProbeOnly ||
		(!m_bNormalDeathEventPending &&
			m_eNormalSpawnState != EmpiresNormalSpawnState::COMPLETE) ||
		m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		(!m_bNormalDeathEventPending &&
			!m_pPlayerInfo->IsDead() && m_pPlayerInfo->GetHealth() > 0) ||
		engine == nullptr)
		return false;

	const int index = engine->IndexOfEdict(m_pEdict);
	const int userId = engine->GetPlayerUserId(m_pEdict);
	const char* previousPathState = normalPathStateName();
	m_bNormalDeathEventPending = false;

	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		static_cast<CWaypointNavigator*>(m_pNavigator)->clear();

	m_eNormalPathState = EmpiresNormalPathState::IDLE;
	m_bNormalPathAutoStarted = false;
	m_bNormalPathTerminalAfterStop = false;
	m_bNormalPathSearchStarted = false;
	m_iNormalPathSearchFrames = 0;
	m_iNormalPathStartWaypoint = -1;
	m_iNormalPathDestinationWaypoint = -1;
	m_iNormalPathLastWaypoint = -1;
	m_iNormalPathActiveFrames = 0;
	m_iNormalPathNoProgressFrames = 0;
	m_iNormalPathExpectedIndex = -1;
	m_iNormalPathExpectedUserId = -1;
	m_pNormalPathExpectedEdict = nullptr;
	m_flNormalPathStartTime = 0.0f;
	m_flNormalPathDeadline = 0.0f;
	m_bNormalPathLastOriginKnown = false;
	m_NormalPathRoute.clear();
	m_pEmpiresApproachTarget = nullptr;
	m_bEmpiresApproachTaskIssued = false;
	m_pEmpiresLastSpotTarget = nullptr;
	m_flEmpiresNextSpotTime = 0.0f;
	m_bEmpiresLastEnemyObscured = false;
	m_bEmpiresLastEnemySearchActive = false;
	m_bEmpiresLastEnemySearchTaskLogged = false;

	m_eNormalSpawnState = EmpiresNormalSpawnState::IDLE;
	m_iNormalSpawn = -1;
	m_iNormalSpawnFrames = 0;
	m_flNormalSpawnDeadline = 0.0f;
	m_flNormalSpawnObserveDeadline = 0.0f;
	m_flNormalSpawnConfirmEarliestTime = 0.0f;
	m_iNormalSpawnExpectedIndex = -1;
	m_iNormalSpawnExpectedUserId = -1;
	m_pNormalSpawnExpectedEdict = nullptr;
	m_pNormalSpawnSelectionEdict = nullptr;
	m_bNormalSpawnEligibilityLogged = false;
	m_bNormalSpawnLastLifeStateKnown = false;
	m_iNormalSpawnLastLifeState = -1;
	m_bNormalSpawnLastTimerKnown = false;
	m_flNormalSpawnLastTimer = 0.0f;

	logger->Log(LogLevel::INFO,
		"Empires normal lifecycle=DEAD->RESPAWN_PREP index=%d userid=%d previous_path_state=%s respawn_cycle=repeatable spawn_state=IDLE class_preserved=%d team_preserved=%d",
		index, userId, previousPathState, m_iDesiredClass,
		getTeam());
	return true;
}

int CBotEmpires::getTeam()
{
	// Controller probes intentionally do not retain IPlayerInfo. The existing
	// waypoint search calls CBot::getTeam() repeatedly, so use the team that
	// was validated immediately before this bounded path test instead of
	// dereferencing the deliberately empty base pointer.
	if (m_bControllerProbeOnly && m_pPlayerInfo == nullptr)
		return m_iControllerProbePathTeam;

	return CBot::getTeam();
}

bool CBotEmpires::canGotoWaypoint(const Vector& vPrevWaypoint,
	CWaypoint* pWaypoint, CWaypoint* pPrev)
{
	// CBot::canGotoWaypoint() reads getHealthPercent() for fall waypoints.
	// That sensor is not available on the controller probe. Reject only this
	// input-requiring route condition for the probe; normal bots retain the
	// established implementation unchanged.
	if (m_bControllerProbeOnly && m_pPlayerInfo == nullptr && pWaypoint != nullptr &&
		pWaypoint->hasFlag(CWaypointTypes::W_FL_FALL))
		return false;

	return CBot::canGotoWaypoint(vPrevWaypoint, pWaypoint, pPrev);
}

const char* CBotEmpires::controllerProbeLifecycleName() const
{
	switch (m_eControllerProbeLifecycle)
	{
	case EmpiresControllerProbeLifecycle::INACTIVE:
		return "INACTIVE";
	case EmpiresControllerProbeLifecycle::ENGINE_BOT_CREATED:
		return "ENGINE_BOT_CREATED";
	case EmpiresControllerProbeLifecycle::SLOT_BOUND:
		return "SLOT_BOUND";
	case EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED:
		return "CONTROLLER_VERIFIED";
	case EmpiresControllerProbeLifecycle::REMOVAL_PENDING:
		return "REMOVAL_PENDING";
	}

	return "UNKNOWN";
}

edict_t* CBotEmpires::getCurrentControlledTestEdict() const
{
	if (!isControlledTestOnly() || engine == nullptr || gpGlobals == nullptr)
		return nullptr;

	const int index = m_bControllerProbeOnly ? m_iControllerProbeClientIndex :
		(m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1);
	if (index <= 0 || index > gpGlobals->maxClients)
		return nullptr;

	edict_t* pCurrentEdict = engine->PEntityOfEntIndex(index);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree())
		return nullptr;

	if (m_bControllerProbeOnly && m_iControllerProbeSlot != index - 1)
		return nullptr;

	return pCurrentEdict;
}

edict_t* CBotEmpires::getCurrentBootstrapEdict() const
{
	return getCurrentControlledTestEdict();
}

bool CBotEmpires::readEmpiresIntProperty(const edict_t* pEdict,
	const char* propertyName, int& value) const
{
	value = -1;
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || servergamedll == nullptr || propertyName == nullptr)
		return false;

	const ServerClass* playerClass = UTIL_FindServerClass("CEmpPlayer");
	if (playerClass == nullptr || playerClass->m_pTable == nullptr)
		return false;

	SendProp* prop = UTIL_FindSendProp(playerClass->m_pTable, propertyName);
	if (prop == nullptr || prop->GetType() != DPT_Int || prop->GetNumElements() != 1)
		return false;

	unsigned offset = 0;
	if (!UTIL_FindSendPropInfo(playerClass, propertyName, &offset) || offset == 0)
		return false;

	CClassInterfaceValue propertyValue("CEmpPlayer", propertyName, 0);
	propertyValue.findOffset();
	// The pinned SDK2013 declares CBaseEntity::m_lifeState as a char, even
	// though it is transported by a DPT_Int SendProp. Reading it as an int
	// consumes adjacent bytes and produced the invalid runtime value 512.
	if (std::strcmp(propertyName, "m_lifeState") == 0)
	{
		byte* currentValue = propertyValue.getBytePointer(pCurrentEdict);
		if (currentValue == nullptr)
			return false;

		value = static_cast<int>(*currentValue);
		return true;
	}

	int* currentValue = propertyValue.getIntPointer(pCurrentEdict);
	if (currentValue == nullptr)
		return false;

	value = *currentValue;
	return true;
}

bool CBotEmpires::readBootstrapIntProperty(const char* propertyName, int& value) const
{
	return readEmpiresIntProperty(getCurrentBootstrapEdict(), propertyName, value);
}

bool CBotEmpires::readEmpiresFloatProperty(const edict_t* pEdict,
	const char* propertyName, float& value) const
{
	value = 0.0f;
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || servergamedll == nullptr || propertyName == nullptr)
		return false;

	const ServerClass* playerClass = UTIL_FindServerClass("CEmpPlayer");
	if (playerClass == nullptr || playerClass->m_pTable == nullptr)
		return false;

	SendProp* prop = UTIL_FindSendProp(playerClass->m_pTable, propertyName);
	if (prop == nullptr || prop->GetType() != DPT_Float || prop->GetNumElements() != 1)
		return false;

	unsigned offset = 0;
	if (!UTIL_FindSendPropInfo(playerClass, propertyName, &offset) || offset == 0)
		return false;

	CClassInterfaceValue propertyValue("CEmpPlayer", propertyName, 0);
	propertyValue.findOffset();
	float* currentValue = propertyValue.getFloatPointer(pCurrentEdict);
	if (currentValue == nullptr)
		return false;

	value = *currentValue;
	return true;
}

bool CBotEmpires::readBootstrapFloatProperty(const char* propertyName, float& value) const
{
	return readEmpiresFloatProperty(getCurrentBootstrapEdict(), propertyName, value);
}

bool CBotEmpires::readEmpiresBoolProperty(const edict_t* pEdict,
	const char* propertyName, bool& value) const
{
	value = false;
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || servergamedll == nullptr || propertyName == nullptr)
		return false;

	const ServerClass* playerClass = UTIL_FindServerClass("CEmpPlayer");
	if (playerClass == nullptr || playerClass->m_pTable == nullptr)
		return false;

	SendProp* prop = UTIL_FindSendProp(playerClass->m_pTable, propertyName);
	if (prop == nullptr || prop->GetType() != DPT_Int || prop->GetNumElements() != 1)
		return false;

	unsigned offset = 0;
	if (!UTIL_FindSendPropInfo(playerClass, propertyName, &offset) || offset == 0)
		return false;

	CClassInterfaceValue propertyValue("CEmpPlayer", propertyName, 0);
	propertyValue.findOffset();
	bool* currentValue = propertyValue.getBoolPointer(pCurrentEdict);
	if (currentValue == nullptr)
		return false;

	value = *currentValue;
	return true;
}

bool CBotEmpires::readEmpiresWeaponBoolProperty(const edict_t* pEdict,
	const char* propertyName, bool& value) const
{
	value = false;
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree() ||
		servergamedll == nullptr || propertyName == nullptr)
	{
		return false;
	}

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

	bool* pValue = reinterpret_cast<bool*>(reinterpret_cast<char*>(pEntity) + offset);
	value = *pValue;
	return true;
}

bool CBotEmpires::readEmpiresWeaponFloatProperty(const edict_t* pEdict,
	const char* propertyName, float& value) const
{
	value = 0.0f;
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree() ||
		servergamedll == nullptr || propertyName == nullptr)
	{
		return false;
	}

	IServerUnknown* pUnknown = pCurrentEdict->GetUnknown();
	IServerNetworkable* pNetworkable = pUnknown != nullptr ?
		pUnknown->GetNetworkable() : nullptr;
	const ServerClass* pClass = pNetworkable != nullptr ?
		pNetworkable->GetServerClass() : nullptr;
	if (pClass != nullptr && pClass->m_pTable != nullptr)
	{
		SendProp* pProp = UTIL_FindSendProp(pClass->m_pTable, propertyName);
		unsigned offset = 0;
		if (pProp != nullptr && pProp->GetType() == DPT_Float &&
			pProp->GetNumElements() == 1 &&
			UTIL_FindSendPropInfo(pClass, propertyName, &offset) && offset != 0)
		{
			CClassInterfaceValue propertyValue(pClass->m_pNetworkName,
				propertyName, 0);
			propertyValue.findOffset();
			float* pValue = propertyValue.getFloatPointer(pCurrentEdict);
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

	float* pValue = reinterpret_cast<float*>(reinterpret_cast<char*>(pEntity) + offset);
	value = *pValue;
	return true;
}

namespace
{
// Entity index of a handle, -1 if the handle is invalid or stale: the edict
// serial (low NUM_NETWORKED_EHANDLE_SERIAL_NUMBER_BITS bits of the handle
// serial, see CBaseEntity::NetworkProp in the SDK) must match the slot.
int CheckedHandleIndex(const CBaseHandle* handle)
{
	if (handle == nullptr || engine == nullptr || !handle->IsValid())
		return -1;
	const int index = handle->GetEntryIndex();
	edict_t* edict = engine->PEntityOfEntIndex(index);
	if (edict == nullptr || edict->IsFree())
		return -1;
	const int mask = (1 << NUM_NETWORKED_EHANDLE_SERIAL_NUMBER_BITS) - 1;
	return (edict->m_NetworkSerialNumber & mask) == (handle->GetSerialNumber() & mask) ? index : -1;
}
}

bool CBotEmpires::readEmpiresWeaponHandleProperty(const edict_t* pEdict,
	const char* propertyName, int& entityIndex) const
{
	entityIndex = -1;
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree() ||
		servergamedll == nullptr || propertyName == nullptr)
	{
		return false;
	}

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
			CBaseHandle* pHandle = propertyValue.getEntityHandle(pCurrentEdict);
			if (pHandle != nullptr)
			{
				entityIndex = CheckedHandleIndex(pHandle);
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

	CBaseHandle* pHandle = reinterpret_cast<CBaseHandle*>(
		reinterpret_cast<char*>(pEntity) + offset);
	entityIndex = CheckedHandleIndex(pHandle);
	return true;
}

bool CBotEmpires::readEmpiresEntityHandleProperty(const edict_t* pEdict,
	const char* propertyName, const int element, int& entityIndex) const
{
	entityIndex = -1;
	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree() ||
		servergamedll == nullptr || propertyName == nullptr || element < 0)
	{
		return false;
	}

	IServerUnknown* pUnknown = pCurrentEdict->GetUnknown();
	IServerNetworkable* pNetworkable = pUnknown != nullptr ?
		pUnknown->GetNetworkable() : nullptr;
	const ServerClass* pClass = pNetworkable != nullptr ?
		pNetworkable->GetServerClass() : nullptr;
	if (pClass == nullptr || pClass->m_pTable == nullptr)
		return false;

	SendProp* pProp = UTIL_FindSendProp(pClass->m_pTable, propertyName);
	unsigned offset = 0;
	if (pProp == nullptr || !UTIL_FindSendPropInfo(pClass, propertyName, &offset) ||
		offset == 0)
	{
		return false;
	}

	if (pProp->GetType() == DPT_Int && pProp->GetNumElements() == 1)
	{
		if (element != 0)
			return false;

		CClassInterfaceValue propertyValue(pClass->m_pNetworkName,
			propertyName, 0);
		propertyValue.findOffset();
		CBaseHandle* pHandle = propertyValue.getEntityHandle(pCurrentEdict);
		if (pHandle == nullptr)
			return false;

		entityIndex = CheckedHandleIndex(pHandle);
		return true;
	}

	if (pProp->GetType() != DPT_Array ||
		pProp->GetArrayProp() == nullptr ||
		pProp->GetArrayProp()->GetType() != DPT_Int ||
		element >= pProp->GetNumElements())
	{
		return false;
	}

	// UTIL_FindSendPropInfo returns the dynamic base offset of the array.
	// The SDK2013 SendProp stride is the authoritative element layout; no
	// fixed offset or guessed vehicle layout is used here.
	const unsigned elementOffset = offset +
		static_cast<unsigned>(pProp->GetElementStride() * element);
	CBaseEntity* pEntity = pUnknown != nullptr ? pUnknown->GetBaseEntity() : nullptr;
	if (pEntity == nullptr)
		return false;

	CBaseHandle* pHandle = reinterpret_cast<CBaseHandle*>(
		reinterpret_cast<char*>(pEntity) + elementOffset);
	entityIndex = CheckedHandleIndex(pHandle);
	return true;
}

bool CBotEmpires::findEmpiresVehicleFactory(const int team,
	edict_t*& factory, Vector& origin, int& factoryTeam) const
{
	factory = nullptr;
	origin = vec3_origin;
	factoryTeam = -1;
	if (servertools == nullptr || engine == nullptr ||
		(team != EMP_TEAM_NF && team != EMP_TEAM_BE))
	{
		return false;
	}

	float bestDistance = std::numeric_limits<float>::max();
	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pEdict = pNetworkable != nullptr ?
			pNetworkable->GetEdict() : nullptr;
		if (pEdict == nullptr || pEdict->IsFree() ||
			pEdict->GetClassName() == nullptr)
		{
			continue;
		}

		const char* className = pEdict->GetClassName();
		const bool isGenericFactory =
			std::strcmp(className, "emp_building_vehiclefactory") == 0;
		const bool isNfFactory =
			std::strcmp(className, "emp_building_nf_vehiclefactory") == 0;
		const bool isBeFactory =
			std::strcmp(className, "emp_building_imp_vehiclefactory") == 0;
		if (!isGenericFactory && !isNfFactory && !isBeFactory)
			continue;

		int candidateTeam = -1;
		const bool teamKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iTeamNum", candidateTeam);
		if (teamKnown && candidateTeam != team)
			continue;
		if (!teamKnown && ((isNfFactory && team != EMP_TEAM_NF) ||
			(isBeFactory && team != EMP_TEAM_BE)))
		{
			continue;
		}

		int health = -1;
		int maxHealth = -1;
		bool fullyBuilt = false;
		const bool healthKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iHealth", health);
		const bool maxHealthKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iMaxHealth", maxHealth);
		const bool fullyBuiltKnown = readEmpiresEntityBoolProperty(
			pEdict, "m_bFullyBuilt", fullyBuilt);
		if ((healthKnown && health <= 0) ||
			(healthKnown && maxHealthKnown && maxHealth > 0 && health < maxHealth) ||
			(fullyBuiltKnown && !fullyBuilt))
		{
			continue;
		}

		const Vector candidateOrigin = CBotGlobals::entityOrigin(pEdict);
		const float distance = (candidateOrigin - getOrigin()).LengthSqr();
		if (distance >= bestDistance)
			continue;

		bestDistance = distance;
		factory = pEdict;
		origin = candidateOrigin;
		factoryTeam = teamKnown ? candidateTeam : team;
	}

	return factory != nullptr;
}

bool CBotEmpires::findEmpiresTutorialExit(const int team,
	edict_t*& exitTrigger, Vector& origin) const
{
	exitTrigger = nullptr;
	origin = vec3_origin;
	if (servertools == nullptr || engine == nullptr ||
		(team != EMP_TEAM_NF && team != EMP_TEAM_BE))
	{
		return false;
	}

	const char* targetName = team == EMP_TEAM_NF ?
		"leave_tutorial_nf" : "leave_tutorial_imp";
	float bestDistance = std::numeric_limits<float>::max();
	for (CBaseEntity* pEntity = servertools->FirstEntity();
		pEntity != nullptr; pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pCandidate = pNetworkable != nullptr ?
			pNetworkable->GetEdict() : nullptr;
		if (pCandidate == nullptr || pCandidate->IsFree() ||
			pCandidate->GetClassName() == nullptr ||
			std::strcmp(pCandidate->GetClassName(), "trigger_teleport") != 0)
		{
			continue;
		}

		char candidateTargetName[128] = {};
		if (!servertools->GetKeyValue(pEntity, "targetname",
			candidateTargetName, sizeof(candidateTargetName)) ||
			std::strcmp(candidateTargetName, targetName) != 0)
		{
			continue;
		}

		const Vector candidateOrigin = CBotGlobals::entityOrigin(pCandidate);
		const float distance = (candidateOrigin - getOrigin()).LengthSqr();
		if (distance >= bestDistance)
			continue;

		bestDistance = distance;
		exitTrigger = pCandidate;
		origin = candidateOrigin;
	}

	return exitTrigger != nullptr;
}

bool CBotEmpires::findEmpiresVehicle(const int team, edict_t*& vehicle,
	Vector& origin, int& vehicleTeam, int& passenger0,
	const std::vector<int>* excludedIndices) const
{
	vehicle = nullptr;
	origin = vec3_origin;
	vehicleTeam = -1;
	passenger0 = -1;
	if (servertools == nullptr || engine == nullptr ||
		(team != EMP_TEAM_NF && team != EMP_TEAM_BE))
	{
		return false;
	}

	float bestDistance = std::numeric_limits<float>::max();
	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pEdict = pNetworkable != nullptr ?
			pNetworkable->GetEdict() : nullptr;
		if (pEdict == nullptr || pEdict->IsFree() ||
			pEdict->GetClassName() == nullptr ||
			std::strcmp(pEdict->GetClassName(), "emp_vehicle") != 0)
		{
			continue;
		}

		const int candidateIndex = engine->IndexOfEdict(pEdict);
		bool excluded = false;
		if (excludedIndices != nullptr)
		{
			for (const int excludedIndex : *excludedIndices)
			{
				if (excludedIndex == candidateIndex)
				{
					excluded = true;
					break;
				}
			}
		}
		if (excluded)
			continue;

		int candidateTeam = -1;
		const bool teamKnown = readEmpiresEntityIntProperty(
			pEdict, "m_iTeamNum", candidateTeam);
		if (teamKnown && candidateTeam != team)
			continue;

		int candidatePassenger0 = -1;
		if (!readEmpiresEntityHandleProperty(pEdict, "m_hPassengers", 0,
			candidatePassenger0) || candidatePassenger0 >= 0)
		{
			continue;
		}

		const Vector candidateOrigin = CBotGlobals::entityOrigin(pEdict);
		const float distance = (candidateOrigin - getOrigin()).LengthSqr();
		if (distance >= bestDistance)
			continue;

		bestDistance = distance;
		vehicle = pEdict;
		origin = candidateOrigin;
		vehicleTeam = teamKnown ? candidateTeam : -1;
		passenger0 = candidatePassenger0;
	}

	return vehicle != nullptr;
}

bool CBotEmpires::isEmpiresVehicleActionActive() const
{
	return m_eEmpiresVehicleActionState != EmpiresVehicleActionState::IDLE &&
		m_eEmpiresVehicleActionState != EmpiresVehicleActionState::COMPLETE &&
		m_eEmpiresVehicleActionState != EmpiresVehicleActionState::FAILED;
}

void CBotEmpires::failEmpiresVehicleAction(const char* reason)
{
	if (!isEmpiresVehicleActionActive())
		return;

	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		static_cast<CWaypointNavigator*>(m_pNavigator)->clear();
	if (m_pButtons != nullptr)
	{
		m_pButtons->letGo(IN_USE);
		m_pButtons->letGo(IN_FORWARD);
		m_pButtons->letGo(IN_BACK);
		m_pButtons->letGo(IN_MOVELEFT);
		m_pButtons->letGo(IN_MOVERIGHT);
	}
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	stopMoving();

	if (logger != nullptr)
	{
		logger->Log(LogLevel::WARN,
			"Empires vehicle action=result=FAIL index=%d userid=%d vehicle_index=%d vehicle_team=%d frames=%d moved=%d reason=%s path=normal-RunPlayerMove",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
			engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
			m_iEmpiresVehicleActionVehicleIndex,
			m_iEmpiresVehicleActionVehicleTeam,
			m_iEmpiresVehicleActionFrames,
			m_bEmpiresVehicleActionMoved ? 1 : 0,
			reason != nullptr ? reason : "unspecified");
	}

	m_eEmpiresVehicleActionState = EmpiresVehicleActionState::FAILED;
}

bool CBotEmpires::requestEmpiresBuildAssignment(edict_t* building)
{
	// A started refinery-turret job finishes first (user rule 2026-11-12: no detours).
	if (m_iTurretRefinery != 0 && building != nullptr && !building->IsFree() &&
		engine != nullptr && engine->IndexOfEdict(building) != m_iTurretBuilding)
		return false;
	if (m_iBuildAssignStage != 0 || building == nullptr || building->IsFree() ||
		building->GetUnknown() == nullptr || m_pSchedules == nullptr || !isAlive() ||
		isControlledTestOnly() || commanderVehicleEntryActive() || engine == nullptr)
		return false;

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown, classPropertyName) ||
		classId != EMP_CLASS_ENGINEER)
		return false;

	Vector spot = vec3_origin;
	if (!findEmpiresBuildStandSpot(building, spot))
		return false;

	// A synthetic move (Commander escort toward a resource point) owns getTasks ahead of
	// the assignment; the building is the reason for the walk, so it takes over.
	if (m_bEmpiresCommanderMoveTestActive ||
		(m_bEmpiresCommanderMoveRouteIssued && m_bEmpiresCommanderMoveSynthetic))
		clearEmpiresCommanderMoveState("build-assignment");

	m_hBuildAssignBuilding = building;
	m_vBuildAssignSpot = spot;
	m_iBuildAssignStage = 1;
	m_iBuildAssignAttempts = 0;
	m_fBuildAssignStart = engine->Time();
	m_fBuildAssignNextTry = 0.0f;
	cancelNavigationAttempt("build-assignment-replaced");
	m_pSchedules->freeMemory();
	queueEmpiresBuildAssignRoute();
	Msg("[RCBOT2] build-assign stage=approach userid=%d building=%d distance=%.1f spot=(%.0f %.0f %.0f)\n",
		engine->GetPlayerUserId(m_pEdict), engine->IndexOfEdict(building),
		distanceFrom(building), spot.x, spot.y, spot.z);
	return true;
}

// The Engineer kit traces only 100 units from the eye (server binary,
// CWeaponNFEngineerKit::PrimaryAttack), so what counts is the distance to the
// building's collision surface, not to its origin.
bool CBotEmpires::findEmpiresKitAim(edict_t* building, const Vector& eye, Vector& aim, const bool keepAim) const
{
	constexpr float kReach = 92.0f;
	if (enginetrace == nullptr || building == nullptr || building->IsFree() ||
		building->GetIServerEntity() == nullptr)
		return false;
	const Vector center = CBotGlobals::entityOrigin(building);
	const float yaw = std::atan2(center.y - eye.y, center.x - eye.x);
	CTraceFilterWorldOnly worldOnly;
	float best = kReach;
	bool found = false;
	// Keep the aim of the last frame while it still works, so the view rests.
	Vector keep = aim - eye;
	if (keepAim && VectorNormalize(keep) > 1.0f)
	{
		Ray_t ray;
		ray.Init(eye, eye + keep * kReach);
		trace_t hit, world;
		enginetrace->ClipRayToEntity(ray, MASK_SHOT, building->GetIServerEntity(), &hit);
		enginetrace->TraceRay(ray, MASK_SHOT, &worldOnly, &world);
		if (!hit.startsolid && hit.fraction < 1.0f && world.fraction >= hit.fraction)
			return true;
	}
	for (const float yawOffset : {0.0f, -0.35f, 0.35f, -0.7f, 0.7f})
	{
		for (const float rise : {0.0f, -0.35f, 0.35f, -0.7f})
		{
			Vector dir(std::cos(yaw + yawOffset), std::sin(yaw + yawOffset), rise);
			VectorNormalize(dir);
			Ray_t ray;
			ray.Init(eye, eye + dir * kReach);
			trace_t hit;
			enginetrace->ClipRayToEntity(ray, MASK_SHOT, building->GetIServerEntity(), &hit);
			if (hit.startsolid || hit.fraction >= 1.0f || hit.fraction * kReach >= best)
				continue;
			trace_t world;
			enginetrace->TraceRay(ray, MASK_SHOT, &worldOnly, &world);
			if (world.fraction < hit.fraction)
				continue;
			best = hit.fraction * kReach;
			aim = eye + dir * (best + 8.0f);
			found = true;
		}
	}
	return found;
}

bool CBotEmpires::findEmpiresBuildStandSpot(edict_t* building, Vector& spot) const
{
	const Vector center = CBotGlobals::entityOrigin(building);
	const Vector eyeOffset = getEyePosition() - getOrigin();
	const ICollideable* collide = building->GetCollideable();
	const float outer = (collide != nullptr ?
		std::max(collide->OBBMins().Length2D(), collide->OBBMaxs().Length2D()) : 1000.0f) + 200.0f;
	CBaseEntity* const buildingEntity = building->GetUnknown()->GetBaseEntity();
	CCommanderEntryTraceFilter filter(m_pEdict->GetUnknown());
	float spotCost = std::numeric_limits<float>::max();
	int noSurface = 0, noGround = 0, blocked = 0, noAim = 0;
	for (int i = 0; i < 32; ++i)
	{
		const float angle = i * (6.28318530718f / 32.0f);
		const Vector dir(std::cos(angle), std::sin(angle), 0.0f);
		for (const float height : {40.0f, 100.0f})
		{
			// Outer wall as seen from outside in this direction.
			Ray_t inward;
			inward.Init(center + dir * outer + Vector(0.0f, 0.0f, height), center + Vector(0.0f, 0.0f, height));
			trace_t wall;
			enginetrace->ClipRayToEntity(inward, MASK_PLAYERSOLID, building->GetIServerEntity(), &wall);
			if (wall.startsolid || wall.fraction >= 1.0f)
			{
				++noSurface;
				continue;
			}
			const Vector stand = wall.endpos + dir * 30.0f;
			Ray_t down;
			down.Init(stand + Vector(0.0f, 0.0f, 80.0f), stand - Vector(0.0f, 0.0f, 240.0f));
			trace_t ground;
			enginetrace->TraceRay(down, MASK_PLAYERSOLID, &filter, &ground);
			if (ground.startsolid || ground.fraction >= 1.0f || ground.m_pEnt == buildingEntity)
			{
				++noGround;
				continue;
			}
			Ray_t body;
			body.Init(ground.endpos + Vector(0.0f, 0.0f, 2.0f), ground.endpos + Vector(0.0f, 0.0f, 3.0f),
				Vector(-16.0f, -16.0f, 0.0f), Vector(16.0f, 16.0f, 72.0f));
			trace_t bodyHit;
			enginetrace->TraceRay(body, MASK_PLAYERSOLID, &filter, &bodyHit);
			if (bodyHit.startsolid || bodyHit.fraction < 1.0f)
			{
				++blocked;
				continue;
			}
			Vector aim;
			if (!findEmpiresKitAim(building, ground.endpos + eyeOffset, aim))
			{
				++noAim;
				continue;
			}
			const float cost = (ground.endpos - getOrigin()).LengthSqr();
			if (cost < spotCost)
			{
				spotCost = cost;
				spot = ground.endpos;
			}
		}
	}
	if (spotCost == std::numeric_limits<float>::max())
	{
		Msg("[RCBOT2] build-assign result=REJECT reason=no-standing-spot building=%d no_surface=%d no_ground=%d blocked=%d no_aim=%d\n",
			engine->IndexOfEdict(building), noSurface, noGround, blocked, noAim);
		return false;
	}
	return true;
}

void CBotEmpires::queueEmpiresBuildAssignRoute()
{
	const Vector here = getOrigin();
	CBotSchedule* route = new CBotSchedule();
	route->addTask(new CFindPathTask(m_vBuildAssignSpot));
	route->addTask(new CMoveToTask(m_vBuildAssignSpot));
	m_pSchedules->addFront(route);
	Msg("[RCBOT2] build-assign route from=(%.0f %.0f %.0f)\n", here.x, here.y, here.z);
}

// Commander "Guard unit" (m_iCommandType 4, target entity in
// m_iCommandTarget[0]; reverse-engineering/notes/commander-order-types.md).
// An Engineer guarding a damaged friendly building goes and repairs it with
// the existing build-assignment flow. The order itself stays set by the game.
// Test-only: lower a building's health so a test Guard order has work to do.
void CBotEmpires::damageEmpiresTestGuardBuilding(int entIndex)
{
	if (engine == nullptr || entIndex <= 0 || entIndex >= engine->GetEntityCount())
		return;
	edict_t* building = engine->PEntityOfEntIndex(entIndex);
	ServerClass* serverClass = building != nullptr && !building->IsFree() &&
		building->GetNetworkable() != nullptr ? building->GetNetworkable()->GetServerClass() : nullptr;
	if (serverClass == nullptr)
		return;
	CClassInterfaceValue healthValue(serverClass->m_pNetworkName, "m_iHealth", 0);
	healthValue.findOffset();
	int* health = healthValue.getIntPointer(building);
	if (health == nullptr)
		return;
	*health = 100;
	Msg("[RCBOT2] guard-order test damaged building %d to 100\n", entIndex);
}

void CBotEmpires::tickEmpiresGuardOrder()
{
	constexpr int kGuardType = 4;
	if (engine == nullptr || m_pEdict == nullptr || m_pPlayerInfo == nullptr ||
		!m_pPlayerInfo->IsFakeClient() || m_iBuildAssignStage != 0 || !isAlive())
		return;
	const float now = engine->Time();
	if (now < m_fEmpiresGuardNextCheck)
		return;
	m_fEmpiresGuardNextCheck = now + 3.0f;

	int commandType = -1;
	if (m_iEmpiresTestGuardTarget > 0)
		commandType = kGuardType;
	else if (!readEmpiresIntProperty(m_pEdict, "m_iCommandType", commandType) ||
		commandType != kGuardType)
	{
		m_iEmpiresGuardLoggedTarget = -1;
		return;
	}
	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown, classPropertyName) ||
		classId != EMP_CLASS_ENGINEER)
		return;

	// m_iCommandTarget is an int[32]; element 0 sits 0x10 behind m_iCommandType.
	int targetIndex = m_iEmpiresTestGuardTarget;
	unsigned typeOffset = 0;
	unsigned locationOffset = 0;
	ServerClass* serverClass = m_pEdict->GetNetworkable() != nullptr ?
		m_pEdict->GetNetworkable()->GetServerClass() : nullptr;
	CBaseEntity* self = m_pEdict->GetUnknown() != nullptr ?
		m_pEdict->GetUnknown()->GetBaseEntity() : nullptr;
	if (targetIndex <= 0)
	{
		if (serverClass == nullptr || self == nullptr ||
			!UTIL_FindSendPropInfo(serverClass, "m_iCommandType", &typeOffset) ||
			!UTIL_FindSendPropInfo(serverClass, "m_vCommandLocation", &locationOffset) ||
			typeOffset == 0 || locationOffset != typeOffset + 4)
			return;
		targetIndex = *reinterpret_cast<const int*>(
			reinterpret_cast<const char*>(self) + typeOffset + 0x10);
	}
	if (targetIndex <= 0 || targetIndex >= engine->GetEntityCount())
		return;
	edict_t* target = engine->PEntityOfEntIndex(targetIndex);
	if (target == nullptr || target->IsFree() || target->GetClassName() == nullptr ||
		std::strncmp(target->GetClassName(), "emp_building", 12) != 0 ||
		CClassInterface::getTeam(target) != getTeam())
		return;
	int health = -1;
	int maxHealth = -1;
	if (!readEmpiresEntityIntProperty(target, "m_iHealth", health) ||
		!readEmpiresEntityIntProperty(target, "m_iMaxHealth", maxHealth) ||
		health >= maxHealth)
		return;
	const bool started = requestEmpiresBuildAssignment(target);
	if (m_iEmpiresGuardLoggedTarget != targetIndex)
	{
		m_iEmpiresGuardLoggedTarget = targetIndex;
		Msg("[RCBOT2] guard-order userid=%d target=%d class=%s health=%d/%d started=%d\n",
			engine->GetPlayerUserId(m_pEdict), targetIndex, target->GetClassName(),
			health, maxHealth, started ? 1 : 0);
	}
}

void CBotEmpires::tickEmpiresBuildAssignment()
{
	if (m_iBuildAssignStage == 0 || engine == nullptr)
		return;

	const float now = engine->Time();
	const float elapsed = now - m_fBuildAssignStart;
	edict_t* building = m_hBuildAssignBuilding.get();
	int health = -1;
	int maxHealth = -1;
	const bool readable = building != nullptr && !building->IsFree() &&
		readEmpiresEntityIntProperty(building, "m_iHealth", health) &&
		readEmpiresEntityIntProperty(building, "m_iMaxHealth", maxHealth);

	const auto finish = [&](const char* result)
	{
		Msg("[RCBOT2] build-assign result=%s userid=%d elapsed=%.1f attempts=%d health=%d max_health=%d distance=%.1f\n",
			result, engine->GetPlayerUserId(m_pEdict), elapsed, m_iBuildAssignAttempts, health, maxHealth,
			building != nullptr ? distanceFrom(building) : -1.0f);
		m_iBuildAssignStage = 0;
	};

	if (!readable)
	{
		finish("FAIL_INVALID");
		return;
	}
	if (!isAlive())
		return;
	if (health >= maxHealth)
	{
		finish("COMPLETE");
		return;
	}
	if (elapsed > 480.0f)
	{
		finish("FAIL_TIMEOUT");
		return;
	}
	if (isEmpiresEngineerRepairActionActive())
	{
		if (now >= m_fBuildAssignNextLog)
		{
			m_fBuildAssignNextLog = now + 5.0f;
			int clip = -1;
			edict_t* weapon = CClassInterface::getCurrentWeapon(m_pEdict);
			if (weapon != nullptr)
				readEmpiresEntityIntProperty(weapon, "m_iClip1", clip);
			Msg("[RCBOT2] build-assign progress t=%.0f health=%d/%d clip=%d\n", elapsed, health, maxHealth, clip);
		}
		return;
	}

	const float distance = distanceFrom(building);
	const Vector toSpot = m_vBuildAssignSpot - getOrigin();
	Vector aim;
	// Arrived means the kit can actually reach the building from here.
	if (!findEmpiresKitAim(building, getEyePosition(), aim))
	{
		m_iBuildAssignStage = 1;
		if (m_pSchedules == nullptr || !m_pSchedules->isEmpty())
			return;
		if (toSpot.Length2D() <= 200.0f && std::fabs(toSpot.z) <= 48.0f && toSpot.Length2D() > 8.0f)
		{
			// The route task stops up to 64 short; walk the rest directly.
			setMoveTo(m_vBuildAssignSpot);
		}
		else if (now >= m_fBuildAssignNextTry)
		{
			m_fBuildAssignNextTry = now + 2.0f;
			// A stuck attempt blocks this goal for 15 s, although a fresh route
			// from where the bot now stands usually works at once (recorder,
			// 2026-11-10: ~20 s lost inside the spawn barracks).
			clearNavigationAttemptCooldownForStartAnchorRetry();
			queueEmpiresBuildAssignRoute();
		}
		return;
	}

	if (m_iBuildAssignStage == 1)
	{
		cancelNavigationAttempt("build-assignment-arrived");
		if (m_pSchedules != nullptr)
			m_pSchedules->freeMemory();
		stopMoving();
		m_iBuildAssignStage = 2;
		Msg("[RCBOT2] build-assign stage=arrived elapsed=%.1f distance=%.1f health=%d/%d\n",
			elapsed, distance, health, maxHealth);
	}

	if (now < m_fBuildAssignNextTry)
		return;
	m_fBuildAssignNextTry = now + 1.0f;
	++m_iBuildAssignAttempts;
	if (m_iBuildAssignAttempts > 20)
	{
		finish("FAIL_ATTEMPTS");
		return;
	}
	const bool started = requestEmpiresEngineerRepairOnce(engine->IndexOfEdict(building), true);
	Msg("[RCBOT2] build-assign stage=work attempt=%d started=%d health=%d/%d distance=%.1f\n",
		m_iBuildAssignAttempts, started ? 1 : 0, health, maxHealth, distance);
}

bool CBotEmpires::requestCommanderVehicleEntryOnce()
{
	if (m_iCommanderEntryStage != 0 || isControlledTestOnly() || !isAlive() ||
		engine == nullptr || gpGlobals == nullptr || helpers == nullptr ||
		m_pButtons == nullptr || m_pSchedules == nullptr ||
		isEmpiresCommanderMoveActive() || isEmpiresVehicleActionActive())
		return false;

	edict_t* target = nullptr;
	float nearest = 32768.0f * 32768.0f;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; ++i)
	{
		edict_t* entity = engine->PEntityOfEntIndex(i);
		if (entity == nullptr || entity->IsFree() || entity->GetUnknown() == nullptr ||
			CClassInterface::getTeam(entity) != getTeam())
			continue;
		const char* name = entity->GetClassName();
		if (name == nullptr || (std::strcmp(name, "emp_nf_commander") != 0 &&
			std::strcmp(name, "emp_imp_commander") != 0))
			continue;
		const float distance = (CBotGlobals::entityOrigin(entity) - getOrigin()).LengthSqr();
		if (distance < nearest)
		{
			nearest = distance;
			target = entity;
		}
	}
	if (target == nullptr)
	{
		Msg("[RCBOT2] commander-entry result=REJECT reason=no-command-vehicle team=%d\n", getTeam());
		return false;
	}

	// The vehicle origin is inside its collision, so neither waypoints nor the
	// navmesh can route to it. Find a real standing spot beside it instead.
	const Vector center = CBotGlobals::entityOrigin(target);
	const Vector eyeOffset = getEyePosition() - getOrigin();
	Vector standSpot = vec3_origin;
	float standCost = std::numeric_limits<float>::max();
	for (int i = 0; i < 16; ++i)
	{
		const float angle = i * (6.28318530718f / 16.0f);
		const Vector top = center + Vector(std::cos(angle) * 136.0f, std::sin(angle) * 136.0f, 64.0f);
		Ray_t ray;
		ray.Init(top, top - Vector(0.0f, 0.0f, 256.0f));
		CCommanderEntryTraceFilter filter(m_pEdict->GetUnknown());
		trace_t ground;
		enginetrace->TraceRay(ray, MASK_SOLID, &filter, &ground);
		// Only world ground: a spot on a building (run 151900: the new VF beside the CV, z +73)
		// lies inside its detour box and the route never reaches it.
		edict_t* world = engine->PEntityOfEntIndex(0);
		if (ground.startsolid || ground.allsolid || ground.fraction >= 1.0f ||
			ground.m_pEnt == target->GetUnknown()->GetBaseEntity() ||
			(world != nullptr && world->GetUnknown() != nullptr && ground.m_pEnt != world->GetUnknown()->GetBaseEntity()))
			continue;
		trace_t use;
		if (!TraceCommanderEntryUse(m_pEdict, target, ground.endpos + eyeOffset, use))
			continue;
		const float cost = (ground.endpos - getOrigin()).LengthSqr();
		if (cost < standCost)
		{
			standCost = cost;
			standSpot = ground.endpos;
		}
	}
	if (standCost == std::numeric_limits<float>::max())
	{
		Msg("[RCBOT2] commander-entry result=REJECT reason=no-standing-spot vehicle=%d\n",
			engine->IndexOfEdict(target));
		return false;
	}

	m_hCommanderEntryVehicle = target;
	m_iCommanderEntryUserId = engine->GetPlayerUserId(m_pEdict);
	m_fCommanderEntryDeadline = gpGlobals->curtime + 180.0f;
	m_iCommanderEntryStage = 1;
	m_vCommanderEntrySpot = standSpot;
	cancelNavigationAttempt("commander-entry-replaced");
	m_pSchedules->freeMemory();
	m_pSchedules->addFront(new CBotGotoOriginSched(standSpot));
	Msg("[RCBOT2] commander-entry stage=approach userid=%d vehicle=%d distance=%.1f spot=(%.0f %.0f %.0f)\n",
		m_iCommanderEntryUserId, engine->IndexOfEdict(target), std::sqrt(nearest),
		standSpot.x, standSpot.y, standSpot.z);
	return true;
}

void CBotEmpires::handleCommanderVehicleEntry()
{
	if (!commanderVehicleEntryActive())
		return;

	edict_t* vehicle = m_hCommanderEntryVehicle.get();
	const bool stale = vehicle == nullptr || m_pEdict == nullptr || m_pEdict->IsFree() ||
		engine == nullptr || engine->GetPlayerUserId(m_pEdict) != m_iCommanderEntryUserId ||
		!isAlive() || CClassInterface::getTeam(vehicle) != getTeam();
	if (stale || (m_iCommanderEntryStage < 4 && gpGlobals->curtime > m_fCommanderEntryDeadline))
	{
		Msg("[RCBOT2] commander-entry result=FAIL reason=%s stage=%d distance=%.1f\n",
			stale ? "stale-context" : "deadline", m_iCommanderEntryStage,
			vehicle != nullptr ? (CBotGlobals::entityOrigin(vehicle) - getOrigin()).Length() : -1.0f);
		m_iCommanderEntryStage = 0;
		cancelNavigationAttempt("commander-entry-failed");
		m_pSchedules->freeMemory();
		m_pButtons->letGo(IN_USE);
		stopMoving();
		return;
	}

	const auto readCommanderMode = [this](bool& commander)
	{
		EmpiresNativePlacement::ContextIdentity identity;
		return EmpiresNativePlacement::CaptureContext(engine->IndexOfEdict(m_pEdict), identity) &&
			EmpiresNativePlacement::ReadCommanderMode(identity, commander);
	};

	if (m_iCommanderEntryStage == 1)
	{
		const float distance = (CBotGlobals::entityOrigin(vehicle) - getOrigin()).Length();
		// A stuck route ends the schedule; keep trying until the deadline.
		if (distance > 200.0f && m_pSchedules->isEmpty() &&
			gpGlobals->curtime >= m_fCommanderEntryNextReplan)
		{
			m_fCommanderEntryNextReplan = gpGlobals->curtime + 2.0f;
			m_pSchedules->addFront(new CBotGotoOriginSched(m_vCommanderEntrySpot));
			Msg("[RCBOT2] commander-entry stage=replan distance=%.1f\n", distance);
		}
		if (distance > 200.0f)
			return;
		trace_t hit;
		if (!TraceCommanderEntryUse(m_pEdict, vehicle, getEyePosition(), hit))
		{
			// The route task stops up to 64 short of the spot, where the Use ray
			// may miss the hull; walk the rest directly.
			const Vector toSpot = m_vCommanderEntrySpot - getOrigin();
			if (m_pSchedules->isEmpty() && toSpot.Length2D() > 8.0f && std::fabs(toSpot.z) <= 48.0f)
				setMoveTo(m_vCommanderEntrySpot);
			return;
		}
		cancelNavigationAttempt("commander-entry-use");
		m_pSchedules->freeMemory();
		stopMoving();
		m_pButtons->tap(IN_USE);
		m_iCommanderEntryStage = 2;
		Msg("[RCBOT2] commander-entry stage=use distance=%.1f\n", distance);
		return;
	}

	if (m_iCommanderEntryStage == 2)
	{
		int currentVehicle = -1;
		if (!readEmpiresWeaponHandleProperty(m_pEdict, "m_hVehicle", currentVehicle) ||
			currentVehicle != engine->IndexOfEdict(vehicle))
			return;
		m_pButtons->letGo(IN_USE);
		helpers->ClientCommand(m_pEdict, "emp_change_seat 1\n");
		m_iCommanderEntryStage = 3;
		Msg("[RCBOT2] commander-entry stage=seat vehicle=%d\n", currentVehicle);
		return;
	}

	bool commander = false;
	if (m_iCommanderEntryStage == 3)
	{
		if (readCommanderMode(commander) && commander)
		{
			m_iCommanderEntryStage = 4;
			stopMoving();
			Msg("[RCBOT2] commander-entry result=COMPLETE userid=%d vehicle=%d native_commander=1\n",
				m_iCommanderEntryUserId, engine->IndexOfEdict(vehicle));
		}
		return;
	}

	if (readCommanderMode(commander) && !commander)
	{
		Msg("[RCBOT2] commander-entry result=LEFT_SEAT userid=%d\n", m_iCommanderEntryUserId);
		m_iCommanderEntryStage = 0;
	}
}

bool CBotEmpires::requestEmpiresVehicleFactoryDriveOnce()
{
	if (m_bBootstrapOnly || m_bControllerProbeOnly ||
		isEmpiresVehicleActionActive() || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		m_pController == nullptr || m_pSchedules == nullptr ||
		m_pNavigator == nullptr || engine == nullptr || gpGlobals == nullptr ||
		!m_pPlayerInfo->IsFakeClient() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0)
	{
		return false;
	}

	edict_t* pFactory = nullptr;
	Vector factoryOrigin = vec3_origin;
	int factoryTeam = -1;
	if (!findEmpiresVehicleFactory(getTeam(), pFactory, factoryOrigin,
		factoryTeam))
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires vehicle action=result=REJECT reason=no-constructed-team-vf team=%d",
				getTeam());
		return false;
	}

	m_EmpiresVehicleActionBaselineVehicleIndices.clear();
	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pEdict = pNetworkable != nullptr ?
			pNetworkable->GetEdict() : nullptr;
		if (pEdict != nullptr && !pEdict->IsFree() &&
			pEdict->GetClassName() != nullptr &&
			std::strcmp(pEdict->GetClassName(), "emp_vehicle") == 0)
		{
			m_EmpiresVehicleActionBaselineVehicleIndices.push_back(
				engine->IndexOfEdict(pEdict));
		}
	}

	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	static_cast<CWaypointNavigator*>(m_pNavigator)->clear();
	m_bFailNextMove = false;
	m_fWaypointStuckTime = 0.0f;
	m_fLastWaypointVisible = 0.0f;

	m_iEmpiresVehicleActionExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iEmpiresVehicleActionExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pEmpiresVehicleActionExpectedEdict = m_pEdict;
	m_pEmpiresVehicleActionFactory = pFactory;
	m_pEmpiresVehicleActionTutorialExit = nullptr;
	m_iEmpiresVehicleActionTutorialExitIndex = -1;
	m_vEmpiresVehicleActionTutorialExitOrigin = vec3_origin;
	m_vEmpiresVehicleActionTutorialEntryOrigin = getOrigin();
	m_flEmpiresVehicleActionTutorialTeleportDeadline = 0.0f;
	m_iEmpiresVehicleActionFactoryIndex = engine->IndexOfEdict(pFactory);
	m_iEmpiresVehicleActionFactoryTeam = factoryTeam;
	m_vEmpiresVehicleActionFactoryOrigin = factoryOrigin;
	m_iEmpiresVehicleActionPurchaseStep = 0;
	m_flEmpiresVehicleActionNextPurchaseCommand = 0.0f;
	m_pEmpiresVehicleActionVehicle = nullptr;
	m_iEmpiresVehicleActionVehicleIndex = -1;
	m_iEmpiresVehicleActionVehicleTeam = -1;
	m_iEmpiresVehicleActionPassenger0 = -1;
	m_iEmpiresVehicleActionFrames = 0;
	m_flEmpiresVehicleActionDeadline = engine->Time() +
		kEmpiresVehicleActionSeconds;
	m_flEmpiresVehicleActionDriveStart = 0.0f;
	m_vEmpiresVehicleActionStartOrigin = factoryOrigin;
	m_vEmpiresVehicleActionLastOrigin = factoryOrigin;
	m_qEmpiresVehicleActionDriveAngles = vec3_angle;
	m_bEmpiresVehicleActionMoved = false;
	m_bEmpiresVehicleActionUseLogged = false;
	m_bEmpiresVehicleActionExitUseLogged = false;
	edict_t* pTutorialExit = nullptr;
	Vector tutorialExitOrigin = vec3_origin;
	const bool hasTutorialExit = findEmpiresTutorialExit(getTeam(),
		pTutorialExit, tutorialExitOrigin);
	if (hasTutorialExit)
	{
		m_pEmpiresVehicleActionTutorialExit = pTutorialExit;
		m_iEmpiresVehicleActionTutorialExitIndex =
			engine->IndexOfEdict(pTutorialExit);
		m_vEmpiresVehicleActionTutorialExitOrigin = tutorialExitOrigin;
		m_eEmpiresVehicleActionState =
			EmpiresVehicleActionState::APPROACHING_TUTORIAL_EXIT;
	}
	else
	{
		m_eEmpiresVehicleActionState =
			EmpiresVehicleActionState::APPROACHING_FACTORY;
	}

	const Vector routeOrigin = hasTutorialExit ? tutorialExitOrigin :
		factoryOrigin;
	const int anchor = CWaypointLocations::NearestWaypoint(routeOrigin,
		kEmpiresVehicleRouteSearchRange, -1, false, false, true,
		nullptr, false, getTeam());
	CBotSchedule* pSchedule = new CBotSchedule();
	if (anchor >= 0)
		pSchedule->addTask(new CFindPathTask(anchor, LOOK_WAYPOINT));
	pSchedule->addTask(new CMoveToTask(routeOrigin));
	m_pSchedules->addFront(pSchedule);

	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires vehicle action=started index=%d userid=%d tutorial_exit_index=%d tutorial_exit_origin=(%.1f %.1f %.1f) tutorial_required=%d factory_index=%d factory_team=%d factory_origin=(%.1f %.1f %.1f) baseline_vehicles=%zu anchor=%d factory_inside_radius=%.1f deadline=%.1f path=CBot2_CFindPathTask+CBot2_CMoveToTask normal-input=IN_USE+ClientCommand+RunPlayerMove",
			m_iEmpiresVehicleActionExpectedIndex,
			m_iEmpiresVehicleActionExpectedUserId,
			m_iEmpiresVehicleActionTutorialExitIndex,
			tutorialExitOrigin.x, tutorialExitOrigin.y, tutorialExitOrigin.z,
			hasTutorialExit ? 1 : 0,
			m_iEmpiresVehicleActionFactoryIndex,
			m_iEmpiresVehicleActionFactoryTeam,
			factoryOrigin.x, factoryOrigin.y, factoryOrigin.z,
			m_EmpiresVehicleActionBaselineVehicleIndices.size(), anchor,
			kEmpiresVehicleFactoryInsideRadius,
			m_flEmpiresVehicleActionDeadline);
	}
	return true;
}

void CBotEmpires::handleEmpiresVehicleAction()
{
	if (!isEmpiresVehicleActionActive())
		return;

	++m_iEmpiresVehicleActionFrames;
	if (m_pEdict == nullptr || m_pEdict->IsFree() ||
		m_pPlayerInfo == nullptr || m_pController == nullptr ||
		engine == nullptr || gpGlobals == nullptr ||
		engine->IndexOfEdict(m_pEdict) != m_iEmpiresVehicleActionExpectedIndex ||
		engine->GetPlayerUserId(m_pEdict) != m_iEmpiresVehicleActionExpectedUserId ||
		m_pEdict != m_pEmpiresVehicleActionExpectedEdict)
	{
		failEmpiresVehicleAction("client-identity-changed");
		return;
	}

	if (engine->Time() >= m_flEmpiresVehicleActionDeadline)
	{
		failEmpiresVehicleAction("bounded-deadline");
		return;
	}

	edict_t* pFactory = m_pEmpiresVehicleActionFactory.get();
	const bool beforePurchase =
		m_eEmpiresVehicleActionState == EmpiresVehicleActionState::APPROACHING_TUTORIAL_EXIT ||
		m_eEmpiresVehicleActionState == EmpiresVehicleActionState::WAITING_FOR_TUTORIAL_TELEPORT ||
		m_eEmpiresVehicleActionState == EmpiresVehicleActionState::APPROACHING_FACTORY ||
		m_eEmpiresVehicleActionState == EmpiresVehicleActionState::OPENING_FACTORY ||
		m_eEmpiresVehicleActionState == EmpiresVehicleActionState::CONFIGURING_PURCHASE ||
		m_eEmpiresVehicleActionState == EmpiresVehicleActionState::WAITING_FOR_PURCHASE;
	if (beforePurchase && (pFactory == nullptr || pFactory->IsFree() ||
		engine->IndexOfEdict(pFactory) != m_iEmpiresVehicleActionFactoryIndex))
	{
		failEmpiresVehicleAction("factory-invalid-or-replaced-before-purchase");
		return;
	}

	edict_t* pTutorialExit = m_pEmpiresVehicleActionTutorialExit.get();
	const bool inTutorialPhase =
		m_eEmpiresVehicleActionState ==
			EmpiresVehicleActionState::APPROACHING_TUTORIAL_EXIT ||
		m_eEmpiresVehicleActionState ==
			EmpiresVehicleActionState::WAITING_FOR_TUTORIAL_TELEPORT;
	if (inTutorialPhase && (pTutorialExit == nullptr || pTutorialExit->IsFree() ||
		engine->IndexOfEdict(pTutorialExit) !=
			m_iEmpiresVehicleActionTutorialExitIndex))
	{
		failEmpiresVehicleAction("tutorial-exit-invalid-before-teleport");
		return;
	}

	edict_t* pVehicle = m_pEmpiresVehicleActionVehicle.get();
	const bool afterPurchase = !beforePurchase;
	if (afterPurchase && (pVehicle == nullptr || pVehicle->IsFree() ||
		engine->IndexOfEdict(pVehicle) != m_iEmpiresVehicleActionVehicleIndex))
	{
		failEmpiresVehicleAction("purchased-vehicle-invalid-or-replaced");
		return;
	}

	int playerVehicle = -1;
	const bool playerVehicleKnown = readEmpiresWeaponHandleProperty(
		m_pEdict, "m_hVehicle", playerVehicle);
	const Vector factoryOrigin = m_vEmpiresVehicleActionFactoryOrigin;
	const float distanceToFactory = (factoryOrigin - getOrigin()).Length();
	const float distanceToTutorialExit = inTutorialPhase ?
		(m_vEmpiresVehicleActionTutorialExitOrigin - getOrigin()).Length() :
		std::numeric_limits<float>::max();
	const bool tutorialTeleportObserved = inTutorialPhase &&
		((getOrigin() - m_vEmpiresVehicleActionTutorialEntryOrigin).Length() >=
			kEmpiresVehicleTutorialTeleportDistance) &&
		distanceToTutorialExit > kEmpiresVehicleTutorialExitRadius * 2.0f;
	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::APPROACHING_TUTORIAL_EXIT &&
		(m_iEmpiresVehicleActionFrames <= 5 ||
			m_iEmpiresVehicleActionFrames % 30 == 0))
	{
		logger->Log(LogLevel::INFO,
			"Empires vehicle action=approach-telemetry frame=%d origin=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f) distance=%.1f move_to_valid=%d forward=%.1f side=%.1f up=%.1f buttons=0x%x schedule_empty=%d",
			m_iEmpiresVehicleActionFrames, getOrigin().x, getOrigin().y,
			getOrigin().z, m_vEmpiresVehicleActionTutorialExitOrigin.x,
			m_vEmpiresVehicleActionTutorialExitOrigin.y,
			m_vEmpiresVehicleActionTutorialExitOrigin.z,
			distanceToTutorialExit, moveToIsValid() ? 1 : 0, m_fForwardSpeed,
			m_fSideSpeed, m_fUpSpeed, m_iButtons,
			m_pSchedules != nullptr && m_pSchedules->isEmpty() ? 1 : 0);
	}
	const Vector vehicleOrigin = afterPurchase ?
		CBotGlobals::entityOrigin(pVehicle) : vec3_origin;
	const float distanceToVehicle = afterPurchase ?
		(vehicleOrigin - getOrigin()).Length() : 0.0f;

	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::APPROACHING_TUTORIAL_EXIT)
	{
		if (tutorialTeleportObserved)
		{
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires vehicle action=tutorial-complete teleport=normal-trigger_teleport origin=(%.1f %.1f %.1f) distance_from_entry=%.1f",
					getOrigin().x, getOrigin().y, getOrigin().z,
					(getOrigin() - m_vEmpiresVehicleActionTutorialEntryOrigin).Length());
			if (m_pSchedules != nullptr)
				m_pSchedules->freeMemory();
			m_bFailNextMove = false;
			m_fWaypointStuckTime = 0.0f;
			m_fLastWaypointVisible = 0.0f;
			m_eEmpiresVehicleActionState =
				EmpiresVehicleActionState::APPROACHING_FACTORY;
			CBotSchedule* pSchedule = new CBotSchedule();
			const int anchor = CWaypointLocations::NearestWaypoint(factoryOrigin,
				kEmpiresVehicleRouteSearchRange, -1, false, false, true,
				nullptr, false, getTeam());
			if (anchor >= 0)
				pSchedule->addTask(new CFindPathTask(anchor, LOOK_WAYPOINT));
			pSchedule->addTask(new CMoveToTask(factoryOrigin));
			m_pSchedules->addFront(pSchedule);
			return;
		}

		if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
			return;
		if (distanceToTutorialExit > kEmpiresVehicleTutorialExitRadius)
		{
			failEmpiresVehicleAction("tutorial-exit-route-ended-too-far");
			return;
		}

		m_eEmpiresVehicleActionState =
			EmpiresVehicleActionState::WAITING_FOR_TUTORIAL_TELEPORT;
		m_flEmpiresVehicleActionTutorialTeleportDeadline = engine->Time() +
			kEmpiresVehicleTutorialTeleportWait;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires vehicle action=tutorial-exit-entered tutorial_exit_index=%d distance=%.1f normal-input=RunPlayerMove awaiting=trigger_teleport",
				m_iEmpiresVehicleActionTutorialExitIndex, distanceToTutorialExit);
		return;
	}

	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::WAITING_FOR_TUTORIAL_TELEPORT)
	{
		if (tutorialTeleportObserved)
		{
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires vehicle action=tutorial-complete teleport=normal-trigger_teleport origin=(%.1f %.1f %.1f) distance_from_entry=%.1f",
					getOrigin().x, getOrigin().y, getOrigin().z,
					(getOrigin() - m_vEmpiresVehicleActionTutorialEntryOrigin).Length());
			if (m_pSchedules != nullptr)
				m_pSchedules->freeMemory();
			m_bFailNextMove = false;
			m_fWaypointStuckTime = 0.0f;
			m_fLastWaypointVisible = 0.0f;
			m_eEmpiresVehicleActionState =
				EmpiresVehicleActionState::APPROACHING_FACTORY;
			CBotSchedule* pSchedule = new CBotSchedule();
			const int anchor = CWaypointLocations::NearestWaypoint(factoryOrigin,
				kEmpiresVehicleRouteSearchRange, -1, false, false, true,
				nullptr, false, getTeam());
			if (anchor >= 0)
				pSchedule->addTask(new CFindPathTask(anchor, LOOK_WAYPOINT));
			pSchedule->addTask(new CMoveToTask(factoryOrigin));
			m_pSchedules->addFront(pSchedule);
			return;
		}
		if (engine->Time() >= m_flEmpiresVehicleActionTutorialTeleportDeadline)
		{
			failEmpiresVehicleAction("tutorial-exit-did-not-teleport");
			return;
		}
		return;
	}

	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::APPROACHING_FACTORY)
	{
		if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
			return;

		if (distanceToFactory > kEmpiresVehicleFactoryInsideRadius)
		{
			failEmpiresVehicleAction("vf-route-ended-outside-factory");
			return;
		}

		if (m_pButtons != nullptr && m_pButtons->canPressButton(IN_USE))
		{
			m_pButtons->tap(IN_USE);
			m_eEmpiresVehicleActionState =
				EmpiresVehicleActionState::OPENING_FACTORY;
			m_flEmpiresVehicleActionNextPurchaseCommand = engine->Time() + 0.25f;
			if (logger != nullptr && !m_bEmpiresVehicleActionUseLogged)
			{
				logger->Log(LogLevel::INFO,
					"Empires vehicle action=factory-use-submitted index=%d userid=%d factory_index=%d distance=%.1f inside=1 path=normal-RunPlayerMove",
					m_iEmpiresVehicleActionExpectedIndex,
					m_iEmpiresVehicleActionExpectedUserId,
					m_iEmpiresVehicleActionFactoryIndex, distanceToFactory);
				m_bEmpiresVehicleActionUseLogged = true;
			}
		}
		return;
	}

	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::OPENING_FACTORY)
	{
		if (engine->Time() < m_flEmpiresVehicleActionNextPurchaseCommand)
			return;
		if (helpers == nullptr)
		{
			failEmpiresVehicleAction("client-command-helper-unavailable");
			return;
		}
		helpers->ClientCommand(m_pEdict,
			"emp_show_vehicle_customization\n");
		m_iEmpiresVehicleActionPurchaseStep = 0;
		m_eEmpiresVehicleActionState =
			EmpiresVehicleActionState::CONFIGURING_PURCHASE;
		m_flEmpiresVehicleActionNextPurchaseCommand = engine->Time() +
			kEmpiresVehiclePurchaseCommandInterval;
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires vehicle action=vf-opened index=%d userid=%d factory_index=%d inside=1 normal-command=emp_show_vehicle_customization",
				m_iEmpiresVehicleActionExpectedIndex,
				m_iEmpiresVehicleActionExpectedUserId,
				m_iEmpiresVehicleActionFactoryIndex);
		return;
	}

	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::CONFIGURING_PURCHASE)
	{
		if (engine->Time() < m_flEmpiresVehicleActionNextPurchaseCommand)
			return;
		if (helpers == nullptr)
		{
			failEmpiresVehicleAction("client-command-helper-unavailable");
			return;
		}

		char command[256];
		command[0] = '\0';
		const char* commandName = "unknown";
		switch (m_iEmpiresVehicleActionPurchaseStep)
		{
		case 0:
			std::snprintf(command, sizeof(command),
				"emp_vehicle_factory %d\n",
				m_iEmpiresVehicleActionFactoryIndex);
			commandName = "emp_vehicle_factory";
			break;
		case 1:
			std::snprintf(command, sizeof(command),
				"emp_vehicle_chassis 0\n");
			commandName = "emp_vehicle_chassis";
			break;
		case 2:
			std::snprintf(command, sizeof(command),
				"emp_vehicle_weapon 0 0 0 0 0 0 0 0 0\n");
			commandName = "emp_vehicle_weapon";
			break;
		case 3:
			std::snprintf(command, sizeof(command),
				"emp_vehicle_armor 0 0\n");
			commandName = "emp_vehicle_armor";
			break;
		case 4:
			std::snprintf(command, sizeof(command),
				"emp_vehicle_armor_levels 0 0 0 0\n");
			commandName = "emp_vehicle_armor_levels";
			break;
		case 5:
			std::snprintf(command, sizeof(command),
				"emp_vehicle_engine 0 0\n");
			commandName = "emp_vehicle_engine";
			break;
		case 6:
			std::snprintf(command, sizeof(command),
				"emp_vehicle_build\n");
			commandName = "emp_vehicle_build";
			break;
		default:
			failEmpiresVehicleAction("invalid-purchase-command-step");
			return;
		}

		helpers->ClientCommand(m_pEdict, command);
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires vehicle action=purchase-command step=%d factory_index=%d command=%s path=normal-ClientCommand",
				m_iEmpiresVehicleActionPurchaseStep,
				m_iEmpiresVehicleActionFactoryIndex, commandName);
		if (m_iEmpiresVehicleActionPurchaseStep == 6)
		{
			m_eEmpiresVehicleActionState =
				EmpiresVehicleActionState::WAITING_FOR_PURCHASE;
		}
		else
		{
			++m_iEmpiresVehicleActionPurchaseStep;
			m_flEmpiresVehicleActionNextPurchaseCommand = engine->Time() +
				kEmpiresVehiclePurchaseCommandInterval;
		}
		return;
	}

	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::WAITING_FOR_PURCHASE)
	{
		edict_t* purchasedVehicle = nullptr;
		Vector purchasedOrigin = vec3_origin;
		int purchasedTeam = -1;
		int purchasedPassenger0 = -1;
		if (!findEmpiresVehicle(getTeam(), purchasedVehicle, purchasedOrigin,
			purchasedTeam, purchasedPassenger0,
			&m_EmpiresVehicleActionBaselineVehicleIndices))
		{
			return;
		}

		m_pEmpiresVehicleActionVehicle = purchasedVehicle;
		m_iEmpiresVehicleActionVehicleIndex = engine->IndexOfEdict(
			purchasedVehicle);
		m_iEmpiresVehicleActionVehicleTeam = purchasedTeam;
		m_iEmpiresVehicleActionPassenger0 = purchasedPassenger0;
		m_eEmpiresVehicleActionState =
			EmpiresVehicleActionState::APPROACHING_VEHICLE;
		m_bFailNextMove = false;
		m_fWaypointStuckTime = 0.0f;
		m_fLastWaypointVisible = 0.0f;
		CBotSchedule* pSchedule = new CBotSchedule();
		pSchedule->addTask(new CMoveToTask(purchasedOrigin));
		m_pSchedules->addFront(pSchedule);
		if (logger != nullptr)
			logger->Log(LogLevel::INFO,
				"Empires vehicle action=purchase-observed factory_index=%d vehicle_index=%d vehicle_team=%d passenger0=%d baseline_vehicles=%zu path=live-emp_vehicle",
				m_iEmpiresVehicleActionFactoryIndex,
				m_iEmpiresVehicleActionVehicleIndex,
				m_iEmpiresVehicleActionVehicleTeam,
				m_iEmpiresVehicleActionPassenger0,
				m_EmpiresVehicleActionBaselineVehicleIndices.size());
		return;
	}

	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::APPROACHING_VEHICLE)
	{
		if (m_pSchedules != nullptr && !m_pSchedules->isEmpty())
			return;
		if (distanceToVehicle > kEmpiresVehicleApproachRadius)
		{
			failEmpiresVehicleAction("purchased-vehicle-route-ended-too-far");
			return;
		}
		if (m_pButtons != nullptr && m_pButtons->canPressButton(IN_USE))
		{
			m_pButtons->tap(IN_USE);
			m_eEmpiresVehicleActionState =
				EmpiresVehicleActionState::WAITING_FOR_ENTRY;
			if (logger != nullptr)
				logger->Log(LogLevel::INFO,
					"Empires vehicle action=entry-use-submitted index=%d userid=%d vehicle_index=%d purchased=1 distance=%.1f path=normal-RunPlayerMove",
					m_iEmpiresVehicleActionExpectedIndex,
					m_iEmpiresVehicleActionExpectedUserId,
					m_iEmpiresVehicleActionVehicleIndex,
					distanceToVehicle);
		}
		return;
	}

	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::WAITING_FOR_ENTRY)
	{
		if (!playerVehicleKnown)
		{
			failEmpiresVehicleAction("player-vehicle-handle-unreadable");
			return;
		}
		if (playerVehicle != m_iEmpiresVehicleActionVehicleIndex)
			return;

		int driverIndex = -1;
		if (!readEmpiresEntityHandleProperty(pVehicle, "m_hPassengers", 0,
			driverIndex))
		{
			failEmpiresVehicleAction("driver-seat-sendprop-unreadable");
			return;
		}
		if (driverIndex != m_iEmpiresVehicleActionExpectedIndex)
		{
			failEmpiresVehicleAction("entered-non-driver-seat");
			return;
		}

		m_vEmpiresVehicleActionStartOrigin = vehicleOrigin;
		m_vEmpiresVehicleActionLastOrigin = vehicleOrigin;
		m_flEmpiresVehicleActionDriveStart = engine->Time();
		m_qEmpiresVehicleActionDriveAngles = m_vViewAngles;
		if (pVehicle->GetCollideable() != nullptr)
		{
			const QAngle vehicleAngles =
				pVehicle->GetCollideable()->GetCollisionAngles();
			if (vehicleAngles.IsValid())
				m_qEmpiresVehicleActionDriveAngles = vehicleAngles;
		}
		m_qEmpiresVehicleActionDriveAngles.x = 0.0f;
		m_qEmpiresVehicleActionDriveAngles.z = 0.0f;
		m_eEmpiresVehicleActionState = EmpiresVehicleActionState::DRIVING;
		if (logger != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires vehicle action=entered index=%d userid=%d vehicle_index=%d driver_seat=0 driver_index=%d origin=(%.1f %.1f %.1f) path=normal-RunPlayerMove",
				m_iEmpiresVehicleActionExpectedIndex,
				m_iEmpiresVehicleActionExpectedUserId,
				m_iEmpiresVehicleActionVehicleIndex, driverIndex,
				vehicleOrigin.x, vehicleOrigin.y, vehicleOrigin.z);
		}
		return;
	}

	if (m_eEmpiresVehicleActionState == EmpiresVehicleActionState::DRIVING)
	{
		if (!playerVehicleKnown ||
			playerVehicle != m_iEmpiresVehicleActionVehicleIndex)
		{
			failEmpiresVehicleAction("vehicle-handle-lost-during-drive");
			return;
		}

		int driverIndex = -1;
		if (!readEmpiresEntityHandleProperty(pVehicle, "m_hPassengers", 0,
			driverIndex) ||
			driverIndex != m_iEmpiresVehicleActionExpectedIndex)
		{
			failEmpiresVehicleAction("driver-seat-lost-during-drive");
			return;
		}

		const float displacement = (vehicleOrigin -
			m_vEmpiresVehicleActionStartOrigin).Length();
		if (displacement >= kEmpiresVehicleMovementThreshold)
			m_bEmpiresVehicleActionMoved = true;
		m_vEmpiresVehicleActionLastOrigin = vehicleOrigin;

		if (engine->Time() < m_flEmpiresVehicleActionDriveStart +
			kEmpiresVehicleDriveSeconds)
		{
			m_fForwardSpeed = kEmpiresVehicleDriveSpeed;
			m_fSideSpeed = 0.0f;
			m_fUpSpeed = 0.0f;
			return;
		}

		if (!m_bEmpiresVehicleActionMoved)
		{
			failEmpiresVehicleAction("drive-produced-no-open-geometry-displacement");
			return;
		}

		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		if (m_pButtons != nullptr && m_pButtons->canPressButton(IN_USE))
		{
			m_pButtons->tap(IN_USE);
			m_eEmpiresVehicleActionState =
				EmpiresVehicleActionState::WAITING_FOR_EXIT;
			if (logger != nullptr && !m_bEmpiresVehicleActionExitUseLogged)
			{
				logger->Log(LogLevel::INFO,
					"Empires vehicle action=exit-use-submitted index=%d userid=%d vehicle_index=%d displacement=%.1f path=normal-RunPlayerMove",
					m_iEmpiresVehicleActionExpectedIndex,
					m_iEmpiresVehicleActionExpectedUserId,
					m_iEmpiresVehicleActionVehicleIndex, displacement);
				m_bEmpiresVehicleActionExitUseLogged = true;
			}
		}
		return;
	}

	if (m_eEmpiresVehicleActionState ==
		EmpiresVehicleActionState::WAITING_FOR_EXIT)
	{
		m_fForwardSpeed = 0.0f;
		m_fSideSpeed = 0.0f;
		m_fUpSpeed = 0.0f;
		if (!playerVehicleKnown || playerVehicle >= 0)
			return;

		if (logger != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires vehicle action=result=PASS index=%d userid=%d vehicle_index=%d displacement=%.1f stopped=1 exited=1 path=normal-RunPlayerMove",
				m_iEmpiresVehicleActionExpectedIndex,
				m_iEmpiresVehicleActionExpectedUserId,
				m_iEmpiresVehicleActionVehicleIndex,
				(vehicleOrigin - m_vEmpiresVehicleActionStartOrigin).Length());
		}
		m_eEmpiresVehicleActionState = EmpiresVehicleActionState::COMPLETE;
	}
}


bool CBotEmpires::readEmpiresDebugIntProperty(const edict_t* pEdict,
	const char* propertyName, int& value) const
{
	value = 0;
	if (pEdict == nullptr || pEdict->IsFree() || propertyName == nullptr)
		return false;

	// Prefer the already-validated Empires send-table lookup.  The button
	// bookkeeping fields are server-side datamap fields, so fall back to the
	// same runtime datamap resolver used by the existing probe readers.
	if (readEmpiresIntProperty(pEdict, propertyName, value))
		return true;

	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	IServerUnknown* pUnknown = pCurrentEdict->GetUnknown();
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

bool CBotEmpires::readEmpiresDebugFloatProperty(const edict_t* pEdict,
	const char* propertyName, float& value) const
{
	value = 0.0f;
	if (pEdict == nullptr || pEdict->IsFree() || propertyName == nullptr)
		return false;

	if (readEmpiresFloatProperty(pEdict, propertyName, value))
		return true;

	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	IServerUnknown* pUnknown = pCurrentEdict->GetUnknown();
	CBaseEntity* pEntity = pUnknown != nullptr ? pUnknown->GetBaseEntity() : nullptr;
	if (pEntity == nullptr)
		return false;

	datamap_t* pMap = CBaseEntity_GetDataDescMap(pEntity);
	const unsigned offset = pMap != nullptr ?
		UTIL_FindInDataMap(pMap, propertyName) : 0;
	if (offset == 0)
		return false;

	value = *reinterpret_cast<float*>(reinterpret_cast<char*>(pEntity) + offset);
	return true;
}


void CBotEmpires::logEmpiresMortarRunPlayerMove(const char* boundary,
	const char* capture, const CBotCmd& command)
{
	if (!isEmpiresMortarProbeActive() || logger == nullptr || engine == nullptr ||
		gpGlobals == nullptr || m_pEdict == nullptr || m_pEdict->IsFree())
		return;

	const edict_t* pWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const char* weaponName = pWeapon != nullptr && !pWeapon->IsFree() &&
		pWeapon->GetClassName() != nullptr ? pWeapon->GetClassName() : "<none>";
	const int weaponIndex = pWeapon != nullptr && !pWeapon->IsFree() ?
		engine->IndexOfEdict(const_cast<edict_t*>(pWeapon)) : -1;

	int ownerIndex = -1;
	bool ownerKnown = false;
	if (pWeapon != nullptr && !pWeapon->IsFree())
	{
		ownerKnown = readEmpiresWeaponHandleProperty(pWeapon, "m_hOwner",
			ownerIndex);
		if (!ownerKnown)
			ownerKnown = readEmpiresWeaponHandleProperty(pWeapon, "m_hOwnerEntity",
				ownerIndex);
	}

	int submittedLife = -1;
	const bool lifeKnown = readEmpiresDebugIntProperty(m_pEdict,
		"m_lifeState", submittedLife);
	int processedButtons = 0;
	const bool buttonsKnown = readEmpiresDebugIntProperty(m_pEdict,
		"m_nButtons", processedButtons);
	int buttonPressed = 0;
	const bool pressedKnown = readEmpiresDebugIntProperty(m_pEdict,
		"m_afButtonPressed", buttonPressed);
	int buttonReleased = 0;
	const bool releasedKnown = readEmpiresDebugIntProperty(m_pEdict,
		"m_afButtonReleased", buttonReleased);
	int buttonForced = 0;
	const bool forcedKnown = readEmpiresDebugIntProperty(m_pEdict,
		"m_afButtonForced", buttonForced);
	int buttonDisabled = 0;
	const bool disabledKnown = readEmpiresDebugIntProperty(m_pEdict,
		"m_afButtonDisabled", buttonDisabled);
	int tickBase = 0;
	const bool tickBaseKnown = readEmpiresDebugIntProperty(m_pEdict,
		"m_nTickBase", tickBase);
	float nextAttack = 0.0f;
	const bool nextAttackKnown = readEmpiresDebugFloatProperty(m_pEdict,
		"m_flNextAttack", nextAttack);

	bool mortarLoadedValue = false;
	const bool mortarLoadedKnown = readEmpiresBoolProperty(m_pEdict,
		"m_bMortarLoaded", mortarLoadedValue);
	const int mortarLoaded = mortarLoadedValue ? 1 : 0;
	float reloadTime = 0.0f;
	const bool reloadKnown = pWeapon != nullptr && !pWeapon->IsFree() &&
		readEmpiresWeaponFloatProperty(pWeapon, "m_flReloadTime", reloadTime);
	float nextPrimary = 0.0f;
	const bool primaryKnown = pWeapon != nullptr && !pWeapon->IsFree() &&
		readEmpiresWeaponFloatProperty(pWeapon, "m_flNextPrimaryAttack", nextPrimary);
	int shellHandle = -1;
	const bool shellHandleKnown = pWeapon != nullptr && !pWeapon->IsFree() &&
		readEmpiresWeaponHandleProperty(pWeapon, "m_hShell", shellHandle);

	int clip1 = -1;
	int clip2 = -1;
	int ammoType1 = -1;
	int ammoType2 = -1;
	int ammo1 = -1;
	if (pWeapon != nullptr && !pWeapon->IsFree())
	{
		CClassInterface::getWeaponClip(const_cast<edict_t*>(pWeapon), &clip1, &clip2);
		CClassInterface::getAmmoTypes(const_cast<edict_t*>(pWeapon), &ammoType1,
			&ammoType2);
		if (ammoType1 >= 0)
			ammo1 = getAmmo(static_cast<std::size_t>(ammoType1));
	}

	int shellIndex = -1;
	const char* shellClassName = nullptr;
	const int shellCount = countEmpiresMortarShells(&shellIndex,
		&shellClassName);
	const int flags = CClassInterface::getPlayerFlags(m_pEdict);
	const bool onGround = (flags & FL_ONGROUND) != 0;
	const bool ducking = (flags & FL_DUCKING) != 0;
	const int userId = engine->GetPlayerUserId(m_pEdict);
	const int index = engine->IndexOfEdict(m_pEdict);
	const int health = m_pPlayerInfo != nullptr ? m_pPlayerInfo->GetHealth() : -1;

	logger->Log(LogLevel::INFO,
		"Empires mortar telemetry boundary=%s capture=%s index=%d userid=%d state=%s command_number=%d tick_count=%d submitted_buttons=0x%x processed_buttons_known=%d processed_buttons=0x%x pressed_known=%d pressed=0x%x released_known=%d released=0x%x forced_known=%d forced=0x%x disabled_known=%d disabled=0x%x life_known=%d life=%d health=%d flags=0x%x on_ground=%d ducking=%d fake_client=%d tick_base_known=%d tick_base=%d curtime=%.3f tick_interval=%.6f player_next_attack_known=%d player_next_attack=%.3f active_weapon=%s weapon_index=%d owner_known=%d owner_index=%d ammo_type1=%d ammo1=%d clip1=%d loaded_known=%d loaded=%d reload_known=%d reload_time=%.3f primary_known=%d next_primary=%.3f shell_handle_known=%d shell_handle_index=%d shell_count=%d shell_index=%d shell_class=%s",
		boundary != nullptr ? boundary : "<unknown>",
		capture != nullptr ? capture : "<unknown>", index, userId,
		empiresMortarProbeStateName(m_eEmpiresMortarProbeState),
		command.command_number, command.tick_count, command.buttons,
		buttonsKnown ? 1 : 0, processedButtons, pressedKnown ? 1 : 0,
		buttonPressed, releasedKnown ? 1 : 0, buttonReleased,
		forcedKnown ? 1 : 0, buttonForced, disabledKnown ? 1 : 0,
		buttonDisabled, lifeKnown ? 1 : 0, submittedLife, health, flags,
		onGround ? 1 : 0, ducking ? 1 : 0,
		m_pPlayerInfo != nullptr && m_pPlayerInfo->IsFakeClient() ? 1 : 0,
		tickBaseKnown ? 1 : 0, tickBase, gpGlobals->curtime,
		gpGlobals->interval_per_tick, nextAttackKnown ? 1 : 0, nextAttack,
		weaponName, weaponIndex, ownerKnown ? 1 : 0, ownerIndex, ammoType1,
		ammo1, clip1, mortarLoadedKnown ? 1 : 0, mortarLoaded,
		reloadKnown ? 1 : 0, reloadTime, primaryKnown ? 1 : 0, nextPrimary,
		shellHandleKnown ? 1 : 0, shellHandle, shellCount, shellIndex,
		shellClassName != nullptr ? shellClassName : "<none>");
}

void CBotEmpires::debugBeforeRunPlayerMove(const CBotCmd& command)
{
	if (isEmpiresCommanderMoveActive() && logger != nullptr &&
		engine != nullptr && gpGlobals != nullptr && m_pEdict != nullptr &&
		!m_pEdict->IsFree() &&
		gpGlobals->curtime >= m_flEmpiresCommanderMoveNextCommandDiagnosticLog)
	{
		logEmpiresCommanderMoveDiagnostic("final-before-run", &command, true);
		m_flEmpiresCommanderMoveNextCommandDiagnosticLog =
			gpGlobals->curtime + 0.5f;
		m_bEmpiresCommanderMoveDiagnosticCommandPending = true;
		m_iEmpiresCommanderMoveDiagnosticCommandNumber =
			command.command_number;
	}

	if (isEmpiresEngineerResupplyActionActive() &&
		m_bEmpiresEngineerResupplyActionUseSubmitted &&
		!m_bEmpiresEngineerResupplyActionUsePreRunLogged &&
		(command.buttons & IN_USE) != 0 && logger != nullptr &&
		engine != nullptr && gpGlobals != nullptr && m_pEdict != nullptr)
	{
		const int flags = CClassInterface::getPlayerFlags(m_pEdict);
		logger->Log(LogLevel::INFO,
			"Empires engineer resupply=use-prerun index=%d userid=%d curtime=%.3f buttons=0x%x flags=0x%x on_ground=%d ducking=%d fake_client_flag=%d frozen=%d on_train=%d at_controls=%d path=normal-RunPlayerMove",
			engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
			gpGlobals->curtime, command.buttons, flags,
			(flags & FL_ONGROUND) != 0 ? 1 : 0,
			(flags & FL_DUCKING) != 0 ? 1 : 0,
			(flags & FL_FAKECLIENT) != 0 ? 1 : 0,
			(flags & FL_FROZEN) != 0 ? 1 : 0,
			(flags & FL_ONTRAIN) != 0 ? 1 : 0,
			(flags & FL_ATCONTROLS) != 0 ? 1 : 0);
		m_bEmpiresEngineerResupplyActionUsePreRunLogged = true;
	}

	if (isEmpiresEngineerBuildActionActive())
	{
		if ((command.buttons & IN_ATTACK) != 0 &&
			!m_bEmpiresEngineerBuildActionAttackSubmitted)
		{
			m_bEmpiresEngineerBuildActionAttackSubmitted = true;
			if (logger != nullptr && engine != nullptr && gpGlobals != nullptr &&
				m_pEdict != nullptr)
			{
				logger->Log(LogLevel::INFO,
					"Empires engineer build=outgoing index=%d userid=%d curtime=%.3f buttons=0x%x target_waypoint=%d input=normal-primary-attack",
					engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
					gpGlobals->curtime, command.buttons,
					m_iEmpiresEngineerBuildActionTargetWaypoint);
			}
		}
		return;
	}

	if (isEmpiresEngineerBuildSelectionActive())
	{
		if ((command.buttons & IN_ATTACK2) != 0 &&
			!m_bEmpiresEngineerBuildSelectionInputSubmitted)
		{
			m_bEmpiresEngineerBuildSelectionInputSubmitted = true;
			if (logger != nullptr && engine != nullptr && gpGlobals != nullptr &&
				m_pEdict != nullptr)
			{
				int buildItem = -1;
				int buildState = -1;
				const bool itemKnown = readEmpiresDebugIntProperty(
					m_pEdict, "m_iEngineerBuildItem", buildItem);
				const bool stateKnown = readEmpiresDebugIntProperty(
					m_pEdict, "m_iEngBuild", buildState);
				logger->Log(LogLevel::INFO,
					"Empires engineer build-select=outgoing index=%d userid=%d curtime=%.3f buttons=0x%x attack2=1 item_known=%d item=%d build_state_known=%d build_state=%d",
					engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
					gpGlobals->curtime, command.buttons, itemKnown ? 1 : 0,
					buildItem, stateKnown ? 1 : 0, buildState);
			}
		}
		return;
	}

	if (!isEmpiresMortarProbeActive())
		return;

	const bool attack = (command.buttons & IN_ATTACK) != 0;
	const bool initialNeutral = !attack && !m_bEmpiresMortarProbeInitialNeutralLogged &&
		(m_eEmpiresMortarProbeState == EmpiresMortarProbeState::WAITING_FOR_ACTIVE ||
			m_eEmpiresMortarProbeState == EmpiresMortarProbeState::WAITING_FOR_READY);
	const bool release = !attack && !m_bEmpiresMortarProbeReleaseLogged &&
		m_eEmpiresMortarProbeState == EmpiresMortarProbeState::OBSERVING_SHELL;
	if (!attack && !initialNeutral && !release)
		return;

	const char* capture = attack ? "attack" : (release ? "release" : "neutral");
	logEmpiresMortarRunPlayerMove("before", capture, command);
	if (initialNeutral)
	{
		m_bEmpiresMortarProbeInitialNeutralLogged = true;
		m_iEmpiresMortarProbeTelemetryPending = 1;
	}
	else if (attack)
	{
		m_iEmpiresMortarProbeTelemetryPending = 2;
	}
	else if (release)
	{
		m_bEmpiresMortarProbeReleaseLogged = true;
		m_iEmpiresMortarProbeTelemetryPending = 3;
	}
}

void CBotEmpires::debugAfterRunPlayerMove(const CBotCmd& command)
{
	if (m_bEmpiresCommanderMoveDiagnosticCommandPending &&
		logger != nullptr && m_pPlayerInfo != nullptr && engine != nullptr &&
		gpGlobals != nullptr && m_pEdict != nullptr && !m_pEdict->IsFree())
	{
		logEmpiresCommanderMoveDiagnostic("after-run-player-move", nullptr, true);
		const CBotCmd processed = m_pPlayerInfo->GetLastUserCommand();
		logger->Log(LogLevel::INFO,
			"Empires commander-diag-processed phase=after-run-player-move owner_userid=%d submitted_command_number=%d callback_command_number=%d processed_command_number=%d processed_tick_count=%d processed_forwardmove=%.1f processed_sidemove=%.1f processed_upmove=%.1f processed_buttons=0x%x processed_view=(%.1f %.1f %.1f)",
			engine->GetPlayerUserId(m_pEdict),
			m_iEmpiresCommanderMoveDiagnosticCommandNumber,
			command.command_number, processed.command_number,
			processed.tick_count, processed.forwardmove, processed.sidemove,
			processed.upmove, processed.buttons, processed.viewangles.x,
			processed.viewangles.y, processed.viewangles.z);
		m_bEmpiresCommanderMoveDiagnosticCommandPending = false;
		m_iEmpiresCommanderMoveDiagnosticCommandNumber = -1;
	}

	// The resupply probe must use the normal E/IN_USE path.  Read back the
	// player-side button state once after RunPlayerMove so a failed ammo effect
	// can be located after command delivery, without invoking any private use
	// routine or changing game state.
	if (isEmpiresEngineerResupplyActionActive() &&
		m_bEmpiresEngineerResupplyActionUseSubmitted &&
		!m_bEmpiresEngineerResupplyActionUseProcessedLogged && logger != nullptr &&
		m_pPlayerInfo != nullptr && engine != nullptr && gpGlobals != nullptr &&
		m_pEdict != nullptr && !m_pEdict->IsFree())
	{
		int processedButtons = 0;
		const bool processedButtonsKnown = readEmpiresDebugIntProperty(
			m_pEdict, "m_nButtons", processedButtons);
		int buttonPressed = 0;
		const bool buttonPressedKnown = readEmpiresDebugIntProperty(
			m_pEdict, "m_afButtonPressed", buttonPressed);
		int buttonReleased = 0;
		const bool buttonReleasedKnown = readEmpiresDebugIntProperty(
			m_pEdict, "m_afButtonReleased", buttonReleased);
		const CBotCmd processed = m_pPlayerInfo->GetLastUserCommand();
		logger->Log(LogLevel::INFO,
			"Empires engineer resupply=use-processed index=%d userid=%d curtime=%.3f submitted_buttons=0x%x processed_cmd_buttons=0x%x m_nButtons_known=%d m_nButtons=0x%x pressed_known=%d pressed=0x%x released_known=%d released=0x%x path=normal-RunPlayerMove",
			engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
			gpGlobals->curtime, command.buttons, processed.buttons,
			processedButtonsKnown ? 1 : 0, processedButtons,
			buttonPressedKnown ? 1 : 0, buttonPressed,
			buttonReleasedKnown ? 1 : 0, buttonReleased);
		m_bEmpiresEngineerResupplyActionUseProcessedLogged = true;
	}

	if (isEmpiresMortarActionActive() && m_bEmpiresMortarAimRequested &&
		m_eEmpiresMortarActionState == EmpiresMortarActionState::ATTACK_SUBMITTED &&
		!m_bEmpiresMortarAimProcessedLogged && logger != nullptr &&
		m_pPlayerInfo != nullptr && engine != nullptr && m_pEdict != nullptr)
	{
		const CBotCmd processed = m_pPlayerInfo->GetLastUserCommand();
		logger->Log(LogLevel::INFO,
			"Empires mortar aim=command-processed index=%d userid=%d submitted_buttons=0x%x submitted_view=(%.2f %.2f %.2f) processed_buttons=0x%x processed_view=(%.2f %.2f %.2f)",
			engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
			command.buttons, command.viewangles.x, command.viewangles.y,
			command.viewangles.z, processed.buttons, processed.viewangles.x,
			processed.viewangles.y, processed.viewangles.z);
		m_bEmpiresMortarAimProcessedLogged = true;
	}

	// Engineer repair remains a debug-only action.  Sample the state directly
	// after the normal public RunPlayerMove() call so the probe can distinguish
	// "command submitted" from "Empires' weapon body accepted PrimaryAttack".
	// This is telemetry only; no command or game property is changed here.
	if (isEmpiresEngineerRepairActionActive() && logger != nullptr &&
		m_pPlayerInfo != nullptr && engine != nullptr && gpGlobals != nullptr &&
		m_pEdict != nullptr && !m_pEdict->IsFree() &&
		(m_iEmpiresEngineerRepairActionFrames == 1 ||
		 m_iEmpiresEngineerRepairActionFrames % 30 == 0))
	{
		const edict_t* pWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
		const char* weaponName = pWeapon != nullptr && !pWeapon->IsFree() &&
			pWeapon->GetClassName() != nullptr ? pWeapon->GetClassName() : "<none>";
		int shotsFired = -1;
		const bool shotsFiredKnown = readEmpiresDebugIntProperty(
			m_pEdict, "m_iShotsFired", shotsFired);
		int processedButtons = 0;
		const bool processedButtonsKnown = readEmpiresDebugIntProperty(
			m_pEdict, "m_nButtons", processedButtons);
		int buttonPressed = 0;
		const bool buttonPressedKnown = readEmpiresDebugIntProperty(
			m_pEdict, "m_afButtonPressed", buttonPressed);
		int buttonReleased = 0;
		const bool buttonReleasedKnown = readEmpiresDebugIntProperty(
			m_pEdict, "m_afButtonReleased", buttonReleased);
		float nextAttack = 0.0f;
		const bool nextAttackKnown = readEmpiresDebugFloatProperty(
			m_pEdict, "m_flNextAttack", nextAttack);
		float nextPrimary = 0.0f;
		const bool nextPrimaryKnown = pWeapon != nullptr && !pWeapon->IsFree() &&
			readEmpiresWeaponFloatProperty(pWeapon, "m_flNextPrimaryAttack",
				nextPrimary);
		int clip1 = -1;
		const bool clip1Known = pWeapon != nullptr && !pWeapon->IsFree() &&
			readEmpiresEntityIntProperty(pWeapon, "m_iClip1", clip1);
		const CBotCmd processed = m_pPlayerInfo->GetLastUserCommand();
		logger->Log(LogLevel::INFO,
			"Empires engineer repair=post-runcmd frame=%d index=%d userid=%d curtime=%.3f submitted_buttons=0x%x processed_cmd_buttons=0x%x m_nButtons_known=%d m_nButtons=0x%x pressed_known=%d pressed=0x%x released_known=%d released=0x%x weapon=%s shots_fired_known=%d shots_fired=%d clip1_known=%d clip1=%d next_attack_known=%d next_attack=%.3f next_primary_known=%d next_primary=%.3f",
			m_iEmpiresEngineerRepairActionFrames, engine->IndexOfEdict(m_pEdict),
			engine->GetPlayerUserId(m_pEdict), gpGlobals->curtime,
			command.buttons, processed.buttons, processedButtonsKnown ? 1 : 0,
			processedButtons, buttonPressedKnown ? 1 : 0, buttonPressed,
			buttonReleasedKnown ? 1 : 0, buttonReleased, weaponName,
			shotsFiredKnown ? 1 : 0, shotsFired, clip1Known ? 1 : 0, clip1,
			nextAttackKnown ? 1 : 0, nextAttack, nextPrimaryKnown ? 1 : 0,
			nextPrimary);
	}

	if (!isEmpiresMortarProbeActive() || m_iEmpiresMortarProbeTelemetryPending == 0)
		return;

	const int pending = m_iEmpiresMortarProbeTelemetryPending;
	m_iEmpiresMortarProbeTelemetryPending = 0;
	const char* capture = pending == 1 ? "neutral" :
		pending == 2 ? "attack" : "release";
	logEmpiresMortarRunPlayerMove("after", capture, command);
}

bool CBotEmpires::isEmpiresMortarProbeActive() const
{
	return m_eEmpiresMortarProbeState != EmpiresMortarProbeState::IDLE &&
		m_eEmpiresMortarProbeState != EmpiresMortarProbeState::COMPLETE &&
		m_eEmpiresMortarProbeState != EmpiresMortarProbeState::FAILED;
}

bool CBotEmpires::requestEmpiresMortarSmoke()
{
	constexpr const char* mortarClassName = "weapon_emp_nf_mortar";

	if (isEmpiresMortarProbeActive() || m_bBootstrapOnly ||
		m_bControllerProbeOnly || m_pEdict == nullptr || m_pEdict->IsFree() ||
		m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr || m_pWeapons == nullptr || engine == nullptr ||
		gpGlobals == nullptr ||
		playerinfomanager == nullptr || !m_pPlayerInfo->IsConnected() ||
		m_pPlayerInfo->IsObserver() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0 ||
		m_pPlayerInfo->GetTeamIndex() != EMP_TEAM_NF)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired,
		classDesiredKnown, classPropertyName) || classId != EMP_CLASS_GRENADIER)
	{
		return false;
	}

	CBaseHandle* pWeaponList = CClassInterface::getWeaponList(m_pEdict);
	if (pWeaponList == nullptr)
		return false;

	edict_t* pMortar = nullptr;
	for (int slot = 0; slot < MAX_WEAPONS; ++slot)
	{
		const int weaponIndex = pWeaponList[slot].GetEntryIndex();
		if (weaponIndex <= 0)
			continue;

		edict_t* pWeapon = engine->PEntityOfEntIndex(weaponIndex);
		if (pWeapon == nullptr || pWeapon->IsFree() ||
			pWeapon->GetClassName() == nullptr ||
			std::strcmp(pWeapon->GetClassName(), mortarClassName) != 0 ||
			CClassInterface::getWeaponState(pWeapon) == WEAPON_NOT_CARRIED)
		{
			continue;
		}

		pMortar = pWeapon;
		break;
	}

	if (pMortar == nullptr)
		return false;

	m_iEmpiresMortarProbeExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iEmpiresMortarProbeExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pEmpiresMortarProbeExpectedEdict = m_pEdict;
	m_pEmpiresMortarProbeWeapon = pMortar;
	m_iEmpiresMortarProbeAttempt = 1;
	m_iEmpiresMortarProbeFrames = 0;
	m_iEmpiresMortarProbeShellBaseline = countEmpiresMortarShells();
	m_iEmpiresMortarProbeShellIndex = -1;
	m_iEmpiresMortarProbeLastAmmo = -1;
	m_iEmpiresMortarProbeLastClip = -1;
	m_iEmpiresMortarProbeLastFlags = 0;
	m_bEmpiresMortarProbeLastFlagsKnown = false;
	m_bEmpiresMortarProbePostureReadyLogged = false;
	m_bEmpiresMortarProbeLoadedKnown = false;
	m_bEmpiresMortarProbeLastLoaded = false;
	m_bEmpiresMortarProbeReloadTimeKnown = false;
	m_flEmpiresMortarProbeReloadTime = 0.0f;
	m_bEmpiresMortarProbeNextPrimaryKnown = false;
	m_flEmpiresMortarProbeNextPrimaryAttack = 0.0f;
	m_bEmpiresMortarProbeInitialNeutralLogged = false;
	m_bEmpiresMortarProbeReleaseLogged = false;
	m_iEmpiresMortarProbeTelemetryPending = 0;
	m_flEmpiresMortarProbeDeadline = gpGlobals->curtime + 20.0f;
	m_flEmpiresMortarProbeShellDeadline = 0.0f;

	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	m_pButtons->letGo(IN_ATTACK);
	m_pButtons->letGo(IN_ATTACK2);
	m_pButtons->letGo(IN_RELOAD);
	m_pButtons->letGo(IN_DUCK);
	m_vViewAngles = QAngle(0.0f, 0.0f, 0.0f);
	selectWeaponName(mortarClassName);
	m_eEmpiresMortarProbeState = EmpiresMortarProbeState::WAITING_FOR_ACTIVE;

	logger->Log(LogLevel::INFO,
		"Empires mortar probe=armed index=%d userid=%d class=%d class_property=%s weapon=%s weapon_index=%d origin=(%.1f %.1f %.1f) flags=0x%x shell_baseline=%d attempt=1 deadline=%.3f",
		m_iEmpiresMortarProbeExpectedIndex, m_iEmpiresMortarProbeExpectedUserId,
		classId, classPropertyName != nullptr ? classPropertyName : "<unknown>",
		mortarClassName, engine->IndexOfEdict(pMortar), getOrigin().x,
		getOrigin().y, getOrigin().z, CClassInterface::getPlayerFlags(m_pEdict),
		m_iEmpiresMortarProbeShellBaseline, m_flEmpiresMortarProbeDeadline);
	return true;
}

bool CBotEmpires::requestEmpiresMortarFireOnce()
{
	const int team = m_pPlayerInfo != nullptr ? m_pPlayerInfo->GetTeamIndex() : -1;
	const char* mortarClassName = empiresMortarClassName(team);
	if (isEmpiresMortarActionActive() || isEmpiresMortarProbeActive() ||
		m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pEdict->IsFree() || m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr || m_pWeapons == nullptr || engine == nullptr ||
		gpGlobals == nullptr || playerinfomanager == nullptr || mortarClassName == nullptr ||
		!m_pPlayerInfo->IsConnected() || m_pPlayerInfo->IsObserver() ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		m_pPlayerInfo->GetTeamIndex() != team)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_GRENADIER)
	{
		return false;
	}
	(void)classDesired;
	(void)classDesiredKnown;

	CBaseHandle* pWeaponList = CClassInterface::getWeaponList(m_pEdict);
	if (pWeaponList == nullptr)
		return false;

	edict_t* pMortar = nullptr;
	for (int slot = 0; slot < MAX_WEAPONS; ++slot)
	{
		const int weaponIndex = pWeaponList[slot].GetEntryIndex();
		if (weaponIndex <= 0)
			continue;

		edict_t* pWeapon = engine->PEntityOfEntIndex(weaponIndex);
		if (pWeapon == nullptr || pWeapon->IsFree() ||
			pWeapon->GetClassName() == nullptr ||
			std::strcmp(pWeapon->GetClassName(), mortarClassName) != 0 ||
			CClassInterface::getWeaponState(pWeapon) == WEAPON_NOT_CARRIED)
		{
			continue;
		}

		pMortar = pWeapon;
		break;
	}

	if (pMortar == nullptr)
		return false;

	m_iEmpiresMortarActionTeam = team;
	m_iEmpiresMortarActionExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iEmpiresMortarActionExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pEmpiresMortarActionExpectedEdict = m_pEdict;
	m_pEmpiresMortarActionWeapon = pMortar;
	m_iEmpiresMortarActionShellBaseline = countEmpiresMortarShells();
	m_iEmpiresMortarActionShellIndex = -1;
	m_iEmpiresMortarActionAmmoBefore = -1;
	m_iEmpiresMortarActionAmmoAfter = -1;
	m_iEmpiresMortarActionFrames = 0;
	m_bEmpiresMortarActionPostureLogged = false;
	m_bEmpiresMortarActionAttackLogged = false;
	m_bEmpiresMortarActionReleaseLogged = false;
	m_flEmpiresMortarActionDeadline = gpGlobals->curtime + 20.0f;
	m_flEmpiresMortarActionShellDeadline = 0.0f;
	m_bEmpiresMortarAimRequested = false;
	m_bEmpiresMortarAimCheckArc = false;
	m_iEmpiresMortarAimTargetWaypoint = -1;
	m_vEmpiresMortarAimTarget = Vector(0.0f, 0.0f, 0.0f);
	m_qEmpiresMortarAimAngles = QAngle(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLaunchOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_bEmpiresMortarAimComputed = false;
	m_bEmpiresMortarAimViewApplied = false;
	m_bEmpiresMortarAimProcessedLogged = false;
	m_iEmpiresMortarAimTrajectorySamples = 0;
	m_flEmpiresMortarAimLastSampleTime = 0.0f;
	m_vEmpiresMortarAimFirstShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLastShellOrigin = Vector(0.0f, 0.0f, 0.0f);

	int clip1 = -1;
	int clip2 = -1;
	int ammoType1 = -1;
	int ammoType2 = -1;
	CClassInterface::getWeaponClip(pMortar, &clip1, &clip2);
	CClassInterface::getAmmoTypes(pMortar, &ammoType1, &ammoType2);
	if (ammoType1 >= 0)
		m_iEmpiresMortarActionAmmoBefore = getAmmo(static_cast<std::size_t>(ammoType1));

	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	m_pButtons->letGo(IN_ATTACK);
	m_pButtons->letGo(IN_ATTACK2);
	m_pButtons->letGo(IN_RELOAD);
	m_pButtons->letGo(IN_DUCK);
	m_pButtons->duck(true);
	selectWeaponName(mortarClassName);
	m_eEmpiresMortarActionState = EmpiresMortarActionState::WAITING_FOR_ACTIVE;

	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar action=armed team=%d class=%d class_property=%s index=%d userid=%d weapon=%s weapon_index=%d shell_baseline=%d ammo_before=%d deadline=%.3f",
			team, classId, classPropertyName != nullptr ? classPropertyName : "<unknown>",
			m_iEmpiresMortarActionExpectedIndex, m_iEmpiresMortarActionExpectedUserId,
			mortarClassName, engine->IndexOfEdict(pMortar),
			m_iEmpiresMortarActionShellBaseline, m_iEmpiresMortarActionAmmoBefore,
			m_flEmpiresMortarActionDeadline);
	}
	return true;
}

bool CBotEmpires::requestEmpiresMortarAtEnemyOnce(edict_t* pTarget)
{
	if (pTarget == nullptr || pTarget->IsFree() ||
		!CBotGlobals::entityIsValid(pTarget) ||
		!CBotGlobals::entityIsAlive(pTarget) ||
		!isEnemy(pTarget, false) || !isVisible(pTarget))
	{
		return false;
	}

	const float targetDistance = distanceFrom(pTarget);
	if (!std::isfinite(targetDistance) ||
		targetDistance < kEmpiresGrenadierMortarMinRange ||
		targetDistance > kEmpiresGrenadierMortarMaxRange)
	{
		return false;
	}

	const Vector targetOrigin = CBotGlobals::entityOrigin(pTarget);
	if (!requestEmpiresMortarAtPointOnce(targetOrigin))
		return false;

	if (logger != nullptr && engine != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar tactic=target-snapshot owner_userid=%d target_userid=%d target_index=%d target_origin=(%.1f %.1f %.1f) distance=%.1f",
			engine->GetPlayerUserId(m_pEdict), engine->GetPlayerUserId(pTarget),
			engine->IndexOfEdict(pTarget), targetOrigin.x, targetOrigin.y,
			targetOrigin.z, targetDistance);
	}
	return true;
}

// Mortar one shell at a fixed point (no sight needed: the shell flies an arc).
bool CBotEmpires::requestEmpiresMortarAtPointOnce(const Vector& targetOrigin, bool checkArc)
{
	if (!targetOrigin.IsValid() || !requestEmpiresMortarFireOnce())
		return false;

	// The normal action remains responsible for weapon selection, posture,
	// readiness, input and shell observation. This only supplies a snapshot of
	// the currently visible target to the already verified ballistic helper.
	m_bEmpiresMortarAimRequested = true;
	m_bEmpiresMortarAimCheckArc = checkArc;
	m_iEmpiresMortarAimTargetWaypoint = -1;
	m_vEmpiresMortarAimTarget = targetOrigin;
	m_qEmpiresMortarAimAngles = QAngle(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLaunchOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_bEmpiresMortarAimComputed = false;
	m_bEmpiresMortarAimViewApplied = false;
	m_bEmpiresMortarAimProcessedLogged = false;
	m_iEmpiresMortarAimTrajectorySamples = 0;
	m_flEmpiresMortarAimLastSampleTime = 0.0f;
	m_vEmpiresMortarAimFirstShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLastShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	return true;
}

bool CBotEmpires::requestEmpiresMineFireOnce()
{
	const int team = m_pPlayerInfo != nullptr ? m_pPlayerInfo->GetTeamIndex() : -1;
	const char* mineClassName = empiresMineClassName(team);
	if (isEmpiresMineActionActive() || isEmpiresMortarProbeActive() ||
		isEmpiresMortarActionActive() || m_bBootstrapOnly ||
		m_bControllerProbeOnly || m_pEdict == nullptr || m_pEdict->IsFree() ||
		m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr || engine == nullptr || gpGlobals == nullptr ||
		playerinfomanager == nullptr || mineClassName == nullptr ||
		!m_pPlayerInfo->IsConnected() || m_pPlayerInfo->IsObserver() ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_GRENADIER)
	{
		return false;
	}
	(void)classDesired;
	(void)classDesiredKnown;

	CBaseHandle* pWeaponList = CClassInterface::getWeaponList(m_pEdict);
	if (pWeaponList == nullptr)
		return false;

	edict_t* pMine = nullptr;
	for (int slot = 0; slot < MAX_WEAPONS; ++slot)
	{
		const int weaponIndex = pWeaponList[slot].GetEntryIndex();
		if (weaponIndex <= 0)
			continue;

		edict_t* pWeapon = engine->PEntityOfEntIndex(weaponIndex);
		if (pWeapon == nullptr || pWeapon->IsFree() ||
			pWeapon->GetClassName() == nullptr ||
			std::strcmp(pWeapon->GetClassName(), mineClassName) != 0 ||
			CClassInterface::getWeaponState(pWeapon) == WEAPON_NOT_CARRIED)
		{
			continue;
		}

		pMine = pWeapon;
		break;
	}

	if (pMine == nullptr)
		return false;

	m_iEmpiresMineActionTeam = team;
	m_iEmpiresMineActionExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iEmpiresMineActionExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pEmpiresMineActionExpectedEdict = m_pEdict;
	m_pEmpiresMineActionWeapon = pMine;
	m_iEmpiresMineActionProjectileBaseline = countEmpiresMineProjectiles(team);
	m_iEmpiresMineActionProjectileIndex = -1;
	m_iEmpiresMineActionAmmoBefore = -1;
	m_iEmpiresMineActionAmmoAfter = -1;
	m_iEmpiresMineActionFrames = 0;
	m_iEmpiresMineActionDeployObservations = 0;
	m_iEmpiresMineActionSettledFrames = 0;
	m_iEmpiresMineActionTriggerTargetUserId = -1;
	m_iEmpiresMineActionTriggerTargetIndex = -1;
	m_iEmpiresMineActionTriggerTargetHealthBefore = -1;
	m_iEmpiresMineActionTriggerTargetHealthAfter = -1;
	m_pEmpiresMineActionTriggerTargetEdict = nullptr;
	m_bEmpiresMineActionTriggerMode = false;
	m_flEmpiresMineActionTriggerDelaySeconds = 0.0f;
	m_bEmpiresMineActionTriggerPositionLogged = false;
	m_bEmpiresMineActionTriggerHoldLogged = false;
	m_bEmpiresMineActionTriggerImpactLogged = false;
	m_bEmpiresMineActionTriggerNaturalDeathLogged = false;
	m_bEmpiresMineActionAttackLogged = false;
	m_bEmpiresMineActionReleaseLogged = false;
	m_bEmpiresMineActionDeployLogged = false;
	m_iEmpiresMineActionActivationLast = -1;
	m_bEmpiresMineActionActivationUnavailableLogged = false;
	m_bEmpiresMineActionActivationLogged = false;
	m_flEmpiresMineActionDeployedTime = 0.0f;
	m_flEmpiresMineActionDeadline = gpGlobals->curtime + 12.0f;
	m_flEmpiresMineActionProjectileDeadline = 0.0f;

	int ammoType1 = -1;
	int ammoType2 = -1;
	CClassInterface::getAmmoTypes(pMine, &ammoType1, &ammoType2);
	if (ammoType1 >= 0)
		m_iEmpiresMineActionAmmoBefore = getAmmo(static_cast<std::size_t>(ammoType1));

	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	m_pButtons->letGo(IN_ATTACK);
	m_pButtons->letGo(IN_ATTACK2);
	m_pButtons->letGo(IN_RELOAD);
	m_pButtons->letGo(IN_DUCK);
	selectWeaponName(mineClassName);
	m_eEmpiresMineActionState = EmpiresMineActionState::WAITING_FOR_ACTIVE;

	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires mine action=armed team=%d class=%d class_property=%s index=%d userid=%d weapon=%s weapon_index=%d projectile_baseline=%d ammo_before=%d deadline=%.3f",
			team, classId, classPropertyName != nullptr ? classPropertyName : "<unknown>",
			m_iEmpiresMineActionExpectedIndex, m_iEmpiresMineActionExpectedUserId,
			mineClassName, engine->IndexOfEdict(pMine),
			m_iEmpiresMineActionProjectileBaseline,
			m_iEmpiresMineActionAmmoBefore, m_flEmpiresMineActionDeadline);
	}
	return true;
}

bool CBotEmpires::requestEmpiresMineTriggerOnce(const int targetUserId,
	const float triggerDelaySeconds)
{
	if (targetUserId < 0 || triggerDelaySeconds < 0.0f ||
		triggerDelaySeconds > 8.0f || m_pEdict == nullptr || m_pEdict->IsFree() ||
		m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0 ||
		engine == nullptr || playerinfomanager == nullptr ||
		m_bBootstrapOnly || m_bControllerProbeOnly)
	{
		return false;
	}

	edict_t* pTargetEdict = CBotGlobals::playerByUserId(targetUserId);
	CBot* pTargetBase = pTargetEdict != nullptr ?
		CBots::getBotPointer(pTargetEdict) : nullptr;
	CBotEmpires* pTargetBot = pTargetBase != nullptr ?
		static_cast<CBotEmpires*>(pTargetBase) : nullptr;
	IPlayerInfo* pTargetInfo = pTargetEdict != nullptr ?
		playerinfomanager->GetPlayerInfo(pTargetEdict) : nullptr;
	const int team = m_pPlayerInfo->GetTeamIndex();
	const bool targetValid = pTargetBot != nullptr &&
		pTargetEdict != m_pEdict && !pTargetEdict->IsFree() &&
		pTargetBot->getController() != nullptr && pTargetInfo != nullptr &&
		pTargetInfo->IsConnected() && !pTargetInfo->IsObserver() &&
		!pTargetInfo->IsDead() && pTargetInfo->GetHealth() > 0 &&
		pTargetInfo->GetTeamIndex() != team &&
		(team == EMP_TEAM_NF || team == EMP_TEAM_BE);
	if (!targetValid)
		return false;

	if (!requestEmpiresMineFireOnce())
		return false;

	m_iEmpiresMineActionTriggerTargetUserId = targetUserId;
	m_iEmpiresMineActionTriggerTargetIndex = engine->IndexOfEdict(pTargetEdict);
	m_iEmpiresMineActionTriggerTargetHealthBefore = pTargetInfo->GetHealth();
	m_iEmpiresMineActionTriggerTargetHealthAfter = -1;
	m_pEmpiresMineActionTriggerTargetEdict = pTargetEdict;
	m_bEmpiresMineActionTriggerMode = true;
	m_flEmpiresMineActionTriggerDelaySeconds = triggerDelaySeconds;
	m_bEmpiresMineActionTriggerPositionLogged = false;
	m_bEmpiresMineActionTriggerHoldLogged = false;
	m_bEmpiresMineActionTriggerImpactLogged = false;

	// The target is controlled only by this bounded diagnostic fixture. This
	// does not alter mine state or damage; normal Empires mine think code still
	// owns the trigger and detonation.
	pTargetBot->prepareEmpiresEnemyLosFixture();
	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires mine trigger=armed index=%d userid=%d team=%d target_index=%d target_userid=%d target_team=%d target_health_before=%d mode=debug-only automatic-deploy-then-engine-trigger target_hold_seconds=%.3f deadline=%.3f",
			engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
			team, m_iEmpiresMineActionTriggerTargetIndex, targetUserId,
			pTargetInfo->GetTeamIndex(),
			m_iEmpiresMineActionTriggerTargetHealthBefore,
			m_flEmpiresMineActionTriggerDelaySeconds,
			m_flEmpiresMineActionDeadline);
	}
	return true;
}

bool CBotEmpires::requestEmpiresScoutBinocularOnce(const int scoutUserId,
	const int targetUserId)
{
	if (scoutUserId < 0 || targetUserId < 0 || scoutUserId == targetUserId ||
		engine == nullptr || playerinfomanager == nullptr)
		return false;

	edict_t* pScoutEdict = CBotGlobals::playerByUserId(scoutUserId);
	edict_t* pTargetEdict = CBotGlobals::playerByUserId(targetUserId);
	CBot* pScoutBase = pScoutEdict != nullptr ?
		CBots::getBotPointer(pScoutEdict) : nullptr;
	CBotEmpires* pScout = pScoutBase != nullptr ?
		static_cast<CBotEmpires*>(pScoutBase) : nullptr;
	if (pScout == nullptr || pScout->isControlledTestOnly())
		return false;

	return pScout->requestEmpiresScoutBinocularTarget(pTargetEdict, false);
}

bool CBotEmpires::requestEmpiresScoutBinocularBuildingOnce(
	const int scoutUserId, const int buildingIndex)
{
	if (scoutUserId < 0 || buildingIndex <= 0 || engine == nullptr)
		return false;

	edict_t* pScoutEdict = CBotGlobals::playerByUserId(scoutUserId);
	edict_t* pBuildingEdict = engine->PEntityOfEntIndex(buildingIndex);
	CBot* pScoutBase = pScoutEdict != nullptr ?
		CBots::getBotPointer(pScoutEdict) : nullptr;
	CBotEmpires* pScout = pScoutBase != nullptr ?
		static_cast<CBotEmpires*>(pScoutBase) : nullptr;
	if (pScout == nullptr || pScout->isControlledTestOnly())
		return false;

	return pScout->requestEmpiresScoutBinocularTarget(pBuildingEdict, true);
}

bool CBotEmpires::observeEmpiresScoutSkillsOnce(const int scoutUserId)
{
	if (scoutUserId < 0 || engine == nullptr || playerinfomanager == nullptr ||
		logger == nullptr)
		return false;

	edict_t* pScoutEdict = CBotGlobals::playerByUserId(scoutUserId);
	CBot* pScoutBase = pScoutEdict != nullptr ?
		CBots::getBotPointer(pScoutEdict) : nullptr;
	CBotEmpires* pScout = pScoutBase != nullptr ?
		static_cast<CBotEmpires*>(pScoutBase) : nullptr;
	if (pScout == nullptr || pScout->isControlledTestOnly() ||
		pScout->m_pEdict == nullptr || pScout->m_pPlayerInfo == nullptr ||
		pScout->m_pPlayerInfo->IsDead() || pScout->m_pPlayerInfo->GetHealth() <= 0)
		return false;

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	const bool classKnown = pScout->readEmpiresClass(pScout->m_pEdict,
		classId, classDesired, classDesiredKnown, classPropertyName);
	int skills[4] = {};
	bool skillKnown[4] = {};
	for (int slot = 0; slot < 4; ++slot)
	{
		char propertyName[32] = {};
		std::snprintf(propertyName, sizeof(propertyName), "m_iSkill%d", slot + 1);
		skillKnown[slot] = pScout->readEmpiresEntityIntProperty(
			pScout->m_pEdict, propertyName, skills[slot]);
	}

	logger->Log(LogLevel::INFO,
		"Empires scout personal-skill-observation userid=%d index=%d alive=%d team=%d class_known=%d class=%d class_property=%s skill1_known=%d skill1=%d skill2_known=%d skill2=%d skill3_known=%d skill3=%d skill4_known=%d skill4=%d source=dynamic-public-property not_squad_leader_skill_observation=1 static_scout_enhanced_senses=32 static_autospot_radius=2000 static_autospot_infantry_radius=0",
		scoutUserId, engine->IndexOfEdict(pScout->m_pEdict),
		pScout->m_pPlayerInfo->IsDead() ? 0 : 1,
		pScout->m_pPlayerInfo->GetTeamIndex(), classKnown ? 1 : 0, classId,
		classPropertyName != nullptr ? classPropertyName : "<unknown>",
		skillKnown[0] ? 1 : 0, skills[0], skillKnown[1] ? 1 : 0, skills[1],
		skillKnown[2] ? 1 : 0, skills[2], skillKnown[3] ? 1 : 0, skills[3]);
	return true;
}

bool CBotEmpires::observeEmpiresSquadProbeOnce(const int targetUserId)
{
	using namespace RCBotEmpiresSquadObservation;

	if (engine == nullptr || playerinfomanager == nullptr ||
		gpGlobals == nullptr || logger == nullptr)
		return false;

	// Property reads are member helpers; any live normal Empires bot serves
	// as the reader. No state is written through it.
	CBotEmpires* reader = nullptr;
	int maxClients = gpGlobals->maxClients;
	const int configuredMaxClients = CBotGlobals::maxClients();
	if (configuredMaxClients > 0 && configuredMaxClients < maxClients)
		maxClients = configuredMaxClients;
	if (maxClients >= RCBOT_MAXPLAYERS)
		maxClients = RCBOT_MAXPLAYERS - 1;
	for (int index = 1; index <= maxClients; ++index)
	{
		edict_t* pCandidate = engine->PEntityOfEntIndex(index);
		if (pCandidate == nullptr || pCandidate->IsFree())
			continue;
		CBot* pBase = CBots::getBotPointer(pCandidate);
		if (pBase == nullptr || pBase->isControlledTestOnly())
			continue;
		reader = static_cast<CBotEmpires*>(pBase);
		break;
	}
	if (reader == nullptr)
		return false;

	int dumped = 0;
	for (int index = 1; index <= maxClients; ++index)
	{
		edict_t* pPlayer = engine->PEntityOfEntIndex(index);
		if (pPlayer == nullptr || pPlayer->IsFree())
			continue;
		IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(pPlayer);
		if (pInfo == nullptr || !pInfo->IsConnected())
			continue;
		const int userId = engine->GetPlayerUserId(pPlayer);
		if (targetUserId >= 0 && userId != targetUserId)
			continue;

		const int team = pInfo->GetTeamIndex();
		const int health = pInfo->GetHealth();
		const bool alive = !pInfo->IsObserver() && !pInfo->IsDead() &&
			health > 0;

		int classId = -1;
		bool classDesired = false;
		bool classDesiredKnown = false;
		const char* classPropertyName = nullptr;
		const bool classKnown = reader->readEmpiresClass(pPlayer,
			classId, classDesired, classDesiredKnown, classPropertyName);

		SquadFieldObservation fields;
		fields.squadKnown = reader->readEmpiresEntityIntProperty(
			pPlayer, "m_iSquad", fields.squad);
		fields.positionKnown = reader->readEmpiresEntityIntProperty(
			pPlayer, "m_iSquadPosition", fields.position);
		fields.explicitLeaderKnown = reader->readEmpiresEntityBoolProperty(
			pPlayer, "m_bSquadLeader", fields.explicitLeader);
		int squadXp = -1;
		const bool squadXpKnown = reader->readEmpiresEntityIntProperty(
			pPlayer, "m_iEmpSquadXP", squadXp);

		const LeaderInterpretation leader = InterpretSquadLeader(fields);
		const bool aliveEngineer = IsAliveEngineer(classKnown, classId,
			alive);

		logger->Log(LogLevel::INFO,
			"Empires squad-observation userid=%d index=%d team=%d connected=1 alive=%d health=%d class_known=%d class=%d class_property=%s squad_known=%d squad=%d position_known=%d position=%d explicit_leader_known=%d explicit_leader=%d derived_is_leader=%d encoding_match=%d ambiguous=%d alive_engineer=%d xp_known=%d xp=%d squad_status=%s position_status=%s explicit_status=%s xp_status=%s source=dynamic-public-property read_only=1",
			userId, index, team, alive ? 1 : 0, health,
			classKnown ? 1 : 0, classId,
			classPropertyName != nullptr ? classPropertyName : "<unknown>",
			fields.squadKnown ? 1 : 0, fields.squad,
			fields.positionKnown ? 1 : 0, fields.position,
			fields.explicitLeaderKnown ? 1 : 0,
			fields.explicitLeader ? 1 : 0,
			leader.isLeader ? 1 : 0, leader.encodingMatch ? 1 : 0,
			leader.ambiguous ? 1 : 0, aliveEngineer ? 1 : 0,
			squadXpKnown ? 1 : 0, squadXp,
			fields.squadKnown ? "RESOLVED" : "NOT_FOUND",
			fields.positionKnown ? "RESOLVED" : "NOT_FOUND",
			fields.explicitLeaderKnown ? "RESOLVED" : "NOT_FOUND",
			squadXpKnown ? "RESOLVED" : "NOT_FOUND");
		++dumped;
	}

	logger->Log(LogLevel::INFO,
		"Empires squad-observation-summary dumped=%d target_userid=%d source=dynamic-public-property read_only=1",
		dumped, targetUserId);
	return dumped > 0;
}

namespace
{
RCBotEmpiresLeadActuation::LeadActuationRequest
	g_empiresLeadActuationRequest;
bool g_empiresLeadActuationActive = false;
RCBotEmpiresLeadActuation::LeadActuationStatus
	g_empiresLeadActuationLastResult =
	RCBotEmpiresLeadActuation::LeadActuationStatus::NOT_REQUESTED;
float g_empiresLeadActuationSubmitTime = 0.0f;

CBotEmpires* leadActuationReader(int maxClients)
{
	for (int index = 1; index <= maxClients; ++index)
	{
		edict_t* pCandidate = engine->PEntityOfEntIndex(index);
		if (pCandidate == nullptr || pCandidate->IsFree())
			continue;
		CBot* pBase = CBots::getBotPointer(pCandidate);
		if (pBase == nullptr || pBase->isControlledTestOnly())
			continue;
		return static_cast<CBotEmpires*>(pBase);
	}
	return nullptr;
}
} // namespace

bool CBotEmpires::readLeadActuationParty(CBotEmpires* reader,
	RCBotEmpiresLeadActuation::LeadActuationParty& party)
{
	using namespace RCBotEmpiresLeadActuation;
	edict_t* pEdict = CBotGlobals::playerByUserId(
		party.identity.userId);
	if (pEdict == nullptr || pEdict->IsFree())
		return false;
	CBot* pBase = CBots::getBotPointer(pEdict);
	if (pBase == nullptr || pBase->isControlledTestOnly())
		return false;
	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(pEdict);
	if (pInfo == nullptr || !pInfo->IsConnected() ||
		pInfo->IsObserver())
		return false;
	party.connected = true;
	party.team = pInfo->GetTeamIndex();
	party.identity.entIndex = engine->IndexOfEdict(pEdict);
	int squad = -1;
	int position = -1;
	int squadXp = -1;
	party.squad = reader->readEmpiresEntityIntProperty(pEdict,
		"m_iSquad", squad) ? squad : -1;
	party.positionKnown = reader->readEmpiresEntityIntProperty(
		pEdict, "m_iSquadPosition", position);
	party.position = party.positionKnown ? position : -1;
	party.squadXpKnown = reader->readEmpiresEntityIntProperty(
		pEdict, "m_iEmpSquadXP", squadXp);
	party.squadXp = party.squadXpKnown ? squadXp : -1;
	return true;
}

bool CBotEmpires::requestEmpiresSquadLeadActuation(const int issuerUserId,
	const int targetUserId, const int contextGeneration)
{
	using namespace RCBotEmpiresLeadActuation;

	if (engine == nullptr || playerinfomanager == nullptr ||
		gpGlobals == nullptr || logger == nullptr || helpers == nullptr)
		return false;

	int maxClients = gpGlobals->maxClients;
	const int configuredMaxClients = CBotGlobals::maxClients();
	if (configuredMaxClients > 0 && configuredMaxClients < maxClients)
		maxClients = configuredMaxClients;
	if (maxClients >= RCBOT_MAXPLAYERS)
		maxClients = RCBOT_MAXPLAYERS - 1;

	CBotEmpires* reader = leadActuationReader(maxClients);
	if (reader == nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires squad-lead-actuation=rejected reason=no-reader-bot issuer_userid=%d target_userid=%d",
			issuerUserId, targetUserId);
		return false;
	}

	LeadActuationRequest request;
	request.issuer.identity.userId = issuerUserId;
	request.target.identity.userId = targetUserId;
	request.contextGeneration = contextGeneration;
	if (!readLeadActuationParty(reader, request.issuer) ||
		!readLeadActuationParty(reader, request.target))
	{
		logger->Log(LogLevel::INFO,
			"Empires squad-lead-actuation=rejected reason=unresolved-party issuer_userid=%d target_userid=%d",
			issuerUserId, targetUserId);
		return false;
	}

	LeadActuationRejectReason reason = LeadActuationRejectReason::NONE;
	if (!ValidateLeadActuationParties(request.issuer, request.target,
		reason))
	{
		logger->Log(LogLevel::INFO,
			"Empires squad-lead-actuation=rejected reason=%s issuer_userid=%d issuer_index=%d issuer_squad=%d issuer_position=%d target_userid=%d target_index=%d target_squad=%d target_position=%d",
			LeadActuationRejectReasonName(reason),
			request.issuer.identity.userId,
			request.issuer.identity.entIndex, request.issuer.squad,
			request.issuer.position, request.target.identity.userId,
			request.target.identity.entIndex, request.target.squad,
			request.target.position);
		return false;
	}

	// A newer request supersedes a still-pending one; the old request is
	// never retried silently.
	if (g_empiresLeadActuationActive)
	{
		logger->Log(LogLevel::INFO,
			"Empires squad-lead-actuation=superseded previous_issuer_userid=%d previous_target_userid=%d",
			g_empiresLeadActuationRequest.issuer.identity.userId,
			g_empiresLeadActuationRequest.target.identity.userId);
	}

	char command[64] = {};
	std::snprintf(command, sizeof(command), "emp_make_lead %d\n",
		request.target.identity.entIndex);
	edict_t* pIssuerEdict = CBotGlobals::playerByUserId(issuerUserId);
	if (pIssuerEdict == nullptr || pIssuerEdict->IsFree())
	{
		logger->Log(LogLevel::INFO,
			"Empires squad-lead-actuation=rejected reason=issuer-edict-lost issuer_userid=%d",
			issuerUserId);
		return false;
	}
	helpers->ClientCommand(pIssuerEdict, command);

	request.status = LeadActuationStatus::REQUESTED;
	g_empiresLeadActuationRequest = request;
	g_empiresLeadActuationActive = true;
	g_empiresLeadActuationSubmitTime = gpGlobals->curtime;
	logger->Log(LogLevel::INFO,
		"Empires squad-lead-actuation=requested issuer_userid=%d issuer_index=%d issuer_position=%d target_userid=%d target_index=%d target_position=%d squad=%d xp=%d command=%s source=production-client-command",
		request.issuer.identity.userId,
		request.issuer.identity.entIndex, request.issuer.position,
		request.target.identity.userId,
		request.target.identity.entIndex, request.target.position,
		request.issuer.squad, request.issuer.squadXp, command);
	return true;
}

RCBotEmpiresLeadActuation::LeadActuationStatus
	CBotEmpires::pollEmpiresSquadLeadActuation()
{
	using namespace RCBotEmpiresLeadActuation;

	if (!g_empiresLeadActuationActive)
		return g_empiresLeadActuationLastResult;
	if (engine == nullptr || playerinfomanager == nullptr ||
		gpGlobals == nullptr || logger == nullptr)
		return LeadActuationStatus::REQUESTED;

	int maxClients = gpGlobals->maxClients;
	const int configuredMaxClients = CBotGlobals::maxClients();
	if (configuredMaxClients > 0 && configuredMaxClients < maxClients)
		maxClients = configuredMaxClients;
	if (maxClients >= RCBOT_MAXPLAYERS)
		maxClients = RCBOT_MAXPLAYERS - 1;

	CBotEmpires* reader = leadActuationReader(maxClients);
	if (reader == nullptr)
		return LeadActuationStatus::REQUESTED;

	const LeadActuationRequest& request = g_empiresLeadActuationRequest;
	LeadActuationParty issuerNow;
	LeadActuationParty targetNow;
	issuerNow.identity.userId = request.issuer.identity.userId;
	targetNow.identity.userId = request.target.identity.userId;
	readLeadActuationParty(reader, issuerNow);
	readLeadActuationParty(reader, targetNow);

	// The live entity index is re-resolved on every poll and never cached
	// across disconnect/reconnect: a missing party reads back disconnected
	// and invalidates the request below.
	if (IsLeadActuationRequestStale(request, issuerNow, targetNow,
		request.contextGeneration))
	{
		g_empiresLeadActuationActive = false;
		g_empiresLeadActuationLastResult =
			LeadActuationStatus::INVALIDATED;
		logger->Log(LogLevel::INFO,
			"Empires squad-lead-actuation=invalidated issuer_userid=%d issuer_connected=%d issuer_squad=%d target_userid=%d target_connected=%d target_squad=%d",
			request.issuer.identity.userId,
			issuerNow.connected ? 1 : 0, issuerNow.squad,
			request.target.identity.userId,
			targetNow.connected ? 1 : 0, targetNow.squad);
		return LeadActuationStatus::INVALIDATED;
	}

	const LeadActuationStatus result = PollLeadActuationRequest(
		request, issuerNow, targetNow,
		gpGlobals->curtime - g_empiresLeadActuationSubmitTime);
	if (result == LeadActuationStatus::CONFIRMED ||
		result == LeadActuationStatus::FAILED)
	{
		g_empiresLeadActuationActive = false;
		g_empiresLeadActuationLastResult = result;
	}
	logger->Log(LogLevel::INFO,
		"Empires squad-lead-actuation=%s issuer_userid=%d issuer_position=%d target_userid=%d target_position=%d squad=%d xp_issuer=%d xp_target=%d source=public-sendprop",
		LeadActuationStatusName(result),
		request.issuer.identity.userId, issuerNow.position,
		request.target.identity.userId, targetNow.position,
		request.issuer.squad, issuerNow.squadXp, targetNow.squadXp);
	return result;
}

namespace
{
// Bounded death-vacancy Engineer Mass Revive transaction. One static
// slot: a second squad's vacancy waits until the active one terminates.
// intimacies with the actuation primitive are sequential (promote, then
// much later restore), never concurrent.
struct VacancyReviveTransaction
{
	bool active = false;
	int team = -1;
	int squad = -1;
	int generation = -1;
	int originalLeaderUserId = -1;
	int engineerUserId = -1;
	int originalLeaderPosition = -1;
	int engineerPosition = -1;
	// Death epoch of the original leader at transaction start: the
	// consumed-vacancy identity. Constant for one standing death,
	// advanced by the leader's respawn, so a later death re-opens.
	unsigned long long leaderLifeEpoch = 0;
	// Membership fingerprint: sorted (userid, position) pairs. A userid
	// set change is churn; a position change alone only voids exactness.
	std::vector<std::int64_t> fingerprint;
	int expectedReviveCount = 0;
	int requiredCost = 0;
	int balanceBefore = -1;
	RCBotEmpiresVacancyRevive::VacancyPhase phase =
		RCBotEmpiresVacancyRevive::VacancyPhase::IDLE;
	bool reviveIssued = false;
	int reviveVerifyPasses = 0;
	int reviveBalanceBefore = -1;
	std::vector<int> reviveDeadBefore;
	int reviveBalanceAfter = -1;
	int revivedCount = 0;
	bool restoreIssued = false;
	float reviveSettledTime = -1.0f;
	bool restoreDeferredLogged = false;
};

VacancyReviveTransaction g_vacancyTransaction;

// Consumed vacancy identities: one intervention per distinct leader
// death epoch. Keyed by (team, squad, original leader, that leader's
// life epoch), never by squad-goal generation alone.
RCBotEmpiresVacancyRevive::VacancyConsumedIdentities
	g_vacancyConsumedIdentities;
// Last blocked-identity report, so a standing consumed vacancy does
// not log every think.
RCBotEmpiresVacancyRevive::VacancyIdentity g_vacancyLastBlocked;
bool g_vacancyLastBlockedValid = false;

std::int64_t VacancyFingerprintEntry(const int userId, const int position)
{
	return (static_cast<std::int64_t>(userId) << 32) |
		static_cast<std::uint32_t>(position);
}

bool VacancySameUserIds(const std::vector<std::int64_t>& fingerprint,
	const std::vector<RCBotEmpiresVacancyRevive::VacancyMember>& members)
{
	std::vector<int> current;
	for (std::vector<RCBotEmpiresVacancyRevive::VacancyMember>::
		const_iterator it = members.begin(); it != members.end();
		++it)
		current.push_back(it->userId);
	std::sort(current.begin(), current.end());
	if (current.size() != fingerprint.size())
		return false;
	for (std::vector<int>::size_type i = 0; i < current.size(); ++i)
	{
		if (static_cast<int>(fingerprint[i] >> 32) != current[i])
			return false;
	}
	return true;
}
} // namespace

void CBotEmpires::updateEmpiresVacancyReviveIntervention()
{
	using namespace RCBotEmpiresVacancyRevive;
	using namespace RCBotEmpiresLeadActuation;

	if (!rcbot_empires_vacancy_revive.GetBool())
		return;
	if (m_bBootstrapOnly || m_bControllerProbeOnly ||
		m_pEdict == nullptr || m_pPlayerInfo == nullptr ||
		engine == nullptr || playerinfomanager == nullptr ||
		gpGlobals == nullptr || helpers == nullptr || logger == nullptr)
		return;
	// Only a living connected fake client drives. Dead bots never drive;
	// the dead original leader is still the promotion *issuer* later.
	if (!m_pPlayerInfo->IsConnected() || !m_pPlayerInfo->IsFakeClient() ||
		m_pPlayerInfo->IsObserver() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0)
		return;
	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return;
	int ownSquad = -1;
	if (!readEmpiresEntityIntProperty(m_pEdict, "m_iSquad", ownSquad) ||
		ownSquad <= 0)
		return;

	int maxClients = gpGlobals->maxClients;
	const int configuredMaxClients = CBotGlobals::maxClients();
	if (configuredMaxClients > 0 && configuredMaxClients < maxClients)
		maxClients = configuredMaxClients;
	if (maxClients >= RCBOT_MAXPLAYERS)
		maxClients = RCBOT_MAXPLAYERS - 1;

	// Snapshot every connected same-squad member through public state.
	std::vector<VacancyMember> members;
	int balance = -1;
	bool balanceKnown = false;
	for (int index = 1; index <= maxClients; ++index)
	{
		edict_t* pPlayer = engine->PEntityOfEntIndex(index);
		if (pPlayer == nullptr || pPlayer->IsFree())
			continue;
		IPlayerInfo* pInfo =
			playerinfomanager->GetPlayerInfo(pPlayer);
		if (pInfo == nullptr || !pInfo->IsConnected())
			continue;
		if (pInfo->GetTeamIndex() != team)
			continue;
		int memberSquad = -1;
		if (!readEmpiresEntityIntProperty(pPlayer, "m_iSquad",
			memberSquad) || memberSquad != ownSquad)
			continue;
		VacancyMember member;
		member.userId = engine->GetPlayerUserId(pPlayer);
		if (member.userId <= 0)
			continue;
		member.team = team;
		member.squad = memberSquad;
		member.connected = true;
		int position = -1;
		member.positionKnown = readEmpiresEntityIntProperty(
			pPlayer, "m_iSquadPosition", position);
		member.position = member.positionKnown ? position : -1;
		const int health = pInfo->GetHealth();
		member.alive = !pInfo->IsObserver() && !pInfo->IsDead() &&
			health > 0;
		int classId = -1;
		bool classDesired = false;
		bool classDesiredKnown = false;
		const char* propertyName = nullptr;
		member.isEngineer = readEmpiresClass(pPlayer, classId,
			classDesired, classDesiredKnown, propertyName) &&
			classId == EMP_CLASS_ENGINEER;
		CBot* pMemberBase = CBots::getBotPointer(pPlayer);
		member.controlled = pMemberBase != nullptr &&
			!pMemberBase->isControlledTestOnly();
		int memberXp = -1;
		if (readEmpiresEntityIntProperty(pPlayer, "m_iEmpSquadXP",
			memberXp) && !balanceKnown)
		{
			balance = memberXp;
			balanceKnown = true;
		}
		members.push_back(member);
	}

	// Driver election: exactly the lowest userid among living connected
	// controlled members acts, so concurrent drivers can never double
	// issue the single promotion or the single Revive.
	int lowestLiving = -1;
	for (std::vector<VacancyMember>::const_iterator it = members.begin();
		it != members.end(); ++it)
	{
		if (!it->connected || !it->controlled || !it->alive)
			continue;
		if (lowestLiving < 0 || it->userId < lowestLiving)
			lowestLiving = it->userId;
	}
	const int ownUserId = engine->GetPlayerUserId(m_pEdict);
	if (!IsVacancyReviveDriver(ownUserId, lowestLiving))
		return;

	VacancyReviveTransaction& transaction = g_vacancyTransaction;
	if (transaction.active && (transaction.team != team ||
		transaction.squad != ownSquad))
		return;

	if (!transaction.active)
	{
		const VacancyEvaluation evaluation = EvaluateVacancyRevive(
			members, team, ownSquad, balanceKnown, balance);
		if (evaluation.eligibility != VacancyEligibility::ELIGIBLE)
			return;
		const int generation = m_iEmpiresSquadGoalGeneration;
		// Vacancy epoch: the dead original leader's own life
		// generation. Constant while they stay dead, advanced by
		// their respawn, so a later death is a NEW vacancy even
		// when the squad-goal generation never moved (notably the
		// steady -1 of goal-less squads). Squad-goal generation
		// stays transaction context only, never the consumed key.
		unsigned long long leaderLifeEpoch = 0;
		edict_t* pLeaderEdict = CBotGlobals::playerByUserId(
			evaluation.originalLeaderUserId);
		if (pLeaderEdict != nullptr && !pLeaderEdict->IsFree())
		{
			CBot* pLeaderBot =
				CBots::getBotPointer(pLeaderEdict);
			if (pLeaderBot != nullptr)
			{
				leaderLifeEpoch =
					pLeaderBot->getRecoveryLifeGeneration();
			}
		}
		if (leaderLifeEpoch == 0)
			return;
		VacancyIdentity identity;
		identity.team = team;
		identity.squad = ownSquad;
		identity.originalLeaderUserId =
			evaluation.originalLeaderUserId;
		identity.leaderLifeEpoch = leaderLifeEpoch;
		if (g_vacancyConsumedIdentities.IsConsumed(identity))
		{
			if (!g_vacancyLastBlockedValid ||
				!SameVacancyIdentity(g_vacancyLastBlocked,
				identity))
			{
				g_vacancyLastBlocked = identity;
				g_vacancyLastBlockedValid = true;
				logger->Log(LogLevel::INFO,
					"Empires vacancy-revive event=blocked-consumed team=%d squad=%d generation=%d epoch=%llu orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=same-vacancy-already-attempted",
					team, ownSquad, generation,
					identity.leaderLifeEpoch,
					evaluation.originalLeaderUserId,
					evaluation.engineerUserId,
					evaluation.expectedReviveCount,
					evaluation.requiredCost,
					evaluation.balanceBefore);
			}
			return;
		}
		// Start consumes the vacancy identity even if the run later
		// aborts.
		g_vacancyConsumedIdentities.Consume(identity);
		transaction = VacancyReviveTransaction();
		transaction.active = true;
		transaction.team = team;
		transaction.squad = ownSquad;
		transaction.generation = generation;
		transaction.leaderLifeEpoch = leaderLifeEpoch;
		transaction.originalLeaderUserId =
			evaluation.originalLeaderUserId;
		transaction.engineerUserId = evaluation.engineerUserId;
		transaction.engineerPosition = evaluation.engineerPosition;
		for (std::vector<VacancyMember>::const_iterator it =
			members.begin(); it != members.end(); ++it)
		{
			if (it->userId == evaluation.originalLeaderUserId)
				transaction.originalLeaderPosition = it->position;
			transaction.fingerprint.push_back(
				VacancyFingerprintEntry(it->userId, it->position));
		}
		std::sort(transaction.fingerprint.begin(),
			transaction.fingerprint.end());
		transaction.expectedReviveCount =
			evaluation.expectedReviveCount;
		transaction.requiredCost = evaluation.requiredCost;
		transaction.balanceBefore = evaluation.balanceBefore;
		transaction.phase = VacancyPhase::PROMOTING;
		logger->Log(LogLevel::INFO,
			"Empires vacancy-revive event=started team=%d squad=%d generation=%d epoch=%llu orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=eligible",
			team, ownSquad, generation,
			transaction.leaderLifeEpoch,
			transaction.originalLeaderUserId,
			transaction.engineerUserId,
			transaction.expectedReviveCount,
			transaction.requiredCost, transaction.balanceBefore);
		if (!requestEmpiresSquadLeadActuation(
			transaction.originalLeaderUserId,
			transaction.engineerUserId))
		{
			transaction.phase = VacancyPhase::ABORTED;
			transaction.active = false;
			logger->Log(LogLevel::INFO,
				"Empires vacancy-revive event=aborted team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=promotion-rejected",
				team, ownSquad, generation,
				transaction.originalLeaderUserId,
				transaction.engineerUserId,
				transaction.expectedReviveCount,
				transaction.requiredCost,
				transaction.balanceBefore);
		}
		else
		{
			logger->Log(LogLevel::INFO,
				"Empires vacancy-revive event=promotion-requested team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=primitive-submitted",
				team, ownSquad, generation,
				transaction.originalLeaderUserId,
				transaction.engineerUserId,
				transaction.expectedReviveCount,
				transaction.requiredCost,
				transaction.balanceBefore);
		}
		return;
	}

	if (transaction.phase == VacancyPhase::PROMOTING)
	{
		const LeadActuationStatus status =
			pollEmpiresSquadLeadActuation();
		if (status == LeadActuationStatus::REQUESTED)
			return;
		if (status != LeadActuationStatus::CONFIRMED)
		{
			transaction.phase = VacancyPhase::ABORTED;
			transaction.active = false;
			logger->Log(LogLevel::INFO,
				"Empires vacancy-revive event=aborted team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=promotion-%s",
				transaction.team, transaction.squad,
				transaction.generation,
				transaction.originalLeaderUserId,
				transaction.engineerUserId,
				transaction.expectedReviveCount,
				transaction.requiredCost,
				transaction.balanceBefore,
				LeadActuationStatusName(status));
			return;
		}
		if (!VacancySameUserIds(transaction.fingerprint, members))
		{
			transaction.phase = VacancyPhase::ABORTED;
			transaction.active = false;
			logger->Log(LogLevel::INFO,
				"Empires vacancy-revive event=aborted team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=membership-churn-after-promotion",
				transaction.team, transaction.squad,
				transaction.generation,
				transaction.originalLeaderUserId,
				transaction.engineerUserId,
				transaction.expectedReviveCount,
				transaction.requiredCost,
				transaction.balanceBefore);
			return;
		}
		transaction.phase = PhaseAfterPromotion(true);
		logger->Log(LogLevel::INFO,
			"Empires vacancy-revive event=promotion-confirmed team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=readback-swap",
			transaction.team, transaction.squad,
			transaction.generation,
			transaction.originalLeaderUserId,
			transaction.engineerUserId,
			transaction.expectedReviveCount,
			transaction.requiredCost, transaction.balanceBefore);
		return;
	}

	if (transaction.phase == VacancyPhase::REVIVING)
	{
		if (!transaction.reviveIssued)
		{
			if (!VacancySameUserIds(transaction.fingerprint,
				members))
			{
				transaction.phase = VacancyPhase::ABORTED;
				transaction.active = false;
				logger->Log(LogLevel::INFO,
					"Empires vacancy-revive event=aborted team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=membership-churn-before-revive",
					transaction.team, transaction.squad,
					transaction.generation,
					transaction.originalLeaderUserId,
					transaction.engineerUserId,
					transaction.expectedReviveCount,
					transaction.requiredCost,
					transaction.balanceBefore);
				return;
			}
			bool engineerAlive = false;
			bool engineerAtZero = false;
			int actualDead = 0;
			for (std::vector<VacancyMember>::const_iterator it =
				members.begin(); it != members.end(); ++it)
			{
				if (it->userId == transaction.engineerUserId)
				{
					engineerAlive = it->alive;
					engineerAtZero = it->positionKnown &&
						it->position == 0;
				}
				else if (!it->alive)
					++actualDead;
			}
			if (!ShouldSpendRevive(engineerAlive, engineerAtZero,
				actualDead, balanceKnown, balance))
			{
				transaction.phase = VacancyPhase::RESTORING;
				logger->Log(LogLevel::INFO,
					"Empires vacancy-revive event=revive-skipped team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=%d reason=actual-state-no-spend",
					transaction.team, transaction.squad,
					transaction.generation,
					transaction.originalLeaderUserId,
					transaction.engineerUserId,
					transaction.expectedReviveCount,
					transaction.requiredCost,
					transaction.balanceBefore, balance);
				return;
			}
			edict_t* pEngineerEdict = CBotGlobals::playerByUserId(
				transaction.engineerUserId);
			IPlayerInfo* pEngineerInfo =
				pEngineerEdict != nullptr ?
				playerinfomanager->GetPlayerInfo(
				pEngineerEdict) : nullptr;
			if (pEngineerEdict == nullptr ||
				pEngineerEdict->IsFree() || pEngineerInfo == nullptr ||
				!pEngineerInfo->IsConnected() ||
				pEngineerInfo->IsObserver() || pEngineerInfo->IsDead() ||
				pEngineerInfo->GetHealth() <= 0)
			{
				transaction.phase = VacancyPhase::ABORTED;
				transaction.active = false;
				logger->Log(LogLevel::INFO,
					"Empires vacancy-revive event=aborted team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=engineer-unavailable-at-activation",
					transaction.team, transaction.squad,
					transaction.generation,
					transaction.originalLeaderUserId,
					transaction.engineerUserId,
					transaction.expectedReviveCount,
					transaction.requiredCost,
					transaction.balanceBefore);
				return;
			}
			transaction.reviveDeadBefore.clear();
			for (std::vector<VacancyMember>::const_iterator it =
				members.begin(); it != members.end(); ++it)
			{
				if (!it->alive && it->userId !=
					transaction.engineerUserId)
					transaction.reviveDeadBefore.push_back(
						it->userId);
			}
			transaction.reviveBalanceBefore = balance;
			transaction.reviveIssued = true;
			transaction.reviveVerifyPasses = 0;
			helpers->ClientCommand(pEngineerEdict,
				"emp_menu_command 48 1\n");
			logger->Log(LogLevel::INFO,
				"Empires vacancy-revive event=revive-requested team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=-1 reason=single-activation",
				transaction.team, transaction.squad,
				transaction.generation,
				transaction.originalLeaderUserId,
				transaction.engineerUserId,
				transaction.expectedReviveCount,
				transaction.requiredCost,
				transaction.balanceBefore);
			return;
		}
		int nowAlive = 0;
		for (std::vector<int>::const_iterator it =
			transaction.reviveDeadBefore.begin();
			it != transaction.reviveDeadBefore.end(); ++it)
		{
			for (std::vector<VacancyMember>::const_iterator member =
				members.begin(); member != members.end();
				++member)
			{
				if (member->userId == *it && member->alive)
					++nowAlive;
			}
		}
		transaction.revivedCount = nowAlive;
		const int expectedSpend = ReviveCostForTargets(
			static_cast<int>(transaction.reviveDeadBefore.size()));
		if (balanceKnown)
			transaction.reviveBalanceAfter = balance;
		const bool fullSuccess = balanceKnown &&
			transaction.reviveBalanceAfter >= 0 &&
			transaction.reviveBalanceBefore -
			transaction.reviveBalanceAfter == expectedSpend &&
			nowAlive == static_cast<int>(
			transaction.reviveDeadBefore.size());
		const bool partialSpend = !fullSuccess && balanceKnown &&
			transaction.reviveBalanceAfter >= 0 &&
			transaction.reviveBalanceBefore >
			transaction.reviveBalanceAfter;
		++transaction.reviveVerifyPasses;
		if (!fullSuccess && !partialSpend &&
			transaction.reviveVerifyPasses <
			kVacancyReviveVerifyPasses)
			return;
		transaction.phase = VacancyPhase::RESTORING;
		if (fullSuccess || partialSpend)
			transaction.reviveSettledTime = gpGlobals->curtime;
		logger->Log(LogLevel::INFO,
			"Empires vacancy-revive event=revive-%s team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=%d reason=revived=%d",
			fullSuccess ? "verified" :
			(partialSpend ? "partial" : "failed"),
			transaction.team, transaction.squad,
			transaction.generation,
			transaction.originalLeaderUserId,
			transaction.engineerUserId,
			transaction.expectedReviveCount,
			transaction.requiredCost,
			transaction.reviveBalanceBefore,
			transaction.reviveBalanceAfter, nowAlive);
		return;
	}

	if (transaction.phase == VacancyPhase::RESTORING)
	{
		if (!transaction.restoreIssued)
		{
			// The Revive activation closed the Engineer's native
			// client-command flood gate; submitting restore inside
			// it would be silently eaten. Wait for the verified
			// 2.0s gate plus margin instead of spending the single
			// restoration attempt into the void.
			if (!RestoreGateOpen(gpGlobals->curtime,
				transaction.reviveSettledTime))
			{
				if (!transaction.restoreDeferredLogged)
				{
					transaction.restoreDeferredLogged = true;
					logger->Log(LogLevel::INFO,
						"Empires vacancy-revive event=restoration-deferred team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=%d reason=flood-gate",
						transaction.team, transaction.squad,
						transaction.generation,
						transaction.originalLeaderUserId,
						transaction.engineerUserId,
						transaction.expectedReviveCount,
						transaction.requiredCost,
						transaction.balanceBefore,
						transaction.reviveBalanceAfter);
				}
				return;
			}
			bool originalAlive = false;
			for (std::vector<VacancyMember>::const_iterator it =
				members.begin(); it != members.end(); ++it)
			{
				if (it->userId ==
					transaction.originalLeaderUserId &&
					it->connected && it->alive &&
					it->squad == transaction.squad)
					originalAlive = true;
			}
			if (!originalAlive)
			{
				transaction.phase = VacancyPhase::COMPLETE;
				transaction.active = false;
				logger->Log(LogLevel::INFO,
					"Empires vacancy-revive event=complete team=%d squad=%d generation=%d epoch=%llu orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=%d reason=engineer-remains-leader",
					transaction.team, transaction.squad,
					transaction.generation,
					transaction.leaderLifeEpoch,
					transaction.originalLeaderUserId,
					transaction.engineerUserId,
					transaction.expectedReviveCount,
					transaction.requiredCost,
					transaction.balanceBefore,
					transaction.reviveBalanceAfter);
				return;
			}
			if (!requestEmpiresSquadLeadActuation(
				transaction.engineerUserId,
				transaction.originalLeaderUserId))
			{
				transaction.phase = VacancyPhase::COMPLETE;
				transaction.active = false;
				logger->Log(LogLevel::INFO,
					"Empires vacancy-revive event=complete team=%d squad=%d generation=%d epoch=%llu orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=%d reason=restore-invalid-leave-living-leader",
					transaction.team, transaction.squad,
					transaction.generation,
					transaction.leaderLifeEpoch,
					transaction.originalLeaderUserId,
					transaction.engineerUserId,
					transaction.expectedReviveCount,
					transaction.requiredCost,
					transaction.balanceBefore,
					transaction.reviveBalanceAfter);
				return;
			}
			transaction.restoreIssued = true;
			logger->Log(LogLevel::INFO,
				"Empires vacancy-revive event=restoration-requested team=%d squad=%d generation=%d orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=%d reason=explicit-restore",
				transaction.team, transaction.squad,
				transaction.generation,
				transaction.originalLeaderUserId,
				transaction.engineerUserId,
				transaction.expectedReviveCount,
				transaction.requiredCost, transaction.balanceBefore,
				transaction.reviveBalanceAfter);
			return;
		}
		const LeadActuationStatus status =
			pollEmpiresSquadLeadActuation();
		if (status == LeadActuationStatus::REQUESTED)
			return;
		transaction.phase = VacancyPhase::COMPLETE;
		transaction.active = false;
		logger->Log(LogLevel::INFO,
			"Empires vacancy-revive event=complete team=%d squad=%d generation=%d epoch=%llu orig=%d eng=%d expected=%d cost=%d balance_before=%d balance_after=%d reason=restore-%s",
			transaction.team, transaction.squad,
			transaction.generation,
			transaction.leaderLifeEpoch,
			transaction.originalLeaderUserId,
			transaction.engineerUserId,
			transaction.expectedReviveCount,
			transaction.requiredCost, transaction.balanceBefore,
			transaction.reviveBalanceAfter,
			status == LeadActuationStatus::CONFIRMED ?
			"confirmed" : "failed-leave-living-leader");
		return;
	}
}

void CBotEmpires::observeEmpiresSquadRuntimeShadow()
{
	if (m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pPlayerInfo == nullptr || engine == nullptr ||
		playerinfomanager == nullptr || gpGlobals == nullptr ||
		!m_pPlayerInfo->IsConnected() || !m_pPlayerInfo->IsFakeClient() ||
		m_pPlayerInfo->IsObserver())
		return;

	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return;

	int squad = -1;
	if (!readEmpiresEntityIntProperty(m_pEdict, "m_iSquad", squad) ||
		squad <= 0 || squad > static_cast<int>(
			RCBotEmpiresSquadRuntime::Adapter::kMaxSquadsPerTeam))
		return;

	RCBotEmpiresSquadRuntime::Input input;
	input.team = team;
	input.squadId = static_cast<RCBotOrder::RecipientId>(squad);
	input.now = gpGlobals->curtime;
	bool parentSeen = false;
	bool parentConsistent = true;
	Vector parentTarget = vec3_origin;

	for (int index = 1; index <= gpGlobals->maxClients; ++index)
	{
		edict_t* pPlayer = engine->PEntityOfEntIndex(index);
		IPlayerInfo* pInfo = pPlayer != nullptr ?
			playerinfomanager->GetPlayerInfo(pPlayer) : nullptr;
		if (pPlayer == nullptr || pPlayer->IsFree() || pInfo == nullptr ||
			pInfo->GetTeamIndex() != team)
			continue;

		int observedSquad = -1;
		if (!readEmpiresEntityIntProperty(pPlayer, "m_iSquad", observedSquad) ||
			observedSquad != squad)
			continue;

		int position = -1;
		(void)readEmpiresEntityIntProperty(pPlayer, "m_iSquadPosition", position);
		const int userId = engine->GetPlayerUserId(pPlayer);
		RCBotEmpiresSquadRuntime::MemberObservation observation;
		observation.human = !pInfo->IsFakeClient();
		observation.state.memberId = userId > 0 ?
			static_cast<RCBotOrder::RecipientId>(userId) :
			static_cast<RCBotOrder::RecipientId>(100000 + index);
		observation.state.connected = pInfo->IsConnected();
		observation.state.alive = observation.state.connected &&
			!pInfo->IsObserver() && !pInfo->IsDead() && pInfo->GetHealth() > 0;
		observation.state.eligible = observation.state.alive &&
			pInfo->IsFakeClient();
		observation.state.isLeader = position == 0;
		observation.state.squadPosition = position;
		const Vector origin = pInfo->GetAbsOrigin();
		const Vector entityOrigin = CBotGlobals::entityOrigin(pPlayer);
		const Vector observedOrigin = origin.IsValid() ? origin : entityOrigin;
		observation.state.origin = {
			observedOrigin.x, observedOrigin.y, observedOrigin.z };
		const Vector velocity = CBotGlobals::getVelocity(pPlayer);
		const Vector observedVelocity = velocity.IsValid() ? velocity : vec3_origin;
		observation.state.velocity = {
			observedVelocity.x, observedVelocity.y, observedVelocity.z };
		if (!observation.state.connected || pInfo->IsObserver())
			observation.state.lifecycle =
				RCBotSquadDecision::MemberLifecycle::DISCONNECTED;
		else if (!observation.state.alive)
			observation.state.lifecycle = RCBotSquadDecision::MemberLifecycle::DEAD;
		else
			observation.state.lifecycle = RCBotSquadDecision::MemberLifecycle::ACTIVE;
		input.members.push_back(observation);

		int commandType = 0;
		Vector commandTarget = vec3_origin;
		const bool commandKnown = readEmpiresEntityIntProperty(
			pPlayer, "m_iCommandType", commandType);
		const bool targetKnown = readEmpiresEntityVectorProperty(
			pPlayer, "m_vCommandLocation", commandTarget);
		if (commandKnown && targetKnown && commandType == kEmpiresCommanderMoveType)
		{
			if (!parentSeen)
			{
				parentSeen = true;
				parentTarget = commandTarget;
			}
			else if ((parentTarget - commandTarget).LengthSqr() > 1.0f)
				parentConsistent = false;
		}
	}

	// Keep the normal SendProp observation as a fallback, but prefer a current
	// accepted Phase-10.6 SQUAD parent when its team-scoped recipient matches the
	// live membership being observed. This carries the actual Order Protocol key
	// into the shadow decision without dispatching movement or changing SendProps.
	input.parentOrder.present = parentSeen && parentConsistent;
	if (input.parentOrder.present)
	{
		input.parentOrder.target = {
			parentTarget.x, parentTarget.y, parentTarget.z };
		// Empires does not expose the protocol deadline as a SendProp.  Zero is
		// the neutral observed value; the order identity is derived by the
		// adapter only when no protocol key is available.
		input.parentOrder.deadline = 0.0;
		input.parentOrder.orderState = RCBotOrder::OrderState::EXECUTING;
	}
	const RCBotOrder::OrderRecord protocolParent =
		g_empireSquadMoveRecipient.ParentSnapshot();
	const RCBotOrder::RecipientId scopedSquadId =
		RCBotCommanderMoveTestFixture::TeamScopedSquadId(team,
			static_cast<RCBotOrder::RecipientId>(squad));
	const bool ownsCurrentParent =
		g_empireSquadMoveRecipient.HasActiveParent() && protocolParent.valid &&
		protocolParent.recipient.kind == RCBotOrder::RecipientKind::SQUAD &&
		protocolParent.recipient.id == scopedSquadId;
	if (ownsCurrentParent)
	{
		input.parentOrder.present = true;
		input.parentOrder.parentOrder = {
			protocolParent.orderId, protocolParent.generation };
		input.parentOrder.target = protocolParent.target;
		input.parentOrder.deadline = protocolParent.deadline;
		input.parentOrder.orderState = protocolParent.state;
	}
	input.freezeSpatialPlanForActiveParent = ownsCurrentParent;

	const RCBotEmpiresSquadRuntime::Result decision =
		g_empireSquadRuntimeShadow.Observe(input,
			RecordEmpiresSquadRuntimeTelemetry, this);
	if (!ownsCurrentParent || !g_empireSquadMoveRecipient.HasActiveParent())
		return;

	const double now = input.now;
	const RCBotOrder::OrderKey parentKey = {
		protocolParent.orderId, protocolParent.generation };
	(void)g_empireSquadMoveRecipient.Update(now);
	if (!g_empireSquadMoveRecipient.HasActiveParent())
		return;

	RCBotSquadOrder::SquadMembershipObservation currentMembership =
		g_empireSquadMoveIdentityTracker.Observe(scopedSquadId,
			[&input]() {
				std::vector<RCBotOrder::RecipientId> ids;
				ids.reserve(input.members.size());
				for (const auto& member : input.members)
					ids.push_back(member.state.memberId);
				return ids;
			}());
	if (currentMembership.recipient != protocolParent.recipient)
	{
		(void)g_empireSquadMoveRecipient.CancelActiveParent(
			RCBotOrder::DetailReason::STALE_CONTEXT, now);
		return;
	}
	RCBotSquadOrder::SquadMembershipSnapshot currentSnapshot;
	currentSnapshot.recipient = currentMembership.recipient;
	currentSnapshot.members = currentMembership.members;
	(void)g_empireSquadMoveRecipient.ObserveMembership(parentKey,
		currentSnapshot, now);
	for (const auto& member : input.members)
	{
		if (member.state.eligible)
			continue;
		(void)g_empireSquadMoveRecipient.ObserveMemberLifecycle(parentKey,
			protocolParent.recipient, member.state.memberId,
			RCBotOrder::DetailReason::DEATH_OR_LIFECYCLE, now);
	}
	if (!g_empireSquadMoveRecipient.HasActiveParent())
		return;

	if (!decision.Accepted() || !decision.current || !decision.autonomous ||
		decision.parentOrder.orderId != parentKey.orderId ||
		decision.parentOrder.generation != parentKey.generation ||
		decision.identity.squadGeneration != protocolParent.recipient.generation)
	{
		(void)g_empireSquadMoveRecipient.CancelActiveParent(
			RCBotOrder::DetailReason::STALE_CONTEXT, now);
		return;
	}
	if (g_empireSquadMoveRecipient.ChildrenDispatched() &&
		!decision.semanticChange)
		return;

	RCBotSquadMovement::Input movementInput;
	movementInput.identity = decision.identity;
	movementInput.decisionRevision = decision.decisionRevision;
	movementInput.formationGeneration = decision.formationGeneration;
	movementInput.parentOrder = decision.parentOrder;
	movementInput.participation = decision.decision.participation;
	movementInput.formation = decision.formation;
	movementInput.now = now;
	for (const auto& member : decision.decision.canonicalMembers)
		movementInput.memberSnapshot.push_back(member.memberId);
	const RCBotSquadMovement::Result movement =
		RCBotSquadMovement::Resolve(movementInput);
	if (!movement.valid)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::ERROR,
				"Empires squad spatial movement rejected parent=%llu/%llu squad_generation=%llu decision_revision=%llu formation_generation=%llu diagnostic=%d",
				static_cast<unsigned long long>(parentKey.orderId),
				static_cast<unsigned long long>(parentKey.generation),
				static_cast<unsigned long long>(decision.identity.squadGeneration),
				static_cast<unsigned long long>(decision.decisionRevision),
				static_cast<unsigned long long>(decision.formationGeneration),
				static_cast<int>(movement.diagnostic));
		if (!g_empireSquadMoveRecipient.ChildrenDispatched())
		{
			std::vector<RCBotSquadOrder::ChildMovementTarget> excluded;
			for (const auto memberId :
				g_empireSquadMoveRecipient.ActiveMemberSnapshot())
			{
				RCBotSquadOrder::ChildMovementTarget target;
				target.memberId = memberId;
				excluded.push_back(target);
			}
			(void)g_empireSquadMoveRecipient.DispatchChildren(parentKey,
				excluded, now);
		}
		else
			(void)g_empireSquadMoveRecipient.CancelActiveParent(
				RCBotOrder::DetailReason::STALE_CONTEXT, now);
		return;
	}

	if (!g_empireSquadMoveRecipient.ChildrenDispatched())
	{
		const std::vector<RCBotOrder::RecipientId> snapshotMembers =
			g_empireSquadMoveRecipient.ActiveMemberSnapshot();
		if (snapshotMembers.empty())
		{
			(void)g_empireSquadMoveRecipient.CancelActiveParent(
				RCBotOrder::DetailReason::STALE_CONTEXT, now);
			return;
		}
		std::vector<RCBotSquadOrder::ChildMovementTarget> targets;
		targets.reserve(snapshotMembers.size());
		for (const auto memberId : snapshotMembers)
		{
			RCBotSquadOrder::ChildMovementTarget target;
			target.memberId = memberId;
			const auto resolved = std::find_if(movement.members.begin(),
				movement.members.end(), [memberId](const auto& value) {
					return value.memberId == memberId;
				});
			if (resolved != movement.members.end() && resolved->hasTarget)
			{
				target.hasTarget = true;
				target.target = resolved->target;
				target.slotId = resolved->slotId;
				target.squadGeneration = decision.identity.squadGeneration;
				target.decisionRevision = decision.decisionRevision;
				target.formationGeneration = decision.formationGeneration;
				target.claimResourceKind = static_cast<std::uint8_t>(
					resolved->claimKey.resourceKind);
				target.claimResourceId = resolved->claimKey.resourceId;
				target.claimResourceGeneration =
					resolved->claimKey.resourceGeneration;
			}
			targets.push_back(target);
		}
		(void)g_empireSquadMoveRecipient.SetNavigationSinks(
			StartEmpiresSquadSpatialNavigation,
			CancelEmpiresSquadSpatialNavigation);
		(void)g_empireSquadMoveRecipient.DispatchChildren(parentKey,
			targets, now);
		return;
	}

	for (const auto& resolved : movement.members)
	{
		const std::vector<RCBotSquadOrder::ChildExecution> children =
			g_empireSquadMoveRecipient.ActiveChildren();
		const auto active = std::find_if(children.begin(), children.end(),
			[&resolved](const auto& child) {
				return child.memberId == resolved.memberId &&
					!RCBotOrder::IsTerminal(child.record.state);
			});
		if (active == children.end())
			continue;
		if (!resolved.hasTarget)
		{
			(void)g_empireSquadMoveRecipient.ObserveMemberLifecycle(parentKey,
				protocolParent.recipient, resolved.memberId,
				RCBotOrder::DetailReason::DEATH_OR_LIFECYCLE, now);
			continue;
		}
		RCBotSquadOrder::ChildMovementTarget target;
		target.memberId = resolved.memberId;
		target.hasTarget = true;
		target.target = resolved.target;
		target.slotId = resolved.slotId;
		target.squadGeneration = decision.identity.squadGeneration;
		target.decisionRevision = decision.decisionRevision;
		target.formationGeneration = decision.formationGeneration;
		target.claimResourceKind = static_cast<std::uint8_t>(
			resolved.claimKey.resourceKind);
		target.claimResourceId = resolved.claimKey.resourceId;
		target.claimResourceGeneration = resolved.claimKey.resourceGeneration;
		(void)g_empireSquadMoveRecipient.ReplaceChildMovementTarget(parentKey,
			active->key, resolved.memberId, target, now);
	}
}

void CBotEmpires::observeEmpiresSquadGameplay()
{
	// Empires exposes the squad membership tuple through public SendProps and
	// accepts the normal client command `emp_squad_join <id>`. Keep this first
	// gameplay step deliberately small: only normal fake clients may cooperate,
	// a human-owned squad is never selected, and all decisions are based on the
	// public squad id/position readback. Formation movement and leader powers are
	// separate, not implied by this membership bridge.
	if (m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pPlayerInfo == nullptr || helpers == nullptr || engine == nullptr ||
		playerinfomanager == nullptr || gpGlobals == nullptr ||
		!m_pPlayerInfo->IsConnected() || !m_pPlayerInfo->IsFakeClient() ||
		m_pPlayerInfo->IsObserver() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0 || !rcbot_bots_form_squads.GetBool())
	{
		return;
	}

	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return;

	int squad = -1;
	int position = -1;
	bool explicitLeader = false;
	if (!readEmpiresEntityIntProperty(m_pEdict, "m_iSquad", squad))
		return;
	(void)readEmpiresEntityIntProperty(m_pEdict, "m_iSquadPosition", position);
	const bool explicitLeaderKnown = readEmpiresEntityBoolProperty(m_pEdict,
		"m_bSquadLeader", explicitLeader);
	const bool explicitLeaderChanged = explicitLeaderKnown !=
		m_bEmpiresSquadLeaderKnownObserved ||
		(explicitLeaderKnown && explicitLeader != m_bEmpiresSquadLeaderObserved);

	if (squad != m_iEmpiresSquadObserved ||
		position != m_iEmpiresSquadPositionObserved || explicitLeaderChanged)
	{
		const bool positionIsLeader = position == 0;
		const bool positionChanged =
			(m_iEmpiresSquadPositionObserved == 0) != positionIsLeader;
		char detail[192] = {};
		std::snprintf(detail, sizeof(detail),
			"source=public-sendprop;position=%d;position_is_leader=%d;explicit_leader_known=%d;explicit_leader=%d;encoding_match=%d",
			position, positionIsLeader ? 1 : 0,
			explicitLeaderKnown ? 1 : 0, explicitLeader ? 1 : 0,
			explicitLeaderKnown && positionIsLeader == explicitLeader ? 1 : 0);
		if (m_iEmpiresSquadObserved <= 0 && squad > 0)
			EmpiresAnalysis::RecordBotEvent(this, "SQUAD_JOIN", nullptr,
				"source=public-sendprop");
		else if (m_iEmpiresSquadObserved > 0 && squad <= 0)
			EmpiresAnalysis::RecordBotEvent(this, "SQUAD_LEAVE", nullptr,
				"source=public-sendprop");
		if (m_iEmpiresSquadObserved > 0 && squad > 0 &&
			(positionChanged || (m_bEmpiresSquadLeaderKnownObserved &&
				explicitLeaderKnown && explicitLeaderChanged)))
			EmpiresAnalysis::RecordBotEvent(this, "SQUAD_LEADER_CHANGED", nullptr,
				detail);
		if (squad > 0)
			EmpiresAnalysis::RecordBotEvent(this,
				"SQUAD_LEADER_ENCODING_OBSERVED", nullptr, detail);
		logger->Log(LogLevel::INFO,
			"Empires gameplay squad=state userid=%d team=%d squad=%d position=%d position_is_leader=%d explicit_leader_known=%d explicit_leader=%d encoding_match=%d source=public-sendprop",
			engine->GetPlayerUserId(m_pEdict), team, squad, position,
			positionIsLeader ? 1 : 0, explicitLeaderKnown ? 1 : 0,
			explicitLeader ? 1 : 0,
			explicitLeaderKnown && positionIsLeader == explicitLeader ? 1 : 0);
		m_iEmpiresSquadObserved = squad;
		m_iEmpiresSquadPositionObserved = position;
		m_bEmpiresSquadLeaderObserved = explicitLeader;
		m_bEmpiresSquadLeaderKnownObserved = explicitLeaderKnown;
	}

	if (squad > 0 || engine->Time() < m_flEmpiresNextSquadActionTime ||
		m_iEmpiresSquadJoinAttempts >= 3)
	{
		return;
	}

	int unassignedBotCount = 0;
	int lowestUnassignedBotUserId = 0;
	std::vector<RCBotEmpiresSquadJoin::SquadObservation> squads;
	const int currentUserId = engine->GetPlayerUserId(m_pEdict);

	for (int index = 1; index <= gpGlobals->maxClients; ++index)
	{
		edict_t* pPlayer = engine->PEntityOfEntIndex(index);
		IPlayerInfo* pInfo = pPlayer != nullptr ?
			playerinfomanager->GetPlayerInfo(pPlayer) : nullptr;
		if (pPlayer == nullptr || pPlayer->IsFree() || pInfo == nullptr ||
			!pInfo->IsConnected() || pInfo->IsObserver() ||
			pInfo->GetTeamIndex() != team)
		{
			continue;
		}

		int teammateSquad = 0;
		const bool teammateSquadKnown = readEmpiresEntityIntProperty(
			pPlayer, "m_iSquad", teammateSquad);
		if (!teammateSquadKnown)
			continue;

		if (teammateSquad > 0)
		{
			auto found = std::find_if(squads.begin(), squads.end(),
				[teammateSquad](const RCBotEmpiresSquadJoin::SquadObservation& item)
				{ return item.id == teammateSquad; });
			if (found == squads.end())
			{
				squads.push_back({ teammateSquad, 1, !pInfo->IsFakeClient() });
			}
			else
			{
				++found->members;
				if (!pInfo->IsFakeClient())
					found->humanOwned = true;
			}
		}

		if (!pInfo->IsFakeClient())
			continue;

		CBot* pOtherBot = CBots::getBotPointer(pPlayer);
		if (pOtherBot == nullptr || pOtherBot->isControlledTestOnly())
			continue;
		if (teammateSquad > 0)
			continue;

		if (pInfo->IsDead() || pInfo->GetHealth() <= 0)
			continue;

		++unassignedBotCount;
		const int userId = engine->GetPlayerUserId(pPlayer);
		if (userId > 0 && (lowestUnassignedBotUserId == 0 ||
			userId < lowestUnassignedBotUserId))
		{
			lowestUnassignedBotUserId = userId;
		}
	}

	RCBotEmpiresSquadJoin::Input input;
	input.ownSquad = squad;
	input.attempts = m_iEmpiresSquadJoinAttempts;
	input.unassignedBots = unassignedBotCount;
	input.lowestUnassignedUserId = lowestUnassignedBotUserId;
	input.currentUserId = currentUserId;
	input.squads = std::move(squads);
	const RCBotEmpiresSquadJoin::Result candidate =
		RCBotEmpiresSquadJoin::Select(input);
	if (candidate.status != RCBotEmpiresSquadJoin::Status::JOIN)
		return;
	const int selectedSquad = candidate.squadId;
	if (selectedSquad <= 0)
	{
		m_flEmpiresNextSquadActionTime = engine->Time() + 10.0f;
		return;
	}

	char command[64] = {};
	std::snprintf(command, sizeof(command), "emp_squad_join %d\n", selectedSquad);
	helpers->ClientCommand(m_pEdict, command);
	++m_iEmpiresSquadJoinAttempts;
	m_flEmpiresNextSquadActionTime = engine->Time() + 5.0f;
	logger->Log(LogLevel::INFO,
		"Empires gameplay squad=join-requested userid=%d team=%d squad_id=%d attempts=%d command=%s reason=%s",
		currentUserId, team, selectedSquad, m_iEmpiresSquadJoinAttempts, command,
		candidate.squadId <= 1 ? "first-available-squad" : "first-free-or-next-sequential");
}

void CBotEmpires::clearEmpiresStartAnchorRecovery()
{
	m_empiresStartAnchorRecovery.reset();
	m_iEmpiresStartAnchorRecoveryWaypoint = -1;
	m_vEmpiresStartAnchorRecoveryStartOrigin = vec3_origin;
	m_flEmpiresStartAnchorRecoveryStartTime = 0.0f;
}

void CBotEmpires::resetEmpiresSquadGoalState()
{
	m_bEmpiresSquadGoalTaskIssued = false;
	m_bEmpiresSquadGoalReached = false;
	m_iEmpiresSquadGoalGeneration = -1;
	m_iEmpiresSquadGoalWaypoint = -1;
	m_iEmpiresSquadGoalLeaderUserId = -1;
	m_iEmpiresSquadGoalRecoveryAttempts = 0;
	m_vEmpiresSquadGoalOrigin = vec3_origin;
	m_vEmpiresSquadGoalTarget = vec3_origin;
}

int CBotEmpires::recoveryEpisodeBotId() const
{
	if (engine == nullptr || m_pEdict == nullptr)
		return -1;
	const int userId = engine->GetPlayerUserId(m_pEdict);
	return userId > 0 ? userId : engine->IndexOfEdict(m_pEdict);
}

void CBotEmpires::ensureEmpiresRecoveryEpisode(
	const RCBotRecovery::Domain domain, const char* subjectId,
	const std::uint64_t domainGeneration, const char* reason)
{
	if (subjectId == nullptr || *subjectId == '\0' ||
		domain == RCBotRecovery::Domain::UNKNOWN)
		return;

	if (!getRecoveryEpisodeIdentity().IsActive() &&
		m_empireRecoveryEpisodeTracker.HasActive())
	{
		// A base lifecycle reset already emitted the terminal observation. Drop
		// only the stale local tracker value before opening the new life/domain.
		m_empireRecoveryEpisodeTracker.End();
	}

	const RCBotRecovery::RecoveryEpisodeIdentity& previous =
		m_empireRecoveryEpisodeTracker.Active();
	const int botId = recoveryEpisodeBotId();
	const std::uint64_t lifeGeneration = getRecoveryLifeGeneration();
	const bool sameGoal = previous.IsActive() &&
		previous.botId == botId &&
		previous.lifeGeneration == lifeGeneration &&
		previous.domain == domain &&
		previous.subjectId == subjectId &&
		previous.domainGeneration == domainGeneration;
	if (!sameGoal && previous.IsActive())
	{
		EmpiresAnalysis::RecordRecoveryEpisodeReplacement(this, previous,
			reason != nullptr ? reason : "practical-goal-replaced");
		EmpiresAnalysis::RecordRecoveryEpisodeEnd(this, previous,
			"GOAL_REPLACED");
		m_empireRecoveryEpisodeTracker.End();
		clearRecoveryEpisodeIdentity();
	}

	bool reused = false;
	const RCBotRecovery::RecoveryEpisodeIdentity identity =
		m_empireRecoveryEpisodeTracker.Start(botId, lifeGeneration, domain,
			subjectId, domainGeneration, &reused);
	setRecoveryEpisodeIdentity(identity);
	m_empireRecoveryEpisodePolicy.ObserveEpisode(identity);
	if (!sameGoal && domain == RCBotRecovery::Domain::CAPTURE)
		m_iEmpiresRecoveryCaptureProgress = -1;
	if (reused)
		EmpiresAnalysis::RecordRecoveryEpisodeReuse(this, identity,
			reason != nullptr ? reason : "same-practical-goal");
	else
		EmpiresAnalysis::RecordRecoveryEpisodeBegin(this, identity,
			reason != nullptr ? reason : "practical-goal-selected");
}

void CBotEmpires::reuseEmpiresRecoveryEpisode(const char* reason)
{
	if (getRecoveryEpisodeIdentity().IsActive())
		EmpiresAnalysis::RecordRecoveryEpisodeReuse(this,
			getRecoveryEpisodeIdentity(),
			reason != nullptr ? reason : "same-practical-goal");
}

void CBotEmpires::endEmpiresRecoveryEpisode(const char* reason)
{
	const RCBotRecovery::RecoveryEpisodeIdentity& identity =
		getRecoveryEpisodeIdentity();
	if (identity.IsActive())
	{
		if (reason != nullptr && std::strcmp(reason, "SUCCESS") == 0)
			EmpiresAnalysis::RecordRecoveryPolicyEvent(this,
				"RECOVERY_SUCCESS", identity,
				m_empireRecoveryEpisodePolicy.ConsecutiveExhaustions(),
				"domain-success");
		EmpiresAnalysis::RecordRecoveryEpisodeEnd(this, identity,
			reason != nullptr ? reason : "UNKNOWN");
	}
	m_empireRecoveryEpisodeTracker.End();
	m_empireRecoveryEpisodePolicy.Reset();
	clearRecoveryEpisodeIdentity();
	m_iEmpiresRecoveryCapturePoint = -1;
	m_iEmpiresRecoveryCaptureProgress = -1;
	m_iEmpiresRecoverySquadTeam = -1;
	m_iEmpiresRecoverySquad = -1;
	m_iEmpiresRecoverySquadGeneration = -1;
	m_iEmpiresRecoverySquadWaypoint = -1;
}

void CBotEmpires::observeEmpiresRecoveryDomainExhausted(const char* reason)
{
	const RCBotRecovery::RecoveryEpisodeIdentity& identity =
		getRecoveryEpisodeIdentity();
	if (!identity.IsActive())
		return;

	const RCBotRecovery::DomainExhaustionResult result =
		m_empireRecoveryEpisodePolicy.RecordDomainExhausted(identity,
			engine != nullptr ? static_cast<double>(engine->Time()) : 0.0);
	EmpiresAnalysis::RecordRecoveryPolicyEvent(this,
		"RECOVERY_DOMAIN_EXHAUSTED", identity,
		result.consecutiveExhaustions,
		reason != nullptr ? reason : "domain-recovery-exhausted");
	if (!result.suppressionStarted)
		return;

	if (identity.domain == RCBotRecovery::Domain::SQUAD)
	{
		// Squad remains shadow-only in v1.  This event is the complete
		// diagnostic decision; no shared order, goal or route is changed.
		EmpiresAnalysis::RecordRecoveryPolicyEvent(this,
			"SQUAD_RECOVERY_WOULD_SUPPRESS", identity,
			result.consecutiveExhaustions, "shadow-only");
	}
	else if (identity.domain == RCBotRecovery::Domain::CAPTURE)
	{
		EmpiresAnalysis::RecordRecoveryPolicyEvent(this,
			"RECOVERY_SUPPRESSION_STARTED", identity,
			result.consecutiveExhaustions,
			eta_recovery_active.GetBool() ? "capture-active" :
				"capture-active-disabled");
	}
}

void CBotEmpires::observeEmpiresRecoveryProgress(const char* reason)
{
	const RCBotRecovery::RecoveryEpisodeIdentity& identity =
		getRecoveryEpisodeIdentity();
	if (!identity.IsActive() ||
		!m_empireRecoveryEpisodePolicy.RecordMeaningfulProgress(identity))
		return;

	EmpiresAnalysis::RecordRecoveryPolicyEvent(this,
		"RECOVERY_PROGRESS_RESET", identity, 0,
		reason != nullptr ? reason : "meaningful-domain-progress");
}

bool CBotEmpires::isEmpiresCaptureCandidateSuppressed(const int pointIndex)
{
	if (!eta_recovery_active.GetBool() || engine == nullptr || pointIndex < 0)
		return false;
	const RCBotRecovery::RecoveryEpisodeIdentity& identity =
		getRecoveryEpisodeIdentity();
	if (!identity.IsActive() || identity.domain != RCBotRecovery::Domain::CAPTURE ||
		identity.subjectId != std::to_string(pointIndex))
		return false;

	const RCBotRecovery::SuppressionCheck check =
		m_empireRecoveryEpisodePolicy.CheckSuppression(identity,
			static_cast<double>(engine->Time()));
	if (check == RCBotRecovery::SuppressionCheck::EXPIRED)
	{
		EmpiresAnalysis::RecordRecoveryPolicyEvent(this,
			"RECOVERY_SUPPRESSION_EXPIRED", identity,
			m_empireRecoveryEpisodePolicy.ConsecutiveExhaustions(),
			"suppression-window-expired");
		return false;
	}
	if (check != RCBotRecovery::SuppressionCheck::SUPPRESSED)
		return false;

	if (m_empireRecoveryEpisodePolicy.MarkSuppressionApplied(identity))
	{
		EmpiresAnalysis::RecordRecoveryPolicyEvent(this,
			"RECOVERY_SUPPRESSION_APPLIED", identity,
			m_empireRecoveryEpisodePolicy.ConsecutiveExhaustions(),
			"capture-candidate-skipped");
	}
	return true;
}

bool CBotEmpires::hasActiveEmpiresSquadRecoveryGoal() const
{
	const RCBotRecovery::RecoveryEpisodeIdentity& identity =
		getRecoveryEpisodeIdentity();
	if (!identity.IsActive() ||
		identity.domain != RCBotRecovery::Domain::SQUAD)
		return false;

	const EmpiresSquadGoalState* pGoal = empSquadGoalState(
		m_iEmpiresRecoverySquadTeam, m_iEmpiresRecoverySquad);
	return pGoal != nullptr && pGoal->active &&
		pGoal->generation == m_iEmpiresRecoverySquadGeneration &&
		pGoal->waypoint == m_iEmpiresRecoverySquadWaypoint;
}

void CBotEmpires::observeEmpiresRecoveryEpisode()
{
	const int team = getTeam();
	if (m_iEmpiresRecoveryLastTeam >= 0 && team != m_iEmpiresRecoveryLastTeam &&
		getRecoveryEpisodeIdentity().IsActive())
	{
		endEmpiresRecoveryEpisode("TEAM_CHANGE");
	}
	m_iEmpiresRecoveryLastTeam = team;

	const RCBotRecovery::RecoveryEpisodeIdentity& identity =
		getRecoveryEpisodeIdentity();
	if (!identity.IsActive() || identity.domain != RCBotRecovery::Domain::CAPTURE ||
		m_iEmpiresRecoveryCapturePoint < 0)
		return;

	CBotMod* currentMod = CBotGlobals::getCurrentMod();
	if (currentMod == nullptr || currentMod->getBotType() != BOTTYPE_EMPIRES)
		return;
	const CEmpiresMod* empiresMod = static_cast<const CEmpiresMod*>(currentMod);
	for (const CEmpiresMod::CapturePointObservation& point :
		empiresMod->getCapturePoints())
	{
		if (!point.pointIndexKnown ||
			point.pointIndex != m_iEmpiresRecoveryCapturePoint)
			continue;
		if (point.progressKnown)
		{
			if (m_iEmpiresRecoveryCaptureProgress >= 0 &&
				point.progress > m_iEmpiresRecoveryCaptureProgress)
			{
				observeEmpiresRecoveryProgress("capture-objective-progress");
			}
			m_iEmpiresRecoveryCaptureProgress = point.progress;
		}
		if (point.teamOwnerKnown && point.teamOwner == team)
		{
			const std::string detail = "point=" +
				std::to_string(m_iEmpiresRecoveryCapturePoint) + ";" +
				getEmpiresCombatCorrelation();
			EmpiresAnalysis::RecordBotEvent(this, "CAPTURE_COMPLETED",
				nullptr, detail.c_str());
			endEmpiresRecoveryEpisode("SUCCESS");
		}
		else if ((team == EMP_TEAM_NF ? point.nfCappableKnown :
			point.beCappableKnown) &&
			((team == EMP_TEAM_NF && point.nfCappable == 0) ||
				(team == EMP_TEAM_BE && point.beCappable == 0)))
		{
			const std::string detail = "point=" +
				std::to_string(m_iEmpiresRecoveryCapturePoint) + ";" +
				getEmpiresCombatCorrelation();
			EmpiresAnalysis::RecordBotEvent(this, "OBJECTIVE_INVALIDATED",
				nullptr, detail.c_str());
			endEmpiresRecoveryEpisode("DOMAIN_INVALID");
		}
		break;
	}
}

int CBotEmpires::countEmpiresSquadBots(const int team, const int squad) const
{
	if ((team != EMP_TEAM_NF && team != EMP_TEAM_BE) || squad <= 0 ||
		squad >= 64 || engine == nullptr || playerinfomanager == nullptr ||
		gpGlobals == nullptr)
	{
		return 0;
	}

	int count = 0;
	for (int index = 1; index <= gpGlobals->maxClients; ++index)
	{
		edict_t* pPlayer = engine->PEntityOfEntIndex(index);
		IPlayerInfo* pInfo = pPlayer != nullptr ?
			playerinfomanager->GetPlayerInfo(pPlayer) : nullptr;
		if (pPlayer == nullptr || pPlayer->IsFree() || pInfo == nullptr ||
			!pInfo->IsConnected() || pInfo->IsObserver() ||
			pInfo->GetTeamIndex() != team)
		{
			continue;
		}

		int observedSquad = -1;
		if (!readEmpiresEntityIntProperty(pPlayer, "m_iSquad", observedSquad) ||
			observedSquad != squad)
		{
			continue;
		}

		// A human-owned squad is outside the autonomous coordination boundary.
		// Do not turn an observed human squad into a bot formation target.
		if (!pInfo->IsFakeClient())
			return -1;

		CBot* pOtherBot = CBots::getBotPointer(pPlayer);
		if (pOtherBot == nullptr || pOtherBot->isControlledTestOnly() ||
			pInfo->IsDead() || pInfo->GetHealth() <= 0)
		{
			continue;
		}
		++count;
	}

	return count;
}

int CBotEmpires::findEmpiresSquadLeader(const int team, const int squad) const
{
	if ((team != EMP_TEAM_NF && team != EMP_TEAM_BE) || squad <= 0 ||
		squad >= 64 || engine == nullptr || playerinfomanager == nullptr ||
		gpGlobals == nullptr)
	{
		return -1;
	}

	int leaderUserId = -1;
	int leaderKey = 0;
	bool leaderPositionObserved = false;
	bool humanOwnsSquad = false;
	for (int index = 1; index <= gpGlobals->maxClients; ++index)
	{
		edict_t* pPlayer = engine->PEntityOfEntIndex(index);
		IPlayerInfo* pInfo = pPlayer != nullptr ?
			playerinfomanager->GetPlayerInfo(pPlayer) : nullptr;
		if (pPlayer == nullptr || pPlayer->IsFree() || pInfo == nullptr ||
			!pInfo->IsConnected() || pInfo->IsObserver() ||
			pInfo->GetTeamIndex() != team)
		{
			continue;
		}

		int observedSquad = -1;
		if (!readEmpiresEntityIntProperty(pPlayer, "m_iSquad", observedSquad) ||
			observedSquad != squad)
		{
			continue;
		}
		if (!pInfo->IsFakeClient())
		{
			humanOwnsSquad = true;
			continue;
		}

		CBot* pOtherBot = CBots::getBotPointer(pPlayer);
		if (pOtherBot == nullptr || pOtherBot->isControlledTestOnly() ||
			pInfo->IsDead() || pInfo->GetHealth() <= 0)
		{
			continue;
		}

		int observedPosition = -1;
		(void)readEmpiresEntityIntProperty(pPlayer, "m_iSquadPosition",
			observedPosition);
		const bool candidateIsLeaderPosition = observedPosition == 0;
		const int userId = engine->GetPlayerUserId(pPlayer);
		const int candidateKey = userId > 0 ? userId : index;
		if (leaderUserId < 0 ||
			(candidateIsLeaderPosition && !leaderPositionObserved) ||
			(candidateIsLeaderPosition == leaderPositionObserved &&
				candidateKey < leaderKey))
		{
			leaderUserId = userId > 0 ? userId : -index;
			leaderKey = candidateKey;
			leaderPositionObserved = candidateIsLeaderPosition;
		}
	}

	return humanOwnsSquad ? -1 : leaderUserId;
}

bool CBotEmpires::handleEmpiresSquadGoal()
{
	if (m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pSchedules == nullptr || engine == nullptr ||
		playerinfomanager == nullptr || gpGlobals == nullptr ||
		!m_pPlayerInfo->IsConnected() || !m_pPlayerInfo->IsFakeClient() ||
		m_pPlayerInfo->IsObserver() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0 || !rcbot_bots_form_squads.GetBool())
	{
		return false;
	}

	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	int squad = -1;
	int position = -1;
	if (!readEmpiresEntityIntProperty(m_pEdict, "m_iSquad", squad) ||
		!readEmpiresEntityIntProperty(m_pEdict, "m_iSquadPosition", position) ||
		squad <= 0)
	{
		return false;
	}

	EmpiresSquadGoalState* pGoal = empSquadGoalState(team, squad);
	if (pGoal == nullptr)
		return false;

	const int currentUserId = engine->GetPlayerUserId(m_pEdict);
	if (currentUserId <= 0)
		return false;

	const int electedLeaderUserId = findEmpiresSquadLeader(team, squad);
	if (pGoal->active && electedLeaderUserId < 0)
	{
		if (getRecoveryEpisodeIdentity().IsActive() &&
			getRecoveryEpisodeIdentity().domain == RCBotRecovery::Domain::SQUAD)
			endEmpiresRecoveryEpisode("DOMAIN_INVALID");
		logger->Log(LogLevel::INFO,
			"Empires gameplay squad-goal=cleared reason=human-owner-or-no-live-bot-leader team=%d squad=%d generation=%d",
			team, squad, pGoal->generation);
		pGoal->active = false;
	}

	if (pGoal->active && electedLeaderUserId > 0 &&
		pGoal->leaderUserId != electedLeaderUserId)
	{
		const int previousLeaderUserId = pGoal->leaderUserId;
		pGoal->leaderUserId = electedLeaderUserId;
		++pGoal->generation;
		EmpiresAnalysis::RecordBotEvent(this, "SQUAD_GOAL_RECOVERY", nullptr,
			"reason=leader-changed;new-leader-from-public-squad-state");
		logger->Log(LogLevel::INFO,
			"Empires gameplay squad-goal=recovery reason=leader-changed team=%d squad=%d previous_leader_userid=%d new_leader_userid=%d generation=%d source=public-sendprop",
			team, squad, previousLeaderUserId, electedLeaderUserId,
			pGoal->generation);
	}

	const bool currentScheduleIsGoto =
		m_pSchedules->isCurrentSchedule(SCHED_GOTO_ORIGIN);
	if (!pGoal->active)
	{
		if (getRecoveryEpisodeIdentity().IsActive() &&
			getRecoveryEpisodeIdentity().domain == RCBotRecovery::Domain::SQUAD)
			endEmpiresRecoveryEpisode("DOMAIN_INVALID");
		if (m_bEmpiresSquadGoalTaskIssued)
		{
			if (currentScheduleIsGoto)
			{
				m_pSchedules->freeMemory();
				if (m_pNavigator != nullptr)
					m_pNavigator->clear();
			}
			resetEmpiresSquadGoalState();
		}

		if (electedLeaderUserId <= 0 ||
			countEmpiresSquadBots(team, squad) < 2 ||
			currentUserId != electedLeaderUserId)
		{
			return false;
		}

		if (!m_pSchedules->isEmpty() && !currentScheduleIsGoto)
			return false;

		CWaypoint* pGoalWaypoint = CWaypoints::randomWaypointGoal(-1);
		if (pGoalWaypoint == nullptr)
			return false;

		pGoal->active = true;
		pGoal->team = team;
		pGoal->squad = squad;
		pGoal->leaderUserId = electedLeaderUserId;
		pGoal->waypoint = CWaypoints::getWaypointIndex(pGoalWaypoint);
		pGoal->origin = pGoalWaypoint->getOrigin();
		++pGoal->generation;
		if (pGoal->generation <= 0)
			pGoal->generation = 1;
		logger->Log(LogLevel::INFO,
			"Empires gameplay squad-goal=created team=%d squad=%d leader_userid=%d generation=%d waypoint=%d shared=(%.1f %.1f %.1f) members=%d source=public-sendprop",
			team, squad, pGoal->leaderUserId, pGoal->generation,
			pGoal->waypoint, pGoal->origin.x, pGoal->origin.y,
			pGoal->origin.z, countEmpiresSquadBots(team, squad));
	}

	if (!pGoal->active)
		return false;

	const bool localGoalGenerationMatches = m_bEmpiresSquadGoalTaskIssued &&
		m_iEmpiresSquadGoalGeneration == pGoal->generation;
	if (localGoalGenerationMatches && m_bEmpiresSquadGoalReached)
		return true;

	if (m_bEmpiresSquadGoalTaskIssued && !localGoalGenerationMatches)
	{
		if (!m_pSchedules->isEmpty() && !currentScheduleIsGoto)
		{
			resetEmpiresSquadGoalState();
			return false;
		}
		if (!m_pSchedules->isEmpty())
			m_pSchedules->freeMemory();
		if (m_pNavigator != nullptr)
			m_pNavigator->clear();
		resetEmpiresSquadGoalState();
	}

	if (m_bEmpiresSquadGoalTaskIssued &&
		m_iEmpiresSquadGoalGeneration == pGoal->generation)
	{
		if (!m_pSchedules->isEmpty() && !currentScheduleIsGoto)
		{
			resetEmpiresSquadGoalState();
			return false;
		}
		if (!m_pSchedules->isEmpty())
			return true;

		const float targetDistance = distanceFrom(m_vEmpiresSquadGoalTarget);
		if (targetDistance <= 96.0f)
		{
			if (currentUserId == pGoal->leaderUserId)
			{
					EmpiresAnalysis::RecordBotEvent(this, "SQUAD_GOAL_REACHED",
						nullptr, "role=leader;shared-goal-complete");
					endEmpiresRecoveryEpisode("SUCCESS");
				logger->Log(LogLevel::INFO,
					"Empires gameplay squad-goal=reached role=leader owner_userid=%d team=%d squad=%d generation=%d waypoint=%d distance=%.1f",
					currentUserId, team, squad, pGoal->generation,
					pGoal->waypoint, targetDistance);
				pGoal->active = false;
				resetEmpiresSquadGoalState();
				return false;
			}

				m_bEmpiresSquadGoalReached = true;
				EmpiresAnalysis::RecordBotEvent(this, "SQUAD_GOAL_REACHED", nullptr,
					"role=follower;holding-shared-goal");
				endEmpiresRecoveryEpisode("SUCCESS");
			logger->Log(LogLevel::INFO,
				"Empires gameplay squad-goal=reached role=follower owner_userid=%d team=%d squad=%d leader_userid=%d generation=%d distance=%.1f holding=1",
				currentUserId, team, squad, pGoal->leaderUserId,
				pGoal->generation, targetDistance);
			return true;
		}

		if (m_iEmpiresSquadGoalRecoveryAttempts >= 2)
		{
			// The practical shared goal remains authoritative while its local
			// schedule is being rebuilt. Establish the passive identity before
			// observing the bounded failure so this event and the immediately
			// repeated SQUAD_GOAL_SHARED event share one episode.
			const std::string squadSubject = "team=" + std::to_string(team) +
				";squad=" + std::to_string(squad) + ";waypoint=" +
				std::to_string(pGoal->waypoint);
			m_iEmpiresRecoverySquadTeam = team;
			m_iEmpiresRecoverySquad = squad;
			m_iEmpiresRecoverySquadGeneration = pGoal->generation;
			m_iEmpiresRecoverySquadWaypoint = pGoal->waypoint;
			ensureEmpiresRecoveryEpisode(RCBotRecovery::Domain::SQUAD,
				squadSubject.c_str(),
				static_cast<std::uint64_t>(pGoal->generation),
				"squad-goal-recovery-failed");
			EmpiresAnalysis::RecordBotEvent(this, "SQUAD_GOAL_RECOVERY", nullptr,
				"reason=route-empty-too-far;bounded-retries-exhausted");
			EmpiresAnalysis::CorrelateNavigationSignal(this,
				RCBotNavigationExperience::Signal::SQUAD_RECOVERY_FAILED);
			observeEmpiresRecoveryDomainExhausted(
				"squad-recovery-exhausted");
			logger->Log(LogLevel::WARN,
				"Empires gameplay squad-goal=recovery-failed owner_userid=%d team=%d squad=%d generation=%d waypoint=%d distance=%.1f retries=%d",
				currentUserId, team, squad, pGoal->generation,
				pGoal->waypoint, targetDistance,
				m_iEmpiresSquadGoalRecoveryAttempts);
			resetEmpiresSquadGoalState();
			return false;
		}

		++m_iEmpiresSquadGoalRecoveryAttempts;
		EmpiresAnalysis::RecordBotEvent(this, "SQUAD_GOAL_RECOVERY", nullptr,
			"reason=route-empty-too-far;retrying-existing-shared-goal");
		logger->Log(LogLevel::INFO,
			"Empires gameplay squad-goal=recovery owner_userid=%d team=%d squad=%d generation=%d waypoint=%d distance=%.1f retry=%d route=existing-CFindPathTask+CMoveToTask",
			currentUserId, team, squad, pGoal->generation, pGoal->waypoint,
			targetDistance, m_iEmpiresSquadGoalRecoveryAttempts);
	}

	const bool canReplaceCurrentSchedule = m_pSchedules->isEmpty() ||
		m_pSchedules->isCurrentSchedule(SCHED_GOTO_ORIGIN);
	if (!canReplaceCurrentSchedule)
		return false;
	if (!m_pSchedules->isEmpty())
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		m_pNavigator->clear();

	const Vector offset = position == 1 ? Vector(96.0f, 0.0f, 0.0f) :
		position == 2 ? Vector(-96.0f, 0.0f, 0.0f) :
		position == 3 ? Vector(0.0f, 96.0f, 0.0f) :
		position == 4 ? Vector(0.0f, -96.0f, 0.0f) : vec3_origin;
	const Vector target = pGoal->origin + offset;
	CBotSchedule* pSchedule = new CBotSchedule();
	pSchedule->addTask(new CFindPathTask(pGoal->waypoint, LOOK_WAYPOINT));
	pSchedule->addTask(new CMoveToTask(target));
	pSchedule->addTask(new CBotWaitTask(8.0f));
	pSchedule->setID(SCHED_GOTO_ORIGIN);
	m_pSchedules->addFront(pSchedule);

	m_bEmpiresSquadGoalTaskIssued = true;
	m_bEmpiresSquadGoalReached = false;
	m_iEmpiresSquadGoalGeneration = pGoal->generation;
	m_iEmpiresSquadGoalWaypoint = pGoal->waypoint;
	m_iEmpiresSquadGoalLeaderUserId = pGoal->leaderUserId;
	m_vEmpiresSquadGoalOrigin = pGoal->origin;
	m_vEmpiresSquadGoalTarget = target;
	m_iEmpiresRecoverySquadTeam = team;
	m_iEmpiresRecoverySquad = squad;
	m_iEmpiresRecoverySquadGeneration = pGoal->generation;
	m_iEmpiresRecoverySquadWaypoint = pGoal->waypoint;
	const std::string squadSubject = "team=" + std::to_string(team) +
		";squad=" + std::to_string(squad) + ";waypoint=" +
		std::to_string(pGoal->waypoint);
	ensureEmpiresRecoveryEpisode(RCBotRecovery::Domain::SQUAD,
		squadSubject.c_str(), static_cast<std::uint64_t>(pGoal->generation),
		"squad-goal-shared");
	const char* role = currentUserId == pGoal->leaderUserId ? "leader" : "follower";
	char detail[512] = {};
	std::snprintf(detail, sizeof(detail),
		"role=%s;leader_userid=%d;squad=%d;generation=%d;waypoint=%d;shared-goal;spacing=96",
		role, pGoal->leaderUserId, squad, pGoal->generation, pGoal->waypoint);
	EmpiresAnalysis::RecordBotEvent(this, "SQUAD_GOAL_SHARED", nullptr, detail);
	logger->Log(LogLevel::INFO,
		"Empires gameplay squad-goal=started role=%s owner_userid=%d team=%d squad=%d leader_userid=%d generation=%d waypoint=%d shared=(%.1f %.1f %.1f) offset=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f) spacing=96 route=existing-CFindPathTask+CMoveToTask+CBotWaitTask",
		role, currentUserId, team, squad, pGoal->leaderUserId,
		pGoal->generation, pGoal->waypoint, pGoal->origin.x,
		pGoal->origin.y, pGoal->origin.z, offset.x, offset.y, offset.z,
		target.x, target.y, target.z);
	return true;
}

bool CBotEmpires::requestEmpiresScoutBinocularTarget(edict_t* pTargetEdict,
	const bool targetBuilding)
{
	if (isEmpiresScoutBinocularActionActive() ||
		m_bBootstrapOnly || m_bControllerProbeOnly || m_pEdict == nullptr ||
		m_pEdict->IsFree() || pTargetEdict == nullptr || pTargetEdict->IsFree() ||
		m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr || m_pWeapons == nullptr || engine == nullptr ||
		playerinfomanager == nullptr || gpGlobals == nullptr ||
		!m_pPlayerInfo->IsConnected() || m_pPlayerInfo->IsObserver() ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0)
		return false;

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired,
		classDesiredKnown, classPropertyName) || classId != EMP_CLASS_SCOUT)
		return false;
	(void)classDesired;
	(void)classDesiredKnown;
	(void)classPropertyName;

	IPlayerInfo* pTargetInfo = !targetBuilding ?
		playerinfomanager->GetPlayerInfo(pTargetEdict) : nullptr;
	const int team = m_pPlayerInfo->GetTeamIndex();
	int targetTeam = -1;
	const bool targetTeamKnown = readEmpiresEntityIntProperty(
		pTargetEdict, "m_iTeamNum", targetTeam);
	const char* targetClassName = pTargetEdict->GetClassName();
	const bool validBuilding = targetBuilding && targetClassName != nullptr &&
		std::strncmp(targetClassName, "emp_building_", 13) == 0 &&
		targetTeamKnown && (targetTeam == EMP_TEAM_NF || targetTeam == EMP_TEAM_BE) &&
		targetTeam != team;
	const bool validPlayer = !targetBuilding && pTargetInfo != nullptr &&
		pTargetInfo->IsConnected() && !pTargetInfo->IsObserver() &&
		!pTargetInfo->IsDead() && pTargetInfo->GetHealth() > 0 &&
		pTargetEdict != m_pEdict && pTargetInfo->GetTeamIndex() != team &&
		(pTargetInfo->GetTeamIndex() == EMP_TEAM_NF ||
		 pTargetInfo->GetTeamIndex() == EMP_TEAM_BE);
	if ((!validBuilding && !validPlayer) ||
		(team != EMP_TEAM_NF && team != EMP_TEAM_BE))
		return false;

	const char* binocularClassName = empiresBinocularClassName(team);
	if (binocularClassName == nullptr)
		return false;

	CBaseHandle* pWeaponList = CClassInterface::getWeaponList(m_pEdict);
	if (pWeaponList == nullptr)
		return false;
	edict_t* pBinoculars = nullptr;
	for (int slot = 0; slot < MAX_WEAPONS; ++slot)
	{
		const int weaponIndex = pWeaponList[slot].GetEntryIndex();
		if (weaponIndex <= 0)
			continue;
		edict_t* pWeapon = engine->PEntityOfEntIndex(weaponIndex);
		if (pWeapon == nullptr || pWeapon->IsFree() ||
			pWeapon->GetClassName() == nullptr ||
			std::strcmp(pWeapon->GetClassName(), binocularClassName) != 0 ||
			CClassInterface::getWeaponState(pWeapon) == WEAPON_NOT_CARRIED)
			continue;
		pBinoculars = pWeapon;
		break;
	}
	if (pBinoculars == nullptr)
		return false;

	m_iEmpiresScoutBinocularActionExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iEmpiresScoutBinocularActionExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pEmpiresScoutBinocularActionExpectedEdict = m_pEdict;
	m_pEmpiresScoutBinocularActionTargetEdict = pTargetEdict;
	m_iEmpiresScoutBinocularActionTargetIndex = engine->IndexOfEdict(pTargetEdict);
	m_iEmpiresScoutBinocularActionTargetUserId = validPlayer ?
		pTargetInfo->GetUserID() : -1;
	m_iEmpiresScoutBinocularActionTargetTeam = validBuilding ? targetTeam :
		pTargetInfo->GetTeamIndex();
	m_bEmpiresScoutBinocularActionTargetBuilding = validBuilding;
	m_pEmpiresScoutBinocularActionWeapon = pBinoculars;
	m_iEmpiresScoutBinocularActionFrames = 0;
	m_flEmpiresScoutBinocularActionDeadline = gpGlobals->curtime + 12.0f;
	m_flEmpiresScoutBinocularActionAimUntil = 0.0f;
	m_bEmpiresScoutBinocularActionAttackLogged = false;
	m_bEmpiresScoutBinocularActionReleaseLogged = false;
	m_bEmpiresScoutBinocularActionPropertyBaselineLogged = false;
	m_bEmpiresScoutBinocularActionSpotTransitionLogged = false;
	m_iEmpiresScoutBinocularActionBaselineSpotter = -1;
	m_bEmpiresScoutBinocularActionBaselineSpotterKnown = false;
	m_bEmpiresScoutBinocularActionBaselineSpotted = false;
	m_bEmpiresScoutBinocularActionBaselineSpottedKnown = false;
	m_iEmpiresScoutBinocularActionBaselineVisibilityState = -1;
	m_bEmpiresScoutBinocularActionBaselineVisibilityKnown = false;
	m_iButtons = 0;
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_pButtons->letGo(IN_ATTACK);
	m_pButtons->letGo(IN_ATTACK2);
	m_pButtons->letGo(IN_USE);
	selectWeaponName(binocularClassName);
	m_eEmpiresScoutBinocularActionState =
		EmpiresScoutBinocularActionState::WAITING_FOR_ACTIVE;

	if (logger != nullptr)
		logger->Log(LogLevel::INFO,
			"Empires scout binocular action=armed index=%d userid=%d team=%d class=%d target_index=%d target_userid=%d target_building=%d target_class=%s target_team=%d weapon=%s weapon_index=%d deadline=%.3f protocol=select-aim-primary-attack",
			m_iEmpiresScoutBinocularActionExpectedIndex,
			m_iEmpiresScoutBinocularActionExpectedUserId, team, classId,
			m_iEmpiresScoutBinocularActionTargetIndex,
			m_iEmpiresScoutBinocularActionTargetUserId,
			targetBuilding ? 1 : 0,
			targetClassName != nullptr ? targetClassName : "<none>",
			m_iEmpiresScoutBinocularActionTargetTeam,
			binocularClassName,
			engine->IndexOfEdict(pBinoculars),
			m_flEmpiresScoutBinocularActionDeadline);
	return true;
}

bool CBotEmpires::requestEmpiresMortarAimOnce(const int targetWaypoint)
{
	if (targetWaypoint < 0 || targetWaypoint >= CWaypoints::MAX_WAYPOINTS)
		return false;

	const int team = m_pPlayerInfo != nullptr ? m_pPlayerInfo->GetTeamIndex() : -1;
	if (team != EMP_TEAM_NF)
		return false;

	constexpr int kMortarAimStartWaypoint = 147;
	CWaypoint* pStart = CWaypoints::getWaypoint(kMortarAimStartWaypoint);
	CWaypoint* pTarget = CWaypoints::getWaypoint(targetWaypoint);
	if (pStart == nullptr || pTarget == nullptr || !pStart->isUsed() ||
		!pTarget->isUsed())
	{
		return false;
	}

	Vector startOrigin = pStart->getOrigin();
	const Vector targetOrigin = pTarget->getOrigin();
	const Vector separation = targetOrigin - startOrigin;
	if (!startOrigin.IsValid() || !targetOrigin.IsValid() ||
		separation.Length2DSqr() < 1.0f)
	{
		return false;
	}

	if (!requestEmpiresMortarFireOnce())
		return false;

	// Fixed actor placement is limited to this debug-only command. The mortar
	// action itself continues through the normal controller/RunPlayerMove path.
	m_pController->SetAbsOrigin(startOrigin);
	QAngle startAngles(0.0f,
		RAD2DEG(std::atan2(separation.y, separation.x)), 0.0f);
	m_pController->SetAbsAngles(startAngles);
	m_bEmpiresMortarAimRequested = true;
	m_iEmpiresMortarAimTargetWaypoint = targetWaypoint;
	m_vEmpiresMortarAimTarget = targetOrigin;
	m_qEmpiresMortarAimAngles = QAngle(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLaunchOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_bEmpiresMortarAimComputed = false;
	m_bEmpiresMortarAimViewApplied = false;
	m_bEmpiresMortarAimProcessedLogged = false;
	m_iEmpiresMortarAimTrajectorySamples = 0;
	m_flEmpiresMortarAimLastSampleTime = 0.0f;
	m_vEmpiresMortarAimFirstShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLastShellOrigin = Vector(0.0f, 0.0f, 0.0f);

	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar aim=armed index=%d userid=%d start_waypoint=%d target_waypoint=%d start=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f) separation=%.1f",
			engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
			kMortarAimStartWaypoint, targetWaypoint, startOrigin.x, startOrigin.y,
			startOrigin.z, targetOrigin.x, targetOrigin.y, targetOrigin.z,
			separation.Length());
	}
	return true;
}

bool CBotEmpires::prepareEmpiresEngineerRepairFixture(const int buildingIndex)
{
	if (servertools == nullptr)
		return false;

	CBaseEntity* pBuilding = nullptr;
	if (buildingIndex >= 0)
		pBuilding = servertools->GetBaseEntityByEntIndex(buildingIndex);
	else
		pBuilding = servertools->FindEntityByClassname(nullptr, "emp_building_nf_barracks");

	if (pBuilding == nullptr)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires engineer repair fixture failed: NF Barracks entity not found index=%d",
				buildingIndex);
		return false;
	}

	if (!servertools->SetKeyValue(pBuilding, "targetname", "rcbot_engineer_fixture_barracks"))
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires engineer repair fixture failed: could not assign Barracks targetname index=%d",
				buildingIndex);
		return false;
	}

	if (logger != nullptr)
		logger->Log(LogLevel::INFO,
			"Empires engineer repair fixture prepared: NF Barracks targetname=rcbot_engineer_fixture_barracks requested_index=%d",
			buildingIndex);
	return true;
}

bool CBotEmpires::damageEmpiresEngineerRepairFixture(const int damage,
	const char* buildingClass)
{
	if (servertools == nullptr || damage <= 0 || buildingClass == nullptr)
		return false;

	edict_t* pBuildingEdict = nullptr;
	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		const char* className = pNetworkable != nullptr ?
			pNetworkable->GetClassName() : nullptr;
		// A number selects the entity index instead of a class name.
		const bool byIndex = buildingClass[0] >= '0' && buildingClass[0] <= '9';
		if (className != nullptr && (byIndex ?
			engine != nullptr && pNetworkable->GetEdict() != nullptr &&
				engine->IndexOfEdict(pNetworkable->GetEdict()) == std::atoi(buildingClass) :
			std::strcmp(className, buildingClass) == 0))
		{
			pBuildingEdict = pNetworkable->GetEdict();
			break;
		}
	}

	if (pBuildingEdict == nullptr)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires engineer repair fixture damage failed: NF Barracks edict unavailable damage=%d",
				damage);
		return false;
	}

	int healthBefore = -1;
	int maxHealth = -1;
	if (!readEmpiresEntityIntProperty(pBuildingEdict, "m_iHealth", healthBefore) ||
		!readEmpiresEntityIntProperty(pBuildingEdict, "m_iMaxHealth", maxHealth))
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires engineer repair fixture damage failed: dynamic health properties unavailable index=%d damage=%d",
				engine != nullptr ? engine->IndexOfEdict(pBuildingEdict) : -1, damage);
		return false;
	}

	if (healthBefore <= 0 || maxHealth <= 1)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires engineer repair fixture damage failed: building_index=%d health=%d max_health=%d damage=%d",
				engine != nullptr ? engine->IndexOfEdict(pBuildingEdict) : -1,
				healthBefore, maxHealth, damage);
		return false;
	}

	const int targetHealth = std::max(1,
		std::min(maxHealth - 1, healthBefore - damage));
	if (!writeEmpiresEntityIntProperty(pBuildingEdict, "m_iHealth", targetHealth))
		return false;
	int healthAfter = -1;
	const bool readback = readEmpiresEntityIntProperty(
		pBuildingEdict, "m_iHealth", healthAfter);

	if (logger != nullptr)
		logger->Log(LogLevel::INFO,
			"Empires engineer repair fixture damage result=dynamic-property-write building_index=%d class=%s health_before=%d target_health=%d health_after=%d max_health=%d damage=%d readback=%d changed=%d",
			engine != nullptr ? engine->IndexOfEdict(pBuildingEdict) : -1,
			buildingClass, healthBefore, targetHealth, healthAfter, maxHealth, damage,
			readback ? 1 : 0, readback && healthAfter < healthBefore ? 1 : 0);
	return readback && healthAfter < healthBefore;
}

bool CBotEmpires::scanEmpiresEngineerRepairVisibility(const int buildingIndex)
{
	if (servertools == nullptr || engine == nullptr || m_pEdict == nullptr ||
		m_pEdict->IsFree() || gpGlobals == nullptr)
	{
		return false;
	}

	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	const char* buildingClass = team == EMP_TEAM_NF ?
		"emp_building_nf_barracks" : "emp_building_imp_barracks";
	CBaseEntity* pBuilding = buildingIndex >= 0 ?
		servertools->GetBaseEntityByEntIndex(buildingIndex) :
		servertools->FindEntityByClassname(nullptr, buildingClass);
	if (pBuilding == nullptr)
		return false;

	IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pBuilding);
	IServerNetworkable* pNetworkable = pUnknown != nullptr ?
		pUnknown->GetNetworkable() : nullptr;
	edict_t* pBuildingEdict = pNetworkable != nullptr ?
		pNetworkable->GetEdict() : nullptr;
	const ICollideable* pCollision = pBuildingEdict != nullptr ?
		pBuildingEdict->GetCollideable() : nullptr;
	if (pBuildingEdict == nullptr || pBuildingEdict->IsFree() ||
		pCollision == nullptr)
	{
		return false;
	}

	const Vector collisionOrigin = pCollision->GetCollisionOrigin();
	const Vector mins = pCollision->OBBMins();
	const Vector maxs = pCollision->OBBMaxs();
	const Vector center = collisionOrigin + (mins + maxs) * 0.5f;
	const Vector midX = (mins + maxs) * 0.5f;
	const Vector targetPoints[] =
	{
		center,
		collisionOrigin + Vector(mins.x, midX.y, midX.z),
		collisionOrigin + Vector(maxs.x, midX.y, midX.z),
		collisionOrigin + Vector(midX.x, mins.y, midX.z),
		collisionOrigin + Vector(midX.x, maxs.y, midX.z),
		collisionOrigin + Vector(midX.x, midX.y, mins.z),
		collisionOrigin + Vector(midX.x, midX.y, maxs.z),
		collisionOrigin + Vector(mins.x, mins.y, midX.z),
		collisionOrigin + Vector(maxs.x, maxs.y, midX.z)
	};
	const char* const targetNames[] =
	{
		"center", "min-x", "max-x", "min-y", "max-y", "bottom", "top",
		"min-corner", "max-corner"
	};

	int usedWaypoints = 0;
	int visibleWaypoints = 0;
	int firstWaypoint = -1;
	int firstTarget = -1;
	Vector firstEye;
	Vector firstPoint;
	for (int waypointId = 0; waypointId < CWaypoints::numWaypoints();
		++waypointId)
	{
		CWaypoint* pWaypoint = CWaypoints::getWaypoint(waypointId);
		if (pWaypoint == nullptr || !pWaypoint->isUsed() ||
			!pWaypoint->forTeam(team))
		{
			continue;
		}

		++usedWaypoints;
		// RCBot2 waypoint origins are feet-adjacent map anchors.  The current
		// Empires player eye is approximately 28 units above this map-space
		// origin after normal ground settling.  This is a hypothetical trace
		// only; the scan never teleports the bot or submits input.
		const Vector eye = pWaypoint->getOrigin() + Vector(0.0f, 0.0f, 28.0f);
		int visibleTarget = -1;
		for (int targetId = 0;
			targetId < static_cast<int>(sizeof(targetPoints) / sizeof(targetPoints[0]));
			++targetId)
		{
			if (CBotGlobals::isShotVisible(m_pEdict, eye,
				targetPoints[targetId], pBuildingEdict))
			{
				visibleTarget = targetId;
				break;
			}
		}

		if (visibleTarget < 0)
			continue;

		++visibleWaypoints;
		if (firstWaypoint < 0)
		{
			firstWaypoint = waypointId;
			firstTarget = visibleTarget;
			firstEye = eye;
			firstPoint = targetPoints[visibleTarget];
		}
		if (logger != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires engineer repair visibility scan=candidate waypoint=%d target=%s eye=(%.1f,%.1f,%.1f) target_point=(%.1f,%.1f,%.1f)",
				waypointId, targetNames[visibleTarget], eye.x, eye.y, eye.z,
				targetPoints[visibleTarget].x, targetPoints[visibleTarget].y,
				targetPoints[visibleTarget].z);
		}
	}

	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires engineer repair visibility scan=result=%s building_index=%d building_class=%s used_waypoints=%d visible_waypoints=%d first_waypoint=%d first_target=%s first_eye=(%.1f,%.1f,%.1f) first_point=(%.1f,%.1f,%.1f) input=none movement=none",
			visibleWaypoints > 0 ? "PASS" : "NO_VISIBLE_WAYPOINTS",
			engine->IndexOfEdict(pBuildingEdict), buildingClass, usedWaypoints,
			visibleWaypoints, firstWaypoint,
			firstTarget >= 0 ? targetNames[firstTarget] : "none",
			firstEye.x, firstEye.y, firstEye.z,
			firstPoint.x, firstPoint.y, firstPoint.z);
	}
	return true;
}

bool CBotEmpires::scanEmpiresEngineerRepairVisibilityAllBuildings()
{
	if (servertools == nullptr || engine == nullptr || m_pEdict == nullptr ||
		m_pEdict->IsFree() || gpGlobals == nullptr)
	{
		return false;
	}

	const int team = getTeam();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	// Read-only binary audit evidence identified 400 HU as the current
	// EngineerKit PrimaryAttack trace distance. This classifies candidates
	// only; it does not alter repair behavior or add a range rule.
	constexpr float kObservedEngineerTraceWindow = 400.0f;

	int usedWaypoints = 0;
	for (int waypointId = 0; waypointId < CWaypoints::numWaypoints();
		++waypointId)
	{
		CWaypoint* pWaypoint = CWaypoints::getWaypoint(waypointId);
		if (pWaypoint != nullptr && pWaypoint->isUsed() &&
			pWaypoint->forTeam(team))
		{
			++usedWaypoints;
		}
	}

	int buildings = 0;
	int buildingsWithVisibleWaypoints = 0;
	int buildingsWithinObservedTrace = 0;
	bool foundBuilding = false;

	for (CBaseEntity* pBuilding = servertools->FirstEntity();
		pBuilding != nullptr;
		pBuilding = servertools->NextEntity(pBuilding))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pBuilding);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		const char* className = pNetworkable != nullptr ?
			pNetworkable->GetClassName() : nullptr;
		if (!isEmpiresRepairableBuildingClass(className, team))
		{
			continue;
		}

		edict_t* pBuildingEdict = pNetworkable->GetEdict();
		const int buildingTeam = pBuildingEdict != nullptr ?
			CClassInterface::getTeam(pBuildingEdict) : 0;
		if (pBuildingEdict == nullptr || pBuildingEdict->IsFree() ||
			(buildingTeam > 0 && buildingTeam != team))
		{
			continue;
		}

		const ICollideable* pCollision = pBuildingEdict->GetCollideable();
		if (pCollision == nullptr)
			continue;

		foundBuilding = true;
		++buildings;
		const int buildingIndex = engine->IndexOfEdict(pBuildingEdict);
		const Vector collisionOrigin = pCollision->GetCollisionOrigin();
		const Vector mins = pCollision->OBBMins();
		const Vector maxs = pCollision->OBBMaxs();
		const Vector mid = (mins + maxs) * 0.5f;
		const Vector targetPoints[] =
		{
			collisionOrigin + mid,
			collisionOrigin + Vector(mid.x, mid.y, maxs.z)
		};
		const char* const targetNames[] = { "center", "top" };

		int visibleWaypoints = 0;
		int inTraceWaypoints = 0;
		int nearestWaypoint = -1;
		int nearestTarget = -1;
		float nearestDistance = 1.0e30f;
		float nearestInTraceDistance = 1.0e30f;
		for (int waypointId = 0; waypointId < CWaypoints::numWaypoints();
			++waypointId)
		{
			CWaypoint* pWaypoint = CWaypoints::getWaypoint(waypointId);
			if (pWaypoint == nullptr || !pWaypoint->isUsed() ||
				!pWaypoint->forTeam(team))
			{
				continue;
			}

			const Vector eye = pWaypoint->getOrigin() + Vector(0.0f, 0.0f, 28.0f);
			for (int targetId = 0; targetId < 2; ++targetId)
			{
				if (!CBotGlobals::isShotVisible(m_pEdict, eye,
					targetPoints[targetId], pBuildingEdict))
				{
					continue;
				}

				const float distance = (targetPoints[targetId] - eye).Length();
				++visibleWaypoints;
				if (distance < nearestDistance)
				{
					nearestDistance = distance;
					nearestWaypoint = waypointId;
					nearestTarget = targetId;
				}
				if (distance <= kObservedEngineerTraceWindow)
				{
					++inTraceWaypoints;
					nearestInTraceDistance = std::min(nearestInTraceDistance, distance);
				}

				if (logger != nullptr)
				{
					logger->Log(LogLevel::INFO,
						"Empires engineer repair visibility all=candidate building_index=%d building_class=%s waypoint=%d target=%s distance=%.1f in_observed_trace_window=%d eye=(%.1f,%.1f,%.1f) target_point=(%.1f,%.1f,%.1f)",
						buildingIndex, className, waypointId, targetNames[targetId],
						distance, distance <= kObservedEngineerTraceWindow ? 1 : 0,
						eye.x, eye.y, eye.z, targetPoints[targetId].x,
						targetPoints[targetId].y, targetPoints[targetId].z);
				}
			}
		}

		if (visibleWaypoints > 0)
			++buildingsWithVisibleWaypoints;
		if (inTraceWaypoints > 0)
			++buildingsWithinObservedTrace;
		if (logger != nullptr)
		{
			logger->Log(LogLevel::INFO,
				"Empires engineer repair visibility all=building building_index=%d building_class=%s visible_waypoints=%d in_observed_trace_window=%d nearest_waypoint=%d nearest_target=%s nearest_distance=%.1f nearest_in_trace_distance=%.1f",
				buildingIndex, className, visibleWaypoints, inTraceWaypoints,
				nearestWaypoint, nearestTarget >= 0 ? targetNames[nearestTarget] : "none",
				nearestDistance < 1.0e29f ? nearestDistance : -1.0f,
				nearestInTraceDistance < 1.0e29f ? nearestInTraceDistance : -1.0f);
		}
	}

	if (logger != nullptr)
	{
		logger->Log(LogLevel::INFO,
			"Empires engineer repair visibility all=result=%s team=%d used_waypoints=%d buildings=%d buildings_with_visible_waypoints=%d buildings_within_observed_trace_window=%d input=none movement=none observed_trace_window=%.1f",
			foundBuilding ? "PASS" : "NO_TEAM_BUILDINGS", team, usedWaypoints,
			buildings, buildingsWithVisibleWaypoints, buildingsWithinObservedTrace,
			kObservedEngineerTraceWindow);
	}
	return foundBuilding;
}

bool CBotEmpires::requestEmpiresEngineerRepairOnce(const int buildingIndex,
	const bool requireFullHealth)
{
	if (isEmpiresEngineerRepairActionActive() || isEmpiresMortarProbeActive() ||
		isEmpiresMortarActionActive() || m_bBootstrapOnly ||
		m_bControllerProbeOnly || m_pEdict == nullptr || m_pEdict->IsFree() ||
		m_pPlayerInfo == nullptr || m_pController == nullptr || m_pButtons == nullptr ||
		engine == nullptr || gpGlobals == nullptr || servertools == nullptr ||
		!m_pPlayerInfo->IsConnected() || m_pPlayerInfo->IsObserver() ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_ENGINEER)
	{
		return false;
	}

	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	edict_t* pSelectedBuilding = nullptr;
	edict_t* pFullHealthFallback = nullptr;
	float selectedDistance = 1.0e30f;
	float fallbackDistance = 1.0e30f;

	for (CBaseEntity* pEntity = servertools->FirstEntity(); pEntity != nullptr;
		pEntity = servertools->NextEntity(pEntity))
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		edict_t* pCandidate = pNetworkable != nullptr ?
			pNetworkable->GetEdict() : nullptr;
		const char* className = pNetworkable != nullptr ?
			pNetworkable->GetClassName() : nullptr;
		if (pCandidate == nullptr || pCandidate->IsFree() ||
			!isEmpiresRepairableBuildingClass(className, team))
		{
			continue;
		}

		const int candidateIndex = engine->IndexOfEdict(pCandidate);
		if (buildingIndex >= 0 && candidateIndex != buildingIndex)
			continue;

		const int candidateTeam = CClassInterface::getTeam(pCandidate);
		if (candidateTeam > 0 && candidateTeam != team)
			continue;

		int health = -1;
		int maxHealth = -1;
		if (!readEmpiresEntityIntProperty(pCandidate, "m_iHealth", health) ||
			!readEmpiresEntityIntProperty(pCandidate, "m_iMaxHealth", maxHealth))
		{
			continue;
		}

		const float distance = distanceFrom(pCandidate);
		if (health < maxHealth && distance < selectedDistance)
		{
			pSelectedBuilding = pCandidate;
			selectedDistance = distance;
		}
		else if (pFullHealthFallback == nullptr || distance < fallbackDistance)
		{
			pFullHealthFallback = pCandidate;
			fallbackDistance = distance;
		}
	}

	if (pSelectedBuilding == nullptr)
		pSelectedBuilding = pFullHealthFallback;
	if (pSelectedBuilding == nullptr)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires engineer repair rejected: no friendly building found requested_index=%d team=%d",
				buildingIndex, team);
		return false;
	}

	int healthBefore = -1;
	int maxHealth = -1;
	if (!readEmpiresEntityIntProperty(pSelectedBuilding, "m_iHealth", healthBefore) ||
		!readEmpiresEntityIntProperty(pSelectedBuilding, "m_iMaxHealth", maxHealth))
	{
		return false;
	}

	const int selectedIndex = engine->IndexOfEdict(pSelectedBuilding);
	if (healthBefore >= maxHealth)
	{
		if (logger != nullptr)
			logger->Log(LogLevel::WARN,
				"Empires engineer repair rejected: selected building already full index=%d class=%s health=%d max_health=%d requested_index=%d",
				selectedIndex, pSelectedBuilding->GetClassName(), healthBefore,
				maxHealth, buildingIndex);
		return false;
	}

	// The explicit repair proof owns the bot's input window.  Suspend the
	// already-running normal route and its generic schedules so waypoint
	// steering cannot move the Engineer away while the kit is being tested.
	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		m_pNavigator->clear();
	m_eNormalPathState = EmpiresNormalPathState::IDLE;
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	stopMoving();
	if (logger != nullptr)
		logger->Log(LogLevel::INFO,
			"Empires engineer repair=owns-input normal-route-suspended building_index=%d",
			selectedIndex);

	m_iEmpiresEngineerRepairActionTeam = team;
	m_iEmpiresEngineerRepairActionExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iEmpiresEngineerRepairActionExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pEmpiresEngineerRepairActionExpectedEdict = m_pEdict;
	m_pEmpiresEngineerRepairActionBuilding = pSelectedBuilding;
	m_iEmpiresEngineerRepairActionBuildingIndex = selectedIndex;
	m_iEmpiresEngineerRepairActionHealthBefore = healthBefore;
	m_iEmpiresEngineerRepairActionHealthLast = healthBefore;
	m_iEmpiresEngineerRepairActionMaxHealth = maxHealth;
	m_iEmpiresEngineerRepairActionFrames = 0;
	m_flEmpiresEngineerRepairActionDeadline =
		gpGlobals->curtime + kEmpiresEngineerRepairActionSeconds;
	m_bEmpiresEngineerRepairActionRequireFullHealth = requireFullHealth;
	m_bEmpiresEngineerRepairActionProgressLogged = false;
	m_eEmpiresEngineerRepairActionState =
		EmpiresEngineerRepairActionState::WAITING_FOR_ACTIVE;

	selectWeaponName(team == EMP_TEAM_NF ? "weapon_emp_nf_engineer_kit" :
		"weapon_emp_imp_engineer_kit");
	lookAtEdict(pSelectedBuilding);
	setLookAtTask(LOOK_EDICT);

	if (logger != nullptr)
		logger->Log(LogLevel::INFO,
			"Empires engineer repair=armed index=%d userid=%d team=%d class=%d class_property=%s building_index=%d building_class=%s building_distance=%.1f health_before=%d max_health=%d deadline=%.3f input=normal-primary-attack",
			m_iEmpiresEngineerRepairActionExpectedIndex,
			m_iEmpiresEngineerRepairActionExpectedUserId, team, classId,
			classPropertyName != nullptr ? classPropertyName : "<unknown>",
			selectedIndex, pSelectedBuilding->GetClassName(),
			distanceFrom(pSelectedBuilding), healthBefore, maxHealth,
			m_flEmpiresEngineerRepairActionDeadline);
	return true;
}

bool CBotEmpires::requestEmpiresEngineerBuildSelectionOnce()
{
	if (isEmpiresEngineerRepairActionActive() ||
		isEmpiresEngineerBuildSelectionActive() || isEmpiresMortarProbeActive() ||
		isEmpiresMortarActionActive() || m_bBootstrapOnly ||
		m_bControllerProbeOnly || m_pEdict == nullptr || m_pEdict->IsFree() ||
		m_pPlayerInfo == nullptr || m_pController == nullptr || m_pButtons == nullptr ||
		engine == nullptr || gpGlobals == nullptr ||
		!m_pPlayerInfo->IsConnected() || m_pPlayerInfo->IsObserver() ||
		m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_ENGINEER)
	{
		return false;
	}

	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	int buildItem = -1;
	int buildState = -1;
	const bool itemKnown = readEmpiresDebugIntProperty(
		m_pEdict, "m_iEngineerBuildItem", buildItem);
	const bool stateKnown = readEmpiresDebugIntProperty(
		m_pEdict, "m_iEngBuild", buildState);
	if (!itemKnown && !stateKnown)
		return false;

	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		m_pNavigator->clear();
	m_eNormalPathState = EmpiresNormalPathState::IDLE;
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	stopMoving();

	m_iEmpiresEngineerBuildSelectionTeam = team;
	m_iEmpiresEngineerBuildSelectionExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iEmpiresEngineerBuildSelectionExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pEmpiresEngineerBuildSelectionExpectedEdict = m_pEdict;
	m_pEmpiresEngineerBuildSelectionWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	m_iEmpiresEngineerBuildSelectionFrames = 0;
	m_iEmpiresEngineerBuildSelectionBaselineItem = buildItem;
	m_iEmpiresEngineerBuildSelectionLastItem = buildItem;
	m_iEmpiresEngineerBuildSelectionBaselineState = buildState;
	m_iEmpiresEngineerBuildSelectionLastState = buildState;
	m_bEmpiresEngineerBuildSelectionItemKnown = itemKnown;
	m_bEmpiresEngineerBuildSelectionStateKnown = stateKnown;
	m_bEmpiresEngineerBuildSelectionInputRequested = false;
	m_bEmpiresEngineerBuildSelectionInputSubmitted = false;
	m_bEmpiresEngineerBuildSelectionBaselineLogged = false;
	m_bEmpiresEngineerBuildSelectionTransitionLogged = false;
	m_flEmpiresEngineerBuildSelectionDeadline =
		gpGlobals->curtime + kEmpiresEngineerRepairActionSeconds;
	m_eEmpiresEngineerBuildSelectionState =
		EmpiresEngineerBuildSelectionState::WAITING_FOR_ACTIVE;

	const char* kitClassName = team == EMP_TEAM_NF ?
		"weapon_emp_nf_engineer_kit" : "weapon_emp_imp_engineer_kit";
	selectWeaponName(kitClassName);
	if (logger != nullptr)
		logger->Log(LogLevel::INFO,
			"Empires engineer build-select=armed index=%d userid=%d team=%d class=%d class_property=%s item_known=%d item_before=%d build_state_known=%d build_state_before=%d deadline=%.3f input=normal-secondary-attack",
			m_iEmpiresEngineerBuildSelectionExpectedIndex,
			m_iEmpiresEngineerBuildSelectionExpectedUserId, team, classId,
			classPropertyName != nullptr ? classPropertyName : "<unknown>",
			itemKnown ? 1 : 0, buildItem, stateKnown ? 1 : 0, buildState,
			m_flEmpiresEngineerBuildSelectionDeadline);
	return true;
}

bool CBotEmpires::requestEmpiresEngineerBuildOnce(const int targetWaypoint,
	const Vector* targetOverride, const bool requireFullHealth)
{
	if ((targetOverride == nullptr && (targetWaypoint < 0 ||
		targetWaypoint >= CWaypoints::MAX_WAYPOINTS)) ||
		(targetOverride != nullptr && !targetOverride->IsValid()) ||
		isEmpiresEngineerRepairActionActive() ||
		isEmpiresEngineerBuildSelectionActive() ||
		isEmpiresEngineerBuildActionActive() || isEmpiresMortarProbeActive() ||
		isEmpiresMortarActionActive() || m_bBootstrapOnly ||
		m_bControllerProbeOnly || m_pEdict == nullptr || m_pEdict->IsFree() ||
		m_pPlayerInfo == nullptr || m_pController == nullptr ||
		m_pButtons == nullptr || engine == nullptr || gpGlobals == nullptr ||
		servertools == nullptr || !m_pPlayerInfo->IsConnected() ||
		m_pPlayerInfo->IsObserver() || m_pPlayerInfo->IsDead() ||
		m_pPlayerInfo->GetHealth() <= 0)
	{
		return false;
	}

	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	if (!readEmpiresClass(m_pEdict, classId, classDesired, classDesiredKnown,
		classPropertyName) || classId != EMP_CLASS_ENGINEER)
	{
		return false;
	}

	const int team = m_pPlayerInfo->GetTeamIndex();
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
		return false;

	CWaypoint* pTarget = targetOverride == nullptr ?
		CWaypoints::getWaypoint(targetWaypoint) : nullptr;
	if (targetOverride == nullptr &&
		(pTarget == nullptr || !pTarget->isUsed() || !pTarget->forTeam(team) ||
		!pTarget->getOrigin().IsValid()))
	{
		return false;
	}

	int baselineMaxIndex = -1;
	const int baselineTurrets = countEmpiresEngineerMGTurrets(&baselineMaxIndex);
	if (m_pSchedules != nullptr)
		m_pSchedules->freeMemory();
	if (m_pNavigator != nullptr)
		m_pNavigator->clear();
	m_eNormalPathState = EmpiresNormalPathState::IDLE;
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	stopMoving();

	m_iEmpiresEngineerBuildActionTeam = team;
	m_iEmpiresEngineerBuildActionExpectedIndex = engine->IndexOfEdict(m_pEdict);
	m_iEmpiresEngineerBuildActionExpectedUserId = engine->GetPlayerUserId(m_pEdict);
	m_pEmpiresEngineerBuildActionExpectedEdict = m_pEdict;
	m_iEmpiresEngineerBuildActionTargetWaypoint =
		targetOverride != nullptr ? -1 : targetWaypoint;
	// Current Empires waypoint anchors are 32 units above the player-foot
	// surface on this verified fixture.  Aim at that observed ground level so
	// the kit's own placement trace evaluates the surface rather than an
	// airborne waypoint anchor.  This is confined to the debug build probe.
	m_vEmpiresEngineerBuildActionTarget = targetOverride != nullptr ?
		*targetOverride : pTarget->getOrigin() - Vector(0.0f, 0.0f, 32.0f);
	m_iEmpiresEngineerBuildActionBaselineTurrets = baselineTurrets;
	m_iEmpiresEngineerBuildActionBaselineMaxIndex = baselineMaxIndex;
	m_iEmpiresEngineerBuildActionResultIndex = -1;
	m_iEmpiresEngineerBuildActionFrames = 0;
	m_bEmpiresEngineerBuildActionCommandRequested = false;
	m_bEmpiresEngineerBuildActionCompletionCommandRequested = false;
	m_bEmpiresEngineerBuildActionAttackSubmitted = false;
	m_bEmpiresEngineerBuildActionBaselineLogged = false;
	m_bEmpiresEngineerBuildActionSelectionLogged = false;
	m_bEmpiresEngineerBuildActionResultObservedLogged = false;
	m_bEmpiresEngineerBuildActionRequireFullHealth = requireFullHealth;
	m_iEmpiresEngineerBuildActionLastTraceIndex = -2;
	m_iEmpiresEngineerBuildActionLastTraceOwner = -2;
	m_iEmpiresEngineerBuildActionLastTraceMoveParent = -2;
	m_iEmpiresEngineerBuildActionLastTraceParent = -2;
	m_iEmpiresEngineerBuildActionLastTraceInfoOwner = -2;
	m_iEmpiresEngineerBuildActionLastTraceInfoParent = -2;
	m_iEmpiresEngineerBuildActionLastTraceInfoMoveParent = -2;
	m_iEmpiresEngineerBuildActionLastTraceInfoRootMoveParent = -2;
	m_iEmpiresEngineerBuildActionLastTraceHealth = -2;
	m_iEmpiresEngineerBuildActionLastTraceMaxHealth = -2;
	m_iEmpiresEngineerBuildActionLastTraceFullyBuilt = -2;
	m_flEmpiresEngineerBuildActionDeadline =
		gpGlobals->curtime + kEmpiresEngineerBuildActionSeconds;
	m_eEmpiresEngineerBuildActionState =
		EmpiresEngineerBuildActionState::WAITING_FOR_ACTIVE;

	selectWeaponName(team == EMP_TEAM_NF ? "weapon_emp_nf_engineer_kit" :
		"weapon_emp_imp_engineer_kit");
	if (logger != nullptr)
		logger->Log(LogLevel::INFO,
			"Empires engineer build=armed index=%d userid=%d team=%d class=%d class_property=%s target_waypoint=%d target=(%.1f %.1f %.1f) baseline_turrets=%d baseline_max_index=%d deadline=%.3f input=normal-primary-attack",
			m_iEmpiresEngineerBuildActionExpectedIndex,
			m_iEmpiresEngineerBuildActionExpectedUserId, team, classId,
			classPropertyName != nullptr ? classPropertyName : "<unknown>",
			targetWaypoint, m_vEmpiresEngineerBuildActionTarget.x,
			m_vEmpiresEngineerBuildActionTarget.y,
			m_vEmpiresEngineerBuildActionTarget.z, baselineTurrets,
			baselineMaxIndex, m_flEmpiresEngineerBuildActionDeadline);
	return true;
}

void CBotEmpires::failEmpiresMortarSmoke(const char* reason)
{
	if (!isEmpiresMortarProbeActive())
		return;

	if (m_pButtons != nullptr)
	{
		m_pButtons->letGo(IN_ATTACK);
		m_pButtons->letGo(IN_ATTACK2);
		m_pButtons->letGo(IN_RELOAD);
		m_pButtons->letGo(IN_DUCK);
	}
	m_fForwardSpeed = 0.0f;
	m_fSideSpeed = 0.0f;
	m_fUpSpeed = 0.0f;
	m_iButtons = 0;
	stopMoving();

	logger->Log(LogLevel::WARN,
		"Empires mortar probe=result=FAIL attempt=%d state=%s reason=%s frames=%d shell_baseline=%d shell_index=%d",
		m_iEmpiresMortarProbeAttempt,
		empiresMortarProbeStateName(m_eEmpiresMortarProbeState),
		reason != nullptr ? reason : "unspecified", m_iEmpiresMortarProbeFrames,
		m_iEmpiresMortarProbeShellBaseline, m_iEmpiresMortarProbeShellIndex);
	m_eEmpiresMortarProbeState = EmpiresMortarProbeState::FAILED;
}

void CBotEmpires::observeEmpiresMortarSmoke()
{
	if (!isEmpiresMortarProbeActive() || engine == nullptr || gpGlobals == nullptr ||
		m_pEdict == nullptr || m_pEdict->IsFree() || m_pPlayerInfo == nullptr ||
		m_pController == nullptr)
	{
		return;
	}

	++m_iEmpiresMortarProbeFrames;
	const int index = engine->IndexOfEdict(m_pEdict);
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if (index != m_iEmpiresMortarProbeExpectedIndex ||
		userId != m_iEmpiresMortarProbeExpectedUserId ||
		m_pEdict != m_pEmpiresMortarProbeExpectedEdict)
	{
		failEmpiresMortarSmoke("client-identity-changed");
		return;
	}

	if (m_pPlayerInfo->IsDead() || m_pPlayerInfo->GetHealth() <= 0)
	{
		failEmpiresMortarSmoke("bot-no-longer-alive");
		return;
	}

	if ((m_eEmpiresMortarProbeState == EmpiresMortarProbeState::WAITING_FOR_ACTIVE ||
		m_eEmpiresMortarProbeState == EmpiresMortarProbeState::WAITING_FOR_READY ||
		m_eEmpiresMortarProbeState == EmpiresMortarProbeState::ATTACK_ARMED ||
		m_eEmpiresMortarProbeState == EmpiresMortarProbeState::ATTACK_SENT) &&
		m_pButtons != nullptr)
	{
		// Use the normal button scheduler before doButtons() copies the
		// scheduled input into the next RunPlayerMove command.
		m_pButtons->duck(true);
	}

	const int flags = CClassInterface::getPlayerFlags(m_pEdict);
	const bool onGround = (flags & FL_ONGROUND) != 0;
	const bool ducking = (flags & FL_DUCKING) != 0;
	const bool fakeClientFlag = (flags & FL_FAKECLIENT) != 0;
	const bool fakeClientIdentity = m_pPlayerInfo->IsFakeClient();
	if (!m_bEmpiresMortarProbeLastFlagsKnown ||
		m_iEmpiresMortarProbeLastFlags != flags)
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar probe=flags attempt=%d index=%d userid=%d flags=0x%x fl_onground=%d fl_ducking=%d fl_fakeclient=%d fakeclient_identity=%d",
			m_iEmpiresMortarProbeAttempt, index, userId, flags,
			onGround ? 1 : 0, ducking ? 1 : 0,
			fakeClientFlag ? 1 : 0, fakeClientIdentity ? 1 : 0);
		m_iEmpiresMortarProbeLastFlags = flags;
		m_bEmpiresMortarProbeLastFlagsKnown = true;
	}
	if (onGround && ducking && !m_bEmpiresMortarProbePostureReadyLogged)
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar probe=posture-ready attempt=%d index=%d userid=%d fl_onground=1 fl_ducking=1 fl_fakeclient=%d fakeclient_identity=%d",
			m_iEmpiresMortarProbeAttempt, index, userId,
			fakeClientFlag ? 1 : 0, fakeClientIdentity ? 1 : 0);
		m_bEmpiresMortarProbePostureReadyLogged = true;
	}

	const edict_t* pCurrentWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const bool mortarActive = pCurrentWeapon != nullptr && !pCurrentWeapon->IsFree() &&
		pCurrentWeapon->GetClassName() != nullptr &&
		std::strcmp(pCurrentWeapon->GetClassName(), "weapon_emp_nf_mortar") == 0;

	if (m_eEmpiresMortarProbeState == EmpiresMortarProbeState::WAITING_FOR_ACTIVE)
	{
		if (mortarActive)
		{
			logger->Log(LogLevel::INFO,
				"Empires mortar probe=selected attempt=%d index=%d userid=%d active_weapon=%s weapon_index=%d",
				m_iEmpiresMortarProbeAttempt, index, userId,
				pCurrentWeapon->GetClassName(),
				engine->IndexOfEdict(const_cast<edict_t*>(pCurrentWeapon)));
			m_pEmpiresMortarProbeWeapon = pCurrentWeapon;
			m_eEmpiresMortarProbeState = EmpiresMortarProbeState::WAITING_FOR_READY;
			m_iEmpiresMortarProbeFrames = 0;
		}
		else if (gpGlobals->curtime >= m_flEmpiresMortarProbeDeadline)
		{
			failEmpiresMortarSmoke("mortar-selection-timeout");
		}
		return;
	}

	if (!mortarActive)
	{
		if (gpGlobals->curtime >= m_flEmpiresMortarProbeDeadline)
			failEmpiresMortarSmoke("mortar-no-longer-active");
		return;
	}

	// The active weapon entity can be rebound while Empires equips a weapon.
	// The current active entity is authoritative for the named property reads;
	// do not reject a valid mortar merely because its edict identity changed.
	if (m_pEmpiresMortarProbeWeapon != pCurrentWeapon)
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar probe=weapon-entity-rebound attempt=%d index=%d userid=%d",
			m_iEmpiresMortarProbeAttempt, index, userId);
		m_pEmpiresMortarProbeWeapon = pCurrentWeapon;
	}

	edict_t* pWeapon = const_cast<edict_t*>(pCurrentWeapon);
	if (m_iEmpiresMortarProbeFrames == 1)
	{
		IServerUnknown* pUnknown = pWeapon->GetUnknown();
		IServerNetworkable* pNetworkable = pUnknown != nullptr ?
			pUnknown->GetNetworkable() : nullptr;
		const ServerClass* pClass = pNetworkable != nullptr ?
			pNetworkable->GetServerClass() : nullptr;
		logger->Log(LogLevel::INFO,
			"Empires mortar probe=metadata server_class=%s network_class=%s",
			pClass != nullptr && pClass->m_pNetworkName != nullptr ?
				pClass->m_pNetworkName : "<none>",
			pWeapon->GetClassName() != nullptr ? pWeapon->GetClassName() : "<none>");

		const auto containsMortarProperty = [](const char* name)
		{
			return name != nullptr &&
				(std::strstr(name, "mortar") != nullptr ||
					std::strstr(name, "Mortar") != nullptr ||
					std::strstr(name, "loaded") != nullptr ||
					std::strstr(name, "Loaded") != nullptr ||
					std::strstr(name, "reload") != nullptr ||
					std::strstr(name, "Reload") != nullptr ||
					std::strstr(name, "shell") != nullptr ||
					std::strstr(name, "Shell") != nullptr);
		};

		const auto logSendTable = [&](const auto& self, SendTable* pTable,
			const int parentOffset) -> void
		{
			if (pTable == nullptr)
				return;

			for (int propIndex = 0; propIndex < pTable->GetNumProps(); ++propIndex)
			{
				SendProp* pProp = pTable->GetProp(propIndex);
				if (pProp == nullptr)
					continue;

				const char* name = pProp->GetName();
				if (containsMortarProperty(name))
				{
					logger->Log(LogLevel::INFO,
						"Empires mortar probe=sendprop name=%s type=%d offset=%d parent_offset=%d",
						name != nullptr ? name : "<none>",
						static_cast<int>(pProp->GetType()), pProp->GetOffset(),
						parentOffset);
				}

				if (pProp->GetDataTable() != nullptr)
					self(self, pProp->GetDataTable(), parentOffset + pProp->GetOffset());
			}
		};

		if (pClass != nullptr)
			logSendTable(logSendTable, pClass->m_pTable, 0);

		datamap_t* pMap = pUnknown != nullptr ?
			CBaseEntity_GetDataDescMap(pUnknown->GetBaseEntity()) : nullptr;
		for (; pMap != nullptr; pMap = pMap->baseMap)
		{
			for (int fieldIndex = 0; fieldIndex < pMap->dataNumFields; ++fieldIndex)
			{
				const typedescription_t& field = pMap->dataDesc[fieldIndex];
				if (containsMortarProperty(field.fieldName))
				{
					logger->Log(LogLevel::INFO,
						"Empires mortar probe=datamap name=%s type=%d",
						field.fieldName != nullptr ? field.fieldName : "<none>",
						static_cast<int>(field.fieldType));
				}
			}
		}
	}

	// Empires stores the mortar readiness bit on CEmpPlayer, not on the
	// CWeaponMortar server class.  Keep this a named dynamic property read;
	// the weapon entity is still used for m_flReloadTime and m_hShell below.
	bool loaded = false;
	const bool loadedKnown = readEmpiresBoolProperty(m_pEdict,
		"m_bMortarLoaded", loaded);
	if (m_iEmpiresMortarProbeFrames == 1 ||
		loadedKnown != m_bEmpiresMortarProbeLoadedKnown ||
		(loadedKnown && loaded != m_bEmpiresMortarProbeLastLoaded))
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar probe=readiness attempt=%d index=%d userid=%d loaded_known=%d loaded=%d",
			m_iEmpiresMortarProbeAttempt, index, userId, loadedKnown ? 1 : 0,
			loaded ? 1 : 0);
		m_bEmpiresMortarProbeLoadedKnown = loadedKnown;
		m_bEmpiresMortarProbeLastLoaded = loaded;
	}

	float reloadTime = 0.0f;
	const bool reloadTimeKnown = readEmpiresWeaponFloatProperty(pWeapon,
		"m_flReloadTime", reloadTime);
	if (reloadTimeKnown && (!m_bEmpiresMortarProbeReloadTimeKnown ||
		std::fabs(reloadTime - m_flEmpiresMortarProbeReloadTime) > 0.001f))
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar probe=reload-timer attempt=%d reload_time=%.3f now=%.3f",
			m_iEmpiresMortarProbeAttempt, reloadTime, gpGlobals->curtime);
		m_bEmpiresMortarProbeReloadTimeKnown = true;
		m_flEmpiresMortarProbeReloadTime = reloadTime;
	}

	// Selecting a weapon can leave the normal Source fire cooldown in the
	// future. Wait for the named runtime property instead of pulsing attack
	// immediately after the weapon switch.
	float nextPrimaryAttack = 0.0f;
	const bool nextPrimaryKnown = readEmpiresWeaponFloatProperty(pWeapon,
		"m_flNextPrimaryAttack", nextPrimaryAttack);
	if (nextPrimaryKnown && (!m_bEmpiresMortarProbeNextPrimaryKnown ||
		std::fabs(nextPrimaryAttack - m_flEmpiresMortarProbeNextPrimaryAttack) > 0.001f))
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar probe=primary-timer attempt=%d next_primary=%.3f now=%.3f",
			m_iEmpiresMortarProbeAttempt, nextPrimaryAttack, gpGlobals->curtime);
		m_bEmpiresMortarProbeNextPrimaryKnown = true;
		m_flEmpiresMortarProbeNextPrimaryAttack = nextPrimaryAttack;
	}

	float playerNextAttack = 0.0f;
	const bool playerNextAttackKnown = readEmpiresDebugFloatProperty(m_pEdict,
		"m_flNextAttack", playerNextAttack);

	int shellHandleIndex = -1;
	const bool shellHandleKnown = readEmpiresWeaponHandleProperty(pWeapon,
		"m_hShell", shellHandleIndex);
	int clip1 = -1;
	int clip2 = -1;
	int ammoType1 = -1;
	int ammoType2 = -1;
	int ammo1 = -1;
	if (pWeapon != nullptr && !pWeapon->IsFree())
	{
		CClassInterface::getWeaponClip(pWeapon, &clip1, &clip2);
		CClassInterface::getAmmoTypes(pWeapon, &ammoType1, &ammoType2);
		if (ammoType1 >= 0)
			ammo1 = getAmmo(static_cast<std::size_t>(ammoType1));
	}
	if (clip1 != m_iEmpiresMortarProbeLastClip || ammo1 != m_iEmpiresMortarProbeLastAmmo)
	{
		logger->Log(LogLevel::INFO,
			"Empires mortar probe=inventory attempt=%d clip1=%d ammo_type1=%d ammo1=%d shell_handle_known=%d shell_handle_index=%d",
			m_iEmpiresMortarProbeAttempt, clip1, ammoType1, ammo1,
			shellHandleKnown ? 1 : 0, shellHandleIndex);
		m_iEmpiresMortarProbeLastClip = clip1;
		m_iEmpiresMortarProbeLastAmmo = ammo1;
	}

	// Weapon/player attack timers use the simulation clock.  Keep this
	// debug-only probe on the same clock; engine->Time() is not the command
	// processing time observed by the server in this fixture.
	const float now = gpGlobals->curtime;
	if (m_eEmpiresMortarProbeState == EmpiresMortarProbeState::WAITING_FOR_READY ||
		m_eEmpiresMortarProbeState == EmpiresMortarProbeState::RELOAD_WAIT)
	{
		const bool reloadReady = reloadTimeKnown && reloadTime <= now;
		const bool playerAttackReady = playerNextAttackKnown &&
			playerNextAttack <= now;
		const bool primaryReady = nextPrimaryKnown && nextPrimaryAttack <= now;
		if (loadedKnown && loaded && onGround && ducking && reloadReady &&
			playerAttackReady && primaryReady)
		{
			logger->Log(LogLevel::INFO,
				"Empires mortar probe=ready attempt=%d index=%d userid=%d loaded=1 curtime=%.3f fl_onground=%d fl_ducking=%d fl_fakeclient=%d fakeclient_identity=%d reload_time_known=%d reload_time=%.3f player_next_attack_known=%d player_next_attack=%.3f next_primary_known=%d next_primary=%.3f clip1=%d ammo1=%d",
				m_iEmpiresMortarProbeAttempt, index, userId, gpGlobals->curtime,
				onGround ? 1 : 0, ducking ? 1 : 0, fakeClientFlag ? 1 : 0,
				fakeClientIdentity ? 1 : 0, reloadTimeKnown ? 1 : 0, reloadTime,
				playerNextAttackKnown ? 1 : 0, playerNextAttack,
				nextPrimaryKnown ? 1 : 0, nextPrimaryAttack, clip1, ammo1);
			m_pButtons->duck(true);
			m_pButtons->letGo(IN_ATTACK);
			primaryAttack(false);
			m_eEmpiresMortarProbeState = EmpiresMortarProbeState::ATTACK_ARMED;
			m_iEmpiresMortarProbeFrames = 0;
			return;
		}

		if (m_eEmpiresMortarProbeState == EmpiresMortarProbeState::WAITING_FOR_READY &&
			m_iEmpiresMortarProbeFrames >= 90)
		{
			// This is the explicitly bounded fallback for a stalled automatic
			// ItemPostFrame reload.  It does not alter normal weapon handling.
			m_pButtons->letGo(IN_RELOAD);
			reload();
			m_iEmpiresMortarProbeAttempt = 3;
			m_iEmpiresMortarProbeFrames = 0;
			m_eEmpiresMortarProbeState = EmpiresMortarProbeState::RELOAD_WAIT;
			logger->Log(LogLevel::INFO,
				"Empires mortar probe=reload-requested attempt=3 reason=automatic-readiness-stalled");
			return;
		}

		if (m_eEmpiresMortarProbeState == EmpiresMortarProbeState::RELOAD_WAIT &&
			m_iEmpiresMortarProbeFrames >= 150)
		{
			failEmpiresMortarSmoke(loadedKnown ?
				"mortar-readiness-did-not-complete" :
				"mortar-readiness-property-unavailable");
		}
		return;
	}

	if (m_eEmpiresMortarProbeState == EmpiresMortarProbeState::COUCH_PREP)
	{
		m_pButtons->duck(true);
		if (onGround && ducking)
		{
			m_iEmpiresMortarProbeAttempt = 2;
			m_iEmpiresMortarProbeFrames = 0;
			m_eEmpiresMortarProbeState = EmpiresMortarProbeState::WAITING_FOR_READY;
			logger->Log(LogLevel::INFO,
				"Empires mortar probe=posture-ready attempt=2 on_ground=1 ducking=1");
		}
		else if (now >= m_flEmpiresMortarProbeDeadline)
		{
			failEmpiresMortarSmoke("crouch-posture-timeout");
		}
		return;
	}

	if (m_eEmpiresMortarProbeState == EmpiresMortarProbeState::OBSERVING_SHELL)
	{
		int shellIndex = -1;
		const char* shellClassName = nullptr;
		const int shellCount = countEmpiresMortarShells(&shellIndex,
			&shellClassName);
		if (shellCount > m_iEmpiresMortarProbeShellBaseline)
		{
			m_iEmpiresMortarProbeShellIndex = shellIndex;
			m_pButtons->letGo(IN_ATTACK);
			m_pButtons->letGo(IN_ATTACK2);
			m_pButtons->letGo(IN_RELOAD);
			m_pButtons->letGo(IN_DUCK);
			m_fForwardSpeed = 0.0f;
			m_fSideSpeed = 0.0f;
			m_fUpSpeed = 0.0f;
			m_iButtons = 0;
			stopMoving();
			logger->Log(LogLevel::INFO,
				"Empires mortar probe=result=PASS attempt=%d attack_input=1 shell_created=1 shell_class=%s shell_index=%d shell_count=%d baseline=%d target=none",
				m_iEmpiresMortarProbeAttempt,
				shellClassName != nullptr ? shellClassName : "<unknown>",
				m_iEmpiresMortarProbeShellIndex, shellCount,
				m_iEmpiresMortarProbeShellBaseline);
			m_eEmpiresMortarProbeState = EmpiresMortarProbeState::COMPLETE;
			return;
		}

		if (now >= m_flEmpiresMortarProbeShellDeadline)
		{
			if (m_iEmpiresMortarProbeAttempt == 1 && !onGround)
			{
				m_iEmpiresMortarProbeAttempt = 2;
				m_iEmpiresMortarProbeFrames = 0;
				m_flEmpiresMortarProbeShellDeadline = now + 4.0f;
				m_eEmpiresMortarProbeState = EmpiresMortarProbeState::COUCH_PREP;
				logger->Log(LogLevel::INFO,
					"Empires mortar probe=posture-fallback attempt=2 reason=first-attack-no-shell-not-grounded");
				return;
			}

			failEmpiresMortarSmoke(onGround ?
				"no-shell-posture-accepted" :
				"no-shell-posture-rejected-after-crouch-window");
			return;
		}
	}

	if (now >= m_flEmpiresMortarProbeDeadline)
		failEmpiresMortarSmoke("probe-deadline");
}

bool CBotEmpires::readBootstrapBoolProperty(const char* propertyName, bool& value) const
{
	return readEmpiresBoolProperty(getCurrentBootstrapEdict(), propertyName, value);
}

bool CBotEmpires::readBootstrapPlayerState(EmpiresBootstrapPlayerState& state) const
{
	state = {};

	if (!isControlledTestOnly() || engine == nullptr || gpGlobals == nullptr || playerinfomanager == nullptr)
	{
		return false;
	}

	edict_t* pCurrentEdict = getCurrentBootstrapEdict();
	if (pCurrentEdict == nullptr)
	{
		return false;
	}
	const int index = engine->IndexOfEdict(pCurrentEdict);

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(pCurrentEdict);
	if (pInfo == nullptr)
	{
		return false;
	}

	state.index = index;
	state.userId = engine->GetPlayerUserId(pCurrentEdict);
	state.edict = pCurrentEdict;
	state.connected = pInfo->IsConnected();
	state.fakeClient = pInfo->IsFakeClient();
	state.observer = pInfo->IsObserver();
	state.active = state.connected && !state.observer;

	if (state.connected && pInfo->IsPlayer())
	{
		state.team = pInfo->GetTeamIndex();
		state.dead = pInfo->IsDead();
		state.alive = !state.dead && !state.observer;
		state.health = pInfo->GetHealth();
		state.healthKnown = true;
		state.origin = pInfo->GetAbsOrigin();
		state.originKnown = true;
		if (entityinfomanager != nullptr)
		{
			IEntityInfo* pEntityInfo = entityinfomanager->GetEntityInfo(pCurrentEdict);
			if (pEntityInfo != nullptr)
			{
				state.velocity = pEntityInfo->GetAbsVelocity();
				state.velocityKnown = true;
			}
		}
	}

	state.lifeStateKnown = readBootstrapIntProperty("m_lifeState", state.lifeState);
	state.appliedSidearmKnown = readBootstrapIntProperty("m_iSidearmWeapon", state.appliedSidearm);
	state.sidearmDesiredKnown = readBootstrapBoolProperty("m_bSidearmWeaponDesired", state.sidearmDesired);
	state.appliedPrimaryKnown = readBootstrapIntProperty("m_iPrimaryWeapon", state.appliedPrimary);
	state.primaryDesiredKnown = readBootstrapBoolProperty("m_bPrimaryWeaponDesired", state.primaryDesired);
	state.appliedGrenadeKnown = readBootstrapIntProperty("m_iGrenade", state.appliedGrenade);
	state.grenadeDesiredKnown = readBootstrapBoolProperty("m_bGrenadeDesired", state.grenadeDesired);
	state.appliedEquipmentKnown = readBootstrapIntProperty("m_iEquipment", state.appliedEquipment);
	state.equipmentDesiredKnown = readBootstrapBoolProperty("m_bEquipmentDesired", state.equipmentDesired);
	state.desiredSpawnKnown = readBootstrapIntProperty("m_iDesiredSpawn", state.desiredSpawn);
	state.respawnTimerKnown = readBootstrapFloatProperty("m_flTimeToRespawn", state.respawnTimer);
	state.currentTimeKnown = gpGlobals != nullptr;
	state.currentTime = gpGlobals != nullptr ? gpGlobals->curtime : 0.0f;
	const CBotCmd lastUserCommand = pInfo->GetLastUserCommand();
	state.lastUserCommandKnown = true;
	state.lastUserCommandNumber = lastUserCommand.command_number;
	state.lastUserCommandTick = lastUserCommand.tick_count;
	state.lastUserCommandButtons = lastUserCommand.buttons;
	state.lastUserCommandForwardMove = lastUserCommand.forwardmove;
	state.lastUserCommandSideMove = lastUserCommand.sidemove;
	state.lastUserCommandUpMove = lastUserCommand.upmove;
	state.lastUserCommandImpulse = lastUserCommand.impulse;
	state.lastUserCommandWeaponSelect = lastUserCommand.weaponselect;

	const char* classPropertyName = nullptr;
	state.classKnown = readBootstrapClass(state.classId, state.classDesired,
		state.classDesiredKnown, classPropertyName);

	return true;
}

bool CBotEmpires::readEmpiresClass(const edict_t* pEdict, int& classId,
	bool& classDesired, bool& classDesiredKnown, const char*& propertyName) const
{
	classId = -1;
	classDesired = false;
	classDesiredKnown = false;
	propertyName = nullptr;

	edict_t* pCurrentEdict = const_cast<edict_t*>(pEdict);
	if (pCurrentEdict == nullptr || servergamedll == nullptr)
		return false;

	const ServerClass* playerClass = UTIL_FindServerClass("CEmpPlayer");
	if (playerClass == nullptr || playerClass->m_pTable == nullptr)
		return false;

	// The property name is selected from the current public CEmpPlayer
	// SendTable. No binary offset or historical layout value is used.
	constexpr const char* propertyCandidates[] = { "m_iEmpClass", "m_iClass" };
	bool classReadable = false;
	for (const char* candidate : propertyCandidates)
	{
		SendProp* prop = UTIL_FindSendProp(playerClass->m_pTable, candidate);
		if (prop == nullptr || prop->GetType() != DPT_Int || prop->GetNumElements() != 1)
			continue;

		unsigned offset = 0;
		if (!UTIL_FindSendPropInfo(playerClass, candidate, &offset) || offset == 0)
			continue;

		CClassInterfaceValue value("CEmpPlayer", candidate, 0);
		value.findOffset();
		int* currentClass = value.getIntPointer(pCurrentEdict);
		if (currentClass == nullptr)
			continue;

		classId = *currentClass;
		propertyName = candidate;
		classReadable = true;
		break;
	}

	// Empires exposes whether a class change is pending separately from the
	// currently applied class. It is read only when the current public table
	// contains the verified scalar SendProp.
	SendProp* desiredProp = UTIL_FindSendProp(playerClass->m_pTable, "m_bEmpClassDesired");
	if (desiredProp != nullptr && desiredProp->GetType() == DPT_Int &&
		desiredProp->GetNumElements() == 1)
	{
		unsigned desiredOffset = 0;
		if (UTIL_FindSendPropInfo(playerClass, "m_bEmpClassDesired", &desiredOffset) && desiredOffset != 0)
		{
			CClassInterfaceValue desiredValue("CEmpPlayer", "m_bEmpClassDesired", 0);
			desiredValue.findOffset();
			bool* desired = desiredValue.getBoolPointer(pCurrentEdict);
			if (desired != nullptr)
			{
				classDesired = *desired;
				classDesiredKnown = true;
			}
		}
	}

	return classReadable;
}

bool CBotEmpires::readBootstrapClass(int& classId, bool& classDesired,
	bool& classDesiredKnown, const char*& propertyName) const
{
	return readEmpiresClass(getCurrentBootstrapEdict(), classId, classDesired,
		classDesiredKnown, propertyName);
}

bool CBotEmpires::requestBootstrapTeam(const int requestedTeam)
{
	if (requestedTeam != EMP_TEAM_NF && requestedTeam != EMP_TEAM_BE)
	{
		return false;
	}

	if (!m_bBootstrapOnly || m_bTeamCommandPending)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap jointeam rejected: bootstrap inactive or another team request is pending");
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before))
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap jointeam rejected: current client state is unavailable");
		return false;
	}

	if (before.team == requestedTeam)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap jointeam rejected: index=%d userid=%d already on team=%d",
			before.index, before.userId, before.team);
		return false;
	}

	edict_t* pCurrentEdict = INDEXENT(before.index);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree())
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap jointeam rejected: index=%d edict became invalid before command",
			before.index);
		return false;
	}

	logger->Log(LogLevel::INFO,
		"Empires bootstrap jointeam before index=%d userid=%d requested_team=%d team_before=%d command=jointeam %d connected=%d active=%d dead=%d alive=%d",
		before.index, before.userId, requestedTeam, before.team, requestedTeam,
		before.connected ? 1 : 0, before.active ? 1 : 0, before.dead ? 1 : 0,
		before.alive ? 1 : 0);

	// This is the public SDK path used by a real client typing the Empires
	// command. No class or spawn command is issued here.
	if (helpers == nullptr)
	{
		logger->Log(LogLevel::ERROR, "Empires bootstrap jointeam rejected: server plugin helpers are unavailable");
		return false;
	}

	char command[32];
	std::snprintf(command, sizeof(command), "jointeam %d\n", requestedTeam);
	helpers->ClientCommand(pCurrentEdict, command);

	m_bTeamCommandPending = true;
	m_iPendingTeam = requestedTeam;
	m_iPendingTeamFrames = 0;
	m_flPendingTeamDeadline = engine->Time() + 2.0f;

	EmpiresBootstrapPlayerState after;
	if (readBootstrapPlayerState(after))
	{
		logger->Log(LogLevel::INFO,
			"Empires bootstrap jointeam after-dispatch index=%d userid=%d requested_team=%d team_after=%d connected=%d active=%d dead=%d alive=%d observation=pending",
			after.index, after.userId, requestedTeam, after.team, after.connected ? 1 : 0,
			after.active ? 1 : 0, after.dead ? 1 : 0, after.alive ? 1 : 0);

		// Empires can hibernate before delivering another GameFrame. The
		// public team readback is nevertheless already authoritative here.
		if (!after.alive && after.team == requestedTeam)
		{
			logger->Log(LogLevel::INFO,
				"Empires bootstrap jointeam observed index=%d userid=%d requested_team=%d team_after=%d connected=%d active=%d dead=%d alive=%d observation=dispatch-readback",
				after.index, after.userId, requestedTeam, after.team, after.connected ? 1 : 0,
				after.active ? 1 : 0, after.dead ? 1 : 0, after.alive ? 1 : 0);
			m_bTeamCommandPending = false;
			m_iPendingTeam = -1;
		}
	}
	else
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap jointeam after-dispatch state unavailable; observation=pending");
	}

	return true;
}

void CBotEmpires::observePendingTeamChange()
{
	if (!m_bTeamCommandPending)
	{
		return;
	}

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state))
	{
		if (engine == nullptr || engine->Time() < m_flPendingTeamDeadline)
			return;

		logger->Log(LogLevel::ERROR,
			"Empires bootstrap jointeam observation failed: client state unavailable requested_team=%d",
			m_iPendingTeam);
		m_bTeamCommandPending = false;
		m_iPendingTeam = -1;
		return;
	}

	++m_iPendingTeamFrames;
	const bool enoughSamples = m_iPendingTeamFrames >= 2;
	const bool deadlineReached = engine != nullptr && engine->Time() >= m_flPendingTeamDeadline;
	if (state.team == m_iPendingTeam && enoughSamples)
	{
		logger->Log(LogLevel::INFO,
			"Empires bootstrap jointeam observed index=%d userid=%d requested_team=%d team_after=%d connected=%d active=%d dead=%d alive=%d observation=success frames=%d",
			state.index, state.userId, m_iPendingTeam, state.team, state.connected ? 1 : 0,
			state.active ? 1 : 0, state.dead ? 1 : 0, state.alive ? 1 : 0,
			m_iPendingTeamFrames);
		m_bTeamCommandPending = false;
		m_iPendingTeam = -1;
		return;
	}

	if (deadlineReached)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap jointeam observed index=%d userid=%d requested_team=%d team_after=%d connected=%d active=%d dead=%d alive=%d observation=failed frames=%d",
			state.index, state.userId, m_iPendingTeam, state.team, state.connected ? 1 : 0,
			state.active ? 1 : 0, state.dead ? 1 : 0, state.alive ? 1 : 0,
			m_iPendingTeamFrames);
		m_bTeamCommandPending = false;
		m_iPendingTeam = -1;
	}
}

bool CBotEmpires::requestBootstrapClass(const int requestedClass)
{
	if (requestedClass < EMP_CLASS_SCOUT || requestedClass > EMP_CLASS_ENGINEER)
		return false;

	if (!m_bBootstrapOnly || m_bClassCommandPending)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap changeclass rejected: bootstrap inactive or another class request is pending");
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before))
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap changeclass rejected: current client state is unavailable");
		return false;
	}

	if (before.team != EMP_TEAM_NF && before.team != EMP_TEAM_BE)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap changeclass rejected: client must already be on NF or BE team=%d",
			before.team);
		return false;
	}

	const char* propertyName = nullptr;
	int classBefore = -1;
	bool classDesiredBefore = false;
	bool classDesiredKnownBefore = false;
	const bool classReadable = readBootstrapClass(classBefore, classDesiredBefore,
		classDesiredKnownBefore, propertyName);
	if (classReadable && classBefore == requestedClass)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap changeclass rejected: index=%d userid=%d already has class=%d property=%s",
			before.index, before.userId, classBefore, propertyName);
		return false;
	}

	if (helpers == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap changeclass rejected: server plugin helpers are unavailable");
		return false;
	}

	logger->Log(LogLevel::INFO,
		"Empires bootstrap changeclass before index=%d userid=%d team=%d class_requested=%d class_before=%d class_readback=%d property=%s class_desired_before=%d class_desired_readback=%d command=changeclass %d alive=%d dead=%d",
		before.index, before.userId, before.team, requestedClass, classBefore,
		classReadable ? 1 : 0, propertyName != nullptr ? propertyName : "unavailable",
		classDesiredBefore ? 1 : 0, classDesiredKnownBefore ? 1 : 0,
		requestedClass, before.alive ? 1 : 0, before.dead ? 1 : 0);

	edict_t* pCurrentEdict = INDEXENT(before.index);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree())
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap changeclass rejected: index=%d edict became invalid before command",
			before.index);
		return false;
	}

	char command[32];
	std::snprintf(command, sizeof(command), "changeclass %d\n", requestedClass);
	helpers->ClientCommand(pCurrentEdict, command);

	m_bClassCommandPending = true;
	m_iPendingClass = requestedClass;
	m_iPendingClassFrames = 0;
	m_flPendingClassDeadline = engine->Time() + 2.0f;
	m_bPendingClassDesiredKnown = classDesiredKnownBefore;
	m_bPendingClassDesiredBefore = classDesiredBefore;

	EmpiresBootstrapPlayerState after;
	if (readBootstrapPlayerState(after))
	{
		logger->Log(LogLevel::INFO,
			"Empires bootstrap changeclass after-dispatch index=%d userid=%d team=%d class_after=%d class_readback=%d class_desired_after=%d class_desired_readback=%d alive=%d dead=%d observation=pending",
			after.index, after.userId, after.team, after.classId,
			after.classKnown ? 1 : 0, after.classDesired ? 1 : 0,
			after.classDesiredKnown ? 1 : 0, after.alive ? 1 : 0, after.dead ? 1 : 0);

		// This is still only a public SendProp readback. It records the
		// requested class as accepted without calling any game-specific spawn
		// or revive routine when no simulated frame is available.
		if (!after.alive && ((after.classKnown && after.classId == requestedClass) ||
			(after.classDesiredKnown && after.classDesired)))
		{
			logger->Log(LogLevel::INFO,
				"Empires bootstrap changeclass observed index=%d userid=%d requested_class=%d class_after=%d class_readback=%d class_desired_after=%d class_desired_readback=%d alive=%d dead=%d observation=dispatch-readback",
				after.index, after.userId, requestedClass, after.classId,
				after.classKnown ? 1 : 0, after.classDesired ? 1 : 0,
				after.classDesiredKnown ? 1 : 0, after.alive ? 1 : 0, after.dead ? 1 : 0);
			m_bClassCommandPending = false;
			m_iSelectedBootstrapClass = requestedClass;
			m_iPendingClass = -1;
		}
	}
	else
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap changeclass after-dispatch state unavailable; observation=pending");
	}

	return true;
}

void CBotEmpires::observePendingClassChange()
{
	if (!m_bClassCommandPending)
		return;

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state))
	{
		if (engine == nullptr || engine->Time() < m_flPendingClassDeadline)
			return;

		logger->Log(LogLevel::ERROR,
			"Empires bootstrap changeclass observation failed: client state unavailable requested_class=%d",
			m_iPendingClass);
		m_bClassCommandPending = false;
		m_iPendingClass = -1;
		m_iSelectedBootstrapClass = -1;
		return;
	}

	++m_iPendingClassFrames;
	if (state.alive)
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap changeclass observation stopped: automatic spawn detected index=%d userid=%d requested_class=%d class_after=%d frames=%d",
			state.index, state.userId, m_iPendingClass, state.classId, m_iPendingClassFrames);
		m_bClassCommandPending = false;
		m_iSelectedBootstrapClass = -1;
		m_iPendingClass = -1;
		return;
	}

	const bool enoughSamples = m_iPendingClassFrames >= 2;
	const bool deadlineReached = engine != nullptr && engine->Time() >= m_flPendingClassDeadline;
	const bool appliedClassReadback = state.classKnown && state.classId == m_iPendingClass;
	// While the bootstrap client is dead, Empires keeps m_iEmpClass at its
	// not-yet-applied value and acknowledges the requested class through the
	// public m_bEmpClassDesired flag. Treat that state as accepted, but keep
	// the alive check above so this path can never silently spawn the client.
	const bool desiredClassReadback = state.classDesiredKnown && state.classDesired;
	if ((appliedClassReadback || desiredClassReadback) && enoughSamples)
	{
		logger->Log(LogLevel::INFO,
			"Empires bootstrap changeclass observed index=%d userid=%d requested_class=%d class_after=%d class_readback=%d class_desired_after=%d class_desired_readback=%d alive=%d dead=%d observation=success frames=%d",
			state.index, state.userId, m_iPendingClass, state.classId,
			state.classKnown ? 1 : 0, state.classDesired ? 1 : 0,
			state.classDesiredKnown ? 1 : 0, state.alive ? 1 : 0, state.dead ? 1 : 0,
			m_iPendingClassFrames);
		m_bClassCommandPending = false;
		m_iPendingClass = -1;
		return;
	}

	if (deadlineReached)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap changeclass observed index=%d userid=%d requested_class=%d class_after=%d class_readback=%d class_desired_after=%d class_desired_readback=%d alive=%d dead=%d observation=%s frames=%d",
			state.index, state.userId, m_iPendingClass, state.classId,
			state.classKnown ? 1 : 0, state.classDesired ? 1 : 0,
			state.classDesiredKnown ? 1 : 0, state.alive ? 1 : 0, state.dead ? 1 : 0,
			state.classKnown ? "failed" : "unavailable", m_iPendingClassFrames);
		m_bClassCommandPending = false;
		m_iPendingClass = -1;
		m_iSelectedBootstrapClass = -1;
	}
}

bool CBotEmpires::resolveBootstrapSpawnId(const int team, int& spawnId, edict_t*& spawnEdict) const
{
	spawnId = -1;
	spawnEdict = nullptr;
	if (team != EMP_TEAM_NF && team != EMP_TEAM_BE)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawn discovery rejected: unsupported team=%d", team);
		return false;
	}

	if (servertools == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawn discovery unavailable: public IServerTools is null");
		return false;
	}

	// The live CEmpPlayer::GetDesiredSpawn() logic uses these lowercase names.
	// Enumeration is case-insensitive because report_entities canonicalizes them.
	const char* classname = team == EMP_TEAM_NF ? "emp_info_player_nf" : "emp_info_player_imp";
	int configuredSpawnId = -1;
	const bool hasConfiguredSpawnId = readConfiguredEmpiresSpawnId(team, configuredSpawnId);
	logger->Log(LogLevel::INFO,
		"Empires bootstrap spawn selection map=%s team=%d configured=%d spawn_id=%d",
		gpGlobals != nullptr ? gpGlobals->mapname.ToCStr() : "unavailable", team,
		hasConfiguredSpawnId ? 1 : 0, configuredSpawnId);
	if (!hasConfiguredSpawnId)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawn selection has no map/team override; using first live public candidate as fallback map=%s team=%d",
			gpGlobals != nullptr ? gpGlobals->mapname.ToCStr() : "unavailable", team);
	}
	CBaseEntity* pEntity = servertools->FirstEntity();
	if (pEntity == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawn discovery unavailable: public IServerTools::FirstEntity returned null team=%d classname=%s",
			team, classname);
		return false;
	}

	int candidateCount = 0;
	int selectedId = -1;
	edict_t* selectedEdict = nullptr;
	bool selectedEnabled = false;
	int selectedFrontScore = 1 << 20;

	// Front line: spawn groups share pointNumber with capture points, and on
	// linear maps the numbers follow the front (as_escort: BE holds 6, NF 1-5, NF
	// must spawn at 5). Score = numeric gap to the nearest point this team does
	// not own; groups without a capture point keep the old first-found order.
	std::vector<int> foreignPoints;
	std::vector<int> knownPoints;
	if (const CBotMod* mod = CBotGlobals::getCurrentMod(); mod != nullptr)
		for (const CEmpiresMod::CapturePointObservation& point :
			static_cast<const CEmpiresMod*>(mod)->getCapturePoints())
		{
			if (!point.pointIndexKnown)
				continue;
			knownPoints.push_back(point.pointIndex);
			if (point.teamOwnerKnown && point.teamOwner != team)
				foreignPoints.push_back(point.pointIndex);
		}
	auto spawnFrontScore = [&](const int id)
	{
		if (foreignPoints.empty() || std::find(knownPoints.begin(), knownPoints.end(), id) == knownPoints.end())
			return 1 << 19;
		int best = 1 << 19;
		for (const int foreign : foreignPoints)
			best = std::min(best, std::abs(foreign - id));
		return best;
	};

	while (pEntity != nullptr)
	{
		IServerUnknown* pUnknown = reinterpret_cast<IServerUnknown*>(pEntity);
		IServerNetworkable* pNetworkable = pUnknown != nullptr ? pUnknown->GetNetworkable() : nullptr;
		edict_t* pCandidateEdict = pNetworkable != nullptr ? pNetworkable->GetEdict() : nullptr;
		const char* candidateClassname = pNetworkable != nullptr ? pNetworkable->GetClassName() : nullptr;
		const bool classMatches = candidateClassname != nullptr &&
			equalsIgnoreCase(candidateClassname, classname);
		char rawSpawnId[32] = {};
		const bool hasPointNumber = classMatches && servertools->GetKeyValue(pEntity, "pointNumber",
			rawSpawnId, sizeof(rawSpawnId));
		int candidateId = -1;
		const bool validSpawnPoint = classMatches && hasPointNumber &&
			parseSpawnPointId(rawSpawnId, candidateId);

		// CEmpSpawn::SetEnabled (0x67fd70) keeps the enabled flag at +0x354 next to
		// pointNumber at +0x358. Maps switch spawn groups by I/O (as_escort BE: groups
		// 2-6, the first one in entity order is off at start and the spawn failed).
		const bool enabled = validSpawnPoint &&
			(!EmpiresBinaryOffsetsTrusted() || // unverified offset: treat every spawn as enabled
				*(reinterpret_cast<const unsigned char*>(pEntity) + 0x354) != 0);
		logger->Log(LogLevel::INFO,
			"Empires bootstrap spawn discovery team=%d classname=%s candidate_class=%s index=%d pointNumber=%s valid=%d enabled=%d",
			team, classname, candidateClassname != nullptr ? candidateClassname : "unavailable",
			pCandidateEdict != nullptr && engine != nullptr ? engine->IndexOfEdict(pCandidateEdict) : -1,
			hasPointNumber ? rawSpawnId : "unavailable", validSpawnPoint ? 1 : 0, enabled ? 1 : 0);

		const bool matchesConfiguredSpawn = !hasConfiguredSpawnId || candidateId == configuredSpawnId;
		if (validSpawnPoint && matchesConfiguredSpawn && pCandidateEdict != nullptr && !pCandidateEdict->IsFree())
		{
			++candidateCount;
			const int frontScore = spawnFrontScore(candidateId);
			if (selectedEdict == nullptr || (enabled && !selectedEnabled) ||
				(enabled == selectedEnabled && frontScore < selectedFrontScore))
			{
				selectedId = candidateId;
				selectedEdict = pCandidateEdict;
				selectedEnabled = enabled;
				selectedFrontScore = frontScore;
			}
		}

		pEntity = servertools->NextEntity(pEntity);
	}

	if (candidateCount == 0 || selectedEdict == nullptr)
	{
		if (hasConfiguredSpawnId)
		{
			logger->Log(LogLevel::ERROR,
				"Empires bootstrap spawn selection configured point not live map=%s team=%d spawn_id=%d",
				gpGlobals != nullptr ? gpGlobals->mapname.ToCStr() : "unavailable", team,
				configuredSpawnId);
		}
		return false;
	}

	spawnId = selectedId;
	spawnEdict = selectedEdict;
	logger->Log(LogLevel::INFO,
		"Empires bootstrap spawn selection result map=%s team=%d spawn_id=%d source=%s enabled=%d front_score=%d cap_points=%zu foreign=%zu",
		gpGlobals != nullptr ? gpGlobals->mapname.ToCStr() : "unavailable", team, spawnId,
		hasConfiguredSpawnId ? "config" : "first-live-fallback", selectedEnabled ? 1 : 0,
		selectedFrontScore, knownPoints.size(), foreignPoints.size());
	return true;
}

bool CBotEmpires::requestBootstrapSpawn()
{
	if (!m_bBootstrapOnly || m_bTeamCommandPending || m_bClassCommandPending || m_bSpawnCommandPending ||
		m_eSpawnConfirmationState != EmpiresBootstrapSpawnConfirmationState::IDLE)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawn rejected: bootstrap inactive or team/class/spawn request is pending");
		return false;
	}

	if (m_iSelectedBootstrapClass < EMP_CLASS_SCOUT || m_iSelectedBootstrapClass > EMP_CLASS_ENGINEER)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawn rejected: no verified infantry class request is active");
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) || before.edict == nullptr)
	{
		logger->Log(LogLevel::ERROR, "Empires bootstrap spawn rejected: current client state is unavailable");
		return false;
	}

	if (before.team != EMP_TEAM_NF && before.team != EMP_TEAM_BE)
	{
		logger->Log(LogLevel::WARN, "Empires bootstrap spawn rejected: client must already be on NF or BE team=%d",
			before.team);
		return false;
	}

	if (before.alive || !before.dead)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawn rejected: client is not dead index=%d userid=%d alive=%d dead=%d",
			before.index, before.userId, before.alive ? 1 : 0, before.dead ? 1 : 0);
		return false;
	}

	if (!before.classDesiredKnown || !before.classDesired)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawn rejected: requested class is not acknowledged index=%d class=%d desired_known=%d desired=%d",
			before.index, m_iSelectedBootstrapClass, before.classDesiredKnown ? 1 : 0,
			before.classDesired ? 1 : 0);
		return false;
	}

	if (!before.desiredSpawnKnown)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawn rejected: public m_iDesiredSpawn state is unavailable index=%d",
			before.index);
		return false;
	}

	if (before.desiredSpawn != -1)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawn rejected: Empires already has a pending desired spawn index=%d desired_spawn=%d",
			before.index, before.desiredSpawn);
		return false;
	}

	int spawnId = -1;
	edict_t* pSpawnEdict = nullptr;
	if (!resolveBootstrapSpawnId(before.team, spawnId, pSpawnEdict))
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawn rejected: no live public spawnPoint was discoverable for team=%d",
			before.team);
		return false;
	}

	if (helpers == nullptr)
	{
		logger->Log(LogLevel::ERROR, "Empires bootstrap spawn rejected: server plugin helpers are unavailable");
		return false;
	}

	logger->Log(LogLevel::INFO,
		"Empires bootstrap spawn before index=%d userid=%d edict=%p team=%d class=%d desired_spawn=%d spawn_id=%d spawn_edict=%p health=%d dead=%d alive=%d command=emp_spawn_select %d",
		before.index, before.userId, static_cast<const void*>(before.edict), before.team,
		m_iSelectedBootstrapClass, before.desiredSpawn, spawnId,
		static_cast<void*>(pSpawnEdict), before.health, before.dead ? 1 : 0, before.alive ? 1 : 0, spawnId);

	char command[64];
	std::snprintf(command, sizeof(command), "emp_spawn_select %d\n", spawnId);
	helpers->ClientCommand(const_cast<edict_t*>(before.edict), command);

	m_bSpawnCommandPending = true;
	m_iPendingSpawn = spawnId;
	m_iPendingSpawnFrames = 0;
	m_flPendingSpawnDeadline = engine != nullptr ? engine->Time() + 4.0f : 0.0f;
	m_vSpawnOriginBefore = before.origin;
	m_bSpawnOriginBeforeKnown = before.originKnown;
	m_vSpawnOriginAfter = Vector();
	m_bSpawnOriginAfterKnown = false;
	m_iSpawnStableSamples = 0;

	EmpiresBootstrapPlayerState after;
	if (readBootstrapPlayerState(after))
	{
		logger->Log(LogLevel::INFO,
			"Empires bootstrap spawn after-dispatch index=%d userid=%d edict=%p team=%d class=%d desired_spawn=%d health=%d dead=%d alive=%d observation=pending",
			after.index, after.userId, static_cast<const void*>(after.edict), after.team,
			after.classId, after.desiredSpawn, after.health, after.dead ? 1 : 0, after.alive ? 1 : 0);
	}

	return true;
}

bool CBotEmpires::requestBootstrapSpawnConfirmation()
{
	if (!m_bBootstrapOnly || m_bTeamCommandPending || m_bClassCommandPending ||
		m_eSpawnConfirmationState != EmpiresBootstrapSpawnConfirmationState::IDLE)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawnconfirm rejected: bootstrap inactive, team/class request pending, or confirmation already used state=%s",
			spawnConfirmationStateName(m_eSpawnConfirmationState));
		return false;
	}

	if (!m_bSpawnCommandPending || m_iPendingSpawn < 0)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawnconfirm rejected: no pending verified spawn selection is available");
		return false;
	}

	if (m_iSelectedBootstrapClass < EMP_CLASS_SCOUT ||
		m_iSelectedBootstrapClass > EMP_CLASS_ENGINEER)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawnconfirm rejected: no verified infantry class request is active class=%d",
			m_iSelectedBootstrapClass);
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) || before.edict == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawnconfirm rejected: current client state is unavailable");
		return false;
	}

	if (before.team != EMP_TEAM_NF && before.team != EMP_TEAM_BE)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawnconfirm rejected: unsupported team=%d", before.team);
		return false;
	}

	// This object is only created by CBots::createEmpiresBootstrapBot(), which
	// uses engine->CreateFakeClient().  Empires' IPlayerInfo implementation
	// reports IsFakeClient()==false for that client after team assignment, so
	// the object provenance is the authoritative fake-client check here.
	if (!before.connected || !before.active || before.alive || !before.dead)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawnconfirm rejected: client is not connected/active/dead index=%d userid=%d connected=%d active=%d playerinfo_fake=%d bootstrap_fake=1 dead=%d alive=%d",
			before.index, before.userId, before.connected ? 1 : 0, before.active ? 1 : 0,
			before.fakeClient ? 1 : 0, before.dead ? 1 : 0, before.alive ? 1 : 0);
		return false;
	}

	if (!before.classDesiredKnown || !before.classDesired ||
		before.desiredSpawn != m_iPendingSpawn)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawnconfirm rejected: requested state not acknowledged index=%d class=%d class_desired_known=%d class_desired=%d desired_spawn=%d selected_spawn=%d",
			before.index, before.classId, before.classDesiredKnown ? 1 : 0,
			before.classDesired ? 1 : 0, before.desiredSpawn, m_iPendingSpawn);
		return false;
	}

	if (before.index <= 0 || before.userId <= 0 || before.edict->IsFree())
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawnconfirm rejected: invalid client identity index=%d userid=%d edict=%p",
			before.index, before.userId, static_cast<const void*>(before.edict));
		return false;
	}

	m_eSpawnConfirmationState = EmpiresBootstrapSpawnConfirmationState::ATTACK_PRESS_PENDING;
	m_iSpawnConfirmationFrames = 0;
	m_iSpawnConfirmationPressCount = 0;
	m_iSpawnConfirmationReleaseCount = 0;
	m_iSpawnConfirmationExpectedIndex = before.index;
	m_iSpawnConfirmationExpectedUserId = before.userId;
	m_pSpawnConfirmationExpectedEdict = before.edict;
	m_vSpawnConfirmationOrigin = before.origin;
	m_bSpawnConfirmationOriginKnown = before.originKnown;
	m_iSpawnConfirmationStableSamples = 0;

	logger->Log(LogLevel::INFO,
		"Empires bootstrap spawnconfirm armed state=%s index=%d userid=%d edict=%p team=%d class=%d selected_spawn=%d health=%d dead=%d alive=%d lastcmd_known=%d lastcmd_number=%d lastcmd_tick=%d lastcmd_buttons=0x%x",
		spawnConfirmationStateName(m_eSpawnConfirmationState), before.index, before.userId,
		static_cast<const void*>(before.edict), before.team, m_iSelectedBootstrapClass,
		m_iPendingSpawn, before.health, before.dead ? 1 : 0, before.alive ? 1 : 0,
		before.lastUserCommandKnown ? 1 : 0, before.lastUserCommandNumber,
		before.lastUserCommandTick, before.lastUserCommandButtons);
	return true;
}

bool CBotEmpires::submitNormalSpawnUserCmd(const int buttons, const char* phase)
{
	if (m_pEdict == nullptr || m_pEdict->IsFree() || gpGlobals == nullptr ||
		playerinfomanager == nullptr || g_pBotManager == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires normal spawnconfirm %s rejected: bot input interfaces unavailable",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(m_pEdict);
	IBotController* pController = g_pBotManager->GetBotController(m_pEdict);
	if (pInfo == nullptr || pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires normal spawnconfirm %s rejected: PlayerInfo=%s controller=%s",
			phase != nullptr ? phase : "unknown", pInfo != nullptr ? "available" : "null",
			pController != nullptr ? "available" : "null");
		return false;
	}

	const CBotCmd lastCommand = pInfo->GetLastUserCommand();
	const int commandNumber = lastCommand.command_number > 0 ?
		lastCommand.command_number + 1 : 1;

	CBotCmd userCommand;
	userCommand.command_number = commandNumber;
	userCommand.tick_count = gpGlobals->tickcount;
	// Match the already successful controller-probe spawn path: preserve the
	// actual last public view angles rather than the normal AI's cached angles.
	userCommand.viewangles = lastCommand.viewangles;
	userCommand.forwardmove = 0.0f;
	userCommand.sidemove = 0.0f;
	userCommand.upmove = 0.0f;
	userCommand.buttons = buttons;
	userCommand.impulse = 0;
	userCommand.weaponselect = 0;
	userCommand.weaponsubtype = 0;
	userCommand.random_seed = 0;
	userCommand.mousedx = 0;
	userCommand.mousedy = 0;
	userCommand.hasbeenpredicted = false;

	logger->Log(LogLevel::INFO,
		"Empires normal spawnconfirm submit phase=%s index=%d userid=%d edict=%p command_number=%d tick_count=%d buttons=0x%x forwardmove=0.0 sidemove=0.0 upmove=0.0",
		phase != nullptr ? phase : "unknown", engine->IndexOfEdict(m_pEdict),
		engine->GetPlayerUserId(m_pEdict), static_cast<void*>(m_pEdict),
		userCommand.command_number, userCommand.tick_count, userCommand.buttons);

	// Use the SDK2013 public bot-input interface. Normal bot movement is
	// suppressed by suppressBotThink() for the entire spawn state machine, so
	// this command is the only input submitted in these confirmation frames.
	pController->RunPlayerMove(&userCommand);
	cmd.Reset();
	cmd.command_number = commandNumber;
	cmd.viewangles = userCommand.viewangles;
	int entityButtons = -1;
	const bool entityButtonsKnown =
		readEmpiresEntityIntProperty(m_pEdict, "m_nButtons", entityButtons);
	int entityButtonsLast = -1;
	const bool entityButtonsLastKnown =
		readEmpiresEntityIntProperty(m_pEdict, "m_afButtonLast", entityButtonsLast);

	int afterLifeState = -1;
	const bool afterLifeStateKnown = readEmpiresIntProperty(
		m_pEdict, "m_lifeState", afterLifeState);
	int afterHealth = -1;
	IPlayerInfo* afterInfo = playerinfomanager->GetPlayerInfo(m_pEdict);
	const bool afterHealthKnown = afterInfo != nullptr;
	CBotCmd afterCommand;
	if (afterInfo != nullptr)
	{
		afterHealth = afterInfo->GetHealth();
		afterCommand = afterInfo->GetLastUserCommand();
	}
	logger->Log(LogLevel::INFO,
		"Empires normal spawnconfirm after phase=%s index=%d userid=%d life_state=%d life_state_known=%d health=%d lastcmd_number=%d lastcmd_tick=%d lastcmd_buttons=0x%x entity_buttons=0x%x entity_buttons_known=%d entity_buttons_last=0x%x entity_buttons_last_known=%d lastcmd_forward=%.1f lastcmd_side=%.1f lastcmd_up=%.1f viewangles=(%.1f %.1f %.1f)",
		phase != nullptr ? phase : "unknown", engine->IndexOfEdict(m_pEdict),
		engine->GetPlayerUserId(m_pEdict), afterLifeState, afterLifeStateKnown ? 1 : 0,
		afterHealth, afterCommand.command_number, afterCommand.tick_count,
		afterCommand.buttons, entityButtonsKnown ? entityButtons : -1,
		entityButtonsKnown ? 1 : 0, entityButtonsLastKnown ? entityButtonsLast : -1,
		entityButtonsLastKnown ? 1 : 0, afterCommand.forwardmove, afterCommand.sidemove,
		afterCommand.upmove, afterCommand.viewangles.x, afterCommand.viewangles.y,
		afterCommand.viewangles.z);
	EmpiresBootstrapPlayerState afterState;
	if (readBootstrapPlayerState(afterState))
	{
		logger->Log(LogLevel::INFO,
			"Empires normal spawnconfirm after-state phase=%s team=%d class=%d class_desired=%d sidearm=%d sidearm_desired=%d primary=%d primary_desired=%d grenade=%d grenade_desired=%d equipment=%d equipment_desired=%d desired_spawn=%d life_state=%d health=%d",
			phase != nullptr ? phase : "unknown", afterState.team, afterState.classId,
			afterState.classDesired ? 1 : 0, afterState.appliedSidearm,
			afterState.sidearmDesired ? 1 : 0, afterState.appliedPrimary,
			afterState.primaryDesired ? 1 : 0, afterState.appliedGrenade,
			afterState.grenadeDesired ? 1 : 0, afterState.appliedEquipment,
			afterState.equipmentDesired ? 1 : 0, afterState.desiredSpawn,
			afterState.lifeState, afterState.health);
	}
	return true;
}

void CBotEmpires::observeNormalSpawn()
{
	if (m_eNormalSpawnState == EmpiresNormalSpawnState::IDLE ||
		m_eNormalSpawnState == EmpiresNormalSpawnState::COMPLETE)
	{
		return;
	}

	if (m_pEdict == nullptr || m_pEdict->IsFree() || engine == nullptr ||
		gpGlobals == nullptr || playerinfomanager == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires normal spawn state=%s result=FAILED reason=client identity unavailable",
			normalSpawnStateName(m_eNormalSpawnState));
		m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
		return;
	}

	const int index = engine->IndexOfEdict(m_pEdict);
	const int userId = engine->GetPlayerUserId(m_pEdict);
	if ((m_iNormalSpawnExpectedIndex > 0 && index != m_iNormalSpawnExpectedIndex) ||
		(m_iNormalSpawnExpectedUserId > 0 && userId != m_iNormalSpawnExpectedUserId) ||
		(m_pNormalSpawnExpectedEdict != nullptr && m_pNormalSpawnExpectedEdict != m_pEdict))
	{
		logger->Log(LogLevel::ERROR,
			"Empires normal spawn state=%s result=FAILED reason=client identity changed index=%d expected_index=%d userid=%d expected_userid=%d edict=%p expected_edict=%p",
			normalSpawnStateName(m_eNormalSpawnState), index, m_iNormalSpawnExpectedIndex,
			userId, m_iNormalSpawnExpectedUserId, static_cast<void*>(m_pEdict),
			static_cast<const void*>(m_pNormalSpawnExpectedEdict));
		m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
		return;
	}

	int lifeState = -1;
	const bool lifeStateKnown = readEmpiresIntProperty(m_pEdict, "m_lifeState", lifeState);
	int desiredSpawn = -1;
	const bool desiredSpawnKnown = readEmpiresIntProperty(m_pEdict, "m_iDesiredSpawn", desiredSpawn);
	float respawnTimer = 0.0f;
	const bool respawnTimerKnown = readEmpiresFloatProperty(m_pEdict,
		"m_flTimeToRespawn", respawnTimer);

	if (lifeStateKnown && (!m_bNormalSpawnLastLifeStateKnown ||
		m_iNormalSpawnLastLifeState != lifeState))
	{
		logger->Log(LogLevel::INFO,
			"Empires normal spawn transition index=%d userid=%d state=%s life_state=%d life_state_name=%s desired_spawn=%d timer=%.3f health=%d",
			index, userId, normalSpawnStateName(m_eNormalSpawnState), lifeState,
			lifeStateName(lifeState), desiredSpawnKnown ? desiredSpawn : -1,
			respawnTimerKnown ? respawnTimer : 0.0f,
			playerinfomanager->GetPlayerInfo(m_pEdict)->GetHealth());
		m_bNormalSpawnLastLifeStateKnown = true;
		m_iNormalSpawnLastLifeState = lifeState;
	}

	if (respawnTimerKnown && (!m_bNormalSpawnLastTimerKnown ||
		std::fabs(m_flNormalSpawnLastTimer - respawnTimer) > 0.0005f))
	{
		logger->Log(LogLevel::INFO,
			"Empires normal spawn timer index=%d userid=%d old=%.3f new=%.3f state=%s",
			index, userId, m_bNormalSpawnLastTimerKnown ? m_flNormalSpawnLastTimer : -1.0f,
			respawnTimer, normalSpawnStateName(m_eNormalSpawnState));
		m_bNormalSpawnLastTimerKnown = true;
		m_flNormalSpawnLastTimer = respawnTimer;
	}

	const int health = playerinfomanager->GetPlayerInfo(m_pEdict)->GetHealth();
	int classId = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	const bool classKnown = readEmpiresClass(m_pEdict, classId, classDesired,
		classDesiredKnown, classPropertyName);
	const bool classMatches = classKnown && classId == m_iDesiredClass;
	const bool alive = lifeStateKnown && lifeState == LIFE_ALIVE && health > 0 &&
		classMatches && !playerinfomanager->GetPlayerInfo(m_pEdict)->IsDead();

	++m_iNormalSpawnFrames;

	if (m_eNormalSpawnState == EmpiresNormalSpawnState::SPAWNPOINT_REQUESTED)
	{
		if (alive)
		{
			logNormalSpawnInventory();
			logger->Log(LogLevel::INFO,
				"Empires normal spawn result=COMPLETE reason=automatic-spawn index=%d userid=%d team=%d class=%d property=%s desired_spawn=%d health=%d life_state=%d",
				index, userId, m_iDesiredTeam, classId,
				classPropertyName != nullptr ? classPropertyName : "unavailable",
				desiredSpawnKnown ? desiredSpawn : -1, health, lifeState);
			m_eNormalSpawnState = EmpiresNormalSpawnState::COMPLETE;
			return;
		}

		if (desiredSpawnKnown && desiredSpawn == m_iNormalSpawn)
		{
			m_eNormalSpawnState = EmpiresNormalSpawnState::WAITING_FOR_RESPAWNABLE;
			m_iNormalSpawnFrames = 0;
			m_flNormalSpawnDeadline = engine->Time() + kNormalSpawnReadinessTimeout;
			logger->Log(LogLevel::INFO,
				"Empires normal spawn transition state=%s index=%d userid=%d selected_spawn=%d desired_spawn=%d life_state=%d life_state_name=%s timer=%.3f",
				normalSpawnStateName(m_eNormalSpawnState), index, userId, m_iNormalSpawn,
				desiredSpawn, lifeState, lifeStateName(lifeState),
				respawnTimerKnown ? respawnTimer : 0.0f);
			return;
		}

		if (engine->Time() >= m_flNormalSpawnDeadline)
		{
			logger->Log(LogLevel::ERROR,
				"Empires normal spawn result=FAILED reason=spawn-selection-not-acknowledged index=%d userid=%d selected_spawn=%d desired_spawn=%d desired_spawn_known=%d frames=%d",
				index, userId, m_iNormalSpawn, desiredSpawnKnown ? desiredSpawn : -1,
				desiredSpawnKnown ? 1 : 0, m_iNormalSpawnFrames);
			m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
		}
		return;
	}

	if (m_eNormalSpawnState == EmpiresNormalSpawnState::WAITING_FOR_RESPAWNABLE)
	{
		if (alive)
		{
			logNormalSpawnInventory();
			logger->Log(LogLevel::INFO,
				"Empires normal spawn result=COMPLETE reason=automatic-spawn index=%d userid=%d class=%d desired_spawn=%d health=%d life_state=%d",
				index, userId, classId, desiredSpawnKnown ? desiredSpawn : -1, health, lifeState);
			m_eNormalSpawnState = EmpiresNormalSpawnState::COMPLETE;
			return;
		}

		if (lifeStateKnown && lifeState == LIFE_RESPAWNABLE)
		{
			m_eNormalSpawnState = EmpiresNormalSpawnState::ATTACK_FRAME_1;
			m_flNormalSpawnConfirmEarliestTime = engine->Time() +
				kNormalSpawnConfirmDelaySeconds;
			logger->Log(LogLevel::INFO,
				"Empires normal spawn transition state=%s index=%d userid=%d life_state=%d life_state_name=%s selected_spawn=%d timer=%.3f confirm_delay_until=%.3f",
				normalSpawnStateName(m_eNormalSpawnState), index, userId, lifeState,
				lifeStateName(lifeState), m_iNormalSpawn,
				respawnTimerKnown ? respawnTimer : 0.0f,
				m_flNormalSpawnConfirmEarliestTime);
			return;
		}

		// Empires advances the normal fake client's death/respawn lifecycle
		// when it receives the same neutral UserCmd stream used by the verified
		// controller path. This remains the only input owner while waiting; no
		// movement, combat or navigation input is introduced.
		if (!submitNormalSpawnUserCmd(0, "respawn-wait-neutral"))
		{
			logger->Log(LogLevel::ERROR,
				"Empires normal spawn result=FAILED reason=respawn-wait-neutral-input-failed index=%d",
				index);
			m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
			return;
		}

		if (engine->Time() >= m_flNormalSpawnDeadline)
		{
			logger->Log(LogLevel::ERROR,
				"Empires normal spawn result=FAILED reason=respawnable-not-reached index=%d userid=%d selected_spawn=%d desired_spawn=%d health=%d life_state=%d life_state_known=%d timer=%.3f frames=%d",
				index, userId, m_iNormalSpawn, desiredSpawnKnown ? desiredSpawn : -1,
				health, lifeState, lifeStateKnown ? 1 : 0,
				respawnTimerKnown ? respawnTimer : 0.0f, m_iNormalSpawnFrames);
			m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
		}
		return;
	}

	if (m_eNormalSpawnState == EmpiresNormalSpawnState::ATTACK_FRAME_1 ||
		m_eNormalSpawnState == EmpiresNormalSpawnState::ATTACK_FRAME_2)
	{
		if (alive)
		{
			logNormalSpawnInventory();
			logger->Log(LogLevel::INFO,
				"Empires normal spawn result=COMPLETE reason=automatic-spawn-before-confirm index=%d userid=%d class=%d health=%d life_state=%d",
				index, userId, classId, health, lifeState);
			m_eNormalSpawnState = EmpiresNormalSpawnState::COMPLETE;
			return;
		}

		if (!lifeStateKnown || lifeState != LIFE_RESPAWNABLE)
		{
			logger->Log(LogLevel::ERROR,
				"Empires normal spawn result=FAILED reason=respawnable-state-lost-before-confirm index=%d userid=%d life_state=%d life_state_known=%d",
				index, userId, lifeState, lifeStateKnown ? 1 : 0);
			m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
			return;
		}

		// Empires consumes the attack edge on a later player-think pass.  The
		// previously verified '<' boundary must remain permissive, but an exact
		// engine-time equality can still submit one frame too early: a repeated
		// recovery run reached LIFE_RESPAWNABLE, accepted both attack frames and
		// then remained there until the observer timed out.  Wait one simulation
		// tick past the deadline so the command is processed on a subsequent
		// player-think pass without changing the public attack protocol.
		const float confirmTick =
			gpGlobals != nullptr && std::isfinite(gpGlobals->interval_per_tick) &&
			gpGlobals->interval_per_tick > 0.0f ? gpGlobals->interval_per_tick : 0.0f;
		if (engine->Time() < m_flNormalSpawnConfirmEarliestTime + confirmTick)
		{
			if (!submitNormalSpawnUserCmd(0, "respawn-ready-neutral"))
			{
				logger->Log(LogLevel::ERROR,
					"Empires normal spawn result=FAILED reason=respawn-ready-delay-input-failed index=%d userid=%d",
					index, userId);
				m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
			}
			return;
		}

		const bool firstAttackFrame =
			m_eNormalSpawnState == EmpiresNormalSpawnState::ATTACK_FRAME_1;
		const char* phase = firstAttackFrame ? "attack-frame-1" : "attack-frame-2";
		if (firstAttackFrame && !m_bNormalSpawnEligibilityLogged)
		{
			const edict_t* pSpawn = m_pNormalSpawnSelectionEdict;
			edict_t* pSpawnMutable = const_cast<edict_t*>(pSpawn);
			const int spawnIndex = pSpawn != nullptr && engine != nullptr ?
				engine->IndexOfEdict(pSpawnMutable) : -1;
			const char* spawnClass = pSpawn != nullptr && !pSpawn->IsFree() &&
				pSpawn->GetClassName() != nullptr ? pSpawn->GetClassName() : "unavailable";
			IServerUnknown* pSpawnUnknown = pSpawnMutable != nullptr && !pSpawnMutable->IsFree() ?
				pSpawnMutable->GetUnknown() : nullptr;
			CBaseEntity* pSpawnEntity = pSpawnUnknown != nullptr ?
				pSpawnUnknown->GetBaseEntity() : nullptr;
			char pointNumber[32] = {};
			char startDisabled[32] = {};
			char spawnFlags[32] = {};
			const bool pointNumberKnown = pSpawnEntity != nullptr && servertools != nullptr &&
				servertools->GetKeyValue(pSpawnEntity, "pointNumber", pointNumber, sizeof(pointNumber));
			const bool startDisabledKnown = pSpawnEntity != nullptr && servertools != nullptr &&
				servertools->GetKeyValue(pSpawnEntity, "StartDisabled", startDisabled, sizeof(startDisabled));
			const bool spawnFlagsKnown = pSpawnEntity != nullptr && servertools != nullptr &&
				servertools->GetKeyValue(pSpawnEntity, "spawnflags", spawnFlags, sizeof(spawnFlags));
			bool disabled = false;
			const bool disabledKnown = pSpawn != nullptr &&
				readEmpiresEntityBoolProperty(pSpawn, "m_bDisabled", disabled);
			int playerFlags = -1;
			const bool playerFlagsKnown = readEmpiresIntProperty(m_pEdict, "m_fFlags", playerFlags);
			const Vector spawnOrigin = pSpawn != nullptr && !pSpawn->IsFree() ?
				CBotGlobals::entityOrigin(const_cast<edict_t*>(pSpawn)) : Vector(0.0f, 0.0f, 0.0f);
			const bool spawnOriginKnown = spawnOrigin.IsValid();
			logger->Log(LogLevel::INFO,
				"Empires normal spawn eligibility index=%d userid=%d team=%d class=%d curtime=%.3f engine_time=%.3f player_life=%d health=%d desired_spawn=%d respawn_timer=%.3f spawn_index=%d spawn_class=%s pointNumber=%s pointNumber_known=%d spawn_origin=(%.1f %.1f %.1f) spawn_origin_known=%d StartDisabled=%s StartDisabled_known=%d spawnflags=%s spawnflags_known=%d m_bDisabled=%d m_bDisabled_known=%d player_flags=0x%x player_flags_known=%d round_team_eligibility=unknown",
				index, userId, m_iDesiredTeam, classId,
				gpGlobals != nullptr ? gpGlobals->curtime : 0.0f,
				engine != nullptr ? engine->Time() : 0.0f, lifeState, health,
				desiredSpawnKnown ? desiredSpawn : -1,
				respawnTimerKnown ? respawnTimer : 0.0f, spawnIndex, spawnClass,
				pointNumberKnown ? pointNumber : "unavailable", pointNumberKnown ? 1 : 0,
				spawnOrigin.x, spawnOrigin.y, spawnOrigin.z, spawnOriginKnown ? 1 : 0,
				startDisabledKnown ? startDisabled : "unavailable", startDisabledKnown ? 1 : 0,
				spawnFlagsKnown ? spawnFlags : "unavailable", spawnFlagsKnown ? 1 : 0,
				disabled ? 1 : 0, disabledKnown ? 1 : 0, playerFlags,
				playerFlagsKnown ? 1 : 0);
			m_bNormalSpawnEligibilityLogged = true;
		}
		if (!submitNormalSpawnUserCmd(IN_ATTACK, phase))
		{
			logger->Log(LogLevel::ERROR,
				"Empires normal spawn result=FAILED reason=spawn-confirm-input-failed phase=%s",
				phase);
			m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
			return;
		}

		// Empires' PlayerDeathThink consumes the current m_nButtons state on a
		// later player-think pass. Keep the attack held for the same two-frame
		// sequence already proven by the controller probe, then send the neutral
		// release so the edge is cleanly closed.
		m_eNormalSpawnState = firstAttackFrame ?
			EmpiresNormalSpawnState::ATTACK_FRAME_2 :
			EmpiresNormalSpawnState::RELEASE_FRAME;
		logger->Log(LogLevel::INFO,
			"Empires normal spawn transition state=%s index=%d userid=%d attack_frame=%s",
			normalSpawnStateName(m_eNormalSpawnState), index, userId,
			firstAttackFrame ? "first" : "second");
		return;
	}

	if (m_eNormalSpawnState == EmpiresNormalSpawnState::RELEASE_FRAME)
	{
		if (!submitNormalSpawnUserCmd(0, "release"))
		{
			logger->Log(LogLevel::ERROR,
				"Empires normal spawn result=FAILED reason=spawn-confirm-release-failed index=%d userid=%d",
				index, userId);
			m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
			return;
		}

		m_eNormalSpawnState = EmpiresNormalSpawnState::OBSERVING_RESULT;
		m_iNormalSpawnFrames = 0;
		m_flNormalSpawnObserveDeadline = engine->Time() + kNormalSpawnResultTimeout;
		logger->Log(LogLevel::INFO,
			"Empires normal spawn transition state=%s index=%d userid=%d",
			normalSpawnStateName(m_eNormalSpawnState), index, userId);
		return;
	}

	if (m_eNormalSpawnState == EmpiresNormalSpawnState::OBSERVING_RESULT)
	{
		if (alive)
		{
			logNormalSpawnInventory();
			logger->Log(LogLevel::INFO,
				"Empires normal spawn result=COMPLETE index=%d userid=%d team=%d class=%d property=%s desired_spawn=%d health=%d life_state=%d life_state_name=%s frames=%d",
				index, userId, m_iDesiredTeam, classId,
				classPropertyName != nullptr ? classPropertyName : "unavailable",
				desiredSpawnKnown ? desiredSpawn : -1, health, lifeState,
				lifeStateName(lifeState), m_iNormalSpawnFrames);
			m_eNormalSpawnState = EmpiresNormalSpawnState::COMPLETE;
			return;
		}

		if (engine->Time() >= m_flNormalSpawnObserveDeadline)
		{
			logger->Log(LogLevel::ERROR,
				"Empires normal spawn result=FAILED reason=client-remained-not-alive index=%d userid=%d team=%d class=%d class_matches=%d desired_spawn=%d health=%d life_state=%d life_state_known=%d frames=%d",
				index, userId, m_iDesiredTeam, classId, classMatches ? 1 : 0,
				desiredSpawnKnown ? desiredSpawn : -1, health, lifeState,
				lifeStateKnown ? 1 : 0, m_iNormalSpawnFrames);
			m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
		}
	}
}

void CBotEmpires::logNormalSpawnInventory() const
{
	if (m_pEdict == nullptr || m_pEdict->IsFree() || logger == nullptr ||
		engine == nullptr)
	{
		return;
	}

	const edict_t* activeWeapon = CClassInterface::getCurrentWeapon(m_pEdict);
	const char* activeName = activeWeapon != nullptr && !activeWeapon->IsFree() &&
		activeWeapon->GetClassName() != nullptr ? activeWeapon->GetClassName() :
		"unavailable";

	char ownedWeapons[1024] = {};
	const CBaseHandle* weaponHandles = CClassInterface::getWeaponList(m_pEdict);
	if (weaponHandles != nullptr)
	{
		for (int slot = 0; slot < MAX_WEAPONS; ++slot)
		{
			const int entryIndex = weaponHandles[slot].GetEntryIndex();
			if (entryIndex <= 0)
			{
				continue;
			}

			edict_t* weapon = INDEXENT(entryIndex);
			if (weapon == nullptr || weapon->IsFree() ||
				CClassInterface::getWeaponState(weapon) == WEAPON_NOT_CARRIED ||
				weapon->GetClassName() == nullptr)
			{
				continue;
			}

			if (ownedWeapons[0] != '\0')
			{
				std::strncat(ownedWeapons, ",", sizeof(ownedWeapons) -
					std::strlen(ownedWeapons) - 1);
			}
			std::strncat(ownedWeapons, weapon->GetClassName(),
				sizeof(ownedWeapons) - std::strlen(ownedWeapons) - 1);
		}
	}

	logger->Log(LogLevel::INFO,
		"Empires normal spawn inventory index=%d userid=%d active_weapon=%s owned_weapons=%s source=public_weapon_list",
		engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
		activeName, ownedWeapons[0] != '\0' ? ownedWeapons : "unavailable");
}

bool CBotEmpires::submitBootstrapUserCmd(const int buttons, const char* phase)
{
	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) || before.edict == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawnconfirm %s rejected: state unavailable before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	if (before.index != m_iSpawnConfirmationExpectedIndex ||
		before.userId != m_iSpawnConfirmationExpectedUserId ||
		before.edict != m_pSpawnConfirmationExpectedEdict)
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawnconfirm %s failed: client identity changed expected_index=%d actual_index=%d expected_userid=%d actual_userid=%d expected_edict=%p actual_edict=%p",
			phase != nullptr ? phase : "unknown", m_iSpawnConfirmationExpectedIndex,
			before.index, m_iSpawnConfirmationExpectedUserId, before.userId,
			static_cast<const void*>(m_pSpawnConfirmationExpectedEdict),
			static_cast<const void*>(before.edict));
		return false;
	}

	if (g_pBotManager == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawnconfirm %s failed: IBotManager is unavailable",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	IBotController* pController = g_pBotManager->GetBotController(
		const_cast<edict_t*>(before.edict));
	if (pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawnconfirm %s failed: fresh GetBotController returned null index=%d userid=%d edict=%p",
			phase != nullptr ? phase : "unknown", before.index, before.userId,
			static_cast<const void*>(before.edict));
		return false;
	}

	CBotCmd userCommand;
	const int commandNumber = cmd.command_number > 0 ? cmd.command_number + 1 : 1;
	userCommand.command_number = commandNumber;
	userCommand.tick_count = gpGlobals != nullptr ? gpGlobals->tickcount : 0;
	userCommand.viewangles = m_vViewAngles;
	userCommand.forwardmove = 0.0f;
	userCommand.sidemove = 0.0f;
	userCommand.upmove = 0.0f;
	userCommand.buttons = buttons;
	userCommand.impulse = 0;
	userCommand.weaponselect = 0;
	userCommand.weaponsubtype = 0;
	userCommand.random_seed = 0;
	userCommand.mousedx = 0;
	userCommand.mousedy = 0;
	userCommand.hasbeenpredicted = false;

	logger->Log(LogLevel::INFO,
		"Empires bootstrap spawnconfirm submit phase=%s index=%d userid=%d edict=%p command_number=%d tick_count=%d buttons=0x%x forwardmove=%.1f sidemove=%.1f upmove=%.1f impulse=%d weaponselect=%d weaponsubtype=%d random_seed=%d mousedx=%d mousedy=%d viewangles=(%.1f %.1f %.1f)",
		phase != nullptr ? phase : "unknown", before.index, before.userId,
		static_cast<const void*>(before.edict), userCommand.command_number,
		userCommand.tick_count, userCommand.buttons, userCommand.forwardmove,
		userCommand.sidemove, userCommand.upmove, userCommand.impulse,
		userCommand.weaponselect, userCommand.weaponsubtype, userCommand.random_seed,
		userCommand.mousedx, userCommand.mousedy, userCommand.viewangles.x,
		userCommand.viewangles.y, userCommand.viewangles.z);

	// This is the SDK2013 public bot input path. The controller is reacquired for
	// every controlled frame; the cached CBot controller is never used here.
	pController->RunPlayerMove(&userCommand);
	cmd.Reset();
	cmd.command_number = commandNumber;
	cmd.viewangles = userCommand.viewangles;

	EmpiresBootstrapPlayerState after;
	if (readBootstrapPlayerState(after))
	{
		logger->Log(LogLevel::INFO,
			"Empires bootstrap spawnconfirm after-%s index=%d userid=%d edict=%p team=%d class=%d class_desired=%d desired_spawn=%d health=%d life_state=%d life_state_known=%d dead=%d alive=%d origin=(%.1f %.1f %.1f) lastcmd_known=%d lastcmd_number=%d lastcmd_tick=%d lastcmd_buttons=0x%x lastcmd_forward=%.1f lastcmd_side=%.1f lastcmd_up=%.1f lastcmd_impulse=%d lastcmd_weaponselect=%d",
			phase != nullptr ? phase : "unknown", after.index, after.userId,
			static_cast<const void*>(after.edict), after.team, after.classId,
			after.classDesired ? 1 : 0, after.desiredSpawn, after.health,
			after.lifeState, after.lifeStateKnown ? 1 : 0, after.dead ? 1 : 0,
			after.alive ? 1 : 0, after.origin.x, after.origin.y, after.origin.z,
			after.lastUserCommandKnown ? 1 : 0, after.lastUserCommandNumber,
			after.lastUserCommandTick, after.lastUserCommandButtons,
			after.lastUserCommandForwardMove, after.lastUserCommandSideMove,
			after.lastUserCommandUpMove, after.lastUserCommandImpulse,
			after.lastUserCommandWeaponSelect);
	}
	else
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawnconfirm after-%s state unavailable",
			phase != nullptr ? phase : "unknown");
	}

	return true;
}

void CBotEmpires::failBootstrapSpawnConfirmation(const char* reason)
{
	logger->Log(LogLevel::ERROR,
		"Empires bootstrap spawnconfirm state=%s result=FAILED reason=%s press_count=%d release_count=%d observe_frames=%d",
		spawnConfirmationStateName(m_eSpawnConfirmationState),
		reason != nullptr ? reason : "unknown", m_iSpawnConfirmationPressCount,
		m_iSpawnConfirmationReleaseCount, m_iSpawnConfirmationFrames);
	m_eSpawnConfirmationState = EmpiresBootstrapSpawnConfirmationState::FAILED;
	m_bSpawnCommandPending = false;
	m_iPendingSpawn = -1;
}

void CBotEmpires::processBootstrapSpawnConfirmation()
{
	if (m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::IDLE ||
		m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::COMPLETE ||
		m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::FAILED)
	{
		return;
	}

	if (m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::ATTACK_PRESS_PENDING)
	{
		EmpiresBootstrapPlayerState state;
		if (!readBootstrapPlayerState(state) || !state.dead || state.alive)
		{
			failBootstrapSpawnConfirmation("client was not still dead immediately before the single attack press");
			return;
		}

		if (!submitBootstrapUserCmd(IN_ATTACK, "attack-press"))
		{
			failBootstrapSpawnConfirmation("attack press could not be submitted");
			return;
		}

		++m_iSpawnConfirmationPressCount;
		m_eSpawnConfirmationState = EmpiresBootstrapSpawnConfirmationState::ATTACK_RELEASE_PENDING;
		logger->Log(LogLevel::INFO,
			"Empires bootstrap spawnconfirm transition state=%s press_count=%d release_required=1",
			spawnConfirmationStateName(m_eSpawnConfirmationState),
			m_iSpawnConfirmationPressCount);
		return;
	}

	if (m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::ATTACK_RELEASE_PENDING)
	{
		if (!submitBootstrapUserCmd(0, "attack-release"))
		{
			failBootstrapSpawnConfirmation("mandatory neutral release could not be submitted");
			return;
		}

		++m_iSpawnConfirmationReleaseCount;
		m_iSpawnConfirmationFrames = 0;
		m_iSpawnConfirmationStableSamples = 0;
		m_bSpawnConfirmationOriginKnown = false;
		m_eSpawnConfirmationState = EmpiresBootstrapSpawnConfirmationState::OBSERVING_RESULT;
		logger->Log(LogLevel::INFO,
			"Empires bootstrap spawnconfirm transition state=%s release_count=%d press_count=%d",
			spawnConfirmationStateName(m_eSpawnConfirmationState),
			m_iSpawnConfirmationReleaseCount, m_iSpawnConfirmationPressCount);
		return;
	}

	++m_iSpawnConfirmationFrames;
	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state))
	{
		failBootstrapSpawnConfirmation("client state became unavailable during bounded observation");
		return;
	}

	if (state.index != m_iSpawnConfirmationExpectedIndex ||
		state.userId != m_iSpawnConfirmationExpectedUserId ||
		state.edict != m_pSpawnConfirmationExpectedEdict)
	{
		failBootstrapSpawnConfirmation("client index, userid, or edict changed during observation");
		return;
	}

	const bool classMatches = state.classKnown && state.classId == m_iSelectedBootstrapClass;
	const bool healthPositive = state.healthKnown && state.health > 0;
	const bool lifeStateAlive = state.lifeStateKnown && state.lifeState == 0;
	const bool aliveIndicators = state.alive && !state.dead && healthPositive &&
		lifeStateAlive && classMatches;

	// The transition from a dead client to a spawned player is expected to
	// move the entity to the selected spawn point.  Establish the immobility
	// baseline only after the complete alive-state predicate is true; compare
	// subsequent alive samples against that baseline.
	if (aliveIndicators && state.originKnown)
	{
		if (!m_bSpawnConfirmationOriginKnown)
		{
			m_vSpawnConfirmationOrigin = state.origin;
			m_bSpawnConfirmationOriginKnown = true;
			m_iSpawnConfirmationStableSamples = 1;
		}
		else
		{
			const float deltaSqr = (state.origin - m_vSpawnConfirmationOrigin).LengthSqr();
			if (deltaSqr > 0.01f)
			{
				logger->Log(LogLevel::ERROR,
					"Empires bootstrap spawnconfirm result=FAILED reason=motion detected index=%d userid=%d origin=(%.1f %.1f %.1f) previous=(%.1f %.1f %.1f) delta_sqr=%.3f frame=%d",
					state.index, state.userId, state.origin.x, state.origin.y, state.origin.z,
					m_vSpawnConfirmationOrigin.x, m_vSpawnConfirmationOrigin.y,
					m_vSpawnConfirmationOrigin.z, deltaSqr, m_iSpawnConfirmationFrames);
				failBootstrapSpawnConfirmation("origin changed after the controlled input");
				return;
			}

			++m_iSpawnConfirmationStableSamples;
		}
	}

	if (m_iSpawnConfirmationFrames <= 3 || (m_iSpawnConfirmationFrames % 10) == 0 || aliveIndicators)
	{
		logger->Log(LogLevel::INFO,
			"Empires bootstrap spawnconfirm observe frame=%d index=%d userid=%d edict=%p team=%d class=%d class_matches=%d class_desired=%d desired_spawn=%d health=%d health_positive=%d life_state=%d life_state_known=%d life_state_alive=%d dead=%d alive=%d origin=(%.1f %.1f %.1f) lastcmd_buttons=0x%x lastcmd_number=%d stable_samples=%d",
			m_iSpawnConfirmationFrames, state.index, state.userId,
			static_cast<const void*>(state.edict), state.team, state.classId,
			classMatches ? 1 : 0, state.classDesired ? 1 : 0, state.desiredSpawn,
			state.health, healthPositive ? 1 : 0, state.lifeState,
			state.lifeStateKnown ? 1 : 0, lifeStateAlive ? 1 : 0,
			state.dead ? 1 : 0, state.alive ? 1 : 0, state.origin.x, state.origin.y,
			state.origin.z, state.lastUserCommandButtons, state.lastUserCommandNumber,
			m_iSpawnConfirmationStableSamples);
	}

	if (aliveIndicators && m_iSpawnConfirmationStableSamples >= 2)
	{
		logger->Log(LogLevel::INFO,
			"Empires bootstrap spawnconfirm result=COMPLETE index=%d userid=%d edict=%p team=%d applied_class=%d desired_class=%d desired_spawn=%d health=%d life_state=%d dead=%d alive=%d press_count=%d release_count=%d observe_frames=%d stable_samples=%d lastcmd_buttons=0x%x",
			state.index, state.userId, static_cast<const void*>(state.edict), state.team,
			state.classId, state.classDesired ? 1 : 0, state.desiredSpawn, state.health,
			state.lifeState, state.dead ? 1 : 0, state.alive ? 1 : 0,
			m_iSpawnConfirmationPressCount, m_iSpawnConfirmationReleaseCount,
			m_iSpawnConfirmationFrames, m_iSpawnConfirmationStableSamples,
			state.lastUserCommandButtons);
		m_eSpawnConfirmationState = EmpiresBootstrapSpawnConfirmationState::COMPLETE;
		m_bSpawnCommandPending = false;
		m_iPendingSpawn = -1;
		return;
	}

	constexpr int kMaxObservationFrames = 120;
	if (m_iSpawnConfirmationFrames >= kMaxObservationFrames)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawnconfirm result=FAILED reason=client remained dead after bounded observation index=%d userid=%d edict=%p team=%d class=%d class_matches=%d class_desired=%d desired_spawn=%d health=%d life_state=%d life_state_known=%d dead=%d alive=%d press_count=%d release_count=%d observe_frames=%d lastcmd_buttons=0x%x",
			state.index, state.userId, static_cast<const void*>(state.edict), state.team,
			state.classId, classMatches ? 1 : 0, state.classDesired ? 1 : 0,
			state.desiredSpawn, state.health, state.lifeState,
			state.lifeStateKnown ? 1 : 0, state.dead ? 1 : 0, state.alive ? 1 : 0,
			m_iSpawnConfirmationPressCount, m_iSpawnConfirmationReleaseCount,
			m_iSpawnConfirmationFrames, state.lastUserCommandButtons);
		m_eSpawnConfirmationState = EmpiresBootstrapSpawnConfirmationState::FAILED;
		m_bSpawnCommandPending = false;
		m_iPendingSpawn = -1;
	}
}

void CBotEmpires::observePendingSpawn()
{
	if (!m_bSpawnCommandPending)
		return;

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state))
	{
		if (engine == nullptr || engine->Time() < m_flPendingSpawnDeadline)
			return;

		logger->Log(LogLevel::ERROR,
			"Empires bootstrap spawn observation failed: client state unavailable spawn_id=%d frames=%d",
			m_iPendingSpawn, m_iPendingSpawnFrames);
		m_bSpawnCommandPending = false;
		m_iPendingSpawn = -1;
		return;
	}

	++m_iPendingSpawnFrames;
	if (state.alive && !state.dead)
	{
		const bool classMatches = state.classKnown && state.classId == m_iSelectedBootstrapClass;
		const bool healthPositive = state.healthKnown && state.health > 0;
		if (classMatches && healthPositive && state.originKnown)
		{
			bool originStable = true;
			if (!m_bSpawnOriginAfterKnown)
			{
				m_vSpawnOriginAfter = state.origin;
				m_bSpawnOriginAfterKnown = true;
			}
			else
			{
				originStable = (state.origin - m_vSpawnOriginAfter).LengthSqr() <= 0.01f;
			}

			if (!originStable)
			{
				logger->Log(LogLevel::ERROR,
					"Empires bootstrap spawn observation failed: motion detected index=%d userid=%d origin_before=(%.1f %.1f %.1f) origin_after=(%.1f %.1f %.1f) origin_current=(%.1f %.1f %.1f) frames=%d",
					state.index, state.userId,
					m_vSpawnOriginBefore.x, m_vSpawnOriginBefore.y, m_vSpawnOriginBefore.z,
					m_vSpawnOriginAfter.x, m_vSpawnOriginAfter.y, m_vSpawnOriginAfter.z,
					state.origin.x, state.origin.y, state.origin.z, m_iPendingSpawnFrames);
				m_bSpawnCommandPending = false;
				m_iPendingSpawn = -1;
				return;
			}

			++m_iSpawnStableSamples;
			if (m_iSpawnStableSamples < 2)
				return;

			logger->Log(LogLevel::INFO,
				"Empires bootstrap spawn observed index=%d userid=%d edict=%p team=%d class=%d desired_spawn=%d health=%d life_state=%d life_state_known=%d dead=%d alive=%d class_matches=%d origin_before_known=%d origin=(%.1f %.1f %.1f) origin_stable=%d frames=%d stable_samples=%d callbacks=not-hooked observation=success",
				state.index, state.userId, static_cast<const void*>(state.edict), state.team,
				state.classId, state.desiredSpawn, state.health, state.lifeState,
				state.lifeStateKnown ? 1 : 0, state.dead ? 1 : 0, state.alive ? 1 : 0,
				classMatches ? 1 : 0, m_bSpawnOriginBeforeKnown ? 1 : 0,
				state.origin.x, state.origin.y, state.origin.z, 1,
				m_iPendingSpawnFrames, m_iSpawnStableSamples);
			m_bSpawnCommandPending = false;
			m_iPendingSpawn = -1;
			return;
		}

		const bool deadlineReached = engine != nullptr && engine->Time() >= m_flPendingSpawnDeadline;
		if (!deadlineReached)
			return;

		logger->Log(LogLevel::INFO,
			"Empires bootstrap spawn observation rejected after alive transition: index=%d userid=%d edict=%p team=%d class=%d class_known=%d health=%d health_known=%d origin_known=%d class_matches=%d frames=%d",
			state.index, state.userId, static_cast<const void*>(state.edict), state.team,
			state.classId, state.classKnown ? 1 : 0, state.health,
			state.healthKnown ? 1 : 0, state.originKnown ? 1 : 0,
			classMatches ? 1 : 0, m_iPendingSpawnFrames);
	}

	if (engine != nullptr && engine->Time() >= m_flPendingSpawnDeadline)
	{
		logger->Log(LogLevel::WARN,
			"Empires bootstrap spawn observed index=%d userid=%d edict=%p team=%d class=%d desired_spawn=%d health=%d dead=%d alive=%d observation=failed frames=%d",
			state.index, state.userId, static_cast<const void*>(state.edict), state.team,
			state.classId, state.desiredSpawn, state.health, state.dead ? 1 : 0,
			state.alive ? 1 : 0, m_iPendingSpawnFrames);
		m_bSpawnCommandPending = false;
		m_iPendingSpawn = -1;
	}
}

void CBotEmpires::resetControlledTestState()
{
	m_bBootstrapOnly = false;
	m_bControllerProbeOnly = false;
	m_eControllerProbeLifecycle = EmpiresControllerProbeLifecycle::INACTIVE;
	m_eControllerProbeSpawnSequenceState =
		EmpiresControllerProbeSpawnSequenceState::IDLE;
	m_eControllerProbeSpawnHoldState = EmpiresControllerProbeSpawnHoldState::IDLE;
	m_eControllerProbeNeutralReadinessState =
		EmpiresControllerProbeNeutralReadinessState::IDLE;
	m_eControllerProbeLoadoutReadinessState =
		EmpiresControllerProbeLoadoutReadinessState::IDLE;
	m_eControllerProbeAppliedLoadoutReadinessState =
		EmpiresControllerProbeAppliedLoadoutReadinessState::IDLE;
	m_eControllerProbeSpawnConfirmReadyState =
		EmpiresControllerProbeSpawnConfirmReadyState::IDLE;
	m_bControllerProbeReadinessOnly = false;
	m_iControllerProbeSlot = -1;
	m_iControllerProbeClientIndex = -1;
	m_iControllerProbePathTeam = -1;
	m_iControllerProbeSequenceExpectedIndex = -1;
	m_iControllerProbeSequenceExpectedUserId = -1;
	m_iControllerProbeSequenceStageFrames = 0;
	m_iControllerProbeSequencePressCount = 0;
	m_iControllerProbeSequenceReleaseCount = 0;
	m_iControllerProbeSequenceStableSamples = 0;
	m_bControllerProbeSequenceInitialAlive = false;
	m_vControllerProbeSequenceOrigin = Vector();
	m_bControllerProbeSequenceOriginKnown = false;
	m_iControllerProbeHoldExpectedIndex = -1;
	m_iControllerProbeHoldExpectedUserId = -1;
	m_pControllerProbeHoldExpectedEdict = nullptr;
	m_iControllerProbeHoldStageFrames = 0;
	m_iControllerProbeHoldObserveFrames = 0;
	m_iControllerProbeHoldAttackCount = 0;
	m_iControllerProbeHoldReleaseCount = 0;
	m_bControllerProbeHoldTimerKnownEverySample = true;
	m_bControllerProbeHoldTimerFutureObserved = false;
	m_bControllerProbeHoldTimerExpired = false;
	m_bControllerProbeReadinessStartTimeKnown = false;
	m_flControllerProbeReadinessStartTime = 0.0f;
	m_bControllerProbeReadinessLastLifeStateKnown = false;
	m_iControllerProbeReadinessLastLifeState = -1;
	m_bControllerProbeReadinessLastTimerKnown = false;
	m_flControllerProbeReadinessLastTimer = 0.0f;
	m_bControllerProbeReadinessLatestDeadlineKnown = false;
	m_flControllerProbeReadinessLatestDeadline = 0.0f;
	m_iControllerProbeNeutralExpectedIndex = -1;
	m_iControllerProbeNeutralExpectedUserId = -1;
	m_pControllerProbeNeutralExpectedEdict = nullptr;
	m_iControllerProbeNeutralStageFrames = 0;
	m_iControllerProbeNeutralSubmittedCommands = 0;
	m_iControllerProbeNeutralMaxCommands = 0;
	m_iControllerProbeNeutralLastSubmittedTick = -1;
	m_iControllerProbeNeutralLastSubmittedCommand = 0;
	m_iControllerProbeNeutralLastProgressLogCommand = 0;
	m_bControllerProbeNeutralStartTimeKnown = false;
	m_flControllerProbeNeutralStartTime = 0.0f;
	m_bControllerProbeNeutralLastLifeStateKnown = false;
	m_iControllerProbeNeutralLastLifeState = -1;
	m_bControllerProbeNeutralLastTimerKnown = false;
	m_flControllerProbeNeutralLastTimer = 0.0f;
	m_bControllerProbeNeutralLatestDeadlineKnown = false;
	m_flControllerProbeNeutralLatestDeadline = 0.0f;
	m_iControllerProbeLoadoutExpectedIndex = -1;
	m_iControllerProbeLoadoutExpectedUserId = -1;
	m_pControllerProbeLoadoutExpectedEdict = nullptr;
	m_iControllerProbeLoadoutStageFrames = 0;
	m_iControllerProbeLoadoutSubmittedCommands = 0;
	m_iControllerProbeLoadoutMaxCommands = 0;
	m_iControllerProbeLoadoutLastSubmittedTick = -1;
	m_iControllerProbeLoadoutLastSubmittedCommand = 0;
	m_iControllerProbeLoadoutLastProgressLogCommand = 0;
	m_bControllerProbeLoadoutClassFirst = false;
	m_bControllerProbeLoadoutStartTimeKnown = false;
	m_flControllerProbeLoadoutStartTime = 0.0f;
	m_bControllerProbeLoadoutNeutralStartTimeKnown = false;
	m_flControllerProbeLoadoutNeutralStartTime = 0.0f;
	m_bControllerProbeLoadoutLastLifeStateKnown = false;
	m_iControllerProbeLoadoutLastLifeState = -1;
	m_bControllerProbeLoadoutLastTimerKnown = false;
	m_flControllerProbeLoadoutLastTimer = 0.0f;
	m_bControllerProbeLoadoutLatestDeadlineKnown = false;
	m_flControllerProbeLoadoutLatestDeadline = 0.0f;
	m_iControllerProbeAppliedLoadoutExpectedIndex = -1;
	m_iControllerProbeAppliedLoadoutExpectedUserId = -1;
	m_pControllerProbeAppliedLoadoutExpectedEdict = nullptr;
	m_iControllerProbeAppliedLoadoutStageFrames = 0;
	m_iControllerProbeAppliedLoadoutSubmittedCommands = 0;
	m_iControllerProbeAppliedLoadoutMaxCommands = 0;
	m_iControllerProbeAppliedLoadoutLastSubmittedTick = -1;
	m_iControllerProbeAppliedLoadoutLastSubmittedCommand = 0;
	m_iControllerProbeAppliedLoadoutLastProgressLogCommand = 0;
	m_bControllerProbeAppliedLoadoutStartTimeKnown = false;
	m_flControllerProbeAppliedLoadoutStartTime = 0.0f;
	m_bControllerProbeAppliedLoadoutNeutralStartTimeKnown = false;
	m_flControllerProbeAppliedLoadoutNeutralStartTime = 0.0f;
	m_bControllerProbeAppliedLoadoutLastStateKnown = false;
	m_controllerProbeAppliedLoadoutLastState = {};
	m_bControllerProbeAppliedLoadoutBeforeEquipmentKnown = false;
	m_controllerProbeAppliedLoadoutBeforeEquipment = {};
	m_iControllerProbeSpawnConfirmReadyExpectedIndex = -1;
	m_iControllerProbeSpawnConfirmReadyExpectedUserId = -1;
	m_pControllerProbeSpawnConfirmReadyExpectedEdict = nullptr;
	m_iControllerProbeSpawnConfirmReadyPressCount = 0;
	m_iControllerProbeSpawnConfirmReadyReleaseCount = 0;
	m_iControllerProbeSpawnConfirmReadyObserveFrames = 0;
	m_iControllerProbeSpawnConfirmReadyLastSubmittedCommand = 0;
	m_iControllerProbeSpawnConfirmReadyLastSubmittedTick = -1;
	m_iControllerProbeSpawnConfirmReadyLastLifeState = -1;
	m_bControllerProbeSpawnConfirmReadyLastLifeStateKnown = false;
	m_vControllerProbeSpawnConfirmReadyOrigin = Vector();
	m_bControllerProbeSpawnConfirmReadyOriginKnown = false;
	m_eControllerProbeMovementState =
		EmpiresControllerProbeMovementState::IDLE;
	m_eControllerProbeWaypointState =
		EmpiresControllerProbeWaypointState::IDLE;
	m_iControllerProbeMovementExpectedIndex = -1;
	m_iControllerProbeMovementExpectedUserId = -1;
	m_pControllerProbeMovementExpectedEdict = nullptr;
	m_iControllerProbeMovementActiveFrames = 0;
	m_iControllerProbeMovementFinalObserveFrames = 0;
	m_iControllerProbeMovementSubmittedCommands = 0;
	m_iControllerProbeMovementLastSubmittedCommand = 0;
	m_iControllerProbeMovementLastSubmittedTick = -1;
	m_bControllerProbeMovementStartOriginKnown = false;
	m_vControllerProbeMovementStartOrigin = Vector();
	m_bControllerProbeMovementFinalOriginKnown = false;
	m_vControllerProbeMovementFinalOrigin = Vector();
	m_vControllerProbeMovementViewAngles = QAngle(0.0f, 0.0f, 0.0f);
	m_iControllerProbeWaypointExpectedIndex = -1;
	m_iControllerProbeWaypointExpectedUserId = -1;
	m_pControllerProbeWaypointExpectedEdict = nullptr;
	m_iControllerProbeWaypointId = -1;
	m_iControllerProbePathStartWaypoint = -1;
	m_iControllerProbePathDestinationWaypoint = -1;
	m_bControllerProbeWaypointRouteMode = false;
	m_bControllerProbeWaypointPathMode = false;
	m_bControllerProbeWaypointPathSearchStarted = false;
	m_iControllerProbeWaypointPathSearchFrames = 0;
	m_iControllerProbeWaypointPathNodeIndex = 0;
	m_ControllerProbeWaypointPath.clear();
	m_iControllerProbeWaypointRouteTargetsVisited = 0;
	m_iControllerProbeWaypointRoutePreviousId = -1;
	m_iControllerProbeWaypointRouteSegmentFrames = 0;
	m_iControllerProbeWaypointActiveFrames = 0;
	m_iControllerProbeWaypointFinalObserveFrames = 0;
	m_iControllerProbeWaypointSubmittedCommands = 0;
	m_iControllerProbeWaypointLastSubmittedCommand = 0;
	m_iControllerProbeWaypointLastSubmittedTick = -1;
	m_iControllerProbeWaypointNoProgressFrames = 0;
	m_bControllerProbeWaypointInputActive = false;
	m_bControllerProbeWaypointStartOriginKnown = false;
	m_vControllerProbeWaypointStartOrigin = Vector();
	m_vControllerProbeWaypointViewAngles = QAngle(0.0f, 0.0f, 0.0f);
	m_bControllerProbeWaypointTargetKnown = false;
	m_vControllerProbeWaypointTargetOrigin = Vector();
	m_bControllerProbeWaypointFinalOriginKnown = false;
	m_vControllerProbeWaypointFinalOrigin = Vector();
	m_flControllerProbeWaypointStartTime = 0.0f;
	m_flControllerProbeWaypointInitialDistance = 0.0f;
	m_flControllerProbeWaypointBestDistance = 0.0f;
	m_bControllerProbeWaypointReached = false;
	m_bControllerProbeWaypointTimedOut = false;
	m_bControllerProbeWaypointNoProgress = false;
	m_bThinkGateLogged = false;
	m_bStartGameGateLogged = false;
	m_bNormalTeamStartComplete = false;
	m_bNormalTeamCommandPending = false;
	m_bNormalTeamStartFailed = false;
	m_iNormalTeam = -1;
	m_iNormalTeamFrames = 0;
	m_flNormalTeamDeadline = 0.0f;
	m_pNormalTeamEdict = nullptr;
	m_bNormalClassStartComplete = false;
	m_bNormalClassCommandPending = false;
	m_bNormalClassStartFailed = false;
	m_iNormalClass = -1;
	m_iNormalClassFrames = 0;
	m_flNormalClassDeadline = 0.0f;
	m_eEmpiresMortarProbeState = EmpiresMortarProbeState::IDLE;
	m_iEmpiresMortarProbeAttempt = 0;
	m_iEmpiresMortarProbeFrames = 0;
	m_iEmpiresMortarProbeShellBaseline = 0;
	m_iEmpiresMortarProbeShellIndex = -1;
	m_iEmpiresMortarProbeLastAmmo = -1;
	m_iEmpiresMortarProbeLastClip = -1;
	m_iEmpiresMortarProbeLastFlags = 0;
	m_bEmpiresMortarProbeLastFlagsKnown = false;
	m_bEmpiresMortarProbePostureReadyLogged = false;
	m_bEmpiresMortarProbeLoadedKnown = false;
	m_bEmpiresMortarProbeLastLoaded = false;
	m_bEmpiresMortarProbeReloadTimeKnown = false;
	m_flEmpiresMortarProbeReloadTime = 0.0f;
	m_bEmpiresMortarProbeNextPrimaryKnown = false;
	m_flEmpiresMortarProbeNextPrimaryAttack = 0.0f;
	m_bEmpiresMortarProbeInitialNeutralLogged = false;
	m_bEmpiresMortarProbeReleaseLogged = false;
	m_iEmpiresMortarProbeTelemetryPending = 0;
	m_flEmpiresMortarProbeDeadline = 0.0f;
	m_flEmpiresMortarProbeShellDeadline = 0.0f;
	m_iEmpiresMortarProbeExpectedIndex = -1;
	m_iEmpiresMortarProbeExpectedUserId = -1;
	m_pEmpiresMortarProbeExpectedEdict = nullptr;
	m_pEmpiresMortarProbeWeapon = nullptr;
	m_bRemovalRequested = false;
	m_bTeamCommandPending = false;
	m_iPendingTeam = -1;
	m_iPendingTeamFrames = 0;
	m_flPendingTeamDeadline = 0.0f;
	m_bClassCommandPending = false;
	m_iPendingClass = -1;
	m_iPendingClassFrames = 0;
	m_flPendingClassDeadline = 0.0f;
	m_bPendingClassDesiredKnown = false;
	m_bPendingClassDesiredBefore = false;
	m_iSelectedBootstrapClass = -1;
	m_bSpawnCommandPending = false;
	m_iPendingSpawn = -1;
	m_iPendingSpawnFrames = 0;
	m_flPendingSpawnDeadline = 0.0f;
	m_vSpawnOriginBefore = Vector();
	m_bSpawnOriginBeforeKnown = false;
	m_vSpawnOriginAfter = Vector();
	m_bSpawnOriginAfterKnown = false;
	m_iSpawnStableSamples = 0;
	m_eSpawnConfirmationState = EmpiresBootstrapSpawnConfirmationState::IDLE;
	m_iSpawnConfirmationFrames = 0;
	m_iSpawnConfirmationPressCount = 0;
	m_iSpawnConfirmationReleaseCount = 0;
	m_iSpawnConfirmationExpectedIndex = -1;
	m_iSpawnConfirmationExpectedUserId = -1;
	m_pSpawnConfirmationExpectedEdict = nullptr;
	m_vSpawnConfirmationOrigin = Vector();
	m_bSpawnConfirmationOriginKnown = false;
	m_iSpawnConfirmationStableSamples = 0;
}

bool CBotEmpires::attachBootstrapEdict(edict_t* pEdict)
{
	if (pEdict == nullptr || pEdict->IsFree() || engine == nullptr || gpGlobals == nullptr)
	{
		return false;
	}

	const int index = engine->IndexOfEdict(pEdict);
	if (index <= 0 || index > gpGlobals->maxClients)
	{
		return false;
	}

	// Reset the base state without spawnInit(), setup(), PlayerInfo caching,
	// schedules, navigation or weapons. The edict is attached explicitly below.
	CBot::init(false);
	m_pEdict = pEdict;
	m_bUsed = true;
	m_fTimeCreated = engine->Time();
	m_pController = g_pBotManager != nullptr ? g_pBotManager->GetBotController(pEdict) : nullptr;
	m_pPlayerInfo = nullptr;
	std::strncpy(m_szBotName, "EmpiresBootstrap", sizeof(m_szBotName) - 1);
	m_szBotName[sizeof(m_szBotName) - 1] = '\0';
	resetControlledTestState();
	m_bBootstrapOnly = true;

	// Diagnostics are intentionally transient: do not store IPlayerInfo and do
	// not query team/class/spawn state in this phase.
	IPlayerInfo* pInfo = playerinfomanager != nullptr ? playerinfomanager->GetPlayerInfo(pEdict) : nullptr;
	const bool connected = pInfo != nullptr && pInfo->IsConnected();
	const bool fakeClient = pInfo != nullptr && pInfo->IsFakeClient();
	logger->Log(LogLevel::INFO,
		"Empires bootstrap lifecycle=attached index=%d edict=%p controller=%s playerinfo=%s connected=%d fake=%d",
		index, static_cast<void*>(pEdict), m_pController != nullptr ? "yes" : "no",
		pInfo != nullptr ? "available" : "unavailable", connected ? 1 : 0, fakeClient ? 1 : 0);

	return true;
}

bool CBotEmpires::attachControllerProbeEdict(edict_t* pEdict, const int slot)
{
	if (pEdict == nullptr || pEdict->IsFree() || engine == nullptr || gpGlobals == nullptr)
		return false;

	const int index = engine->IndexOfEdict(pEdict);
	if (index <= 0 || index > gpGlobals->maxClients || slot < 0 || slot >= RCBOT_MAXPLAYERS || slot != index - 1)
		return false;

	m_eControllerProbeLifecycle = EmpiresControllerProbeLifecycle::ENGINE_BOT_CREATED;
	m_iControllerProbeSlot = slot;
	m_iControllerProbeClientIndex = index;

	// CreateBot() has already performed the engine/GameDLL creation path. The
	// probe deliberately avoids createBotFromEdict(), setEdict(), setup(),
	// spawnInit(), profiles, and all cached player/controller interfaces.
	CBot::init(false);
	m_pEdict = pEdict;
	m_bUsed = true;
	m_fTimeCreated = engine->Time();
	m_pController = nullptr;
	m_pPlayerInfo = nullptr;
	std::strncpy(m_szBotName, "EmpiresControllerProbe", sizeof(m_szBotName) - 1);
	m_szBotName[sizeof(m_szBotName) - 1] = '\0';
	resetControlledTestState();
	m_bControllerProbeOnly = true;
	m_eControllerProbeLifecycle = EmpiresControllerProbeLifecycle::SLOT_BOUND;
	m_iControllerProbeSlot = slot;
	m_iControllerProbeClientIndex = index;

	// Read current state through fresh interfaces for diagnostics only. No
	// pointer returned here is retained by the probe.
	EmpiresBootstrapPlayerState state;
	const bool stateAvailable = readBootstrapPlayerState(state);
	IBotController* pController = g_pBotManager != nullptr ?
		g_pBotManager->GetBotController(pEdict) : nullptr;
	if (!stateAvailable || pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe lifecycle=failed slot=%d index=%d state=%s controller=%s",
			slot, index, stateAvailable ? "available" : "unavailable",
			pController != nullptr ? "yes" : "no");
		CBot::freeAllMemory();
		return false;
	}

	m_eControllerProbeLifecycle = EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED;

	logger->Log(LogLevel::INFO,
		"Empires controller probe lifecycle=%s slot=%d index=%d edict=%p state=%s controller=%s playerinfo=cached=no connected=%d active=%d fake=%d observer=%d dead=%d alive=%d team=%d class=%d class_desired=%d desired_spawn=%d life_state=%d lastcmd=%d",
		controllerProbeLifecycleName(), slot, index, static_cast<void*>(pEdict),
		stateAvailable ? "available" : "unavailable",
		pController != nullptr ? "yes" : "no",
		state.connected ? 1 : 0, state.active ? 1 : 0, state.fakeClient ? 1 : 0,
		state.observer ? 1 : 0, state.dead ? 1 : 0, state.alive ? 1 : 0,
		state.team, state.classId, state.classDesired ? 1 : 0, state.desiredSpawn,
		state.lifeState, state.lastUserCommandKnown ? state.lastUserCommandNumber : -1);

	return true;
}

const char* CBotEmpires::controllerProbeSpawnSequenceStateName() const
{
	switch (m_eControllerProbeSpawnSequenceState)
	{
	case EmpiresControllerProbeSpawnSequenceState::IDLE:
		return "IDLE";
	case EmpiresControllerProbeSpawnSequenceState::JOIN_NF_REQUESTED:
		return "JOIN_NF_REQUESTED";
	case EmpiresControllerProbeSpawnSequenceState::OBSERVE_TEAM:
		return "OBSERVE_TEAM";
	case EmpiresControllerProbeSpawnSequenceState::RIFLEMAN_REQUESTED:
		return "RIFLEMAN_REQUESTED";
	case EmpiresControllerProbeSpawnSequenceState::OBSERVE_CLASS:
		return "OBSERVE_CLASS";
	case EmpiresControllerProbeSpawnSequenceState::SPAWNPOINT_REQUESTED:
		return "SPAWNPOINT_REQUESTED";
	case EmpiresControllerProbeSpawnSequenceState::OBSERVE_SPAWNPOINT:
		return "OBSERVE_SPAWNPOINT";
	case EmpiresControllerProbeSpawnSequenceState::ATTACK_PRESS_PENDING:
		return "ATTACK_PRESS_PENDING";
	case EmpiresControllerProbeSpawnSequenceState::ATTACK_RELEASE_PENDING:
		return "ATTACK_RELEASE_PENDING";
	case EmpiresControllerProbeSpawnSequenceState::OBSERVE_SPAWN:
		return "OBSERVE_SPAWN";
	case EmpiresControllerProbeSpawnSequenceState::COMPLETE:
		return "COMPLETE";
	case EmpiresControllerProbeSpawnSequenceState::FAILED:
		return "FAILED";
	}

	return "UNKNOWN";
}

const char* CBotEmpires::controllerProbeSpawnHoldStateName() const
{
	switch (m_eControllerProbeSpawnHoldState)
	{
	case EmpiresControllerProbeSpawnHoldState::IDLE:
		return "IDLE";
	case EmpiresControllerProbeSpawnHoldState::JOIN_NF_REQUESTED:
		return "JOIN_NF_REQUESTED";
	case EmpiresControllerProbeSpawnHoldState::OBSERVE_TEAM:
		return "OBSERVE_TEAM";
	case EmpiresControllerProbeSpawnHoldState::RIFLEMAN_REQUESTED:
		return "RIFLEMAN_REQUESTED";
	case EmpiresControllerProbeSpawnHoldState::OBSERVE_CLASS:
		return "OBSERVE_CLASS";
	case EmpiresControllerProbeSpawnHoldState::SPAWNPOINT_REQUESTED:
		return "SPAWNPOINT_REQUESTED";
	case EmpiresControllerProbeSpawnHoldState::OBSERVE_SPAWNPOINT:
		return "OBSERVE_SPAWNPOINT";
	case EmpiresControllerProbeSpawnHoldState::WAITING_FOR_READY:
		return "WAITING_FOR_READY";
	case EmpiresControllerProbeSpawnHoldState::ATTACK_FRAME_1:
		return "ATTACK_FRAME_1";
	case EmpiresControllerProbeSpawnHoldState::ATTACK_FRAME_2:
		return "ATTACK_FRAME_2";
	case EmpiresControllerProbeSpawnHoldState::RELEASE_FRAME:
		return "RELEASE_FRAME";
	case EmpiresControllerProbeSpawnHoldState::OBSERVE_RESULT:
		return "OBSERVE_RESULT";
	case EmpiresControllerProbeSpawnHoldState::COMPLETE:
		return "COMPLETE";
	case EmpiresControllerProbeSpawnHoldState::FAILED:
		return "FAILED";
	}

	return "UNKNOWN";
}

bool CBotEmpires::controllerProbeSpawnSequenceIdentityMatches(
	const EmpiresBootstrapPlayerState& state) const
{
	return m_bControllerProbeOnly &&
		state.index == m_iControllerProbeSequenceExpectedIndex &&
		state.userId == m_iControllerProbeSequenceExpectedUserId &&
		state.edict != nullptr && !state.edict->IsFree();
}

bool CBotEmpires::controllerProbeSpawnHoldIdentityMatches(
	const EmpiresBootstrapPlayerState& state) const
{
	return m_bControllerProbeOnly &&
		state.index == m_iControllerProbeHoldExpectedIndex &&
		state.userId == m_iControllerProbeHoldExpectedUserId &&
		state.edict == m_pControllerProbeHoldExpectedEdict &&
		state.edict != nullptr && !state.edict->IsFree();
}

void CBotEmpires::logControllerProbeSpawnSequenceState(const char* phase,
	const EmpiresBootstrapPlayerState& state) const
{
	IBotController* pController = nullptr;
	if (g_pBotManager != nullptr && state.edict != nullptr)
	{
		// This is deliberately reacquired for every log/observation. The probe
		// never stores this controller or the IPlayerInfo returned by the SDK.
		pController = g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict));
	}

	logger->Log(LogLevel::INFO,
		"Empires controller probe spawnsequence phase=%s state=%s index=%d userid=%d edict=%p controller=%s connected=%d active=%d fake=%d observer=%d team=%d class=%d class_known=%d class_desired=%d class_desired_known=%d desired_spawn=%d desired_spawn_known=%d health=%d health_known=%d life_state=%d life_state_known=%d dead=%d alive=%d origin_known=%d origin=(%.1f %.1f %.1f) velocity=unavailable lastcmd_known=%d lastcmd_number=%d lastcmd_tick=%d lastcmd_buttons=0x%x lastcmd_forward=%.1f lastcmd_side=%.1f lastcmd_up=%.1f press_count=%d release_count=%d stage_frames=%d",
		phase != nullptr ? phase : "unknown", controllerProbeSpawnSequenceStateName(),
		state.index, state.userId, static_cast<const void*>(state.edict),
		pController != nullptr ? "available" : "null", state.connected ? 1 : 0,
		state.active ? 1 : 0, state.fakeClient ? 1 : 0, state.observer ? 1 : 0,
		state.team, state.classId, state.classKnown ? 1 : 0,
		state.classDesired ? 1 : 0, state.classDesiredKnown ? 1 : 0,
		state.desiredSpawn, state.desiredSpawnKnown ? 1 : 0, state.health,
		state.healthKnown ? 1 : 0, state.lifeState, state.lifeStateKnown ? 1 : 0,
		state.dead ? 1 : 0, state.alive ? 1 : 0, state.originKnown ? 1 : 0,
		state.origin.x, state.origin.y, state.origin.z,
		state.lastUserCommandKnown ? 1 : 0, state.lastUserCommandNumber,
		state.lastUserCommandTick, state.lastUserCommandButtons,
		state.lastUserCommandForwardMove, state.lastUserCommandSideMove,
		state.lastUserCommandUpMove, m_iControllerProbeSequencePressCount,
		m_iControllerProbeSequenceReleaseCount, m_iControllerProbeSequenceStageFrames);
}

void CBotEmpires::logControllerProbeSpawnHoldState(const char* phase,
	const EmpiresBootstrapPlayerState& state) const
{
	IBotController* pController = nullptr;
	if (g_pBotManager != nullptr && state.edict != nullptr)
	{
		pController = g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict));
	}

	logger->Log(LogLevel::INFO,
		"Empires controller probe spawnhold phase=%s state=%s index=%d userid=%d edict=%p controller=%s connected=%d active=%d fake=%d observer=%d team=%d class=%d class_known=%d class_desired=%d class_desired_known=%d desired_spawn=%d desired_spawn_known=%d health=%d health_known=%d life_state=%d life_state_name=%s life_state_known=%d dead=%d alive=%d respawn_timer_known=%d respawn_timer=%.3f current_time_known=%d current_time=%.3f lastcmd_known=%d lastcmd_number=%d lastcmd_tick=%d lastcmd_buttons=0x%x lastcmd_forward=%.1f lastcmd_side=%.1f lastcmd_up=%.1f attack_count=%d release_count=%d stage_frames=%d observe_frames=%d",
		phase != nullptr ? phase : "unknown", controllerProbeSpawnHoldStateName(),
		state.index, state.userId, static_cast<const void*>(state.edict),
		pController != nullptr ? "available" : "null", state.connected ? 1 : 0,
		state.active ? 1 : 0, state.fakeClient ? 1 : 0, state.observer ? 1 : 0,
		state.team, state.classId, state.classKnown ? 1 : 0,
		state.classDesired ? 1 : 0, state.classDesiredKnown ? 1 : 0,
		state.desiredSpawn, state.desiredSpawnKnown ? 1 : 0, state.health,
		state.healthKnown ? 1 : 0, state.lifeState, lifeStateName(state.lifeState),
		state.lifeStateKnown ? 1 : 0, state.dead ? 1 : 0, state.alive ? 1 : 0,
		state.respawnTimerKnown ? 1 : 0, state.respawnTimer,
		state.currentTimeKnown ? 1 : 0, state.currentTime,
		state.lastUserCommandKnown ? 1 : 0, state.lastUserCommandNumber,
		state.lastUserCommandTick, state.lastUserCommandButtons,
		state.lastUserCommandForwardMove, state.lastUserCommandSideMove,
		state.lastUserCommandUpMove, m_iControllerProbeHoldAttackCount,
		m_iControllerProbeHoldReleaseCount, m_iControllerProbeHoldStageFrames,
		m_iControllerProbeHoldObserveFrames);
}

const char* CBotEmpires::controllerProbeNeutralReadinessStateName() const
{
	switch (m_eControllerProbeNeutralReadinessState)
	{
	case EmpiresControllerProbeNeutralReadinessState::IDLE:
		return "IDLE";
	case EmpiresControllerProbeNeutralReadinessState::JOIN_NF_REQUESTED:
		return "JOIN_NF_REQUESTED";
	case EmpiresControllerProbeNeutralReadinessState::OBSERVE_TEAM:
		return "OBSERVE_TEAM";
	case EmpiresControllerProbeNeutralReadinessState::RIFLEMAN_REQUESTED:
		return "RIFLEMAN_REQUESTED";
	case EmpiresControllerProbeNeutralReadinessState::OBSERVE_CLASS:
		return "OBSERVE_CLASS";
	case EmpiresControllerProbeNeutralReadinessState::SPAWNPOINT_REQUESTED:
		return "SPAWNPOINT_REQUESTED";
	case EmpiresControllerProbeNeutralReadinessState::OBSERVE_SPAWNPOINT:
		return "OBSERVE_SPAWNPOINT";
	case EmpiresControllerProbeNeutralReadinessState::NEUTRAL_SIMULATION:
		return "NEUTRAL_SIMULATION";
	case EmpiresControllerProbeNeutralReadinessState::LIFE_RESPAWNABLE_REACHED:
		return "LIFE_RESPAWNABLE_REACHED";
	case EmpiresControllerProbeNeutralReadinessState::COMPLETE:
		return "COMPLETE";
	case EmpiresControllerProbeNeutralReadinessState::FAILED:
		return "FAILED";
	}

	return "UNKNOWN";
}

bool CBotEmpires::controllerProbeNeutralIdentityMatches(
	const EmpiresBootstrapPlayerState& state) const
{
	return m_bControllerProbeOnly &&
		state.index == m_iControllerProbeNeutralExpectedIndex &&
		state.userId == m_iControllerProbeNeutralExpectedUserId &&
		state.edict == m_pControllerProbeNeutralExpectedEdict &&
		state.edict != nullptr && !state.edict->IsFree();
}

void CBotEmpires::logControllerProbeNeutralReadinessState(const char* phase,
	const EmpiresBootstrapPlayerState& state) const
{
	IBotController* pController = nullptr;
	if (g_pBotManager != nullptr && state.edict != nullptr && !state.edict->IsFree())
		pController = g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict));

	logger->Log(LogLevel::INFO,
		"Empires controller probe neutralreadiness phase=%s state=%s curtime=%.3f tickcount=%d index=%d userid=%d edict=%p controller=%s connected=%d active=%d fake=%d observer=%d dead=%d alive=%d team=%d class=%d class_known=%d desired_class=%d desired_class_known=%d desired_spawn=%d desired_spawn_known=%d health=%d health_known=%d life_state=%d life_state_name=%s life_state_known=%d respawn_timer_known=%d respawn_timer=%.3f lastcmd_known=%d lastcmd_number=%d lastcmd_tick=%d lastcmd_buttons=0x%x lastcmd_forward=%.1f lastcmd_side=%.1f lastcmd_up=%.1f lastcmd_impulse=%d lastcmd_weaponselect=%d submitted=%d max_commands=%d last_submitted_command=%d last_submitted_tick=%d stage_frames=%d",
		phase != nullptr ? phase : "unknown", controllerProbeNeutralReadinessStateName(),
		state.currentTime, gpGlobals != nullptr ? gpGlobals->tickcount : -1,
		state.index, state.userId, static_cast<const void*>(state.edict),
		pController != nullptr ? "available" : "null", state.connected ? 1 : 0,
		state.active ? 1 : 0, state.fakeClient ? 1 : 0, state.observer ? 1 : 0,
		state.dead ? 1 : 0, state.alive ? 1 : 0, state.team, state.classId,
		state.classKnown ? 1 : 0, state.classDesired ? 1 : 0,
		state.classDesiredKnown ? 1 : 0, state.desiredSpawn,
		state.desiredSpawnKnown ? 1 : 0, state.health, state.healthKnown ? 1 : 0,
		state.lifeState, lifeStateName(state.lifeState), state.lifeStateKnown ? 1 : 0,
		state.respawnTimerKnown ? 1 : 0, state.respawnTimer,
		state.lastUserCommandKnown ? 1 : 0, state.lastUserCommandNumber,
		state.lastUserCommandTick, state.lastUserCommandButtons,
		state.lastUserCommandForwardMove, state.lastUserCommandSideMove,
		state.lastUserCommandUpMove, state.lastUserCommandImpulse,
		state.lastUserCommandWeaponSelect, m_iControllerProbeNeutralSubmittedCommands,
		m_iControllerProbeNeutralMaxCommands,
		m_iControllerProbeNeutralLastSubmittedCommand,
		m_iControllerProbeNeutralLastSubmittedTick,
		m_iControllerProbeNeutralStageFrames);
}

void CBotEmpires::recordControllerProbeNeutralReadinessChanges(
	const EmpiresBootstrapPlayerState& state)
{
	if (!m_bControllerProbeOnly || logger == nullptr)
		return;

	if (state.lifeStateKnown)
	{
		if (!m_bControllerProbeNeutralLastLifeStateKnown)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe neutralreadiness life-transition previous=UNKNOWN current=%d current_name=%s curtime=%.3f tickcount=%d command_number=%d",
				state.lifeState, lifeStateName(state.lifeState), state.currentTime,
				gpGlobals != nullptr ? gpGlobals->tickcount : -1,
				state.lastUserCommandNumber);
		}
		else if (m_iControllerProbeNeutralLastLifeState != state.lifeState)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe neutralreadiness life-transition previous=%d previous_name=%s current=%d current_name=%s curtime=%.3f tickcount=%d command_number=%d",
				m_iControllerProbeNeutralLastLifeState,
				lifeStateName(m_iControllerProbeNeutralLastLifeState), state.lifeState,
				lifeStateName(state.lifeState), state.currentTime,
				gpGlobals != nullptr ? gpGlobals->tickcount : -1,
				state.lastUserCommandNumber);
		}
		m_bControllerProbeNeutralLastLifeStateKnown = true;
		m_iControllerProbeNeutralLastLifeState = state.lifeState;
	}

	if (state.respawnTimerKnown && std::isfinite(state.respawnTimer))
	{
		if (!m_bControllerProbeNeutralLastTimerKnown)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe neutralreadiness timer-transition previous=UNKNOWN current=%.3f curtime=%.3f",
				state.respawnTimer, state.currentTime);
		}
		else if (std::fabs(m_flControllerProbeNeutralLastTimer - state.respawnTimer) > 0.0005f)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe neutralreadiness timer-transition previous=%.3f current=%.3f curtime=%.3f",
				m_flControllerProbeNeutralLastTimer, state.respawnTimer, state.currentTime);
		}
		m_bControllerProbeNeutralLastTimerKnown = true;
		m_flControllerProbeNeutralLastTimer = state.respawnTimer;
		if (!m_bControllerProbeNeutralLatestDeadlineKnown ||
			std::fabs(m_flControllerProbeNeutralLatestDeadline - state.respawnTimer) > 0.0005f)
		{
			m_bControllerProbeNeutralLatestDeadlineKnown = true;
			m_flControllerProbeNeutralLatestDeadline = state.respawnTimer;
		}
	}
}

void CBotEmpires::recordControllerProbeReadinessChanges(
	const EmpiresBootstrapPlayerState& state)
{
	if (!m_bControllerProbeReadinessOnly || logger == nullptr)
		return;

	if (state.lifeStateKnown)
	{
		if (!m_bControllerProbeReadinessLastLifeStateKnown)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe readiness life-transition previous=UNKNOWN current=%d current_name=%s curtime=%.3f",
				state.lifeState, lifeStateName(state.lifeState), state.currentTime);
		}
		else if (m_iControllerProbeReadinessLastLifeState != state.lifeState)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe readiness life-transition previous=%d previous_name=%s current=%d current_name=%s curtime=%.3f",
				m_iControllerProbeReadinessLastLifeState,
				lifeStateName(m_iControllerProbeReadinessLastLifeState),
				state.lifeState, lifeStateName(state.lifeState), state.currentTime);
		}
		m_bControllerProbeReadinessLastLifeStateKnown = true;
		m_iControllerProbeReadinessLastLifeState = state.lifeState;
	}

	if (state.respawnTimerKnown && std::isfinite(state.respawnTimer))
	{
		if (!m_bControllerProbeReadinessLastTimerKnown)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe readiness timer-transition previous=UNKNOWN current=%.3f curtime=%.3f",
				state.respawnTimer, state.currentTime);
		}
		else if (std::fabs(m_flControllerProbeReadinessLastTimer - state.respawnTimer) > 0.0005f)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe readiness timer-transition previous=%.3f current=%.3f curtime=%.3f",
				m_flControllerProbeReadinessLastTimer, state.respawnTimer,
				state.currentTime);
		}
		m_bControllerProbeReadinessLastTimerKnown = true;
		m_flControllerProbeReadinessLastTimer = state.respawnTimer;
	}
}

bool CBotEmpires::controllerProbeReadinessTimedOut(
	const EmpiresBootstrapPlayerState& state) const
{
	return m_bControllerProbeReadinessOnly &&
		m_bControllerProbeReadinessStartTimeKnown && state.currentTimeKnown &&
		state.currentTime >=
			m_flControllerProbeReadinessStartTime + kControllerProbeReadinessSafetySeconds;
}

void CBotEmpires::failControllerProbeSpawnSequence(const char* reason)
{
	logger->Log(LogLevel::ERROR,
		"Empires controller probe spawnsequence result=FAILED state=%s reason=%s press_count=%d release_count=%d stage_frames=%d expected_index=%d expected_userid=%d",
		controllerProbeSpawnSequenceStateName(), reason != nullptr ? reason : "unknown",
		m_iControllerProbeSequencePressCount, m_iControllerProbeSequenceReleaseCount,
		m_iControllerProbeSequenceStageFrames,
		m_iControllerProbeSequenceExpectedIndex,
		m_iControllerProbeSequenceExpectedUserId);
	m_eControllerProbeSpawnSequenceState =
		EmpiresControllerProbeSpawnSequenceState::FAILED;
}

void CBotEmpires::failControllerProbeSpawnHoldTest(const char* reason)
{
	logger->Log(LogLevel::ERROR,
		"Empires controller probe spawnhold result=FAILED state=%s reason=%s attack_count=%d release_count=%d stage_frames=%d observe_frames=%d",
		controllerProbeSpawnHoldStateName(), reason != nullptr ? reason : "unknown",
		m_iControllerProbeHoldAttackCount, m_iControllerProbeHoldReleaseCount,
		m_iControllerProbeHoldStageFrames, m_iControllerProbeHoldObserveFrames);
	m_eControllerProbeSpawnHoldState = EmpiresControllerProbeSpawnHoldState::FAILED;
}

void CBotEmpires::failControllerProbeNeutralReadiness(const char* reason)
{
	logger->Log(LogLevel::ERROR,
		"Empires controller probe neutralreadiness result=FAILED state=%s reason=%s submitted=%d max_commands=%d last_submitted_command=%d last_submitted_tick=%d",
		controllerProbeNeutralReadinessStateName(), reason != nullptr ? reason : "unknown",
		m_iControllerProbeNeutralSubmittedCommands,
		m_iControllerProbeNeutralMaxCommands,
		m_iControllerProbeNeutralLastSubmittedCommand,
		m_iControllerProbeNeutralLastSubmittedTick);
	m_eControllerProbeNeutralReadinessState =
		EmpiresControllerProbeNeutralReadinessState::FAILED;
}

bool CBotEmpires::issueControllerProbeClientCommand(const char* command,
	const char* phase)
{
	EmpiresBootstrapPlayerState before;
	const bool holdTestActive =
		m_eControllerProbeSpawnHoldState != EmpiresControllerProbeSpawnHoldState::IDLE;
	const bool identityBeforeMatches = readBootstrapPlayerState(before) &&
		(holdTestActive ? controllerProbeSpawnHoldIdentityMatches(before) :
			controllerProbeSpawnSequenceIdentityMatches(before));
	if (!identityBeforeMatches)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence command=%s rejected: fresh client identity unavailable or changed",
			command != nullptr ? command : "unknown");
		return false;
	}
	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(before.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence command=%s rejected: fresh controller unavailable before dispatch",
			command != nullptr ? command : "unknown");
		return false;
	}

	logControllerProbeSpawnSequenceState(phase != nullptr ? phase : "command-before", before);
	if (helpers == nullptr || command == nullptr || *command == '\0')
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence command rejected: helpers or command unavailable command=%s",
			command != nullptr ? command : "null");
		return false;
	}

	// ClientCommand is the public server-side route used for the three
	// separately observed Empires client commands. No private GameRules call is
	// made here.
	helpers->ClientCommand(const_cast<edict_t*>(before.edict), command);

	EmpiresBootstrapPlayerState after;
	const bool identityAfterMatches = readBootstrapPlayerState(after) &&
		(holdTestActive ? controllerProbeSpawnHoldIdentityMatches(after) :
			controllerProbeSpawnSequenceIdentityMatches(after));
	if (!identityAfterMatches)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence command=%s failed: client identity changed or state unavailable after dispatch",
			command);
		return false;
	}
	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(after.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence command=%s failed: fresh controller unavailable after dispatch",
			command != nullptr ? command : "unknown");
		return false;
	}

	logControllerProbeSpawnSequenceState(phase != nullptr ? phase : "command-after", after);
	return true;
}

bool CBotEmpires::submitControllerProbeUserCmd(const int buttons,
	const char* phase)
{
	if (m_bControllerProbeReadinessOnly)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe readiness rejected an attempted UserCmd phase=%s buttons=0x%x",
			phase != nullptr ? phase : "unknown", buttons);
		return false;
	}

	EmpiresBootstrapPlayerState before;
	const bool holdTestActive =
		m_eControllerProbeSpawnHoldState != EmpiresControllerProbeSpawnHoldState::IDLE;
	const bool identityBeforeMatches = readBootstrapPlayerState(before) &&
		(holdTestActive ? controllerProbeSpawnHoldIdentityMatches(before) :
			controllerProbeSpawnSequenceIdentityMatches(before));
	if (!identityBeforeMatches)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence phase=%s rejected: fresh client identity unavailable or changed before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	if (playerinfomanager == nullptr || g_pBotManager == nullptr ||
		gpGlobals == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence phase=%s rejected: SDK interfaces unavailable before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(
		const_cast<edict_t*>(before.edict));
	IBotController* pController = g_pBotManager->GetBotController(
		const_cast<edict_t*>(before.edict));
	if (pInfo == nullptr || pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence phase=%s rejected: fresh PlayerInfo=%s controller=%s",
			phase != nullptr ? phase : "unknown", pInfo != nullptr ? "available" : "null",
			pController != nullptr ? "available" : "null");
		return false;
	}

	const CBotCmd lastCommand = pInfo->GetLastUserCommand();
	CBotCmd userCommand;
	userCommand.command_number = lastCommand.command_number > 0 ?
		lastCommand.command_number + 1 : 1;
	userCommand.tick_count = gpGlobals->tickcount;
	// Preserve the last public view angles. The probe does not intentionally
	// turn the client and does not use m_vViewAngles from normal bot AI.
	userCommand.viewangles = lastCommand.viewangles;
	userCommand.forwardmove = 0.0f;
	userCommand.sidemove = 0.0f;
	userCommand.upmove = 0.0f;
	userCommand.buttons = buttons;
	userCommand.impulse = 0;
	userCommand.weaponselect = 0;
	userCommand.weaponsubtype = 0;
	userCommand.random_seed = 0;
	userCommand.mousedx = 0;
	userCommand.mousedy = 0;
	userCommand.hasbeenpredicted = false;

	logger->Log(LogLevel::INFO,
		"Empires controller probe spawnsequence submit phase=%s index=%d userid=%d edict=%p command_number=%d tick_count=%d buttons=0x%x forwardmove=%.1f sidemove=%.1f upmove=%.1f impulse=%d weaponselect=%d viewangles=(%.1f %.1f %.1f)",
		phase != nullptr ? phase : "unknown", before.index, before.userId,
		static_cast<const void*>(before.edict), userCommand.command_number,
		userCommand.tick_count, userCommand.buttons, userCommand.forwardmove,
		userCommand.sidemove, userCommand.upmove, userCommand.impulse,
		userCommand.weaponselect, userCommand.viewangles.x,
		userCommand.viewangles.y, userCommand.viewangles.z);

	// This is the only UserCmd path in the sequence. The controller is fresh
	// for this exact frame and the command contains no movement, weapon or view
	// changes.
	pController->RunPlayerMove(&userCommand);

	EmpiresBootstrapPlayerState after;
	const bool identityAfterMatches = readBootstrapPlayerState(after) &&
		(holdTestActive ? controllerProbeSpawnHoldIdentityMatches(after) :
			controllerProbeSpawnSequenceIdentityMatches(after));
	if (!identityAfterMatches)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence phase=%s failed: client identity changed or state unavailable after RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	if (m_eControllerProbeSpawnHoldState != EmpiresControllerProbeSpawnHoldState::IDLE)
		logControllerProbeSpawnHoldState(phase != nullptr ? phase : "usercmd-after", after);
	else
		logControllerProbeSpawnSequenceState(phase != nullptr ? phase : "usercmd-after", after);
	return true;
}

bool CBotEmpires::issueControllerProbeNeutralClientCommand(const char* command,
	const char* phase)
{
	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) ||
		!controllerProbeNeutralIdentityMatches(before))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness command=%s rejected: fresh client identity unavailable or changed",
			command != nullptr ? command : "unknown");
		return false;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(before.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness command=%s rejected: fresh controller unavailable",
			command != nullptr ? command : "unknown");
		return false;
	}

	if (helpers == nullptr || command == nullptr || *command == '\0')
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness command rejected: helpers or command unavailable command=%s",
			command != nullptr ? command : "null");
		return false;
	}

	logControllerProbeNeutralReadinessState(phase != nullptr ? phase : "command-before", before);
	helpers->ClientCommand(const_cast<edict_t*>(before.edict), command);

	EmpiresBootstrapPlayerState after;
	if (!readBootstrapPlayerState(after) ||
		!controllerProbeNeutralIdentityMatches(after))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness command=%s failed: client identity changed or state unavailable after dispatch",
			command);
		return false;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(after.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness command=%s failed: fresh controller unavailable after dispatch",
			command);
		return false;
	}

	logControllerProbeNeutralReadinessState(phase != nullptr ? phase : "command-after", after);
	return true;
}

bool CBotEmpires::submitControllerProbeNeutralUserCmd(const char* phase)
{
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeNeutralReadinessState !=
			EmpiresControllerProbeNeutralReadinessState::NEUTRAL_SIMULATION)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness rejected UserCmd phase=%s outside NEUTRAL_SIMULATION",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	if (m_iControllerProbeNeutralSubmittedCommands >=
		m_iControllerProbeNeutralMaxCommands)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness command cap reached submitted=%d cap=%d",
			m_iControllerProbeNeutralSubmittedCommands,
			m_iControllerProbeNeutralMaxCommands);
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) ||
		!controllerProbeNeutralIdentityMatches(before))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s rejected: fresh client identity unavailable or changed before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	if (before.lifeStateKnown && before.lifeState == LIFE_RESPAWNABLE)
	{
		m_eControllerProbeNeutralReadinessState =
			EmpiresControllerProbeNeutralReadinessState::LIFE_RESPAWNABLE_REACHED;
		logControllerProbeNeutralReadinessState("life-respawnable-reached", before);
		m_eControllerProbeNeutralReadinessState =
			EmpiresControllerProbeNeutralReadinessState::COMPLETE;
		logControllerProbeNeutralReadinessState("complete", before);
		return true;
	}

	if (!before.lifeStateKnown ||
		(before.lifeState != LIFE_DYING && before.lifeState != LIFE_DEAD))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s rejected: lifecycle is not LIFE_DYING/LIFE_DEAD before command life_state=%d",
			phase != nullptr ? phase : "unknown", before.lifeState);
		return false;
	}

	if (before.team != EMP_TEAM_NF ||
		!before.classDesiredKnown || !before.classDesired ||
		!before.desiredSpawnKnown || before.desiredSpawn != 32 ||
		!before.healthKnown || before.health > 0)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s rejected: readiness prerequisites changed team=%d desired_class=%d desired_spawn=%d health=%d",
			phase != nullptr ? phase : "unknown", before.team,
			before.classDesired ? 1 : 0, before.desiredSpawn, before.health);
		return false;
	}

	if (m_iControllerProbeNeutralSubmittedCommands > 0 &&
		before.lastUserCommandNumber != m_iControllerProbeNeutralLastSubmittedCommand)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s rejected: stored command diverged expected=%d actual=%d",
			phase != nullptr ? phase : "unknown",
			m_iControllerProbeNeutralLastSubmittedCommand,
			before.lastUserCommandNumber);
		return false;
	}

	if (playerinfomanager == nullptr || g_pBotManager == nullptr ||
		gpGlobals == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s rejected: SDK interfaces unavailable before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(
		const_cast<edict_t*>(before.edict));
	IBotController* pController = g_pBotManager->GetBotController(
		const_cast<edict_t*>(before.edict));
	if (pInfo == nullptr || pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s rejected: fresh PlayerInfo=%s controller=%s",
			phase != nullptr ? phase : "unknown", pInfo != nullptr ? "available" : "null",
			pController != nullptr ? "available" : "null");
		return false;
	}

	const CBotCmd lastCommand = pInfo->GetLastUserCommand();
	if (lastCommand.command_number != before.lastUserCommandNumber ||
		lastCommand.tick_count != before.lastUserCommandTick)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s rejected: fresh last-command readback changed before construction state_number=%d fresh_number=%d state_tick=%d fresh_tick=%d",
			phase != nullptr ? phase : "unknown", before.lastUserCommandNumber,
			lastCommand.command_number, before.lastUserCommandTick,
			lastCommand.tick_count);
		return false;
	}

	const int nextCommandNumber = lastCommand.command_number > 0 ?
		lastCommand.command_number + 1 : 1;
	if (m_iControllerProbeNeutralSubmittedCommands > 0 &&
		nextCommandNumber != m_iControllerProbeNeutralLastSubmittedCommand + 1)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s rejected: command number is not monotonic previous=%d next=%d",
			phase != nullptr ? phase : "unknown",
			m_iControllerProbeNeutralLastSubmittedCommand, nextCommandNumber);
		return false;
	}

	CBotCmd userCommand;
	userCommand.Reset();
	userCommand.command_number = nextCommandNumber;
	userCommand.tick_count = gpGlobals->tickcount;
	userCommand.viewangles = lastCommand.viewangles;
	userCommand.forwardmove = 0.0f;
	userCommand.sidemove = 0.0f;
	userCommand.upmove = 0.0f;
	userCommand.buttons = 0;
	userCommand.impulse = 0;
	userCommand.weaponselect = 0;
	userCommand.weaponsubtype = 0;
	userCommand.random_seed = 0;
	userCommand.mousedx = 0;
	userCommand.mousedy = 0;
	userCommand.hasbeenpredicted = false;

	const bool logSubmittedCommand =
		m_iControllerProbeNeutralSubmittedCommands < 3 ||
		((m_iControllerProbeNeutralSubmittedCommands + 1) %
			kControllerProbeNeutralReadinessProgressInterval) == 0;
	if (logSubmittedCommand)
	{
		logger->Log(LogLevel::INFO,
			"Empires controller probe neutralreadiness submit phase=%s index=%d userid=%d edict=%p command_number=%d tick_count=%d buttons=0x%x forwardmove=%.1f sidemove=%.1f upmove=%.1f impulse=%d weaponselect=%d viewangles=(%.1f %.1f %.1f)",
			phase != nullptr ? phase : "unknown", before.index, before.userId,
			static_cast<const void*>(before.edict), userCommand.command_number,
			userCommand.tick_count, userCommand.buttons, userCommand.forwardmove,
			userCommand.sidemove, userCommand.upmove, userCommand.impulse,
			userCommand.weaponselect, userCommand.viewangles.x,
			userCommand.viewangles.y, userCommand.viewangles.z);
	}

	// This is the only call site allowed to submit input for the neutral
	// readiness state. It intentionally does not enter CBot::runPlayerMove().
	pController->RunPlayerMove(&userCommand);

	EmpiresBootstrapPlayerState after;
	if (!readBootstrapPlayerState(after) ||
		!controllerProbeNeutralIdentityMatches(after))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s failed: client identity changed or state unavailable after RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(after.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s failed: controller unavailable after RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	const bool commandVisible = after.lastUserCommandKnown &&
		after.lastUserCommandNumber == userCommand.command_number &&
		after.lastUserCommandTick > before.lastUserCommandTick;
	const bool commandNeutral = after.lastUserCommandKnown &&
		after.lastUserCommandButtons == 0 &&
		std::fabs(after.lastUserCommandForwardMove) <= 0.01f &&
		std::fabs(after.lastUserCommandSideMove) <= 0.01f &&
		std::fabs(after.lastUserCommandUpMove) <= 0.01f &&
		after.lastUserCommandImpulse == 0 &&
		after.lastUserCommandWeaponSelect == 0;
	if (!commandVisible || !commandNeutral)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness phase=%s failed: engine readback rejected submitted neutral command visible=%d neutral=%d stored_number=%d stored_tick=%d stored_buttons=0x%x forward=%.1f side=%.1f up=%.1f impulse=%d weaponselect=%d",
			phase != nullptr ? phase : "unknown", commandVisible ? 1 : 0,
			commandNeutral ? 1 : 0, after.lastUserCommandNumber,
			after.lastUserCommandTick, after.lastUserCommandButtons,
			after.lastUserCommandForwardMove, after.lastUserCommandSideMove,
			after.lastUserCommandUpMove, after.lastUserCommandImpulse,
			after.lastUserCommandWeaponSelect);
		return false;
	}

	++m_iControllerProbeNeutralSubmittedCommands;
	m_iControllerProbeNeutralLastSubmittedCommand = userCommand.command_number;
	m_iControllerProbeNeutralLastSubmittedTick = userCommand.tick_count;
	recordControllerProbeNeutralReadinessChanges(after);
	if (m_iControllerProbeNeutralSubmittedCommands -
		m_iControllerProbeNeutralLastProgressLogCommand >=
		kControllerProbeNeutralReadinessProgressInterval)
	{
		logControllerProbeNeutralReadinessState(phase != nullptr ? phase : "usercmd-progress", after);
		m_iControllerProbeNeutralLastProgressLogCommand =
			m_iControllerProbeNeutralSubmittedCommands;
	}

	if (after.lifeStateKnown && after.lifeState == LIFE_RESPAWNABLE)
	{
		m_eControllerProbeNeutralReadinessState =
			EmpiresControllerProbeNeutralReadinessState::LIFE_RESPAWNABLE_REACHED;
		logControllerProbeNeutralReadinessState("life-respawnable-reached", after);
		m_eControllerProbeNeutralReadinessState =
			EmpiresControllerProbeNeutralReadinessState::COMPLETE;
		logControllerProbeNeutralReadinessState("complete", after);
	}
	else if (after.lifeStateKnown && after.lifeState == LIFE_ALIVE)
	{
		failControllerProbeNeutralReadiness(
			"client became alive during neutral simulation");
		return false;
	}
	return true;
}

bool CBotEmpires::requestControllerProbeSpawnHoldTest()
{
	return requestControllerProbeSpawnHoldTestInternal(false);
}

bool CBotEmpires::requestControllerProbeReadinessTest()
{
	return requestControllerProbeSpawnHoldTestInternal(true);
}

bool CBotEmpires::requestControllerProbeNeutralReadiness()
{
	const bool noPendingInput =
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE &&
		m_eControllerProbeSpawnHoldState ==
			EmpiresControllerProbeSpawnHoldState::IDLE &&
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::IDLE &&
		!m_bTeamCommandPending && !m_bClassCommandPending &&
		!m_bSpawnCommandPending &&
		m_eSpawnConfirmationState ==
			EmpiresBootstrapSpawnConfirmationState::IDLE;

	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle != EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr || !noPendingInput)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe neutralreadiness rejected: probe unavailable or another input operation is pending spawnsequence=%s spawnhold=%s neutral=%s lifecycle=%s",
			controllerProbeSpawnSequenceStateName(), controllerProbeSpawnHoldStateName(),
			controllerProbeNeutralReadinessStateName(), controllerProbeLifecycleName());
		return false;
	}

	if (gpGlobals == nullptr || !std::isfinite(gpGlobals->interval_per_tick) ||
		gpGlobals->interval_per_tick <= 0.0f)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness rejected: invalid server interval_per_tick");
		return false;
	}

	EmpiresBootstrapPlayerState initial;
	if (!readBootstrapPlayerState(initial) || !initial.connected ||
		!initial.fakeClient || !initial.active || initial.index <= 0 ||
		initial.userId <= 0 || initial.edict == nullptr ||
		!initial.lifeStateKnown || !initial.healthKnown)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness rejected: initial client is not a valid non-alive fake client index=%d userid=%d connected=%d fake=%d active=%d life_state=%d health=%d",
			initial.index, initial.userId, initial.connected ? 1 : 0,
			initial.fakeClient ? 1 : 0, initial.active ? 1 : 0,
			initial.lifeState, initial.health);
		return false;
	}

	const bool alreadyPrepared = initial.team == EMP_TEAM_NF &&
		initial.classDesiredKnown && initial.classDesired &&
		initial.desiredSpawnKnown && initial.desiredSpawn == 32;
	if (alreadyPrepared &&
		(initial.lifeState == LIFE_ALIVE || initial.health > 0))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness rejected: prepared client is unexpectedly alive team=%d desired_class=%d desired_spawn=%d life_state=%d health=%d",
			initial.team, initial.classDesired ? 1 : 0, initial.desiredSpawn,
			initial.lifeState, initial.health);
		return false;
	}

	IBotController* pController = g_pBotManager != nullptr ?
		g_pBotManager->GetBotController(const_cast<edict_t*>(initial.edict)) : nullptr;
	if (pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe neutralreadiness rejected: fresh controller unavailable index=%d userid=%d edict=%p",
			initial.index, initial.userId, static_cast<const void*>(initial.edict));
		return false;
	}

	m_iControllerProbeNeutralExpectedIndex = initial.index;
	m_iControllerProbeNeutralExpectedUserId = initial.userId;
	m_pControllerProbeNeutralExpectedEdict = initial.edict;
	m_iControllerProbeNeutralStageFrames = 0;
	m_iControllerProbeNeutralSubmittedCommands = 0;
	m_iControllerProbeNeutralMaxCommands = static_cast<int>(std::ceil(
		kControllerProbeNeutralReadinessSafetySeconds /
		gpGlobals->interval_per_tick)) + 2;
	if (m_iControllerProbeNeutralMaxCommands < 1)
		m_iControllerProbeNeutralMaxCommands = 1;
	if (m_iControllerProbeNeutralMaxCommands >
		kControllerProbeNeutralReadinessHardCommandCap)
		m_iControllerProbeNeutralMaxCommands =
			kControllerProbeNeutralReadinessHardCommandCap;
	m_iControllerProbeNeutralLastSubmittedTick = -1;
	m_iControllerProbeNeutralLastSubmittedCommand = 0;
	m_iControllerProbeNeutralLastProgressLogCommand = 0;
	m_bControllerProbeNeutralStartTimeKnown = initial.currentTimeKnown;
	m_flControllerProbeNeutralStartTime = initial.currentTime;
	m_bControllerProbeNeutralLastLifeStateKnown = false;
	m_iControllerProbeNeutralLastLifeState = -1;
	m_bControllerProbeNeutralLastTimerKnown = false;
	m_flControllerProbeNeutralLastTimer = 0.0f;
	m_bControllerProbeNeutralLatestDeadlineKnown = false;
	m_flControllerProbeNeutralLatestDeadline = 0.0f;
	recordControllerProbeNeutralReadinessChanges(initial);

	m_eControllerProbeNeutralReadinessState =
		EmpiresControllerProbeNeutralReadinessState::JOIN_NF_REQUESTED;
	logControllerProbeNeutralReadinessState("initial", initial);

	if (alreadyPrepared)
	{
		m_eControllerProbeNeutralReadinessState =
			EmpiresControllerProbeNeutralReadinessState::NEUTRAL_SIMULATION;
		logger->Log(LogLevel::INFO,
			"Empires controller probe neutralreadiness armed from existing NF/Rifleman/spawnpoint setup interval_per_tick=%.6f max_commands=%d start=%.3f",
			gpGlobals->interval_per_tick, m_iControllerProbeNeutralMaxCommands,
			m_flControllerProbeNeutralStartTime);
		return true;
	}

	logger->Log(LogLevel::INFO,
		"Empires controller probe neutralreadiness setup state=%s command=jointeam 2 requested_class=1 requested_spawn=32 interval_per_tick=%.6f max_commands=%d",
		controllerProbeNeutralReadinessStateName(), gpGlobals->interval_per_tick,
		m_iControllerProbeNeutralMaxCommands);
	if (!issueControllerProbeNeutralClientCommand("jointeam 2\n",
		"jointeam-before-after"))
	{
		failControllerProbeNeutralReadiness(
			"jointeam 2 could not be dispatched with stable client identity");
		return false;
	}

	EmpiresBootstrapPlayerState afterJoin;
	if (!readBootstrapPlayerState(afterJoin) ||
		!controllerProbeNeutralIdentityMatches(afterJoin))
	{
		failControllerProbeNeutralReadiness("fresh state unavailable after jointeam 2");
		return false;
	}
	recordControllerProbeNeutralReadinessChanges(afterJoin);
	m_eControllerProbeNeutralReadinessState =
		EmpiresControllerProbeNeutralReadinessState::OBSERVE_TEAM;
	m_iControllerProbeNeutralStageFrames = 0;
	logControllerProbeNeutralReadinessState("after-jointeam-dispatch", afterJoin);
	return true;
}

void CBotEmpires::observeControllerProbeNeutralReadiness()
{
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::IDLE ||
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::COMPLETE ||
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::FAILED)
	{
		return;
	}

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state) ||
		!controllerProbeNeutralIdentityMatches(state))
	{
		failControllerProbeNeutralReadiness(
			"fresh client identity unavailable or changed during neutral-readiness test");
		return;
	}
	recordControllerProbeNeutralReadinessChanges(state);
	++m_iControllerProbeNeutralStageFrames;

	if (!state.connected || !state.active || state.observer || !state.fakeClient ||
		!state.lifeStateKnown || !state.healthKnown || !state.currentTimeKnown)
	{
		failControllerProbeNeutralReadiness(
			"client state was not fully available before the next test step");
		return;
	}

	if (m_bControllerProbeNeutralStartTimeKnown &&
		state.currentTime >= m_flControllerProbeNeutralStartTime +
			kControllerProbeNeutralReadinessSafetySeconds)
	{
		logControllerProbeNeutralReadinessState("safety-timeout", state);
		failControllerProbeNeutralReadiness(
			"20-second neutral-readiness safety timeout reached");
		return;
	}

	if (state.lifeState == LIFE_RESPAWNABLE)
	{
		if (state.team != EMP_TEAM_NF || !state.classDesiredKnown ||
			!state.classDesired || !state.desiredSpawnKnown ||
			state.desiredSpawn != 32 || state.health > 0)
		{
			failControllerProbeNeutralReadiness(
				"LIFE_RESPAWNABLE was reached with invalid NF/Rifleman/spawnpoint state");
			return;
		}

		m_eControllerProbeNeutralReadinessState =
			EmpiresControllerProbeNeutralReadinessState::LIFE_RESPAWNABLE_REACHED;
		logControllerProbeNeutralReadinessState("life-respawnable-reached", state);
		m_eControllerProbeNeutralReadinessState =
			EmpiresControllerProbeNeutralReadinessState::COMPLETE;
		logControllerProbeNeutralReadinessState("complete", state);
		return;
	}

	if (state.lifeState == LIFE_ALIVE || state.health > 0)
	{
		failControllerProbeNeutralReadiness(
			"client became alive without an allowed spawn operation");
		return;
	}

	switch (m_eControllerProbeNeutralReadinessState)
	{
	case EmpiresControllerProbeNeutralReadinessState::OBSERVE_TEAM:
		if (state.team == EMP_TEAM_NF && state.lifeState != LIFE_DYING &&
			state.lifeState != LIFE_DEAD)
		{
			failControllerProbeNeutralReadiness(
				"NF client did not remain in LIFE_DYING or LIFE_DEAD during setup");
			return;
		}
		if (state.team == EMP_TEAM_NF &&
			m_iControllerProbeNeutralStageFrames >=
				kControllerProbeNeutralReadinessSetupSamples)
		{
			m_eControllerProbeNeutralReadinessState =
				EmpiresControllerProbeNeutralReadinessState::RIFLEMAN_REQUESTED;
			m_iControllerProbeNeutralStageFrames = 0;
			logger->Log(LogLevel::INFO,
				"Empires controller probe neutralreadiness setup state=%s command=changeclass 1",
				controllerProbeNeutralReadinessStateName());
			if (!issueControllerProbeNeutralClientCommand("changeclass 1\n",
				"changeclass-before-after"))
			{
				failControllerProbeNeutralReadiness(
					"changeclass 1 could not be dispatched with stable client identity");
				return;
			}

			EmpiresBootstrapPlayerState afterClass;
			if (!readBootstrapPlayerState(afterClass) ||
				!controllerProbeNeutralIdentityMatches(afterClass))
			{
				failControllerProbeNeutralReadiness(
					"fresh state unavailable after changeclass 1");
				return;
			}
			recordControllerProbeNeutralReadinessChanges(afterClass);
			if (afterClass.lifeState == LIFE_ALIVE || afterClass.health > 0)
			{
				failControllerProbeNeutralReadiness(
					"unexpected automatic spawn after changeclass 1");
				return;
			}
			m_eControllerProbeNeutralReadinessState =
				EmpiresControllerProbeNeutralReadinessState::OBSERVE_CLASS;
			logControllerProbeNeutralReadinessState("after-changeclass-dispatch", afterClass);
		}
		return;

	case EmpiresControllerProbeNeutralReadinessState::OBSERVE_CLASS:
		if (state.team != EMP_TEAM_NF ||
			(state.lifeState != LIFE_DYING && state.lifeState != LIFE_DEAD))
		{
			failControllerProbeNeutralReadiness(
				"client left the non-alive NF state while observing class selection");
			return;
		}
		if (controllerProbeRiflemanAcknowledged(state) &&
			m_iControllerProbeNeutralStageFrames >=
				kControllerProbeNeutralReadinessSetupSamples)
		{
			m_eControllerProbeNeutralReadinessState =
				EmpiresControllerProbeNeutralReadinessState::SPAWNPOINT_REQUESTED;
			m_iControllerProbeNeutralStageFrames = 0;
			logger->Log(LogLevel::INFO,
				"Empires controller probe neutralreadiness setup state=%s command=emp_spawn_select 32 class_acknowledged=1",
				controllerProbeNeutralReadinessStateName());
			if (!issueControllerProbeNeutralClientCommand("emp_spawn_select 32\n",
				"spawnpoint-before-after"))
			{
				failControllerProbeNeutralReadiness(
					"emp_spawn_select 32 could not be dispatched with stable client identity");
				return;
			}

			EmpiresBootstrapPlayerState afterSpawn;
			if (!readBootstrapPlayerState(afterSpawn) ||
				!controllerProbeNeutralIdentityMatches(afterSpawn))
			{
				failControllerProbeNeutralReadiness(
					"fresh state unavailable after emp_spawn_select 32");
				return;
			}
			recordControllerProbeNeutralReadinessChanges(afterSpawn);
			if (afterSpawn.lifeState == LIFE_ALIVE || afterSpawn.health > 0)
			{
				failControllerProbeNeutralReadiness(
					"unexpected automatic spawn after emp_spawn_select 32");
				return;
			}
			m_eControllerProbeNeutralReadinessState =
				EmpiresControllerProbeNeutralReadinessState::OBSERVE_SPAWNPOINT;
			logControllerProbeNeutralReadinessState("after-spawnpoint-dispatch", afterSpawn);
		}
		return;

	case EmpiresControllerProbeNeutralReadinessState::OBSERVE_SPAWNPOINT:
		if (state.team != EMP_TEAM_NF ||
			(state.lifeState != LIFE_DYING && state.lifeState != LIFE_DEAD) ||
			!controllerProbeRiflemanAcknowledged(state) ||
			!state.desiredSpawnKnown || state.desiredSpawn != 32)
		{
			failControllerProbeNeutralReadiness(
				"spawn-point setup lost NF/non-alive/Rifleman/spawnpoint readiness");
			return;
		}
		if (m_iControllerProbeNeutralStageFrames >=
			kControllerProbeNeutralReadinessSetupSamples)
		{
			m_eControllerProbeNeutralReadinessState =
				EmpiresControllerProbeNeutralReadinessState::NEUTRAL_SIMULATION;
			m_iControllerProbeNeutralStageFrames = 0;
			logger->Log(LogLevel::INFO,
				"Empires controller probe neutralreadiness transition state=%s start=%.3f interval_per_tick=%.6f max_commands=%d",
				controllerProbeNeutralReadinessStateName(), state.currentTime,
				gpGlobals->interval_per_tick, m_iControllerProbeNeutralMaxCommands);
		}
		return;

	case EmpiresControllerProbeNeutralReadinessState::NEUTRAL_SIMULATION:
		if (state.team != EMP_TEAM_NF ||
			(state.lifeState != LIFE_DYING && state.lifeState != LIFE_DEAD) ||
			!controllerProbeRiflemanAcknowledged(state) ||
			!state.desiredSpawnKnown || state.desiredSpawn != 32 ||
			!state.lastUserCommandKnown || state.lastUserCommandButtons != 0 ||
			std::fabs(state.lastUserCommandForwardMove) > 0.01f ||
			std::fabs(state.lastUserCommandSideMove) > 0.01f ||
			std::fabs(state.lastUserCommandUpMove) > 0.01f ||
			state.lastUserCommandImpulse != 0 ||
			state.lastUserCommandWeaponSelect != 0)
		{
			failControllerProbeNeutralReadiness(
				"non-neutral input or readiness state observed before next command");
			return;
		}

		if (gpGlobals == nullptr || gpGlobals->tickcount ==
			m_iControllerProbeNeutralLastSubmittedTick)
		{
			return;
		}

		if (!submitControllerProbeNeutralUserCmd("neutral-frame"))
		{
			if (m_eControllerProbeNeutralReadinessState !=
				EmpiresControllerProbeNeutralReadinessState::FAILED)
			{
				failControllerProbeNeutralReadiness(
					"neutral RunPlayerMove submission or readback failed");
			}
			return;
		}
		return;

	case EmpiresControllerProbeNeutralReadinessState::IDLE:
	case EmpiresControllerProbeNeutralReadinessState::JOIN_NF_REQUESTED:
	case EmpiresControllerProbeNeutralReadinessState::RIFLEMAN_REQUESTED:
	case EmpiresControllerProbeNeutralReadinessState::SPAWNPOINT_REQUESTED:
	case EmpiresControllerProbeNeutralReadinessState::LIFE_RESPAWNABLE_REACHED:
	case EmpiresControllerProbeNeutralReadinessState::COMPLETE:
	case EmpiresControllerProbeNeutralReadinessState::FAILED:
		return;
	}
}

const char* CBotEmpires::controllerProbeLoadoutReadinessStateName() const
{
	switch (m_eControllerProbeLoadoutReadinessState)
	{
	case EmpiresControllerProbeLoadoutReadinessState::IDLE:
		return "IDLE";
	case EmpiresControllerProbeLoadoutReadinessState::JOIN_NF_REQUESTED:
		return "JOIN_NF_REQUESTED";
	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_TEAM:
		return "OBSERVE_TEAM";
	case EmpiresControllerProbeLoadoutReadinessState::SET_SIDEARM:
		return "SET_SIDEARM";
	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_SIDEARM:
		return "OBSERVE_SIDEARM";
	case EmpiresControllerProbeLoadoutReadinessState::SET_PRIMARY:
		return "SET_PRIMARY";
	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_PRIMARY:
		return "OBSERVE_PRIMARY";
	case EmpiresControllerProbeLoadoutReadinessState::SET_GRENADE:
		return "SET_GRENADE";
	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_GRENADE:
		return "OBSERVE_GRENADE";
	case EmpiresControllerProbeLoadoutReadinessState::SET_EQUIPMENT:
		return "SET_EQUIPMENT";
	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_EQUIPMENT:
		return "OBSERVE_EQUIPMENT";
	case EmpiresControllerProbeLoadoutReadinessState::REQUEST_RIFLEMAN:
		return "REQUEST_RIFLEMAN";
	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_CLASS:
		return "OBSERVE_CLASS";
	case EmpiresControllerProbeLoadoutReadinessState::SELECT_SPAWNPOINT:
		return "SELECT_SPAWNPOINT";
	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_SPAWNPOINT:
		return "OBSERVE_SPAWNPOINT";
	case EmpiresControllerProbeLoadoutReadinessState::NEUTRAL_SIMULATION:
		return "NEUTRAL_SIMULATION";
	case EmpiresControllerProbeLoadoutReadinessState::LIFE_RESPAWNABLE_REACHED:
		return "LIFE_RESPAWNABLE_REACHED";
	case EmpiresControllerProbeLoadoutReadinessState::COMPLETE:
		return "COMPLETE";
	case EmpiresControllerProbeLoadoutReadinessState::FAILED:
		return "FAILED";
	}

	return "UNKNOWN";
}

const char* CBotEmpires::controllerProbeAppliedLoadoutReadinessStateName() const
{
	switch (m_eControllerProbeAppliedLoadoutReadinessState)
	{
	case EmpiresControllerProbeAppliedLoadoutReadinessState::IDLE:
		return "IDLE";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::JOIN_NF:
		return "JOIN_NF";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_TEAM:
		return "OBSERVE_TEAM";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::REQUEST_RIFLEMAN:
		return "REQUEST_RIFLEMAN";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_CLASS_REQUEST:
		return "OBSERVE_CLASS_REQUEST";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SET_SIDEARM:
		return "SET_SIDEARM";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_SIDEARM:
		return "OBSERVE_SIDEARM";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SET_PRIMARY:
		return "SET_PRIMARY";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_PRIMARY:
		return "OBSERVE_PRIMARY";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SET_GRENADE:
		return "SET_GRENADE";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_GRENADE:
		return "OBSERVE_GRENADE";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SET_EQUIPMENT:
		return "SET_EQUIPMENT";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_APPLIED_LOADOUT:
		return "OBSERVE_APPLIED_LOADOUT";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SELECT_SPAWN:
		return "SELECT_SPAWN";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_SPAWN:
		return "OBSERVE_SPAWN";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::NEUTRAL_SIMULATION:
		return "NEUTRAL_SIMULATION";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::LIFE_RESPAWNABLE_REACHED:
		return "LIFE_RESPAWNABLE_REACHED";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::COMPLETE:
		return "COMPLETE";
	case EmpiresControllerProbeAppliedLoadoutReadinessState::FAILED:
		return "FAILED";
	}

	return "UNKNOWN";
}

const char* CBotEmpires::controllerProbeSpawnConfirmReadyStateName() const
{
	switch (m_eControllerProbeSpawnConfirmReadyState)
	{
	case EmpiresControllerProbeSpawnConfirmReadyState::IDLE:
		return "IDLE";
	case EmpiresControllerProbeSpawnConfirmReadyState::ATTACK_PRESS_PENDING:
		return "ATTACK_PRESS_PENDING";
	case EmpiresControllerProbeSpawnConfirmReadyState::ATTACK_RELEASE_PENDING:
		return "ATTACK_RELEASE_PENDING";
	case EmpiresControllerProbeSpawnConfirmReadyState::OBSERVE_SPAWN:
		return "OBSERVE_SPAWN";
	case EmpiresControllerProbeSpawnConfirmReadyState::COMPLETE:
		return "COMPLETE";
	case EmpiresControllerProbeSpawnConfirmReadyState::FAILED:
		return "FAILED";
	}

	return "UNKNOWN";
}

const char* CBotEmpires::controllerProbeMovementStateName() const
{
	switch (m_eControllerProbeMovementState)
	{
	case EmpiresControllerProbeMovementState::IDLE:
		return "IDLE";
	case EmpiresControllerProbeMovementState::MOVE_START_PENDING:
		return "MOVE_START_PENDING";
	case EmpiresControllerProbeMovementState::MOVE_ACTIVE:
		return "MOVE_ACTIVE";
	case EmpiresControllerProbeMovementState::MOVE_STOP_PENDING:
		return "MOVE_STOP_PENDING";
	case EmpiresControllerProbeMovementState::FINAL_OBSERVE:
		return "FINAL_OBSERVE";
	case EmpiresControllerProbeMovementState::COMPLETE:
		return "COMPLETE";
	case EmpiresControllerProbeMovementState::FAILED:
		return "FAILED";
	}

	return "UNKNOWN";
}

const char* CBotEmpires::controllerProbeWaypointStateName() const
{
	switch (m_eControllerProbeWaypointState)
	{
	case EmpiresControllerProbeWaypointState::IDLE:
		return "IDLE";
	case EmpiresControllerProbeWaypointState::TARGET_SELECTED:
		return "TARGET_SELECTED";
	case EmpiresControllerProbeWaypointState::MOVE_ACTIVE:
		return "MOVE_ACTIVE";
	case EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING:
		return "MOVE_STOP_PENDING";
	case EmpiresControllerProbeWaypointState::FINAL_OBSERVE:
		return "FINAL_OBSERVE";
	case EmpiresControllerProbeWaypointState::COMPLETE:
		return "COMPLETE";
	case EmpiresControllerProbeWaypointState::FAILED:
		return "FAILED";
	}

	return "UNKNOWN";
}

bool CBotEmpires::controllerProbeAppliedLoadoutIdentityMatches(
	const EmpiresBootstrapPlayerState& state) const
{
	return m_bControllerProbeOnly &&
		state.index == m_iControllerProbeAppliedLoadoutExpectedIndex &&
		state.userId == m_iControllerProbeAppliedLoadoutExpectedUserId &&
		state.edict == m_pControllerProbeAppliedLoadoutExpectedEdict &&
		state.edict != nullptr && !state.edict->IsFree();
}

bool CBotEmpires::controllerProbeSpawnConfirmReadyIdentityMatches(
	const EmpiresBootstrapPlayerState& state) const
{
	return m_bControllerProbeOnly &&
		state.index == m_iControllerProbeSpawnConfirmReadyExpectedIndex &&
		state.userId == m_iControllerProbeSpawnConfirmReadyExpectedUserId &&
		state.edict == m_pControllerProbeSpawnConfirmReadyExpectedEdict &&
		state.edict != nullptr && !state.edict->IsFree();
}

void CBotEmpires::logControllerProbeSpawnConfirmReadyState(
	const char* phase, const EmpiresBootstrapPlayerState& state) const
{
	IBotController* pController = nullptr;
	if (g_pBotManager != nullptr && state.edict != nullptr && !state.edict->IsFree())
		pController = g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict));

	logger->Log(LogLevel::INFO,
		"Empires controller probe spawnconfirm_ready phase=%s state=%s curtime=%.3f tickcount=%d index=%d userid=%d edict=%p controller=%s connected=%d active=%d fake=%d observer=%d team=%d applied_class=%d applied_class_known=%d class_desired=%d class_desired_known=%d sidearm=%d sidearm_known=%d sidearm_desired=%d sidearm_desired_known=%d primary=%d primary_known=%d primary_desired=%d primary_desired_known=%d grenade=%d grenade_known=%d grenade_desired=%d grenade_desired_known=%d equipment=%d equipment_known=%d equipment_desired=%d equipment_desired_known=%d desired_spawn=%d desired_spawn_known=%d health=%d health_known=%d life_state=%d life_state_name=%s life_state_known=%d playerinfo_dead=%d semantic_dead=%d alive=%d origin_known=%d origin=(%.1f %.1f %.1f) velocity_known=%d velocity=(%.1f %.1f %.1f) lastcmd_known=%d lastcmd_number=%d lastcmd_tick=%d lastcmd_buttons=0x%x lastcmd_forward=%.1f lastcmd_side=%.1f lastcmd_up=%.1f lastcmd_impulse=%d lastcmd_weaponselect=%d attack_count=%d release_count=%d observe_frames=%d",
		phase != nullptr ? phase : "unknown", controllerProbeSpawnConfirmReadyStateName(),
		state.currentTime, gpGlobals != nullptr ? gpGlobals->tickcount : -1,
		state.index, state.userId, static_cast<const void*>(state.edict),
		pController != nullptr ? "available" : "null", state.connected ? 1 : 0,
		state.active ? 1 : 0, state.fakeClient ? 1 : 0, state.observer ? 1 : 0,
		state.team, state.classId, state.classKnown ? 1 : 0,
		state.classDesired ? 1 : 0, state.classDesiredKnown ? 1 : 0,
		state.appliedSidearm, state.appliedSidearmKnown ? 1 : 0,
		state.sidearmDesired ? 1 : 0, state.sidearmDesiredKnown ? 1 : 0,
		state.appliedPrimary, state.appliedPrimaryKnown ? 1 : 0,
		state.primaryDesired ? 1 : 0, state.primaryDesiredKnown ? 1 : 0,
		state.appliedGrenade, state.appliedGrenadeKnown ? 1 : 0,
		state.grenadeDesired ? 1 : 0, state.grenadeDesiredKnown ? 1 : 0,
		state.appliedEquipment, state.appliedEquipmentKnown ? 1 : 0,
		state.equipmentDesired ? 1 : 0, state.equipmentDesiredKnown ? 1 : 0,
		state.desiredSpawn, state.desiredSpawnKnown ? 1 : 0,
		state.health, state.healthKnown ? 1 : 0, state.lifeState,
		lifeStateName(state.lifeState), state.lifeStateKnown ? 1 : 0,
		state.dead ? 1 : 0,
		state.lifeStateKnown && state.healthKnown &&
			state.lifeState == LIFE_RESPAWNABLE && state.health == 0 ? 1 : 0,
		state.alive ? 1 : 0, state.originKnown ? 1 : 0,
		state.origin.x, state.origin.y, state.origin.z,
		state.velocityKnown ? 1 : 0, state.velocity.x, state.velocity.y,
		state.velocity.z, state.lastUserCommandKnown ? 1 : 0,
		state.lastUserCommandNumber, state.lastUserCommandTick,
		state.lastUserCommandButtons, state.lastUserCommandForwardMove,
		state.lastUserCommandSideMove, state.lastUserCommandUpMove,
		state.lastUserCommandImpulse, state.lastUserCommandWeaponSelect,
		m_iControllerProbeSpawnConfirmReadyPressCount,
		m_iControllerProbeSpawnConfirmReadyReleaseCount,
		m_iControllerProbeSpawnConfirmReadyObserveFrames);
}

bool CBotEmpires::controllerProbeMovementIdentityMatches(
	const EmpiresBootstrapPlayerState& state) const
{
	return m_bControllerProbeOnly &&
		state.index == m_iControllerProbeMovementExpectedIndex &&
		state.userId == m_iControllerProbeMovementExpectedUserId &&
		state.edict == m_pControllerProbeMovementExpectedEdict &&
		state.edict != nullptr && !state.edict->IsFree();
}

bool CBotEmpires::controllerProbeWaypointIdentityMatches(
	const EmpiresBootstrapPlayerState& state) const
{
	return m_bControllerProbeOnly &&
		state.index == m_iControllerProbeWaypointExpectedIndex &&
		state.userId == m_iControllerProbeWaypointExpectedUserId &&
		state.edict == m_pControllerProbeWaypointExpectedEdict &&
		state.edict != nullptr && !state.edict->IsFree();
}

void CBotEmpires::logControllerProbeMovementState(
	const char* phase, const EmpiresBootstrapPlayerState& state) const
{
	IBotController* pController = nullptr;
	if (g_pBotManager != nullptr && state.edict != nullptr && !state.edict->IsFree())
		pController = g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict));

	const Vector currentOrigin = state.originKnown ? state.origin : Vector();
	const Vector finalOrigin = m_bControllerProbeMovementFinalOriginKnown ?
		m_vControllerProbeMovementFinalOrigin : currentOrigin;
	const float displacement = m_bControllerProbeMovementStartOriginKnown &&
		(state.originKnown || m_bControllerProbeMovementFinalOriginKnown) ?
		std::sqrt((finalOrigin - m_vControllerProbeMovementStartOrigin).LengthSqr()) : -1.0f;

	logger->Log(LogLevel::INFO,
		"Empires controller probe movement phase=%s state=%s curtime=%.3f tickcount=%d index=%d userid=%d edict=%p controller=%s alive=%d dead=%d health=%d life_state=%d origin_known=%d origin=(%.1f %.1f %.1f) start_known=%d start=(%.1f %.1f %.1f) final_known=%d final=(%.1f %.1f %.1f) displacement=%.2f active_frames=%d final_observe_frames=%d submitted=%d lastcmd=%d lastcmd_tick=%d lastcmd_buttons=0x%x lastcmd_forward=%.1f lastcmd_side=%.1f lastcmd_up=%.1f viewangles=(%.1f %.1f %.1f)",
		phase != nullptr ? phase : "unknown", controllerProbeMovementStateName(),
		state.currentTime, gpGlobals != nullptr ? gpGlobals->tickcount : -1,
		state.index, state.userId, static_cast<const void*>(state.edict),
		pController != nullptr ? "available" : "null", state.alive ? 1 : 0,
		state.dead ? 1 : 0, state.health, state.lifeState,
		state.originKnown ? 1 : 0, state.origin.x, state.origin.y, state.origin.z,
		m_bControllerProbeMovementStartOriginKnown ? 1 : 0,
		m_vControllerProbeMovementStartOrigin.x,
		m_vControllerProbeMovementStartOrigin.y,
		m_vControllerProbeMovementStartOrigin.z,
		m_bControllerProbeMovementFinalOriginKnown ? 1 : 0,
		finalOrigin.x, finalOrigin.y, finalOrigin.z, displacement,
		m_iControllerProbeMovementActiveFrames,
		m_iControllerProbeMovementFinalObserveFrames,
		m_iControllerProbeMovementSubmittedCommands,
		state.lastUserCommandKnown ? state.lastUserCommandNumber : -1,
		state.lastUserCommandKnown ? state.lastUserCommandTick : -1,
		state.lastUserCommandKnown ? state.lastUserCommandButtons : 0,
		state.lastUserCommandKnown ? state.lastUserCommandForwardMove : 0.0f,
		state.lastUserCommandKnown ? state.lastUserCommandSideMove : 0.0f,
		state.lastUserCommandKnown ? state.lastUserCommandUpMove : 0.0f,
		m_vControllerProbeMovementViewAngles.x,
		m_vControllerProbeMovementViewAngles.y,
		m_vControllerProbeMovementViewAngles.z);
}

void CBotEmpires::failControllerProbeMovement(const char* reason)
{
	logger->Log(LogLevel::ERROR,
		"Empires controller probe movement result=FAIL state=%s reason=%s active_frames=%d final_observe_frames=%d submitted=%d start_known=%d final_known=%d",
		controllerProbeMovementStateName(), reason != nullptr ? reason : "unknown",
		m_iControllerProbeMovementActiveFrames,
		m_iControllerProbeMovementFinalObserveFrames,
		m_iControllerProbeMovementSubmittedCommands,
		m_bControllerProbeMovementStartOriginKnown ? 1 : 0,
		m_bControllerProbeMovementFinalOriginKnown ? 1 : 0);
	m_eControllerProbeMovementState =
		EmpiresControllerProbeMovementState::FAILED;
}

void CBotEmpires::logControllerProbeWaypointState(
	const char* phase, const EmpiresBootstrapPlayerState& state) const
{
	IBotController* pController = nullptr;
	if (g_pBotManager != nullptr && state.edict != nullptr && !state.edict->IsFree())
		pController = g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict));

	const Vector currentOrigin = state.originKnown ? state.origin : Vector();
	const Vector finalOrigin = m_bControllerProbeWaypointFinalOriginKnown ?
		m_vControllerProbeWaypointFinalOrigin : currentOrigin;
	const float targetDistance = m_bControllerProbeWaypointTargetKnown && state.originKnown ?
		std::sqrt((currentOrigin - m_vControllerProbeWaypointTargetOrigin).LengthSqr()) : -1.0f;
	const float displacement = m_bControllerProbeWaypointStartOriginKnown &&
		(state.originKnown || m_bControllerProbeWaypointFinalOriginKnown) ?
		std::sqrt((finalOrigin - m_vControllerProbeWaypointStartOrigin).LengthSqr()) : -1.0f;

	logger->Log(LogLevel::INFO,
		"Empires controller probe waypoint phase=%s state=%s curtime=%.3f tickcount=%d index=%d userid=%d edict=%p controller=%s alive=%d dead=%d health=%d life_state=%d origin=(%.1f %.1f %.1f) start=(%.1f %.1f %.1f) target_id=%d target=(%.1f %.1f %.1f) target_distance=%.2f best_distance=%.2f final=(%.1f %.1f %.1f) displacement=%.2f active_frames=%d final_observe_frames=%d submitted=%d no_progress_frames=%d lastcmd=%d lastcmd_tick=%d lastcmd_buttons=0x%x lastcmd_forward=%.1f viewangles=(%.1f %.1f %.1f)",
		phase != nullptr ? phase : "unknown", controllerProbeWaypointStateName(),
		state.currentTime, gpGlobals != nullptr ? gpGlobals->tickcount : -1,
		state.index, state.userId, static_cast<const void*>(state.edict),
		pController != nullptr ? "available" : "null", state.alive ? 1 : 0,
		state.dead ? 1 : 0, state.health, state.lifeState,
		currentOrigin.x, currentOrigin.y, currentOrigin.z,
		m_vControllerProbeWaypointStartOrigin.x,
		m_vControllerProbeWaypointStartOrigin.y,
		m_vControllerProbeWaypointStartOrigin.z,
		m_iControllerProbeWaypointId,
		m_vControllerProbeWaypointTargetOrigin.x,
		m_vControllerProbeWaypointTargetOrigin.y,
		m_vControllerProbeWaypointTargetOrigin.z, targetDistance,
		m_flControllerProbeWaypointBestDistance, finalOrigin.x,
		finalOrigin.y, finalOrigin.z, displacement,
		m_iControllerProbeWaypointActiveFrames,
		m_iControllerProbeWaypointFinalObserveFrames,
		m_iControllerProbeWaypointSubmittedCommands,
		m_iControllerProbeWaypointNoProgressFrames,
		state.lastUserCommandKnown ? state.lastUserCommandNumber : -1,
		state.lastUserCommandKnown ? state.lastUserCommandTick : -1,
		state.lastUserCommandKnown ? state.lastUserCommandButtons : 0,
		state.lastUserCommandKnown ? state.lastUserCommandForwardMove : 0.0f,
		m_vControllerProbeWaypointViewAngles.x,
		m_vControllerProbeWaypointViewAngles.y,
		m_vControllerProbeWaypointViewAngles.z);
}

void CBotEmpires::failControllerProbeWaypoint(const char* reason)
{
	if (m_bControllerProbeWaypointInputActive)
	{
		EmpiresBootstrapPlayerState state;
		const bool identityAvailable = readBootstrapPlayerState(state) &&
			controllerProbeWaypointIdentityMatches(state) &&
			g_pBotManager != nullptr &&
			g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict)) != nullptr;
		if (identityAvailable)
		{
			if (submitControllerProbeMovementUserCmd(0, 0.0f,
				"waypoint-failure-release", &m_vControllerProbeWaypointViewAngles))
			{
				m_bControllerProbeWaypointInputActive = false;
			}
			else
			{
				logger->Log(LogLevel::ERROR,
					"Empires controller probe waypoint failure release was not accepted; input state could not be verified");
			}
		}
		else
		{
			logger->Log(LogLevel::WARN,
				"Empires controller probe waypoint failure release skipped: client identity or controller unavailable");
		}
	}

	logger->Log(LogLevel::ERROR,
		"Empires controller probe waypoint result=FAIL state=%s reason=%s active_frames=%d final_observe_frames=%d submitted=%d start_known=%d target_known=%d final_known=%d",
		controllerProbeWaypointStateName(), reason != nullptr ? reason : "unknown",
		m_iControllerProbeWaypointActiveFrames,
		m_iControllerProbeWaypointFinalObserveFrames,
		m_iControllerProbeWaypointSubmittedCommands,
		m_bControllerProbeWaypointStartOriginKnown ? 1 : 0,
		m_bControllerProbeWaypointTargetKnown ? 1 : 0,
		m_bControllerProbeWaypointFinalOriginKnown ? 1 : 0);
	m_eControllerProbeWaypointState = EmpiresControllerProbeWaypointState::FAILED;
}

bool CBotEmpires::submitControllerProbeMovementUserCmd(
	const int buttons, const float forwardmove, const char* phase,
	const QAngle* viewAngles)
{
	const bool waypointActive =
		m_eControllerProbeWaypointState != EmpiresControllerProbeWaypointState::IDLE;
	const bool movementActive =
		m_eControllerProbeMovementState != EmpiresControllerProbeMovementState::IDLE;
	const bool waypointInputState =
		m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::TARGET_SELECTED ||
		m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::MOVE_ACTIVE ||
		m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING;
	const bool movementInputState =
		m_eControllerProbeMovementState == EmpiresControllerProbeMovementState::MOVE_START_PENDING ||
		m_eControllerProbeMovementState == EmpiresControllerProbeMovementState::MOVE_ACTIVE ||
		m_eControllerProbeMovementState == EmpiresControllerProbeMovementState::MOVE_STOP_PENDING;
	const bool isForward = buttons == IN_FORWARD &&
		std::fabs(forwardmove - kControllerProbeMovementForwardSpeed) <= 0.01f;
	const bool isNeutral = buttons == 0 && std::fabs(forwardmove) <= 0.01f;
	const bool validInputState = waypointInputState || movementInputState;
	const char* inputName = waypointActive ? "waypoint" : "movement";
	if (!m_bControllerProbeOnly || (!isForward && !isNeutral) ||
		!validInputState || (waypointActive && movementActive))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe %s phase=%s rejected: movement_state=%s waypoint_state=%s buttons=0x%x forwardmove=%.1f",
			inputName, phase != nullptr ? phase : "unknown",
			controllerProbeMovementStateName(), controllerProbeWaypointStateName(),
			buttons, forwardmove);
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) ||
		!(waypointActive ? controllerProbeWaypointIdentityMatches(before) :
			controllerProbeMovementIdentityMatches(before)))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe %s phase=%s rejected: client identity unavailable or changed before RunPlayerMove",
			inputName, phase != nullptr ? phase : "unknown");
		return false;
	}

	if (playerinfomanager == nullptr || g_pBotManager == nullptr ||
		gpGlobals == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe %s phase=%s rejected: SDK interfaces unavailable before RunPlayerMove",
			inputName, phase != nullptr ? phase : "unknown");
		return false;
	}

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(
		const_cast<edict_t*>(before.edict));
	IBotController* pController = g_pBotManager->GetBotController(
		const_cast<edict_t*>(before.edict));
	if (pInfo == nullptr || pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe %s phase=%s rejected: fresh PlayerInfo=%s controller=%s",
			inputName, phase != nullptr ? phase : "unknown",
			pInfo != nullptr ? "available" : "null",
			pController != nullptr ? "available" : "null");
		return false;
	}

	const CBotCmd lastCommand = pInfo->GetLastUserCommand();
	const int nextCommandNumber = lastCommand.command_number > 0 ?
		lastCommand.command_number + 1 : 1;
	const int lastSubmittedCommand = waypointActive ?
		m_iControllerProbeWaypointLastSubmittedCommand :
		m_iControllerProbeMovementLastSubmittedCommand;
	if (lastSubmittedCommand > 0 &&
		lastCommand.command_number != lastSubmittedCommand)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe %s phase=%s rejected: last command changed unexpectedly stored=%d fresh=%d",
			inputName, phase != nullptr ? phase : "unknown",
			lastSubmittedCommand,
			lastCommand.command_number);
		return false;
	}

	CBotCmd userCommand;
	userCommand.Reset();
	userCommand.command_number = nextCommandNumber;
	userCommand.tick_count = gpGlobals->tickcount;
	// The ordinary movement proof preserves the spawn view. The waypoint proof
	// supplies a freshly computed yaw toward its one selected target.
	userCommand.viewangles = viewAngles != nullptr ? *viewAngles :
		m_vControllerProbeMovementViewAngles;
	userCommand.forwardmove = forwardmove;
	userCommand.sidemove = 0.0f;
	userCommand.upmove = 0.0f;
	userCommand.buttons = buttons;
	userCommand.impulse = 0;
	userCommand.weaponselect = 0;
	userCommand.weaponsubtype = 0;
	userCommand.random_seed = 0;
	userCommand.mousedx = 0;
	userCommand.mousedy = 0;
	userCommand.hasbeenpredicted = false;

	logger->Log(LogLevel::INFO,
		"Empires controller probe %s submit phase=%s index=%d userid=%d edict=%p command_number=%d tick_count=%d buttons=0x%x forwardmove=%.1f sidemove=0.0 upmove=0.0 viewangles=(%.1f %.1f %.1f)",
		inputName, phase != nullptr ? phase : "unknown", before.index, before.userId,
		static_cast<const void*>(before.edict), userCommand.command_number,
		userCommand.tick_count, userCommand.buttons, userCommand.forwardmove,
		userCommand.viewangles.x, userCommand.viewangles.y,
		userCommand.viewangles.z);

	// This is the same normal controller input path used by CBot::runPlayerMove;
	// the probe bypasses only the generic AI loop.
	pController->RunPlayerMove(&userCommand);

	EmpiresBootstrapPlayerState after;
	if (!readBootstrapPlayerState(after) ||
		!(waypointActive ? controllerProbeWaypointIdentityMatches(after) :
			controllerProbeMovementIdentityMatches(after)))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe %s phase=%s failed: client identity changed or state unavailable after RunPlayerMove",
			inputName, phase != nullptr ? phase : "unknown");
		return false;
	}

	const bool commandVisible = after.lastUserCommandKnown &&
		after.lastUserCommandNumber == userCommand.command_number &&
		after.lastUserCommandTick > before.lastUserCommandTick;
	const bool commandMatches = after.lastUserCommandKnown &&
		after.lastUserCommandButtons == buttons &&
		std::fabs(after.lastUserCommandForwardMove - forwardmove) <= 0.01f &&
		std::fabs(after.lastUserCommandSideMove) <= 0.01f &&
		std::fabs(after.lastUserCommandUpMove) <= 0.01f &&
		after.lastUserCommandImpulse == 0 &&
		after.lastUserCommandWeaponSelect == 0;
	if (!commandVisible || !commandMatches)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe %s phase=%s failed: command readback visible=%d matches=%d stored_number=%d stored_tick=%d buttons=0x%x forward=%.1f side=%.1f up=%.1f",
			inputName, phase != nullptr ? phase : "unknown", commandVisible ? 1 : 0,
			commandMatches ? 1 : 0, after.lastUserCommandNumber,
			after.lastUserCommandTick, after.lastUserCommandButtons,
			after.lastUserCommandForwardMove, after.lastUserCommandSideMove,
			after.lastUserCommandUpMove);
		return false;
	}

	if (waypointActive)
	{
		m_iControllerProbeWaypointLastSubmittedCommand = userCommand.command_number;
		m_iControllerProbeWaypointLastSubmittedTick = userCommand.tick_count;
		++m_iControllerProbeWaypointSubmittedCommands;
		m_bControllerProbeWaypointInputActive = isForward;
		if (m_iControllerProbeWaypointSubmittedCommands == 1 || isNeutral ||
			m_iControllerProbeWaypointSubmittedCommands % 30 == 0)
			logControllerProbeWaypointState(phase != nullptr ? phase : "usercmd-after", after);
	}
	else
	{
		m_iControllerProbeMovementLastSubmittedCommand = userCommand.command_number;
		m_iControllerProbeMovementLastSubmittedTick = userCommand.tick_count;
		++m_iControllerProbeMovementSubmittedCommands;
		logControllerProbeMovementState(phase != nullptr ? phase : "usercmd-after", after);
	}
	return true;
}

void CBotEmpires::logControllerProbeAppliedLoadoutReadinessState(
	const char* phase, const EmpiresBootstrapPlayerState& state) const
{
	IBotController* pController = nullptr;
	if (g_pBotManager != nullptr && state.edict != nullptr && !state.edict->IsFree())
		pController = g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict));

	logger->Log(LogLevel::INFO,
		"Empires controller probe appliedloadoutreadiness phase=%s state=%s curtime=%.3f tickcount=%d index=%d userid=%d edict=%p controller=%s connected=%d active=%d fake=%d observer=%d dead=%d alive=%d team=%d applied_class=%d applied_class_known=%d class_desired_flag=%d class_desired_known=%d applied_sidearm=%d applied_sidearm_known=%d sidearm_desired_flag=%d sidearm_desired_known=%d applied_primary=%d applied_primary_known=%d primary_desired_flag=%d primary_desired_known=%d applied_grenade=%d applied_grenade_known=%d grenade_desired_flag=%d grenade_desired_known=%d applied_equipment=%d applied_equipment_known=%d equipment_desired_flag=%d equipment_desired_known=%d desired_spawn=%d desired_spawn_known=%d health=%d health_known=%d life_state=%d life_state_name=%s life_state_known=%d respawn_timer_known=%d respawn_timer=%.3f lastcmd_known=%d lastcmd_number=%d lastcmd_tick=%d lastcmd_buttons=0x%x lastcmd_forward=%.1f lastcmd_side=%.1f lastcmd_up=%.1f lastcmd_impulse=%d lastcmd_weaponselect=%d submitted=%d max_commands=%d",
		phase != nullptr ? phase : "unknown",
		controllerProbeAppliedLoadoutReadinessStateName(), state.currentTime,
		gpGlobals != nullptr ? gpGlobals->tickcount : -1, state.index, state.userId,
		static_cast<const void*>(state.edict), pController != nullptr ? "available" : "null",
		state.connected ? 1 : 0, state.active ? 1 : 0, state.fakeClient ? 1 : 0,
		state.observer ? 1 : 0, state.dead ? 1 : 0, state.alive ? 1 : 0,
		state.team, state.classId, state.classKnown ? 1 : 0,
		state.classDesired ? 1 : 0, state.classDesiredKnown ? 1 : 0,
		state.appliedSidearm, state.appliedSidearmKnown ? 1 : 0,
		state.sidearmDesired ? 1 : 0, state.sidearmDesiredKnown ? 1 : 0,
		state.appliedPrimary, state.appliedPrimaryKnown ? 1 : 0,
		state.primaryDesired ? 1 : 0, state.primaryDesiredKnown ? 1 : 0,
		state.appliedGrenade, state.appliedGrenadeKnown ? 1 : 0,
		state.grenadeDesired ? 1 : 0, state.grenadeDesiredKnown ? 1 : 0,
		state.appliedEquipment, state.appliedEquipmentKnown ? 1 : 0,
		state.equipmentDesired ? 1 : 0, state.equipmentDesiredKnown ? 1 : 0,
		state.desiredSpawn, state.desiredSpawnKnown ? 1 : 0, state.health,
		state.healthKnown ? 1 : 0, state.lifeState, lifeStateName(state.lifeState),
		state.lifeStateKnown ? 1 : 0, state.respawnTimerKnown ? 1 : 0,
		state.respawnTimer, state.lastUserCommandKnown ? 1 : 0,
		state.lastUserCommandNumber, state.lastUserCommandTick,
		state.lastUserCommandButtons, state.lastUserCommandForwardMove,
		state.lastUserCommandSideMove, state.lastUserCommandUpMove,
		state.lastUserCommandImpulse, state.lastUserCommandWeaponSelect,
		m_iControllerProbeAppliedLoadoutSubmittedCommands,
		m_iControllerProbeAppliedLoadoutMaxCommands);
}

void CBotEmpires::recordControllerProbeAppliedLoadoutReadinessChanges(
	const EmpiresBootstrapPlayerState& state)
{
	if (!m_bControllerProbeAppliedLoadoutLastStateKnown)
	{
		m_controllerProbeAppliedLoadoutLastState = state;
		m_bControllerProbeAppliedLoadoutLastStateKnown = true;
		return;
	}

	const EmpiresBootstrapPlayerState& previous =
		m_controllerProbeAppliedLoadoutLastState;
	if (previous.lifeStateKnown != state.lifeStateKnown ||
		(previous.lifeStateKnown && previous.lifeState != state.lifeState))
	{
		logger->Log(LogLevel::INFO,
			"Empires controller probe appliedloadoutreadiness life-transition previous=%d previous_name=%s current=%d current_name=%s curtime=%.3f tickcount=%d",
			previous.lifeState, lifeStateName(previous.lifeState), state.lifeState,
			lifeStateName(state.lifeState), state.currentTime,
			gpGlobals != nullptr ? gpGlobals->tickcount : -1);
	}

	const bool appliedChanged = previous.classId != state.classId ||
		previous.appliedSidearm != state.appliedSidearm ||
		previous.appliedPrimary != state.appliedPrimary ||
		previous.appliedGrenade != state.appliedGrenade ||
		previous.appliedEquipment != state.appliedEquipment;
	const bool desiredFlagsChanged = previous.classDesired != state.classDesired ||
		previous.sidearmDesired != state.sidearmDesired ||
		previous.primaryDesired != state.primaryDesired ||
		previous.grenadeDesired != state.grenadeDesired ||
		previous.equipmentDesired != state.equipmentDesired;
	if (appliedChanged || desiredFlagsChanged)
	{
		logger->Log(LogLevel::INFO,
			"Empires controller probe appliedloadoutreadiness field-transition curtime=%.3f tickcount=%d applied_class=%d->%d class_desired_flag=%d->%d applied_sidearm=%d->%d sidearm_desired_flag=%d->%d applied_primary=%d->%d primary_desired_flag=%d->%d applied_grenade=%d->%d grenade_desired_flag=%d->%d applied_equipment=%d->%d equipment_desired_flag=%d->%d",
			state.currentTime, gpGlobals != nullptr ? gpGlobals->tickcount : -1,
			previous.classId, state.classId, previous.classDesired ? 1 : 0,
			state.classDesired ? 1 : 0, previous.appliedSidearm, state.appliedSidearm,
			previous.sidearmDesired ? 1 : 0, state.sidearmDesired ? 1 : 0,
			previous.appliedPrimary, state.appliedPrimary,
			previous.primaryDesired ? 1 : 0, state.primaryDesired ? 1 : 0,
			previous.appliedGrenade, state.appliedGrenade,
			previous.grenadeDesired ? 1 : 0, state.grenadeDesired ? 1 : 0,
			previous.appliedEquipment, state.appliedEquipment,
			previous.equipmentDesired ? 1 : 0, state.equipmentDesired ? 1 : 0);
	}

	m_controllerProbeAppliedLoadoutLastState = state;
}

bool CBotEmpires::controllerProbeLoadoutIdentityMatches(
	const EmpiresBootstrapPlayerState& state) const
{
	return m_bControllerProbeOnly &&
		state.index == m_iControllerProbeLoadoutExpectedIndex &&
		state.userId == m_iControllerProbeLoadoutExpectedUserId &&
		state.edict == m_pControllerProbeLoadoutExpectedEdict &&
		state.edict != nullptr && !state.edict->IsFree();
}

void CBotEmpires::logControllerProbeLoadoutReadinessState(const char* phase,
	const EmpiresBootstrapPlayerState& state) const
{
	IBotController* pController = nullptr;
	if (g_pBotManager != nullptr && state.edict != nullptr && !state.edict->IsFree())
		pController = g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict));

	logger->Log(LogLevel::INFO,
		"Empires controller probe loadoutreadiness phase=%s state=%s curtime=%.3f tickcount=%d index=%d userid=%d edict=%p controller=%s connected=%d active=%d fake=%d observer=%d dead=%d alive=%d team=%d class=%d class_known=%d class_desired_flag=%d class_desired_known=%d applied_sidearm=%d applied_sidearm_known=%d sidearm_desired_flag=%d sidearm_desired_known=%d applied_primary=%d applied_primary_known=%d primary_desired_flag=%d primary_desired_known=%d applied_grenade=%d applied_grenade_known=%d grenade_desired_flag=%d grenade_desired_known=%d applied_equipment=%d applied_equipment_known=%d equipment_desired_flag=%d equipment_desired_known=%d desired_spawn=%d desired_spawn_known=%d health=%d health_known=%d life_state=%d life_state_name=%s life_state_known=%d respawn_timer_known=%d respawn_timer=%.3f lastcmd_known=%d lastcmd_number=%d lastcmd_tick=%d lastcmd_buttons=0x%x lastcmd_forward=%.1f lastcmd_side=%.1f lastcmd_up=%.1f lastcmd_impulse=%d lastcmd_weaponselect=%d submitted=%d max_commands=%d",
		phase != nullptr ? phase : "unknown", controllerProbeLoadoutReadinessStateName(),
		state.currentTime, gpGlobals != nullptr ? gpGlobals->tickcount : -1,
		state.index, state.userId, static_cast<const void*>(state.edict),
		pController != nullptr ? "available" : "null", state.connected ? 1 : 0,
		state.active ? 1 : 0, state.fakeClient ? 1 : 0, state.observer ? 1 : 0,
		state.dead ? 1 : 0, state.alive ? 1 : 0, state.team, state.classId,
		state.classKnown ? 1 : 0, state.classDesired ? 1 : 0,
		state.classDesiredKnown ? 1 : 0, state.appliedSidearm,
		state.appliedSidearmKnown ? 1 : 0, state.sidearmDesired ? 1 : 0,
		state.sidearmDesiredKnown ? 1 : 0, state.appliedPrimary,
		state.appliedPrimaryKnown ? 1 : 0, state.primaryDesired ? 1 : 0,
		state.primaryDesiredKnown ? 1 : 0, state.appliedGrenade,
		state.appliedGrenadeKnown ? 1 : 0, state.grenadeDesired ? 1 : 0,
		state.grenadeDesiredKnown ? 1 : 0, state.appliedEquipment,
		state.appliedEquipmentKnown ? 1 : 0, state.equipmentDesired ? 1 : 0,
		state.equipmentDesiredKnown ? 1 : 0, state.desiredSpawn,
		state.desiredSpawnKnown ? 1 : 0, state.health, state.healthKnown ? 1 : 0,
		state.lifeState, lifeStateName(state.lifeState), state.lifeStateKnown ? 1 : 0,
		state.respawnTimerKnown ? 1 : 0, state.respawnTimer,
		state.lastUserCommandKnown ? 1 : 0, state.lastUserCommandNumber,
		state.lastUserCommandTick, state.lastUserCommandButtons,
		state.lastUserCommandForwardMove, state.lastUserCommandSideMove,
		state.lastUserCommandUpMove, state.lastUserCommandImpulse,
		state.lastUserCommandWeaponSelect, m_iControllerProbeLoadoutSubmittedCommands,
		m_iControllerProbeLoadoutMaxCommands);
}

void CBotEmpires::recordControllerProbeLoadoutReadinessChanges(
	const EmpiresBootstrapPlayerState& state)
{
	if (!m_bControllerProbeOnly || logger == nullptr)
		return;

	if (state.lifeStateKnown)
	{
		if (!m_bControllerProbeLoadoutLastLifeStateKnown)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe loadoutreadiness life-transition previous=UNKNOWN current=%d current_name=%s curtime=%.3f tickcount=%d",
				state.lifeState, lifeStateName(state.lifeState), state.currentTime,
				gpGlobals != nullptr ? gpGlobals->tickcount : -1);
		}
		else if (m_iControllerProbeLoadoutLastLifeState != state.lifeState)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe loadoutreadiness life-transition previous=%d previous_name=%s current=%d current_name=%s curtime=%.3f tickcount=%d",
				m_iControllerProbeLoadoutLastLifeState,
				lifeStateName(m_iControllerProbeLoadoutLastLifeState), state.lifeState,
				lifeStateName(state.lifeState), state.currentTime,
				gpGlobals != nullptr ? gpGlobals->tickcount : -1);
		}
		m_bControllerProbeLoadoutLastLifeStateKnown = true;
		m_iControllerProbeLoadoutLastLifeState = state.lifeState;
	}

	if (state.respawnTimerKnown && std::isfinite(state.respawnTimer))
	{
		if (!m_bControllerProbeLoadoutLastTimerKnown)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe loadoutreadiness timer-transition previous=UNKNOWN current=%.3f curtime=%.3f",
				state.respawnTimer, state.currentTime);
		}
		else if (std::fabs(m_flControllerProbeLoadoutLastTimer - state.respawnTimer) > 0.0005f)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe loadoutreadiness timer-transition previous=%.3f current=%.3f curtime=%.3f",
				m_flControllerProbeLoadoutLastTimer, state.respawnTimer,
				state.currentTime);
		}
		m_bControllerProbeLoadoutLastTimerKnown = true;
		m_flControllerProbeLoadoutLastTimer = state.respawnTimer;
		if (!m_bControllerProbeLoadoutLatestDeadlineKnown ||
			std::fabs(m_flControllerProbeLoadoutLatestDeadline - state.respawnTimer) > 0.0005f)
		{
			m_bControllerProbeLoadoutLatestDeadlineKnown = true;
			m_flControllerProbeLoadoutLatestDeadline = state.respawnTimer;
		}
	}
}

void CBotEmpires::failControllerProbeLoadoutReadiness(const char* reason)
{
	logger->Log(LogLevel::ERROR,
		"Empires controller probe loadoutreadiness result=FAILED state=%s reason=%s submitted=%d max_commands=%d stage_frames=%d expected_index=%d expected_userid=%d",
		controllerProbeLoadoutReadinessStateName(), reason != nullptr ? reason : "unknown",
		m_iControllerProbeLoadoutSubmittedCommands,
		m_iControllerProbeLoadoutMaxCommands, m_iControllerProbeLoadoutStageFrames,
		m_iControllerProbeLoadoutExpectedIndex,
		m_iControllerProbeLoadoutExpectedUserId);
	m_eControllerProbeLoadoutReadinessState =
		EmpiresControllerProbeLoadoutReadinessState::FAILED;
}

bool CBotEmpires::issueControllerProbeLoadoutClientCommand(const char* command,
	const char* phase)
{
	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) ||
		!controllerProbeLoadoutIdentityMatches(before))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness command=%s rejected: fresh client identity unavailable or changed",
			command != nullptr ? command : "unknown");
		return false;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(before.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness command=%s rejected: fresh controller unavailable before dispatch",
			command != nullptr ? command : "unknown");
		return false;
	}

	if (helpers == nullptr || command == nullptr || *command == '\0')
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness command rejected: helpers or command unavailable command=%s",
			command != nullptr ? command : "null");
		return false;
	}

	logControllerProbeLoadoutReadinessState(phase != nullptr ? phase : "command-before", before);
	// Public Empires client command only. acceptLoadout is a client/UI command
	// and is intentionally not invoked by this server-side probe.
	helpers->ClientCommand(const_cast<edict_t*>(before.edict), command);

	EmpiresBootstrapPlayerState after;
	if (!readBootstrapPlayerState(after) ||
		!controllerProbeLoadoutIdentityMatches(after))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness command=%s failed: client identity changed or state unavailable after dispatch",
			command);
		return false;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(after.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness command=%s failed: fresh controller unavailable after dispatch",
			command != nullptr ? command : "unknown");
		return false;
	}

	logControllerProbeLoadoutReadinessState(phase != nullptr ? phase : "command-after", after);
	return true;
}

void CBotEmpires::failControllerProbeAppliedLoadoutReadiness(const char* reason)
{
	logger->Log(LogLevel::ERROR,
		"Empires controller probe appliedloadoutreadiness result=FAILED state=%s reason=%s submitted=%d max_commands=%d stage_frames=%d expected_index=%d expected_userid=%d",
		controllerProbeAppliedLoadoutReadinessStateName(),
		reason != nullptr ? reason : "unknown",
		m_iControllerProbeAppliedLoadoutSubmittedCommands,
		m_iControllerProbeAppliedLoadoutMaxCommands,
		m_iControllerProbeAppliedLoadoutStageFrames,
		m_iControllerProbeAppliedLoadoutExpectedIndex,
		m_iControllerProbeAppliedLoadoutExpectedUserId);
	m_eControllerProbeAppliedLoadoutReadinessState =
		EmpiresControllerProbeAppliedLoadoutReadinessState::FAILED;
}

bool CBotEmpires::issueControllerProbeAppliedLoadoutClientCommand(
	const char* command, const char* phase)
{
	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) ||
		!controllerProbeAppliedLoadoutIdentityMatches(before))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness command=%s rejected: fresh client identity unavailable or changed",
			command != nullptr ? command : "unknown");
		return false;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(before.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness command=%s rejected: fresh controller unavailable before dispatch",
			command != nullptr ? command : "unknown");
		return false;
	}

	if (helpers == nullptr || command == nullptr || *command == '\0')
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness command rejected: helpers or command unavailable command=%s",
			command != nullptr ? command : "null");
		return false;
	}

	logControllerProbeAppliedLoadoutReadinessState(
		phase != nullptr ? phase : "command-before", before);
	helpers->ClientCommand(const_cast<edict_t*>(before.edict), command);

	EmpiresBootstrapPlayerState after;
	if (!readBootstrapPlayerState(after) ||
		!controllerProbeAppliedLoadoutIdentityMatches(after))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness command=%s failed: client identity changed or state unavailable after dispatch",
			command);
		return false;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(after.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness command=%s failed: fresh controller unavailable after dispatch",
			command != nullptr ? command : "unknown");
		return false;
	}

	logControllerProbeAppliedLoadoutReadinessState(
		phase != nullptr ? phase : "command-after", after);
	return true;
}

bool CBotEmpires::requestControllerProbeLoadoutReadiness()
{
	return requestControllerProbeLoadoutReadinessInternal(false);
}

bool CBotEmpires::requestControllerProbeLoadoutReadinessClassFirst()
{
	return requestControllerProbeLoadoutReadinessInternal(true);
}

bool CBotEmpires::requestControllerProbeAppliedLoadoutReadiness()
{
	const bool noPendingInput =
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE &&
		m_eControllerProbeSpawnHoldState == EmpiresControllerProbeSpawnHoldState::IDLE &&
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::IDLE &&
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::IDLE &&
		m_eControllerProbeAppliedLoadoutReadinessState ==
			EmpiresControllerProbeAppliedLoadoutReadinessState::IDLE &&
		!m_bTeamCommandPending && !m_bClassCommandPending &&
		!m_bSpawnCommandPending &&
		m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::IDLE;

	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle != EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr || !noPendingInput)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe appliedloadoutreadiness rejected: probe unavailable or another input operation is pending state=%s lifecycle=%s",
			controllerProbeAppliedLoadoutReadinessStateName(),
			controllerProbeLifecycleName());
		return false;
	}

	if (gpGlobals == nullptr || !std::isfinite(gpGlobals->interval_per_tick) ||
		gpGlobals->interval_per_tick <= 0.0f)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness rejected: invalid server interval_per_tick");
		return false;
	}

	EmpiresBootstrapPlayerState initial;
	if (!readBootstrapPlayerState(initial) || !initial.connected ||
		!initial.fakeClient || !initial.active || initial.team != 0 ||
		initial.index <= 0 || initial.userId <= 0 || initial.edict == nullptr ||
		!initial.lifeStateKnown || !initial.healthKnown || !initial.currentTimeKnown)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness rejected: expected fresh unassigned fake client team=0 index=%d userid=%d connected=%d fake=%d active=%d life_state=%d health=%d",
			initial.index, initial.userId, initial.connected ? 1 : 0,
			initial.fakeClient ? 1 : 0, initial.active ? 1 : 0,
			initial.lifeState, initial.health);
		return false;
	}

	IBotController* pController = g_pBotManager != nullptr ?
		g_pBotManager->GetBotController(const_cast<edict_t*>(initial.edict)) : nullptr;
	if (pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness rejected: fresh controller unavailable index=%d userid=%d edict=%p",
			initial.index, initial.userId, static_cast<const void*>(initial.edict));
		return false;
	}

	m_iControllerProbeAppliedLoadoutExpectedIndex = initial.index;
	m_iControllerProbeAppliedLoadoutExpectedUserId = initial.userId;
	m_pControllerProbeAppliedLoadoutExpectedEdict = initial.edict;
	m_iControllerProbeAppliedLoadoutStageFrames = 0;
	m_iControllerProbeAppliedLoadoutSubmittedCommands = 0;
	m_iControllerProbeAppliedLoadoutMaxCommands = static_cast<int>(std::ceil(
		kControllerProbeLoadoutNeutralSafetySeconds / gpGlobals->interval_per_tick)) + 2;
	if (m_iControllerProbeAppliedLoadoutMaxCommands < 1)
		m_iControllerProbeAppliedLoadoutMaxCommands = 1;
	if (m_iControllerProbeAppliedLoadoutMaxCommands > 2048)
		m_iControllerProbeAppliedLoadoutMaxCommands = 2048;
	m_iControllerProbeAppliedLoadoutLastSubmittedTick = -1;
	m_iControllerProbeAppliedLoadoutLastSubmittedCommand = 0;
	m_iControllerProbeAppliedLoadoutLastProgressLogCommand = 0;
	m_bControllerProbeAppliedLoadoutStartTimeKnown = true;
	m_flControllerProbeAppliedLoadoutStartTime = initial.currentTime;
	m_bControllerProbeAppliedLoadoutNeutralStartTimeKnown = false;
	m_flControllerProbeAppliedLoadoutNeutralStartTime = 0.0f;
	m_bControllerProbeAppliedLoadoutLastStateKnown = false;
	m_bControllerProbeAppliedLoadoutBeforeEquipmentKnown = false;
	m_controllerProbeAppliedLoadoutBeforeEquipment = {};
	recordControllerProbeAppliedLoadoutReadinessChanges(initial);

	m_eControllerProbeAppliedLoadoutReadinessState =
		EmpiresControllerProbeAppliedLoadoutReadinessState::JOIN_NF;
	logControllerProbeAppliedLoadoutReadinessState("initial", initial);
	logger->Log(LogLevel::INFO,
		"Empires controller probe appliedloadoutreadiness armed predicate=applied_sendprops_exact_tuple_and_all_desired_flags_clear command_order=jointeam 2, changeclass 1, changesidearm 1, changeprimary 6, changegrenade 12, changeequipment 21, emp_spawn_select 32 total_timeout=30.0 neutral_timeout=20.0 post_equipment_frames=%d",
		kControllerProbeAppliedLoadoutObservationFrames);

	if (!issueControllerProbeAppliedLoadoutClientCommand("jointeam 2\n",
		"jointeam-before-after"))
	{
		failControllerProbeAppliedLoadoutReadiness(
			"jointeam 2 could not be dispatched with stable client identity");
		return false;
	}

	EmpiresBootstrapPlayerState afterJoin;
	if (!readBootstrapPlayerState(afterJoin) ||
		!controllerProbeAppliedLoadoutIdentityMatches(afterJoin))
	{
		failControllerProbeAppliedLoadoutReadiness(
			"fresh state unavailable after jointeam 2");
		return false;
	}
	recordControllerProbeAppliedLoadoutReadinessChanges(afterJoin);
	m_eControllerProbeAppliedLoadoutReadinessState =
		EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_TEAM;
	m_iControllerProbeAppliedLoadoutStageFrames = 0;
	logControllerProbeAppliedLoadoutReadinessState(
		"after-jointeam-dispatch", afterJoin);
	return true;
}

bool CBotEmpires::requestControllerProbeLoadoutReadinessInternal(const bool classFirst)
{
	m_bControllerProbeLoadoutClassFirst = classFirst;
	const bool noPendingInput =
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE &&
		m_eControllerProbeSpawnHoldState ==
			EmpiresControllerProbeSpawnHoldState::IDLE &&
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::IDLE &&
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::IDLE &&
		!m_bTeamCommandPending && !m_bClassCommandPending &&
		!m_bSpawnCommandPending &&
		m_eSpawnConfirmationState ==
			EmpiresBootstrapSpawnConfirmationState::IDLE;

	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle != EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr || !noPendingInput)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe loadoutreadiness rejected: probe unavailable or another input operation is pending sequence=%s hold=%s neutral=%s loadout=%s lifecycle=%s",
			controllerProbeSpawnSequenceStateName(), controllerProbeSpawnHoldStateName(),
			controllerProbeNeutralReadinessStateName(),
			controllerProbeLoadoutReadinessStateName(), controllerProbeLifecycleName());
		return false;
	}

	if (gpGlobals == nullptr || !std::isfinite(gpGlobals->interval_per_tick) ||
		gpGlobals->interval_per_tick <= 0.0f)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness rejected: invalid server interval_per_tick");
		return false;
	}

	EmpiresBootstrapPlayerState initial;
	if (!readBootstrapPlayerState(initial) || !initial.connected ||
		!initial.fakeClient || !initial.active || initial.team != 0 ||
		initial.index <= 0 || initial.userId <= 0 || initial.edict == nullptr ||
		!initial.lifeStateKnown || !initial.healthKnown || !initial.currentTimeKnown)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness rejected: expected fresh unassigned non-alive fake client team=0 index=%d userid=%d connected=%d fake=%d active=%d life_state=%d health=%d",
			initial.index, initial.userId, initial.connected ? 1 : 0,
			initial.fakeClient ? 1 : 0, initial.active ? 1 : 0,
			initial.lifeState, initial.health);
		return false;
	}

	IBotController* pController = g_pBotManager != nullptr ?
		g_pBotManager->GetBotController(const_cast<edict_t*>(initial.edict)) : nullptr;
	if (pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness rejected: fresh controller unavailable index=%d userid=%d edict=%p",
			initial.index, initial.userId, static_cast<const void*>(initial.edict));
		return false;
	}

	m_iControllerProbeLoadoutExpectedIndex = initial.index;
	m_iControllerProbeLoadoutExpectedUserId = initial.userId;
	m_pControllerProbeLoadoutExpectedEdict = initial.edict;
	m_iControllerProbeLoadoutStageFrames = 0;
	m_iControllerProbeLoadoutSubmittedCommands = 0;
	m_iControllerProbeLoadoutMaxCommands = static_cast<int>(std::ceil(
		kControllerProbeLoadoutNeutralSafetySeconds / gpGlobals->interval_per_tick)) + 2;
	if (m_iControllerProbeLoadoutMaxCommands < 1)
		m_iControllerProbeLoadoutMaxCommands = 1;
	if (m_iControllerProbeLoadoutMaxCommands > 2048)
		m_iControllerProbeLoadoutMaxCommands = 2048;
	m_iControllerProbeLoadoutLastSubmittedTick = -1;
	m_iControllerProbeLoadoutLastSubmittedCommand = 0;
	m_iControllerProbeLoadoutLastProgressLogCommand = 0;
	m_bControllerProbeLoadoutStartTimeKnown = true;
	m_flControllerProbeLoadoutStartTime = initial.currentTime;
	m_bControllerProbeLoadoutNeutralStartTimeKnown = false;
	m_flControllerProbeLoadoutNeutralStartTime = 0.0f;
	m_bControllerProbeLoadoutLastLifeStateKnown = false;
	m_iControllerProbeLoadoutLastLifeState = -1;
	m_bControllerProbeLoadoutLastTimerKnown = false;
	m_flControllerProbeLoadoutLastTimer = 0.0f;
	m_bControllerProbeLoadoutLatestDeadlineKnown = false;
	m_flControllerProbeLoadoutLatestDeadline = 0.0f;
	recordControllerProbeLoadoutReadinessChanges(initial);

	m_eControllerProbeLoadoutReadinessState =
		EmpiresControllerProbeLoadoutReadinessState::JOIN_NF_REQUESTED;
	logControllerProbeLoadoutReadinessState("initial", initial);
	logger->Log(LogLevel::INFO,
		"Empires controller probe loadoutreadiness armed variant=%s command_order=%s total_timeout=%.1f neutral_timeout=%.1f",
		m_bControllerProbeLoadoutClassFirst ? "class-first" : "legacy-equipment-first",
		m_bControllerProbeLoadoutClassFirst ?
			"jointeam 2, changeclass 1, changesidearm 1, changeprimary 6, changegrenade 12, changeequipment 21, emp_spawn_select 32" :
			"jointeam 2, changesidearm 1, changeprimary 6, changegrenade 12, changeequipment 21, changeclass 1, emp_spawn_select 32",
		kControllerProbeLoadoutReadinessSafetySeconds,
		kControllerProbeLoadoutNeutralSafetySeconds);

	if (!issueControllerProbeLoadoutClientCommand("jointeam 2\n",
		"jointeam-before-after"))
	{
		failControllerProbeLoadoutReadiness(
			"jointeam 2 could not be dispatched with stable client identity");
		return false;
	}

	EmpiresBootstrapPlayerState afterJoin;
	if (!readBootstrapPlayerState(afterJoin) ||
		!controllerProbeLoadoutIdentityMatches(afterJoin))
	{
		failControllerProbeLoadoutReadiness("fresh state unavailable after jointeam 2");
		return false;
	}
	recordControllerProbeLoadoutReadinessChanges(afterJoin);
	m_eControllerProbeLoadoutReadinessState =
		EmpiresControllerProbeLoadoutReadinessState::OBSERVE_TEAM;
	m_iControllerProbeLoadoutStageFrames = 0;
	logControllerProbeLoadoutReadinessState("after-jointeam-dispatch", afterJoin);
	return true;
}

void CBotEmpires::observeControllerProbeLoadoutReadiness()
{
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::IDLE ||
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::COMPLETE ||
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::FAILED)
		return;

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state) ||
		!controllerProbeLoadoutIdentityMatches(state))
	{
		failControllerProbeLoadoutReadiness(
			"fresh client identity unavailable or changed during loadout-readiness test");
		return;
	}
	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict)) == nullptr)
	{
		failControllerProbeLoadoutReadiness(
			"fresh IBotController unavailable during loadout-readiness test");
		return;
	}

	recordControllerProbeLoadoutReadinessChanges(state);
	++m_iControllerProbeLoadoutStageFrames;
	if (!state.connected || !state.active || state.observer || !state.fakeClient ||
		!state.lifeStateKnown || !state.healthKnown || !state.currentTimeKnown)
	{
		failControllerProbeLoadoutReadiness(
			"client state was not fully available before the next test step");
		return;
	}

	if (m_bControllerProbeLoadoutStartTimeKnown &&
		state.currentTime >= m_flControllerProbeLoadoutStartTime +
			kControllerProbeLoadoutReadinessSafetySeconds)
	{
		logControllerProbeLoadoutReadinessState("total-safety-timeout", state);
		failControllerProbeLoadoutReadiness("30-second total safety timeout reached");
		return;
	}

	if (state.lifeState == LIFE_RESPAWNABLE)
	{
		failControllerProbeLoadoutReadiness(
			"LIFE_RESPAWNABLE was reached before the verified neutral simulation stage");
		return;
	}
	const bool teamReady = state.team == EMP_TEAM_NF;
	// The four m_i*Weapon/m_iGrenade/m_iEquipment props are the currently
	// applied loadout. Empires keeps the client-requested value private until
	// ValidateLoadout() applies it, while the public m_b*Desired flags are the
	// available acknowledgement for these pre-spawn commands.
	const bool sidearmReady = state.sidearmDesiredKnown && state.sidearmDesired;
	const bool primaryReady = state.primaryDesiredKnown && state.primaryDesired;
	const bool grenadeReady = state.grenadeDesiredKnown && state.grenadeDesired;
	const bool equipmentReady = state.equipmentDesiredKnown && state.equipmentDesired;
	const bool classReady = controllerProbeRiflemanAcknowledged(state);
	const bool spawnReady = state.desiredSpawnKnown && state.desiredSpawn == 32;
	const bool setupNonAlive = state.lifeStateKnown &&
		(state.lifeState == LIFE_DYING || state.lifeState == LIFE_DEAD) &&
		state.health <= 0;
	if (!setupNonAlive)
	{
		// Empires performs the team change by committing the fresh client to
		// suicide. During LIFE_DYING, IPlayerInfo::IsDead() can still report
		// alive; the dynamic Empires life-state plus health is authoritative for
		// this setup gate. Wait only in OBSERVE_TEAM for the public transition.
		if (m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::OBSERVE_TEAM && teamReady)
		{
			if (m_iControllerProbeLoadoutStageFrames == 1 ||
				(m_iControllerProbeLoadoutStageFrames % 64) == 0)
			{
				logger->Log(LogLevel::INFO,
					"Empires controller probe loadoutreadiness waiting for jointeam suicide transition frame=%d life_state=%d health=%d",
					m_iControllerProbeLoadoutStageFrames, state.lifeState,
					state.health);
			}
			return;
		}
		failControllerProbeLoadoutReadiness(
			"client was not in the verified non-alive LIFE_DYING/LIFE_DEAD setup state");
		return;
	}

	switch (m_eControllerProbeLoadoutReadinessState)
	{
	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_TEAM:
		if (!teamReady || !setupNonAlive)
		{
			failControllerProbeLoadoutReadiness(
				"jointeam 2 did not produce a stable non-alive NF client");
			return;
		}
		if (m_iControllerProbeLoadoutStageFrames >=
			kControllerProbeLoadoutReadinessSetupSamples)
		{
			m_eControllerProbeLoadoutReadinessState = m_bControllerProbeLoadoutClassFirst ?
				EmpiresControllerProbeLoadoutReadinessState::REQUEST_RIFLEMAN :
				EmpiresControllerProbeLoadoutReadinessState::SET_SIDEARM;
			m_iControllerProbeLoadoutStageFrames = 0;
			const char* command = m_bControllerProbeLoadoutClassFirst ?
				"changeclass 1\n" : "changesidearm 1\n";
			const char* phase = m_bControllerProbeLoadoutClassFirst ?
				"changeclass-before-equipment" : "changesidearm-before-after";
			if (!issueControllerProbeLoadoutClientCommand(command, phase))
			{
				failControllerProbeLoadoutReadiness(
					m_bControllerProbeLoadoutClassFirst ?
						"changeclass 1 could not be dispatched with stable client identity" :
						"changesidearm 1 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeLoadoutReadinessState =
				m_bControllerProbeLoadoutClassFirst ?
				EmpiresControllerProbeLoadoutReadinessState::OBSERVE_CLASS :
				EmpiresControllerProbeLoadoutReadinessState::OBSERVE_SIDEARM;
		}
		return;

	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_SIDEARM:
		if (!teamReady || !setupNonAlive || !sidearmReady)
		{
			failControllerProbeLoadoutReadiness(
				"changesidearm 1 was not reflected in the current desired loadout fields");
			return;
		}
		if (m_iControllerProbeLoadoutStageFrames >= kControllerProbeLoadoutReadinessSetupSamples)
		{
			m_eControllerProbeLoadoutReadinessState =
				EmpiresControllerProbeLoadoutReadinessState::SET_PRIMARY;
			m_iControllerProbeLoadoutStageFrames = 0;
			if (!issueControllerProbeLoadoutClientCommand("changeprimary 6\n",
				"changeprimary-before-after"))
			{
				failControllerProbeLoadoutReadiness(
					"changeprimary 6 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeLoadoutReadinessState =
				EmpiresControllerProbeLoadoutReadinessState::OBSERVE_PRIMARY;
		}
		return;

	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_PRIMARY:
		if (!teamReady || !setupNonAlive || !sidearmReady || !primaryReady)
		{
			failControllerProbeLoadoutReadiness(
				"changeprimary 6 was not reflected in the current desired loadout fields");
			return;
		}
		if (m_iControllerProbeLoadoutStageFrames >= kControllerProbeLoadoutReadinessSetupSamples)
		{
			m_eControllerProbeLoadoutReadinessState =
				EmpiresControllerProbeLoadoutReadinessState::SET_GRENADE;
			m_iControllerProbeLoadoutStageFrames = 0;
			if (!issueControllerProbeLoadoutClientCommand("changegrenade 12\n",
				"changegrenade-before-after"))
			{
				failControllerProbeLoadoutReadiness(
					"changegrenade 12 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeLoadoutReadinessState =
				EmpiresControllerProbeLoadoutReadinessState::OBSERVE_GRENADE;
		}
		return;

	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_GRENADE:
		if (!teamReady || !setupNonAlive || !sidearmReady || !primaryReady || !grenadeReady)
		{
			failControllerProbeLoadoutReadiness(
				"changegrenade 12 was not reflected in the current desired loadout fields");
			return;
		}
		if (m_iControllerProbeLoadoutStageFrames >= kControllerProbeLoadoutReadinessSetupSamples)
		{
			m_eControllerProbeLoadoutReadinessState =
				EmpiresControllerProbeLoadoutReadinessState::SET_EQUIPMENT;
			m_iControllerProbeLoadoutStageFrames = 0;
			if (!issueControllerProbeLoadoutClientCommand("changeequipment 21\n",
				"changeequipment-before-after"))
			{
				failControllerProbeLoadoutReadiness(
					"changeequipment 21 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeLoadoutReadinessState =
				EmpiresControllerProbeLoadoutReadinessState::OBSERVE_EQUIPMENT;
		}
		return;

	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_EQUIPMENT:
		if (!teamReady || !setupNonAlive || !sidearmReady || !primaryReady ||
			!grenadeReady || !equipmentReady)
		{
			failControllerProbeLoadoutReadiness(
				"changeequipment 21 was not reflected in the current desired loadout fields");
			return;
		}
		if (m_iControllerProbeLoadoutStageFrames >= kControllerProbeLoadoutReadinessSetupSamples)
		{
			m_eControllerProbeLoadoutReadinessState = m_bControllerProbeLoadoutClassFirst ?
				EmpiresControllerProbeLoadoutReadinessState::SELECT_SPAWNPOINT :
				EmpiresControllerProbeLoadoutReadinessState::REQUEST_RIFLEMAN;
			m_iControllerProbeLoadoutStageFrames = 0;
			if (m_bControllerProbeLoadoutClassFirst)
			{
				if (!issueControllerProbeLoadoutClientCommand("emp_spawn_select 32\n",
					"spawnpoint-after-validated-loadout"))
				{
					failControllerProbeLoadoutReadiness(
						"emp_spawn_select 32 could not be dispatched with stable client identity");
					return;
				}
			}
			else if (!issueControllerProbeLoadoutClientCommand("changeclass 1\n",
				"changeclass-before-after"))
			{
				failControllerProbeLoadoutReadiness(
					"changeclass 1 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeLoadoutReadinessState =
				m_bControllerProbeLoadoutClassFirst ?
				EmpiresControllerProbeLoadoutReadinessState::OBSERVE_SPAWNPOINT :
				EmpiresControllerProbeLoadoutReadinessState::OBSERVE_CLASS;
		}
		return;

	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_CLASS:
		if (!teamReady || !setupNonAlive || !classReady ||
			(!m_bControllerProbeLoadoutClassFirst &&
				(!sidearmReady || !primaryReady || !grenadeReady || !equipmentReady)))
		{
			failControllerProbeLoadoutReadiness(
				m_bControllerProbeLoadoutClassFirst ?
					"changeclass 1 was not reflected before loadout setup" :
					"changeclass 1 was not reflected while the complete NF loadout remained valid");
			return;
		}
		if (m_iControllerProbeLoadoutStageFrames >= kControllerProbeLoadoutReadinessSetupSamples)
		{
			m_eControllerProbeLoadoutReadinessState = m_bControllerProbeLoadoutClassFirst ?
				EmpiresControllerProbeLoadoutReadinessState::SET_SIDEARM :
				EmpiresControllerProbeLoadoutReadinessState::SELECT_SPAWNPOINT;
			m_iControllerProbeLoadoutStageFrames = 0;
			const char* command = m_bControllerProbeLoadoutClassFirst ?
				"changesidearm 1\n" : "emp_spawn_select 32\n";
			const char* phase = m_bControllerProbeLoadoutClassFirst ?
				"changesidearm-after-class" : "spawnpoint-before-after";
			if (!issueControllerProbeLoadoutClientCommand(command, phase))
			{
				failControllerProbeLoadoutReadiness(
					m_bControllerProbeLoadoutClassFirst ?
						"changesidearm 1 could not be dispatched with stable client identity" :
						"emp_spawn_select 32 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeLoadoutReadinessState =
				m_bControllerProbeLoadoutClassFirst ?
				EmpiresControllerProbeLoadoutReadinessState::OBSERVE_SIDEARM :
				EmpiresControllerProbeLoadoutReadinessState::OBSERVE_SPAWNPOINT;
		}
		return;

	case EmpiresControllerProbeLoadoutReadinessState::OBSERVE_SPAWNPOINT:
		if (!teamReady || !setupNonAlive || !sidearmReady || !primaryReady ||
			!grenadeReady || !equipmentReady || !classReady || !spawnReady)
		{
			failControllerProbeLoadoutReadiness(
				"spawn point 32 was not reflected with complete valid NF Rifleman loadout");
			return;
		}
		if (m_iControllerProbeLoadoutStageFrames >= kControllerProbeLoadoutReadinessSetupSamples)
		{
			m_eControllerProbeLoadoutReadinessState =
				EmpiresControllerProbeLoadoutReadinessState::NEUTRAL_SIMULATION;
			m_iControllerProbeLoadoutStageFrames = 0;
			m_iControllerProbeLoadoutSubmittedCommands = 0;
			m_iControllerProbeLoadoutLastSubmittedTick = -1;
			m_iControllerProbeLoadoutLastSubmittedCommand = 0;
			m_iControllerProbeLoadoutLastProgressLogCommand = 0;
			m_bControllerProbeLoadoutNeutralStartTimeKnown = true;
			m_flControllerProbeLoadoutNeutralStartTime = state.currentTime;
			logger->Log(LogLevel::INFO,
				"Empires controller probe loadoutreadiness transition state=%s neutral_input=enabled_only_after_setup start=%.3f interval_per_tick=%.6f max_commands=%d",
				controllerProbeLoadoutReadinessStateName(), state.currentTime,
				gpGlobals != nullptr ? gpGlobals->interval_per_tick : 0.0f,
				m_iControllerProbeLoadoutMaxCommands);
		}
		return;

	case EmpiresControllerProbeLoadoutReadinessState::NEUTRAL_SIMULATION:
		if (!teamReady || !setupNonAlive || !sidearmReady || !primaryReady ||
			!grenadeReady || !equipmentReady || !classReady || !spawnReady ||
			!state.lastUserCommandKnown || state.lastUserCommandButtons != 0 ||
			std::fabs(state.lastUserCommandForwardMove) > 0.01f ||
			std::fabs(state.lastUserCommandSideMove) > 0.01f ||
			std::fabs(state.lastUserCommandUpMove) > 0.01f ||
			state.lastUserCommandImpulse != 0 || state.lastUserCommandWeaponSelect != 0)
		{
			failControllerProbeLoadoutReadiness(
				"non-neutral input or loadout readiness state observed before next neutral command");
			return;
		}
		if (m_bControllerProbeLoadoutNeutralStartTimeKnown &&
			state.currentTime >= m_flControllerProbeLoadoutNeutralStartTime +
				kControllerProbeLoadoutNeutralSafetySeconds)
		{
			logControllerProbeLoadoutReadinessState("neutral-safety-timeout", state);
			failControllerProbeLoadoutReadiness(
				"20-second neutral simulation safety timeout reached");
			return;
		}
		if (gpGlobals == nullptr || gpGlobals->tickcount ==
			m_iControllerProbeLoadoutLastSubmittedTick)
			return;
		if (!submitControllerProbeLoadoutNeutralUserCmd("neutral-frame") &&
			m_eControllerProbeLoadoutReadinessState !=
				EmpiresControllerProbeLoadoutReadinessState::FAILED)
		{
			failControllerProbeLoadoutReadiness(
				"neutral RunPlayerMove submission or readback failed");
		}
		return;

	case EmpiresControllerProbeLoadoutReadinessState::IDLE:
	case EmpiresControllerProbeLoadoutReadinessState::JOIN_NF_REQUESTED:
	case EmpiresControllerProbeLoadoutReadinessState::SET_SIDEARM:
	case EmpiresControllerProbeLoadoutReadinessState::SET_PRIMARY:
	case EmpiresControllerProbeLoadoutReadinessState::SET_GRENADE:
	case EmpiresControllerProbeLoadoutReadinessState::SET_EQUIPMENT:
	case EmpiresControllerProbeLoadoutReadinessState::REQUEST_RIFLEMAN:
	case EmpiresControllerProbeLoadoutReadinessState::SELECT_SPAWNPOINT:
	case EmpiresControllerProbeLoadoutReadinessState::LIFE_RESPAWNABLE_REACHED:
	case EmpiresControllerProbeLoadoutReadinessState::COMPLETE:
	case EmpiresControllerProbeLoadoutReadinessState::FAILED:
		return;
	}
}

void CBotEmpires::observeControllerProbeAppliedLoadoutReadiness()
{
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeAppliedLoadoutReadinessState ==
			EmpiresControllerProbeAppliedLoadoutReadinessState::IDLE ||
		m_eControllerProbeAppliedLoadoutReadinessState ==
			EmpiresControllerProbeAppliedLoadoutReadinessState::COMPLETE ||
		m_eControllerProbeAppliedLoadoutReadinessState ==
			EmpiresControllerProbeAppliedLoadoutReadinessState::FAILED)
		return;

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state) ||
		!controllerProbeAppliedLoadoutIdentityMatches(state))
	{
		failControllerProbeAppliedLoadoutReadiness(
			"fresh client identity unavailable or changed during applied-loadout test");
		return;
	}
	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict)) == nullptr)
	{
		failControllerProbeAppliedLoadoutReadiness(
			"fresh IBotController unavailable during applied-loadout test");
		return;
	}

	recordControllerProbeAppliedLoadoutReadinessChanges(state);
	++m_iControllerProbeAppliedLoadoutStageFrames;
	if (!state.connected || !state.active || state.observer || !state.fakeClient ||
		!state.lifeStateKnown || !state.healthKnown || !state.currentTimeKnown)
	{
		failControllerProbeAppliedLoadoutReadiness(
			"client state was not fully available before the next test step");
		return;
	}

	if (m_bControllerProbeAppliedLoadoutStartTimeKnown &&
		state.currentTime >= m_flControllerProbeAppliedLoadoutStartTime + 30.0f)
	{
		logControllerProbeAppliedLoadoutReadinessState("total-safety-timeout", state);
		failControllerProbeAppliedLoadoutReadiness(
			"30-second complete-sequence safety timeout reached");
		return;
	}

	if (!appliedLoadoutFieldsKnown(state))
	{
		logControllerProbeAppliedLoadoutReadinessState("unknown-applied-field", state);
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness result classification=unknown reason=required applied SendProp or Desired flag unavailable");
		failControllerProbeAppliedLoadoutReadiness(
			"required applied SendProp or Desired flag could not be resolved safely");
		return;
	}

	if (state.lifeState == LIFE_RESPAWNABLE)
	{
		if (m_eControllerProbeAppliedLoadoutReadinessState !=
			EmpiresControllerProbeAppliedLoadoutReadinessState::NEUTRAL_SIMULATION ||
			!appliedLoadoutMatches(state) || !state.desiredSpawnKnown ||
			state.desiredSpawn != 32)
		{
			failControllerProbeAppliedLoadoutReadiness(
				"LIFE_RESPAWNABLE was reached outside the proven neutral-simulation predicate");
			return;
		}

		m_eControllerProbeAppliedLoadoutReadinessState =
			EmpiresControllerProbeAppliedLoadoutReadinessState::LIFE_RESPAWNABLE_REACHED;
		logControllerProbeAppliedLoadoutReadinessState(
			"life-respawnable-reached", state);
		m_eControllerProbeAppliedLoadoutReadinessState =
			EmpiresControllerProbeAppliedLoadoutReadinessState::COMPLETE;
		logControllerProbeAppliedLoadoutReadinessState("complete", state);
		return;
	}

	if (state.lifeState == LIFE_ALIVE || state.health > 0)
	{
		failControllerProbeAppliedLoadoutReadiness(
			"client became alive before LIFE_RESPAWNABLE");
		return;
	}

	const bool setupNonAlive =
		(state.lifeState == LIFE_DYING || state.lifeState == LIFE_DEAD) &&
		state.health <= 0;
	const bool teamReady = state.team == EMP_TEAM_NF;
	const bool classRequestObserved =
		(state.classDesiredKnown && state.classDesired) ||
		(state.classKnown && state.classId == EMP_CLASS_RIFLEMAN);
	const bool sidearmRequestObserved =
		(state.sidearmDesiredKnown && state.sidearmDesired) ||
		(state.appliedSidearm == kControllerProbeLoadoutSidearm);
	const bool primaryRequestObserved =
		(state.primaryDesiredKnown && state.primaryDesired) ||
		(state.appliedPrimary == kControllerProbeLoadoutPrimary);
	const bool grenadeRequestObserved =
		(state.grenadeDesiredKnown && state.grenadeDesired) ||
		(state.appliedGrenade == kControllerProbeLoadoutGrenade);
	const bool spawnObserved = state.desiredSpawnKnown && state.desiredSpawn == 32;

	if (!setupNonAlive || !teamReady)
	{
		if (m_eControllerProbeAppliedLoadoutReadinessState ==
			EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_TEAM &&
			teamReady && !setupNonAlive)
		{
			if (m_iControllerProbeAppliedLoadoutStageFrames == 1 ||
				(m_iControllerProbeAppliedLoadoutStageFrames % 64) == 0)
			{
				logger->Log(LogLevel::INFO,
					"Empires controller probe appliedloadoutreadiness waiting for jointeam suicide transition frame=%d life_state=%d health=%d",
					m_iControllerProbeAppliedLoadoutStageFrames, state.lifeState,
					state.health);
			}
			return;
		}

		failControllerProbeAppliedLoadoutReadiness(
			"client was not in the verified non-alive NF setup state");
		return;
	}

	switch (m_eControllerProbeAppliedLoadoutReadinessState)
	{
	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_TEAM:
		if (m_iControllerProbeAppliedLoadoutStageFrames >= 2)
		{
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::REQUEST_RIFLEMAN;
			m_iControllerProbeAppliedLoadoutStageFrames = 0;
			if (!issueControllerProbeAppliedLoadoutClientCommand(
				"changeclass 1\n", "changeclass-before-after"))
			{
				failControllerProbeAppliedLoadoutReadiness(
					"changeclass 1 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_CLASS_REQUEST;
		}
		return;

	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_CLASS_REQUEST:
		if (!classRequestObserved)
		{
			failControllerProbeAppliedLoadoutReadiness(
				"changeclass 1 was not reflected in the public class state");
			return;
		}
		if (m_iControllerProbeAppliedLoadoutStageFrames >= 2)
		{
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::SET_SIDEARM;
			m_iControllerProbeAppliedLoadoutStageFrames = 0;
			if (!issueControllerProbeAppliedLoadoutClientCommand(
				"changesidearm 1\n", "changesidearm-before-after"))
			{
				failControllerProbeAppliedLoadoutReadiness(
					"changesidearm 1 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_SIDEARM;
		}
		return;

	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_SIDEARM:
		if (!sidearmRequestObserved)
		{
			failControllerProbeAppliedLoadoutReadiness(
				"changesidearm 1 was not reflected in public state");
			return;
		}
		if (m_iControllerProbeAppliedLoadoutStageFrames >= 2)
		{
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::SET_PRIMARY;
			m_iControllerProbeAppliedLoadoutStageFrames = 0;
			if (!issueControllerProbeAppliedLoadoutClientCommand(
				"changeprimary 6\n", "changeprimary-before-after"))
			{
				failControllerProbeAppliedLoadoutReadiness(
					"changeprimary 6 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_PRIMARY;
		}
		return;

	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_PRIMARY:
		if (!sidearmRequestObserved || !primaryRequestObserved)
		{
			failControllerProbeAppliedLoadoutReadiness(
				"changeprimary 6 was not reflected in public state");
			return;
		}
		if (m_iControllerProbeAppliedLoadoutStageFrames >= 2)
		{
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::SET_GRENADE;
			m_iControllerProbeAppliedLoadoutStageFrames = 0;
			if (!issueControllerProbeAppliedLoadoutClientCommand(
				"changegrenade 12\n", "changegrenade-before-after"))
			{
				failControllerProbeAppliedLoadoutReadiness(
					"changegrenade 12 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_GRENADE;
		}
		return;

	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_GRENADE:
		if (!sidearmRequestObserved || !primaryRequestObserved ||
			!grenadeRequestObserved)
		{
			failControllerProbeAppliedLoadoutReadiness(
				"changegrenade 12 was not reflected in public state");
			return;
		}
		if (m_iControllerProbeAppliedLoadoutStageFrames >= 2)
		{
			EmpiresBootstrapPlayerState beforeEquipment;
			if (!readBootstrapPlayerState(beforeEquipment) ||
				!controllerProbeAppliedLoadoutIdentityMatches(beforeEquipment) ||
				!appliedLoadoutFieldsKnown(beforeEquipment))
			{
				logger->Log(LogLevel::ERROR,
					"Empires controller probe appliedloadoutreadiness result classification=unknown reason=pre-equipment applied state unavailable");
				failControllerProbeAppliedLoadoutReadiness(
					"pre-equipment applied tuple or Desired flags unavailable");
				return;
			}

			m_controllerProbeAppliedLoadoutBeforeEquipment = beforeEquipment;
			m_bControllerProbeAppliedLoadoutBeforeEquipmentKnown = true;
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::SET_EQUIPMENT;
			m_iControllerProbeAppliedLoadoutStageFrames = 0;
			logControllerProbeAppliedLoadoutReadinessState(
				"pre-changeequipment", beforeEquipment);
			if (!issueControllerProbeAppliedLoadoutClientCommand(
				"changeequipment 21\n", "changeequipment-before-after"))
			{
				failControllerProbeAppliedLoadoutReadiness(
					"changeequipment 21 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_APPLIED_LOADOUT;
		}
		return;

	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_APPLIED_LOADOUT:
		if (m_iControllerProbeAppliedLoadoutStageFrames == 1)
			logControllerProbeAppliedLoadoutReadinessState("one-frame", state);
		else if (m_iControllerProbeAppliedLoadoutStageFrames == 2)
			logControllerProbeAppliedLoadoutReadinessState("two-frame", state);
		else if (m_iControllerProbeAppliedLoadoutStageFrames >=
			kControllerProbeAppliedLoadoutObservationFrames)
		{
			logControllerProbeAppliedLoadoutReadinessState("final-bounded", state);
			const char* classification = classifyAppliedLoadout(
				m_bControllerProbeAppliedLoadoutBeforeEquipmentKnown ?
					m_controllerProbeAppliedLoadoutBeforeEquipment : state, state);
			logger->Log(LogLevel::INFO,
				"Empires controller probe appliedloadoutreadiness result classification=%s applied_tuple=%d/%d/%d/%d/%d desired_flags=%d/%d/%d/%d/%d",
				classification, state.classId, state.appliedSidearm,
				state.appliedPrimary, state.appliedGrenade, state.appliedEquipment,
				state.classDesired ? 1 : 0, state.sidearmDesired ? 1 : 0,
				state.primaryDesired ? 1 : 0, state.grenadeDesired ? 1 : 0,
				state.equipmentDesired ? 1 : 0);

			if (std::strcmp(classification, "exact-application") != 0)
			{
				failControllerProbeAppliedLoadoutReadiness(
					"exact applied NF Rifleman tuple with all Desired flags clear was not reached");
				return;
			}

			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::SELECT_SPAWN;
			m_iControllerProbeAppliedLoadoutStageFrames = 0;
			if (!issueControllerProbeAppliedLoadoutClientCommand(
				"emp_spawn_select 32\n", "spawnpoint-after-applied-loadout"))
			{
				failControllerProbeAppliedLoadoutReadiness(
					"emp_spawn_select 32 could not be dispatched with stable client identity");
				return;
			}
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_SPAWN;
		}
		return;

	case EmpiresControllerProbeAppliedLoadoutReadinessState::OBSERVE_SPAWN:
		if (!appliedLoadoutMatches(state) ||
			!state.desiredSpawnKnown || state.desiredSpawn != 32)
		{
			failControllerProbeAppliedLoadoutReadiness(
				"spawn point 32 was not reflected after exact applied-loadout success");
			return;
		}
		if (m_iControllerProbeAppliedLoadoutStageFrames >= 2)
		{
			m_eControllerProbeAppliedLoadoutReadinessState =
				EmpiresControllerProbeAppliedLoadoutReadinessState::NEUTRAL_SIMULATION;
			m_iControllerProbeAppliedLoadoutStageFrames = 0;
			m_iControllerProbeAppliedLoadoutSubmittedCommands = 0;
			m_iControllerProbeAppliedLoadoutLastSubmittedTick = -1;
			m_iControllerProbeAppliedLoadoutLastSubmittedCommand = 0;
			m_iControllerProbeAppliedLoadoutLastProgressLogCommand = 0;
			m_bControllerProbeAppliedLoadoutNeutralStartTimeKnown = true;
			m_flControllerProbeAppliedLoadoutNeutralStartTime = state.currentTime;
			logger->Log(LogLevel::INFO,
				"Empires controller probe appliedloadoutreadiness transition state=NEUTRAL_SIMULATION neutral_input=enabled_only_after_exact_applied_tuple start=%.3f interval_per_tick=%.6f max_commands=%d",
				state.currentTime, gpGlobals != nullptr ? gpGlobals->interval_per_tick : 0.0f,
				m_iControllerProbeAppliedLoadoutMaxCommands);
		}
		return;

	case EmpiresControllerProbeAppliedLoadoutReadinessState::NEUTRAL_SIMULATION:
		if (!appliedLoadoutMatches(state) || !state.desiredSpawnKnown ||
			state.desiredSpawn != 32 || !state.lastUserCommandKnown ||
			state.lastUserCommandButtons != 0 ||
			std::fabs(state.lastUserCommandForwardMove) > 0.01f ||
			std::fabs(state.lastUserCommandSideMove) > 0.01f ||
			std::fabs(state.lastUserCommandUpMove) > 0.01f ||
			state.lastUserCommandImpulse != 0 ||
			state.lastUserCommandWeaponSelect != 0)
		{
			failControllerProbeAppliedLoadoutReadiness(
				"non-neutral input or exact applied-loadout state was not present before the next neutral command");
			return;
		}
		if (m_bControllerProbeAppliedLoadoutNeutralStartTimeKnown &&
			state.currentTime >= m_flControllerProbeAppliedLoadoutNeutralStartTime + 20.0f)
		{
			logControllerProbeAppliedLoadoutReadinessState("neutral-safety-timeout", state);
			failControllerProbeAppliedLoadoutReadiness(
				"20-second neutral simulation safety timeout reached");
			return;
		}
		if (gpGlobals == nullptr || gpGlobals->tickcount ==
			m_iControllerProbeAppliedLoadoutLastSubmittedTick)
			return;
		if (!submitControllerProbeAppliedLoadoutNeutralUserCmd("neutral-frame") &&
			m_eControllerProbeAppliedLoadoutReadinessState !=
				EmpiresControllerProbeAppliedLoadoutReadinessState::FAILED)
		{
			failControllerProbeAppliedLoadoutReadiness(
				"neutral RunPlayerMove submission or readback failed");
		}
		return;

	case EmpiresControllerProbeAppliedLoadoutReadinessState::IDLE:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::JOIN_NF:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::REQUEST_RIFLEMAN:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SET_SIDEARM:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SET_PRIMARY:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SET_GRENADE:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SET_EQUIPMENT:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::SELECT_SPAWN:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::LIFE_RESPAWNABLE_REACHED:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::COMPLETE:
	case EmpiresControllerProbeAppliedLoadoutReadinessState::FAILED:
		return;
	}
}

bool CBotEmpires::submitControllerProbeLoadoutNeutralUserCmd(const char* phase)
{
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLoadoutReadinessState !=
			EmpiresControllerProbeLoadoutReadinessState::NEUTRAL_SIMULATION)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness rejected UserCmd phase=%s outside NEUTRAL_SIMULATION",
			phase != nullptr ? phase : "unknown");
		return false;
	}
	if (m_iControllerProbeLoadoutSubmittedCommands >=
		m_iControllerProbeLoadoutMaxCommands)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness command cap reached submitted=%d cap=%d",
			m_iControllerProbeLoadoutSubmittedCommands,
			m_iControllerProbeLoadoutMaxCommands);
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) ||
		!controllerProbeLoadoutIdentityMatches(before))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness phase=%s rejected: fresh client identity unavailable or changed before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}
	if (before.lifeStateKnown && before.lifeState == LIFE_RESPAWNABLE)
	{
		m_eControllerProbeLoadoutReadinessState =
			EmpiresControllerProbeLoadoutReadinessState::LIFE_RESPAWNABLE_REACHED;
		logControllerProbeLoadoutReadinessState("life-respawnable-reached", before);
		m_eControllerProbeLoadoutReadinessState =
			EmpiresControllerProbeLoadoutReadinessState::COMPLETE;
		logControllerProbeLoadoutReadinessState("complete", before);
		return true;
	}
	if (!before.lifeStateKnown ||
		(before.lifeState != LIFE_DYING && before.lifeState != LIFE_DEAD) ||
		before.team != EMP_TEAM_NF || before.health > 0)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness phase=%s rejected: pre-command lifecycle/readiness invalid life_state=%d team=%d alive=%d health=%d",
			phase != nullptr ? phase : "unknown", before.lifeState, before.team,
			before.alive ? 1 : 0, before.health);
		return false;
	}

	if (g_pBotManager == nullptr || playerinfomanager == nullptr || gpGlobals == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness phase=%s rejected: SDK interfaces unavailable before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}
	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(
		const_cast<edict_t*>(before.edict));
	IBotController* pController = g_pBotManager->GetBotController(
		const_cast<edict_t*>(before.edict));
	if (pInfo == nullptr || pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness phase=%s rejected: fresh PlayerInfo=%s controller=%s",
			phase != nullptr ? phase : "unknown", pInfo != nullptr ? "available" : "null",
			pController != nullptr ? "available" : "null");
		return false;
	}

	const CBotCmd lastCommand = pInfo->GetLastUserCommand();
	if (lastCommand.command_number != before.lastUserCommandNumber ||
		lastCommand.tick_count != before.lastUserCommandTick)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness phase=%s rejected: last command changed before construction state_number=%d fresh_number=%d state_tick=%d fresh_tick=%d",
			phase != nullptr ? phase : "unknown", before.lastUserCommandNumber,
			lastCommand.command_number, before.lastUserCommandTick,
			lastCommand.tick_count);
		return false;
	}

	const int nextCommandNumber = lastCommand.command_number > 0 ?
		lastCommand.command_number + 1 : 1;
	if (m_iControllerProbeLoadoutSubmittedCommands > 0 &&
		nextCommandNumber != m_iControllerProbeLoadoutLastSubmittedCommand + 1)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness phase=%s rejected: command number is not monotonic previous=%d next=%d",
			phase != nullptr ? phase : "unknown",
			m_iControllerProbeLoadoutLastSubmittedCommand, nextCommandNumber);
		return false;
	}

	CBotCmd userCommand;
	userCommand.Reset();
	userCommand.command_number = nextCommandNumber;
	userCommand.tick_count = gpGlobals->tickcount;
	userCommand.viewangles = lastCommand.viewangles;
	userCommand.forwardmove = 0.0f;
	userCommand.sidemove = 0.0f;
	userCommand.upmove = 0.0f;
	userCommand.buttons = 0;
	userCommand.impulse = 0;
	userCommand.weaponselect = 0;
	userCommand.weaponsubtype = 0;
	userCommand.random_seed = 0;
	userCommand.mousedx = 0;
	userCommand.mousedy = 0;
	userCommand.hasbeenpredicted = false;

	logger->Log(LogLevel::INFO,
		"Empires controller probe loadoutreadiness submit phase=%s index=%d userid=%d edict=%p command_number=%d tick_count=%d buttons=0x%x forwardmove=%.1f sidemove=%.1f upmove=%.1f impulse=%d weaponselect=%d",
		phase != nullptr ? phase : "unknown", before.index, before.userId,
		static_cast<const void*>(before.edict), userCommand.command_number,
		userCommand.tick_count, userCommand.buttons, userCommand.forwardmove,
		userCommand.sidemove, userCommand.upmove, userCommand.impulse,
		userCommand.weaponselect);

	// The only UserCmd call in this new test. All preceding stages use public
	// client commands and normal bot movement is gated by suppressBotThink().
	pController->RunPlayerMove(&userCommand);

	EmpiresBootstrapPlayerState after;
	if (!readBootstrapPlayerState(after) ||
		!controllerProbeLoadoutIdentityMatches(after))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness phase=%s failed: client identity changed or state unavailable after RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}
	if (g_pBotManager->GetBotController(const_cast<edict_t*>(after.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness phase=%s failed: controller unavailable after RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	const bool commandVisible = after.lastUserCommandKnown &&
		after.lastUserCommandNumber == userCommand.command_number &&
		after.lastUserCommandTick > before.lastUserCommandTick;
	const bool commandNeutral = after.lastUserCommandKnown &&
		after.lastUserCommandButtons == 0 &&
		std::fabs(after.lastUserCommandForwardMove) <= 0.01f &&
		std::fabs(after.lastUserCommandSideMove) <= 0.01f &&
		std::fabs(after.lastUserCommandUpMove) <= 0.01f &&
		after.lastUserCommandImpulse == 0 &&
		after.lastUserCommandWeaponSelect == 0;
	if (!commandVisible || !commandNeutral)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe loadoutreadiness phase=%s failed: neutral command readback visible=%d neutral=%d stored_number=%d stored_tick=%d buttons=0x%x forward=%.1f side=%.1f up=%.1f impulse=%d weaponselect=%d",
			phase != nullptr ? phase : "unknown", commandVisible ? 1 : 0,
			commandNeutral ? 1 : 0, after.lastUserCommandNumber,
			after.lastUserCommandTick, after.lastUserCommandButtons,
			after.lastUserCommandForwardMove, after.lastUserCommandSideMove,
			after.lastUserCommandUpMove, after.lastUserCommandImpulse,
			after.lastUserCommandWeaponSelect);
		return false;
	}

	++m_iControllerProbeLoadoutSubmittedCommands;
	m_iControllerProbeLoadoutLastSubmittedCommand = userCommand.command_number;
	m_iControllerProbeLoadoutLastSubmittedTick = userCommand.tick_count;
	recordControllerProbeLoadoutReadinessChanges(after);
	if (m_iControllerProbeLoadoutSubmittedCommands <= 3 ||
		m_iControllerProbeLoadoutSubmittedCommands -
			m_iControllerProbeLoadoutLastProgressLogCommand >=
			kControllerProbeLoadoutReadinessProgressInterval)
	{
		logControllerProbeLoadoutReadinessState("neutral-progress", after);
		m_iControllerProbeLoadoutLastProgressLogCommand =
			m_iControllerProbeLoadoutSubmittedCommands;
	}

	if (after.lifeStateKnown && after.lifeState == LIFE_RESPAWNABLE)
	{
		m_eControllerProbeLoadoutReadinessState =
			EmpiresControllerProbeLoadoutReadinessState::LIFE_RESPAWNABLE_REACHED;
		logControllerProbeLoadoutReadinessState("life-respawnable-reached", after);
		m_eControllerProbeLoadoutReadinessState =
			EmpiresControllerProbeLoadoutReadinessState::COMPLETE;
		logControllerProbeLoadoutReadinessState("complete", after);
	}
	else if (after.lifeStateKnown && after.lifeState == LIFE_ALIVE)
	{
		failControllerProbeLoadoutReadiness(
			"client became alive during neutral simulation");
		return false;
	}
	return true;
}

bool CBotEmpires::submitControllerProbeAppliedLoadoutNeutralUserCmd(
	const char* phase)
{
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeAppliedLoadoutReadinessState !=
			EmpiresControllerProbeAppliedLoadoutReadinessState::NEUTRAL_SIMULATION)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness rejected UserCmd phase=%s outside NEUTRAL_SIMULATION",
			phase != nullptr ? phase : "unknown");
		return false;
	}
	if (m_iControllerProbeAppliedLoadoutSubmittedCommands >=
		m_iControllerProbeAppliedLoadoutMaxCommands)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness command cap reached submitted=%d cap=%d",
			m_iControllerProbeAppliedLoadoutSubmittedCommands,
			m_iControllerProbeAppliedLoadoutMaxCommands);
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) ||
		!controllerProbeAppliedLoadoutIdentityMatches(before))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness phase=%s rejected: fresh client identity unavailable or changed before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}
	if (!appliedLoadoutMatches(before) || !before.desiredSpawnKnown ||
		before.desiredSpawn != 32 || !before.lifeStateKnown ||
		(before.lifeState != LIFE_DYING && before.lifeState != LIFE_DEAD) ||
		!before.healthKnown || before.health > 0)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness phase=%s rejected: pre-command public state was not exactly ready for neutral simulation applied_tuple=%d/%d/%d/%d/%d desired_flags=%d/%d/%d/%d/%d spawn=%d life_state=%d health=%d",
			phase != nullptr ? phase : "unknown", before.classId,
			before.appliedSidearm, before.appliedPrimary, before.appliedGrenade,
			before.appliedEquipment, before.classDesired ? 1 : 0,
			before.sidearmDesired ? 1 : 0, before.primaryDesired ? 1 : 0,
			before.grenadeDesired ? 1 : 0, before.equipmentDesired ? 1 : 0,
			before.desiredSpawn, before.lifeState, before.health);
		return false;
	}

	if (g_pBotManager == nullptr || playerinfomanager == nullptr || gpGlobals == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness phase=%s rejected: SDK interfaces unavailable before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}
	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(
		const_cast<edict_t*>(before.edict));
	IBotController* pController = g_pBotManager->GetBotController(
		const_cast<edict_t*>(before.edict));
	if (pInfo == nullptr || pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness phase=%s rejected: fresh PlayerInfo=%s controller=%s",
			phase != nullptr ? phase : "unknown", pInfo != nullptr ? "available" : "null",
			pController != nullptr ? "available" : "null");
		return false;
	}

	const CBotCmd lastCommand = pInfo->GetLastUserCommand();
	if (lastCommand.command_number != before.lastUserCommandNumber ||
		lastCommand.tick_count != before.lastUserCommandTick)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness phase=%s rejected: last command changed before construction state_number=%d fresh_number=%d state_tick=%d fresh_tick=%d",
			phase != nullptr ? phase : "unknown", before.lastUserCommandNumber,
			lastCommand.command_number, before.lastUserCommandTick,
			lastCommand.tick_count);
		return false;
	}

	const int nextCommandNumber = lastCommand.command_number > 0 ?
		lastCommand.command_number + 1 : 1;
	if (m_iControllerProbeAppliedLoadoutSubmittedCommands > 0 &&
		nextCommandNumber != m_iControllerProbeAppliedLoadoutLastSubmittedCommand + 1)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness phase=%s rejected: command number is not monotonic previous=%d next=%d",
			phase != nullptr ? phase : "unknown",
			m_iControllerProbeAppliedLoadoutLastSubmittedCommand, nextCommandNumber);
		return false;
	}

	CBotCmd userCommand;
	userCommand.Reset();
	userCommand.command_number = nextCommandNumber;
	userCommand.tick_count = gpGlobals->tickcount;
	userCommand.viewangles = lastCommand.viewangles;
	userCommand.forwardmove = 0.0f;
	userCommand.sidemove = 0.0f;
	userCommand.upmove = 0.0f;
	userCommand.buttons = 0;
	userCommand.impulse = 0;
	userCommand.weaponselect = 0;
	userCommand.weaponsubtype = 0;
	userCommand.random_seed = 0;
	userCommand.mousedx = 0;
	userCommand.mousedy = 0;
	userCommand.hasbeenpredicted = false;

	logger->Log(LogLevel::INFO,
		"Empires controller probe appliedloadoutreadiness submit phase=%s index=%d userid=%d edict=%p command_number=%d tick_count=%d buttons=0x%x forwardmove=%.1f sidemove=%.1f upmove=%.1f impulse=%d weaponselect=%d",
		phase != nullptr ? phase : "unknown", before.index, before.userId,
		static_cast<const void*>(before.edict), userCommand.command_number,
		userCommand.tick_count, userCommand.buttons, userCommand.forwardmove,
		userCommand.sidemove, userCommand.upmove, userCommand.impulse,
		userCommand.weaponselect);

	pController->RunPlayerMove(&userCommand);

	EmpiresBootstrapPlayerState after;
	if (!readBootstrapPlayerState(after) ||
		!controllerProbeAppliedLoadoutIdentityMatches(after))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness phase=%s failed: client identity changed or state unavailable after RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}
	if (g_pBotManager->GetBotController(const_cast<edict_t*>(after.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness phase=%s failed: controller unavailable after RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	const bool commandVisible = after.lastUserCommandKnown &&
		after.lastUserCommandNumber == userCommand.command_number &&
		after.lastUserCommandTick > before.lastUserCommandTick;
	const bool commandNeutral = after.lastUserCommandKnown &&
		after.lastUserCommandButtons == 0 &&
		std::fabs(after.lastUserCommandForwardMove) <= 0.01f &&
		std::fabs(after.lastUserCommandSideMove) <= 0.01f &&
		std::fabs(after.lastUserCommandUpMove) <= 0.01f &&
		after.lastUserCommandImpulse == 0 &&
		after.lastUserCommandWeaponSelect == 0;
	if (!commandVisible || !commandNeutral)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe appliedloadoutreadiness phase=%s failed: neutral command readback visible=%d neutral=%d stored_number=%d stored_tick=%d buttons=0x%x forward=%.1f side=%.1f up=%.1f impulse=%d weaponselect=%d",
			phase != nullptr ? phase : "unknown", commandVisible ? 1 : 0,
			commandNeutral ? 1 : 0, after.lastUserCommandNumber,
			after.lastUserCommandTick, after.lastUserCommandButtons,
			after.lastUserCommandForwardMove, after.lastUserCommandSideMove,
			after.lastUserCommandUpMove, after.lastUserCommandImpulse,
			after.lastUserCommandWeaponSelect);
		return false;
	}

	++m_iControllerProbeAppliedLoadoutSubmittedCommands;
	m_iControllerProbeAppliedLoadoutLastSubmittedCommand = userCommand.command_number;
	m_iControllerProbeAppliedLoadoutLastSubmittedTick = userCommand.tick_count;
	recordControllerProbeAppliedLoadoutReadinessChanges(after);
	if (m_iControllerProbeAppliedLoadoutSubmittedCommands <= 3 ||
		m_iControllerProbeAppliedLoadoutSubmittedCommands -
			m_iControllerProbeAppliedLoadoutLastProgressLogCommand >= 64)
	{
		logControllerProbeAppliedLoadoutReadinessState("neutral-progress", after);
		m_iControllerProbeAppliedLoadoutLastProgressLogCommand =
			m_iControllerProbeAppliedLoadoutSubmittedCommands;
	}

	if (after.lifeStateKnown && after.lifeState == LIFE_RESPAWNABLE)
	{
		if (!appliedLoadoutMatches(after) || !after.desiredSpawnKnown ||
			after.desiredSpawn != 32)
		{
			failControllerProbeAppliedLoadoutReadiness(
				"LIFE_RESPAWNABLE was reached with invalid applied tuple or spawn point");
			return false;
		}
		m_eControllerProbeAppliedLoadoutReadinessState =
			EmpiresControllerProbeAppliedLoadoutReadinessState::LIFE_RESPAWNABLE_REACHED;
		logControllerProbeAppliedLoadoutReadinessState(
			"life-respawnable-reached", after);
		m_eControllerProbeAppliedLoadoutReadinessState =
			EmpiresControllerProbeAppliedLoadoutReadinessState::COMPLETE;
		logControllerProbeAppliedLoadoutReadinessState("complete", after);
	}
	else if (after.lifeStateKnown && after.lifeState == LIFE_ALIVE)
	{
		failControllerProbeAppliedLoadoutReadiness(
			"client became alive during neutral simulation");
		return false;
	}

	return true;
}

void CBotEmpires::failControllerProbeSpawnConfirmReady(const char* reason)
{
	logger->Log(LogLevel::ERROR,
		"Empires controller probe spawnconfirm_ready result=FAILED state=%s reason=%s press_count=%d release_count=%d observe_frames=%d expected_index=%d expected_userid=%d",
		controllerProbeSpawnConfirmReadyStateName(),
		reason != nullptr ? reason : "unknown",
		m_iControllerProbeSpawnConfirmReadyPressCount,
		m_iControllerProbeSpawnConfirmReadyReleaseCount,
		m_iControllerProbeSpawnConfirmReadyObserveFrames,
		m_iControllerProbeSpawnConfirmReadyExpectedIndex,
		m_iControllerProbeSpawnConfirmReadyExpectedUserId);
	m_eControllerProbeSpawnConfirmReadyState =
		EmpiresControllerProbeSpawnConfirmReadyState::FAILED;
}

bool CBotEmpires::submitControllerProbeSpawnConfirmReadyUserCmd(
	const int buttons, const char* phase)
{
	if (!m_bControllerProbeOnly ||
		(m_eControllerProbeSpawnConfirmReadyState !=
			EmpiresControllerProbeSpawnConfirmReadyState::ATTACK_PRESS_PENDING &&
		 m_eControllerProbeSpawnConfirmReadyState !=
			EmpiresControllerProbeSpawnConfirmReadyState::ATTACK_RELEASE_PENDING) ||
		(buttons != 0 && buttons != IN_ATTACK))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready phase=%s rejected: invalid state or buttons=0x%x",
			phase != nullptr ? phase : "unknown", buttons);
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) ||
		!controllerProbeSpawnConfirmReadyIdentityMatches(before))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready phase=%s rejected: fresh client identity unavailable or changed before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	if (playerinfomanager == nullptr || g_pBotManager == nullptr ||
		gpGlobals == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready phase=%s rejected: SDK interfaces unavailable before RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(
		const_cast<edict_t*>(before.edict));
	IBotController* pController = g_pBotManager->GetBotController(
		const_cast<edict_t*>(before.edict));
	if (pInfo == nullptr || pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready phase=%s rejected: fresh PlayerInfo=%s controller=%s",
			phase != nullptr ? phase : "unknown",
			pInfo != nullptr ? "available" : "null",
			pController != nullptr ? "available" : "null");
		return false;
	}

	const CBotCmd lastCommand = pInfo->GetLastUserCommand();
	if (!before.lastUserCommandKnown ||
		lastCommand.command_number != before.lastUserCommandNumber ||
		lastCommand.tick_count != before.lastUserCommandTick)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready phase=%s rejected: last command changed before construction state_number=%d fresh_number=%d state_tick=%d fresh_tick=%d",
			phase != nullptr ? phase : "unknown", before.lastUserCommandNumber,
			lastCommand.command_number, before.lastUserCommandTick,
			lastCommand.tick_count);
		return false;
	}

	const int nextCommandNumber = lastCommand.command_number > 0 ?
		lastCommand.command_number + 1 : 1;
	if (m_iControllerProbeSpawnConfirmReadyLastSubmittedCommand > 0 &&
		nextCommandNumber !=
			m_iControllerProbeSpawnConfirmReadyLastSubmittedCommand + 1)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready phase=%s rejected: command number is not previous submitted plus one previous=%d next=%d",
			phase != nullptr ? phase : "unknown",
			m_iControllerProbeSpawnConfirmReadyLastSubmittedCommand,
			nextCommandNumber);
		return false;
	}

	CBotCmd userCommand;
	userCommand.Reset();
	userCommand.command_number = nextCommandNumber;
	userCommand.tick_count = gpGlobals->tickcount;
	userCommand.viewangles = lastCommand.viewangles;
	userCommand.forwardmove = 0.0f;
	userCommand.sidemove = 0.0f;
	userCommand.upmove = 0.0f;
	userCommand.buttons = buttons;
	userCommand.impulse = 0;
	userCommand.weaponselect = 0;
	userCommand.weaponsubtype = 0;
	userCommand.random_seed = 0;
	userCommand.mousedx = 0;
	userCommand.mousedy = 0;
	userCommand.hasbeenpredicted = false;

	logger->Log(LogLevel::INFO,
		"Empires controller probe spawnconfirm_ready submit phase=%s index=%d userid=%d edict=%p command_number=%d tick_count=%d buttons=0x%x forwardmove=%.1f sidemove=%.1f upmove=%.1f impulse=%d weaponselect=%d viewangles=(%.1f %.1f %.1f)",
		phase != nullptr ? phase : "unknown", before.index, before.userId,
		static_cast<const void*>(before.edict), userCommand.command_number,
		userCommand.tick_count, userCommand.buttons, userCommand.forwardmove,
		userCommand.sidemove, userCommand.upmove, userCommand.impulse,
		userCommand.weaponselect, userCommand.viewangles.x,
		userCommand.viewangles.y, userCommand.viewangles.z);

	// This is the only input owner for spawnconfirm_ready. The PlayerInfo and
	// controller are local to this frame and are reacquired for the release.
	pController->RunPlayerMove(&userCommand);

	EmpiresBootstrapPlayerState after;
	if (!readBootstrapPlayerState(after) ||
		!controllerProbeSpawnConfirmReadyIdentityMatches(after))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready phase=%s failed: client identity changed or state unavailable after RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}
	if (g_pBotManager->GetBotController(const_cast<edict_t*>(after.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready phase=%s failed: fresh controller unavailable after RunPlayerMove",
			phase != nullptr ? phase : "unknown");
		return false;
	}

	const bool commandVisible = after.lastUserCommandKnown &&
		after.lastUserCommandNumber == userCommand.command_number &&
		after.lastUserCommandTick > before.lastUserCommandTick;
	const bool commandMatches = after.lastUserCommandKnown &&
		after.lastUserCommandButtons == buttons &&
		std::fabs(after.lastUserCommandForwardMove) <= 0.01f &&
		std::fabs(after.lastUserCommandSideMove) <= 0.01f &&
		std::fabs(after.lastUserCommandUpMove) <= 0.01f &&
		after.lastUserCommandImpulse == 0 &&
		after.lastUserCommandWeaponSelect == 0;
	if (!commandVisible || !commandMatches)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready phase=%s failed: command readback visible=%d matches=%d stored_number=%d stored_tick=%d buttons=0x%x forward=%.1f side=%.1f up=%.1f impulse=%d weaponselect=%d",
			phase != nullptr ? phase : "unknown", commandVisible ? 1 : 0,
			commandMatches ? 1 : 0, after.lastUserCommandNumber,
			after.lastUserCommandTick, after.lastUserCommandButtons,
			after.lastUserCommandForwardMove, after.lastUserCommandSideMove,
			after.lastUserCommandUpMove, after.lastUserCommandImpulse,
			after.lastUserCommandWeaponSelect);
		return false;
	}

	m_iControllerProbeSpawnConfirmReadyLastSubmittedCommand =
		userCommand.command_number;
	m_iControllerProbeSpawnConfirmReadyLastSubmittedTick = userCommand.tick_count;
	if (buttons == IN_ATTACK)
		++m_iControllerProbeSpawnConfirmReadyPressCount;
	else
		++m_iControllerProbeSpawnConfirmReadyReleaseCount;
	logControllerProbeSpawnConfirmReadyState(
		phase != nullptr ? phase : "usercmd-after", after);
	return true;
}

bool CBotEmpires::requestControllerProbeSpawnConfirmReady()
{
	const bool noPendingInput =
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE &&
		m_eControllerProbeSpawnHoldState ==
			EmpiresControllerProbeSpawnHoldState::IDLE &&
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::IDLE &&
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::IDLE &&
		m_eControllerProbeSpawnConfirmReadyState ==
			EmpiresControllerProbeSpawnConfirmReadyState::IDLE &&
		!m_bTeamCommandPending && !m_bClassCommandPending &&
		!m_bSpawnCommandPending &&
		m_eSpawnConfirmationState ==
			EmpiresBootstrapSpawnConfirmationState::IDLE;

	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle !=
			EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr ||
		m_eControllerProbeAppliedLoadoutReadinessState !=
			EmpiresControllerProbeAppliedLoadoutReadinessState::COMPLETE ||
		!noPendingInput)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe spawnconfirm_ready rejected: requires completed applied-loadout readiness and no pending operation applied_state=%s spawnconfirm_state=%s lifecycle=%s",
			controllerProbeAppliedLoadoutReadinessStateName(),
			controllerProbeSpawnConfirmReadyStateName(),
			controllerProbeLifecycleName());
		return false;
	}

	EmpiresBootstrapPlayerState initial;
	if (!readBootstrapPlayerState(initial) || initial.edict == nullptr ||
		initial.edict->IsFree() || initial.index <= 0 || initial.userId <= 0)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready rejected: fresh slot identity unavailable index=%d userid=%d edict=%p",
			initial.index, initial.userId, static_cast<const void*>(initial.edict));
		return false;
	}

	IBotController* pController = g_pBotManager != nullptr ?
		g_pBotManager->GetBotController(const_cast<edict_t*>(initial.edict)) : nullptr;
	if (pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready rejected: fresh controller unavailable index=%d userid=%d edict=%p",
			initial.index, initial.userId, static_cast<const void*>(initial.edict));
		return false;
	}

	if (!controllerProbeSpawnConfirmReadyPredicate(initial))
	{
		logControllerProbeSpawnConfirmReadyState("precondition-rejected", initial);
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnconfirm_ready rejected: exact NF/Rifleman/applied-loadout/spawn32/LIFE_RESPAWNABLE/health0/neutral-input predicate failed");
		return false;
	}

	m_iControllerProbeSpawnConfirmReadyExpectedIndex = initial.index;
	m_iControllerProbeSpawnConfirmReadyExpectedUserId = initial.userId;
	m_pControllerProbeSpawnConfirmReadyExpectedEdict = initial.edict;
	m_iControllerProbeSpawnConfirmReadyPressCount = 0;
	m_iControllerProbeSpawnConfirmReadyReleaseCount = 0;
	m_iControllerProbeSpawnConfirmReadyObserveFrames = 0;
	m_iControllerProbeSpawnConfirmReadyLastSubmittedCommand = 0;
	m_iControllerProbeSpawnConfirmReadyLastSubmittedTick = -1;
	m_bControllerProbeSpawnConfirmReadyLastLifeStateKnown =
		initial.lifeStateKnown;
	m_iControllerProbeSpawnConfirmReadyLastLifeState = initial.lifeState;
	m_vControllerProbeSpawnConfirmReadyOrigin = initial.origin;
	m_bControllerProbeSpawnConfirmReadyOriginKnown = initial.originKnown;
	m_eControllerProbeSpawnConfirmReadyState =
		EmpiresControllerProbeSpawnConfirmReadyState::ATTACK_PRESS_PENDING;

	logger->Log(LogLevel::INFO,
		"Empires controller probe spawnconfirm_ready armed exactly_once=1 press_frames=1 release_frames=1 observation_frames=%d expected_team=2 expected_class=1 expected_loadout=1/6/12/21 expected_spawn=32 precondition_life_state=LIFE_RESPAWNABLE precondition_health=0",
		kControllerProbeSpawnConfirmReadyObservationFrames);
	logControllerProbeSpawnConfirmReadyState("pre-attack", initial);
	return true;
}

void CBotEmpires::observeControllerProbeSpawnConfirmReady()
{
	if (m_eControllerProbeSpawnConfirmReadyState ==
			EmpiresControllerProbeSpawnConfirmReadyState::IDLE ||
		m_eControllerProbeSpawnConfirmReadyState ==
			EmpiresControllerProbeSpawnConfirmReadyState::COMPLETE ||
		m_eControllerProbeSpawnConfirmReadyState ==
			EmpiresControllerProbeSpawnConfirmReadyState::FAILED)
		return;

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state) ||
		!controllerProbeSpawnConfirmReadyIdentityMatches(state))
	{
		failControllerProbeSpawnConfirmReady(
			"fresh client identity unavailable or changed");
		return;
	}

	if (!m_bControllerProbeSpawnConfirmReadyLastLifeStateKnown ||
		(state.lifeStateKnown && state.lifeState !=
			m_iControllerProbeSpawnConfirmReadyLastLifeState))
	{
		if (m_bControllerProbeSpawnConfirmReadyLastLifeStateKnown &&
			state.lifeStateKnown)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe spawnconfirm_ready life_state_transition from=%s to=%s curtime=%.3f tickcount=%d",
				lifeStateName(m_iControllerProbeSpawnConfirmReadyLastLifeState),
				lifeStateName(state.lifeState), state.currentTime,
				gpGlobals != nullptr ? gpGlobals->tickcount : -1);
		}
		m_bControllerProbeSpawnConfirmReadyLastLifeStateKnown =
			state.lifeStateKnown;
		m_iControllerProbeSpawnConfirmReadyLastLifeState = state.lifeState;
	}

	const bool controllerAvailable = g_pBotManager != nullptr &&
		g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict)) != nullptr;
	if (!controllerAvailable)
	{
		failControllerProbeSpawnConfirmReady("fresh controller unavailable during observation");
		return;
	}

	switch (m_eControllerProbeSpawnConfirmReadyState)
	{
	case EmpiresControllerProbeSpawnConfirmReadyState::ATTACK_PRESS_PENDING:
		if (m_iControllerProbeSpawnConfirmReadyPressCount != 0 ||
			!controllerProbeSpawnConfirmReadyPredicate(state))
		{
			logControllerProbeSpawnConfirmReadyState("pre-attack-rejected", state);
			failControllerProbeSpawnConfirmReady(
				"fresh pre-attack readiness predicate failed; no attack submitted");
			return;
		}
		if (!submitControllerProbeSpawnConfirmReadyUserCmd(IN_ATTACK,
			"attack-press"))
		{
			failControllerProbeSpawnConfirmReady(
				"exactly-one IN_ATTACK press could not be submitted");
			return;
		}
		m_eControllerProbeSpawnConfirmReadyState =
			EmpiresControllerProbeSpawnConfirmReadyState::ATTACK_RELEASE_PENDING;
		logger->Log(LogLevel::INFO,
			"Empires controller probe spawnconfirm_ready transition state=ATTACK_RELEASE_PENDING press_count=%d release_count=%d next_action=mandatory_neutral_release",
			m_iControllerProbeSpawnConfirmReadyPressCount,
			m_iControllerProbeSpawnConfirmReadyReleaseCount);
		return;

	case EmpiresControllerProbeSpawnConfirmReadyState::ATTACK_RELEASE_PENDING:
		if (m_iControllerProbeSpawnConfirmReadyPressCount != 1 ||
			m_iControllerProbeSpawnConfirmReadyReleaseCount != 0)
		{
			failControllerProbeSpawnConfirmReady(
				"attack/release counters were not exactly one/zero before release");
			return;
		}
		if (gpGlobals != nullptr && gpGlobals->tickcount <=
			m_iControllerProbeSpawnConfirmReadyLastSubmittedTick)
			return;

		// The release is mandatory even when the press already changed the
		// life state. It is submitted on the next simulation frame only.
		if (!submitControllerProbeSpawnConfirmReadyUserCmd(0,
			"attack-release"))
		{
			failControllerProbeSpawnConfirmReady(
				"mandatory next-frame neutral release could not be submitted");
			return;
		}
		m_eControllerProbeSpawnConfirmReadyState =
			EmpiresControllerProbeSpawnConfirmReadyState::OBSERVE_SPAWN;
		m_iControllerProbeSpawnConfirmReadyObserveFrames = 0;
		logger->Log(LogLevel::INFO,
			"Empires controller probe spawnconfirm_ready transition state=OBSERVE_SPAWN press_count=%d release_count=%d",
			m_iControllerProbeSpawnConfirmReadyPressCount,
			m_iControllerProbeSpawnConfirmReadyReleaseCount);
		return;

	case EmpiresControllerProbeSpawnConfirmReadyState::OBSERVE_SPAWN:
		++m_iControllerProbeSpawnConfirmReadyObserveFrames;
		{
			const bool aliveResult = state.team == EMP_TEAM_NF &&
				state.classKnown && state.classId == EMP_CLASS_RIFLEMAN &&
				state.healthKnown && state.health > 0 &&
				state.lifeStateKnown && state.lifeState == LIFE_ALIVE &&
				state.alive && !state.dead &&
				state.lastUserCommandKnown &&
				state.lastUserCommandButtons == 0 &&
				std::fabs(state.lastUserCommandForwardMove) <= 0.01f &&
				std::fabs(state.lastUserCommandSideMove) <= 0.01f &&
				std::fabs(state.lastUserCommandUpMove) <= 0.01f &&
				state.lastUserCommandImpulse == 0 &&
				state.lastUserCommandWeaponSelect == 0 &&
				m_iControllerProbeSpawnConfirmReadyPressCount == 1 &&
				m_iControllerProbeSpawnConfirmReadyReleaseCount == 1;
			const bool shouldLog = m_iControllerProbeSpawnConfirmReadyObserveFrames == 1 ||
				aliveResult ||
				(m_iControllerProbeSpawnConfirmReadyObserveFrames % 16) == 0 ||
				m_iControllerProbeSpawnConfirmReadyObserveFrames >=
					kControllerProbeSpawnConfirmReadyObservationFrames;
			if (shouldLog)
				logControllerProbeSpawnConfirmReadyState("observe", state);

			if (aliveResult)
			{
				m_eControllerProbeSpawnConfirmReadyState =
					EmpiresControllerProbeSpawnConfirmReadyState::COMPLETE;
				logControllerProbeSpawnConfirmReadyState("complete", state);
				return;
			}

			if (m_iControllerProbeSpawnConfirmReadyObserveFrames >=
				kControllerProbeSpawnConfirmReadyObservationFrames)
			{
				failControllerProbeSpawnConfirmReady(
					"client did not become alive within 120 frames after one press and one release");
			}
		}
		return;

	case EmpiresControllerProbeSpawnConfirmReadyState::IDLE:
	case EmpiresControllerProbeSpawnConfirmReadyState::COMPLETE:
	case EmpiresControllerProbeSpawnConfirmReadyState::FAILED:
		return;
	}
}

bool CBotEmpires::requestControllerProbeMovementProof()
{
	const bool noPendingInput =
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE &&
		m_eControllerProbeSpawnHoldState ==
			EmpiresControllerProbeSpawnHoldState::IDLE &&
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::IDLE &&
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::IDLE &&
		m_eControllerProbeAppliedLoadoutReadinessState ==
			EmpiresControllerProbeAppliedLoadoutReadinessState::COMPLETE &&
		m_eControllerProbeSpawnConfirmReadyState ==
			EmpiresControllerProbeSpawnConfirmReadyState::COMPLETE &&
		m_eControllerProbeMovementState ==
			EmpiresControllerProbeMovementState::IDLE &&
		!m_bTeamCommandPending && !m_bClassCommandPending &&
		!m_bSpawnCommandPending &&
		m_eSpawnConfirmationState ==
			EmpiresBootstrapSpawnConfirmationState::IDLE;

	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle !=
			EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr || !noPendingInput)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe movement rejected: requires completed spawn confirmation and no pending input spawnconfirm_state=%s movement_state=%s lifecycle=%s",
			controllerProbeSpawnConfirmReadyStateName(),
			controllerProbeMovementStateName(), controllerProbeLifecycleName());
		return false;
	}

	EmpiresBootstrapPlayerState initial;
	if (!readBootstrapPlayerState(initial) || initial.edict == nullptr ||
		initial.edict->IsFree() || initial.index <= 0 || initial.userId <= 0 ||
		!initial.originKnown || !initial.alive || initial.dead ||
		!initial.healthKnown || initial.health <= 0 ||
		!initial.lifeStateKnown || initial.lifeState != LIFE_ALIVE ||
		initial.team != EMP_TEAM_NF || !initial.classKnown ||
		initial.classId != EMP_CLASS_RIFLEMAN || !initial.lastUserCommandKnown ||
		initial.lastUserCommandButtons != 0 ||
		std::fabs(initial.lastUserCommandForwardMove) > 0.01f ||
		std::fabs(initial.lastUserCommandSideMove) > 0.01f ||
		std::fabs(initial.lastUserCommandUpMove) > 0.01f ||
		initial.lastUserCommandImpulse != 0 ||
		initial.lastUserCommandWeaponSelect != 0)
	{
		logControllerProbeMovementState("precondition-rejected", initial);
		logger->Log(LogLevel::ERROR,
			"Empires controller probe movement rejected: bot must be alive NF Rifleman with known origin and neutral last UserCmd");
		return false;
	}

	if (playerinfomanager == nullptr || g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(initial.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe movement rejected: fresh PlayerInfo or controller unavailable");
		return false;
	}

	IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(
		const_cast<edict_t*>(initial.edict));
	if (pInfo == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe movement rejected: fresh PlayerInfo unavailable for view readback");
		return false;
	}

	const CBotCmd lastCommand = pInfo->GetLastUserCommand();
	if (!initial.lastUserCommandKnown ||
		lastCommand.command_number != initial.lastUserCommandNumber ||
		lastCommand.tick_count != initial.lastUserCommandTick)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe movement rejected: last UserCmd changed before arming state_number=%d fresh_number=%d state_tick=%d fresh_tick=%d",
			initial.lastUserCommandNumber, lastCommand.command_number,
			initial.lastUserCommandTick, lastCommand.tick_count);
		return false;
	}

	m_iControllerProbeMovementExpectedIndex = initial.index;
	m_iControllerProbeMovementExpectedUserId = initial.userId;
	m_pControllerProbeMovementExpectedEdict = initial.edict;
	m_iControllerProbeMovementActiveFrames = 0;
	m_iControllerProbeMovementFinalObserveFrames = 0;
	m_iControllerProbeMovementSubmittedCommands = 0;
	m_iControllerProbeMovementLastSubmittedCommand = 0;
	m_iControllerProbeMovementLastSubmittedTick = -1;
	m_bControllerProbeMovementStartOriginKnown = true;
	m_vControllerProbeMovementStartOrigin = initial.origin;
	m_bControllerProbeMovementFinalOriginKnown = false;
	m_vControllerProbeMovementFinalOrigin = Vector();
	// The spawn point supplies the initial view. Preserve it so this proof
	// tests movement input rather than adding a camera/orientation behavior.
	m_vControllerProbeMovementViewAngles = lastCommand.viewangles;
	m_eControllerProbeMovementState =
		EmpiresControllerProbeMovementState::MOVE_START_PENDING;

	logger->Log(LogLevel::INFO,
		"Empires controller probe movement armed exactly_once=1 active_frames=%d forwardmove=%.1f buttons=IN_FORWARD minimum_displacement=%.1f view_control=preserve_spawn_angles start_origin=(%.1f %.1f %.1f) index=%d userid=%d edict=%p",
		kControllerProbeMovementActiveFrames,
		kControllerProbeMovementForwardSpeed,
		kControllerProbeMovementMinimumDisplacement,
		m_vControllerProbeMovementStartOrigin.x,
		m_vControllerProbeMovementStartOrigin.y,
		m_vControllerProbeMovementStartOrigin.z,
		initial.index, initial.userId, static_cast<const void*>(initial.edict));
	logControllerProbeMovementState("spawn-confirmed", initial);
	return true;
}

void CBotEmpires::observeControllerProbeMovementProof()
{
	if (m_eControllerProbeMovementState ==
			EmpiresControllerProbeMovementState::IDLE ||
		m_eControllerProbeMovementState ==
			EmpiresControllerProbeMovementState::COMPLETE ||
		m_eControllerProbeMovementState ==
			EmpiresControllerProbeMovementState::FAILED)
		return;

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state) ||
		!controllerProbeMovementIdentityMatches(state))
	{
		failControllerProbeMovement(
			"fresh client identity unavailable or changed");
		return;
	}

	if (m_iControllerProbeMovementActiveFrames +
			m_iControllerProbeMovementFinalObserveFrames >=
			kControllerProbeMovementSafetyFrames)
	{
		failControllerProbeMovement("movement state exceeded safety frame bound");
		return;
	}

	const bool alive = state.alive && !state.dead && state.healthKnown &&
		state.health > 0 && state.lifeStateKnown &&
		state.lifeState == LIFE_ALIVE;
	if (!alive)
	{
		logControllerProbeMovementState("alive-check-failed", state);
		failControllerProbeMovement("bot stopped being alive during movement proof");
		return;
	}

	const bool neutralLastCommand = state.lastUserCommandKnown &&
		state.lastUserCommandButtons == 0 &&
		std::fabs(state.lastUserCommandForwardMove) <= 0.01f &&
		std::fabs(state.lastUserCommandSideMove) <= 0.01f &&
		std::fabs(state.lastUserCommandUpMove) <= 0.01f &&
		state.lastUserCommandImpulse == 0 &&
		state.lastUserCommandWeaponSelect == 0;

	switch (m_eControllerProbeMovementState)
	{
	case EmpiresControllerProbeMovementState::MOVE_START_PENDING:
		if (!submitControllerProbeMovementUserCmd(IN_FORWARD,
			kControllerProbeMovementForwardSpeed, "movement-start"))
		{
			failControllerProbeMovement("forward movement UserCmd was not accepted");
			return;
		}
		m_iControllerProbeMovementActiveFrames = 1;
		m_eControllerProbeMovementState =
			EmpiresControllerProbeMovementState::MOVE_ACTIVE;
		logControllerProbeMovementState("movement-press-start", state);
		return;

	case EmpiresControllerProbeMovementState::MOVE_ACTIVE:
		if (m_iControllerProbeMovementActiveFrames >=
			kControllerProbeMovementActiveFrames)
		{
			m_eControllerProbeMovementState =
				EmpiresControllerProbeMovementState::MOVE_STOP_PENDING;
			logger->Log(LogLevel::INFO,
				"Empires controller probe movement transition state=MOVE_STOP_PENDING active_frames=%d",
				m_iControllerProbeMovementActiveFrames);
			return;
		}
		if (!submitControllerProbeMovementUserCmd(IN_FORWARD,
			kControllerProbeMovementForwardSpeed, "movement-active"))
		{
			failControllerProbeMovement("active forward movement UserCmd was not accepted");
			return;
		}
		++m_iControllerProbeMovementActiveFrames;
		if (m_iControllerProbeMovementActiveFrames ==
			kControllerProbeMovementActiveFrames)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe movement active complete active_frames=%d; next_action=neutral_release",
				m_iControllerProbeMovementActiveFrames);
		}
		else if (m_iControllerProbeMovementActiveFrames == 2)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe movement active active_frames=%d",
				m_iControllerProbeMovementActiveFrames);
		}
		return;

	case EmpiresControllerProbeMovementState::MOVE_STOP_PENDING:
		if (!submitControllerProbeMovementUserCmd(0, 0.0f,
			"movement-release-stop"))
		{
			failControllerProbeMovement("neutral movement release UserCmd was not accepted");
			return;
		}
		m_eControllerProbeMovementState =
			EmpiresControllerProbeMovementState::FINAL_OBSERVE;
		m_iControllerProbeMovementFinalObserveFrames = 0;
		logger->Log(LogLevel::INFO,
			"Empires controller probe movement release/stop submitted active_frames=%d; next_action=final_origin_observation",
			m_iControllerProbeMovementActiveFrames);
		return;

	case EmpiresControllerProbeMovementState::FINAL_OBSERVE:
	{
		if (!neutralLastCommand || !state.originKnown)
		{
			logControllerProbeMovementState("final-observe-rejected", state);
			failControllerProbeMovement(
				"post-stop state did not expose a neutral command and known origin");
			return;
		}

		++m_iControllerProbeMovementFinalObserveFrames;
		if (m_iControllerProbeMovementFinalObserveFrames == 1)
		{
			m_vControllerProbeMovementFinalOrigin = state.origin;
			m_bControllerProbeMovementFinalOriginKnown = true;
			logControllerProbeMovementState("final-origin", state);
			return;
		}

		const Vector previousFinalOrigin = m_vControllerProbeMovementFinalOrigin;
		const float displacement = std::sqrt(
			(state.origin - m_vControllerProbeMovementStartOrigin).LengthSqr());
		const float postStopDelta = std::sqrt(
			(state.origin - previousFinalOrigin).LengthSqr());
		m_vControllerProbeMovementFinalOrigin = state.origin;
		m_bControllerProbeMovementFinalOriginKnown = true;
		logControllerProbeMovementState("final-origin-stable", state);
		if (postStopDelta > 1.0f)
		{
			failControllerProbeMovement(
				"origin continued changing after neutral release");
			return;
		}
		if (displacement < kControllerProbeMovementMinimumDisplacement)
		{
			failControllerProbeMovement(
				"final origin did not differ meaningfully from start origin");
			return;
		}

		m_eControllerProbeMovementState =
			EmpiresControllerProbeMovementState::COMPLETE;
		logger->Log(LogLevel::INFO,
			"Empires controller probe movement result=PASS start_origin=(%.1f %.1f %.1f) final_origin=(%.1f %.1f %.1f) displacement=%.2f active_frames=%d final_observe_frames=%d submitted=%d movement_stopped=1 alive=1",
			m_vControllerProbeMovementStartOrigin.x,
			m_vControllerProbeMovementStartOrigin.y,
			m_vControllerProbeMovementStartOrigin.z,
			m_vControllerProbeMovementFinalOrigin.x,
			m_vControllerProbeMovementFinalOrigin.y,
			m_vControllerProbeMovementFinalOrigin.z,
			displacement, m_iControllerProbeMovementActiveFrames,
			m_iControllerProbeMovementFinalObserveFrames,
			m_iControllerProbeMovementSubmittedCommands);
		return;
	}

	case EmpiresControllerProbeMovementState::IDLE:
	case EmpiresControllerProbeMovementState::COMPLETE:
	case EmpiresControllerProbeMovementState::FAILED:
		return;
	}
}

bool CBotEmpires::requestControllerProbeWaypointOnce()
{
	const bool noPendingInput =
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE &&
		m_eControllerProbeSpawnHoldState == EmpiresControllerProbeSpawnHoldState::IDLE &&
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::IDLE &&
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::IDLE &&
		m_eControllerProbeAppliedLoadoutReadinessState ==
			EmpiresControllerProbeAppliedLoadoutReadinessState::COMPLETE &&
		m_eControllerProbeSpawnConfirmReadyState ==
			EmpiresControllerProbeSpawnConfirmReadyState::COMPLETE &&
		m_eControllerProbeMovementState == EmpiresControllerProbeMovementState::IDLE &&
		m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::IDLE &&
		!m_bTeamCommandPending && !m_bClassCommandPending &&
		!m_bSpawnCommandPending &&
		m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::IDLE;

	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle !=
			EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr || !noPendingInput)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe waypoint rejected: requires completed spawn confirmation, no pending input and a fresh controller");
		return false;
	}

	const int waypointCount = CWaypoints::numWaypoints();
	if (waypointCount <= 0)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe waypoint result=NO_WAYPOINT_DATA map=%s count=%d",
			CBotGlobals::getMapName(), waypointCount);
		return false;
	}

	EmpiresBootstrapPlayerState initial;
	if (!readBootstrapPlayerState(initial) || !initial.connected || !initial.active ||
		initial.edict == nullptr || initial.edict->IsFree() || initial.index <= 0 ||
		initial.userId <= 0 || !initial.originKnown || !initial.alive || initial.dead ||
		!initial.healthKnown || initial.health <= 0 || !initial.lifeStateKnown ||
		initial.lifeState != LIFE_ALIVE || initial.team != EMP_TEAM_NF ||
		!initial.classKnown || initial.classId != EMP_CLASS_RIFLEMAN ||
		!initial.lastUserCommandKnown || initial.lastUserCommandButtons != 0 ||
		std::fabs(initial.lastUserCommandForwardMove) > 0.01f ||
		std::fabs(initial.lastUserCommandSideMove) > 0.01f ||
		std::fabs(initial.lastUserCommandUpMove) > 0.01f ||
		initial.lastUserCommandImpulse != 0 ||
		initial.lastUserCommandWeaponSelect != 0)
	{
		logControllerProbeWaypointState("precondition-rejected", initial);
		logger->Log(LogLevel::ERROR,
			"Empires controller probe waypoint rejected: bot must be an alive NF Rifleman with a neutral last UserCmd");
		return false;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(initial.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe waypoint rejected: fresh controller unavailable");
		return false;
	}

	int selectedWaypoint = -1;
	float selectedDistance = kControllerProbeWaypointMaximumTargetDistance;
	for (int i = 0; i < waypointCount; ++i)
	{
		CWaypoint* pWaypoint = CWaypoints::getWaypoint(i);
		if (pWaypoint == nullptr || !pWaypoint->isUsed() ||
			pWaypoint->hasFlag(CWaypointTypes::W_FL_UNREACHABLE) ||
			!pWaypoint->forTeam(initial.team))
			continue;

		const Vector waypointOrigin = pWaypoint->getOrigin();
		if (!std::isfinite(waypointOrigin.x) || !std::isfinite(waypointOrigin.y) ||
			!std::isfinite(waypointOrigin.z))
			continue;

		const float distance = (waypointOrigin - initial.origin).Length2D();
		if (distance < kControllerProbeWaypointMinimumTargetDistance ||
			distance > kControllerProbeWaypointMaximumTargetDistance ||
			distance >= selectedDistance)
			continue;

		selectedWaypoint = i;
		selectedDistance = distance;
	}

	if (selectedWaypoint < 0)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe waypoint result=NO_VALID_WAYPOINT map=%s count=%d origin=(%.1f %.1f %.1f) search_range=(%.1f %.1f) team=%d",
			CBotGlobals::getMapName(), waypointCount, initial.origin.x,
			initial.origin.y, initial.origin.z,
			kControllerProbeWaypointMinimumTargetDistance,
			kControllerProbeWaypointMaximumTargetDistance, initial.team);
		return false;
	}

	CWaypoint* pTarget = CWaypoints::getWaypoint(selectedWaypoint);
	if (pTarget == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe waypoint result=WAYPOINT_LOAD_FAILED selected_id=%d",
			selectedWaypoint);
		return false;
	}

	resetTouchDistance(kControllerProbeWaypointArrivalDistance);
	m_iControllerProbeWaypointExpectedIndex = initial.index;
	m_iControllerProbeWaypointExpectedUserId = initial.userId;
	m_pControllerProbeWaypointExpectedEdict = initial.edict;
	m_iControllerProbeWaypointId = selectedWaypoint;
	m_iControllerProbeWaypointActiveFrames = 0;
	m_iControllerProbeWaypointFinalObserveFrames = 0;
	m_iControllerProbeWaypointSubmittedCommands = 0;
	m_iControllerProbeWaypointLastSubmittedCommand = 0;
	m_iControllerProbeWaypointLastSubmittedTick = -1;
	m_iControllerProbeWaypointNoProgressFrames = 0;
	m_bControllerProbeWaypointInputActive = false;
	m_bControllerProbeWaypointStartOriginKnown = true;
	m_vControllerProbeWaypointStartOrigin = initial.origin;
	m_vControllerProbeWaypointViewAngles = QAngle(0.0f, 0.0f, 0.0f);
	m_bControllerProbeWaypointTargetKnown = true;
	m_vControllerProbeWaypointTargetOrigin = pTarget->getOrigin();
	m_bControllerProbeWaypointFinalOriginKnown = false;
	m_vControllerProbeWaypointFinalOrigin = Vector();
	m_flControllerProbeWaypointStartTime = initial.currentTime;
	m_flControllerProbeWaypointInitialDistance =
		std::sqrt((initial.origin - m_vControllerProbeWaypointTargetOrigin).LengthSqr());
	m_flControllerProbeWaypointBestDistance = m_flControllerProbeWaypointInitialDistance;
	m_bControllerProbeWaypointReached = false;
	m_bControllerProbeWaypointTimedOut = false;
	m_bControllerProbeWaypointNoProgress = false;
	m_eControllerProbeWaypointState =
		EmpiresControllerProbeWaypointState::TARGET_SELECTED;

	logger->Log(LogLevel::INFO,
		"Empires controller probe waypoint loaded=1 map=%s count=%d selected_id=%d flags=0x%x used=%d radius=%.1f origin=(%.1f %.1f %.1f) start_distance=%.2f arrival_distance=%.1f team=%d",
		CBotGlobals::getMapName(), waypointCount, selectedWaypoint,
		pTarget->getFlags(), pTarget->isUsed() ? 1 : 0, pTarget->getRadius(),
		m_vControllerProbeWaypointTargetOrigin.x,
		m_vControllerProbeWaypointTargetOrigin.y,
		m_vControllerProbeWaypointTargetOrigin.z,
		m_flControllerProbeWaypointInitialDistance,
		kControllerProbeWaypointArrivalDistance, initial.team);
	logControllerProbeWaypointState("target-selected", initial);
	return true;
}

bool CBotEmpires::requestControllerProbeWaypointRouteOnce()
{
	m_bControllerProbeWaypointRouteMode = true;
	if (!requestControllerProbeWaypointOnce())
	{
		m_bControllerProbeWaypointRouteMode = false;
		return false;
	}

	m_iControllerProbeWaypointRouteTargetsVisited = 1;
	m_iControllerProbeWaypointRoutePreviousId = -1;
	m_iControllerProbeWaypointRouteSegmentFrames = 0;
	logger->Log(LogLevel::INFO,
		"Empires controller probe waypoint route armed max_targets=%d segment_frames=%d initial_target=%d",
		kControllerProbeWaypointRouteMaximumTargets,
		kControllerProbeWaypointRouteSegmentFrames,
		m_iControllerProbeWaypointId);
	return true;
}

bool CBotEmpires::requestControllerProbePathOnce(int startWaypoint,
	int destinationWaypoint)
{
	const int selectedStartWaypoint = startWaypoint >= 0 ? startWaypoint :
		kControllerProbePathStartWaypoint;
	const int selectedDestinationWaypoint = destinationWaypoint >= 0 ?
		destinationWaypoint : kControllerProbePathDestinationWaypoint;
	const bool noPendingInput =
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE &&
		m_eControllerProbeSpawnHoldState == EmpiresControllerProbeSpawnHoldState::IDLE &&
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::IDLE &&
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::IDLE &&
		m_eControllerProbeAppliedLoadoutReadinessState ==
			EmpiresControllerProbeAppliedLoadoutReadinessState::COMPLETE &&
		m_eControllerProbeSpawnConfirmReadyState ==
			EmpiresControllerProbeSpawnConfirmReadyState::COMPLETE &&
		m_eControllerProbeMovementState == EmpiresControllerProbeMovementState::IDLE &&
		m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::IDLE &&
		!m_bTeamCommandPending && !m_bClassCommandPending &&
		!m_bSpawnCommandPending &&
		m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::IDLE;

	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle !=
			EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr || !noPendingInput)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe path rejected: requires completed spawn confirmation, no pending input and a fresh controller");
		return false;
	}

	if (CWaypoints::numWaypoints() <= 0)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe path result=NO_SUITABLE_TEST_ROUTE reason=waypoint data unavailable map=%s",
			CBotGlobals::getMapName());
		return false;
	}

	EmpiresBootstrapPlayerState initial;
	if (!readBootstrapPlayerState(initial) || !initial.connected || !initial.active ||
		initial.edict == nullptr || initial.edict->IsFree() || initial.index <= 0 ||
		initial.userId <= 0 || !initial.originKnown || !initial.alive || initial.dead ||
		!initial.healthKnown || initial.health <= 0 || !initial.lifeStateKnown ||
		initial.lifeState != LIFE_ALIVE || initial.team != EMP_TEAM_NF ||
		!initial.classKnown || initial.classId != EMP_CLASS_RIFLEMAN ||
		!initial.lastUserCommandKnown || initial.lastUserCommandButtons != 0 ||
		std::fabs(initial.lastUserCommandForwardMove) > 0.01f ||
		std::fabs(initial.lastUserCommandSideMove) > 0.01f ||
		std::fabs(initial.lastUserCommandUpMove) > 0.01f ||
		initial.lastUserCommandImpulse != 0 ||
		initial.lastUserCommandWeaponSelect != 0)
	{
		logControllerProbeWaypointState("path-precondition-rejected", initial);
		logger->Log(LogLevel::ERROR,
			"Empires controller probe path rejected: bot must be an alive NF Rifleman with a neutral last UserCmd");
		return false;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(initial.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe path rejected: fresh controller unavailable");
		return false;
	}

	CWaypoint* pStart = CWaypoints::getWaypoint(selectedStartWaypoint);
	CWaypoint* pDestination =
		CWaypoints::getWaypoint(selectedDestinationWaypoint);
	if (pStart == nullptr || pDestination == nullptr || !pStart->isUsed() ||
		!pDestination->isUsed() ||
		pStart->hasFlag(CWaypointTypes::W_FL_UNREACHABLE) ||
		pDestination->hasFlag(CWaypointTypes::W_FL_UNREACHABLE) ||
		!pStart->forTeam(initial.team) || !pDestination->forTeam(initial.team))
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe path result=NO_SUITABLE_TEST_ROUTE reason=fixed waypoint pair unavailable start=%d destination=%d team=%d",
			selectedStartWaypoint, selectedDestinationWaypoint, initial.team);
		return false;
	}

	if (selectedStartWaypoint == selectedDestinationWaypoint)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe path result=NO_SUITABLE_TEST_ROUTE reason=start and destination are identical waypoint=%d",
			selectedStartWaypoint);
		return false;
	}

	if (m_pNavigator != nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe path result=NO_SUITABLE_TEST_ROUTE reason=probe already owns a navigator");
		return false;
	}

	m_pProfile = CBotProfiles::getDefaultProfile();
	if (m_pProfile == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe path result=PATH_SEARCH_FAILED reason=default profile unavailable");
		return false;
	}

	// The normal bot setup is intentionally not entered.  The existing A* route
	// implementation only needs its concrete waypoint navigator and a profile;
	// suppressBotThink() still keeps schedules, combat and autonomous Think out
	// of this one-shot probe.
	m_pNavigator = new CWaypointNavigator(this);
	static_cast<CWaypointNavigator*>(m_pNavigator)->disableBeliefLoadingForProbe();
	m_iControllerProbePathTeam = initial.team;
	m_iControllerProbePathStartWaypoint = selectedStartWaypoint;
	m_iControllerProbePathDestinationWaypoint = selectedDestinationWaypoint;
	m_bControllerProbeWaypointRouteMode = false;
	m_bControllerProbeWaypointPathMode = true;
	m_bControllerProbeWaypointPathSearchStarted = false;
	m_iControllerProbeWaypointPathSearchFrames = 0;
	m_iControllerProbeWaypointPathNodeIndex = 0;
	m_ControllerProbeWaypointPath.clear();
	m_iControllerProbeWaypointExpectedIndex = initial.index;
	m_iControllerProbeWaypointExpectedUserId = initial.userId;
	m_pControllerProbeWaypointExpectedEdict = initial.edict;
	m_iControllerProbeWaypointId = selectedStartWaypoint;
	m_iControllerProbeWaypointRouteTargetsVisited = 0;
	m_iControllerProbeWaypointRoutePreviousId = -1;
	m_iControllerProbeWaypointRouteSegmentFrames = 0;
	m_iControllerProbeWaypointActiveFrames = 0;
	m_iControllerProbeWaypointFinalObserveFrames = 0;
	m_iControllerProbeWaypointSubmittedCommands = 0;
	m_iControllerProbeWaypointLastSubmittedCommand = 0;
	m_iControllerProbeWaypointLastSubmittedTick = -1;
	m_iControllerProbeWaypointNoProgressFrames = 0;
	m_bControllerProbeWaypointInputActive = false;
	m_bControllerProbeWaypointStartOriginKnown = true;
	m_vControllerProbeWaypointStartOrigin = initial.origin;
	m_vControllerProbeWaypointViewAngles = QAngle(0.0f, 0.0f, 0.0f);
	// Read the complete command again so the probe preserves the same validated
	// spawn view without retaining a PlayerInfo pointer.
	if (playerinfomanager != nullptr)
	{
		IPlayerInfo* pInfo = playerinfomanager->GetPlayerInfo(
			const_cast<edict_t*>(initial.edict));
		if (pInfo != nullptr)
			m_vControllerProbeWaypointViewAngles = pInfo->GetLastUserCommand().viewangles;
	}
	m_bControllerProbeWaypointTargetKnown = true;
	m_vControllerProbeWaypointTargetOrigin = pDestination->getOrigin();
	m_bControllerProbeWaypointFinalOriginKnown = false;
	m_vControllerProbeWaypointFinalOrigin = Vector();
	m_flControllerProbeWaypointStartTime = initial.currentTimeKnown ?
		initial.currentTime : (engine != nullptr ? engine->Time() : 0.0f);
	m_flControllerProbeWaypointInitialDistance = std::sqrt(
		(initial.origin - pDestination->getOrigin()).LengthSqr());
	m_flControllerProbeWaypointBestDistance = std::sqrt(
		(initial.origin - pStart->getOrigin()).LengthSqr());
	m_bControllerProbeWaypointReached = false;
	m_bControllerProbeWaypointTimedOut = false;
	m_bControllerProbeWaypointNoProgress = false;
	resetTouchDistance(kControllerProbeWaypointArrivalDistance);
	m_eControllerProbeWaypointState =
		EmpiresControllerProbeWaypointState::TARGET_SELECTED;

	logger->Log(LogLevel::INFO,
		"Empires controller probe path armed exactly_once=1 start_candidate=%d destination=%d minimum_nodes=%d maximum_nodes=%d origin=(%.1f %.1f %.1f) index=%d userid=%d edict=%p",
		selectedStartWaypoint, selectedDestinationWaypoint,
		kControllerProbePathMinimumNodes, kControllerProbePathMaximumNodes,
		initial.origin.x, initial.origin.y, initial.origin.z, initial.index,
		initial.userId, static_cast<const void*>(initial.edict));
	logControllerProbeWaypointState("path-armed", initial);
	return true;
}

void CBotEmpires::failControllerProbePath(const char* result, const char* reason)
{
	if (m_bControllerProbeWaypointInputActive)
	{
		if (!submitControllerProbeMovementUserCmd(0, 0.0f,
			"path-failure-release", &m_vControllerProbeWaypointViewAngles))
		{
			logger->Log(LogLevel::ERROR,
				"Empires controller probe path failed to submit neutral failure release");
		}
		m_bControllerProbeWaypointInputActive = false;
	}

	m_eControllerProbeWaypointState =
		EmpiresControllerProbeWaypointState::FAILED;
	logger->Log(LogLevel::ERROR,
		"Empires controller probe path result=%s reason=%s route_nodes=%d traversed_nodes=%d active_frames=%d submitted=%d",
		result != nullptr ? result : "PATH_SEARCH_FAILED",
		reason != nullptr ? reason : "unspecified",
		static_cast<int>(m_ControllerProbeWaypointPath.size()),
		m_iControllerProbeWaypointRouteTargetsVisited,
		m_iControllerProbeWaypointActiveFrames,
		m_iControllerProbeWaypointSubmittedCommands);
}

void CBotEmpires::observeControllerProbePathOnce()
{
	if (m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::IDLE ||
		m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::COMPLETE ||
		m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::FAILED)
		return;

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state) ||
		!controllerProbeWaypointIdentityMatches(state))
	{
		failControllerProbePath("PATH_FOUND_STEERING_BLOCKED",
			"client identity unavailable or changed");
		return;
	}

	if (g_pBotManager == nullptr)
	{
		failControllerProbePath("PATH_FOUND_STEERING_BLOCKED", "controller lost");
		return;
	}

	IBotController* pController =
		g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict));
	if (pController == nullptr)
	{
		failControllerProbePath("PATH_FOUND_STEERING_BLOCKED", "controller lost");
		return;
	}

	// The controller probe normally keeps the base actuator pointer empty and
	// obtains a fresh controller only for direct probe commands. The existing
	// CWaypointNavigator API uses CBot::getOrigin()/distanceFrom(), which read
	// that base actuator synchronously during route search. Install the already
	// verified controller for this callback; it is refreshed on every callback
	// after identity validation and is cleared again by the normal bot reset.
	m_pController = pController;

	const bool alive = state.alive && !state.dead && state.healthKnown &&
		state.health > 0 && state.lifeStateKnown && state.lifeState == LIFE_ALIVE;
	if (!alive)
	{
		logControllerProbeWaypointState("path-alive-check-failed", state);
		failControllerProbePath("PATH_FOUND_STEERING_BLOCKED",
			"bot stopped being alive during path proof");
		return;
	}

	CWaypointNavigator* pNavigator =
		static_cast<CWaypointNavigator*>(m_pNavigator);
	if (pNavigator == nullptr)
	{
		failControllerProbePath("PATH_SEARCH_FAILED",
			"waypoint navigator unavailable");
		return;
	}

	if (m_eControllerProbeWaypointState ==
		EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING)
	{
		if (!submitControllerProbeMovementUserCmd(0, 0.0f,
			"path-movement-release", &m_vControllerProbeWaypointViewAngles))
		{
			failControllerProbePath("PATH_FOUND_STEERING_BLOCKED",
				"neutral path release UserCmd was not accepted");
			return;
		}
		m_bControllerProbeWaypointInputActive = false;
		m_eControllerProbeWaypointState =
			EmpiresControllerProbeWaypointState::FINAL_OBSERVE;
		m_iControllerProbeWaypointFinalObserveFrames = 0;
		logger->Log(LogLevel::INFO,
			"Empires controller probe path movement release/stop submitted route_nodes=%d traversed_nodes=%d; next_action=final_origin_observation",
			static_cast<int>(m_ControllerProbeWaypointPath.size()),
			m_iControllerProbeWaypointRouteTargetsVisited);
		return;
	}

	if (m_eControllerProbeWaypointState ==
		EmpiresControllerProbeWaypointState::FINAL_OBSERVE)
	{
		const bool neutralLastCommand = state.lastUserCommandKnown &&
			state.lastUserCommandButtons == 0 &&
			std::fabs(state.lastUserCommandForwardMove) <= 0.01f &&
			std::fabs(state.lastUserCommandSideMove) <= 0.01f &&
			std::fabs(state.lastUserCommandUpMove) <= 0.01f &&
			state.lastUserCommandImpulse == 0 &&
			state.lastUserCommandWeaponSelect == 0;
		if (!neutralLastCommand || !state.originKnown)
		{
			logControllerProbeWaypointState("path-final-observe-rejected", state);
			failControllerProbePath("PATH_FOUND_STEERING_BLOCKED",
				"post-stop path state did not expose a neutral command and known origin");
			return;
		}

		++m_iControllerProbeWaypointFinalObserveFrames;
		if (m_iControllerProbeWaypointFinalObserveFrames == 1)
		{
			m_vControllerProbeWaypointFinalOrigin = state.origin;
			m_bControllerProbeWaypointFinalOriginKnown = true;
			logControllerProbeWaypointState("path-final-origin", state);
			return;
		}

		const Vector previousFinalOrigin = m_vControllerProbeWaypointFinalOrigin;
		const float displacement = std::sqrt(
			(state.origin - m_vControllerProbeWaypointStartOrigin).LengthSqr());
		const float postStopDelta = std::sqrt(
			(state.origin - previousFinalOrigin).LengthSqr());
		m_vControllerProbeWaypointFinalOrigin = state.origin;
		m_bControllerProbeWaypointFinalOriginKnown = true;
		logControllerProbeWaypointState("path-final-origin-stable", state);
		if (postStopDelta > 1.0f)
		{
			failControllerProbePath("PATH_FOUND_STEERING_BLOCKED",
				"origin continued changing after neutral path release");
			return;
		}

		m_eControllerProbeWaypointState =
			EmpiresControllerProbeWaypointState::COMPLETE;
		logger->Log(LogLevel::INFO,
			"Empires controller probe path result=PATH_FOUND_AND_TRAVERSED route_nodes=%d traversed_nodes=%d destination=%d start_origin=(%.1f %.1f %.1f) final_origin=(%.1f %.1f %.1f) displacement=%.2f active_frames=%d final_observe_frames=%d submitted=%d movement_stopped=1 alive=1",
			static_cast<int>(m_ControllerProbeWaypointPath.size()),
			m_iControllerProbeWaypointRouteTargetsVisited,
			m_iControllerProbePathDestinationWaypoint,
			m_vControllerProbeWaypointStartOrigin.x,
			m_vControllerProbeWaypointStartOrigin.y,
			m_vControllerProbeWaypointStartOrigin.z,
			m_vControllerProbeWaypointFinalOrigin.x,
			m_vControllerProbeWaypointFinalOrigin.y,
			m_vControllerProbeWaypointFinalOrigin.z,
			displacement, m_iControllerProbeWaypointActiveFrames,
			m_iControllerProbeWaypointFinalObserveFrames,
			m_iControllerProbeWaypointSubmittedCommands);
		return;
	}

	if (!m_bControllerProbeWaypointPathSearchStarted)
	{
		CWaypoint* pDestination = CWaypoints::getWaypoint(
			m_iControllerProbePathDestinationWaypoint);
		if (pDestination == nullptr || !pDestination->isUsed())
		{
			failControllerProbePath("NO_SUITABLE_TEST_ROUTE",
				"fixed destination disappeared before search");
			return;
		}

		bool searchFailed = false;
		const bool searchFinished = pNavigator->workRoute(
			state.origin, pDestination->getOrigin(), &searchFailed,
			m_iControllerProbeWaypointPathSearchFrames == 0, true,
			m_iControllerProbePathDestinationWaypoint, 0, -1);
		m_bControllerProbeWaypointPathSearchStarted = true;
		++m_iControllerProbeWaypointPathSearchFrames;

		if (!searchFinished)
		{
			if (m_iControllerProbeWaypointPathSearchFrames >=
				kControllerProbePathSearchMaximumFrames)
			{
				failControllerProbePath("PATH_SEARCH_FAILED",
					"A* search exceeded its bounded search-frame limit");
			}
			return;
		}

		if (searchFailed || !pNavigator->routeFound() ||
			!pNavigator->copyRoute(&m_ControllerProbeWaypointPath))
		{
			failControllerProbePath("PATH_SEARCH_FAILED",
				"existing CWaypointNavigator returned no route");
			return;
		}

		logger->Log(LogLevel::INFO,
			"Empires controller probe path route-found=1 start=%d destination=%d route_nodes=%d remaining_nodes=%d",
			m_ControllerProbeWaypointPath.front(),
			m_ControllerProbeWaypointPath.back(),
			static_cast<int>(m_ControllerProbeWaypointPath.size()),
			pNavigator->getRouteSize());
		for (int node = 0; node < static_cast<int>(m_ControllerProbeWaypointPath.size()); ++node)
		{
			logger->Log(LogLevel::INFO,
				"Empires controller probe path route-node index=%d waypoint=%d",
				node, m_ControllerProbeWaypointPath[node]);
		}

		if (static_cast<int>(m_ControllerProbeWaypointPath.size()) <
			kControllerProbePathMinimumNodes ||
			static_cast<int>(m_ControllerProbeWaypointPath.size()) >
			kControllerProbePathMaximumNodes ||
			m_ControllerProbeWaypointPath.front() != m_iControllerProbePathStartWaypoint ||
			m_ControllerProbeWaypointPath.back() != m_iControllerProbePathDestinationWaypoint)
		{
			failControllerProbePath("NO_SUITABLE_TEST_ROUTE",
				"calculated route is outside the fixed 5-10 node test bounds or has a different endpoint");
			return;
		}

		for (int node = 0; node < static_cast<int>(m_ControllerProbeWaypointPath.size()); ++node)
		{
			CWaypoint* pWaypoint = CWaypoints::getWaypoint(
				m_ControllerProbeWaypointPath[node]);
			if (pWaypoint == nullptr || !pWaypoint->isUsed() ||
				!pWaypoint->forTeam(state.team) ||
				pWaypoint->hasFlag(CWaypointTypes::W_FL_UNREACHABLE) ||
				pWaypoint->hasFlag(CWaypointTypes::W_FL_JUMP) ||
				pWaypoint->hasFlag(CWaypointTypes::W_FL_CROUCH) ||
				pWaypoint->hasFlag(CWaypointTypes::W_FL_LADDER) ||
				pWaypoint->hasFlag(CWaypointTypes::W_FL_TELEPORT_CHEAT) ||
				pWaypoint->hasFlag(CWaypointTypes::W_FL_USE))
			{
				failControllerProbePath("NO_SUITABLE_TEST_ROUTE",
					"route contains an invalid, team-restricted or input-requiring waypoint");
				return;
			}
		}

		m_iControllerProbeWaypointPathNodeIndex = 0;
		m_iControllerProbeWaypointId = m_ControllerProbeWaypointPath.front();
		m_iControllerProbeWaypointRouteTargetsVisited = 1;
		m_iControllerProbeWaypointRoutePreviousId = -1;
		m_vControllerProbeWaypointTargetOrigin =
			CWaypoints::getWaypoint(m_iControllerProbeWaypointId)->getOrigin();
		m_flControllerProbeWaypointBestDistance = std::sqrt(
			(state.origin - m_vControllerProbeWaypointTargetOrigin).LengthSqr());
		m_iControllerProbeWaypointNoProgressFrames = 0;
		logger->Log(LogLevel::INFO,
			"Empires controller probe path route-validated=1 route_nodes=%d first_target=%d next_action=waypoint-steering",
			static_cast<int>(m_ControllerProbeWaypointPath.size()),
			m_iControllerProbeWaypointId);
		return;
	}

	if (m_ControllerProbeWaypointPath.empty() ||
		m_iControllerProbeWaypointPathNodeIndex < 0 ||
		m_iControllerProbeWaypointPathNodeIndex >=
			static_cast<int>(m_ControllerProbeWaypointPath.size()))
	{
		failControllerProbePath("PATH_SEARCH_FAILED",
			"fixed route index became invalid");
		return;
	}

	const int expectedWaypoint =
		m_ControllerProbeWaypointPath[m_iControllerProbeWaypointPathNodeIndex];
	const int currentWaypoint = pNavigator->getCurrentWaypointID();
	if (currentWaypoint != expectedWaypoint)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe path navigator mismatch expected=%d actual=%d route_index=%d",
			expectedWaypoint, currentWaypoint,
			m_iControllerProbeWaypointPathNodeIndex);
		failControllerProbePath("PATH_SEARCH_FAILED",
			"navigator current waypoint diverged from the fixed route");
		return;
	}

	CWaypoint* pTarget = CWaypoints::getWaypoint(currentWaypoint);
	if (pTarget == nullptr || !pTarget->isUsed())
	{
		failControllerProbePath("PATH_SEARCH_FAILED",
			"current route waypoint became invalid");
		return;
	}

	float currentDistance = state.originKnown ? std::sqrt(
		(state.origin - pTarget->getOrigin()).LengthSqr()) : -1.0f;
	bool arrived = state.originKnown && pTarget->touched(
		state.origin, Vector(0.0f, 0.0f, 0.0f),
		kControllerProbeWaypointArrivalDistance, true);

	if (arrived)
	{
		if (m_iControllerProbeWaypointPathNodeIndex + 1 >=
			static_cast<int>(m_ControllerProbeWaypointPath.size()))
		{
			m_bControllerProbeWaypointReached = true;
			m_iControllerProbeWaypointRouteTargetsVisited =
				static_cast<int>(m_ControllerProbeWaypointPath.size());
			m_eControllerProbeWaypointState =
				EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING;
			logger->Log(LogLevel::INFO,
				"Empires controller probe path destination-reached waypoint=%d route_index=%d route_nodes=%d",
				currentWaypoint, m_iControllerProbeWaypointPathNodeIndex,
				static_cast<int>(m_ControllerProbeWaypointPath.size()));
			return;
		}

		const int previousWaypoint = currentWaypoint;
		pNavigator->updatePosition();
		const int nextWaypoint = pNavigator->getCurrentWaypointID();
		const int expectedNextWaypoint =
			m_ControllerProbeWaypointPath[m_iControllerProbeWaypointPathNodeIndex + 1];
		if (nextWaypoint != expectedNextWaypoint)
		{
			logger->Log(LogLevel::ERROR,
				"Empires controller probe path route advance mismatch from=%d expected_next=%d actual_next=%d",
				previousWaypoint, expectedNextWaypoint, nextWaypoint);
			failControllerProbePath("PATH_SEARCH_FAILED",
				"navigator did not advance to the next fixed route node");
			return;
		}

		++m_iControllerProbeWaypointPathNodeIndex;
		m_iControllerProbeWaypointId = nextWaypoint;
		m_iControllerProbeWaypointRoutePreviousId = previousWaypoint;
		m_iControllerProbeWaypointRouteTargetsVisited =
			m_iControllerProbeWaypointPathNodeIndex + 1;
		m_vControllerProbeWaypointTargetOrigin =
			CWaypoints::getWaypoint(nextWaypoint)->getOrigin();
		m_flControllerProbeWaypointBestDistance = std::sqrt(
			(state.origin - m_vControllerProbeWaypointTargetOrigin).LengthSqr());
		m_iControllerProbeWaypointNoProgressFrames = 0;
		m_eControllerProbeWaypointState =
			EmpiresControllerProbeWaypointState::TARGET_SELECTED;
		logger->Log(LogLevel::INFO,
			"Empires controller probe path advance from=%d to=%d route_index=%d/%d traversed_nodes=%d",
			previousWaypoint, nextWaypoint,
			m_iControllerProbeWaypointPathNodeIndex,
			static_cast<int>(m_ControllerProbeWaypointPath.size()) - 1,
			m_iControllerProbeWaypointRouteTargetsVisited);
		currentDistance = std::sqrt(
			(state.origin - m_vControllerProbeWaypointTargetOrigin).LengthSqr());
		arrived = false;
	}

	if (currentDistance >= 0.0f && currentDistance +
		kControllerProbeWaypointNoProgressEpsilon <
		m_flControllerProbeWaypointBestDistance)
	{
		m_flControllerProbeWaypointBestDistance = currentDistance;
		m_iControllerProbeWaypointNoProgressFrames = 0;
	}
	else if (m_eControllerProbeWaypointState ==
		EmpiresControllerProbeWaypointState::MOVE_ACTIVE)
	{
		++m_iControllerProbeWaypointNoProgressFrames;
	}

	const bool timedOut =
		(state.currentTimeKnown && state.currentTime >=
			m_flControllerProbeWaypointStartTime + kControllerProbePathMaximumSeconds) ||
		m_iControllerProbeWaypointActiveFrames >= kControllerProbePathMaximumFrames;
	if (timedOut)
	{
		m_bControllerProbeWaypointTimedOut = true;
		m_eControllerProbeWaypointState =
			EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING;
		logger->Log(LogLevel::WARN,
			"Empires controller probe path transition state=MOVE_STOP_PENDING reason=TIMEOUT current_waypoint=%d route_index=%d active_frames=%d curtime=%.3f deadline=%.3f",
			m_iControllerProbeWaypointId,
			m_iControllerProbeWaypointPathNodeIndex,
			m_iControllerProbeWaypointActiveFrames, state.currentTime,
			m_flControllerProbeWaypointStartTime + kControllerProbePathMaximumSeconds);
		return;
	}

	if (m_iControllerProbeWaypointNoProgressFrames >=
		kControllerProbeWaypointNoProgressFrames)
	{
		m_bControllerProbeWaypointNoProgress = true;
		m_eControllerProbeWaypointState =
			EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING;
		logger->Log(LogLevel::WARN,
			"Empires controller probe path transition state=MOVE_STOP_PENDING reason=NO_PROGRESS current_waypoint=%d route_index=%d active_frames=%d best_distance=%.2f current_distance=%.2f",
			m_iControllerProbeWaypointId,
			m_iControllerProbeWaypointPathNodeIndex,
			m_iControllerProbeWaypointActiveFrames,
			m_flControllerProbeWaypointBestDistance, currentDistance);
		return;
	}

	const Vector delta = m_vControllerProbeWaypointTargetOrigin - state.origin;
	const float yaw = std::atan2(delta.y, delta.x) *
		(180.0f / 3.14159265358979323846f);
	m_vControllerProbeWaypointViewAngles = QAngle(0.0f, yaw, 0.0f);
	const char* phase = m_eControllerProbeWaypointState ==
		EmpiresControllerProbeWaypointState::TARGET_SELECTED ?
		"path-movement-start" : "path-movement-active";
	if (!submitControllerProbeMovementUserCmd(IN_FORWARD,
		kControllerProbeMovementForwardSpeed, phase,
		&m_vControllerProbeWaypointViewAngles))
	{
		failControllerProbePath("PATH_FOUND_STEERING_BLOCKED",
			"forward waypoint UserCmd was not accepted");
		return;
	}

	++m_iControllerProbeWaypointActiveFrames;
	m_eControllerProbeWaypointState =
		EmpiresControllerProbeWaypointState::MOVE_ACTIVE;
}

void CBotEmpires::observeControllerProbeWaypointOnce()
{
	if (m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::IDLE ||
		m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::COMPLETE ||
		m_eControllerProbeWaypointState == EmpiresControllerProbeWaypointState::FAILED)
		return;

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state) ||
		!controllerProbeWaypointIdentityMatches(state))
	{
		failControllerProbeWaypoint("client identity unavailable or changed");
		return;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict)) == nullptr)
	{
		failControllerProbeWaypoint("controller lost");
		return;
	}

	const bool alive = state.alive && !state.dead && state.healthKnown &&
		state.health > 0 && state.lifeStateKnown && state.lifeState == LIFE_ALIVE;
	if (!alive)
	{
		logControllerProbeWaypointState("alive-check-failed", state);
		failControllerProbeWaypoint("bot stopped being alive during waypoint proof");
		return;
	}

	CWaypoint* pTarget = CWaypoints::getWaypoint(m_iControllerProbeWaypointId);
	if (pTarget == nullptr || !pTarget->isUsed())
	{
		failControllerProbeWaypoint("selected waypoint became invalid");
		return;
	}

	const float currentDistance = state.originKnown ?
		std::sqrt((state.origin - m_vControllerProbeWaypointTargetOrigin).LengthSqr()) :
		-1.0f;
	const bool arrived = state.originKnown &&
		pTarget->touched(state.origin, Vector(0.0f, 0.0f, 0.0f),
			kControllerProbeWaypointArrivalDistance, true);
	const bool timedOut = state.currentTimeKnown &&
		state.currentTime >= m_flControllerProbeWaypointStartTime +
			kControllerProbeWaypointMaximumSeconds;

	if (currentDistance >= 0.0f &&
		currentDistance + kControllerProbeWaypointNoProgressEpsilon <
			m_flControllerProbeWaypointBestDistance)
	{
		m_flControllerProbeWaypointBestDistance = currentDistance;
		m_iControllerProbeWaypointNoProgressFrames = 0;
	}
	else if (m_eControllerProbeWaypointState ==
		EmpiresControllerProbeWaypointState::MOVE_ACTIVE)
	{
		++m_iControllerProbeWaypointNoProgressFrames;
	}

	switch (m_eControllerProbeWaypointState)
	{
	case EmpiresControllerProbeWaypointState::TARGET_SELECTED:
	{
		if (arrived)
		{
			m_bControllerProbeWaypointReached = true;
			m_eControllerProbeWaypointState =
				EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING;
			logControllerProbeWaypointState("already-within-arrival-radius", state);
			return;
		}

		const Vector delta = m_vControllerProbeWaypointTargetOrigin - state.origin;
		const float yaw = std::atan2(delta.y, delta.x) *
			(180.0f / 3.14159265358979323846f);
		m_vControllerProbeWaypointViewAngles = QAngle(0.0f, yaw, 0.0f);
		if (!submitControllerProbeMovementUserCmd(IN_FORWARD,
			kControllerProbeMovementForwardSpeed, "waypoint-movement-start",
			&m_vControllerProbeWaypointViewAngles))
		{
			failControllerProbeWaypoint("forward waypoint UserCmd was not accepted");
			return;
		}
		m_iControllerProbeWaypointActiveFrames = 1;
		m_eControllerProbeWaypointState =
			EmpiresControllerProbeWaypointState::MOVE_ACTIVE;
		logControllerProbeWaypointState("movement-press-start", state);
		return;
	}

	case EmpiresControllerProbeWaypointState::MOVE_ACTIVE:
	{
		if (arrived)
		{
			m_bControllerProbeWaypointReached = true;
			m_eControllerProbeWaypointState =
				EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING;
			logControllerProbeWaypointState("waypoint-arrival", state);
			return;
		}

		if (m_bControllerProbeWaypointRouteMode &&
			m_iControllerProbeWaypointActiveFrames >=
				kControllerProbeWaypointRouteSegmentFrames)
		{
			m_eControllerProbeWaypointState =
				EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING;
			logger->Log(LogLevel::INFO,
				"Empires controller probe waypoint route segment stop target=%d segment_frames=%d targets_visited=%d",
				m_iControllerProbeWaypointId,
				m_iControllerProbeWaypointActiveFrames,
				m_iControllerProbeWaypointRouteTargetsVisited);
			return;
		}

		if (timedOut)
		{
			m_bControllerProbeWaypointTimedOut = true;
			m_eControllerProbeWaypointState =
				EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING;
			logger->Log(LogLevel::WARN,
				"Empires controller probe waypoint transition state=MOVE_STOP_PENDING reason=TIMEOUT active_frames=%d curtime=%.3f deadline=%.3f",
				m_iControllerProbeWaypointActiveFrames, state.currentTime,
				m_flControllerProbeWaypointStartTime +
					kControllerProbeWaypointMaximumSeconds);
			return;
		}

		if (m_iControllerProbeWaypointNoProgressFrames >=
			kControllerProbeWaypointNoProgressFrames)
		{
			m_bControllerProbeWaypointNoProgress = true;
			m_eControllerProbeWaypointState =
				EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING;
			logger->Log(LogLevel::WARN,
				"Empires controller probe waypoint transition state=MOVE_STOP_PENDING reason=NO_PROGRESS active_frames=%d best_distance=%.2f current_distance=%.2f",
				m_iControllerProbeWaypointActiveFrames,
				m_flControllerProbeWaypointBestDistance, currentDistance);
			return;
		}

		const Vector delta = m_vControllerProbeWaypointTargetOrigin - state.origin;
		const float yaw = std::atan2(delta.y, delta.x) *
			(180.0f / 3.14159265358979323846f);
		m_vControllerProbeWaypointViewAngles = QAngle(0.0f, yaw, 0.0f);
		if (!submitControllerProbeMovementUserCmd(IN_FORWARD,
			kControllerProbeMovementForwardSpeed, "waypoint-movement-active",
			&m_vControllerProbeWaypointViewAngles))
		{
			failControllerProbeWaypoint("active waypoint UserCmd was not accepted");
			return;
		}
		++m_iControllerProbeWaypointActiveFrames;
		return;
	}

	case EmpiresControllerProbeWaypointState::MOVE_STOP_PENDING:
	{
		if (!submitControllerProbeMovementUserCmd(0, 0.0f,
			"waypoint-movement-release", &m_vControllerProbeWaypointViewAngles))
		{
			failControllerProbeWaypoint("neutral waypoint release UserCmd was not accepted");
			return;
		}
		m_bControllerProbeWaypointInputActive = false;

		if (m_bControllerProbeWaypointRouteMode &&
			m_iControllerProbeWaypointRouteTargetsVisited <
				kControllerProbeWaypointRouteMaximumTargets)
		{
			CWaypoint* pCurrent = CWaypoints::getWaypoint(
				m_iControllerProbeWaypointId);
			int nextWaypoint = -1;
			if (pCurrent != nullptr)
			{
				for (int path = 0; path < pCurrent->numPaths(); ++path)
				{
					const int candidate = pCurrent->getPath(path);
					if (candidate == m_iControllerProbeWaypointRoutePreviousId ||
						candidate == m_iControllerProbeWaypointId ||
						!CWaypoints::validWaypointIndex(candidate))
						continue;

					CWaypoint* pCandidate = CWaypoints::getWaypoint(candidate);
					if (pCandidate == nullptr || !pCandidate->isUsed() ||
						pCandidate->hasFlag(CWaypointTypes::W_FL_UNREACHABLE) ||
						!pCandidate->forTeam(state.team))
						continue;

					const Vector candidateOrigin = pCandidate->getOrigin();
					if (!std::isfinite(candidateOrigin.x) ||
						!std::isfinite(candidateOrigin.y) ||
						!std::isfinite(candidateOrigin.z))
						continue;

					nextWaypoint = candidate;
					break;
				}
			}

			if (nextWaypoint >= 0)
			{
				m_iControllerProbeWaypointRoutePreviousId =
					m_iControllerProbeWaypointId;
				m_iControllerProbeWaypointId = nextWaypoint;
				m_vControllerProbeWaypointTargetOrigin =
					CWaypoints::getWaypoint(nextWaypoint)->getOrigin();
				m_iControllerProbeWaypointRouteSegmentFrames = 0;
				m_iControllerProbeWaypointActiveFrames = 0;
				m_iControllerProbeWaypointNoProgressFrames = 0;
				m_flControllerProbeWaypointBestDistance =
					std::sqrt((state.origin -
						m_vControllerProbeWaypointTargetOrigin).LengthSqr());
				++m_iControllerProbeWaypointRouteTargetsVisited;
				m_eControllerProbeWaypointState =
					EmpiresControllerProbeWaypointState::TARGET_SELECTED;
				logger->Log(LogLevel::INFO,
					"Empires controller probe waypoint route advance from=%d to=%d targets_visited=%d",
					m_iControllerProbeWaypointRoutePreviousId,
					m_iControllerProbeWaypointId,
					m_iControllerProbeWaypointRouteTargetsVisited);
				return;
			}

			logger->Log(LogLevel::WARN,
				"Empires controller probe waypoint route stopped: no valid forward path from target=%d targets_visited=%d",
				m_iControllerProbeWaypointId,
				m_iControllerProbeWaypointRouteTargetsVisited);
		}

		m_eControllerProbeWaypointState =
			EmpiresControllerProbeWaypointState::FINAL_OBSERVE;
		m_iControllerProbeWaypointFinalObserveFrames = 0;
		logger->Log(LogLevel::INFO,
			"Empires controller probe waypoint movement release/stop submitted active_frames=%d reason=%s",
			m_iControllerProbeWaypointActiveFrames,
			m_bControllerProbeWaypointReached ? "WAYPOINT_REACHED" :
			m_bControllerProbeWaypointNoProgress ? "NO_PROGRESS" :
			m_bControllerProbeWaypointTimedOut ? "TIMEOUT" : "STOP_REQUESTED");
		return;
	}

	case EmpiresControllerProbeWaypointState::FINAL_OBSERVE:
	{
		const bool neutralLastCommand = state.lastUserCommandKnown &&
			state.lastUserCommandButtons == 0 &&
			std::fabs(state.lastUserCommandForwardMove) <= 0.01f &&
			std::fabs(state.lastUserCommandSideMove) <= 0.01f &&
			std::fabs(state.lastUserCommandUpMove) <= 0.01f &&
			state.lastUserCommandImpulse == 0 &&
			state.lastUserCommandWeaponSelect == 0;
		if (!neutralLastCommand || !state.originKnown)
		{
			logControllerProbeWaypointState("final-observe-rejected", state);
			failControllerProbeWaypoint(
				"post-stop state did not expose a neutral command and known origin");
			return;
		}

		++m_iControllerProbeWaypointFinalObserveFrames;
		if (m_iControllerProbeWaypointFinalObserveFrames == 1)
		{
			m_vControllerProbeWaypointFinalOrigin = state.origin;
			m_bControllerProbeWaypointFinalOriginKnown = true;
			logControllerProbeWaypointState("final-origin", state);
			return;
		}

		const Vector previousFinalOrigin = m_vControllerProbeWaypointFinalOrigin;
		const float displacement = std::sqrt(
			(state.origin - m_vControllerProbeWaypointStartOrigin).LengthSqr());
		const float postStopDelta = std::sqrt(
			(state.origin - previousFinalOrigin).LengthSqr());
		m_vControllerProbeWaypointFinalOrigin = state.origin;
		m_bControllerProbeWaypointFinalOriginKnown = true;
		logControllerProbeWaypointState("final-origin-stable", state);
		if (postStopDelta > 1.0f)
		{
			failControllerProbeWaypoint(
				"origin continued changing after neutral release");
			return;
		}

		const float distanceReduction =
			m_flControllerProbeWaypointInitialDistance -
			m_flControllerProbeWaypointBestDistance;
		const char* result = "TIMEOUT";
		if (m_bControllerProbeWaypointRouteMode)
			result = m_iControllerProbeWaypointRouteTargetsVisited >=
				kControllerProbeWaypointRouteMaximumTargets ?
				"ROUTE_SEQUENCE_COMPLETE" : "ROUTE_SEQUENCE_PARTIAL";
		else if (m_bControllerProbeWaypointReached)
			result = "WAYPOINT_REACHED";
		else if (displacement < kControllerProbeMovementMinimumDisplacement)
			result = "NO_MOVEMENT";
		else if (distanceReduction >= kControllerProbeMovementMinimumDisplacement)
			result = "WAYPOINT_STEERING_WORKS_PATH_REQUIRED";
		else if (m_bControllerProbeWaypointNoProgress)
			result = "NO_PROGRESS";

		m_eControllerProbeWaypointState =
			EmpiresControllerProbeWaypointState::COMPLETE;
		logger->Log(LogLevel::INFO,
			"Empires controller probe waypoint result=%s route_mode=%d targets_visited=%d start_origin=(%.1f %.1f %.1f) final_origin=(%.1f %.1f %.1f) target_id=%d target=(%.1f %.1f %.1f) final_distance=%.2f displacement=%.2f distance_reduction=%.2f active_frames=%d final_observe_frames=%d submitted=%d movement_stopped=1 alive=1",
			result, m_bControllerProbeWaypointRouteMode ? 1 : 0,
			m_iControllerProbeWaypointRouteTargetsVisited,
			m_vControllerProbeWaypointStartOrigin.x,
			m_vControllerProbeWaypointStartOrigin.y,
			m_vControllerProbeWaypointStartOrigin.z,
			m_vControllerProbeWaypointFinalOrigin.x,
			m_vControllerProbeWaypointFinalOrigin.y,
			m_vControllerProbeWaypointFinalOrigin.z, m_iControllerProbeWaypointId,
			m_vControllerProbeWaypointTargetOrigin.x,
			m_vControllerProbeWaypointTargetOrigin.y,
			m_vControllerProbeWaypointTargetOrigin.z,
			std::sqrt((state.origin - m_vControllerProbeWaypointTargetOrigin).LengthSqr()),
			displacement, distanceReduction,
			m_iControllerProbeWaypointActiveFrames,
			m_iControllerProbeWaypointFinalObserveFrames,
			m_iControllerProbeWaypointSubmittedCommands);
		return;
	}

	case EmpiresControllerProbeWaypointState::IDLE:
	case EmpiresControllerProbeWaypointState::COMPLETE:
	case EmpiresControllerProbeWaypointState::FAILED:
		return;
	}
}

bool CBotEmpires::requestControllerProbeSpawnHoldTestInternal(const bool readinessOnly)
{
	const bool noPendingInput =
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE &&
		m_eControllerProbeSpawnHoldState == EmpiresControllerProbeSpawnHoldState::IDLE &&
		!m_bTeamCommandPending && !m_bClassCommandPending && !m_bSpawnCommandPending &&
		m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::IDLE;

	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle != EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr || !noPendingInput)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe spawnhold rejected: probe unavailable or another input operation is pending old_state=%s hold_state=%s lifecycle=%s",
			controllerProbeSpawnSequenceStateName(), controllerProbeSpawnHoldStateName(),
			controllerProbeLifecycleName());
		return false;
	}

	EmpiresBootstrapPlayerState initial;
	if (!readBootstrapPlayerState(initial) || !initial.connected || !initial.active ||
		initial.index <= 0 || initial.userId <= 0 || initial.edict == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnhold rejected: initial state is not a valid active slot-bound client");
		return false;
	}

	IBotController* pController = g_pBotManager != nullptr ?
		g_pBotManager->GetBotController(const_cast<edict_t*>(initial.edict)) : nullptr;
	if (pController == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnhold rejected: fresh GetBotController returned null index=%d userid=%d edict=%p",
			initial.index, initial.userId, static_cast<const void*>(initial.edict));
		return false;
	}

	m_iControllerProbeHoldExpectedIndex = initial.index;
	m_iControllerProbeHoldExpectedUserId = initial.userId;
	m_pControllerProbeHoldExpectedEdict = initial.edict;
	m_iControllerProbeHoldStageFrames = 0;
	m_iControllerProbeHoldObserveFrames = 0;
	m_iControllerProbeHoldAttackCount = 0;
	m_iControllerProbeHoldReleaseCount = 0;
	m_bControllerProbeHoldTimerKnownEverySample = true;
	m_bControllerProbeHoldTimerFutureObserved = false;
	m_bControllerProbeHoldTimerExpired = false;
	m_bControllerProbeReadinessOnly = readinessOnly;
	m_bControllerProbeReadinessStartTimeKnown = initial.currentTimeKnown;
	m_flControllerProbeReadinessStartTime = initial.currentTime;
	m_bControllerProbeReadinessLastLifeStateKnown = false;
	m_iControllerProbeReadinessLastLifeState = -1;
	m_bControllerProbeReadinessLastTimerKnown = false;
	m_flControllerProbeReadinessLastTimer = 0.0f;
	m_bControllerProbeReadinessLatestDeadlineKnown = false;
	m_flControllerProbeReadinessLatestDeadline = 0.0f;
	recordControllerProbeReadinessChanges(initial);

	m_eControllerProbeSpawnHoldState =
		EmpiresControllerProbeSpawnHoldState::JOIN_NF_REQUESTED;
	logControllerProbeSpawnHoldState("initial", initial);

	// This command owns only the setup commands and the bounded input test. It
	// never calls Spawn/Revive/ForceRespawn and it does not enable normal bot AI.
	if (initial.team == EMP_TEAM_NF && initial.dead && !initial.alive &&
		controllerProbeRiflemanAcknowledged(initial) && initial.desiredSpawnKnown &&
		initial.desiredSpawn == 32)
	{
		m_eControllerProbeSpawnHoldState =
			EmpiresControllerProbeSpawnHoldState::WAITING_FOR_READY;
		logControllerProbeSpawnHoldState("already-prepared", initial);
		return true;
	}

	logger->Log(LogLevel::INFO,
		"Empires controller probe spawnhold transition state=%s command=jointeam 2 requested_class=1 requested_spawn=32",
		controllerProbeSpawnHoldStateName());
	if (!issueControllerProbeClientCommand("jointeam 2\n", "jointeam-before-after"))
	{
		failControllerProbeSpawnHoldTest("jointeam 2 could not be dispatched with stable client identity");
		return false;
	}

	EmpiresBootstrapPlayerState afterJoin;
	if (!readBootstrapPlayerState(afterJoin) ||
		!controllerProbeSpawnHoldIdentityMatches(afterJoin))
	{
		failControllerProbeSpawnHoldTest("fresh state unavailable after jointeam 2");
		return false;
	}
	recordControllerProbeReadinessChanges(afterJoin);
	m_eControllerProbeSpawnHoldState = EmpiresControllerProbeSpawnHoldState::OBSERVE_TEAM;
	m_iControllerProbeHoldStageFrames = 0;
	logControllerProbeSpawnHoldState("after-jointeam-dispatch", afterJoin);
	return true;
}

void CBotEmpires::observeControllerProbeSpawnHoldTest()
{
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeSpawnHoldState == EmpiresControllerProbeSpawnHoldState::IDLE ||
		m_eControllerProbeSpawnHoldState == EmpiresControllerProbeSpawnHoldState::COMPLETE ||
		m_eControllerProbeSpawnHoldState == EmpiresControllerProbeSpawnHoldState::FAILED)
	{
		return;
	}

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state))
	{
		failControllerProbeSpawnHoldTest("fresh state unavailable during hold-test observation");
		return;
	}
	if (!controllerProbeSpawnHoldIdentityMatches(state))
	{
		failControllerProbeSpawnHoldTest("client index/userid/edict identity changed during hold test");
		return;
	}
	recordControllerProbeReadinessChanges(state);

	++m_iControllerProbeHoldStageFrames;
	constexpr int kRequiredObservationFrames = 2;
	constexpr int kLegacyHoldSetupMaxFrames = 120;
	constexpr int kMaxResultFrames = 60;
	const bool setupTimeout = m_bControllerProbeReadinessOnly ?
		controllerProbeReadinessTimedOut(state) :
		m_iControllerProbeHoldStageFrames >= kLegacyHoldSetupMaxFrames;
	const bool setupLifeState = state.lifeStateKnown &&
		(state.lifeState == LIFE_DYING || state.lifeState == LIFE_DEAD ||
			state.lifeState == LIFE_RESPAWNABLE);
	const bool setupNonAlive = setupLifeState &&
		(!state.healthKnown || state.health <= 0);
	const bool setupAlive = state.lifeStateKnown ?
		(state.lifeState == LIFE_ALIVE && (!state.healthKnown || state.health > 0)) :
		(state.alive && !state.dead);

	switch (m_eControllerProbeSpawnHoldState)
	{
	case EmpiresControllerProbeSpawnHoldState::OBSERVE_TEAM:
		logControllerProbeSpawnHoldState("observe-team", state);
		if (state.team == EMP_TEAM_NF && setupAlive)
		{
			failControllerProbeSpawnHoldTest("automatic spawn detected while waiting for dead NF state");
			return;
		}
		if (state.team == EMP_TEAM_NF && !setupNonAlive)
		{
			failControllerProbeSpawnHoldTest("NF state was neither an SDK non-alive transition nor a verified dead state");
			return;
		}
		if (state.team == EMP_TEAM_NF && setupNonAlive &&
			m_iControllerProbeHoldStageFrames >= kRequiredObservationFrames)
		{
			m_eControllerProbeSpawnHoldState =
				EmpiresControllerProbeSpawnHoldState::RIFLEMAN_REQUESTED;
			m_iControllerProbeHoldStageFrames = 0;
			logger->Log(LogLevel::INFO,
				"Empires controller probe spawnhold transition state=%s command=changeclass 1",
				controllerProbeSpawnHoldStateName());
			if (!issueControllerProbeClientCommand("changeclass 1\n", "changeclass-before-after"))
			{
				failControllerProbeSpawnHoldTest("changeclass 1 could not be dispatched with stable client identity");
				return;
			}
			EmpiresBootstrapPlayerState afterClass;
			if (!readBootstrapPlayerState(afterClass) ||
				!controllerProbeSpawnHoldIdentityMatches(afterClass))
			{
				failControllerProbeSpawnHoldTest("fresh state unavailable after changeclass 1");
				return;
			}
			recordControllerProbeReadinessChanges(afterClass);
			if (afterClass.lifeStateKnown && afterClass.lifeState == LIFE_ALIVE &&
				(!afterClass.healthKnown || afterClass.health > 0))
			{
				failControllerProbeSpawnHoldTest("unexpected automatic spawn after changeclass 1");
				return;
			}
			m_eControllerProbeSpawnHoldState =
				EmpiresControllerProbeSpawnHoldState::OBSERVE_CLASS;
			logControllerProbeSpawnHoldState("after-changeclass-dispatch", afterClass);
			return;
		}
		if (setupTimeout)
			failControllerProbeSpawnHoldTest("jointeam 2 did not produce a stable dead NF client");
		return;

	case EmpiresControllerProbeSpawnHoldState::OBSERVE_CLASS:
		logControllerProbeSpawnHoldState("observe-class", state);
		if (setupAlive || state.team != EMP_TEAM_NF || !setupNonAlive)
		{
			failControllerProbeSpawnHoldTest("client left the non-alive NF state while observing class selection");
			return;
		}
		if (controllerProbeRiflemanAcknowledged(state) &&
			m_iControllerProbeHoldStageFrames >= kRequiredObservationFrames)
		{
			m_eControllerProbeSpawnHoldState =
				EmpiresControllerProbeSpawnHoldState::SPAWNPOINT_REQUESTED;
			m_iControllerProbeHoldStageFrames = 0;
			logger->Log(LogLevel::INFO,
				"Empires controller probe spawnhold transition state=%s command=emp_spawn_select 32 class_acknowledged=1",
				controllerProbeSpawnHoldStateName());
			if (!issueControllerProbeClientCommand("emp_spawn_select 32\n", "spawnpoint-before-after"))
			{
				failControllerProbeSpawnHoldTest("emp_spawn_select 32 could not be dispatched with stable client identity");
				return;
			}
			EmpiresBootstrapPlayerState afterSpawn;
			if (!readBootstrapPlayerState(afterSpawn) ||
				!controllerProbeSpawnHoldIdentityMatches(afterSpawn))
			{
				failControllerProbeSpawnHoldTest("fresh state unavailable after emp_spawn_select 32");
				return;
			}
			recordControllerProbeReadinessChanges(afterSpawn);
			if (afterSpawn.lifeStateKnown && afterSpawn.lifeState == LIFE_ALIVE &&
				(!afterSpawn.healthKnown || afterSpawn.health > 0))
			{
				failControllerProbeSpawnHoldTest("unexpected automatic spawn after emp_spawn_select 32");
				return;
			}
			m_eControllerProbeSpawnHoldState =
				EmpiresControllerProbeSpawnHoldState::OBSERVE_SPAWNPOINT;
			logControllerProbeSpawnHoldState("after-spawnpoint-dispatch", afterSpawn);
			return;
		}
		if (setupTimeout)
			failControllerProbeSpawnHoldTest("changeclass 1 was not acknowledged while client stayed dead");
		return;

	case EmpiresControllerProbeSpawnHoldState::OBSERVE_SPAWNPOINT:
		logControllerProbeSpawnHoldState("observe-spawnpoint", state);
		if (setupAlive || state.team != EMP_TEAM_NF || !setupNonAlive ||
			!controllerProbeRiflemanAcknowledged(state))
		{
			failControllerProbeSpawnHoldTest("spawn-point observation lost NF/non-alive/Rifleman readiness");
			return;
		}
		if (state.desiredSpawnKnown && state.desiredSpawn == 32 &&
			m_iControllerProbeHoldStageFrames >= kRequiredObservationFrames)
		{
			m_eControllerProbeSpawnHoldState =
				EmpiresControllerProbeSpawnHoldState::WAITING_FOR_READY;
			m_iControllerProbeHoldStageFrames = 0;
			logControllerProbeSpawnHoldState("before-readiness-wait", state);
			return;
		}
		if (setupTimeout)
			failControllerProbeSpawnHoldTest("emp_spawn_select 32 was not reflected while client stayed dead");
		return;

	case EmpiresControllerProbeSpawnHoldState::WAITING_FOR_READY:
		logControllerProbeSpawnHoldState("wait-ready", state);
		if (!state.connected || !state.active || state.observer)
		{
			failControllerProbeSpawnHoldTest("client association became invalid before input");
			return;
		}
		if (state.team != EMP_TEAM_NF)
		{
			failControllerProbeSpawnHoldTest("team changed before input");
			return;
		}
		if (setupAlive)
		{
			failControllerProbeSpawnHoldTest("automatic alive transition occurred before controlled input");
			return;
		}
		if (!controllerProbeRiflemanAcknowledged(state))
		{
			failControllerProbeSpawnHoldTest("desired Rifleman state was rejected before input");
			return;
		}
		if (!state.desiredSpawnKnown || state.desiredSpawn != 32)
		{
			failControllerProbeSpawnHoldTest("desired spawn 32 was reset or unavailable before input");
			return;
		}
		if (!state.lifeStateKnown)
		{
			failControllerProbeSpawnHoldTest("life state is not available through the current public SendProp");
			return;
		}
		if (state.respawnTimerKnown && state.currentTimeKnown)
		{
			if (!std::isfinite(state.respawnTimer))
			{
				m_bControllerProbeHoldTimerKnownEverySample = false;
			}
			else if (state.respawnTimer > state.currentTime)
			{
				m_bControllerProbeHoldTimerFutureObserved = true;
			}
			else
			{
				m_bControllerProbeHoldTimerExpired = true;
			}
		}
	else
	{
		m_bControllerProbeHoldTimerKnownEverySample = false;
	}

	if (m_bControllerProbeReadinessOnly)
	{
		if (!state.currentTimeKnown || !state.respawnTimerKnown ||
			!std::isfinite(state.respawnTimer))
		{
			failControllerProbeSpawnHoldTest(
				"current time or finite m_flTimeToRespawn became unavailable during readiness observation");
			return;
		}

		if (!m_bControllerProbeReadinessLatestDeadlineKnown ||
			std::fabs(m_flControllerProbeReadinessLatestDeadline - state.respawnTimer) > 0.0005f)
		{
			if (m_bControllerProbeReadinessLatestDeadlineKnown)
			{
				logger->Log(LogLevel::INFO,
					"Empires controller probe readiness latest-deadline old=%.3f new=%.3f curtime=%.3f",
					m_flControllerProbeReadinessLatestDeadline, state.respawnTimer,
					state.currentTime);
			}
			m_bControllerProbeReadinessLatestDeadlineKnown = true;
			m_flControllerProbeReadinessLatestDeadline = state.respawnTimer;
		}
	}

	if (state.lifeState == LIFE_RESPAWNABLE)
	{
			if (state.respawnTimerKnown && state.currentTimeKnown &&
				std::isfinite(state.respawnTimer) &&
				state.respawnTimer > state.currentTime)
			{
				failControllerProbeSpawnHoldTest("life state became respawnable while verified respawn timer was still in the future");
			return;
		}
		if (m_bControllerProbeReadinessOnly)
		{
			m_eControllerProbeSpawnHoldState =
				EmpiresControllerProbeSpawnHoldState::COMPLETE;
			logger->Log(LogLevel::INFO,
				"Empires controller probe readiness result=RESPAWNABLE curtime=%.3f deadline=%.3f index=%d userid=%d edict=%p controller=available team=%d class=%d class_desired=%d desired_spawn=%d health=%d life_state=%d life_state_name=%s lastcmd_buttons=0x%x",
				state.currentTime, m_flControllerProbeReadinessLatestDeadline,
				state.index, state.userId, static_cast<const void*>(state.edict),
				state.team, state.classId, state.classDesired ? 1 : 0,
				state.desiredSpawn, state.health, state.lifeState,
				lifeStateName(state.lifeState), state.lastUserCommandButtons);
			return;
		}
		m_eControllerProbeSpawnHoldState =
			EmpiresControllerProbeSpawnHoldState::ATTACK_FRAME_1;
			m_iControllerProbeHoldStageFrames = 0;
			logger->Log(LogLevel::INFO,
				"Empires controller probe spawnhold transition state=%s verified_ready=1",
				controllerProbeSpawnHoldStateName());
			return;
		}

		if (state.lifeState != LIFE_DEAD && state.lifeState != LIFE_DYING)
		{
			failControllerProbeSpawnHoldTest("unexpected SDK life state before input");
			return;
		}
	if (m_bControllerProbeReadinessOnly &&
		m_bControllerProbeReadinessLatestDeadlineKnown &&
		state.currentTime >= m_flControllerProbeReadinessLatestDeadline + 2.0f)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe readiness deadline passed by at least two seconds curtime=%.3f deadline=%.3f life_state=%d life_state_name=%s team=%d class=%d class_desired=%d desired_spawn=%d health=%d",
			state.currentTime, m_flControllerProbeReadinessLatestDeadline,
			state.lifeState, lifeStateName(state.lifeState), state.team,
			state.classId, state.classDesired ? 1 : 0, state.desiredSpawn,
			state.health);
		failControllerProbeSpawnHoldTest(
			"latest verified m_flTimeToRespawn deadline expired by two seconds without LIFE_RESPAWNABLE");
		return;
	}
	if (m_bControllerProbeReadinessOnly && controllerProbeReadinessTimedOut(state))
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe readiness safety timeout curtime=%.3f start=%.3f latest_deadline_known=%d latest_deadline=%.3f life_state=%d life_state_name=%s",
			state.currentTime, m_flControllerProbeReadinessStartTime,
			m_bControllerProbeReadinessLatestDeadlineKnown ? 1 : 0,
			m_flControllerProbeReadinessLatestDeadline, state.lifeState,
			lifeStateName(state.lifeState));
		failControllerProbeSpawnHoldTest(
			"20-second readiness safety timeout reached without LIFE_RESPAWNABLE");
		return;
	}
	if (!m_bControllerProbeReadinessOnly &&
		m_iControllerProbeHoldStageFrames >= kLegacyHoldSetupMaxFrames)
	{
			if (m_bControllerProbeHoldTimerKnownEverySample &&
				m_bControllerProbeHoldTimerFutureObserved &&
				!m_bControllerProbeHoldTimerExpired)
			{
				failControllerProbeSpawnHoldTest("verified m_flTimeToRespawn remained in the future for the entire readiness window");
			}
			else if (state.lifeState == LIFE_DEAD)
			{
				failControllerProbeSpawnHoldTest("client never reached SDK LIFE_RESPAWNABLE before input");
			}
			else
			{
				failControllerProbeSpawnHoldTest("client remained in SDK LIFE_DYING before input");
			}
		}
		return;

	case EmpiresControllerProbeSpawnHoldState::ATTACK_FRAME_1:
		logControllerProbeSpawnHoldState("before-attack-frame-1", state);
		if (m_iControllerProbeHoldAttackCount != 0 || state.team != EMP_TEAM_NF ||
			!state.dead || state.alive || !state.lifeStateKnown ||
			state.lifeState != LIFE_RESPAWNABLE ||
			!controllerProbeRiflemanAcknowledged(state) ||
			!state.desiredSpawnKnown || state.desiredSpawn != 32)
		{
			failControllerProbeSpawnHoldTest("verified readiness predicate failed before attack frame 1; no UserCmd was sent");
			return;
		}
		if (!submitControllerProbeUserCmd(IN_ATTACK, "attack-frame-1"))
		{
			failControllerProbeSpawnHoldTest("attack frame 1 could not be submitted");
			return;
		}
		m_iControllerProbeHoldAttackCount = 1;
		m_iControllerProbeHoldStageFrames = 0;
		m_eControllerProbeSpawnHoldState =
			EmpiresControllerProbeSpawnHoldState::ATTACK_FRAME_2;
		logger->Log(LogLevel::INFO,
			"Empires controller probe spawnhold transition state=%s attack_count=1 second_attack_required=1",
			controllerProbeSpawnHoldStateName());
		return;

	case EmpiresControllerProbeSpawnHoldState::ATTACK_FRAME_2:
		logControllerProbeSpawnHoldState("before-attack-frame-2", state);
		if (state.alive && !state.dead)
		{
			m_eControllerProbeSpawnHoldState =
				EmpiresControllerProbeSpawnHoldState::RELEASE_FRAME;
			m_iControllerProbeHoldStageFrames = 0;
			logger->Log(LogLevel::INFO,
				"Empires controller probe spawnhold transition state=%s second attack skipped because first attack already spawned",
				controllerProbeSpawnHoldStateName());
			return;
		}
		if (m_iControllerProbeHoldAttackCount != 1 || state.team != EMP_TEAM_NF ||
			!state.dead || state.alive || !state.lifeStateKnown ||
			(state.lifeState != LIFE_DEAD && state.lifeState != LIFE_RESPAWNABLE) ||
			!controllerProbeRiflemanAcknowledged(state) ||
			!state.desiredSpawnKnown || state.desiredSpawn != 32)
		{
			failControllerProbeSpawnHoldTest("state became invalid before attack frame 2; no second UserCmd was sent");
			return;
		}
		if (!submitControllerProbeUserCmd(IN_ATTACK, "attack-frame-2"))
		{
			failControllerProbeSpawnHoldTest("attack frame 2 could not be submitted");
			return;
		}
		m_iControllerProbeHoldAttackCount = 2;
		m_iControllerProbeHoldStageFrames = 0;
		m_eControllerProbeSpawnHoldState =
			EmpiresControllerProbeSpawnHoldState::RELEASE_FRAME;
		logger->Log(LogLevel::INFO,
			"Empires controller probe spawnhold transition state=%s attack_count=2 release_required=1",
			controllerProbeSpawnHoldStateName());
		return;

	case EmpiresControllerProbeSpawnHoldState::RELEASE_FRAME:
	{
		logControllerProbeSpawnHoldState("before-release", state);
		if ((m_iControllerProbeHoldAttackCount != 1 &&
			m_iControllerProbeHoldAttackCount != 2) ||
			m_iControllerProbeHoldReleaseCount != 0)
		{
			failControllerProbeSpawnHoldTest("attack/release counters were not exactly one-or-two attacks and zero releases");
			return;
		}
		if (!submitControllerProbeUserCmd(0, "release"))
		{
			failControllerProbeSpawnHoldTest("neutral release could not be submitted");
			return;
		}
		m_iControllerProbeHoldReleaseCount = 1;
		m_iControllerProbeHoldStageFrames = 0;
		m_iControllerProbeHoldObserveFrames = 0;
		m_eControllerProbeSpawnHoldState =
			EmpiresControllerProbeSpawnHoldState::OBSERVE_RESULT;
		EmpiresBootstrapPlayerState afterRelease;
		if (readBootstrapPlayerState(afterRelease) &&
			controllerProbeSpawnHoldIdentityMatches(afterRelease))
		{
			logControllerProbeSpawnHoldState("after-release", afterRelease);
		}
		return;
	}

	case EmpiresControllerProbeSpawnHoldState::OBSERVE_RESULT:
	{
		++m_iControllerProbeHoldObserveFrames;
		if (g_pBotManager == nullptr ||
			g_pBotManager->GetBotController(const_cast<edict_t*>(state.edict)) == nullptr)
		{
			failControllerProbeSpawnHoldTest("BotController became unavailable after release");
			return;
		}
		if (state.lastUserCommandKnown &&
			(state.lastUserCommandButtons & IN_ATTACK) != 0)
		{
			failControllerProbeSpawnHoldTest("attack button was still present after the neutral release");
			return;
		}
		const bool noMovement = state.lastUserCommandKnown &&
			std::fabs(state.lastUserCommandForwardMove) <= 0.01f &&
			std::fabs(state.lastUserCommandSideMove) <= 0.01f &&
			std::fabs(state.lastUserCommandUpMove) <= 0.01f;
		const bool classApplied = state.classKnown &&
			state.classId == EMP_CLASS_RIFLEMAN;
		const bool aliveIndicators = state.lifeStateKnown &&
			state.lifeState == LIFE_ALIVE && state.alive && !state.dead &&
			state.healthKnown && state.health > 0 && classApplied &&
			state.lastUserCommandKnown && noMovement;

		logControllerProbeSpawnHoldState("observe-result", state);
		logger->Log(LogLevel::INFO,
			"Empires controller probe spawnhold observe-result frame=%d alive_indicators=%d class_applied=%d health_positive=%d life_state_alive=%d attack_released=%d no_movement=%d",
			m_iControllerProbeHoldObserveFrames, aliveIndicators ? 1 : 0,
			classApplied ? 1 : 0,
			state.healthKnown && state.health > 0 ? 1 : 0,
			state.lifeStateKnown && state.lifeState == LIFE_ALIVE ? 1 : 0,
			state.lastUserCommandKnown &&
				(state.lastUserCommandButtons & IN_ATTACK) == 0 ? 1 : 0,
			noMovement ? 1 : 0);

		if (aliveIndicators)
		{
			m_eControllerProbeSpawnHoldState =
				EmpiresControllerProbeSpawnHoldState::COMPLETE;
			logger->Log(LogLevel::INFO,
				"Empires controller probe spawnhold result=COMPLETE index=%d userid=%d team=%d applied_class=%d desired_class=%d desired_spawn=%d health=%d life_state=%d life_state_name=%s attack_count=%d release_count=%d lastcmd_buttons=0x%x forward=%.1f side=%.1f up=%.1f",
				state.index, state.userId, state.team, state.classId,
				state.classDesired ? 1 : 0, state.desiredSpawn, state.health,
				state.lifeState, lifeStateName(state.lifeState),
				m_iControllerProbeHoldAttackCount, m_iControllerProbeHoldReleaseCount,
				state.lastUserCommandButtons, state.lastUserCommandForwardMove,
				state.lastUserCommandSideMove, state.lastUserCommandUpMove);
			return;
		}
		if (m_iControllerProbeHoldObserveFrames >= kMaxResultFrames)
		{
			failControllerProbeSpawnHoldTest("client remained dead after two attack frames and one neutral release");
		}
		return;
	}

	case EmpiresControllerProbeSpawnHoldState::IDLE:
	case EmpiresControllerProbeSpawnHoldState::JOIN_NF_REQUESTED:
	case EmpiresControllerProbeSpawnHoldState::RIFLEMAN_REQUESTED:
	case EmpiresControllerProbeSpawnHoldState::SPAWNPOINT_REQUESTED:
	case EmpiresControllerProbeSpawnHoldState::COMPLETE:
	case EmpiresControllerProbeSpawnHoldState::FAILED:
		return;
	}
}

bool CBotEmpires::requestControllerProbeSpawnSequence()
{
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle != EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr ||
		m_eControllerProbeSpawnSequenceState !=
			EmpiresControllerProbeSpawnSequenceState::IDLE)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe spawnsequence rejected: probe unavailable or sequence already used state=%s lifecycle=%s",
			controllerProbeSpawnSequenceStateName(), controllerProbeLifecycleName());
		return false;
	}

	EmpiresBootstrapPlayerState initial;
	if (!readBootstrapPlayerState(initial) || !initial.connected ||
		!initial.active || !initial.fakeClient || initial.index <= 0 ||
		initial.userId <= 0)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence rejected: initial fresh state is not a connected fake client");
		return false;
	}
	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(initial.edict)) == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe spawnsequence rejected: initial fresh controller is unavailable");
		return false;
	}

	m_iControllerProbeSequenceExpectedIndex = initial.index;
	m_iControllerProbeSequenceExpectedUserId = initial.userId;
	m_iControllerProbeSequenceStageFrames = 0;
	m_iControllerProbeSequencePressCount = 0;
	m_iControllerProbeSequenceReleaseCount = 0;
	m_iControllerProbeSequenceStableSamples = 0;
	m_bControllerProbeSequenceInitialAlive = initial.alive;
	m_vControllerProbeSequenceOrigin = Vector();
	m_bControllerProbeSequenceOriginKnown = false;

	m_eControllerProbeSpawnSequenceState =
		EmpiresControllerProbeSpawnSequenceState::JOIN_NF_REQUESTED;
	logControllerProbeSpawnSequenceState("initial", initial);
	logger->Log(LogLevel::INFO,
		"Empires controller probe spawnsequence transition state=%s command=jointeam 2 requested_class=1 requested_spawn=32 initial_alive=%d initial_dead=%d",
		controllerProbeSpawnSequenceStateName(), initial.alive ? 1 : 0,
		initial.dead ? 1 : 0);

	if (!issueControllerProbeClientCommand("jointeam 2\n", "jointeam-before-after"))
	{
		failControllerProbeSpawnSequence("jointeam 2 could not be dispatched with stable client identity");
		return false;
	}

	EmpiresBootstrapPlayerState afterJoin;
	if (!readBootstrapPlayerState(afterJoin) ||
		!controllerProbeSpawnSequenceIdentityMatches(afterJoin))
	{
		failControllerProbeSpawnSequence("fresh state unavailable after jointeam 2");
		return false;
	}
	m_eControllerProbeSpawnSequenceState =
		EmpiresControllerProbeSpawnSequenceState::OBSERVE_TEAM;
	m_iControllerProbeSequenceStageFrames = 0;
	logControllerProbeSpawnSequenceState("after-jointeam-dispatch", afterJoin);
	return true;
}

void CBotEmpires::observeControllerProbeSpawnSequence()
{
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE ||
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::COMPLETE ||
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::FAILED)
	{
		return;
	}

	EmpiresBootstrapPlayerState state;
	if (!readBootstrapPlayerState(state))
	{
		failControllerProbeSpawnSequence("fresh state unavailable during sequence observation");
		return;
	}
	if (!controllerProbeSpawnSequenceIdentityMatches(state))
	{
		failControllerProbeSpawnSequence("client index/userid/edict identity changed during sequence");
		return;
	}

	++m_iControllerProbeSequenceStageFrames;
	const bool shouldLogObservation =
		m_iControllerProbeSequenceStageFrames <= 3 ||
		(m_iControllerProbeSequenceStageFrames % 10) == 0 ||
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::ATTACK_PRESS_PENDING ||
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::ATTACK_RELEASE_PENDING;
	if (shouldLogObservation)
		logControllerProbeSpawnSequenceState("observe", state);

	constexpr int kRequiredObservationFrames = 2;
	constexpr int kMaxObservationFrames = 120;

	switch (m_eControllerProbeSpawnSequenceState)
	{
	case EmpiresControllerProbeSpawnSequenceState::OBSERVE_TEAM:
		if (state.team == EMP_TEAM_NF && state.alive)
		{
			// The engine-created probe is initially alive before team selection.
			// Allow that already-observed state to settle for one observation
			// frame; an initially dead probe never receives this exception.
			if (!m_bControllerProbeSequenceInitialAlive ||
				m_iControllerProbeSequenceStageFrames >= kRequiredObservationFrames)
			{
				failControllerProbeSpawnSequence("automatic spawn or unchanged alive state detected while observing NF team");
			}
			return;
		}
		if (state.team == EMP_TEAM_NF && !state.dead)
		{
			failControllerProbeSpawnSequence("NF team state was neither dead nor alive");
			return;
		}
		if (state.team == EMP_TEAM_NF && state.dead && !state.alive &&
			m_iControllerProbeSequenceStageFrames >= kRequiredObservationFrames)
		{
			m_eControllerProbeSpawnSequenceState =
				EmpiresControllerProbeSpawnSequenceState::RIFLEMAN_REQUESTED;
			m_iControllerProbeSequenceStageFrames = 0;
			logger->Log(LogLevel::INFO,
				"Empires controller probe spawnsequence transition state=%s command=changeclass 1",
				controllerProbeSpawnSequenceStateName());
			if (!issueControllerProbeClientCommand("changeclass 1\n", "changeclass-before-after"))
			{
				failControllerProbeSpawnSequence("changeclass 1 could not be dispatched with stable client identity");
				return;
			}

			EmpiresBootstrapPlayerState afterClassDispatch;
			if (!readBootstrapPlayerState(afterClassDispatch) ||
				!controllerProbeSpawnSequenceIdentityMatches(afterClassDispatch))
			{
				failControllerProbeSpawnSequence("fresh state unavailable after changeclass 1");
				return;
			}
			if (afterClassDispatch.alive)
			{
				failControllerProbeSpawnSequence("unexpected automatic spawn after changeclass 1");
				return;
			}
			m_eControllerProbeSpawnSequenceState =
				EmpiresControllerProbeSpawnSequenceState::OBSERVE_CLASS;
			logControllerProbeSpawnSequenceState("after-changeclass-dispatch", afterClassDispatch);
			return;
		}
		if (m_iControllerProbeSequenceStageFrames >= kMaxObservationFrames)
		{
			failControllerProbeSpawnSequence("jointeam 2 did not produce a stable dead NF client");
		}
		return;

	case EmpiresControllerProbeSpawnSequenceState::OBSERVE_CLASS:
		if (state.alive)
		{
			failControllerProbeSpawnSequence("automatic spawn detected while observing Rifleman request");
			return;
		}
		if (state.team != EMP_TEAM_NF || !state.dead)
		{
			failControllerProbeSpawnSequence("client left the dead NF state while observing Rifleman request");
			return;
		}
		if (controllerProbeRiflemanAcknowledged(state) &&
			m_iControllerProbeSequenceStageFrames >= kRequiredObservationFrames)
		{
			m_eControllerProbeSpawnSequenceState =
				EmpiresControllerProbeSpawnSequenceState::SPAWNPOINT_REQUESTED;
			m_iControllerProbeSequenceStageFrames = 0;
			logger->Log(LogLevel::INFO,
				"Empires controller probe spawnsequence transition state=%s command=emp_spawn_select 32 class_acknowledged=1",
				controllerProbeSpawnSequenceStateName());
			if (!issueControllerProbeClientCommand("emp_spawn_select 32\n", "spawnpoint-before-after"))
			{
				failControllerProbeSpawnSequence("emp_spawn_select 32 could not be dispatched with stable client identity");
				return;
			}

			EmpiresBootstrapPlayerState afterSpawnPointDispatch;
			if (!readBootstrapPlayerState(afterSpawnPointDispatch) ||
				!controllerProbeSpawnSequenceIdentityMatches(afterSpawnPointDispatch))
			{
				failControllerProbeSpawnSequence("fresh state unavailable after emp_spawn_select 32");
				return;
			}
			if (afterSpawnPointDispatch.alive)
			{
				failControllerProbeSpawnSequence("unexpected automatic spawn after emp_spawn_select 32");
				return;
			}
			m_eControllerProbeSpawnSequenceState =
				EmpiresControllerProbeSpawnSequenceState::OBSERVE_SPAWNPOINT;
			logControllerProbeSpawnSequenceState("after-spawnpoint-dispatch", afterSpawnPointDispatch);
			return;
		}
		if (m_iControllerProbeSequenceStageFrames >= kMaxObservationFrames)
		{
			failControllerProbeSpawnSequence("changeclass 1 was not acknowledged while client stayed dead");
		}
		return;

	case EmpiresControllerProbeSpawnSequenceState::OBSERVE_SPAWNPOINT:
		if (state.alive)
		{
			failControllerProbeSpawnSequence("automatic spawn detected before the controlled attack confirmation");
			return;
		}
		if (state.team != EMP_TEAM_NF || !state.dead ||
			!controllerProbeRiflemanAcknowledged(state))
		{
			failControllerProbeSpawnSequence("spawn-point observation lost NF/dead/Rifleman readiness");
			return;
		}
		if (state.desiredSpawnKnown && state.desiredSpawn == 32 &&
			m_iControllerProbeSequenceStageFrames >= kRequiredObservationFrames)
		{
			m_eControllerProbeSpawnSequenceState =
				EmpiresControllerProbeSpawnSequenceState::ATTACK_PRESS_PENDING;
			m_iControllerProbeSequenceStageFrames = 0;
			logControllerProbeSpawnSequenceState("before-attack-pending", state);
			return;
		}
		if (m_iControllerProbeSequenceStageFrames >= kMaxObservationFrames)
		{
			failControllerProbeSpawnSequence("emp_spawn_select 32 was not reflected in m_iDesiredSpawn while client stayed dead");
		}
		return;

	case EmpiresControllerProbeSpawnSequenceState::ATTACK_PRESS_PENDING:
		logControllerProbeSpawnSequenceState("before-attack", state);
		if (m_iControllerProbeSequencePressCount != 0 ||
			state.team != EMP_TEAM_NF || !state.dead || state.alive ||
			!controllerProbeRiflemanAcknowledged(state) ||
			!state.desiredSpawnKnown || state.desiredSpawn != 32)
		{
			failControllerProbeSpawnSequence("pre-attack readiness predicate failed; no UserCmd was sent");
			return;
		}
		if (!submitControllerProbeUserCmd(IN_ATTACK, "attack-press"))
		{
			failControllerProbeSpawnSequence("single IN_ATTACK press could not be submitted");
			return;
		}
		m_iControllerProbeSequencePressCount = 1;
		m_eControllerProbeSpawnSequenceState =
			EmpiresControllerProbeSpawnSequenceState::ATTACK_RELEASE_PENDING;
		m_iControllerProbeSequenceStageFrames = 0;
		logger->Log(LogLevel::INFO,
			"Empires controller probe spawnsequence transition state=%s press_count=%d release_required=1",
			controllerProbeSpawnSequenceStateName(), m_iControllerProbeSequencePressCount);
		return;

	case EmpiresControllerProbeSpawnSequenceState::ATTACK_RELEASE_PENDING:
		logControllerProbeSpawnSequenceState("before-attack-release", state);
		if (m_iControllerProbeSequencePressCount != 1 ||
			m_iControllerProbeSequenceReleaseCount != 0)
		{
			failControllerProbeSpawnSequence("attack press/release counters were not exactly one-before-zero");
			return;
		}
		if (state.alive)
		{
			// The press has already been sent, so the neutral release remains
			// mandatory even if the game changed life state in response to it.
			logger->Log(LogLevel::WARN,
				"Empires controller probe spawnsequence unexpected alive transition before mandatory neutral release; sending release only");
		}
		if (!submitControllerProbeUserCmd(0, "attack-release"))
		{
			failControllerProbeSpawnSequence("mandatory neutral release could not be submitted");
			return;
		}
		m_iControllerProbeSequenceReleaseCount = 1;
		m_eControllerProbeSpawnSequenceState =
			EmpiresControllerProbeSpawnSequenceState::OBSERVE_SPAWN;
		m_iControllerProbeSequenceStageFrames = 0;
		m_iControllerProbeSequenceStableSamples = 0;
		m_bControllerProbeSequenceOriginKnown = false;
		logControllerProbeSpawnSequenceState("after-attack-release", state);
		return;

	case EmpiresControllerProbeSpawnSequenceState::OBSERVE_SPAWN:
		{
			const bool classApplied = state.classKnown &&
				state.classId == EMP_CLASS_RIFLEMAN;
			const bool healthPositive = state.healthKnown && state.health > 0;
			const bool lifeStateAlive = state.lifeStateKnown && state.lifeState == 0;
			const bool attackReleased = state.lastUserCommandKnown &&
				(state.lastUserCommandButtons & IN_ATTACK) == 0;
			const bool noMovement = state.lastUserCommandKnown &&
				std::fabs(state.lastUserCommandForwardMove) <= 0.01f &&
				std::fabs(state.lastUserCommandSideMove) <= 0.01f &&
				std::fabs(state.lastUserCommandUpMove) <= 0.01f;
			const bool aliveIndicators = state.alive && !state.dead &&
				classApplied && healthPositive && lifeStateAlive &&
				attackReleased && noMovement;

			if (aliveIndicators && state.originKnown)
			{
				if (!m_bControllerProbeSequenceOriginKnown)
				{
					m_vControllerProbeSequenceOrigin = state.origin;
					m_bControllerProbeSequenceOriginKnown = true;
					m_iControllerProbeSequenceStableSamples = 1;
				}
				else
				{
					const float deltaSqr =
						(state.origin - m_vControllerProbeSequenceOrigin).LengthSqr();
					if (deltaSqr > 0.01f)
					{
						failControllerProbeSpawnSequence("origin moved after the controlled no-movement input");
						return;
					}
					++m_iControllerProbeSequenceStableSamples;
				}
			}

			if (shouldLogObservation || aliveIndicators)
			{
				logger->Log(LogLevel::INFO,
					"Empires controller probe spawnsequence observe-result frame=%d alive_indicators=%d class_applied=%d health_positive=%d life_state_alive=%d attack_released=%d no_movement=%d stable_samples=%d",
					m_iControllerProbeSequenceStageFrames, aliveIndicators ? 1 : 0,
					classApplied ? 1 : 0, healthPositive ? 1 : 0,
					lifeStateAlive ? 1 : 0, attackReleased ? 1 : 0,
					noMovement ? 1 : 0, m_iControllerProbeSequenceStableSamples);
			}

			if (aliveIndicators && m_iControllerProbeSequenceStableSamples >= 2)
			{
				m_eControllerProbeSpawnSequenceState =
					EmpiresControllerProbeSpawnSequenceState::COMPLETE;
				logger->Log(LogLevel::INFO,
					"Empires controller probe spawnsequence result=COMPLETE index=%d userid=%d team=%d applied_class=%d desired_spawn=%d health=%d life_state=%d dead=%d alive=%d press_count=%d release_count=%d stable_samples=%d lastcmd_buttons=0x%x forward=%.1f side=%.1f up=%.1f",
					state.index, state.userId, state.team, state.classId,
					state.desiredSpawn, state.health, state.lifeState,
					state.dead ? 1 : 0, state.alive ? 1 : 0,
					m_iControllerProbeSequencePressCount,
					m_iControllerProbeSequenceReleaseCount,
					m_iControllerProbeSequenceStableSamples,
					state.lastUserCommandButtons,
					state.lastUserCommandForwardMove,
					state.lastUserCommandSideMove,
					state.lastUserCommandUpMove);
				return;
			}

			if (m_iControllerProbeSequenceStageFrames >= kMaxObservationFrames)
			{
				failControllerProbeSpawnSequence(
					"client did not reach a stable alive Rifleman state after one press and one release");
			}
		}
		return;

	case EmpiresControllerProbeSpawnSequenceState::IDLE:
	case EmpiresControllerProbeSpawnSequenceState::JOIN_NF_REQUESTED:
	case EmpiresControllerProbeSpawnSequenceState::RIFLEMAN_REQUESTED:
	case EmpiresControllerProbeSpawnSequenceState::SPAWNPOINT_REQUESTED:
	case EmpiresControllerProbeSpawnSequenceState::COMPLETE:
	case EmpiresControllerProbeSpawnSequenceState::FAILED:
		return;
	}
}

bool CBotEmpires::requestBootstrapRemoval()
{
	if (!m_bBootstrapOnly || m_pEdict == nullptr || m_bRemovalRequested)
	{
		return false;
	}

	m_bRemovalRequested = true;
	const int index = engine != nullptr ? engine->IndexOfEdict(m_pEdict) : -1;
	logger->Log(LogLevel::INFO, "Empires bootstrap lifecycle=remove-requested index=%d", index);
	return true;
}

bool CBotEmpires::requestControllerProbeTeam(const int requestedTeam)
{
	if (requestedTeam != EMP_TEAM_NF && requestedTeam != EMP_TEAM_BE)
		return false;

	const bool noPendingProbeOperation =
		m_eControllerProbeSpawnSequenceState ==
			EmpiresControllerProbeSpawnSequenceState::IDLE &&
		m_eControllerProbeSpawnHoldState ==
			EmpiresControllerProbeSpawnHoldState::IDLE &&
		m_eControllerProbeNeutralReadinessState ==
			EmpiresControllerProbeNeutralReadinessState::IDLE &&
		m_eControllerProbeLoadoutReadinessState ==
			EmpiresControllerProbeLoadoutReadinessState::IDLE &&
		m_eControllerProbeAppliedLoadoutReadinessState ==
			EmpiresControllerProbeAppliedLoadoutReadinessState::IDLE;
	if (!m_bControllerProbeOnly ||
		m_eControllerProbeLifecycle != EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		!noPendingProbeOperation)
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe jointeam rejected: probe unavailable or another probe operation is pending");
		return false;
	}

	EmpiresBootstrapPlayerState before;
	if (!readBootstrapPlayerState(before) || before.edict == nullptr ||
		before.edict->IsFree() || !before.connected || !before.active ||
		!before.fakeClient || before.index <= 0 || before.userId <= 0)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe jointeam rejected: client state is unavailable or invalid");
		return false;
	}

	if (before.team == requestedTeam)
	{
		logger->Log(LogLevel::INFO,
			"Empires controller probe jointeam already satisfied index=%d userid=%d team=%d",
			before.index, before.userId, before.team);
		return true;
	}

	if (g_pBotManager == nullptr ||
		g_pBotManager->GetBotController(const_cast<edict_t*>(before.edict)) == nullptr ||
		engine == nullptr)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe jointeam rejected: fresh controller or engine unavailable");
		return false;
	}

	edict_t* pCurrentEdict = engine->PEntityOfEntIndex(before.index);
	if (pCurrentEdict == nullptr || pCurrentEdict->IsFree() || pCurrentEdict != before.edict ||
		engine->IndexOfEdict(pCurrentEdict) != before.index)
	{
		logger->Log(LogLevel::ERROR,
			"Empires controller probe jointeam rejected: client identity changed before command index=%d edict=%p fresh_edict=%p",
			before.index, static_cast<const void*>(before.edict), static_cast<void*>(pCurrentEdict));
		return false;
	}

	char command[32];
	std::snprintf(command, sizeof(command), "jointeam %d\n", requestedTeam);
	logger->Log(LogLevel::INFO,
		"Empires controller probe jointeam before index=%d userid=%d team_before=%d requested_team=%d command=%s timer_observation=not_started",
		before.index, before.userId, before.team, requestedTeam, command);

	// Use the documented IVEngineServer route that mimics the specified client
	// typing the command. The edict was freshly resolved and identity-checked
	// above so a connected human cannot be selected accidentally.
	// This operation assigns only the team; it does not choose a class, select a
	// spawn point, submit a UserCmd, or invoke GameRules.
	engine->ClientCommand(pCurrentEdict, "%s", command);

	EmpiresBootstrapPlayerState after;
	if (readBootstrapPlayerState(after))
	{
		logger->Log(LogLevel::INFO,
			"Empires controller probe jointeam after index=%d userid=%d team_after=%d requested_team=%d dead=%d alive=%d life_state=%d health=%d observation=pending commander_timer=not_read",
			after.index, after.userId, after.team, requestedTeam, after.dead ? 1 : 0,
			after.alive ? 1 : 0, after.lifeState, after.health);
	}
	else
	{
		logger->Log(LogLevel::WARN,
			"Empires controller probe jointeam after-dispatch state unavailable observation=pending");
	}

	return true;
}

bool CBotEmpires::requestControllerProbeRemoval()
{
	if (!m_bControllerProbeOnly || m_eControllerProbeLifecycle != EmpiresControllerProbeLifecycle::CONTROLLER_VERIFIED ||
		getCurrentControlledTestEdict() == nullptr || m_bRemovalRequested)
		return false;

	m_bRemovalRequested = true;
	m_eControllerProbeLifecycle = EmpiresControllerProbeLifecycle::REMOVAL_PENDING;
	logger->Log(LogLevel::INFO,
		"Empires controller probe lifecycle=REMOVAL_PENDING slot=%d index=%d",
		m_iControllerProbeSlot, m_iControllerProbeClientIndex);
	return true;
}

bool CBotEmpires::suppressBotThink()
{
	if (m_bControllerProbeOnly)
	{
		if (m_bControllerProbeWaypointPathMode &&
			m_eControllerProbeWaypointState !=
			EmpiresControllerProbeWaypointState::IDLE)
			observeControllerProbePathOnce();
		else if (m_eControllerProbeWaypointState !=
			EmpiresControllerProbeWaypointState::IDLE)
			observeControllerProbeWaypointOnce();
		else if (m_eControllerProbeMovementState !=
			EmpiresControllerProbeMovementState::IDLE)
			observeControllerProbeMovementProof();
		else if (m_eControllerProbeSpawnConfirmReadyState !=
			EmpiresControllerProbeSpawnConfirmReadyState::IDLE)
			observeControllerProbeSpawnConfirmReady();
		else if (m_eControllerProbeAppliedLoadoutReadinessState !=
			EmpiresControllerProbeAppliedLoadoutReadinessState::IDLE)
			observeControllerProbeAppliedLoadoutReadiness();
		else if (m_eControllerProbeLoadoutReadinessState !=
			EmpiresControllerProbeLoadoutReadinessState::IDLE)
			observeControllerProbeLoadoutReadiness();
		else if (m_eControllerProbeNeutralReadinessState !=
			EmpiresControllerProbeNeutralReadinessState::IDLE)
			observeControllerProbeNeutralReadiness();
		else if (m_eControllerProbeSpawnHoldState != EmpiresControllerProbeSpawnHoldState::IDLE)
			observeControllerProbeSpawnHoldTest();
		else
			observeControllerProbeSpawnSequence();
		if (!m_bThinkGateLogged)
		{
			logger->Log(LogLevel::INFO, "Empires controller probe lifecycle=think-gated index=%d",
				engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1);
			m_bThinkGateLogged = true;
		}
		return true;
	}

	// Observe before any Empires-specific think gate. Normal spawn and respawn
	// confirmation runs behind this gate, so observing only from modThink()
	// misses the dead state and cannot report the later alive transition.
	// Passive conflict sampling also drives the optional runtime Local
	// Yielding controller, so it must not depend on an analysis session.
	EmpiresAnalysis::ObserveBot(this);

	// A kicked fake client can be replaced at the same edict address. Reset the
	// normal Empires state before the terminal path gate, using the engine user
	// id as the client-generation identity.
	if (!m_bBootstrapOnly && m_pEdict != nullptr && engine != nullptr)
	{
		const int currentUserId = engine->GetPlayerUserId(m_pEdict);
		if (currentUserId > 0 && currentUserId != m_iNormalLifecycleUserId)
		{
			if (m_iNormalLifecycleUserId > 0)
				RCBotLocalYielding::ClearBot(m_iNormalLifecycleUserId);
			m_iNormalLifecycleUserId = currentUserId;
			m_pNormalTeamEdict = nullptr;
			// Clear the terminal manual-path gate before the next call to
			// CBot::think(). The full normal-state reset is performed by
			// startGame() once the null team-edict marker is observed.
			m_eNormalPathState = EmpiresNormalPathState::IDLE;
			m_bNormalPathTerminalAfterStop = false;
			m_bNormalStartGameEntryLogged = false;
			m_bNormalThinkObserverEntryLogged = false;
			logger->Log(LogLevel::INFO,
				"Empires normal lifecycle=new-client-generation userid=%d state-reset=1",
				currentUserId);
		}
	}

	// A revive Engineer is on the way: stay dead (as a player waits in the spawn menu).
	// Revived during the hold: alive again, so no respawn (selection would move the bot).
	if (m_bNormalDeathEventPending && m_bReviveHoldActive && CBotGlobals::entityIsAlive(m_pEdict))
	{
		m_bNormalDeathEventPending = false;
		m_bReviveHoldActive = false;
		Msg("[RCBOT2] revive revived-skip-respawn userid=%d\n", engine->GetPlayerUserId(m_pEdict));
	}
	if (m_bNormalDeathEventPending && holdRespawnForRevive())
	{
		m_bReviveHoldActive = true;
		return true;
	}
	m_bReviveHoldActive = false;
	if (beginNormalRespawn())
		return false;

	// The explicit empires_normal_path_once regression command is terminal. The
	// normal lifecycle route is non-terminal and must return to the ordinary
	// Think loop for condition processing and the small gameplay task bridge.
	if (m_bNormalPathTerminalAfterStop &&
		(m_eNormalPathState == EmpiresNormalPathState::COMPLETE ||
		 m_eNormalPathState == EmpiresNormalPathState::FAILED))
		return true;

	// During normal Empires spawn selection/confirmation, suppress the generic
	// bot loop as well. observeNormalSpawn() owns the bounded input sequence and
	// keeps navigation, combat and movement out of these frames.
	if (!m_bBootstrapOnly && m_eNormalSpawnState != EmpiresNormalSpawnState::IDLE &&
		m_eNormalSpawnState != EmpiresNormalSpawnState::COMPLETE &&
		m_eNormalSpawnState != EmpiresNormalSpawnState::FAILED)
	{
		if (!m_bNormalThinkObserverEntryLogged)
		{
			logger->Log(LogLevel::INFO,
				"Empires normal think observer entry index=%d userid=%d edict=%p state=%s",
				engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1,
				engine != nullptr && m_pEdict != nullptr ? engine->GetPlayerUserId(m_pEdict) : -1,
				static_cast<void*>(m_pEdict), normalSpawnStateName(m_eNormalSpawnState));
			m_bNormalThinkObserverEntryLogged = true;
		}
		observeNormalSpawn();
		// Do not submit UserCmds while the normal Empires client is waiting for
		// LIFE_RESPAWNABLE. The successful controller path remains input-free in
		// this interval; only the bounded attack/release confirmation below owns
		// UserCmd submission after readiness is observed.
		return true;
	}

	if (!m_bBootstrapOnly)
	{
		return false;
	}

	observePendingTeamChange();
	observePendingClassChange();
	if (m_eSpawnConfirmationState == EmpiresBootstrapSpawnConfirmationState::IDLE)
		observePendingSpawn();
	else
		processBootstrapSpawnConfirmation();

	if (!m_bThinkGateLogged)
	{
		logger->Log(LogLevel::INFO, "Empires bootstrap lifecycle=think-gated index=%d",
			engine != nullptr && m_pEdict != nullptr ? engine->IndexOfEdict(m_pEdict) : -1);
		m_bThinkGateLogged = true;
	}

	return true;
}

bool CBotEmpires::startGame()
{
	if (isControlledTestOnly())
	{
		if (!m_bStartGameGateLogged)
		{
			logger->Log(LogLevel::INFO, "Empires controlled test lifecycle=startGame-gated");
			m_bStartGameGateLogged = true;
		}
		return false;
	}

	if (m_pEdict == nullptr || m_pPlayerInfo == nullptr ||
		helpers == nullptr || engine == nullptr)
	{
		return false;
	}

	if (!m_bNormalStartGameEntryLogged)
	{
		logger->Log(LogLevel::INFO,
			"Empires normal startGame entry index=%d userid=%d edict=%p state=%s team_edict=%p team_complete=%d team_pending=%d class_complete=%d class_pending=%d controller=%s playerinfo=%s",
			engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
			static_cast<void*>(m_pEdict), normalSpawnStateName(m_eNormalSpawnState),
			static_cast<const void*>(m_pNormalTeamEdict), m_bNormalTeamStartComplete ? 1 : 0,
			m_bNormalTeamCommandPending ? 1 : 0, m_bNormalClassStartComplete ? 1 : 0,
			m_bNormalClassCommandPending ? 1 : 0, m_pController != nullptr ? "available" : "null",
			m_pPlayerInfo != nullptr ? "available" : "null");
		m_bNormalStartGameEntryLogged = true;
	}

	// A CBotEmpires slot can be reused after a fake client disconnects. Do not
	// carry the previous client's team-start state into the new edict.
	if (m_pNormalTeamEdict != m_pEdict)
	{
		m_pNormalTeamEdict = m_pEdict;
		m_bNormalTeamStartComplete = false;
		m_bNormalTeamCommandPending = false;
		m_bNormalTeamStartFailed = false;
		m_iNormalTeam = -1;
		m_iNormalTeamFrames = 0;
		m_flNormalTeamDeadline = 0.0f;
		m_bNormalClassStartComplete = false;
		m_bNormalClassCommandPending = false;
		m_bNormalClassStartFailed = false;
		m_bNormalLoadoutRequested = false;
		m_bNormalLoadoutAccepted = false;
		m_iNormalClass = -1;
		m_iNormalClassFrames = 0;
		m_flNormalClassDeadline = 0.0f;
		m_eNormalSpawnState = EmpiresNormalSpawnState::IDLE;
		m_iNormalSpawn = -1;
		m_iNormalSpawnFrames = 0;
		m_flNormalSpawnDeadline = 0.0f;
		m_flNormalSpawnObserveDeadline = 0.0f;
		m_flNormalSpawnConfirmEarliestTime = 0.0f;
		m_iNormalSpawnExpectedIndex = -1;
		m_iNormalSpawnExpectedUserId = -1;
		m_pNormalSpawnExpectedEdict = nullptr;
		m_pNormalSpawnSelectionEdict = nullptr;
		m_bNormalSpawnEligibilityLogged = false;
		m_bNormalSpawnLastLifeStateKnown = false;
		m_iNormalSpawnLastLifeState = -1;
		m_bNormalSpawnLastTimerKnown = false;
		m_flNormalSpawnLastTimer = 0.0f;
		m_eNormalPathState = EmpiresNormalPathState::IDLE;
		m_bNormalPathAutoStarted = false;
		m_bNormalPathTerminalAfterStop = false;
		m_bNormalPathSearchStarted = false;
		m_iNormalPathSearchFrames = 0;
		m_bNormalDeathEventPending = false;
		m_iNormalPathStartWaypoint = -1;
		m_iNormalPathDestinationWaypoint = -1;
		m_iNormalPathLastWaypoint = -1;
		m_iNormalPathActiveFrames = 0;
		m_iNormalPathNoProgressFrames = 0;
		m_iNormalPathExpectedIndex = -1;
		m_iNormalPathExpectedUserId = -1;
		m_pNormalPathExpectedEdict = nullptr;
		m_flNormalPathStartTime = 0.0f;
		m_flNormalPathDeadline = 0.0f;
		m_bNormalPathLastOriginKnown = false;
		m_NormalPathRoute.clear();
		m_pEmpiresApproachTarget = nullptr;
		m_bEmpiresApproachTaskIssued = false;
		m_bEmpiresLastEnemyObscured = false;
		m_bEmpiresLastEnemySearchActive = false;
		m_bEmpiresLastEnemySearchTaskLogged = false;
		m_iEmpiresSquadObserved = -1;
		m_iEmpiresSquadPositionObserved = -1;
		m_bEmpiresSquadLeaderObserved = false;
		m_bEmpiresSquadLeaderKnownObserved = false;
		m_flEmpiresNextSquadActionTime = 0.0f;
		m_iEmpiresSquadJoinAttempts = 0;
		resetEmpiresSquadGoalState();
		m_bEmpiresCombatEngaged = false;
		m_bEmpiresEngineerSupportTaskIssued = false;
		m_bEmpiresEngineerSupportNearby = false;
		m_iEmpiresEngineerSupportBuildingIndex = -1;
		m_iEmpiresEngineerSupportWaypoint = -1;
		m_iEmpiresEngineerSupportRejectedBuildingIndex = -1;
		m_iEmpiresEngineerSupportRejectedWaypoint = -1;
		m_vEmpiresEngineerSupportWaypointOrigin = vec3_origin;
		m_flEmpiresNextEngineerSupportTime = 0.0f;
		m_flEmpiresEngineerSupportRejectedUntil = 0.0f;
		m_eEmpiresMortarActionState = EmpiresMortarActionState::IDLE;
		m_iEmpiresMortarActionTeam = -1;
		m_iEmpiresMortarActionExpectedIndex = -1;
		m_iEmpiresMortarActionExpectedUserId = -1;
		m_pEmpiresMortarActionExpectedEdict = nullptr;
		m_pEmpiresMortarActionWeapon = nullptr;
		m_iEmpiresMortarActionShellBaseline = 0;
		m_iEmpiresMortarActionShellIndex = -1;
		m_iEmpiresMortarActionAmmoBefore = -1;
		m_iEmpiresMortarActionAmmoAfter = -1;
		m_iEmpiresMortarActionFrames = 0;
		m_bEmpiresMortarActionPostureLogged = false;
		m_bEmpiresMortarActionAttackLogged = false;
		m_bEmpiresMortarActionReleaseLogged = false;
		m_flEmpiresMortarActionDeadline = 0.0f;
		m_flEmpiresMortarActionShellDeadline = 0.0f;
		m_eEmpiresEngineerRepairActionState = EmpiresEngineerRepairActionState::IDLE;
		m_iEmpiresEngineerRepairActionTeam = -1;
		m_iEmpiresEngineerRepairActionExpectedIndex = -1;
		m_iEmpiresEngineerRepairActionExpectedUserId = -1;
		m_pEmpiresEngineerRepairActionExpectedEdict = nullptr;
		m_pEmpiresEngineerRepairActionBuilding = nullptr;
		m_iEmpiresEngineerRepairActionBuildingIndex = -1;
		m_iEmpiresEngineerRepairActionHealthBefore = -1;
		m_iEmpiresEngineerRepairActionHealthLast = -1;
		m_iEmpiresEngineerRepairActionMaxHealth = -1;
		m_iEmpiresEngineerRepairActionFrames = 0;
		m_flEmpiresEngineerRepairActionDeadline = 0.0f;
		m_bEmpiresEngineerRepairActionRequireFullHealth = false;
		m_bEmpiresEngineerRepairActionProgressLogged = false;
		m_eEmpiresEngineerBuildSelectionState = EmpiresEngineerBuildSelectionState::IDLE;
		m_iEmpiresEngineerBuildSelectionTeam = -1;
		m_iEmpiresEngineerBuildSelectionExpectedIndex = -1;
		m_iEmpiresEngineerBuildSelectionExpectedUserId = -1;
		m_pEmpiresEngineerBuildSelectionExpectedEdict = nullptr;
		m_pEmpiresEngineerBuildSelectionWeapon = nullptr;
		m_iEmpiresEngineerBuildSelectionFrames = 0;
		m_iEmpiresEngineerBuildSelectionBaselineItem = -1;
		m_iEmpiresEngineerBuildSelectionLastItem = -1;
		m_iEmpiresEngineerBuildSelectionBaselineState = -1;
		m_iEmpiresEngineerBuildSelectionLastState = -1;
		m_bEmpiresEngineerBuildSelectionItemKnown = false;
		m_bEmpiresEngineerBuildSelectionStateKnown = false;
		m_bEmpiresEngineerBuildSelectionInputRequested = false;
		m_bEmpiresEngineerBuildSelectionInputSubmitted = false;
		m_bEmpiresEngineerBuildSelectionBaselineLogged = false;
		m_bEmpiresEngineerBuildSelectionTransitionLogged = false;
		m_flEmpiresEngineerBuildSelectionDeadline = 0.0f;
		m_bEmpiresMortarAimRequested = false;
		m_iEmpiresMortarAimTargetWaypoint = -1;
		m_vEmpiresMortarAimTarget = Vector(0.0f, 0.0f, 0.0f);
		m_qEmpiresMortarAimAngles = QAngle(0.0f, 0.0f, 0.0f);
		m_vEmpiresMortarAimLaunchOrigin = Vector(0.0f, 0.0f, 0.0f);
		m_bEmpiresMortarAimComputed = false;
		m_bEmpiresMortarAimViewApplied = false;
		m_iEmpiresMortarAimTrajectorySamples = 0;
		m_flEmpiresMortarAimLastSampleTime = 0.0f;
		m_vEmpiresMortarAimFirstShellOrigin = Vector(0.0f, 0.0f, 0.0f);
		m_vEmpiresMortarAimLastShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	}

	const int currentTeam = m_pPlayerInfo->GetTeamIndex();
	if (currentTeam != EMP_TEAM_NF && currentTeam != EMP_TEAM_BE)
	{
		if (m_bNormalTeamStartFailed)
			return false;

		if (m_bNormalTeamCommandPending)
		{
			++m_iNormalTeamFrames;
			if (currentTeam == m_iNormalTeam)
			{
				m_bNormalTeamStartComplete = true;
				m_bNormalTeamCommandPending = false;
				logger->Log(LogLevel::INFO,
					"Empires normal bot lifecycle=team-confirmed index=%d team=%d frames=%d",
					engine->IndexOfEdict(m_pEdict), currentTeam, m_iNormalTeamFrames);
			}
			else if (engine->Time() >= m_flNormalTeamDeadline)
			{
				m_bNormalTeamCommandPending = false;
				m_bNormalTeamStartFailed = true;
				logger->Log(LogLevel::ERROR,
					"Empires normal bot lifecycle=team-failed index=%d requested_team=%d team_after=%d frames=%d",
					engine->IndexOfEdict(m_pEdict), m_iNormalTeam, currentTeam,
					m_iNormalTeamFrames);
			}
			return false;
		}

		int requestedTeam = m_iDesiredTeam;
		if (requestedTeam != EMP_TEAM_NF && requestedTeam != EMP_TEAM_BE)
		{
			requestedTeam = CBotGlobals::numPlayersOnTeam(EMP_TEAM_BE, false) <=
				CBotGlobals::numPlayersOnTeam(EMP_TEAM_NF, false) ?
				EMP_TEAM_BE : EMP_TEAM_NF;
		}

		char command[32];
		std::snprintf(command, sizeof(command), "jointeam %d\n", requestedTeam);
		logger->Log(LogLevel::INFO,
			"Empires normal bot lifecycle=team-requested index=%d requested_team=%d team_before=%d command=%s",
			engine->IndexOfEdict(m_pEdict), requestedTeam, currentTeam, command);

		// This is the public Empires command route. Only team selection is
		// performed here; class, loadout and spawn are separate stages.
		helpers->ClientCommand(m_pEdict, command);
		m_bNormalTeamCommandPending = true;
		m_iNormalTeam = requestedTeam;
		m_iNormalTeamFrames = 0;
		m_flNormalTeamDeadline = engine->Time() + 3.0f;
		return false;
	}

	if (!m_bNormalTeamStartComplete)
	{
		logger->Log(LogLevel::INFO,
			"Empires normal bot lifecycle=team-confirmed index=%d team=%d",
			engine->IndexOfEdict(m_pEdict), currentTeam);
	}
	m_bNormalTeamStartComplete = true;
	m_bNormalTeamCommandPending = false;
	if (m_eNormalSpawnState == EmpiresNormalSpawnState::FAILED)
		return false;
	if (m_eNormalSpawnState == EmpiresNormalSpawnState::COMPLETE)
	{
		// Spawn completion returns directly to the ordinary Empires task loop.
		// The old automatic fixed-route probe to waypoint 155 belonged to the
		// flagless emp_streetsoffire diagnostic and produced false path failures
		// on capture maps. Keep fixed-route coverage explicit through the
		// empires_normal_path_once command; never arm it from production startup.
		return CBot::startGame();
	}

	// A profile class outside Empires' verified infantry range means that no
	// class request was asked for. Keep the generic fallback unchanged for
	// that case; a controlled class test uses `rcbotd addbot 1 2`.
	if (m_iDesiredClass < EMP_CLASS_SCOUT || m_iDesiredClass > EMP_CLASS_ENGINEER)
		return CBot::startGame();
	if (m_bNormalClassStartComplete)
	{
		// The class stage is complete, but the normal bot must still follow the
		// public Empires spawn-selection and respawn-confirmation path.
		if (!m_bNormalLoadoutAccepted)
		{
			// Match the final two public client commands issued by the normal
			// loadout/spawn UI. This is particularly important on the tutorial
			// map: its tutorial spawn controller does not advance from the
			// loadout screen on emp_spawn_select alone.
			helpers->ClientCommand(m_pEdict, "acceptLoadout\n");
			helpers->ClientCommand(m_pEdict, "emp_map_spawn\n");
			m_bNormalLoadoutAccepted = true;
			logger->Log(LogLevel::INFO,
				"Empires normal bot lifecycle=loadout-accepted index=%d team=%d command_order=acceptLoadout,emp_map_spawn public-client-command=1",
				engine->IndexOfEdict(m_pEdict), currentTeam);
			return false;
		}
		if (m_eNormalSpawnState == EmpiresNormalSpawnState::IDLE)
		{
			int spawnId = -1;
			edict_t* spawnEdict = nullptr;
			if (!resolveBootstrapSpawnId(currentTeam, spawnId, spawnEdict))
			{
				logger->Log(LogLevel::ERROR,
					"Empires normal spawn result=FAILED reason=no-public-spawnpoint team=%d index=%d",
					currentTeam, engine->IndexOfEdict(m_pEdict));
				m_eNormalSpawnState = EmpiresNormalSpawnState::FAILED;
				return false;
			}

			char command[64];
			std::snprintf(command, sizeof(command), "emp_spawn_select %d\n", spawnId);
			logger->Log(LogLevel::INFO,
				"Empires normal spawn request index=%d userid=%d team=%d class=%d spawn_id=%d spawn_edict=%p command=%s",
				engine->IndexOfEdict(m_pEdict), engine->GetPlayerUserId(m_pEdict),
				currentTeam, m_iDesiredClass, spawnId, static_cast<void*>(spawnEdict), command);
			helpers->ClientCommand(m_pEdict, command);

			m_eNormalSpawnState = EmpiresNormalSpawnState::SPAWNPOINT_REQUESTED;
			m_iNormalSpawn = spawnId;
			m_pNormalSpawnSelectionEdict = spawnEdict;
			m_bNormalSpawnEligibilityLogged = false;
			m_iNormalSpawnFrames = 0;
			m_flNormalSpawnDeadline = engine->Time() + kNormalSpawnSelectionTimeout;
			m_iNormalSpawnExpectedIndex = engine->IndexOfEdict(m_pEdict);
			m_iNormalSpawnExpectedUserId = engine->GetPlayerUserId(m_pEdict);
			m_pNormalSpawnExpectedEdict = m_pEdict;
			m_bNormalSpawnLastLifeStateKnown = false;
			m_iNormalSpawnLastLifeState = -1;
			m_bNormalSpawnLastTimerKnown = false;
			m_flNormalSpawnLastTimer = 0.0f;
			return false;
		}

		return false;
	}

	int currentClass = -1;
	bool classDesired = false;
	bool classDesiredKnown = false;
	const char* classPropertyName = nullptr;
	const bool classReadable = readEmpiresClass(m_pEdict, currentClass, classDesired,
		classDesiredKnown, classPropertyName);
	if (!classReadable)
	{
		if (m_bNormalClassCommandPending && engine->Time() >= m_flNormalClassDeadline)
		{
			m_bNormalClassCommandPending = false;
			m_bNormalClassStartFailed = true;
			logger->Log(LogLevel::INFO,
				"Empires normal bot lifecycle=class-failed index=%d requested_class=%d reason=dynamic-class-readback-unavailable",
				engine->IndexOfEdict(m_pEdict), m_iNormalClass);
		}
		return false;
	}

	const int requestedClass = m_iDesiredClass;
	if (m_bNormalClassStartFailed)
		return false;

	if (m_bNormalClassCommandPending)
	{
		++m_iNormalClassFrames;
		// Empires exposes class_desired as an acknowledgement of the request,
		// not as proof that the player class has been applied.  The previous
		// normal path advanced with m_iEmpClass=5 and class_desired=1, then
		// selected a spawn before the server had an applied infantry class.
		const bool classAccepted = currentClass == m_iNormalClass;
		if (classAccepted && m_iNormalClassFrames >= 2)
		{
			m_bNormalClassStartComplete = true;
			m_bNormalClassCommandPending = false;
			int appliedSidearm = -1;
			int appliedPrimary = -1;
			int appliedGrenade = -1;
			int appliedEquipment = -1;
			bool sidearmDesired = false;
			bool primaryDesired = false;
			bool grenadeDesired = false;
			bool equipmentDesired = false;
			const bool sidearmKnown = readEmpiresIntProperty(
				m_pEdict, "m_iSidearmWeapon", appliedSidearm);
			const bool primaryKnown = readEmpiresIntProperty(
				m_pEdict, "m_iPrimaryWeapon", appliedPrimary);
			const bool grenadeKnown = readEmpiresIntProperty(
				m_pEdict, "m_iGrenade", appliedGrenade);
			const bool equipmentKnown = readEmpiresIntProperty(
				m_pEdict, "m_iEquipment", appliedEquipment);
			const bool sidearmDesiredKnown = readEmpiresBoolProperty(
				m_pEdict, "m_bSidearmWeaponDesired", sidearmDesired);
			const bool primaryDesiredKnown = readEmpiresBoolProperty(
				m_pEdict, "m_bPrimaryWeaponDesired", primaryDesired);
			const bool grenadeDesiredKnown = readEmpiresBoolProperty(
				m_pEdict, "m_bGrenadeDesired", grenadeDesired);
			const bool equipmentDesiredKnown = readEmpiresBoolProperty(
				m_pEdict, "m_bEquipmentDesired", equipmentDesired);
			logger->Log(LogLevel::INFO,
				"Empires normal bot lifecycle=class-confirmed index=%d team=%d requested_class=%d class_after=%d class_property=%s class_desired_known=%d class_desired=%d frames=%d dead=%d",
				engine->IndexOfEdict(m_pEdict), currentTeam, m_iNormalClass, currentClass,
				classPropertyName != nullptr ? classPropertyName : "unavailable",
				classDesiredKnown ? 1 : 0, classDesired ? 1 : 0,
				m_iNormalClassFrames, m_pPlayerInfo->IsDead() ? 1 : 0);
			logger->Log(LogLevel::INFO,
				"Empires normal bot lifecycle=class-loadout-readback index=%d class=%d class_desired=%d sidearm=%d/%d sidearm_desired=%d/%d primary=%d/%d primary_desired=%d/%d grenade=%d/%d grenade_desired=%d/%d equipment=%d/%d equipment_desired=%d/%d",
				engine->IndexOfEdict(m_pEdict), currentClass, classDesired ? 1 : 0,
				appliedSidearm, sidearmKnown ? 1 : 0, sidearmDesired ? 1 : 0,
				sidearmDesiredKnown ? 1 : 0, appliedPrimary, primaryKnown ? 1 : 0,
				primaryDesired ? 1 : 0, primaryDesiredKnown ? 1 : 0,
				appliedGrenade, grenadeKnown ? 1 : 0, grenadeDesired ? 1 : 0,
				grenadeDesiredKnown ? 1 : 0, appliedEquipment, equipmentKnown ? 1 : 0,
				equipmentDesired ? 1 : 0, equipmentDesiredKnown ? 1 : 0);
			// Do not enter generic bot thinking yet. Spawn is a separate, not-yet-
			// implemented Empires lifecycle stage.
			return false;
		}

		if (engine->Time() >= m_flNormalClassDeadline)
		{
			m_bNormalClassCommandPending = false;
			m_bNormalClassStartFailed = true;
			logger->Log(LogLevel::ERROR,
				"Empires normal bot lifecycle=class-failed index=%d team=%d requested_class=%d class_after=%d class_property=%s class_desired_known=%d class_desired=%d frames=%d dead=%d",
				engine->IndexOfEdict(m_pEdict), currentTeam, m_iNormalClass, currentClass,
				classPropertyName != nullptr ? classPropertyName : "unavailable",
				classDesiredKnown ? 1 : 0, classDesired ? 1 : 0,
				m_iNormalClassFrames, m_pPlayerInfo->IsDead() ? 1 : 0);
		}
		return false;
	}

	if (currentClass == requestedClass)
	{
		m_bNormalClassStartComplete = true;
		logger->Log(LogLevel::INFO,
			"Empires normal bot lifecycle=class-confirmed index=%d team=%d requested_class=%d class_after=%d class_property=%s class_desired_known=%d class_desired=%d frames=0 dead=%d",
			engine->IndexOfEdict(m_pEdict), currentTeam, requestedClass, currentClass,
			classPropertyName != nullptr ? classPropertyName : "unavailable",
			classDesiredKnown ? 1 : 0, classDesired ? 1 : 0,
			m_pPlayerInfo->IsDead() ? 1 : 0);
		return false;
	}

	char command[32];
	std::snprintf(command, sizeof(command), "changeclass %d\n", requestedClass);
	logger->Log(LogLevel::INFO,
		"Empires normal bot lifecycle=class-requested index=%d team=%d requested_class=%d class_before=%d class_property=%s class_desired_known=%d class_desired=%d command=%s dead=%d",
		engine->IndexOfEdict(m_pEdict), currentTeam, requestedClass, currentClass,
		classPropertyName != nullptr ? classPropertyName : "unavailable",
		classDesiredKnown ? 1 : 0, classDesired ? 1 : 0, command,
		m_pPlayerInfo->IsDead() ? 1 : 0);

	// This is the current public Empires command route verified against the
	// server's ValidateLoadout(true) path.  A desired class alone is not
	// sufficient: the four loadout commands must follow changeclass, with
	// changeequipment as the final validation/commit trigger.
	helpers->ClientCommand(m_pEdict, command);
	const EmpiresLoadoutSpec* loadout =
		findEmpiresNormalLoadout(currentTeam, requestedClass);
	if (loadout != nullptr && !m_bNormalLoadoutRequested)
	{
		char loadoutCommand[32];
		std::snprintf(loadoutCommand, sizeof(loadoutCommand),
			"changesidearm %d\n", loadout->sidearm);
		helpers->ClientCommand(m_pEdict, loadoutCommand);
		std::snprintf(loadoutCommand, sizeof(loadoutCommand),
			"changeprimary %d\n", loadout->primary);
		helpers->ClientCommand(m_pEdict, loadoutCommand);
		std::snprintf(loadoutCommand, sizeof(loadoutCommand),
			"changegrenade %d\n", loadout->grenade);
		helpers->ClientCommand(m_pEdict, loadoutCommand);
		std::snprintf(loadoutCommand, sizeof(loadoutCommand),
			"changeequipment %d\n", loadout->equipment);
		helpers->ClientCommand(m_pEdict, loadoutCommand);
		m_bNormalLoadoutRequested = true;
		if (requestedClass == EMP_CLASS_RIFLEMAN)
		{
			logger->Log(LogLevel::INFO,
				"Empires normal bot lifecycle=rifleman-loadout-requested index=%d team=%d tuple=%d/%d/%d/%d",
				engine->IndexOfEdict(m_pEdict), currentTeam,
				loadout->sidearm, loadout->primary, loadout->grenade,
				loadout->equipment);
		}
		else
		{
			logger->Log(LogLevel::INFO,
				"Empires normal bot lifecycle=class-loadout-requested index=%d team=%d class=%d tuple=%d/%d/%d/%d",
				engine->IndexOfEdict(m_pEdict), currentTeam, requestedClass,
				loadout->sidearm, loadout->primary, loadout->grenade,
				loadout->equipment);
		}
	}
	m_bNormalClassCommandPending = true;
	m_iNormalClass = requestedClass;
	m_iNormalClassFrames = 0;
	m_flNormalClassDeadline = engine->Time() + 3.0f;
	return false;
}

void CBotEmpires::freeMapMemory()
{
	// Entity indices are per map.
	m_bEmpiresNavGoalActive = false;
	m_flEmpiresNavGoalHoldUntil = 0.0f;
	m_flEmpiresNavGoalNextPick = 0.0f;
	m_flVoidNextCheck = 0.0f;
	m_flVoidSince = -1.0f;
	m_bVoidKilled = false;
	for (EmpiresNavGoalAvoid& slot : m_EmpiresNavGoalAvoid)
		slot = EmpiresNavGoalAvoid();
	RCBotLocalYielding::Reset();
	endEmpiresRecoveryEpisode("MAP_LIFECYCLE");
	g_empireSquadRuntimeShadow.Reset();
	if (isEmpiresScoutBinocularActionActive())
		failEmpiresScoutBinocularAction("map-shutdown-before-action-complete");
	clearEmpiresEnemyLosFixture("map-shutdown");
	clearEmpiresScoutPriorityFixture("map-shutdown");
	clearEmpiresSquadGoals();
	const bool wasAttached = isControlledTestOnly() || m_pEdict != nullptr;
	const int previousSlot = m_iControllerProbeSlot;
	const int previousIndex = m_iControllerProbeClientIndex;
	const char* previousLifecycle = controllerProbeLifecycleName();
	CBot::freeMapMemory();
	for (int i = 0; i < RCBOT_MAXPLAYERS; ++i)
	{
		m_iLastEmpiresPlayerObservation[i] =
			static_cast<int>(EmpiresPlayerObservation::UNKNOWN);
		m_pLastEmpiresPlayerEdict[i] = nullptr;
		m_iLastEmpiresPlayerUserId[i] = -1;
	}
	m_eNormalPathState = EmpiresNormalPathState::IDLE;
	m_bNormalPathAutoStarted = false;
	m_bNormalPathTerminalAfterStop = false;
	m_bNormalPathSearchStarted = false;
	m_iNormalPathSearchFrames = 0;
	m_bNormalDeathEventPending = false;
	m_NormalPathRoute.clear();
	m_pEmpiresApproachTarget = nullptr;
	m_bEmpiresApproachTaskIssued = false;
	m_bEmpiresLastEnemyObscured = false;
	m_bEmpiresLastEnemySearchActive = false;
	m_bEmpiresLastEnemySearchTaskLogged = false;
	m_iEmpiresSquadObserved = -1;
	m_iEmpiresSquadPositionObserved = -1;
	m_bEmpiresSquadLeaderObserved = false;
	m_bEmpiresSquadLeaderKnownObserved = false;
	m_flEmpiresNextSquadActionTime = 0.0f;
	m_iEmpiresSquadJoinAttempts = 0;
	resetEmpiresSquadGoalState();
	m_bEmpiresCombatEngaged = false;
	m_bEmpiresEngineerSupportTaskIssued = false;
	m_bEmpiresEngineerSupportNearby = false;
	m_bEmpiresEngineerResupplyTaskIssued = false;
	m_iEmpiresEngineerSupportBuildingIndex = -1;
	m_iEmpiresEngineerSupportWaypoint = -1;
	m_iEmpiresEngineerSupportRejectedBuildingIndex = -1;
	m_iEmpiresEngineerSupportRejectedWaypoint = -1;
	m_iEmpiresEngineerResupplyWaypoint = -1;
	m_vEmpiresEngineerSupportWaypointOrigin = vec3_origin;
	m_vEmpiresEngineerResupplyWaypointOrigin = vec3_origin;
	m_flEmpiresNextEngineerSupportTime = 0.0f;
	m_flEmpiresEngineerSupportRejectedUntil = 0.0f;
	m_flEmpiresNextEngineerResupplyTime = 0.0f;
	m_bEmpiresCoverScheduleActive = false;
	m_bEmpiresCaptureApproachIssued = false;
	m_bEmpiresCaptureHoldIssued = false;
	m_iEmpiresCaptureApproachPoint = -1;
	m_iEmpiresCaptureRouteWaypoint = -1;
	m_iEmpiresCaptureApproachRetries = 0;
	m_captureInterruptionLatch.reset();
	clearEmpiresStartAnchorRecovery();
	m_flEmpiresCaptureRouteIssuedTime = -1.0f;
	m_iEmpiresCaptureRejectedPoint = -1;
	m_vEmpiresCaptureApproachOrigin = vec3_origin;
	m_vEmpiresCaptureApproachStartOrigin = vec3_origin;
	m_flEmpiresNextCaptureApproachTime = 0.0f;
	m_flEmpiresNextCaptureApproachProgressLogTime = 0.0f;
	m_flEmpiresCaptureHoldDeadline = 0.0f;
	m_flEmpiresCaptureRejectedUntil = 0.0f;
	m_flEmpiresNextLastKnownMineTime = 0.0f;
	m_flEmpiresNextVisibleMortarTime = 0.0f;
	m_eEmpiresMortarActionState = EmpiresMortarActionState::IDLE;
	m_iEmpiresMortarActionTeam = -1;
	m_iEmpiresMortarActionExpectedIndex = -1;
	m_iEmpiresMortarActionExpectedUserId = -1;
	m_pEmpiresMortarActionExpectedEdict = nullptr;
	m_pEmpiresMortarActionWeapon = nullptr;
	m_iEmpiresMortarActionShellBaseline = 0;
	m_iEmpiresMortarActionShellIndex = -1;
	m_iEmpiresMortarActionAmmoBefore = -1;
	m_iEmpiresMortarActionAmmoAfter = -1;
	m_iEmpiresMortarActionFrames = 0;
	m_bEmpiresMortarActionPostureLogged = false;
	m_bEmpiresMortarActionAttackLogged = false;
	m_bEmpiresMortarActionReleaseLogged = false;
	m_flEmpiresMortarActionDeadline = 0.0f;
	m_flEmpiresMortarActionShellDeadline = 0.0f;
	m_eEmpiresMineActionState = EmpiresMineActionState::IDLE;
	m_iEmpiresMineActionTeam = -1;
	m_iEmpiresMineActionExpectedIndex = -1;
	m_iEmpiresMineActionExpectedUserId = -1;
	m_pEmpiresMineActionExpectedEdict = nullptr;
	m_pEmpiresMineActionWeapon = nullptr;
	m_iEmpiresMineActionProjectileBaseline = 0;
	m_iEmpiresMineActionProjectileIndex = -1;
	m_iEmpiresMineActionAmmoBefore = -1;
	m_iEmpiresMineActionAmmoAfter = -1;
	m_iEmpiresMineActionFrames = 0;
	m_iEmpiresMineActionDeployObservations = 0;
	m_iEmpiresMineActionSettledFrames = 0;
	m_iEmpiresMineActionTriggerTargetUserId = -1;
	m_iEmpiresMineActionTriggerTargetIndex = -1;
	m_iEmpiresMineActionTriggerTargetHealthBefore = -1;
	m_iEmpiresMineActionTriggerTargetHealthAfter = -1;
	m_pEmpiresMineActionTriggerTargetEdict = nullptr;
	m_bEmpiresMineActionTriggerMode = false;
	m_flEmpiresMineActionTriggerDelaySeconds = 0.0f;
	m_bEmpiresMineActionTriggerPositionLogged = false;
	m_bEmpiresMineActionTriggerHoldLogged = false;
	m_bEmpiresMineActionTriggerImpactLogged = false;
	m_bEmpiresMineActionTriggerNaturalDeathLogged = false;
	m_bEmpiresMineActionAttackLogged = false;
	m_bEmpiresMineActionReleaseLogged = false;
	m_bEmpiresMineActionDeployLogged = false;
	m_iEmpiresMineActionActivationLast = -1;
	m_bEmpiresMineActionActivationUnavailableLogged = false;
	m_bEmpiresMineActionActivationLogged = false;
	m_flEmpiresMineActionDeployedTime = 0.0f;
	m_flEmpiresMineActionDeadline = 0.0f;
	m_flEmpiresMineActionProjectileDeadline = 0.0f;
	m_eEmpiresEngineerRepairActionState = EmpiresEngineerRepairActionState::IDLE;
	m_iEmpiresEngineerRepairActionTeam = -1;
	m_iEmpiresEngineerRepairActionExpectedIndex = -1;
	m_iEmpiresEngineerRepairActionExpectedUserId = -1;
	m_pEmpiresEngineerRepairActionExpectedEdict = nullptr;
	m_pEmpiresEngineerRepairActionBuilding = nullptr;
	m_iEmpiresEngineerRepairActionBuildingIndex = -1;
	m_iEmpiresEngineerRepairActionHealthBefore = -1;
	m_iEmpiresEngineerRepairActionHealthLast = -1;
	m_iEmpiresEngineerRepairActionMaxHealth = -1;
	m_iEmpiresEngineerRepairActionFrames = 0;
	m_flEmpiresEngineerRepairActionDeadline = 0.0f;
	m_bEmpiresEngineerRepairActionRequireFullHealth = false;
	m_bEmpiresEngineerRepairActionProgressLogged = false;
	m_eEmpiresEngineerBuildSelectionState = EmpiresEngineerBuildSelectionState::IDLE;
	m_iEmpiresEngineerBuildSelectionTeam = -1;
	m_iEmpiresEngineerBuildSelectionExpectedIndex = -1;
	m_iEmpiresEngineerBuildSelectionExpectedUserId = -1;
	m_pEmpiresEngineerBuildSelectionExpectedEdict = nullptr;
	m_pEmpiresEngineerBuildSelectionWeapon = nullptr;
	m_iEmpiresEngineerBuildSelectionFrames = 0;
	m_iEmpiresEngineerBuildSelectionBaselineItem = -1;
	m_iEmpiresEngineerBuildSelectionLastItem = -1;
	m_iEmpiresEngineerBuildSelectionBaselineState = -1;
	m_iEmpiresEngineerBuildSelectionLastState = -1;
	m_bEmpiresEngineerBuildSelectionItemKnown = false;
	m_bEmpiresEngineerBuildSelectionStateKnown = false;
	m_bEmpiresEngineerBuildSelectionInputRequested = false;
	m_bEmpiresEngineerBuildSelectionInputSubmitted = false;
	m_bEmpiresEngineerBuildSelectionBaselineLogged = false;
	m_bEmpiresEngineerBuildSelectionTransitionLogged = false;
	m_flEmpiresEngineerBuildSelectionDeadline = 0.0f;
	m_eEmpiresEngineerBuildActionState = EmpiresEngineerBuildActionState::IDLE;
	m_iEmpiresEngineerBuildActionTeam = -1;
	m_iEmpiresEngineerBuildActionExpectedIndex = -1;
	m_iEmpiresEngineerBuildActionExpectedUserId = -1;
	m_pEmpiresEngineerBuildActionExpectedEdict = nullptr;
	m_iEmpiresEngineerBuildActionTargetWaypoint = -1;
	m_iEmpiresEngineerBuildActionBaselineTurrets = 0;
	m_iEmpiresEngineerBuildActionBaselineMaxIndex = -1;
	m_iEmpiresEngineerBuildActionResultIndex = -1;
	m_iEmpiresEngineerBuildActionFrames = 0;
	m_bEmpiresEngineerBuildActionCommandRequested = false;
	m_bEmpiresEngineerBuildActionCompletionCommandRequested = false;
	m_bEmpiresEngineerBuildActionAttackSubmitted = false;
	m_bEmpiresEngineerBuildActionBaselineLogged = false;
	m_bEmpiresEngineerBuildActionSelectionLogged = false;
	m_bEmpiresEngineerBuildActionResultObservedLogged = false;
	m_bEmpiresEngineerBuildActionRequireFullHealth = false;
	m_vEmpiresEngineerBuildActionTarget = vec3_origin;
	m_flEmpiresEngineerBuildActionDeadline = 0.0f;
	m_eEmpiresEngineerResupplyActionState =
		EmpiresEngineerResupplyActionState::IDLE;
	m_iEmpiresEngineerResupplyActionTeam = -1;
	m_iEmpiresEngineerResupplyActionExpectedIndex = -1;
	m_iEmpiresEngineerResupplyActionExpectedUserId = -1;
	m_pEmpiresEngineerResupplyActionExpectedEdict = nullptr;
	m_pEmpiresEngineerResupplyActionSource = nullptr;
	m_iEmpiresEngineerResupplyActionSourceIndex = -1;
	m_vEmpiresEngineerResupplyActionSourceOrigin = vec3_origin;
	m_iEmpiresEngineerResupplyActionAmmoType = -1;
	m_iEmpiresEngineerResupplyActionAmmoBefore = -1;
	m_iEmpiresEngineerResupplyActionAmmoLast = -1;
	m_iEmpiresEngineerResupplyActionClipBefore = -1;
	m_iEmpiresEngineerResupplyActionClipLast = -1;
	m_iEmpiresEngineerResupplyActionFrames = 0;
		m_bEmpiresEngineerResupplyActionUseSubmitted = false;
		m_bEmpiresEngineerResupplyActionUseInputLogged = false;
		m_bEmpiresEngineerResupplyActionUsePreRunLogged = false;
		m_bEmpiresEngineerResupplyActionUseProcessedLogged = false;
	m_bEmpiresEngineerResupplyActionUseTraceBlockedLogged = false;
	m_bEmpiresEngineerResupplyActionBaselineLogged = false;
	m_bEmpiresEngineerResupplyActionDepletionAttackSubmitted = false;
	m_bEmpiresEngineerResupplyActionDepletionReadinessLogged = false;
	m_bEmpiresEngineerResupplyActionDepletionReloadSubmitted = false;
	m_bEmpiresEngineerResupplyActionDepletionComplete = false;
	m_bEmpiresEngineerResupplyActionDepletionOnly = false;
	m_flEmpiresEngineerResupplyActionDeadline = 0.0f;
	m_bEmpiresMortarAimRequested = false;
	m_iEmpiresMortarAimTargetWaypoint = -1;
	m_vEmpiresMortarAimTarget = Vector(0.0f, 0.0f, 0.0f);
	m_qEmpiresMortarAimAngles = QAngle(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLaunchOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_bEmpiresMortarAimComputed = false;
	m_bEmpiresMortarAimViewApplied = false;
	m_iEmpiresMortarAimTrajectorySamples = 0;
	m_flEmpiresMortarAimLastSampleTime = 0.0f;
	m_vEmpiresMortarAimFirstShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	m_vEmpiresMortarAimLastShellOrigin = Vector(0.0f, 0.0f, 0.0f);
	// Orders and timers of the old map: entity indices and gpGlobals->curtime
	// deadlines mean nothing on the next map. Initial values as in the header.
	m_vehicle = EmpiresVehicleBrain();
	m_iCommanderEntryStage = 0;
	m_iCommanderEntryUserId = -1;
	m_hCommanderEntryVehicle = nullptr;
	m_fCommanderEntryDeadline = 0.0f;
	m_vCommanderEntrySpot = Vector(0.0f, 0.0f, 0.0f);
	m_fCommanderEntryNextReplan = 0.0f;
	m_iCommanderBuildStage = 0;
	m_hCommanderBuildTarget = nullptr;
	m_fCommanderBuildDeadline = 0.0f;
	m_vCommanderBuildDriveGoal = Vector(0.0f, 0.0f, 0.0f);
	m_bCommanderBuildTurret = false;
	m_bCommanderTurretStarted = false;
	m_bCommanderHunt = false;
	m_fCommanderHuntZone = 0.0f;
	m_commanderEconomy = CommanderEconomyState();
	m_fEmpiresGuardNextCheck = 0.0f;
	m_iEmpiresGuardLoggedTarget = -1;
	m_iBuildAssignStage = 0;
	m_iBuildAssignAttempts = 0;
	m_hBuildAssignBuilding = nullptr;
	m_fBuildAssignStart = 0.0f;
	m_fBuildAssignNextTry = 0.0f;
	m_vBuildAssignSpot = Vector(0.0f, 0.0f, 0.0f);
	m_fBuildAssignNextLog = 0.0f;
	m_vEmpiresKitAim = Vector(0.0f, 0.0f, 0.0f);
	m_iReviveTarget = 0;
	m_flReviveUntil = 0.0f;
	m_flReviveNextNav = 0.0f;
	m_flReviveSent = 0.0f;
	m_flReviveNextSearch = 0.0f;
	m_flReviveNextLog = 0.0f;
	m_flReviveDeathTime = 0.0f;
	m_bReviveHoldLogged = false;
	m_bReviveHoldActive = false;
	m_iTurretRefinery = 0;
	m_iTurretAttempt = 0;
	m_iTurretBuilding = 0;
	m_iTurretAssignRejects = 0;
	m_bTurretBuildArmed = false;
	m_vTurretSpot = vec3_origin;
	m_flTurretYaw = 0.0f;
	m_flTurretUntil = 0.0f;
	m_flTurretNextNav = 0.0f;
	m_flTurretNextSearch = 0.0f;
	m_flTurretNextLog = 0.0f;
	m_pEmpiresLastSpotTarget = nullptr;
	m_flEmpiresNextSpotTime = 0.0f;
	m_iEmpiresNavGoalType = -1;
	m_iEmpiresNavGoalEntity = -1;
	m_vEmpiresNavGoalOrigin = Vector(0, 0, 0);
	m_flEmpiresNavGoalStart = 0.0f;
	m_flEmpiresNavGoalDeadline = 0.0f;
	m_bEmpiresNavGoalArrivedAtCapture = false;
	m_iEmpiresNavGoalRetries = 0;
	m_iEmpiresNavGoalAttempts = 0;
	m_iEmpiresDefendHoldEntity = -1;
	m_vEmpiresDefendHoldOrigin = vec3_origin;
	m_flEmpiresDefendMineNext = 0.0f;
	m_flEmpiresDefendMineAimUntil = 0.0f;
	m_bEmpiresDefendMineFullLogged = false;
	m_iEmpiresLastHealth = 0;
	m_iEmpiresTurretThreat = 0;
	m_vEmpiresTurretThreatOrigin = vec3_origin;
	m_flEmpiresTurretThreatRange = 0.0f;
	m_flEmpiresTurretThreatUntil = 0.0f;
	m_bEmpiresTurretRetreatPending = false;
	m_bEmpiresTurretRetreating = false;
	m_vEmpiresTurretRetreatPoint = vec3_origin;
	m_flEmpiresTurretRetreatDeadline = 0.0f;
	m_hEmpiresTurretHitBy = nullptr;
	m_hEmpiresTurretFireTarget = nullptr;
	m_hEmpiresTurretRocketTarget = nullptr;
	m_hEmpiresTurretRocketWeapon = nullptr;
	m_qEmpiresTurretRocketAim = QAngle(0.0f, 0.0f, 0.0f);
	m_flEmpiresTurretRocketUntil = 0.0f;
	m_iEmpiresTurretRocketAmmoType = -1;
	m_iEmpiresTurretRocketAmmo = 0;
	m_iEmpiresTurretRocketHealth = 0;
	m_bEmpiresTurretRocketFired = false;
	m_bEmpiresTurretRocketApproach = false;
	m_iEmpiresDoorButton = -1;
	m_iEmpiresDoorButtonDoor = -1;
	m_vEmpiresDoorButtonOrigin = Vector(0, 0, 0);
	m_flEmpiresDoorButtonUntil = 0.0f;
	m_flEmpiresDoorButtonNext = 0.0f;
	m_flEmpiresDoorButtonLastUse = 0.0f;
	m_bEmpiresDoorButtonMoving = false;
	m_flEmpiresTurretMortarNext = 0.0f;
	m_flEmpiresTurretFireMoveUntil = 0.0f;
	m_flEmpiresTurretFireMoveNext = 0.0f;
	m_iEmpiresTurretMortarFails = 0;
	m_iEmpiresTurretMortarShots = 0;
	m_vEmpiresTurretLastFireSpot = vec3_origin;
	m_vEmpiresTurretFailedMortarSpot = vec3_origin;
	m_vEmpiresTurretSettleOrigin = vec3_origin;
	m_flEmpiresTurretSettledSince = 0.0f;
	m_bEmpiresTurretMortarWasActive = false;
	m_bEmpiresTurretOutOfReachLogged = false;
	resetControlledTestState();

	if (wasAttached)
	{
		logger->Log(LogLevel::INFO,
			"Empires controlled test lifecycle=INACTIVE cleared slot=%d index=%d previous=%s in_use=0",
			previousSlot, previousIndex, previousLifecycle);
	}

	CBots::controlledTestLifecycleMayEnd();
}
