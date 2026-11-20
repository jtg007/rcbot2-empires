// Development-only bootstrap/controller-probe harness (console commands
// empires_bootstrap_*). Split out of bot_empires.cpp; no gameplay path uses it.
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

#include "bot_empires_internal.h"

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
	snprintf(command, sizeof(command), "jointeam %d\n", requestedTeam);
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
