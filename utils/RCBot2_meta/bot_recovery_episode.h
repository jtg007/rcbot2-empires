#ifndef __RCBOT_RECOVERY_EPISODE_H__
#define __RCBOT_RECOVERY_EPISODE_H__

#include <cstdint>
#include <string>

namespace RCBotRecovery
{
	enum class Domain : std::uint8_t
	{
		CAPTURE = 0,
		SQUAD,
		ATTACK,
		LAST_KNOWN,
		ROAM,
		COMMANDER,
		UNKNOWN
	};

	inline const char* DomainName(const Domain domain)
	{
		switch (domain)
		{
		case Domain::CAPTURE: return "CAPTURE";
		case Domain::SQUAD: return "SQUAD";
		case Domain::ATTACK: return "ATTACK";
		case Domain::LAST_KNOWN: return "LAST_KNOWN";
		case Domain::ROAM: return "ROAM";
		case Domain::COMMANDER: return "COMMANDER";
		case Domain::UNKNOWN: return "UNKNOWN";
		}
		return "UNKNOWN";
	}

	// This is identity and observation context only. It has no movement or
	// retry authority and intentionally contains no pointers to game objects.
	struct RecoveryEpisodeIdentity
	{
		int botId = -1;
		std::uint64_t lifeGeneration = 0;
		Domain domain = Domain::UNKNOWN;
		std::string subjectId;
		std::uint64_t domainGeneration = 0;
		std::uint64_t episodeId = 0;
		bool active = false;

		bool IsActive() const
		{
			return active && lifeGeneration != 0 && episodeId != 0 &&
				domain != Domain::UNKNOWN && !subjectId.empty();
		}

		bool SamePracticalGoal(const RecoveryEpisodeIdentity& other) const
		{
			return IsActive() && other.IsActive() &&
				botId == other.botId &&
				lifeGeneration == other.lifeGeneration &&
				domain == other.domain &&
				subjectId == other.subjectId &&
				domainGeneration == other.domainGeneration;
		}

		void Reset()
		{
			botId = -1;
			lifeGeneration = 0;
			domain = Domain::UNKNOWN;
			subjectId.clear();
			domainGeneration = 0;
			episodeId = 0;
			active = false;
		}
	};

	// One active episode and one monotonic counter are sufficient for the
	// observe-only phase. End() drops only the active value; the counter is kept
	// so a later episode cannot reuse an ID within the bot instance.
	class EpisodeTracker
	{
	public:
		const RecoveryEpisodeIdentity& Active() const { return m_active; }
		bool HasActive() const { return m_active.IsActive(); }

		RecoveryEpisodeIdentity Start(const int botId,
			const std::uint64_t lifeGeneration, const Domain domain,
			const std::string& subjectId, const std::uint64_t domainGeneration,
			bool* reused = nullptr)
		{
			RecoveryEpisodeIdentity candidate;
			candidate.botId = botId;
			candidate.lifeGeneration = lifeGeneration;
			candidate.domain = domain;
			candidate.subjectId = subjectId;
			candidate.domainGeneration = domainGeneration;
			candidate.active = true;

			if (MatchesPracticalGoal(candidate))
			{
				if (reused != nullptr)
					*reused = true;
				return m_active;
			}

			if (reused != nullptr)
				*reused = false;
			++m_nextEpisodeId;
			if (m_nextEpisodeId == 0)
				++m_nextEpisodeId;
			candidate.episodeId = m_nextEpisodeId;
			m_active = candidate;
			return m_active;
		}

		void End()
		{
			m_active.Reset();
		}

		void Reset()
		{
			m_active.Reset();
			m_nextEpisodeId = 0;
		}

	private:
		bool MatchesPracticalGoal(const RecoveryEpisodeIdentity& candidate) const
		{
			return m_active.IsActive() &&
				m_active.botId == candidate.botId &&
				m_active.lifeGeneration == candidate.lifeGeneration &&
				m_active.domain == candidate.domain &&
				m_active.subjectId == candidate.subjectId &&
				m_active.domainGeneration == candidate.domainGeneration;
		}

		RecoveryEpisodeIdentity m_active;
		std::uint64_t m_nextEpisodeId = 0;
	};

	struct DomainExhaustionResult
	{
		int consecutiveExhaustions = 0;
		bool suppressionStarted = false;
		bool suppressed = false;
	};

	enum class SuppressionCheck : std::uint8_t
	{
		NOT_APPLICABLE = 0,
		ALLOWED,
		SUPPRESSED,
		EXPIRED
	};

	// Engine-free policy state for the first active recovery rollout.  It is
	// deliberately one practical goal wide: the existing EpisodeTracker owns
	// identity, while this value component only counts domain terminal cycles
	// and carries one temporary suppression window.
	class RecoveryEpisodePolicy
	{
	public:
		static constexpr int kMaxConsecutiveExhaustions = 2;
		static constexpr double kSuppressionSeconds = 30.0;

		const RecoveryEpisodeIdentity& Episode() const { return m_episode; }
		int ConsecutiveExhaustions() const { return m_consecutiveExhaustions; }
		bool IsSuppressed() const { return m_suppressed; }
		double SuppressedUntil() const { return m_suppressedUntil; }

		void Reset()
		{
			m_episode.Reset();
			m_consecutiveExhaustions = 0;
			m_suppressed = false;
			m_suppressedUntil = 0.0;
			m_suppressionApplied = false;
		}

		void ObserveEpisode(const RecoveryEpisodeIdentity& episode)
		{
			if (!episode.IsActive())
			{
				Reset();
				return;
			}

			if (!m_episode.IsActive())
			{
				m_episode = episode;
				return;
			}

			if (m_episode.episodeId != episode.episodeId ||
				!m_episode.SamePracticalGoal(episode))
			{
				Reset();
				m_episode = episode;
			}
		}

		DomainExhaustionResult RecordDomainExhausted(
			const RecoveryEpisodeIdentity& episode, const double now)
		{
			ObserveEpisode(episode);
			DomainExhaustionResult result;
			if (!m_episode.IsActive())
				return result;

			if (m_consecutiveExhaustions < kMaxConsecutiveExhaustions)
				++m_consecutiveExhaustions;
			if (m_consecutiveExhaustions >= kMaxConsecutiveExhaustions)
			{
				if (!m_suppressed || now >= m_suppressedUntil)
				{
					m_suppressed = true;
					m_suppressedUntil = now + kSuppressionSeconds;
					m_suppressionApplied = false;
					result.suppressionStarted = true;
				}
				result.suppressed = true;
			}
			result.consecutiveExhaustions = m_consecutiveExhaustions;
			return result;
		}

		bool RecordMeaningfulProgress(const RecoveryEpisodeIdentity& episode)
		{
			ObserveEpisode(episode);
			if (!m_episode.IsActive() || m_consecutiveExhaustions == 0)
				return false;
			m_consecutiveExhaustions = 0;
			m_suppressed = false;
			m_suppressedUntil = 0.0;
			m_suppressionApplied = false;
			return true;
		}

		SuppressionCheck CheckSuppression(
			const RecoveryEpisodeIdentity& episode, const double now)
		{
			ObserveEpisode(episode);
			if (!m_episode.IsActive() || !m_suppressed)
				return SuppressionCheck::NOT_APPLICABLE;
			if (now < m_suppressedUntil)
				return SuppressionCheck::SUPPRESSED;
			m_suppressed = false;
			m_suppressionApplied = false;
			return SuppressionCheck::EXPIRED;
		}

		bool MarkSuppressionApplied(const RecoveryEpisodeIdentity& episode)
		{
			ObserveEpisode(episode);
			if (!m_episode.IsActive() || !m_suppressed ||
				m_suppressionApplied)
				return false;
			m_suppressionApplied = true;
			return true;
		}

		void RecordSuccess(const RecoveryEpisodeIdentity& episode)
		{
			ObserveEpisode(episode);
			if (m_episode.IsActive())
				Reset();
		}

	private:
		RecoveryEpisodeIdentity m_episode;
		int m_consecutiveExhaustions = 0;
		bool m_suppressed = false;
		double m_suppressedUntil = 0.0;
		bool m_suppressionApplied = false;
	};
}

#endif // __RCBOT_RECOVERY_EPISODE_H__
