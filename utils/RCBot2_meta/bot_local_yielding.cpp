#include "bot_local_yielding.h"

#include <algorithm>
#include <limits>

namespace
{
	using RCBotLocalAvoidance::ConflictType;
	const RCBotLocalYielding::LocalYieldPolicy g_policy{};
RCBotLocalYielding::Controller g_controller;

	bool isPrecision(const RCBotLocalAvoidance::LocalBotConflictObservation& observation,
		const int botId)
	{
		return (botId == observation.observerBot &&
			observation.observerPrecisionContext) ||
			(botId == observation.otherBot && observation.otherPrecisionContext);
	}
}

namespace RCBotLocalYielding
{
	const char* RoleName(const Role role)
	{
		switch (role)
		{
		case Role::NONE: return "NONE";
		case Role::PROCEED: return "PROCEED";
		case Role::WAIT: return "WAIT";
		}
		return "NONE";
	}

	const char* ReasonName(const Reason reason)
	{
		switch (reason)
		{
		case Reason::NONE: return "NONE";
		case Reason::PRECISION_TRAVERSER: return "PRECISION_TRAVERSER";
		case Reason::PRIOR_WAITER: return "PRIOR_WAITER";
		case Reason::STATIONARY_MOVING_BOT: return "STATIONARY_MOVING_BOT";
		case Reason::STABLE_BOT_ID: return "STABLE_BOT_ID";
		case Reason::STICKY_DECISION: return "STICKY_DECISION";
		case Reason::BOUNDED_REEVALUATION: return "BOUNDED_REEVALUATION";
		case Reason::STARVATION_ROTATE: return "STARVATION_ROTATE";
		case Reason::BOTH_PRECISION_BLOCKED: return "BOTH_PRECISION_BLOCKED";
		case Reason::WAIT_RESOLVED: return "WAIT_RESOLVED";
		case Reason::WAIT_TIMEOUT_UNRESOLVED:
			return "WAIT_TIMEOUT_UNRESOLVED";
		case Reason::BOTH_GEOMETRICALLY_BLOCKED:
			return "BOTH_GEOMETRICALLY_BLOCKED";
		case Reason::PRECISION_BLOCKED: return "PRECISION_BLOCKED";
		case Reason::OTHER: return "OTHER";
		}
		return "OTHER";
	}

	const char* EventName(const Event event)
	{
		switch (event)
		{
		case Event::NONE: return "NONE";
		case Event::DECISION: return "ALLY_YIELD_DECISION";
		case Event::BEGIN: return "ALLY_YIELD_BEGIN";
		case Event::REEVALUATE: return "ALLY_YIELD_REEVALUATE";
		case Event::END: return "ALLY_YIELD_END";
		case Event::STARVATION_ROTATE:
			return "ALLY_YIELD_STARVATION_ROTATE";
		}
		return "NONE";
	}

	bool Controller::activeConflict(const ConflictType type)
	{
		// CROSSING, FOLLOW_BLOCK and UNKNOWN remain observation-only in v1.
		return type == ConflictType::HEAD_ON ||
			type == ConflictType::STATIONARY_BLOCK ||
			type == ConflictType::CHOKE_CONFLICT ||
			type == ConflictType::PRECISION_TRAVERSAL_CONFLICT ||
			type == ConflictType::WORLD_PLUS_ALLY_BLOCK;
	}

	int Controller::findPair(const int lowBot, const int highBot) const
	{
		for (std::size_t i = 0; i < m_pairs.size(); ++i)
			if (m_pairs[i].active && m_pairs[i].lowBot == lowBot &&
				m_pairs[i].highBot == highBot)
				return static_cast<int>(i);
		return -1;
	}

	int Controller::findFreePair()
	{
		for (std::size_t i = 0; i < m_pairs.size(); ++i)
			if (!m_pairs[i].active)
				return static_cast<int>(i);
		return -1;
	}

	int Controller::findBotAge(const int botId) const
	{
		for (std::size_t i = 0; i < m_waitAges.size(); ++i)
			if (m_waitAges[i].active && m_waitAges[i].botId == botId)
				return static_cast<int>(i);
		return -1;
	}

	float Controller::previousWaitAge(const int botId) const
	{
		const int index = findBotAge(botId);
		return index >= 0 ? m_waitAges[static_cast<std::size_t>(index)].seconds : 0.0f;
	}

	void Controller::addWaitAge(const int botId, const float seconds)
	{
		if (botId < 0 || seconds <= 0.0f)
			return;
		int index = findBotAge(botId);
		if (index < 0)
		{
			for (std::size_t i = 0; i < m_waitAges.size(); ++i)
				if (!m_waitAges[i].active)
				{
					index = static_cast<int>(i);
					break;
				}
		}
		if (index < 0)
		{
			index = 0;
			for (std::size_t i = 1; i < m_waitAges.size(); ++i)
				if (m_waitAges[i].seconds < m_waitAges[static_cast<std::size_t>(index)].seconds)
					index = static_cast<int>(i);
		}
		BotWaitAge& age = m_waitAges[static_cast<std::size_t>(index)];
		age.active = true;
		age.botId = botId;
		age.seconds = (std::min)(age.seconds + seconds, 60.0f);
	}

	void Controller::clearWaitAge(const int botId)
	{
		const int index = findBotAge(botId);
		if (index >= 0)
			m_waitAges[static_cast<std::size_t>(index)] = BotWaitAge{};
	}

	void Controller::updateWaitAge(PairState& pair, const double now)
	{
		if (!pair.active)
			return;
		if (pair.low.role == Role::WAIT)
			pair.low.waitAge = static_cast<float>((std::max)(0.0,
				now - pair.low.startedAt));
		else if (pair.high.role == Role::WAIT)
			pair.high.waitAge = static_cast<float>((std::max)(0.0,
				now - pair.high.startedAt));
	}

	void Controller::fillEvaluation(Evaluation& result, const PairState& pair,
		const RCBotLocalAvoidance::LocalBotConflictObservation& observation,
		const RCBotLocalAvoidance::LocalConflictEpisode& episode)
	{
		result.active = pair.active;
		result.lowBot = pair.lowBot;
		result.highBot = pair.highBot;
		result.proceed = pair.low.role == Role::PROCEED ? pair.low : pair.high;
		result.wait = pair.low.role == Role::WAIT ? pair.low : pair.high;
		result.winnerBot = result.proceed.botId;
		result.waiterBot = result.wait.botId;
		result.observation = observation;
		result.episode = episode;
	}

	void Controller::assignRoles(PairState& pair,
		const RCBotLocalAvoidance::LocalBotConflictObservation& observation,
		const RCBotLocalAvoidance::LocalConflictEpisode& episode,
		const double now, const LocalYieldPolicy& policy, const int forcedWinner,
		const Reason reason)
	{
		const int low = pair.lowBot;
		const int high = pair.highBot;
		int winner = forcedWinner;
		Reason selectedReason = reason;

		if (winner < 0)
		{
			const bool lowPrecision = isPrecision(observation, low);
			const bool highPrecision = isPrecision(observation, high);
			if (lowPrecision != highPrecision)
			{
				winner = lowPrecision ? low : high;
				selectedReason = Reason::PRECISION_TRAVERSER;
			}
			else if (episode.type == ConflictType::STATIONARY_BLOCK &&
				observation.observerWantsMovement != observation.otherWantsMovement)
			{
				winner = observation.observerWantsMovement ?
					observation.observerBot : observation.otherBot;
				selectedReason = Reason::STATIONARY_MOVING_BOT;
			}
			else
			{
				const float lowAge = previousWaitAge(low);
				const float highAge = previousWaitAge(high);
				if (lowAge > highAge)
				{
					winner = low;
					selectedReason = Reason::PRIOR_WAITER;
				}
				else if (highAge > lowAge)
				{
					winner = high;
					selectedReason = Reason::PRIOR_WAITER;
				}
				else
				{
					winner = low < high ? low : high;
					selectedReason = Reason::STABLE_BOT_ID;
				}
			}
		}

		pair.low.active = true;
		pair.low.botId = low;
		pair.low.partnerBotId = high;
		pair.low.conflictEpisodeId = episode.episodeId;
		pair.low.role = winner == low ? Role::PROCEED : Role::WAIT;
		pair.low.startedAt = now;
		pair.low.reevaluateAt = now + (std::max)(0.01f, policy.stickySeconds);
		pair.low.maxWaitDeadline = now + (std::max)(policy.maxWaitSeconds,
			policy.stickySeconds);
		pair.low.waitAge = 0.0f;
		pair.low.priorityEpoch = pair.low.priorityEpoch == 0 ? 1 :
			pair.low.priorityEpoch + 1;
		pair.low.reason = selectedReason;

		pair.high = pair.low;
		pair.high.botId = high;
		pair.high.partnerBotId = low;
		pair.high.role = winner == high ? Role::PROCEED : Role::WAIT;
	}

	Evaluation Controller::Observe(
		const RCBotLocalAvoidance::LocalBotConflictObservation& observation,
		const RCBotLocalAvoidance::LocalConflictEpisode& episode,
		const double now, const LocalYieldPolicy& policy)
	{
		Evaluation result;
		if (!activeConflict(episode.type) || observation.observerBot < 0 ||
			observation.otherBot < 0 || observation.observerBot == observation.otherBot)
			return result;

		const int low = (std::min)(observation.observerBot, observation.otherBot);
		const int high = (std::max)(observation.observerBot, observation.otherBot);
		const bool lowPrecision = isPrecision(observation, low);
		const bool highPrecision = isPrecision(observation, high);
		if (lowPrecision && highPrecision)
		{
			result.event = Event::DECISION;
			result.changed = false;
			result.lowBot = low;
			result.highBot = high;
			result.winnerBot = -1;
			result.waiterBot = -1;
			result.observation = observation;
			result.episode = episode;
			result.wait.reason = Reason::BOTH_PRECISION_BLOCKED;
			return result;
		}

		int index = findPair(low, high);
		if (index < 0)
		{
			index = findFreePair();
			if (index < 0)
				return result;
			PairState& pair = m_pairs[static_cast<std::size_t>(index)];
			pair = PairState{};
			pair.active = true;
			pair.lowBot = low;
			pair.highBot = high;
			pair.type = episode.type;
			pair.conflictEpisodeId = episode.episodeId;
			assignRoles(pair, observation, episode, now, policy, -1,
				Reason::STABLE_BOT_ID);
			result.event = Event::BEGIN;
			result.changed = true;
			fillEvaluation(result, pair, observation, episode);
			return result;
		}

		PairState& pair = m_pairs[static_cast<std::size_t>(index)];
		if (pair.conflictEpisodeId != episode.episodeId || pair.type != episode.type)
		{
			// The passive tracker normally emits RESOLVED before BEGIN when the
			// context changes. Keep this guard safe for callers that only deliver
			// the latest sample.
			assignRoles(pair, observation, episode, now, policy, -1,
				Reason::BOUNDED_REEVALUATION);
			pair.type = episode.type;
			pair.conflictEpisodeId = episode.episodeId;
			result.event = Event::REEVALUATE;
			result.changed = true;
			fillEvaluation(result, pair, observation, episode);
			return result;
		}

		updateWaitAge(pair, now);
		if (now < pair.low.reevaluateAt && now < pair.high.reevaluateAt)
		{
			result.event = Event::NONE;
			fillEvaluation(result, pair, observation, episode);
			return result;
		}

		if (now >= pair.low.maxWaitDeadline || now >= pair.high.maxWaitDeadline)
		{
			const int oldWaiter = pair.low.role == Role::WAIT ? pair.low.botId :
				pair.high.botId;
			assignRoles(pair, observation, episode, now, policy, oldWaiter,
				Reason::STARVATION_ROTATE);
			clearWaitAge(oldWaiter);
			result.event = Event::STARVATION_ROTATE;
			result.changed = true;
		}
		else
		{
			pair.low.reevaluateAt = now + (std::max)(0.01f, policy.stickySeconds);
			pair.high.reevaluateAt = pair.low.reevaluateAt;
			pair.low.reason = Reason::BOUNDED_REEVALUATION;
			pair.high.reason = Reason::BOUNDED_REEVALUATION;
			result.event = Event::REEVALUATE;
			result.changed = false;
		}
		fillEvaluation(result, pair, observation, episode);
		return result;
	}

	Evaluation Controller::End(
		const RCBotLocalAvoidance::LocalConflictEpisode& episode,
		const double now, const Reason reason)
	{
		Evaluation result;
		const int index = findPair(episode.lowBot, episode.highBot);
		if (index < 0)
			return result;
		PairState& pair = m_pairs[static_cast<std::size_t>(index)];
		updateWaitAge(pair, now);
		const int waiter = pair.low.role == Role::WAIT ? pair.low.botId : pair.high.botId;
		const float age = pair.low.role == Role::WAIT ? pair.low.waitAge : pair.high.waitAge;
		addWaitAge(waiter, age);
		result.event = Event::END;
		result.changed = true;
		fillEvaluation(result, pair, episode.lastObservation, episode);
		result.wait.reason = reason;
		result.wait.waitAge = age;
		pair = PairState{};
		return result;
	}

	void Controller::ClearBot(const int botId)
	{
		if (botId < 0)
			return;
		for (PairState& pair : m_pairs)
			if (pair.active && (pair.lowBot == botId || pair.highBot == botId))
				pair = PairState{};
		clearWaitAge(botId);
	}

	void Controller::Reset()
	{
		m_pairs.fill(PairState{});
		m_waitAges.fill(BotWaitAge{});
	}

	bool Controller::IsWaiting(const int botId) const
	{
		for (const PairState& pair : m_pairs)
			if (pair.active && ((pair.low.botId == botId && pair.low.role == Role::WAIT) ||
				(pair.high.botId == botId && pair.high.role == Role::WAIT)))
				return true;
		return false;
	}

	const LocalYieldState* Controller::StateFor(const int botId) const
	{
		for (const PairState& pair : m_pairs)
		{
			if (!pair.active)
				continue;
			if (pair.low.botId == botId)
				return &pair.low;
			if (pair.high.botId == botId)
				return &pair.high;
		}
		return nullptr;
	}

	std::size_t Controller::StateCount() const
	{
		std::size_t count = 0;
		for (const PairState& pair : m_pairs)
			if (pair.active)
				++count;
		return count;
	}

	const LocalYieldPolicy& Policy() { return g_policy; }
	void Reset() { g_controller.Reset(); }
	Evaluation Observe(
		const RCBotLocalAvoidance::LocalBotConflictObservation& observation,
		const RCBotLocalAvoidance::LocalConflictEpisode& episode,
		const double now)
	{
		return g_controller.Observe(observation, episode, now, Policy());
	}
	Evaluation End(const RCBotLocalAvoidance::LocalConflictEpisode& episode,
		const double now, const Reason reason)
	{
		return g_controller.End(episode, now, reason);
	}
	void ClearBot(const int botId) { g_controller.ClearBot(botId); }
	bool IsWaiting(const int botId) { return g_controller.IsWaiting(botId); }
	const LocalYieldState* StateFor(const int botId)
	{
		return g_controller.StateFor(botId);
	}
	std::size_t StateCount() { return g_controller.StateCount(); }
}
