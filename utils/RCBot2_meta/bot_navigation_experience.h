#ifndef __RCBOT_NAVIGATION_EXPERIENCE_H__
#define __RCBOT_NAVIGATION_EXPERIENCE_H__

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

// Bounded, value-only analysis of completed NavigationAttempts. This header
// intentionally has no navigation-policy or movement API: recording an
// observation cannot select a provider or alter a route.
namespace RCBotNavigationExperience
{
enum class Provider : std::uint8_t { WAYPOINT, NAVMESH, LOCAL_EXPLORER, UNKNOWN };
enum class Traversal : std::uint8_t
{
	NONE, LADDER, JUMP, DROP, DOOR, OTHER, STEP_UP, STEP_DOWN, NORMAL, UNKNOWN
};
enum class Result : std::uint8_t
{
	SUCCESS, PATH_FAILED, PATH_STUCK, UNREACHABLE, CANCELLED_EXTERNAL,
	CANCELLED_REPLACEMENT, CANCELLED_LIFECYCLE, UNKNOWN
};
enum class Signal : std::uint8_t
{
	STUCK_HEURISTIC, CAPTURE_ROUTE_EXHAUSTED, SQUAD_RECOVERY_FAILED
};
enum class Severity : std::uint8_t { MEDIUM, HIGH, CRITICAL };
enum class TransitionResult : std::uint8_t
{
	TRANSITION_SUCCESS,
	NO_PROGRESS,
	STRUCTURAL_PATH_FAILURE,
	CANCELLED,
	UNKNOWN
};
enum class CancellationKind : std::uint8_t
{
	NONE, REPLACEMENT, LIFECYCLE, EXTERNAL
};

// A concrete, directed provider edge observed during one provider attempt.
// Unknown transitions use fromNode == toNode == -1 and never create an edge
// key. The value owns no engine or graph pointers.
struct NavigationTransitionObservation
{
	std::string mapIdentity;
	Provider provider = Provider::UNKNOWN;
	std::uint64_t intentId = 0;
	std::uint64_t attemptId = 0;
	std::int64_t fromNode = -1;
	std::int64_t toNode = -1;
	Traversal traversal = Traversal::UNKNOWN;
	TransitionResult result = TransitionResult::UNKNOWN;
	CancellationKind cancellation = CancellationKind::NONE;
	int botId = -1;
	std::uint64_t pathRequestId = 0;
	std::uint64_t attemptGeneration = 0;
	std::uint64_t timestampMs = 0;
	double durationSeconds = 0.0;
	double progressUnits = 0.0;
	bool transitionKnown = false;
};

struct TransitionKey
{
	std::string map;
	Provider provider = Provider::UNKNOWN;
	std::int64_t fromNode = -1;
	std::int64_t toNode = -1;
	Traversal traversal = Traversal::UNKNOWN;

	bool operator<(const TransitionKey& other) const
	{
		return std::tie(map, provider, fromNode, toNode, traversal) <
			std::tie(other.map, other.provider, other.fromNode, other.toNode,
				other.traversal);
	}
	bool operator==(const TransitionKey& other) const
	{
		return !(operator<(other)) && !(other < *this);
	}
};

struct Key
{
	std::string map;
	Provider provider = Provider::UNKNOWN;
	std::int64_t startNode = -1;
	std::int64_t destinationNode = -1;
	Traversal traversal = Traversal::NONE;

	bool operator<(const Key& other) const
	{
		return std::tie(map, provider, startNode, destinationNode, traversal) <
			std::tie(other.map, other.provider, other.startNode,
				other.destinationNode, other.traversal);
	}
	bool operator==(const Key& other) const
	{
		return !(operator<(other)) && !(other < *this);
	}
};

struct Observation
{
	Key key;
	int botId = -1; // session user ID, falling back to the entity index
	std::uint64_t attemptId = 0;
	std::uint64_t timestampMs = 0;
	Result result = Result::UNKNOWN;
	double traversalSeconds = 0.0;
	double progressUnits = 0.0;
};

struct RecentResult
{
	std::uint64_t timestampMs = 0;
	std::uint64_t attemptId = 0;
	int botId = -1;
	Result result = Result::UNKNOWN;
};

struct Limits
{
	std::size_t maxEntries = 1024;
	std::size_t maxHotspots = 64;
	std::size_t maxBotIdentitiesPerEntry = 128;
	std::size_t maxRecentResultsPerEntry = 32;
	std::size_t maxTransitionEntries = 2048;
	std::size_t maxTransitionRecentResultsPerEntry = 32;
};

struct RecentTransitionResult
{
	std::uint64_t timestampMs = 0;
	std::uint64_t attemptId = 0;
	std::uint64_t attemptGeneration = 0;
	TransitionResult result = TransitionResult::UNKNOWN;
	CancellationKind cancellation = CancellationKind::NONE;
};

struct TransitionEntry
{
	TransitionKey key;
	std::uint64_t attempts = 0;
	std::uint64_t successes = 0;
	std::uint64_t noProgress = 0;
	std::uint64_t structuralFailures = 0;
	std::uint64_t cancellations = 0;
	std::uint64_t cancellationReplacement = 0;
	std::uint64_t cancellationLifecycle = 0;
	std::uint64_t cancellationExternal = 0;
	std::uint64_t unknown = 0;
	std::uint64_t totalDurationMs = 0;
	std::uint64_t totalProgressMilliUnits = 0;
	std::uint64_t lastSeenMs = 0;
	std::uint64_t consecutiveFailures = 0;
	std::set<int> botIds;
	std::set<int> failureBotIds;
	bool botIdsTruncated = false;
	bool failureBotIdsTruncated = false;
	std::vector<RecentTransitionResult> recentResults;
};

struct TransitionHotspot
{
	TransitionKey key;
	std::uint64_t attempts = 0;
	std::uint64_t successes = 0;
	std::uint64_t failures = 0;
	std::uint64_t noProgress = 0;
	std::uint64_t structuralFailures = 0;
	std::size_t affectedBots = 0;
	std::size_t affectedFailureBots = 0;
	bool affectedBotsTruncated = false;
	bool affectedFailureBotsTruncated = false;
};

// Active v1 learned-cost policy.  This policy only consumes the directed
// transition store; it has no route mutation or provider-arbitration state.
// The bounds are deliberately small so a learned edge remains traversable.
struct LearnedNavigationCostPolicy
{
	bool enabled = false;
	std::uint64_t minAttempts = 4;
	std::uint64_t minFailures = 2;
	std::size_t minDistinctFailureBots = 2;
	double maxPenalty = 128.0;
	double penaltyScale = 1.0;
	double successRecoveryWeight = 1.0;
};

struct LearnedNavigationCost
{
	double confidence = 0.0;
	double penalty = 0.0;
	std::uint64_t attempts = 0;
	std::uint64_t classifiedAttempts = 0;
	std::uint64_t failures = 0;
	std::uint64_t noProgress = 0;
	std::uint64_t structuralFailures = 0;
	std::uint64_t successes = 0;
	std::size_t distinctFailureBots = 0;
	bool eligible = false;
};

constexpr std::uint64_t kMinimumTransitionHotspotAttempts = 4;
constexpr std::uint64_t kMinimumTransitionHotspotFailures = 2;
constexpr std::size_t kMinimumTransitionHotspotBots = 2;

struct Entry
{
	Key key;
	std::uint64_t attempts = 0;
	std::uint64_t successes = 0;
	std::uint64_t pathFailed = 0;
	std::uint64_t stuck = 0;
	std::uint64_t unreachable = 0;
	std::uint64_t cancelledExternal = 0;
	std::uint64_t cancelledReplacement = 0;
	std::uint64_t cancelledLifecycle = 0;
	std::uint64_t unknown = 0;
	std::uint64_t totalTraversalMs = 0;
	std::uint64_t totalProgressMilliUnits = 0;
	std::uint64_t lastSeenMs = 0;
	std::uint64_t lastResultMs = 0;
	std::uint64_t lastAttemptId = 0;
	Result lastResult = Result::UNKNOWN;
	std::uint64_t consecutiveFailures = 0;
	std::set<int> botIds;
	bool botIdsTruncated = false;
	std::array<std::uint64_t, 3> correlatedSignals{};
	std::vector<RecentResult> recentResults;
};

struct Hotspot
{
	Key key;
	std::uint64_t attempts = 0;
	std::uint64_t successes = 0;
	std::uint64_t failures = 0;
	std::uint64_t stuck = 0;
	std::uint64_t unreachable = 0;
	std::size_t affectedBots = 0;
	bool affectedBotsTruncated = false;
	std::uint32_t failureRatioBasisPoints = 0;
	Severity severity = Severity::MEDIUM;
};

inline const char* ProviderName(const Provider provider)
{
	switch (provider)
	{
	case Provider::WAYPOINT: return "WAYPOINT";
	case Provider::NAVMESH: return "NAVMESH";
	case Provider::LOCAL_EXPLORER: return "LOCAL_EXPLORER";
	default: return "UNKNOWN";
	}
}

inline const char* TraversalName(const Traversal traversal)
{
	switch (traversal)
	{
	case Traversal::LADDER: return "LADDER";
	case Traversal::JUMP: return "JUMP";
	case Traversal::DROP: return "DROP";
	case Traversal::DOOR: return "DOOR";
	case Traversal::OTHER: return "OTHER";
	case Traversal::STEP_UP: return "STEP_UP";
	case Traversal::STEP_DOWN: return "STEP_DOWN";
	case Traversal::NORMAL: return "NORMAL";
	case Traversal::UNKNOWN: return "UNKNOWN";
	default: return "NONE";
	}
}

inline const char* TransitionResultName(const TransitionResult result)
{
	switch (result)
	{
	case TransitionResult::TRANSITION_SUCCESS: return "TRANSITION_SUCCESS";
	case TransitionResult::NO_PROGRESS: return "NO_PROGRESS";
	case TransitionResult::STRUCTURAL_PATH_FAILURE: return "STRUCTURAL_PATH_FAILURE";
	case TransitionResult::CANCELLED: return "CANCELLED";
	default: return "UNKNOWN";
	}
}

inline const char* CancellationKindName(const CancellationKind kind)
{
	switch (kind)
	{
	case CancellationKind::REPLACEMENT: return "replacement";
	case CancellationKind::LIFECYCLE: return "lifecycle";
	case CancellationKind::EXTERNAL: return "external";
	default: return "none";
	}
}

inline const char* ResultName(const Result result)
{
	switch (result)
	{
	case Result::SUCCESS: return "SUCCESS";
	case Result::PATH_FAILED: return "PATH_FAILED";
	case Result::PATH_STUCK: return "PATH_STUCK";
	case Result::UNREACHABLE: return "UNREACHABLE";
	case Result::CANCELLED_EXTERNAL: return "CANCELLED_EXTERNAL";
	case Result::CANCELLED_REPLACEMENT: return "CANCELLED_REPLACEMENT";
	case Result::CANCELLED_LIFECYCLE: return "CANCELLED_LIFECYCLE";
	default: return "UNKNOWN";
	}
}

inline const char* SignalName(const Signal signal)
{
	switch (signal)
	{
	case Signal::STUCK_HEURISTIC: return "STUCK_HEURISTIC";
	case Signal::CAPTURE_ROUTE_EXHAUSTED: return "CAPTURE_ROUTE_EXHAUSTED";
	default: return "SQUAD_RECOVERY_FAILED";
	}
}

inline const char* SeverityName(const Severity severity)
{
	switch (severity)
	{
	case Severity::HIGH: return "HIGH";
	case Severity::CRITICAL: return "CRITICAL";
	default: return "MEDIUM";
	}
}

// Pack a bounded 256-unit world cell into a stable positive 63-bit identity.
// This is used only for providers which expose no persistent graph node.
inline std::int64_t QuantizedCell(const double x, const double y,
	const double z, const double cellSize = 256.0)
{
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
		!std::isfinite(cellSize) || cellSize <= 0.0)
		return -1;
	const auto quantize = [cellSize](const double value) -> std::int64_t
	{
		const double cell = std::floor(value / cellSize + 0.5);
		if (!std::isfinite(cell) || cell < -1048575.0 || cell > 1048575.0)
			return (std::numeric_limits<std::int64_t>::min)();
		return static_cast<std::int64_t>(cell);
	};
	const std::int64_t coordinates[] = { quantize(x), quantize(y), quantize(z) };
	std::uint64_t packed = 0;
	for (int i = 0; i < 3; ++i)
	{
		if (coordinates[i] == (std::numeric_limits<std::int64_t>::min)())
			return -1;
		const std::uint64_t value = coordinates[i] < 0 ?
			static_cast<std::uint64_t>(-coordinates[i] * 2 - 1) :
			static_cast<std::uint64_t>(coordinates[i] * 2);
		packed |= (value & 0x1fffffULL) << (i * 21);
	}
	return static_cast<std::int64_t>(packed);
}

class Store
{
public:
	explicit Store(const Limits& limits = Limits()) : m_limits(limits)
	{
		m_limits.maxEntries = (std::max<std::size_t>)(1,
			(std::min<std::size_t>)(m_limits.maxEntries, 1024));
		m_limits.maxHotspots = (std::max<std::size_t>)(1,
			(std::min<std::size_t>)(m_limits.maxHotspots, 64));
		m_limits.maxBotIdentitiesPerEntry = (std::max<std::size_t>)(2,
			(std::min<std::size_t>)(m_limits.maxBotIdentitiesPerEntry, 128));
		m_limits.maxRecentResultsPerEntry = (std::max<std::size_t>)(1,
			(std::min<std::size_t>)(m_limits.maxRecentResultsPerEntry, 32));
		m_limits.maxTransitionEntries = (std::max<std::size_t>)(1,
			(std::min<std::size_t>)(m_limits.maxTransitionEntries, 4096));
		m_limits.maxTransitionRecentResultsPerEntry = (std::max<std::size_t>)(1,
			(std::min<std::size_t>)(m_limits.maxTransitionRecentResultsPerEntry, 64));
	}

	void reset(const std::string& mapIdentity,
		const std::string& navigationDataIdentity)
	{
		m_mapIdentity = mapIdentity;
		m_navigationDataIdentity = navigationDataIdentity;
		m_entries.clear();
		m_evictions = 0;
		m_totalObservations = 0;
		m_providerDistribution.fill(0);
		m_resultCounts.fill(0);
		m_transitionEntries.clear();
		m_transitionResultCounts.fill(0);
		m_transitionCancellationCounts.fill(0);
		m_unknownTransitionResults.clear();
		m_transitionTotalObservations = 0;
		m_unknownTransitionObservations = 0;
		m_transitionEvictions = 0;
		m_learnedNavigationBehaviorChanges = 0;
		m_learnedEdgesWithPenalty = 0;
		m_maximumObservedPenalty = 0.0;
		m_averageObservedPenalty = 0.0;
		m_learnedCostsEnabled = false;
	}

	void setLearnedNavigationBehaviorChanges(const std::uint64_t value)
	{
		m_learnedNavigationBehaviorChanges = value;
	}

	void setLearnedNavigationMetrics(const std::uint64_t edgesWithPenalty,
		const double maximumPenalty, const double averagePenalty,
		const bool enabled)
	{
		m_learnedEdgesWithPenalty = edgesWithPenalty;
		m_maximumObservedPenalty = maximumPenalty;
		m_averageObservedPenalty = averagePenalty;
		m_learnedCostsEnabled = enabled;
	}

	bool record(const Observation& observation)
	{
		if (!valid(observation))
			return false;

		auto it = m_entries.find(observation.key);
		if (it != m_entries.end() && std::find_if(
			it->second.recentResults.begin(), it->second.recentResults.end(),
			[&observation](const RecentResult& recent)
			{ return recent.attemptId == observation.attemptId; }) !=
			it->second.recentResults.end())
			return false;
		if (it == m_entries.end())
		{
			if (m_entries.size() >= m_limits.maxEntries)
				evictLeastRecentlySeen();
			Entry entry;
			entry.key = observation.key;
			it = m_entries.emplace(observation.key, entry).first;
		}
		Entry& entry = it->second;
		entry.attempts = saturatedAdd(entry.attempts, 1);
		m_totalObservations = saturatedAdd(m_totalObservations, 1);
		const std::size_t resultIndex = resultIndexFor(observation.result);
		m_resultCounts[resultIndex] = saturatedAdd(m_resultCounts[resultIndex], 1);
		const std::size_t providerIndex = providerIndexFor(observation.key.provider);
		m_providerDistribution[providerIndex] = saturatedAdd(
			m_providerDistribution[providerIndex], 1);
		entry.lastSeenMs = (std::max)(entry.lastSeenMs, observation.timestampMs);
		entry.totalTraversalMs = saturatedAdd(entry.totalTraversalMs,
			static_cast<std::uint64_t>(std::llround(observation.traversalSeconds * 1000.0)));
		entry.totalProgressMilliUnits = saturatedAdd(entry.totalProgressMilliUnits,
			static_cast<std::uint64_t>(std::llround(observation.progressUnits * 1000.0)));
		countResult(entry, observation);
		if (entry.botIds.find(observation.botId) == entry.botIds.end())
		{
			if (entry.botIds.size() < m_limits.maxBotIdentitiesPerEntry)
				entry.botIds.insert(observation.botId);
			else
			{
				entry.botIdsTruncated = true;
				if (observation.botId < *entry.botIds.rbegin())
				{
					entry.botIds.erase(std::prev(entry.botIds.end()));
					entry.botIds.insert(observation.botId);
				}
			}
		}
		insertRecent(entry, observation);
		return true;
	}

	bool recordTransition(const NavigationTransitionObservation& observation)
	{
		if (!validTransition(observation))
			return false;

		const std::size_t resultIndex = static_cast<std::size_t>(observation.result);
		if (!observation.transitionKnown)
		{
			if (std::find_if(m_unknownTransitionResults.begin(),
				m_unknownTransitionResults.end(), [&observation](
					const RecentTransitionResult& recent)
				{
					return recent.attemptId == observation.attemptId &&
						recent.attemptGeneration == observation.attemptGeneration &&
						recent.result == observation.result &&
						recent.cancellation == observation.cancellation;
				}) != m_unknownTransitionResults.end())
				return false;
			countTransitionCancellation(observation);
			m_transitionTotalObservations = saturatedAdd(
				m_transitionTotalObservations, 1);
			m_transitionResultCounts[resultIndex] = saturatedAdd(
				m_transitionResultCounts[resultIndex], 1);
			m_unknownTransitionObservations = saturatedAdd(
				m_unknownTransitionObservations, 1);
			insertTransitionRecent(m_unknownTransitionResults, observation);
			return true;
		}

		const TransitionKey key{observation.mapIdentity, observation.provider,
			observation.fromNode, observation.toNode, observation.traversal};
		auto it = m_transitionEntries.find(key);
		if (it != m_transitionEntries.end() && std::find_if(
			it->second.recentResults.begin(), it->second.recentResults.end(),
			[&observation](const RecentTransitionResult& recent)
			{
				return recent.attemptId == observation.attemptId &&
					recent.attemptGeneration == observation.attemptGeneration &&
					recent.result == observation.result &&
					recent.cancellation == observation.cancellation;
			}) != it->second.recentResults.end())
			return false;
		countTransitionCancellation(observation);
		if (it == m_transitionEntries.end())
		{
			if (m_transitionEntries.size() >= m_limits.maxTransitionEntries)
				evictLeastRecentlySeenTransition();
			TransitionEntry entry;
			entry.key = key;
			it = m_transitionEntries.emplace(key, entry).first;
		}
		TransitionEntry& entry = it->second;
		entry.attempts = saturatedAdd(entry.attempts, 1);
		m_transitionTotalObservations = saturatedAdd(
			m_transitionTotalObservations, 1);
		m_transitionResultCounts[resultIndex] = saturatedAdd(
			m_transitionResultCounts[resultIndex], 1);
		entry.lastSeenMs = (std::max)(entry.lastSeenMs, observation.timestampMs);
		entry.totalDurationMs = saturatedAdd(entry.totalDurationMs,
			static_cast<std::uint64_t>(std::llround(
				observation.durationSeconds * 1000.0)));
		entry.totalProgressMilliUnits = saturatedAdd(
			entry.totalProgressMilliUnits,
			static_cast<std::uint64_t>(std::llround(
				observation.progressUnits * 1000.0)));
		countTransitionResult(entry, observation);
		insertBot(entry.botIds, entry.botIdsTruncated, observation.botId);
		if (isTransitionFailure(observation.result))
			insertBot(entry.failureBotIds, entry.failureBotIdsTruncated,
				observation.botId);
		insertTransitionRecent(entry.recentResults, observation);
		recomputeConsecutiveTransitionFailures(entry);
		return true;
	}

	bool correlate(const Key& key, const Signal signal)
	{
		auto it = m_entries.find(key);
		if (it == m_entries.end())
			return false;
		const std::size_t index = signalIndex(signal);
		it->second.correlatedSignals[index] = saturatedAdd(
			it->second.correlatedSignals[index], 1);
		return true;
	}

	const std::map<Key, Entry>& entries() const { return m_entries; }
	std::uint64_t evictions() const { return m_evictions; }
	std::uint64_t totalObservations() const { return m_totalObservations; }
	std::uint64_t resultCount(const Result result) const
	{
		return m_resultCounts[resultIndexFor(result)];
	}
	const Limits& limits() const { return m_limits; }
	const std::map<TransitionKey, TransitionEntry>& transitionEntries() const
	{
		return m_transitionEntries;
	}
	const TransitionEntry* findTransition(const TransitionKey& key) const
	{
		const auto it = m_transitionEntries.find(key);
		return it == m_transitionEntries.end() ? nullptr : &it->second;
	}

	LearnedNavigationCost learnedCost(const TransitionKey& key,
		const LearnedNavigationCostPolicy& policy) const
	{
		LearnedNavigationCost result;
		const TransitionEntry* entry = findTransition(key);
		if (entry == nullptr)
			return result;
		result.attempts = entry->attempts;
		result.successes = entry->successes;
		result.noProgress = entry->noProgress;
		result.structuralFailures = entry->structuralFailures;
		result.failures = result.noProgress + result.structuralFailures;
		result.classifiedAttempts = result.successes + result.failures;
		result.distinctFailureBots = entry->failureBotIds.size();
		if (!policy.enabled || key.provider == Provider::LOCAL_EXPLORER ||
			result.classifiedAttempts < policy.minAttempts ||
			result.failures < policy.minFailures ||
			result.distinctFailureBots < policy.minDistinctFailureBots)
			return result;

		const double attempts = static_cast<double>(result.classifiedAttempts);
		const double failureRatio = attempts > 0.0 ?
			static_cast<double>(result.failures) / attempts : 0.0;
		const double sampleConfidence = policy.minAttempts == 0 ? 1.0 :
			(std::min)(1.0, attempts /
				(static_cast<double>(policy.minAttempts) * 2.0));
		const double successRatio = (std::min)(1.0,
			static_cast<double>(result.successes) / attempts);
		const double recoveryWeight = (std::max)(0.0,
			(std::min)(1.0, policy.successRecoveryWeight));
		const double recovery = (std::max)(0.0,
			1.0 - recoveryWeight * successRatio);
		result.confidence = (std::max)(0.0, (std::min)(1.0,
			failureRatio * sampleConfidence * recovery));
		const double cap = (std::max)(0.0, policy.maxPenalty);
		const double scale = (std::max)(0.0, policy.penaltyScale);
		result.penalty = (std::max)(0.0, (std::min)(cap,
			cap * scale * result.confidence));
		result.eligible = result.penalty > 0.0;
		return result;
	}
	std::uint64_t transitionObservationCount() const
	{
		return m_transitionTotalObservations;
	}
	std::uint64_t transitionResultCount(const TransitionResult result) const
	{
		return m_transitionResultCounts[static_cast<std::size_t>(result)];
	}
	std::uint64_t unknownTransitionCount() const
	{
		return m_unknownTransitionObservations;
	}
	std::uint64_t transitionEvictions() const { return m_transitionEvictions; }
	std::size_t uniqueTransitionKeys() const { return m_transitionEntries.size(); }

	std::vector<TransitionHotspot> transitionHotspots() const
	{
		std::vector<TransitionHotspot> result;
		for (const auto& pair : m_transitionEntries)
		{
			const TransitionEntry& entry = pair.second;
			const std::uint64_t failures = entry.noProgress +
				entry.structuralFailures;
			const std::uint64_t classified = entry.successes + failures;
			if (classified < kMinimumTransitionHotspotAttempts ||
				failures < kMinimumTransitionHotspotFailures ||
				entry.failureBotIds.size() < kMinimumTransitionHotspotBots)
				continue;
			TransitionHotspot hotspot;
			hotspot.key = entry.key;
			hotspot.attempts = entry.attempts;
			hotspot.successes = entry.successes;
			hotspot.failures = failures;
			hotspot.noProgress = entry.noProgress;
			hotspot.structuralFailures = entry.structuralFailures;
			hotspot.affectedBots = entry.botIds.size();
			hotspot.affectedFailureBots = entry.failureBotIds.size();
			hotspot.affectedBotsTruncated = entry.botIdsTruncated;
			hotspot.affectedFailureBotsTruncated =
				entry.failureBotIdsTruncated;
			result.push_back(hotspot);
		}
		std::sort(result.begin(), result.end(),
			[](const TransitionHotspot& left, const TransitionHotspot& right)
			{
				if (left.failures != right.failures)
					return left.failures > right.failures;
				if (left.affectedFailureBots != right.affectedFailureBots)
					return left.affectedFailureBots > right.affectedFailureBots;
				if (left.attempts != right.attempts)
					return left.attempts > right.attempts;
				return left.key < right.key;
			});
		if (result.size() > m_limits.maxHotspots)
			result.resize(m_limits.maxHotspots);
		return result;
	}

	std::vector<Hotspot> hotspots() const
	{
		std::vector<Hotspot> result;
		for (const auto& pair : m_entries)
		{
			const Entry& entry = pair.second;
			const std::uint64_t failures = entry.pathFailed + entry.stuck +
				entry.unreachable;
			const std::uint64_t classified = entry.successes + failures;
			if (classified < 4 || entry.botIds.size() < 2)
				continue;
			const double ratio = classified == 0 ? 0.0 :
				static_cast<double>(failures) / static_cast<double>(classified);
			const bool highFailureRatio = failures >= 3 && ratio >= 0.60;
			const bool repeatedTraversalFailure = entry.stuck >= 2 ||
				entry.unreachable >= 2;
			if (!highFailureRatio && !repeatedTraversalFailure)
				continue;

			Hotspot hotspot;
			hotspot.key = entry.key;
			hotspot.attempts = entry.attempts;
			hotspot.successes = entry.successes;
			hotspot.failures = failures;
			hotspot.stuck = entry.stuck;
			hotspot.unreachable = entry.unreachable;
			hotspot.affectedBots = entry.botIds.size();
			hotspot.affectedBotsTruncated = entry.botIdsTruncated;
			hotspot.failureRatioBasisPoints = static_cast<std::uint32_t>(
				std::llround(ratio * 10000.0));
			if (failures >= 6 && ratio >= 0.80 && entry.botIds.size() >= 3)
				hotspot.severity = Severity::CRITICAL;
			else if (failures >= 4 && ratio >= 0.70)
				hotspot.severity = Severity::HIGH;
			else
				hotspot.severity = Severity::MEDIUM;
			result.push_back(hotspot);
		}
		std::sort(result.begin(), result.end(),
			[](const Hotspot& left, const Hotspot& right)
			{
				if (left.severity != right.severity)
					return static_cast<int>(left.severity) >
						static_cast<int>(right.severity);
				if (left.failureRatioBasisPoints != right.failureRatioBasisPoints)
					return left.failureRatioBasisPoints >
						right.failureRatioBasisPoints;
				if (left.failures != right.failures)
					return left.failures > right.failures;
				return left.key < right.key;
			});
		if (result.size() > m_limits.maxHotspots)
			result.resize(m_limits.maxHotspots);
		return result;
	}

	std::string canonicalJson() const
	{
		std::string payload = canonicalPayload();
		if (!payload.empty() && payload.back() == '}')
			payload.pop_back();
		payload += ",\"fingerprint\":\"" + fingerprint() + "\"}";
		return payload;
	}

	std::string fingerprint() const
	{
		const std::string payload = canonicalPayload();
		std::uint64_t hash = 14695981039346656037ULL;
		for (const unsigned char byte : payload)
		{
			hash ^= byte;
			hash *= 1099511628211ULL;
		}
		std::ostringstream out;
		out << std::hex << std::setfill('0') << std::setw(16) << hash;
		return out.str();
	}

private:
	static std::uint64_t saturatedAdd(const std::uint64_t left,
		const std::uint64_t right)
	{
		return right > (std::numeric_limits<std::uint64_t>::max)() - left ?
			(std::numeric_limits<std::uint64_t>::max)() : left + right;
	}

	static std::size_t providerIndexFor(const Provider provider)
	{
		return provider == Provider::WAYPOINT ? 0 :
			provider == Provider::NAVMESH ? 1 :
			provider == Provider::LOCAL_EXPLORER ? 2 : 3;
	}

	static std::size_t signalIndex(const Signal signal)
	{
		return signal == Signal::STUCK_HEURISTIC ? 0 :
			signal == Signal::CAPTURE_ROUTE_EXHAUSTED ? 1 : 2;
	}

	static std::size_t resultIndexFor(const Result result)
	{
		return static_cast<std::size_t>(result);
	}

	static bool validTransitionProvider(const Provider provider)
	{
		return provider == Provider::WAYPOINT || provider == Provider::NAVMESH ||
			provider == Provider::LOCAL_EXPLORER;
	}

	bool validTransition(const NavigationTransitionObservation& observation) const
	{
		const bool resultValid = static_cast<unsigned int>(observation.result) <=
			static_cast<unsigned int>(TransitionResult::UNKNOWN);
		const bool cancellationValid =
			static_cast<unsigned int>(observation.cancellation) <=
			static_cast<unsigned int>(CancellationKind::EXTERNAL);
		const bool traversalValid = static_cast<unsigned int>(observation.traversal) <=
			static_cast<unsigned int>(Traversal::UNKNOWN);
		const bool nodePairValid = observation.transitionKnown ?
			(observation.fromNode >= 0 && observation.toNode >= 0 &&
			 observation.fromNode != observation.toNode &&
			 observation.traversal != Traversal::NONE) :
			(observation.fromNode == -1 && observation.toNode == -1 &&
			 observation.traversal == Traversal::UNKNOWN &&
			 (observation.result == TransitionResult::UNKNOWN ||
			  observation.result == TransitionResult::CANCELLED));
		const bool cancellationSemantics =
			(observation.result == TransitionResult::CANCELLED) ?
				observation.cancellation != CancellationKind::NONE :
				observation.cancellation == CancellationKind::NONE;
		return !observation.mapIdentity.empty() &&
			observation.mapIdentity.size() <= 64 &&
			validTransitionProvider(observation.provider) &&
			traversalValid && resultValid && cancellationValid && nodePairValid &&
			cancellationSemantics && observation.intentId != 0 &&
			observation.attemptId != 0 && observation.botId >= 0 &&
			std::isfinite(observation.durationSeconds) &&
			observation.durationSeconds >= 0.0 &&
			observation.durationSeconds <= 86400.0 &&
			std::isfinite(observation.progressUnits) &&
			observation.progressUnits >= 0.0 &&
			observation.progressUnits <= 10000000.0;
	}

	static bool isTransitionFailure(const TransitionResult result)
	{
		return result == TransitionResult::NO_PROGRESS ||
			result == TransitionResult::STRUCTURAL_PATH_FAILURE;
	}

	void countTransitionCancellation(
		const NavigationTransitionObservation& observation)
	{
		if (observation.result != TransitionResult::CANCELLED)
			return;
		const std::size_t index =
			static_cast<std::size_t>(observation.cancellation) - 1;
		m_transitionCancellationCounts[index] = saturatedAdd(
			m_transitionCancellationCounts[index], 1);
	}

	static bool transitionRecentLess(const RecentTransitionResult& left,
		const RecentTransitionResult& right)
	{
		return std::tie(left.timestampMs, left.attemptId,
			left.attemptGeneration, left.result, left.cancellation) <
			std::tie(right.timestampMs, right.attemptId,
				right.attemptGeneration, right.result, right.cancellation);
	}

	void insertBot(std::set<int>& ids, bool& truncated, const int botId)
	{
		if (ids.find(botId) != ids.end())
			return;
		if (ids.size() < m_limits.maxBotIdentitiesPerEntry)
			ids.insert(botId);
		else
		{
			truncated = true;
			if (botId < *ids.rbegin())
			{
				ids.erase(std::prev(ids.end()));
				ids.insert(botId);
			}
		}
	}

	static void countTransitionResult(TransitionEntry& entry,
		const NavigationTransitionObservation& observation)
	{
		switch (observation.result)
		{
		case TransitionResult::TRANSITION_SUCCESS:
			entry.successes = saturatedAdd(entry.successes, 1);
			break;
		case TransitionResult::NO_PROGRESS:
			entry.noProgress = saturatedAdd(entry.noProgress, 1);
			break;
		case TransitionResult::STRUCTURAL_PATH_FAILURE:
			entry.structuralFailures = saturatedAdd(entry.structuralFailures, 1);
			break;
		case TransitionResult::CANCELLED:
			entry.cancellations = saturatedAdd(entry.cancellations, 1);
			if (observation.cancellation == CancellationKind::REPLACEMENT)
				entry.cancellationReplacement = saturatedAdd(
					entry.cancellationReplacement, 1);
			else if (observation.cancellation == CancellationKind::LIFECYCLE)
				entry.cancellationLifecycle = saturatedAdd(
					entry.cancellationLifecycle, 1);
			else if (observation.cancellation == CancellationKind::EXTERNAL)
				entry.cancellationExternal = saturatedAdd(
					entry.cancellationExternal, 1);
			break;
		default:
			entry.unknown = saturatedAdd(entry.unknown, 1);
			break;
		}
	}

	void insertTransitionRecent(std::vector<RecentTransitionResult>& results,
		const NavigationTransitionObservation& observation) const
	{
		results.push_back(RecentTransitionResult{observation.timestampMs,
			observation.attemptId, observation.attemptGeneration,
			observation.result, observation.cancellation});
		std::sort(results.begin(), results.end(), transitionRecentLess);
		if (results.size() > m_limits.maxTransitionRecentResultsPerEntry)
			results.erase(results.begin(), results.begin() +
				(results.size() - m_limits.maxTransitionRecentResultsPerEntry));
	}

	static void recomputeConsecutiveTransitionFailures(TransitionEntry& entry)
	{
		entry.consecutiveFailures = 0;
		for (const RecentTransitionResult& recent : entry.recentResults)
		{
			if (isTransitionFailure(recent.result))
				++entry.consecutiveFailures;
			else
				entry.consecutiveFailures = 0;
		}
	}

	void evictLeastRecentlySeenTransition()
	{
		if (m_transitionEntries.empty())
			return;
		auto victim = m_transitionEntries.begin();
		for (auto it = std::next(m_transitionEntries.begin());
			it != m_transitionEntries.end(); ++it)
			if (it->second.lastSeenMs < victim->second.lastSeenMs)
				victim = it;
		m_transitionEntries.erase(victim);
		m_transitionEvictions = saturatedAdd(m_transitionEvictions, 1);
	}

	bool valid(const Observation& observation) const
	{
		const Key& key = observation.key;
		const bool providerValid = key.provider == Provider::WAYPOINT ||
			key.provider == Provider::NAVMESH ||
			key.provider == Provider::LOCAL_EXPLORER;
		const bool traversalValid = static_cast<unsigned int>(key.traversal) <=
			static_cast<unsigned int>(Traversal::STEP_DOWN);
		const bool resultValid = static_cast<unsigned int>(observation.result) <=
			static_cast<unsigned int>(Result::UNKNOWN);
		return !key.map.empty() && key.map.size() <= 64 && providerValid &&
			traversalValid && resultValid &&
			(key.startNode >= 0 || key.destinationNode >= 0) &&
			observation.botId >= 0 && observation.attemptId != 0 &&
			std::isfinite(observation.traversalSeconds) &&
			observation.traversalSeconds >= 0.0 &&
			observation.traversalSeconds <= 86400.0 &&
			std::isfinite(observation.progressUnits) &&
			observation.progressUnits >= 0.0 &&
			observation.progressUnits <= 10000000.0;
	}

	static bool isFailure(const Result result)
	{
		return result == Result::PATH_FAILED || result == Result::PATH_STUCK ||
			result == Result::UNREACHABLE;
	}

	static bool recentLess(const RecentResult& left, const RecentResult& right)
	{
		return std::tie(left.timestampMs, left.attemptId, left.botId, left.result) <
			std::tie(right.timestampMs, right.attemptId, right.botId, right.result);
	}

	void countResult(Entry& entry, const Observation& observation)
	{
		switch (observation.result)
		{
		case Result::SUCCESS: entry.successes = saturatedAdd(entry.successes, 1); break;
		case Result::PATH_FAILED: entry.pathFailed = saturatedAdd(entry.pathFailed, 1); break;
		case Result::PATH_STUCK: entry.stuck = saturatedAdd(entry.stuck, 1); break;
		case Result::UNREACHABLE: entry.unreachable = saturatedAdd(entry.unreachable, 1); break;
		case Result::CANCELLED_EXTERNAL: entry.cancelledExternal = saturatedAdd(entry.cancelledExternal, 1); break;
		case Result::CANCELLED_REPLACEMENT: entry.cancelledReplacement = saturatedAdd(entry.cancelledReplacement, 1); break;
		case Result::CANCELLED_LIFECYCLE: entry.cancelledLifecycle = saturatedAdd(entry.cancelledLifecycle, 1); break;
		default: entry.unknown = saturatedAdd(entry.unknown, 1); break;
		}
		const bool newer = entry.lastResultMs < observation.timestampMs ||
			(entry.lastResultMs == observation.timestampMs &&
			 entry.lastAttemptId <= observation.attemptId);
		if (newer)
		{
			entry.lastResultMs = observation.timestampMs;
			entry.lastAttemptId = observation.attemptId;
			entry.lastResult = observation.result;
		}
	}

	void insertRecent(Entry& entry, const Observation& observation)
	{
		RecentResult recent{ observation.timestampMs, observation.attemptId,
			observation.botId, observation.result };
		std::vector<RecentResult>& results = entry.recentResults;
		results.push_back(recent);
		std::sort(results.begin(), results.end(), recentLess);
		if (results.size() > m_limits.maxRecentResultsPerEntry)
			results.erase(results.begin(), results.begin() +
				(results.size() - m_limits.maxRecentResultsPerEntry));
		entry.consecutiveFailures = 0;
		for (auto it = results.rbegin(); it != results.rend(); ++it)
		{
			if (isFailure(it->result))
				++entry.consecutiveFailures;
			else
				break;
		}
	}

	void evictLeastRecentlySeen()
	{
		if (m_entries.empty())
			return;
		auto victim = m_entries.begin();
		for (auto it = std::next(m_entries.begin()); it != m_entries.end(); ++it)
			if (it->second.lastSeenMs < victim->second.lastSeenMs)
				victim = it;
		m_entries.erase(victim);
		m_evictions = saturatedAdd(m_evictions, 1);
	}

	static std::string jsonString(const std::string& value)
	{
		std::string out = "\"";
		for (const unsigned char ch : value)
		{
			switch (ch)
			{
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20)
				{
					char escaped[8] = {};
					snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
					out += escaped;
				}
				else out += static_cast<char>(ch);
			}
		}
		out += '"';
		return out;
	}

	static std::string keyJson(const Key& key)
	{
		return "{\"map\":" + jsonString(key.map) +
			",\"provider\":" + jsonString(ProviderName(key.provider)) +
			",\"start_node\":" + std::to_string(key.startNode) +
			",\"destination_node\":" + std::to_string(key.destinationNode) +
			",\"traversal\":" + jsonString(TraversalName(key.traversal)) + "}";
	}

	static std::string transitionKeyJson(const TransitionKey& key)
	{
		return "{\"map_identity\":" + jsonString(key.map) +
			",\"provider\":" + jsonString(ProviderName(key.provider)) +
			",\"from_node\":" + std::to_string(key.fromNode) +
			",\"to_node\":" + std::to_string(key.toNode) +
			",\"traversal_kind\":" +
			jsonString(TraversalName(key.traversal)) + "}";
	}

	std::string transitionExperienceJson() const
	{
		std::string out = "{\"limits\":{\"max_entries\":" +
			std::to_string(m_limits.maxTransitionEntries) +
			",\"max_recent_results_per_entry\":" +
			std::to_string(m_limits.maxTransitionRecentResultsPerEntry) +
			",\"hotspot_minimum_attempts\":" +
			std::to_string(kMinimumTransitionHotspotAttempts) +
			",\"hotspot_minimum_failures\":" +
			std::to_string(kMinimumTransitionHotspotFailures) +
			",\"hotspot_minimum_failure_bots\":" +
			std::to_string(kMinimumTransitionHotspotBots) +
			"},\"totals\":{\"observations\":" +
			std::to_string(m_transitionTotalObservations) +
			",\"unique_transition_keys\":" +
			std::to_string(m_transitionEntries.size()) +
			",\"successes\":" + std::to_string(transitionResultCount(
				TransitionResult::TRANSITION_SUCCESS)) +
			",\"no_progress\":" + std::to_string(transitionResultCount(
				TransitionResult::NO_PROGRESS)) +
			",\"structural_failures\":" + std::to_string(
				transitionResultCount(TransitionResult::STRUCTURAL_PATH_FAILURE)) +
			",\"cancellations\":" + std::to_string(transitionResultCount(
				TransitionResult::CANCELLED)) +
			",\"cancellation_replacement\":" +
			std::to_string(m_transitionCancellationCounts[0]) +
			",\"cancellation_lifecycle\":" +
			std::to_string(m_transitionCancellationCounts[1]) +
			",\"cancellation_external\":" +
			std::to_string(m_transitionCancellationCounts[2]) +
			",\"unknown\":" + std::to_string(transitionResultCount(
				TransitionResult::UNKNOWN)) +
			",\"unknown_transitions\":" +
			std::to_string(m_unknownTransitionObservations) +
			",\"evictions\":" + std::to_string(m_transitionEvictions) +
			"},\"entries\":[";
		bool first = true;
		for (const auto& pair : m_transitionEntries)
		{
			if (!first) out += ',';
			first = false;
			const TransitionEntry& entry = pair.second;
			out += "{\"key\":" + transitionKeyJson(entry.key) +
				",\"attempts\":" + std::to_string(entry.attempts) +
				",\"successes\":" + std::to_string(entry.successes) +
				",\"no_progress\":" + std::to_string(entry.noProgress) +
				",\"structural_failures\":" +
				std::to_string(entry.structuralFailures) +
				",\"cancellations\":" + std::to_string(entry.cancellations) +
				",\"cancellation_replacement\":" +
				std::to_string(entry.cancellationReplacement) +
				",\"cancellation_lifecycle\":" +
				std::to_string(entry.cancellationLifecycle) +
				",\"cancellation_external\":" +
				std::to_string(entry.cancellationExternal) +
				",\"unknown\":" + std::to_string(entry.unknown) +
				",\"unique_bots\":[";
			bool firstBot = true;
			for (const int botId : entry.botIds)
			{
				if (!firstBot) out += ',';
				firstBot = false;
				out += std::to_string(botId);
			}
			out += "],\"unique_bots_truncated\":" +
				std::string(entry.botIdsTruncated ? "true" : "false") +
				",\"unique_failure_bots\":[";
			firstBot = true;
			for (const int botId : entry.failureBotIds)
			{
				if (!firstBot) out += ',';
				firstBot = false;
				out += std::to_string(botId);
			}
			out += "],\"unique_failure_bots_truncated\":" +
				std::string(entry.failureBotIdsTruncated ? "true" : "false") +
				",\"consecutive_failures\":" +
				std::to_string(entry.consecutiveFailures) +
				",\"last_seen_ms\":" + std::to_string(entry.lastSeenMs) +
				",\"total_duration_ms\":" +
				std::to_string(entry.totalDurationMs) +
				",\"total_progress_milli_units\":" +
				std::to_string(entry.totalProgressMilliUnits) + "}";
		}
		out += "],\"hotspots\":[";
		first = true;
		for (const TransitionHotspot& hotspot : transitionHotspots())
		{
			if (!first) out += ',';
			first = false;
			out += "{\"key\":" + transitionKeyJson(hotspot.key) +
				",\"attempts\":" + std::to_string(hotspot.attempts) +
				",\"successes\":" + std::to_string(hotspot.successes) +
				",\"failures\":" + std::to_string(hotspot.failures) +
				",\"no_progress\":" + std::to_string(hotspot.noProgress) +
				",\"structural_failures\":" +
				std::to_string(hotspot.structuralFailures) +
				",\"affected_bots\":" +
				std::to_string(hotspot.affectedBots) +
				",\"affected_failure_bots\":" +
				std::to_string(hotspot.affectedFailureBots) +
				",\"affected_bots_truncated\":" +
				std::string(hotspot.affectedBotsTruncated ? "true" : "false") +
				",\"affected_failure_bots_truncated\":" +
				std::string(hotspot.affectedFailureBotsTruncated ? "true" : "false") +
				"}";
		}
		out += "]}";
		return out;
	}

	std::string canonicalPayload() const
	{
		const char* mode = m_learnedCostsEnabled ? "ACTIVE_COSTS" : "SHADOW_ONLY";
		std::string out = std::string("{\"schema_version\":2,\"mode\":\"") + mode + "\",\"map_identity\":" +
			jsonString(m_mapIdentity) + ",\"navigation_data_identity\":" +
			jsonString(m_navigationDataIdentity) +
			",\"limits\":{\"max_entries\":" + std::to_string(m_limits.maxEntries) +
			",\"max_hotspots\":" + std::to_string(m_limits.maxHotspots) +
			",\"max_bot_identities_per_entry\":" +
			std::to_string(m_limits.maxBotIdentitiesPerEntry) +
			",\"max_recent_results_per_entry\":" +
			std::to_string(m_limits.maxRecentResultsPerEntry) +
			"},\"totals\":{\"observations\":" +
			std::to_string(m_totalObservations) + ",\"successes\":" +
			std::to_string(resultCount(Result::SUCCESS)) +
			",\"path_failed\":" + std::to_string(resultCount(Result::PATH_FAILED)) +
			",\"stuck\":" + std::to_string(resultCount(Result::PATH_STUCK)) +
			",\"unreachable\":" + std::to_string(resultCount(Result::UNREACHABLE)) +
			",\"cancelled_external\":" +
			std::to_string(resultCount(Result::CANCELLED_EXTERNAL)) +
			",\"cancelled_replacement\":" +
			std::to_string(resultCount(Result::CANCELLED_REPLACEMENT)) +
			",\"cancelled_lifecycle\":" +
			std::to_string(resultCount(Result::CANCELLED_LIFECYCLE)) +
			",\"unknown\":" + std::to_string(resultCount(Result::UNKNOWN)) +
			",\"true_failures\":" + std::to_string(
				resultCount(Result::PATH_FAILED) + resultCount(Result::PATH_STUCK) +
				resultCount(Result::UNREACHABLE)) + ",\"experience_keys\":" +
			std::to_string(m_entries.size()) + ",\"evictions\":" +
			std::to_string(m_evictions) + "},\"provider_distribution\":{\"WAYPOINT\":" +
			std::to_string(m_providerDistribution[0]) + ",\"NAVMESH\":" +
			std::to_string(m_providerDistribution[1]) + ",\"LOCAL_EXPLORER\":" +
			std::to_string(m_providerDistribution[2]) + ",\"UNKNOWN\":" +
			std::to_string(m_providerDistribution[3]) + "},\"entries\":[";
		bool first = true;
		for (const auto& pair : m_entries)
		{
			if (!first) out += ',';
			first = false;
			const Entry& e = pair.second;
			out += "{\"key\":" + keyJson(e.key) +
				",\"attempts\":" + std::to_string(e.attempts) +
				",\"successes\":" + std::to_string(e.successes) +
				",\"path_failed\":" + std::to_string(e.pathFailed) +
				",\"stuck\":" + std::to_string(e.stuck) +
				",\"unreachable\":" + std::to_string(e.unreachable) +
				",\"cancelled_external\":" + std::to_string(e.cancelledExternal) +
				",\"cancelled_replacement\":" + std::to_string(e.cancelledReplacement) +
				",\"cancelled_lifecycle\":" + std::to_string(e.cancelledLifecycle) +
				",\"unknown\":" + std::to_string(e.unknown) +
				",\"total_traversal_ms\":" + std::to_string(e.totalTraversalMs) +
				",\"total_progress_milli_units\":" +
				std::to_string(e.totalProgressMilliUnits) +
				",\"consecutive_failures\":" + std::to_string(e.consecutiveFailures) +
				",\"last_result\":" + jsonString(ResultName(e.lastResult)) +
				",\"last_seen_ms\":" + std::to_string(e.lastSeenMs) +
				",\"affected_bots\":[";
			bool firstBot = true;
			for (const int botId : e.botIds)
			{
				if (!firstBot) out += ',';
				firstBot = false;
				out += std::to_string(botId);
			}
			out += "],\"affected_bots_truncated\":" +
				std::string(e.botIdsTruncated ? "true" : "false") +
				",\"correlated_signals\":{\"STUCK_HEURISTIC\":" +
				std::to_string(e.correlatedSignals[0]) +
				",\"CAPTURE_ROUTE_EXHAUSTED\":" +
				std::to_string(e.correlatedSignals[1]) +
				",\"SQUAD_RECOVERY_FAILED\":" +
				std::to_string(e.correlatedSignals[2]) + "}}";
		}
		out += "],\"hotspots\":[";
		first = true;
		for (const Hotspot& h : hotspots())
		{
			if (!first) out += ',';
			first = false;
			out += "{\"event\":\"NAV_HOTSPOT\",\"key\":" + keyJson(h.key) +
				",\"provider\":" + jsonString(ProviderName(h.key.provider)) +
				",\"attempts\":" + std::to_string(h.attempts) +
				",\"successes\":" + std::to_string(h.successes) +
				",\"failures\":" + std::to_string(h.failures) +
				",\"stuck\":" + std::to_string(h.stuck) +
				",\"unreachable\":" + std::to_string(h.unreachable) +
				",\"affected_bots\":" + std::to_string(h.affectedBots) +
				",\"affected_bots_truncated\":" +
				std::string(h.affectedBotsTruncated ? "true" : "false") +
				",\"failure_ratio_basis_points\":" +
				std::to_string(h.failureRatioBasisPoints) +
				",\"severity\":" + jsonString(SeverityName(h.severity)) + "}";
		}
		out += "],\"transition_experience\":" + transitionExperienceJson() +
			",\"learned_navigation_behavior_changes\":" +
			std::to_string(m_learnedNavigationBehaviorChanges) +
			",\"learned_edges_with_penalty\":" +
			std::to_string(m_learnedEdgesWithPenalty) +
			",\"maximum_observed_penalty\":" +
			std::to_string(m_maximumObservedPenalty) +
			",\"average_nonzero_penalty\":" +
			std::to_string(m_averageObservedPenalty) +
			",\"learned_costs_enabled\":" +
			std::string(m_learnedCostsEnabled ? "true" : "false") + "}";
		return out;
	}

	Limits m_limits;
	std::string m_mapIdentity = "unknown";
	std::string m_navigationDataIdentity = "unknown";
	std::map<Key, Entry> m_entries;
	std::map<TransitionKey, TransitionEntry> m_transitionEntries;
	std::vector<RecentTransitionResult> m_unknownTransitionResults;
	// Retained terminal detail is bounded per entry and used only to make the
	// consecutive-failure statistic independent of callback arrival order.
	std::array<std::uint64_t, 4> m_providerDistribution{};
	std::array<std::uint64_t, 8> m_resultCounts{};
	std::array<std::uint64_t, 5> m_transitionResultCounts{};
	std::array<std::uint64_t, 3> m_transitionCancellationCounts{};
	std::uint64_t m_totalObservations = 0;
	std::uint64_t m_evictions = 0;
	std::uint64_t m_transitionTotalObservations = 0;
	std::uint64_t m_unknownTransitionObservations = 0;
	std::uint64_t m_transitionEvictions = 0;
	std::uint64_t m_learnedNavigationBehaviorChanges = 0;
	std::uint64_t m_learnedEdgesWithPenalty = 0;
	double m_maximumObservedPenalty = 0.0;
	double m_averageObservedPenalty = 0.0;
	bool m_learnedCostsEnabled = false;
};
}

#endif // __RCBOT_NAVIGATION_EXPERIENCE_H__
