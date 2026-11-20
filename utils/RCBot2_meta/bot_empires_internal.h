#pragma once
// Constants and pure helpers shared by bot_empires.cpp and
// bot_empires_controller_probe.cpp. Stateless only: each TU gets its own copy.
// Include after the other bot_empires.cpp includes.

namespace
{
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
}
