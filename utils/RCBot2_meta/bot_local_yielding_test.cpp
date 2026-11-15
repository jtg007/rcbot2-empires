#include "bot_local_yielding.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace RCBotLocalAvoidance;
using namespace RCBotLocalYielding;

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

	LocalBotConflictObservation baseObservation()
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
		observation.distance = 32.0f;
		observation.observerDesiredDirection = Vector(1.0f, 0.0f, 0.0f);
		observation.otherDesiredDirection = Vector(-1.0f, 0.0f, 0.0f);
		observation.observerVelocity = Vector(50.0f, 0.0f, 0.0f);
		observation.otherVelocity = Vector(-50.0f, 0.0f, 0.0f);
		observation.sameOrOpposedMotionKnown = true;
		observation.opposedMotion = true;
		observation.relativeHeading = -1.0f;
		observation.otherProvider = "NAVMESH";
		observation.observerProvider = "WAYPOINT";
		observation.observerIntentId = 101;
		observation.otherIntentId = 202;
		observation.observerAttemptId = 1001;
		observation.otherAttemptId = 2002;
		return observation;
	}

	LocalConflictEpisode episodeFor(const LocalBotConflictObservation& observation,
		const ConflictType type, const std::uint64_t id = 1)
	{
		LocalConflictEpisode episode;
		episode.active = true;
		episode.episodeId = id;
		episode.lowBot = (std::min)(observation.observerBot, observation.otherBot);
		episode.highBot = (std::max)(observation.observerBot, observation.otherBot);
		episode.team = observation.team;
		episode.type = type;
		episode.startTime = 0.0;
		episode.lastSampleTime = 0.0;
		episode.lastObservation = observation;
		return episode;
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
		result.observerWantsMovement = input.otherWantsMovement;
		result.otherWantsMovement = input.observerWantsMovement;
		result.observerPrecisionContext = input.otherPrecisionContext;
		result.otherPrecisionContext = input.observerPrecisionContext;
		result.observerPrecisionFlags = input.otherPrecisionFlags;
		result.otherPrecisionFlags = input.observerPrecisionFlags;
		result.observerProvider = input.otherProvider;
		result.otherProvider = input.observerProvider;
		result.observerIntentId = input.otherIntentId;
		result.otherIntentId = input.observerIntentId;
		result.observerAttemptId = input.otherAttemptId;
		result.otherAttemptId = input.observerAttemptId;
		return result;
	}
}

int main()
{
	LocalBotConflictObservation observation = baseObservation();
	LocalConflictEpisode episode = episodeFor(observation, ConflictType::HEAD_ON);
	Controller controller;

	// LYLD-01: the OFF path never submits an observation to the controller.
	controller.Reset();
	require(!controller.IsWaiting(10) && controller.StateCount() == 0,
		"LYLD-01 yielding OFF changes no movement");

	Evaluation begin = controller.Observe(observation, episode, 0.0);
	require(begin.winnerBot >= 0 && begin.waiterBot >= 0,
		"LYLD-02 HEAD_ON creates exactly one winner");
	require(begin.winnerBot != begin.waiterBot && begin.wait.role == Role::WAIT,
		"LYLD-03 HEAD_ON creates exactly one waiter");
	const int stableWinner = begin.winnerBot;
	Evaluation reverseSticky = controller.Observe(reverse(observation), episode, 0.2);
	require(reverseSticky.winnerBot == stableWinner &&
		reverseSticky.event == Event::NONE,
		"LYLD-04 reverse observer order gives same winner");
	require(controller.StateFor(stableWinner)->role == Role::PROCEED,
		"LYLD-05 decision remains sticky");

	Evaluation ended = controller.End(episode, 0.3);
	require(ended.event == Event::END && controller.StateCount() == 0 &&
		!controller.IsWaiting(ended.waiterBot),
		"LYLD-06 resolved conflict ends WAIT");

	LocalBotConflictObservation stationaryObservation = observation;
	stationaryObservation.otherWantsMovement = false;
	stationaryObservation.otherVelocity = Vector();
	LocalConflictEpisode stationary = episodeFor(stationaryObservation,
		ConflictType::STATIONARY_BLOCK, 2);
	Evaluation stationaryDecision = controller.Observe(stationaryObservation,
		stationary, 1.0);
	require(stationaryDecision.winnerBot == stationaryObservation.observerBot &&
		stationaryDecision.waiterBot == stationaryObservation.otherBot,
		"LYLD-07 stationary blocker classification produces deterministic role");
	controller.Reset();
	stationary.type = ConflictType::CHOKE_CONFLICT;
	Evaluation choke = controller.Observe(observation, stationary, 2.0);
	require(choke.winnerBot >= 0 && choke.waiterBot >= 0,
		"LYLD-08 CHOKE conflict supports WAIT");

	controller.Reset();
	for (const ConflictType type : {ConflictType::CROSSING,
		ConflictType::FOLLOW_BLOCK, ConflictType::UNKNOWN})
	{
		episode.type = type;
		require(controller.Observe(observation, episode, 3.0).event == Event::NONE,
			type == ConflictType::CROSSING ?
			"LYLD-09 CROSSING remains observation-only" : type == ConflictType::FOLLOW_BLOCK ?
			"LYLD-10 FOLLOW_BLOCK remains observation-only" :
			"LYLD-11 UNKNOWN remains observation-only");
	}

	controller.Reset();
	LocalBotConflictObservation precision = observation;
	precision.otherPrecisionContext = true;
	episode = episodeFor(precision, ConflictType::PRECISION_TRAVERSAL_CONFLICT, 3);
	Evaluation precisionDecision = controller.Observe(precision, episode, 4.0);
	require(precisionDecision.winnerBot == precision.otherBot &&
		precisionDecision.waiterBot == precision.observerBot,
		"LYLD-12 precision traverser receives priority");
	require(precisionDecision.observation.observerIntentId == observation.observerIntentId,
		"LYLD-13 waiter does not create new NavigationIntent");
	require(precisionDecision.observation.observerProvider == observation.observerProvider,
		"LYLD-14 waiter does not alter route");
	require(precisionDecision.observation.otherProvider == observation.otherProvider,
		"LYLD-15 waiter does not alter provider");
	require(EventName(Event::END) != std::string("PATH_STUCK"),
		"LYLD-16 intentional WAIT does not emit PATH_STUCK");
	require(precisionDecision.episode.samples == episode.samples,
		"LYLD-17 WAIT does not count as navigation progress");

	controller.Reset();
	episode = episodeFor(observation, ConflictType::HEAD_ON, 4);
	Evaluation bounded = controller.Observe(observation, episode, 0.0);
	Evaluation reevaluate = controller.Observe(observation, episode, 0.8);
	require(reevaluate.event == Event::REEVALUATE &&
		reevaluate.winnerBot == bounded.winnerBot,
		"LYLD-18 watchdog resumes after WAIT");
	require(reevaluate.event == Event::REEVALUATE,
		"LYLD-19 bounded wait triggers re-evaluation");
	Evaluation rotated = controller.Observe(observation, episode, 4.1);
	require(rotated.event == Event::STARVATION_ROTATE &&
		rotated.winnerBot == bounded.waiterBot,
		"LYLD-20 starvation protection allows previous waiter priority opportunity");

	controller.ClearBot(rotated.waiterBot);
	require(controller.StateCount() == 0,
		"LYLD-21 death clears yield state");
	controller.Observe(observation, episode, 5.0);
	controller.Reset();
	require(controller.StateCount() == 0,
		"LYLD-22 disconnect/lifecycle clears state");

	Evaluation unchanged = controller.Observe(observation, episode, 6.0);
	require(unchanged.observation.observerIntentId == observation.observerIntentId &&
		unchanged.observation.observerProvider == observation.observerProvider,
		"LYLD-23 squad slot/order unchanged");
	for (int i = 0; i < 140; ++i)
	{
		LocalBotConflictObservation pairObservation = observation;
		pairObservation.observerBot = 1000 + i * 2;
		pairObservation.otherBot = 1001 + i * 2;
		LocalConflictEpisode pairEpisode = episodeFor(pairObservation,
			ConflictType::HEAD_ON, static_cast<std::uint64_t>(10 + i));
		controller.Observe(pairObservation, pairEpisode, 7.0);
	}
	require(controller.StateCount() <= Controller::kMaxPairStates,
		"LYLD-24 bot pair state bounded");
	Evaluation noChange = controller.Observe(observation, episode, 7.1);
	require(!noChange.changed || noChange.event == Event::BEGIN,
		"LYLD-25 behavior counter increments only on actual WAIT");
	controller.Reset();
	Evaluation forward = controller.Observe(observation, episode, 8.0);
	controller.Reset();
	Evaluation backward = controller.Observe(reverse(observation), episode, 8.0);
	require(forward.winnerBot == backward.winnerBot &&
		forward.waiterBot == backward.waiterBot,
		"LYLD-26 deterministic output");
}
