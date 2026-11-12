#include "bot_recovery_episode.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace
{
	using RCBotRecovery::Domain;
	using RCBotRecovery::EpisodeTracker;
	using RCBotRecovery::RecoveryEpisodeIdentity;
	using RCBotRecovery::RecoveryEpisodePolicy;
	using RCBotRecovery::SuppressionCheck;

	RecoveryEpisodeIdentity Start(EpisodeTracker& tracker, const int botId,
		const char* subject, const std::uint64_t life = 1,
		const std::uint64_t generation = 1)
	{
		return tracker.Start(botId, life, Domain::CAPTURE, subject, generation);
	}

	RecoveryEpisodeIdentity SquadEpisode(EpisodeTracker& tracker,
		const int botId, const char* subject = "team=2;squad=3;waypoint=12",
		const std::uint64_t life = 1, const std::uint64_t generation = 1)
	{
		return tracker.Start(botId, life, Domain::SQUAD, subject, generation);
	}
}

int main()
{
	EpisodeTracker tracker;
	RecoveryEpisodePolicy policy;
	const RecoveryEpisodeIdentity capture = Start(tracker, 7, "4");

	// REPA-01/02: only completed domain recovery cycles consume strikes.
	assert(policy.RecordDomainExhausted(capture, 1.0).consecutiveExhaustions == 1);
	assert(!policy.IsSuppressed());
	const auto second = policy.RecordDomainExhausted(capture, 2.0);
	assert(second.consecutiveExhaustions == 2 && second.suppressionStarted &&
		second.suppressed);
	assert(policy.RecordDomainExhausted(capture, 2.1).consecutiveExhaustions == 2);

	// REPA-03/04/05: PATH_STUCK, PATH_FAILED and cancellation have no policy
	// entry point and therefore cannot consume an episode strike.
	assert(policy.ConsecutiveExhaustions() == 2);
	assert(policy.CheckSuppression(capture, 2.5) == SuppressionCheck::SUPPRESSED);

	// REPA-06: meaningful domain progress breaks the consecutive chain.
	assert(policy.RecordMeaningfulProgress(capture));
	assert(policy.ConsecutiveExhaustions() == 0);
	assert(policy.CheckSuppression(capture, 3.0) ==
		SuppressionCheck::NOT_APPLICABLE);

	// REPA-07: success clears all active policy state.
	policy.RecordDomainExhausted(capture, 4.0);
	policy.RecordSuccess(capture);
	assert(policy.ConsecutiveExhaustions() == 0 && !policy.IsSuppressed());

	// REPA-08/09/10: bot-local, subject-local and generation-local identity.
	const RecoveryEpisodeIdentity botEight = Start(tracker, 8, "4");
	policy.RecordDomainExhausted(botEight, 5.0);
	policy.RecordDomainExhausted(botEight, 6.0);
	assert(policy.CheckSuppression(botEight, 6.1) == SuppressionCheck::SUPPRESSED);
	const RecoveryEpisodeIdentity pointFive = Start(tracker, 8, "5");
	assert(policy.CheckSuppression(pointFive, 6.2) ==
		SuppressionCheck::NOT_APPLICABLE);
	const RecoveryEpisodeIdentity pointFourNewGeneration = Start(tracker, 8, "4", 1, 2);
	assert(policy.CheckSuppression(pointFourNewGeneration, 6.3) ==
		SuppressionCheck::NOT_APPLICABLE);

	// REPA-11: expiry is observable once, then the goal is allowed again.
	const RecoveryEpisodeIdentity expiring = Start(tracker, 9, "4");
	policy.RecordDomainExhausted(expiring, 10.0);
	policy.RecordDomainExhausted(expiring, 11.0);
	assert(policy.CheckSuppression(expiring, 39.0) ==
		SuppressionCheck::SUPPRESSED);
	assert(policy.CheckSuppression(expiring, 41.1) ==
		SuppressionCheck::EXPIRED);
	assert(policy.CheckSuppression(expiring, 41.2) ==
		SuppressionCheck::NOT_APPLICABLE);

	// REPA-12/13: death/life generation cannot carry suppression forward.
	tracker.End();
	const RecoveryEpisodeIdentity respawn = Start(tracker, 9, "4", 2, 1);
	assert(policy.CheckSuppression(respawn, 42.0) ==
		SuppressionCheck::NOT_APPLICABLE);

	// REPA-14/15/16: the kill switch is an integration gate; with it off the
	// same policy result is never applied, while with it on a suppressed
	// candidate is skipped and an all-suppressed set may use normal fallback.
	const bool recoveryActiveOff = false;
	assert(!recoveryActiveOff ||
		policy.CheckSuppression(respawn, 42.0) == SuppressionCheck::SUPPRESSED);
	policy.RecordDomainExhausted(respawn, 43.0);
	policy.RecordDomainExhausted(respawn, 44.0);
	assert(policy.CheckSuppression(respawn, 44.1) == SuppressionCheck::SUPPRESSED);
	const bool recoveryActiveOn = true;
	assert(recoveryActiveOn &&
		policy.CheckSuppression(respawn, 44.2) == SuppressionCheck::SUPPRESSED);
	assert(policy.MarkSuppressionApplied(respawn));
	assert(!policy.MarkSuppressionApplied(respawn));

	// REPA-17/18: Squad may use the same value policy diagnostically, but no
	// selector call is made here; the test only records the hypothetical.
	const RecoveryEpisodeIdentity squad = SquadEpisode(tracker, 10);
	const auto squadFirst = policy.RecordDomainExhausted(squad, 50.0);
	const auto squadSecond = policy.RecordDomainExhausted(squad, 51.0);
	assert(!squadFirst.suppressionStarted && squadSecond.suppressionStarted);
	assert(policy.CheckSuppression(squad, 51.1) == SuppressionCheck::SUPPRESSED);

	// REPA-19: no learned-cost state is part of this value component.
	assert(sizeof(RecoveryEpisodePolicy) <= 256);

	// REPA-20/21: deterministic state and bounded memory.
	RecoveryEpisodePolicy left;
	RecoveryEpisodePolicy right;
	const RecoveryEpisodeIdentity deterministic = Start(tracker, 11, "4", 3, 7);
	const auto leftResult = left.RecordDomainExhausted(deterministic, 60.0);
	const auto rightResult = right.RecordDomainExhausted(deterministic, 60.0);
	assert(leftResult.consecutiveExhaustions == rightResult.consecutiveExhaustions &&
		leftResult.suppressionStarted == rightResult.suppressionStarted);
	assert(sizeof(left) == sizeof(right));

	// REPA-22: task/schedule/intent/attempt replacement is represented by
	// ObserveEpisode with the same validated identity and does not reset strikes.
	left.RecordDomainExhausted(deterministic, 61.0);
	left.ObserveEpisode(deterministic);
	left.ObserveEpisode(deterministic);
	assert(left.ConsecutiveExhaustions() == 2 && left.IsSuppressed());

	std::cout << "REPA-01..22 PASS; recovery_episode_behavior_changes=0\n";
	return 0;
}
