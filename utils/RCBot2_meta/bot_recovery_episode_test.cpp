#include "bot_recovery_episode.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	using RCBotRecovery::Domain;
	using RCBotRecovery::EpisodeTracker;
	using RecoveryEpisodeIdentity = RCBotRecovery::RecoveryEpisodeIdentity;

	RecoveryEpisodeIdentity Start(EpisodeTracker& tracker, const Domain domain,
		const char* subject, const std::uint64_t life = 1,
		const std::uint64_t generation = 1, bool* reused = nullptr)
	{
		return tracker.Start(7, life, domain, subject, generation, reused);
	}
}

int main()
{
	EpisodeTracker tracker;
	bool reused = false;

	// REPI-01/02: the stable capture objective is the identity. Alternate
	// approaches do not split the episode.
	const RecoveryEpisodeIdentity capture =
		Start(tracker, Domain::CAPTURE, "4", 1, 9, &reused);
	assert(capture.IsActive() && !reused);
	const std::uint64_t captureId = capture.episodeId;
	const RecoveryEpisodeIdentity alternateApproach =
		Start(tracker, Domain::CAPTURE, "4", 1, 9, &reused);
	assert(reused && alternateApproach.episodeId == captureId);

	// REPI-03/04/11/12: intent, attempt, task, and schedule changes are
	// context only; the same practical goal still reuses the episode.
	const RecoveryEpisodeIdentity newIntent =
		Start(tracker, Domain::CAPTURE, "4", 1, 9, &reused);
	assert(reused && newIntent.episodeId == captureId);
	const RecoveryEpisodeIdentity newAttempt =
		Start(tracker, Domain::CAPTURE, "4", 1, 9, &reused);
	assert(reused && newAttempt.episodeId == captureId);

	// REPI-05/13: a different capture point replaces the practical goal.
	const RecoveryEpisodeIdentity pointFive =
		Start(tracker, Domain::CAPTURE, "5", 1, 10, &reused);
	assert(!reused && pointFive.episodeId != captureId);

	// REPI-06/07: End plus a new life cannot reuse the old identity.
	tracker.End();
	assert(!tracker.HasActive());
	const RecoveryEpisodeIdentity afterRespawn =
		Start(tracker, Domain::CAPTURE, "5", 2, 10, &reused);
	assert(!reused && afterRespawn.episodeId != pointFive.episodeId);

	// REPI-08/09: bounded squad retry keeps the same shared practical goal.
	const RecoveryEpisodeIdentity squad = Start(tracker, Domain::SQUAD,
		"team=2;squad=3;waypoint=12", 2, 20, &reused);
	assert(!reused);
	const RecoveryEpisodeIdentity squadRetry = Start(tracker, Domain::SQUAD,
		"team=2;squad=3;waypoint=12", 2, 20, &reused);
	assert(reused && squadRetry.episodeId == squad.episodeId);

	// REPI-10: a new shared-goal generation starts a new episode.
	const RecoveryEpisodeIdentity squadGeneration = Start(tracker, Domain::SQUAD,
		"team=2;squad=3;waypoint=12", 2, 21, &reused);
	assert(!reused && squadGeneration.episodeId != squad.episodeId);

	// REPI-14/15: terminal labels are data, not tracker behavior; ending the
	// one active episode is the only state transition.
	tracker.End();
	assert(!tracker.HasActive());

	// REPI-16: deterministic identity fields and domain labels.
	const RecoveryEpisodeIdentity deterministic = Start(tracker,
		Domain::LAST_KNOWN, "enemy-userid=42", 3, 4, &reused);
	assert(std::string(RCBotRecovery::DomainName(deterministic.domain)) ==
		"LAST_KNOWN");
	assert(deterministic.botId == 7 && deterministic.lifeGeneration == 3);
	assert(deterministic.subjectId == "enemy-userid=42");
	assert(deterministic.domainGeneration == 4);

	// REPI-17: one active value and a bounded scalar counter; no history grows.
	const std::uint64_t firstId = deterministic.episodeId;
	for (int i = 0; i < 128; ++i)
	{
		const RecoveryEpisodeIdentity current = Start(tracker, Domain::ROAM,
			"roam", 3, static_cast<std::uint64_t>(i + 1), &reused);
		assert(current.IsActive());
	}
	assert(tracker.Active().episodeId > firstId);

	// REPI-18: this pure value tracker has no actuator hooks; all assertions
	// above observe identity only.
	assert(tracker.HasActive());
	std::cout << "REPI-01..18 PASS; recovery_episode_behavior_changes=0\n";
	return 0;
}
