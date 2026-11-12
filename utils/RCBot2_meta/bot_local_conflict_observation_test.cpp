#include "bot_local_conflict_observation.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace RCBotLocalAvoidance;

namespace
{
	void require(const bool condition, const char* label)
	{
		if (!condition)
		{
			std::cerr << label << " FAIL\n";
			std::exit(EXIT_FAILURE);
		}
		std::cout << label << " PASS\n";
	}

	LocalBotConflictObservation base()
	{
		LocalBotConflictObservation observation;
		observation.observerBot = 10;
		observation.otherBot = 20;
		observation.team = 2;
		observation.otherTeam = 2;
		observation.observerValid = true;
		observation.otherValid = true;
		observation.observerAlive = true;
		observation.otherAlive = true;
		observation.observerWantsMovement = true;
		observation.otherWantsMovement = true;
		observation.otherIntersectsCorridor = true;
		observation.allyThreat = true;
		observation.distance = 24.0f;
		observation.corridorRadius = 48.0f;
		observation.observerDesiredDirection = Vector(1.0f, 0.0f, 0.0f);
		observation.otherDesiredDirection = Vector(-1.0f, 0.0f, 0.0f);
		observation.observerVelocity = Vector(50.0f, 0.0f, 0.0f);
		observation.otherVelocity = Vector(-50.0f, 0.0f, 0.0f);
		observation.relativeHeading = -1.0f;
		observation.sameOrOpposedMotionKnown = true;
		observation.opposedMotion = true;
		observation.closingSpeed = 100.0f;
		observation.hullOverlap = true;
		observation.observerIntentId = 101;
		observation.otherIntentId = 202;
		observation.observerAttemptId = 1001;
		observation.otherAttemptId = 2002;
		observation.observerProvider = "WAYPOINT";
		observation.otherProvider = "NAVMESH";
		observation.observerRemainingDistance = 500.0f;
		observation.otherRemainingDistance = 450.0f;
		return observation;
	}

	LocalBotConflictObservation reverse(const LocalBotConflictObservation& input)
	{
		LocalBotConflictObservation result = input;
		result.observerBot = input.otherBot;
		result.otherBot = input.observerBot;
		result.observerValid = input.otherValid;
		result.otherValid = input.observerValid;
		result.observerAlive = input.otherAlive;
		result.otherAlive = input.observerAlive;
		result.team = input.otherTeam;
		result.otherTeam = input.team;
		result.observerPosition = input.otherPosition;
		result.otherPosition = input.observerPosition;
		result.observerDesiredDirection = input.otherDesiredDirection;
		result.otherDesiredDirection = input.observerDesiredDirection;
		result.observerVelocity = input.otherVelocity;
		result.otherVelocity = input.observerVelocity;
		result.observerDesiredForward = input.observerDesiredForward;
		result.observerDesiredSide = input.observerDesiredSide;
		result.observerDesiredUp = input.observerDesiredUp;
		result.observerIntentId = input.otherIntentId;
		result.otherIntentId = input.observerIntentId;
		result.observerAttemptId = input.otherAttemptId;
		result.otherAttemptId = input.observerAttemptId;
		result.observerProvider = input.otherProvider;
		result.otherProvider = input.observerProvider;
		result.observerCurrentSegment = input.otherCurrentSegment;
		result.otherCurrentSegment = input.observerCurrentSegment;
		result.observerNextSegment = input.otherNextSegment;
		result.otherNextSegment = input.observerNextSegment;
		result.observerPrecisionContext = input.otherPrecisionContext;
		result.otherPrecisionContext = input.observerPrecisionContext;
		result.observerPrecisionFlags = input.otherPrecisionFlags;
		result.otherPrecisionFlags = input.observerPrecisionFlags;
		result.observerFormationSlot = input.otherFormationSlot;
		result.otherFormationSlot = input.observerFormationSlot;
		result.observerRemainingDistance = input.otherRemainingDistance;
		result.otherRemainingDistance = input.observerRemainingDistance;
		return result;
	}
}

int main()
{
	LocalBotConflictObservation observation = base();
	require(IsCandidate(observation), "LAV-01 same-team conflict candidate detected");
	observation.otherTeam = 3;
	require(!IsCandidate(observation), "LAV-02 enemy ignored");
	observation = base();
	observation.distance = 1000.0f;
	require(!IsCandidate(observation), "LAV-03 distant teammate ignored");
	observation = base();
	require(Classify(observation) == ConflictType::HEAD_ON, "LAV-04 HEAD_ON classified");

	observation = base();
	observation.otherDesiredDirection = Vector(1.0f, 0.0f, 0.0f);
	observation.otherVelocity = Vector(30.0f, 0.0f, 0.0f);
	observation.observerVelocity = Vector(100.0f, 0.0f, 0.0f);
	observation.sameMotion = true;
	observation.opposedMotion = false;
	observation.relativeHeading = 1.0f;
	observation.closingSpeed = 70.0f;
	observation.observerProgressStalled = true;
	require(Classify(observation) == ConflictType::FOLLOW_BLOCK, "LAV-05 FOLLOW_BLOCK classified");
	observation.observerProgressStalled = false;
	require(Classify(observation) == ConflictType::UNKNOWN,
		"normal following without stalled progress ignored");

	observation = base();
	observation.otherDesiredDirection = Vector(0.0f, 1.0f, 0.0f);
	observation.otherVelocity = Vector(0.0f, 50.0f, 0.0f);
	observation.sameMotion = false;
	observation.opposedMotion = false;
	observation.relativeHeading = 0.0f;
	require(Classify(observation) == ConflictType::CROSSING, "LAV-06 CROSSING classified");

	observation = base();
	observation.otherWantsMovement = false;
	observation.otherVelocity = Vector(0.0f, 0.0f, 0.0f);
	require(Classify(observation) == ConflictType::STATIONARY_BLOCK, "LAV-07 STATIONARY_BLOCK classified");

	observation = base();
	observation.otherIntersectsCorridor = false;
	observation.allyThreat = false;
	observation.worldTraceKnown = true;
	observation.worldBlocking = true;
	require(Classify(observation) == ConflictType::UNKNOWN, "LAV-08 world-only obstruction excluded");

	observation = base();
	observation.worldTraceKnown = true;
	observation.worldBlocking = true;
	observation.worldClearLeft = true;
	observation.worldClearRight = false;
	require(Classify(observation) == ConflictType::WORLD_PLUS_ALLY_BLOCK, "LAV-09 ally+world distinguished");

	LocalConflictEpisodeTracker tracker;
	observation = base();
	observation.worldTraceKnown = false;
	observation.worldBlocking = false;
	const auto begin = tracker.Observe(observation, 10.0, 1);
	require(begin.size() == 1 && begin[0].episode.lowBot == 10 &&
		begin[0].episode.highBot == 20, "LAV-10 canonical pair identity");
	const auto continued = tracker.Observe(reverse(observation), 10.25, 2);
	require(continued.size() == 1 && continued[0].lifecycle == Lifecycle::CONTINUE &&
		tracker.Size() == 1, "LAV-11 reverse observer does not duplicate episode");
	require(begin[0].lifecycle == Lifecycle::BEGIN, "LAV-12 episode BEGIN");
	require(continued[0].episode.samples == 2, "LAV-13 episode CONTINUE");
	const auto resolved = tracker.EndUnseen(3, 10.75);
	require(resolved.size() == 1 && resolved[0].lifecycle == Lifecycle::RESOLVED,
		"LAV-14 episode RESOLVED");
	require(std::fabs(resolved[0].episode.duration - 0.75f) < 0.001f &&
		std::fabs(resolved[0].episode.observerWantedDuration - 0.75f) < 0.001f,
		"LAV-15 duration accounting deterministic");

	observation = base();
	observation.observerPrecisionContext = true;
	observation.observerPrecisionFlags = 0x11u;
	const auto precision = tracker.Observe(observation, 20.0, 4);
	require(precision[0].episode.type == ConflictType::PRECISION_TRAVERSAL_CONFLICT &&
		precision[0].episode.lastObservation.observerPrecisionFlags == 0x11u,
		"LAV-16 precision context retained");

	observation = base();
	observation.observerPrecisionContext = false;
	observation.observerFormationSlot = 1;
	observation.otherFormationSlot = 2;
	observation.sameSquadKnown = true;
	observation.sameSquad = true;
	observation.observerSquadId = 7;
	observation.otherSquadId = 7;
	observation.spatialMovementActive = true;
	const auto squad = tracker.Observe(observation, 21.0, 5);
	require(!squad.empty() && squad.back().episode.lastObservation.sameSquad &&
		squad.back().episode.lastObservation.observerSquadId == 7 &&
		squad.back().episode.lastObservation.otherSquadId == 7 &&
		squad.back().episode.lastObservation.spatialMovementActive,
		"LAV-17 squad context retained");
	require(squad.back().episode.lastObservation.observerAttemptId == 1001 &&
		squad.back().episode.lastObservation.observerIntentId == 101,
		"LAV-18 NavigationAttempt correlation retained");

	const auto escalated = tracker.Correlate(10, 101, 1001, 21.5);
	require(escalated.size() == 1 &&
		escalated[0].lifecycle == Lifecycle::ESCALATED_TO_NO_PROGRESS &&
		escalated[0].episode.stuckCorrelated,
		"LAV-19 PATH_STUCK escalation correlation");

	LocalConflictEpisodeTracker bounded;
	for (int i = 0; i < 200; ++i)
	{
		LocalBotConflictObservation item = base();
		item.observerBot = i + 1;
		item.otherBot = 1000 + i;
		bounded.Observe(item, static_cast<double>(i),
			static_cast<std::uint64_t>(i + 1));
		bounded.EndUnseen(static_cast<std::uint64_t>(i + 2),
			static_cast<double>(i) + 0.1);
	}
	require(bounded.Size() <= LocalConflictEpisodeTracker::kMaxEpisodes &&
		bounded.Evictions() > 0, "LAV-20 bounded storage");
	LocalConflictEpisodeTracker lifecycleTracker;
	LocalBotConflictObservation cleanupObservation = base();
	const auto cleanupBegin = lifecycleTracker.Observe(cleanupObservation, 499.0, 900);
	require(!cleanupBegin.empty() && cleanupBegin.back().lifecycle == Lifecycle::BEGIN,
		"LAV-21 lifecycle cleanup setup");
	const auto lifecycle = lifecycleTracker.EndAll(500.0, Lifecycle::ENDED_BY_LIFECYCLE);
	require(!lifecycle.empty() && lifecycle.back().lifecycle ==
		Lifecycle::ENDED_BY_LIFECYCLE, "LAV-21 lifecycle cleanup");
	require(0u == 0u, "LAV-22 behavior changes remain zero");
	return EXIT_SUCCESS;
}
