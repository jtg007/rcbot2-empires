#ifndef __RCBOT_NAVIGATION_ATTEMPT_H__
#define __RCBOT_NAVIGATION_ATTEMPT_H__

#include <cstddef>
#include <cstdint>

#include "bot_recovery_episode.h"
#include "vector.h"

namespace RCBotNavigation
{
	enum class Provider : std::uint8_t
	{
		UNKNOWN = 0,
		WAYPOINT,
		NAVMESH,
		LOCAL_EXPLORER,
		BUILDING_RELATIVE
	};

	enum class Result : std::uint8_t
	{
		NONE = 0,
		COMPLETE,
		CANCELLED,
		PATH_FAILED,
		PATH_STUCK,
		UNREACHABLE
	};

	// Provider selection is deliberately a policy, not a score.  AUTO is the
	// conservative production order: Waypoint first, then NavMesh and finally
	// the bounded Local Explorer, only after structural/planning failures.
	enum class NavigationPolicy : std::uint8_t
	{
		AUTO = 0,
		WAYPOINT_ONLY,
		NAVMESH_ONLY
	};

	enum class FailureClass : std::uint8_t
	{
		NONE = 0,
		STRUCTURAL_PLANNING,
		MOVEMENT,
		CANCEL_INVALID
	};

	enum class FailureReason : std::uint8_t
	{
		NONE = 0,
		NO_START_ANCHOR,
		NO_GOAL_ANCHOR,
		NO_REACHABLE_ANCHOR,
		START_AREA_NOT_FOUND,
		GOAL_AREA_NOT_FOUND,
		NO_PATH,
		PROVIDER_UNAVAILABLE,
		PATH_FAILED,
		PATH_STUCK,
		REPEATED_LOCOMOTION_FAILURE,
		CANCELLED,
		DEATH,
		TEAM_CHANGED,
		INVALID_INTENT,
		EXPLICIT_HARD_STOP,
		STALE_OUTPUT
	};

	enum class FailureDisposition : std::uint8_t
	{
		REJECTED = 0,
		TERMINAL,
		FALLBACK_STARTED
	};

	struct ProviderFailureRecord
	{
		FailureClass classification = FailureClass::NONE;
		FailureReason reason = FailureReason::NONE;
		Result result = Result::NONE;
		bool recorded = false;
	};

	constexpr std::size_t kTrackedProviderCount = 3;

	inline bool IsTrackedProvider(const Provider provider)
	{
		return provider == Provider::WAYPOINT || provider == Provider::NAVMESH ||
			provider == Provider::LOCAL_EXPLORER;
	}

	inline std::uint8_t ProviderBit(const Provider provider)
	{
		return provider == Provider::WAYPOINT ? 1u :
			provider == Provider::NAVMESH ? 2u :
			provider == Provider::LOCAL_EXPLORER ? 4u : 0u;
	}

	inline std::size_t ProviderFailureIndex(const Provider provider)
	{
		return provider == Provider::NAVMESH ? 1u :
			provider == Provider::LOCAL_EXPLORER ? 2u : 0u;
	}

	// A navigation intent is the small provider-neutral contract between a
	// higher-level task and the movement owner.  It deliberately contains no
	// pathfinding state; the existing NavigationAttempt remains responsible for
	// progress, timeout and retry identity.
	enum class IntentState : std::uint8_t
	{
		IDLE = 0,
		REQUESTED,
		PLANNING,
		MOVING,
		COMPLETE,
		FAILED,
		UNREACHABLE,
		CANCELLED
	};

	enum class IntentOwner : std::uint8_t
	{
		NONE = 0,
		WAYPOINT,
		NAVMESH,
		LOCAL_EXPLORER
	};

	struct NavigationIntent
	{
		bool active = false;
		std::uint64_t id = 0;
		NavigationPolicy policy = NavigationPolicy::AUTO;
		Provider provider = Provider::UNKNOWN;
		IntentOwner owner = IntentOwner::NONE;
		IntentState state = IntentState::IDLE;
		int goalId = -1;
		Vector goalOrigin;
		std::uint64_t attemptId = 0;
		Result terminalResult = Result::NONE;
		// Observation-only higher-level identity. Navigation never reads this
		// field to select, cancel, block or replace movement.
		RCBotRecovery::RecoveryEpisodeIdentity recoveryEpisode;
		// Fixed-size bookkeeping keeps arbitration O(1) and makes a provider
		// retry/ping-pong impossible by construction.
		std::uint8_t providersAttempted = 0;
		ProviderFailureRecord failureReasonPerProvider[kTrackedProviderCount];
		std::uint8_t providerSwitchCount = 0;

		void MarkProviderAttempted(const Provider provider)
		{
			providersAttempted |= ProviderBit(provider);
		}

		bool HasAttemptedProvider(const Provider provider) const
		{
			return (providersAttempted & ProviderBit(provider)) != 0;
		}

		void RecordProviderFailure(const Provider provider,
			const FailureClass classification, const FailureReason reason,
			const Result result)
		{
			if (!IsTrackedProvider(provider))
				return;
			ProviderFailureRecord& record =
				failureReasonPerProvider[ProviderFailureIndex(provider)];
			record.classification = classification;
			record.reason = reason;
			record.result = result;
			record.recorded = true;
		}

		void Reset()
		{
			active = false;
			id = 0;
			policy = NavigationPolicy::AUTO;
			provider = Provider::UNKNOWN;
			owner = IntentOwner::NONE;
			state = IntentState::IDLE;
			goalId = -1;
			goalOrigin = Vector();
			recoveryEpisode.Reset();
			attemptId = 0;
			terminalResult = Result::NONE;
			providersAttempted = 0;
			for (ProviderFailureRecord& record : failureReasonPerProvider)
				record = ProviderFailureRecord{};
			providerSwitchCount = 0;
		}
	};

	enum class Observation : std::uint8_t
	{
		NONE = 0,
		PROGRESS,
		NO_PROGRESS
	};

	struct SegmentKey
	{
		int currentWaypoint = -1;
		int nextWaypoint = -1;

		bool operator==(const SegmentKey& other) const
		{
			return currentWaypoint == other.currentWaypoint &&
				nextWaypoint == other.nextWaypoint;
		}

		bool operator!=(const SegmentKey& other) const
		{
			return !(*this == other);
		}
	};

	struct AttemptKey
	{
		Provider provider = Provider::UNKNOWN;
		int goalId = -1;
		std::uint64_t goalSignature = 0;
		SegmentKey segment;

		bool operator==(const AttemptKey& other) const
		{
			return provider == other.provider && goalId == other.goalId &&
				goalSignature == other.goalSignature && segment == other.segment;
		}

		bool operator!=(const AttemptKey& other) const
		{
			return !(*this == other);
		}
	};

	// A deliberately small lifecycle/progress object.  It owns no bot, task,
	// navigator or telemetry references and can therefore be reused by future
	// navigation providers without creating a second movement controller.
	class NavigationAttempt
	{
	public:
		static constexpr float kMinPositionProgress = 8.0f;
		static constexpr float kMinDistanceProgress = 4.0f;
		static constexpr float kNoProgressTimeout = 3.0f;
		static constexpr float kFailureCooldown = 15.0f;

		bool Start(int botIndex, int botUserId, int team,
			const AttemptKey& key, const Vector& startPosition, float startTime,
			float initialRemainingDistance,
			float noProgressTimeout = kNoProgressTimeout);

		bool CanStart(const AttemptKey& key, float now) const;
		bool IsCooldownBlocked(const AttemptKey& key, float now) const;
		bool MatchesGoal(Provider provider, int goalId,
			std::uint64_t goalSignature) const;

		Observation Observe(float now, const Vector& position,
			float remainingDistance, const SegmentKey& segment);
		// Intentional local yielding is a bounded pause, not progress. Advance
		// the watchdog clock without changing progress counters or route state.
		void PauseNoProgress(float now);
		bool End(Result result, float endTime);

		void Reset();
		void ClearCooldown() { m_hasCooldown = false; }

		bool IsActive() const { return m_active; }
		std::uint64_t GetAttemptId() const { return m_attemptId; }
		int GetBotIndex() const { return m_botIndex; }
		int GetBotUserId() const { return m_botUserId; }
		int GetTeam() const { return m_team; }
		const AttemptKey& GetKey() const { return m_key; }
		const Vector& GetStartPosition() const { return m_startPosition; }
		const Vector& GetLastProgressPosition() const { return m_lastProgressPosition; }
		float GetStartTime() const { return m_startTime; }
		float GetLastProgressTime() const { return m_lastProgressTime; }
		float GetEndTime() const { return m_endTime; }
		float GetBestRemainingDistance() const { return m_bestRemainingDistance; }
		float GetLastRemainingDistance() const { return m_lastRemainingDistance; }
		float GetNoProgressTimeout() const { return m_noProgressTimeout; }
		float GetCooldownUntil() const { return m_cooldownUntil; }
		std::uint32_t GetProgressCount() const { return m_progressCount; }
		std::uint32_t GetFailureCount() const { return m_failureCount; }
		Result GetResult() const { return m_result; }

		static std::uint64_t MakeGoalSignature(int goalId,
			const Vector& goalPosition);

	private:
		static bool SameRetryIdentity(const AttemptKey& left,
			const AttemptKey& right);

		bool m_active = false;
		std::uint64_t m_attemptId = 0;
		int m_botIndex = -1;
		int m_botUserId = -1;
		int m_team = -1;
		AttemptKey m_key;
		Vector m_startPosition;
		Vector m_lastProgressPosition;
		float m_startTime = 0.0f;
		float m_lastProgressTime = 0.0f;
		float m_endTime = 0.0f;
		float m_bestRemainingDistance = 0.0f;
		float m_lastRemainingDistance = 0.0f;
		float m_noProgressTimeout = kNoProgressTimeout;
		std::uint32_t m_progressCount = 0;
		std::uint32_t m_failureCount = 0;
		bool m_hasBestDistance = false;
		Result m_result = Result::NONE;

		bool m_hasCooldown = false;
		AttemptKey m_cooldownKey;
		float m_cooldownUntil = 0.0f;

		static std::uint64_t s_nextAttemptId;
	};
}

#endif // __RCBOT_NAVIGATION_ATTEMPT_H__
