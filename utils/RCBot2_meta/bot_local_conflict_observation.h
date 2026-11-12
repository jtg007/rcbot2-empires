#ifndef __RCBOT_LOCAL_CONFLICT_OBSERVATION_H__
#define __RCBOT_LOCAL_CONFLICT_OBSERVATION_H__

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef RCBOT_LOCAL_CONFLICT_TEST_VECTOR
#include "bot_local_conflict_test_vector.h"
#else
#include "vector.h"
#endif

namespace RCBotLocalAvoidance
{
	enum class ConflictType : std::uint8_t
	{
		HEAD_ON = 0,
		FOLLOW_BLOCK,
		CROSSING,
		STATIONARY_BLOCK,
		CHOKE_CONFLICT,
		PRECISION_TRAVERSAL_CONFLICT,
		WORLD_PLUS_ALLY_BLOCK,
		UNKNOWN
	};

	const char* ConflictTypeName(ConflictType type);

	enum class Lifecycle : std::uint8_t
	{
		NONE = 0,
		BEGIN,
		CONTINUE,
		RESOLVED,
		ESCALATED_TO_NO_PROGRESS,
		ENDED_BY_LIFECYCLE
	};

	const char* LifecycleName(Lifecycle lifecycle);

	enum class CorrelationSignal : std::uint8_t
	{
		NAV_PROGRESS = 0,
		NAV_NO_PROGRESS,
		PATH_STUCK,
		STUCK_HEURISTIC
	};

	const char* CorrelationSignalName(CorrelationSignal signal);

	// One sampled, value-only view of a possible local ally conflict. It is
	// deliberately independent of CBot, entities, routes and movement writers.
	struct LocalBotConflictObservation
	{
		int observerBot = -1;
		int otherBot = -1;
		int team = -1;
		int otherTeam = -1;
		double timestamp = 0.0;
		bool observerValid = false;
		bool otherValid = false;
		bool observerAlive = false;
		bool otherAlive = false;

		Vector observerPosition;
		Vector otherPosition;
		Vector observerDesiredDirection;
		Vector otherDesiredDirection;
		Vector observerVelocity;
		Vector otherVelocity;
		float observerDesiredForward = 0.0f;
		float observerDesiredSide = 0.0f;
		float observerDesiredUp = 0.0f;

		float distance = 0.0f;
		float closingSpeed = 0.0f;
		float relativeHeading = 0.0f;
		float lateralCorridorDistance = 0.0f;
		float corridorRadius = 0.0f;
		bool sameOrOpposedMotionKnown = false;
		bool sameMotion = false;
		bool opposedMotion = false;
		bool hullOverlap = false;
		bool observerWantsMovement = false;
		bool otherWantsMovement = false;
		bool otherIntersectsCorridor = false;
		bool observerProgressStalled = false;

		bool observerNavigationActive = false;
		bool otherNavigationActive = false;
		std::uint64_t observerIntentId = 0;
		std::uint64_t otherIntentId = 0;
		std::uint64_t observerAttemptId = 0;
		std::uint64_t otherAttemptId = 0;
		std::string observerProvider;
		std::string otherProvider;
		int observerCurrentSegment = -1;
		int observerNextSegment = -1;
		int otherCurrentSegment = -1;
		int otherNextSegment = -1;

		bool observerPrecisionContext = false;
		bool otherPrecisionContext = false;
		std::uint32_t observerPrecisionFlags = 0;
		std::uint32_t otherPrecisionFlags = 0;

		bool sameSquadKnown = false;
		bool sameSquad = false;
		int observerSquadId = -1;
		int otherSquadId = -1;
		int observerFormationSlot = -1;
		int otherFormationSlot = -1;
		bool spatialMovementActive = false;

		bool worldTraceKnown = false;
		bool worldClearLeft = false;
		bool worldClearRight = false;
		bool worldBlocking = false;
		bool allyThreat = false;

		float observerRemainingDistance = -1.0f;
		float otherRemainingDistance = -1.0f;
	};

	bool IsCandidate(const LocalBotConflictObservation& observation);
	ConflictType Classify(const LocalBotConflictObservation& observation);
	std::uint32_t ContextKey(const LocalBotConflictObservation& observation);

	struct LocalConflictEpisode
	{
		bool active = false;
		std::uint64_t episodeId = 0;
		bool stuckCorrelated = false;
		std::uint8_t correlationMask = 0;
		int lowBot = -1;
		int highBot = -1;
		int team = -1;
		ConflictType type = ConflictType::UNKNOWN;
		std::uint32_t context = 0;
		Lifecycle lifecycle = Lifecycle::NONE;
		double startTime = 0.0;
		double lastSampleTime = 0.0;
		double endTime = 0.0;
		std::uint64_t lastScanGeneration = 0;
		float duration = 0.0f;
		float minDistance = 0.0f;
		float maxClosingSpeed = 0.0f;
		std::uint32_t samples = 0;
		float observerWantedDuration = 0.0f;
		float progressBefore = -1.0f;
		float progressDuring = -1.0f;
		float progressAfter = -1.0f;
		LocalBotConflictObservation lastObservation;
	};

	struct EpisodeUpdate
	{
		Lifecycle lifecycle = Lifecycle::NONE;
		LocalConflictEpisode episode;
	};

	// Session-scoped and bounded. Completed episodes remain available for exact
	// intent/attempt correlation until they are evicted by the fixed cap.
	class LocalConflictEpisodeTracker
	{
	public:
		static constexpr std::size_t kMaxEpisodes = 128;

		std::vector<EpisodeUpdate> Observe(
			const LocalBotConflictObservation& observation,
			double now, std::uint64_t scanGeneration);
		std::vector<EpisodeUpdate> EndUnseen(
			std::uint64_t scanGeneration, double now);
		std::vector<EpisodeUpdate> EndAll(double now, Lifecycle lifecycle);
		std::vector<EpisodeUpdate> Correlate(
			int botId, std::uint64_t intentId, std::uint64_t attemptId,
			double now,
			CorrelationSignal signal = CorrelationSignal::PATH_STUCK);
		void Reset();
		std::size_t Size() const { return m_count; }
		std::size_t ActiveCount() const;
		std::size_t Evictions() const { return m_evictions; }

	private:
		int findActive(int lowBot, int highBot, ConflictType type,
			std::uint32_t context) const;
		int findFreeOrOldest();
		static void finish(LocalConflictEpisode& episode, double now,
			Lifecycle lifecycle);
		static bool exactNavigationMatch(
			const LocalConflictEpisode& episode, std::uint64_t intentId,
			std::uint64_t attemptId);

		std::array<LocalConflictEpisode, kMaxEpisodes> m_episodes{};
		std::size_t m_count = 0;
		std::size_t m_evictions = 0;
		std::uint64_t m_nextEpisodeId = 0;
	};
}

#endif // __RCBOT_LOCAL_CONFLICT_OBSERVATION_H__
