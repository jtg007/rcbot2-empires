#ifndef __RCBOT_LOCAL_YIELDING_H__
#define __RCBOT_LOCAL_YIELDING_H__

#include "bot_local_conflict_observation.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace RCBotLocalYielding
{
	enum class Role : std::uint8_t
	{
		NONE = 0,
		PROCEED,
		WAIT
	};

	const char* RoleName(Role role);

	enum class Reason : std::uint8_t
	{
		NONE = 0,
		PRECISION_TRAVERSER,
		PRIOR_WAITER,
		STATIONARY_MOVING_BOT,
		STABLE_BOT_ID,
		STICKY_DECISION,
		BOUNDED_REEVALUATION,
		STARVATION_ROTATE,
		BOTH_PRECISION_BLOCKED,
		WAIT_RESOLVED,
		WAIT_TIMEOUT_UNRESOLVED,
		BOTH_GEOMETRICALLY_BLOCKED,
		PRECISION_BLOCKED,
		OTHER
	};

	const char* ReasonName(Reason reason);

	enum class Event : std::uint8_t
	{
		NONE = 0,
		DECISION,
		BEGIN,
		REEVALUATE,
		END,
		STARVATION_ROTATE
	};

	const char* EventName(Event event);

	// These defaults are intentionally centralized. The navigation attempt
	// watchdog is 3 seconds; a 4 second maximum gives a real waiter a bounded
	// opportunity without making an intentional pause unbounded.
	struct LocalYieldPolicy
	{
		float stickySeconds = 0.75f;
		float maxWaitSeconds = 4.0f;
	};

	struct LocalYieldState
	{
		bool active = false;
		int botId = -1;
		int partnerBotId = -1;
		std::uint64_t conflictEpisodeId = 0;
		Role role = Role::NONE;
		double startedAt = 0.0;
		double reevaluateAt = 0.0;
		double maxWaitDeadline = 0.0;
		float waitAge = 0.0f;
		std::uint64_t priorityEpoch = 0;
		Reason reason = Reason::NONE;
	};

	struct Evaluation
	{
		Event event = Event::NONE;
		bool active = false;
		bool changed = false;
		int lowBot = -1;
		int highBot = -1;
		int winnerBot = -1;
		int waiterBot = -1;
		LocalYieldState proceed;
		LocalYieldState wait;
		RCBotLocalAvoidance::LocalBotConflictObservation observation;
		RCBotLocalAvoidance::LocalConflictEpisode episode;
	};

	class Controller
	{
	public:
		static constexpr std::size_t kMaxPairStates = 128;
		static constexpr std::size_t kMaxTrackedBots = 128;

		Evaluation Observe(
			const RCBotLocalAvoidance::LocalBotConflictObservation& observation,
			const RCBotLocalAvoidance::LocalConflictEpisode& episode,
			double now, const LocalYieldPolicy& policy = LocalYieldPolicy{});
		Evaluation End(const RCBotLocalAvoidance::LocalConflictEpisode& episode,
			double now, Reason reason = Reason::WAIT_RESOLVED);
		void ClearBot(int botId);
		void Reset();

		bool IsWaiting(int botId) const;
		const LocalYieldState* StateFor(int botId) const;
		std::size_t StateCount() const;

	private:
		struct PairState
		{
			bool active = false;
			int lowBot = -1;
			int highBot = -1;
			RCBotLocalAvoidance::ConflictType type =
				RCBotLocalAvoidance::ConflictType::UNKNOWN;
			std::uint64_t conflictEpisodeId = 0;
			LocalYieldState low;
			LocalYieldState high;
		};

		struct BotWaitAge
		{
			bool active = false;
			int botId = -1;
			float seconds = 0.0f;
		};

		static bool activeConflict(RCBotLocalAvoidance::ConflictType type);
		int findPair(int lowBot, int highBot) const;
		int findFreePair();
		int findBotAge(int botId) const;
		float previousWaitAge(int botId) const;
		void addWaitAge(int botId, float seconds);
		void clearWaitAge(int botId);
		static void fillEvaluation(Evaluation& result, const PairState& pair,
			const RCBotLocalAvoidance::LocalBotConflictObservation& observation,
			const RCBotLocalAvoidance::LocalConflictEpisode& episode);
		static void updateWaitAge(PairState& pair, double now);
		void assignRoles(PairState& pair,
			const RCBotLocalAvoidance::LocalBotConflictObservation& observation,
			const RCBotLocalAvoidance::LocalConflictEpisode& episode,
			double now, const LocalYieldPolicy& policy, int forcedWinner,
			Reason reason);

		std::array<PairState, kMaxPairStates> m_pairs{};
		std::array<BotWaitAge, kMaxTrackedBots> m_waitAges{};
	};

	const LocalYieldPolicy& Policy();
	void Reset();
	Evaluation Observe(
		const RCBotLocalAvoidance::LocalBotConflictObservation& observation,
		const RCBotLocalAvoidance::LocalConflictEpisode& episode,
		double now);
	Evaluation End(const RCBotLocalAvoidance::LocalConflictEpisode& episode,
		double now, Reason reason = Reason::WAIT_RESOLVED);
	void ClearBot(int botId);
	bool IsWaiting(int botId);
	const LocalYieldState* StateFor(int botId);
	std::size_t StateCount();
}

#endif // __RCBOT_LOCAL_YIELDING_H__
