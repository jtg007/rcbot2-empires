#include "bot_local_conflict_observation.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	constexpr float kLocalProximity = 128.0f;
	constexpr float kHeadingThreshold = 0.55f;
	constexpr float kStationarySpeed = 4.0f;
	constexpr double kCorrelationGraceSeconds = 1.0;

	float dot2d(const Vector& left, const Vector& right)
	{
		return left.x * right.x + left.y * right.y;
	}

	float cross2d(const Vector& left, const Vector& right)
	{
		return left.x * right.y - left.y * right.x;
	}

	float speed2d(const Vector& value)
	{
		return std::sqrt(value.x * value.x + value.y * value.y);
	}

	bool finite(const float value)
	{
		return std::isfinite(value);
	}

	bool hasKnownDirection(const Vector& value)
	{
		return finite(value.x) && finite(value.y) && speed2d(value) > 0.01f;
	}
}

namespace RCBotLocalAvoidance
{
	const char* ConflictTypeName(const ConflictType type)
	{
		switch (type)
		{
		case ConflictType::HEAD_ON: return "HEAD_ON";
		case ConflictType::FOLLOW_BLOCK: return "FOLLOW_BLOCK";
		case ConflictType::CROSSING: return "CROSSING";
		case ConflictType::STATIONARY_BLOCK: return "STATIONARY_BLOCK";
		case ConflictType::CHOKE_CONFLICT: return "CHOKE_CONFLICT";
		case ConflictType::PRECISION_TRAVERSAL_CONFLICT:
			return "PRECISION_TRAVERSAL_CONFLICT";
		case ConflictType::WORLD_PLUS_ALLY_BLOCK: return "WORLD_PLUS_ALLY_BLOCK";
		case ConflictType::UNKNOWN: return "UNKNOWN";
		}
		return "UNKNOWN";
	}

	const char* LifecycleName(const Lifecycle lifecycle)
	{
		switch (lifecycle)
		{
		case Lifecycle::BEGIN: return "BEGIN";
		case Lifecycle::CONTINUE: return "CONTINUE";
		case Lifecycle::RESOLVED: return "RESOLVED";
		case Lifecycle::ESCALATED_TO_NO_PROGRESS:
			return "ESCALATED_TO_NO_PROGRESS";
		case Lifecycle::ENDED_BY_LIFECYCLE: return "ENDED_BY_LIFECYCLE";
		case Lifecycle::NONE: return "NONE";
		}
		return "NONE";
	}

	const char* CorrelationSignalName(const CorrelationSignal signal)
	{
		switch (signal)
		{
		case CorrelationSignal::NAV_PROGRESS: return "NAV_PROGRESS";
		case CorrelationSignal::NAV_NO_PROGRESS: return "NAV_NO_PROGRESS";
		case CorrelationSignal::PATH_STUCK: return "PATH_STUCK";
		case CorrelationSignal::STUCK_HEURISTIC: return "STUCK_HEURISTIC";
		}
		return "UNKNOWN";
	}

	bool IsCandidate(const LocalBotConflictObservation& observation)
	{
		if (observation.observerBot < 0 || observation.otherBot < 0 ||
			observation.observerBot == observation.otherBot ||
			observation.team < 0 || observation.otherTeam != observation.team ||
			!observation.observerValid ||
			!observation.otherValid || !observation.observerAlive ||
			!observation.otherAlive || !observation.observerWantsMovement ||
			!observation.allyThreat || !observation.otherIntersectsCorridor)
			return false;
		if (observation.distance > kLocalProximity || observation.distance < 0.0f)
			return false;
		return true;
	}

	ConflictType Classify(const LocalBotConflictObservation& observation)
	{
		if (!IsCandidate(observation))
			return ConflictType::UNKNOWN;
		if (observation.observerPrecisionContext ||
			observation.otherPrecisionContext)
			return ConflictType::PRECISION_TRAVERSAL_CONFLICT;
		if (observation.worldBlocking && observation.allyThreat &&
			observation.worldTraceKnown && !observation.worldClearLeft &&
			!observation.worldClearRight)
			return ConflictType::CHOKE_CONFLICT;
		if (observation.worldBlocking && observation.allyThreat)
			return ConflictType::WORLD_PLUS_ALLY_BLOCK;

		const float observerSpeed = speed2d(observation.observerVelocity);
		const float otherSpeed = speed2d(observation.otherVelocity);
		if (!observation.otherWantsMovement || otherSpeed <= kStationarySpeed)
			return ConflictType::STATIONARY_BLOCK;

		if (observation.sameOrOpposedMotionKnown && observation.opposedMotion)
			return ConflictType::HEAD_ON;
		if (hasKnownDirection(observation.observerDesiredDirection) &&
			hasKnownDirection(observation.otherVelocity) &&
			dot2d(observation.observerDesiredDirection,
				observation.otherVelocity) > 0.0f &&
			otherSpeed + kStationarySpeed < observerSpeed &&
			observation.observerProgressStalled)
			return ConflictType::FOLLOW_BLOCK;
		if (hasKnownDirection(observation.observerDesiredDirection) &&
			hasKnownDirection(observation.otherVelocity) &&
			observation.hullOverlap && std::fabs(cross2d(
				observation.observerDesiredDirection,
				observation.otherVelocity)) > kHeadingThreshold)
			return ConflictType::CROSSING;
		return ConflictType::UNKNOWN;
	}

	std::uint32_t ContextKey(const LocalBotConflictObservation& observation)
	{
		// Context identity must remain canonical when the same pair is observed
		// from the opposite side. This is especially important once yielding
		// consumes the episode as a shared pair decision.
		const bool observerIsLow = observation.observerBot <= observation.otherBot;
		const std::uint32_t lowPrecisionFlags = observerIsLow ?
			observation.observerPrecisionFlags : observation.otherPrecisionFlags;
		const std::uint32_t highPrecisionFlags = observerIsLow ?
			observation.otherPrecisionFlags : observation.observerPrecisionFlags;
		std::uint32_t key = lowPrecisionFlags | (highPrecisionFlags << 8);
		if (observation.worldBlocking)
			key |= 1u << 16;
		if (observation.sameSquadKnown && observation.sameSquad)
			key |= 1u << 17;
		if (observation.spatialMovementActive)
			key |= 1u << 18;
		return key;
	}

	int LocalConflictEpisodeTracker::findActive(const int lowBot,
		const int highBot, const ConflictType type, const std::uint32_t context) const
	{
		for (std::size_t i = 0; i < m_count; ++i)
		{
			const LocalConflictEpisode& episode = m_episodes[i];
			if (episode.active && episode.lowBot == lowBot &&
				episode.highBot == highBot && episode.type == type &&
				episode.context == context)
				return static_cast<int>(i);
		}
		return -1;
	}

	int LocalConflictEpisodeTracker::findFreeOrOldest()
	{
		if (m_count < kMaxEpisodes)
			return static_cast<int>(m_count++);
		int oldest = -1;
		double oldestTime = (std::numeric_limits<double>::max)();
		for (std::size_t i = 0; i < m_count; ++i)
		{
			if (m_episodes[i].active)
				continue;
			if (m_episodes[i].endTime < oldestTime)
			{
				oldestTime = m_episodes[i].endTime;
				oldest = static_cast<int>(i);
			}
		}
		if (oldest >= 0)
		{
			++m_evictions;
			return oldest;
		}
		return -1;
	}

	void LocalConflictEpisodeTracker::finish(LocalConflictEpisode& episode,
		const double now, const Lifecycle lifecycle)
	{
		if (episode.lastObservation.observerWantsMovement)
			episode.observerWantedDuration += static_cast<float>(
				(std::max)(0.0, now - episode.lastSampleTime));
		episode.active = false;
		episode.lifecycle = lifecycle;
		episode.endTime = now;
		episode.duration = static_cast<float>(
			(std::max)(0.0, now - episode.startTime));
		episode.progressAfter = episode.lastObservation.observerRemainingDistance;
	}

	bool LocalConflictEpisodeTracker::exactNavigationMatch(
		const LocalConflictEpisode& episode, const std::uint64_t intentId,
		const std::uint64_t attemptId)
	{
		const LocalBotConflictObservation& observation = episode.lastObservation;
		const bool attemptMatches = attemptId != 0 &&
			(observation.observerAttemptId == attemptId ||
			 observation.otherAttemptId == attemptId);
		if (attemptMatches)
			return true;
		return intentId != 0 &&
			(observation.observerIntentId == intentId ||
			 observation.otherIntentId == intentId);
	}

	std::vector<EpisodeUpdate> LocalConflictEpisodeTracker::Observe(
		const LocalBotConflictObservation& input, const double now,
		const std::uint64_t scanGeneration)
	{
		std::vector<EpisodeUpdate> updates;
		const ConflictType type = Classify(input);
		if (type == ConflictType::UNKNOWN)
			return updates;
		const int lowBot = (std::min)(input.observerBot, input.otherBot);
		const int highBot = (std::max)(input.observerBot, input.otherBot);
		const std::uint32_t context = ContextKey(input);
		const int activeIndex = findActive(lowBot, highBot, type, context);
		if (activeIndex >= 0)
		{
			LocalConflictEpisode& episode = m_episodes[activeIndex];
			if (episode.lastObservation.observerWantsMovement)
				episode.observerWantedDuration += static_cast<float>(
					(std::max)(0.0, now - episode.lastSampleTime));
			episode.lastSampleTime = now;
			episode.lastScanGeneration = scanGeneration;
			episode.duration = static_cast<float>(
				(std::max)(0.0, now - episode.startTime));
			episode.lastObservation = input;
			episode.minDistance = (std::min)(episode.minDistance, input.distance);
			episode.maxClosingSpeed = (std::max)(episode.maxClosingSpeed,
				input.closingSpeed);
			episode.progressDuring = input.observerRemainingDistance;
			++episode.samples;
			updates.push_back({Lifecycle::CONTINUE, episode});
			return updates;
		}

		for (std::size_t i = 0; i < m_count; ++i)
		{
			LocalConflictEpisode& previous = m_episodes[i];
			if (previous.active && previous.lowBot == lowBot &&
				previous.highBot == highBot)
			{
				finish(previous, now, Lifecycle::RESOLVED);
				updates.push_back({Lifecycle::RESOLVED, previous});
			}
		}

		const int index = findFreeOrOldest();
		if (index < 0)
			return updates;
		LocalConflictEpisode& episode = m_episodes[index];
		episode = LocalConflictEpisode{};
		episode.active = true;
		episode.episodeId = ++m_nextEpisodeId;
		if (episode.episodeId == 0)
			episode.episodeId = ++m_nextEpisodeId;
		episode.lowBot = lowBot;
		episode.highBot = highBot;
		episode.team = input.team;
		episode.type = type;
		episode.context = context;
		episode.lifecycle = Lifecycle::BEGIN;
		episode.startTime = now;
		episode.lastSampleTime = now;
		episode.lastScanGeneration = scanGeneration;
		episode.minDistance = input.distance;
		episode.maxClosingSpeed = input.closingSpeed;
		episode.samples = 1;
		episode.observerWantedDuration = 0.0f;
		episode.progressBefore = input.observerRemainingDistance;
		episode.progressDuring = input.observerRemainingDistance;
		episode.lastObservation = input;
		updates.push_back({Lifecycle::BEGIN, episode});
		return updates;
	}

	std::vector<EpisodeUpdate> LocalConflictEpisodeTracker::EndUnseen(
		const std::uint64_t scanGeneration, const double now)
	{
		std::vector<EpisodeUpdate> updates;
		for (std::size_t i = 0; i < m_count; ++i)
		{
			LocalConflictEpisode& episode = m_episodes[i];
			if (!episode.active || episode.lastScanGeneration == scanGeneration)
				continue;
			finish(episode, now, Lifecycle::RESOLVED);
			updates.push_back({Lifecycle::RESOLVED, episode});
		}
		return updates;
	}

	std::vector<EpisodeUpdate> LocalConflictEpisodeTracker::EndAll(
		const double now, const Lifecycle lifecycle)
	{
		std::vector<EpisodeUpdate> updates;
		for (std::size_t i = 0; i < m_count; ++i)
		{
			LocalConflictEpisode& episode = m_episodes[i];
			if (!episode.active)
				continue;
			finish(episode, now, lifecycle);
			updates.push_back({lifecycle, episode});
		}
		return updates;
	}

	std::vector<EpisodeUpdate> LocalConflictEpisodeTracker::Correlate(
		const int botId, const std::uint64_t intentId,
		const std::uint64_t attemptId, const double now,
		const CorrelationSignal signal)
	{
		std::vector<EpisodeUpdate> updates;
		const std::uint8_t signalBit = static_cast<std::uint8_t>(1u <<
			static_cast<std::uint8_t>(signal));
		int bestIndex = -1;
		for (std::size_t i = 0; i < m_count; ++i)
		{
			const LocalConflictEpisode& episode = m_episodes[i];
			const bool recentResolved = !episode.active && now >= episode.endTime &&
				now - episode.endTime <= kCorrelationGraceSeconds;
			if ((!episode.active && !recentResolved) ||
				(episode.lowBot != botId && episode.highBot != botId) ||
				!exactNavigationMatch(episode, intentId, attemptId))
				continue;
			if (bestIndex < 0 || episode.active != m_episodes[bestIndex].active ||
				episode.lastSampleTime > m_episodes[bestIndex].lastSampleTime)
				bestIndex = static_cast<int>(i);
		}
		if (bestIndex < 0)
			return updates;

		LocalConflictEpisode& episode = m_episodes[bestIndex];
			const bool newSignal = (episode.correlationMask & signalBit) == 0;
			episode.correlationMask |= signalBit;
			if (signal == CorrelationSignal::NAV_PROGRESS || !newSignal)
				return updates;
			episode.stuckCorrelated = true;
			episode.lifecycle = Lifecycle::ESCALATED_TO_NO_PROGRESS;
			episode.lastSampleTime = (std::max)(episode.lastSampleTime, now);
			updates.push_back({Lifecycle::ESCALATED_TO_NO_PROGRESS, episode});
		return updates;
	}

	void LocalConflictEpisodeTracker::Reset()
	{
		m_episodes.fill(LocalConflictEpisode{});
		m_count = 0;
		m_evictions = 0;
		m_nextEpisodeId = 0;
	}

	std::size_t LocalConflictEpisodeTracker::ActiveCount() const
	{
		std::size_t count = 0;
		for (std::size_t i = 0; i < m_count; ++i)
			if (m_episodes[i].active)
				++count;
		return count;
	}
}
