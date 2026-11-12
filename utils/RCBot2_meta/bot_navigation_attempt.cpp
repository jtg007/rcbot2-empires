#include "bot_navigation_attempt.h"

#include <cmath>
#include <cstdint>

namespace RCBotNavigation
{
	std::uint64_t NavigationAttempt::s_nextAttemptId = 0;

	bool NavigationAttempt::Start(const int botIndex, const int botUserId,
		const int team, const AttemptKey& key, const Vector& startPosition,
		const float startTime, const float initialRemainingDistance,
		const float noProgressTimeout)
	{
		if (m_active || !CanStart(key, startTime))
			return false;

		if (m_hasCooldown && startTime >= m_cooldownUntil)
			m_hasCooldown = false;

		m_active = true;
		m_attemptId = ++s_nextAttemptId;
		if (m_attemptId == 0)
			m_attemptId = ++s_nextAttemptId;
		m_botIndex = botIndex;
		m_botUserId = botUserId;
		m_team = team;
		m_key = key;
		m_startPosition = startPosition;
		m_lastProgressPosition = startPosition;
		m_startTime = startTime;
		m_lastProgressTime = startTime;
		m_endTime = 0.0f;
		m_bestRemainingDistance = initialRemainingDistance;
		m_lastRemainingDistance = initialRemainingDistance;
		m_noProgressTimeout = noProgressTimeout > 0.0f ?
			noProgressTimeout : kNoProgressTimeout;
		m_progressCount = 0;
		m_failureCount = 0;
		m_hasBestDistance = std::isfinite(initialRemainingDistance);
		m_result = Result::NONE;
		return true;
	}

	bool NavigationAttempt::CanStart(const AttemptKey& key, const float now) const
	{
		return !m_active && !IsCooldownBlocked(key, now);
	}

	bool NavigationAttempt::IsCooldownBlocked(const AttemptKey& key,
		const float now) const
	{
		return m_hasCooldown && now < m_cooldownUntil &&
			SameRetryIdentity(key, m_cooldownKey);
	}

	bool NavigationAttempt::MatchesGoal(const Provider provider, const int goalId,
		const std::uint64_t goalSignature) const
	{
		return m_active && m_key.provider == provider &&
			m_key.goalId == goalId && m_key.goalSignature == goalSignature;
	}

	Observation NavigationAttempt::Observe(
		const float now, const Vector& position, const float remainingDistance,
		const SegmentKey& segment)
	{
		if (!m_active)
			return Observation::NONE;

		const bool segmentChanged = m_key.segment != segment;
		const bool hasDistance = std::isfinite(remainingDistance);
		const bool distanceProgress = hasDistance &&
			(!m_hasBestDistance ||
			m_bestRemainingDistance - remainingDistance >= kMinDistanceProgress);
		const bool positionProgress =
			(position - m_lastProgressPosition).LengthSqr() >=
			kMinPositionProgress * kMinPositionProgress;

		m_lastRemainingDistance = remainingDistance;

		// A segment transition or a real reduction in the distance-to-segment/
		// goal baseline is progress. Position movement by itself only counts when
		// no usable distance is available; this prevents side-to-side jitter from
		// keeping a blocked attempt alive indefinitely.
		if (segmentChanged || distanceProgress || (!hasDistance && positionProgress))
		{
			m_key.segment = segment;
			if (hasDistance &&
				(!m_hasBestDistance || remainingDistance < m_bestRemainingDistance))
			{
				m_bestRemainingDistance = remainingDistance;
				m_hasBestDistance = true;
			}
			m_lastProgressPosition = position;
			m_lastProgressTime = now;
			++m_progressCount;
			return Observation::PROGRESS;
		}

		if (now - m_lastProgressTime >= m_noProgressTimeout)
			return Observation::NO_PROGRESS;

		return Observation::NONE;
	}

	void NavigationAttempt::PauseNoProgress(const float now)
	{
		if (m_active && now > m_lastProgressTime)
			m_lastProgressTime = now;
	}

	bool NavigationAttempt::End(const Result result, const float endTime)
	{
		if (!m_active)
			return false;

		m_active = false;
		m_result = result;
		m_endTime = endTime;
		if (result == Result::PATH_FAILED || result == Result::PATH_STUCK ||
			result == Result::UNREACHABLE)
		{
			m_cooldownKey = m_key;
			m_cooldownUntil = endTime + kFailureCooldown;
			m_hasCooldown = true;
			++m_failureCount;
		}
		return true;
	}

	void NavigationAttempt::Reset()
	{
		m_active = false;
		m_attemptId = 0;
		m_botIndex = -1;
		m_botUserId = -1;
		m_team = -1;
		m_key = AttemptKey{};
		m_startPosition = Vector();
		m_lastProgressPosition = Vector();
		m_startTime = 0.0f;
		m_lastProgressTime = 0.0f;
		m_endTime = 0.0f;
		m_bestRemainingDistance = 0.0f;
		m_lastRemainingDistance = 0.0f;
		m_noProgressTimeout = kNoProgressTimeout;
		m_progressCount = 0;
		m_failureCount = 0;
		m_hasBestDistance = false;
		m_result = Result::NONE;
		m_hasCooldown = false;
		m_cooldownKey = AttemptKey{};
		m_cooldownUntil = 0.0f;
	}

	bool NavigationAttempt::SameRetryIdentity(const AttemptKey& left,
		const AttemptKey& right)
	{
		if (left.provider != right.provider || left.goalId != right.goalId ||
			left.goalSignature != right.goalSignature)
			return false;

		// Unknown segment information is deliberately conservative: an adapter
		// must not evade a recent failure merely by restarting before it has
		// identified its first segment. A known different segment is a legitimate
		// route change and may be attempted immediately.
		if (left.segment.currentWaypoint != -1 &&
			right.segment.currentWaypoint != -1 &&
			left.segment.currentWaypoint != right.segment.currentWaypoint)
			return false;
		if (left.segment.nextWaypoint != -1 && right.segment.nextWaypoint != -1 &&
			left.segment.nextWaypoint != right.segment.nextWaypoint)
			return false;
		return true;
	}

	std::uint64_t NavigationAttempt::MakeGoalSignature(const int goalId,
		const Vector& goalPosition)
	{
		constexpr std::uint64_t offset = 1469598103934665603ULL;
		constexpr std::uint64_t prime = 1099511628211ULL;
		std::uint64_t hash = offset;
		auto add = [&hash](const std::uint64_t value)
		{
			hash ^= value;
			hash *= prime;
		};
		auto quantize = [](const float value) -> std::int64_t
		{
			if (!std::isfinite(value))
				return 0;
			return static_cast<std::int64_t>(value >= 0.0f ?
				value / 8.0f + 0.5f : value / 8.0f - 0.5f);
		};

		add(static_cast<std::uint32_t>(goalId));
		add(static_cast<std::uint64_t>(quantize(goalPosition.x)));
		add(static_cast<std::uint64_t>(quantize(goalPosition.y)));
		add(static_cast<std::uint64_t>(quantize(goalPosition.z)));
		return hash;
	}
}
