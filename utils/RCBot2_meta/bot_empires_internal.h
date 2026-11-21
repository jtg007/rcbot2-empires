#pragma once
// Constants and pure helpers shared by the bot_empires*.cpp files.
// Stateless only: each TU gets its own copy.
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

constexpr float kNormalSpawnSelectionTimeout = 4.0f;
constexpr float kNormalSpawnReadinessTimeout = 20.0f;
// Clients sit in LIFE_RESPAWNABLE for about three seconds before the attack
// edge confirms the spawn; keep that server-side settling interval.
constexpr float kNormalSpawnConfirmDelaySeconds = 3.0f;
constexpr float kNormalSpawnResultTimeout = 4.0f;
constexpr int kNormalPathMaximumFrames = 1536;
constexpr float kNormalPathMaximumSeconds = 45.0f;
constexpr int kNormalPathSearchMaximumFrames = 64;
constexpr int kNormalPathNoProgressFrames = 180;
constexpr float kNormalPathNoProgressEpsilon = 2.0f;
constexpr int kNormalPathStartWaypoint = 145;
constexpr int kNormalPathDestinationWaypoint = 155;
}
