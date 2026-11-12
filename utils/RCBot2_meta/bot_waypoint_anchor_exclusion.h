#ifndef __RCBOT_WAYPOINT_ANCHOR_EXCLUSION_H__
#define __RCBOT_WAYPOINT_ANCHOR_EXCLUSION_H__

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace RCBotWaypoint
{
	class StartAnchorExclusions
	{
	public:
		static constexpr std::size_t kMaxRetries = 6;

		bool IsExcluded(const int waypoint, const std::uint64_t goalKey,
			const float now)
		{
			Prune(now);
			return goalKey == m_goalKey && std::any_of(m_entries.begin(),
				m_entries.end(), [waypoint](const Entry& entry)
				{
					return entry.waypoint == waypoint;
				});
		}

		bool MarkFailed(const int waypoint, const std::uint64_t goalKey,
			const float now, const float lifetime)
		{
			Prune(now);
			if (waypoint < 0 || lifetime <= 0.0f)
				return false;
			if (m_goalKey != goalKey)
			{
				m_entries.clear();
				m_goalKey = goalKey;
			}
			const auto existing = std::find_if(m_entries.begin(), m_entries.end(),
				[waypoint](const Entry& entry)
				{
					return entry.waypoint == waypoint;
				});
			if (existing != m_entries.end())
			{
				existing->expiresAt = now + lifetime;
				return true;
			}
			if (m_entries.size() >= kMaxRetries)
				return false;
			m_entries.push_back({waypoint, now + lifetime});
			return true;
		}

		std::size_t FailureCount(const std::uint64_t goalKey,
			const float now)
		{
			Prune(now);
			return goalKey == m_goalKey ? m_entries.size() : 0;
		}

		void Clear()
		{
			m_entries.clear();
			m_goalKey = 0;
		}

	private:
		struct Entry
		{
			int waypoint;
			float expiresAt;
		};

		void Prune(const float now)
		{
			m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
				[now](const Entry& entry)
				{
					return entry.expiresAt <= now;
				}), m_entries.end());
			if (m_entries.empty())
				m_goalKey = 0;
		}

		std::vector<Entry> m_entries;
		std::uint64_t m_goalKey = 0;
	};
}

#endif
