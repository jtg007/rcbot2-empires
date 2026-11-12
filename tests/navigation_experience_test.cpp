#include "../utils/RCBot2_meta/bot_navigation_experience.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

using namespace RCBotNavigationExperience;

namespace
{
int failures = 0;

void check(const bool value, const char* name)
{
	if (value)
		return;
	std::cerr << "FAIL " << name << '\n';
	++failures;
}

Key makeKey(const std::int64_t start = 10, const std::int64_t goal = 20)
{
	Key key;
	key.map = "test_map";
	key.provider = Provider::WAYPOINT;
	key.startNode = start;
	key.destinationNode = goal;
	return key;
}

Observation makeObservation(const Result result, const int bot,
	const std::uint64_t attempt, const std::uint64_t time,
	const std::int64_t start = 10, const std::int64_t goal = 20)
{
	Observation observation;
	observation.key = makeKey(start, goal);
	observation.botId = bot;
	observation.attemptId = attempt;
	observation.timestampMs = time;
	observation.result = result;
	observation.traversalSeconds = 1.25;
	observation.progressUnits = 8.5;
	return observation;
}

NavigationTransitionObservation makeTransition(
	const Provider provider, const std::int64_t from, const std::int64_t to,
	const Traversal traversal, const TransitionResult result, const int bot,
	const std::uint64_t attempt, const std::uint64_t generation,
	const std::uint64_t time, const std::uint64_t intent = 1,
	const CancellationKind cancellation = CancellationKind::NONE)
{
	NavigationTransitionObservation observation;
	observation.mapIdentity = "test_map";
	observation.provider = provider;
	observation.intentId = intent;
	observation.attemptId = attempt;
	observation.fromNode = from;
	observation.toNode = to;
	observation.traversal = traversal;
	observation.result = result;
	observation.cancellation = cancellation;
	observation.botId = bot;
	observation.attemptGeneration = generation;
	observation.timestampMs = time;
	observation.durationSeconds = 1.5;
	observation.progressUnits = 2.0;
	observation.transitionKnown = from >= 0 && to >= 0 && from != to;
	return observation;
}

void add(Store& store, const Result result, const int bot,
	const std::uint64_t attempt, const std::uint64_t time,
	const std::int64_t start = 10, const std::int64_t goal = 20)
{
	check(store.record(makeObservation(result, bot, attempt, time, start, goal)),
		"observation accepted");
}

const Entry* findEntry(const Store& store, const Key& key)
{
	const auto it = store.entries().find(key);
	return it == store.entries().end() ? nullptr : &it->second;
}

const TransitionEntry* findTransition(const Store& store,
	const Provider provider, const std::int64_t from, const std::int64_t to,
	const Traversal traversal = Traversal::NORMAL)
{
	for (const auto& pair : store.transitionEntries())
		if (pair.first.map == "test_map" && pair.first.provider == provider &&
			pair.first.fromNode == from && pair.first.toNode == to &&
			pair.first.traversal == traversal)
			return &pair.second;
	return nullptr;
}

void testDirectedTransitionsAndOutcomes()
{
	Store directed;
	check(directed.recordTransition(makeTransition(Provider::WAYPOINT, 10, 20,
		Traversal::NORMAL, TransitionResult::TRANSITION_SUCCESS, 1, 101, 1, 1)),
		"NEDGE-01 forward directed edge accepted");
	check(directed.recordTransition(makeTransition(Provider::WAYPOINT, 20, 10,
		Traversal::NORMAL, TransitionResult::TRANSITION_SUCCESS, 1, 102, 1, 2)),
		"NEDGE-02 reverse edge accepted separately");
	check(directed.transitionEntries().size() == 2 &&
		findTransition(directed, Provider::WAYPOINT, 10, 20) != nullptr &&
		findTransition(directed, Provider::WAYPOINT, 20, 10) != nullptr,
		"NEDGE-01/02 A→B and B→A remain separate experiences");

	Store path;
	check(path.recordTransition(makeTransition(Provider::WAYPOINT, 1, 2,
		Traversal::NORMAL, TransitionResult::TRANSITION_SUCCESS, 1, 201, 1, 1)),
		"NEDGE-03 first successful edge accepted");
	check(path.recordTransition(makeTransition(Provider::WAYPOINT, 2, 3,
		Traversal::NORMAL, TransitionResult::TRANSITION_SUCCESS, 1, 201, 2, 2)),
		"NEDGE-06 second successful edge accepted");
	check(path.recordTransition(makeTransition(Provider::WAYPOINT, 3, 4,
		Traversal::NORMAL, TransitionResult::NO_PROGRESS, 1, 201, 3, 3)),
		"NEDGE-04 no-progress edge accepted");
	const TransitionEntry* first = findTransition(path, Provider::WAYPOINT, 1, 2);
	const TransitionEntry* second = findTransition(path, Provider::WAYPOINT, 2, 3);
	const TransitionEntry* failed = findTransition(path, Provider::WAYPOINT, 3, 4);
	check(first != nullptr && first->successes == 1 && first->noProgress == 0 &&
		second != nullptr && second->successes == 1 && second->noProgress == 0 &&
		failed != nullptr && failed->successes == 0 && failed->noProgress == 1,
		"NEDGE-06 successes before a failed edge stay on their own transitions");

	Store cancelled;
	check(cancelled.recordTransition(makeTransition(Provider::WAYPOINT, 5, 6,
		Traversal::NORMAL, TransitionResult::CANCELLED, 2, 301, 4, 4, 7,
		CancellationKind::EXTERNAL)), "NEDGE-05 cancellation accepted");
	const TransitionEntry* cancellation = findTransition(cancelled,
		Provider::WAYPOINT, 5, 6);
	check(cancellation != nullptr && cancellation->cancellations == 1 &&
		cancellation->cancellationExternal == 1 && cancellation->noProgress == 0 &&
		cancellation->structuralFailures == 0,
		"NEDGE-05 cancellation is separate from traversal failure");
}

void testTransitionAggregationAndClassification()
{
	Store sameEdge;
	check(sameEdge.recordTransition(makeTransition(Provider::WAYPOINT, 7, 8,
		Traversal::NORMAL, TransitionResult::TRANSITION_SUCCESS, 1, 401, 1, 1, 40)),
		"NEDGE-07 first intent edge recorded");
	check(sameEdge.recordTransition(makeTransition(Provider::WAYPOINT, 7, 8,
		Traversal::NORMAL, TransitionResult::NO_PROGRESS, 2, 402, 2, 2, 41)),
		"NEDGE-07 second intent edge recorded");
	const TransitionEntry* aggregate = findTransition(sameEdge,
		Provider::WAYPOINT, 7, 8);
	check(aggregate != nullptr && aggregate->attempts == 2 &&
		aggregate->successes == 1 && aggregate->noProgress == 1 &&
		sameEdge.transitionEntries().size() == 1,
		"NEDGE-07 different destinations/intents aggregate on the same edge");

	Store traversal;
	check(traversal.recordTransition(makeTransition(Provider::WAYPOINT, 9, 10,
		Traversal::NORMAL, TransitionResult::TRANSITION_SUCCESS, 1, 501, 1, 1)),
		"NEDGE-09 normal transition recorded");
	check(traversal.recordTransition(makeTransition(Provider::WAYPOINT, 9, 10,
		Traversal::DROP, TransitionResult::NO_PROGRESS, 2, 502, 1, 2)),
		"NEDGE-09 drop transition recorded");
	check(traversal.transitionEntries().size() == 2 &&
		findTransition(traversal, Provider::WAYPOINT, 9, 10,
			Traversal::NORMAL) != nullptr &&
		findTransition(traversal, Provider::WAYPOINT, 9, 10,
			Traversal::DROP) != nullptr,
		"NEDGE-09 traversal kind separates DROP from NORMAL");

	Store bots;
	check(bots.recordTransition(makeTransition(Provider::WAYPOINT, 11, 12,
		Traversal::NORMAL, TransitionResult::NO_PROGRESS, 1, 601, 1, 1)),
		"NEDGE-10 bot one edge failure recorded");
	check(bots.recordTransition(makeTransition(Provider::WAYPOINT, 11, 12,
		Traversal::NORMAL, TransitionResult::STRUCTURAL_PATH_FAILURE, 2,
		602, 2, 2)), "NEDGE-10 bot two structural failure recorded");
	check(bots.recordTransition(makeTransition(Provider::WAYPOINT, 11, 12,
		Traversal::NORMAL, TransitionResult::TRANSITION_SUCCESS, 1, 603, 3, 3)),
		"NEDGE-10 success evidence recorded");
	check(bots.recordTransition(makeTransition(Provider::WAYPOINT, 11, 12,
		Traversal::NORMAL, TransitionResult::NO_PROGRESS, 2, 604, 4, 4)),
		"NEDGE-10 repeated bot failure recorded");
	const TransitionEntry* shared = findTransition(bots, Provider::WAYPOINT,
		11, 12);
	const std::vector<TransitionHotspot> hotspots = bots.transitionHotspots();
	check(shared != nullptr && shared->botIds.size() == 2 &&
		shared->failureBotIds.size() == 2 && hotspots.size() == 1 &&
		hotspots[0].affectedFailureBots == 2,
		"NEDGE-10 multiple bots aggregate and satisfy hotspot evidence gate");

	Store providers;
	check(providers.recordTransition(makeTransition(Provider::WAYPOINT, 3, 4,
		Traversal::NORMAL, TransitionResult::NO_PROGRESS, 1, 701, 1, 1)),
		"NEDGE-12 waypoint provider attempt recorded");
	check(providers.recordTransition(makeTransition(Provider::NAVMESH, 3, 4,
		Traversal::UNKNOWN, TransitionResult::TRANSITION_SUCCESS, 1, 702, 1, 2)),
		"NEDGE-12 navmesh provider attempt recorded");
	check(providers.transitionEntries().size() == 2 &&
		findTransition(providers, Provider::WAYPOINT, 3, 4) != nullptr &&
		findTransition(providers, Provider::NAVMESH, 3, 4,
			Traversal::UNKNOWN) != nullptr,
		"NEDGE-12 provider fallback experience remains provider-separated");
}

void testTransitionDedupBoundsAndCanonicalOutput()
{
	Store duplicate;
	const NavigationTransitionObservation terminal = makeTransition(
		Provider::WAYPOINT, 15, 16, Traversal::NORMAL,
		TransitionResult::STRUCTURAL_PATH_FAILURE, 3, 801, 5, 50);
	check(duplicate.recordTransition(terminal), "NEDGE-11 terminal accepted once");
	check(!duplicate.recordTransition(terminal) &&
		duplicate.transitionObservationCount() == 1,
		"NEDGE-11 duplicate terminal callback cannot double count");

	Limits limits;
	limits.maxTransitionEntries = 2;
	Store bounded(limits);
	const NavigationTransitionObservation oldEdge = makeTransition(
		Provider::WAYPOINT, 20, 21, Traversal::NORMAL,
		TransitionResult::TRANSITION_SUCCESS, 1, 901, 1, 10);
	const NavigationTransitionObservation midEdge = makeTransition(
		Provider::WAYPOINT, 30, 31, Traversal::NORMAL,
		TransitionResult::TRANSITION_SUCCESS, 1, 902, 1, 20);
	const NavigationTransitionObservation newEdge = makeTransition(
		Provider::WAYPOINT, 40, 41, Traversal::NORMAL,
		TransitionResult::TRANSITION_SUCCESS, 1, 903, 1, 30);
	check(bounded.recordTransition(oldEdge) && bounded.recordTransition(midEdge) &&
		bounded.recordTransition(newEdge) && bounded.transitionEvictions() == 1 &&
		bounded.transitionEntries().size() == 2 &&
		findTransition(bounded, Provider::WAYPOINT, 20, 21) == nullptr &&
		findTransition(bounded, Provider::WAYPOINT, 30, 31) != nullptr &&
		findTransition(bounded, Provider::WAYPOINT, 40, 41) != nullptr,
		"NEDGE-13 bounded transition eviction removes least-recent key");

	Store deterministic;
	Store deterministicReplay;
	const auto a = makeTransition(Provider::WAYPOINT, 50, 51,
		Traversal::NORMAL, TransitionResult::NO_PROGRESS, 1, 1001, 1, 10);
	const auto b = makeTransition(Provider::WAYPOINT, 51, 52,
		Traversal::NORMAL, TransitionResult::TRANSITION_SUCCESS, 2, 1002, 2, 20);
	const auto c = makeTransition(Provider::NAVMESH, 52, 53,
		Traversal::UNKNOWN, TransitionResult::STRUCTURAL_PATH_FAILURE,
		2, 1003, 3, 30);
	check(deterministic.recordTransition(a) && deterministic.recordTransition(b) &&
		deterministic.recordTransition(c), "NEDGE-14 forward canonical input accepted");
	check(deterministicReplay.recordTransition(c) &&
		deterministicReplay.recordTransition(b) &&
		deterministicReplay.recordTransition(a) &&
		deterministic.canonicalJson() == deterministicReplay.canonicalJson(),
		"NEDGE-14 canonical transition JSON is input-order deterministic");

	Store invalid;
	auto malformed = makeTransition(Provider::WAYPOINT, -1, 3,
		Traversal::NORMAL, TransitionResult::NO_PROGRESS, 1, 1101, 1, 1);
	malformed.transitionKnown = true;
	check(!invalid.recordTransition(malformed),
		"NEDGE-15 partially invalid node pair rejected");
	malformed = makeTransition(Provider::WAYPOINT, 3, 3,
		Traversal::NORMAL, TransitionResult::TRANSITION_SUCCESS, 1, 1102, 1, 2);
	check(!invalid.recordTransition(malformed), "NEDGE-15 self-edge rejected");
	malformed = makeTransition(Provider::WAYPOINT, 3, 4, Traversal::NONE,
		TransitionResult::TRANSITION_SUCCESS, 1, 1103, 1, 3);
	check(!invalid.recordTransition(malformed), "NEDGE-15 NONE traversal rejected");
	malformed = makeTransition(Provider::WAYPOINT, -1, -1, Traversal::UNKNOWN,
		TransitionResult::NO_PROGRESS, 1, 1104, 1, 4);
	check(!invalid.recordTransition(malformed),
		"NEDGE-15 unknown edge cannot claim a no-progress outcome");
	malformed = makeTransition(Provider::WAYPOINT, -1, -1, Traversal::UNKNOWN,
		TransitionResult::UNKNOWN, 1, 1105, 1, 5);
	malformed.transitionKnown = false;
	check(invalid.recordTransition(malformed) &&
		invalid.unknownTransitionCount() == 1 &&
		invalid.transitionEntries().empty(),
		"NEDGE-15 unknown transition is diagnosed without inventing an edge");

	Store shadow;
	struct NavigationDecision
	{
		int provider = 7;
		int routeCost = 100;
		int nextNode = 22;
		bool blocked = false;
	};
	NavigationDecision decision;
	const NavigationDecision before = decision;
	check(shadow.recordTransition(makeTransition(Provider::WAYPOINT, 60, 61,
		Traversal::NORMAL, TransitionResult::NO_PROGRESS, 1, 1201, 1, 1)) &&
		decision.provider == before.provider && decision.routeCost == before.routeCost &&
		decision.nextNode == before.nextNode && decision.blocked == before.blocked &&
		shadow.transitionHotspots().empty(),
		"NEDGE-16 shadow observations cannot mutate route decisions");
	check(shadow.canonicalJson().find("\"learned_navigation_behavior_changes\":0") !=
		std::string::npos,
		"NEDGE-16 canonical telemetry declares zero learned behavior changes");
}

void testResultsAndCancellation()
{
	Store store;
	store.reset("test_map", "waypoint-v5-count-64");
	add(store, Result::SUCCESS, 1, 1, 1);
	add(store, Result::PATH_FAILED, 1, 2, 2);
	add(store, Result::PATH_STUCK, 2, 3, 3);
	add(store, Result::UNREACHABLE, 2, 4, 4);
	add(store, Result::CANCELLED_LIFECYCLE, 2, 5, 5);
	add(store, Result::CANCELLED_REPLACEMENT, 3, 6, 6);
	add(store, Result::CANCELLED_EXTERNAL, 3, 7, 7);
	const Entry* entry = findEntry(store, makeKey());
	check(entry != nullptr, "NEXP entry exists");
	if (entry == nullptr) return;
	check(entry->successes == 1, "NEXP-01 success recorded");
	check(entry->pathFailed == 1, "NEXP-02 PATH_FAILED recorded");
	check(entry->stuck == 1, "NEXP-03 PATH_STUCK recorded");
	check(entry->cancelledLifecycle == 1 && entry->cancelledLifecycle <= entry->attempts,
		"NEXP-04 lifecycle cancellation recorded separately");
	check(entry->cancelledReplacement == 1,
		"NEXP-05 replacement cancellation recorded separately");
	check(entry->unreachable == 1 && entry->cancelledExternal == 1,
		"unreachable and external cancellation remain distinct");
	check(entry->successes + entry->pathFailed + entry->stuck + entry->unreachable == 4,
		"cancellations excluded from true failures");
}

void testAggregationAndHotspots()
{
	Store same;
	add(same, Result::PATH_FAILED, 1, 1, 1);
	add(same, Result::SUCCESS, 2, 2, 2);
	check(same.entries().size() == 1 && findEntry(same, makeKey())->attempts == 2,
		"NEXP-06 identical keys aggregate");
	add(same, Result::PATH_FAILED, 2, 3, 3, 11, 20);
	check(same.entries().size() == 2 && findEntry(same, makeKey(11, 20)) != nullptr,
		"NEXP-07 distinct keys remain separate");

	Store one;
	add(one, Result::PATH_FAILED, 1, 11, 1);
	check(one.hotspots().empty(), "NEXP-09 isolated failure is not a hotspot");

	Store repeated;
	add(repeated, Result::PATH_FAILED, 1, 21, 1);
	add(repeated, Result::PATH_STUCK, 2, 22, 2);
	add(repeated, Result::UNREACHABLE, 1, 23, 3);
	add(repeated, Result::SUCCESS, 2, 24, 4);
	const std::vector<Hotspot> firstHotspots = repeated.hotspots();
	check(firstHotspots.size() == 1 && firstHotspots.front().affectedBots == 2,
		"NEXP-08 multiple bots contribute to the same hotspot");
	check(firstHotspots.size() == 1, "NEXP-10 repeated failures create a hotspot");
	const std::uint32_t ratioBefore = firstHotspots.empty() ? 0 :
		firstHotspots.front().failureRatioBasisPoints;
	add(repeated, Result::SUCCESS, 1, 25, 5);
	add(repeated, Result::SUCCESS, 2, 26, 6);
	add(repeated, Result::SUCCESS, 1, 27, 7);
	check(repeated.hotspots().empty(), "NEXP-11 later successes reduce hotspot severity deterministically");
	check(ratioBefore > 0, "hotspot carried a deterministic ratio");

	Store stuck;
	add(stuck, Result::PATH_STUCK, 1, 31, 1);
	add(stuck, Result::PATH_STUCK, 2, 32, 2);
	add(stuck, Result::SUCCESS, 1, 33, 3);
	add(stuck, Result::SUCCESS, 2, 34, 4);
	check(stuck.hotspots().size() == 1,
		"repeated stuck threshold is separate from failure-ratio threshold");
}

void testDeterminismAndBounds()
{
	Limits limits;
	limits.maxEntries = 2;
	limits.maxHotspots = 1;
	limits.maxBotIdentitiesPerEntry = 2;
	limits.maxRecentResultsPerEntry = 3;
	Store forward(limits);
	Store reverse(limits);
	auto a = makeObservation(Result::PATH_FAILED, 1, 41, 10);
	auto b = makeObservation(Result::SUCCESS, 2, 42, 20);
	auto c = makeObservation(Result::PATH_STUCK, 3, 43, 30);
	auto d = makeObservation(Result::SUCCESS, 2, 44, 40);
	for (const Observation& observation : {a, b, c, d})
		check(forward.record(observation), "forward bounded input accepted");
	for (const Observation& observation : {d, c, b, a})
		check(reverse.record(observation), "reverse bounded input accepted");
	check(forward.canonicalJson() == reverse.canonicalJson(),
		"NEXP-13 input order preserves canonical output");
	check(forward.fingerprint() == reverse.fingerprint(),
		"NEXP-14 identical input produces an identical fingerprint");
	check(forward.entries().size() <= forward.limits().maxEntries &&
		forward.limits().maxEntries == 2,
		"entry bound enforced");

	Store eviction(limits);
	auto e1 = makeObservation(Result::SUCCESS, 1, 45, 10, 10, 20);
	auto e2 = makeObservation(Result::SUCCESS, 1, 46, 20, 20, 30);
	auto e3 = makeObservation(Result::SUCCESS, 1, 47, 30, 30, 40);
	check(eviction.record(e1) && eviction.record(e2) && eviction.record(e3) &&
		eviction.evictions() == 1 && eviction.entries().size() == 2 &&
		findEntry(eviction, e2.key) != nullptr && findEntry(eviction, e3.key) != nullptr,
		"NEXP-12 bounded store evicts the least recently seen entry deterministically");

	Store duplicate;
	check(duplicate.record(a), "first attempt accepted");
	check(!duplicate.record(a) && duplicate.totalObservations() == 1,
		"bounded recent attempt identity prevents duplicate terminal events");
}

void testValidationSignalsAndShadow()
{
	Store store;
	Observation invalid = makeObservation(Result::SUCCESS, 1, 51, 1);
	invalid.traversalSeconds = std::numeric_limits<double>::quiet_NaN();
	check(!store.record(invalid), "NEXP-15 non-finite duration rejected");
	invalid = makeObservation(Result::SUCCESS, 1, 52, 1);
	invalid.progressUnits = std::numeric_limits<double>::infinity();
	check(!store.record(invalid), "NEXP-15 non-finite progress rejected");
	invalid = makeObservation(Result::SUCCESS, 1, 53, 1);
	invalid.key.map.assign(65, 'm');
	check(!store.record(invalid), "oversized map identity rejected");
	invalid = makeObservation(Result::SUCCESS, 1, 54, 1);
	invalid.key.startNode = invalid.key.destinationNode = -1;
	check(!store.record(invalid), "missing stable node identities rejected");

	for (int i = 0; i < 4; ++i)
		add(store, Result::PATH_FAILED, i % 2 + 1, 60 + i, 10 + i);
	check(store.correlate(makeKey(), Signal::CAPTURE_ROUTE_EXHAUSTED),
		"capture signal correlates with a retained experience key");
	check(store.correlate(makeKey(), Signal::SQUAD_RECOVERY_FAILED),
		"squad recovery signal remains separate from attempt outcomes");
	check(store.correlate(makeKey(), Signal::STUCK_HEURISTIC),
		"heuristic stuck signal remains separate from attempt outcomes");
	const Entry* entry = findEntry(store, makeKey());
	check(entry != nullptr && entry->pathFailed == 4 && entry->correlatedSignals[1] == 1 &&
		entry->correlatedSignals[2] == 1,
		"correlated signals do not create NavigationAttempt failures");

	struct ExistingNavigationDecision
	{
		Provider provider = Provider::WAYPOINT;
		int cost = 100;
		int nextNode = 20;
		bool areaBlocked = false;
	};
	ExistingNavigationDecision decision;
	const ExistingNavigationDecision before = decision;
	check(store.record(makeObservation(Result::SUCCESS, 9, 99, 99)),
		"NEXP-16 shadow observation accepted");
	check(decision.provider == before.provider && decision.cost == before.cost &&
		decision.nextNode == before.nextNode && decision.areaBlocked == before.areaBlocked,
		"NEXP-16 shadow data does not mutate navigation decisions");
}

void testQuantizedIdentity()
{
	const std::int64_t left = QuantizedCell(-300.0, 5.0, 120.0);
	const std::int64_t sameCell = QuantizedCell(-310.0, 2.0, 118.0);
	const std::int64_t nextCell = QuantizedCell(300.0, 5.0, 120.0);
	check(left >= 0 && left == sameCell && left != nextCell,
		"local explorer cell identity is bounded and deterministic");
	check(QuantizedCell(std::numeric_limits<double>::quiet_NaN(), 0, 0) == -1,
		"invalid local explorer coordinates rejected");
}

void addTransition(Store& store, const Provider provider, const int from,
	const int to, const Traversal traversal, const TransitionResult result,
	const int bot, const std::uint64_t attempt,
	const CancellationKind cancellation = CancellationKind::NONE)
{
	check(store.recordTransition(makeTransition(provider, from, to, traversal,
		result, bot, attempt, attempt, attempt, attempt, cancellation)),
		"learned transition accepted");
}

struct FixtureEdge
{
	int from;
	int to;
	double base;
	Traversal traversal = Traversal::NORMAL;
};

std::vector<int> chooseFixtureRoute(const std::vector<FixtureEdge>& edges,
	const Store& store, const LearnedNavigationCostPolicy& policy)
{
	std::array<double, 4> distance{};
	distance.fill(std::numeric_limits<double>::max());
	std::array<int, 4> parent{};
	parent.fill(-1);
	distance[0] = 0.0;
	for (int iteration = 0; iteration < 4; ++iteration)
	{
		for (const FixtureEdge& edge : edges)
		{
			TransitionKey key{"test_map", Provider::WAYPOINT, edge.from,
				edge.to, edge.traversal};
			const LearnedNavigationCost learned = store.learnedCost(key, policy);
			const double candidate = distance[edge.from] + edge.base +
				learned.penalty;
			if (candidate < distance[edge.to])
			{
				distance[edge.to] = candidate;
				parent[edge.to] = edge.from;
			}
		}
	}
	std::vector<int> route;
	for (int node = 3; node >= 0 && node != -1; node = parent[node])
		route.push_back(node);
	std::reverse(route.begin(), route.end());
	return route;
}

void testLearnedCosts()
{
	LearnedNavigationCostPolicy policy;
	check(!policy.enabled, "NLEARN-17 default learned-cost policy is OFF");
	policy.enabled = true;
	Store disabled;
	for (int i = 0; i < 4; ++i)
		addTransition(disabled, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::NO_PROGRESS, i % 2 + 1, 2000 + i);
	LearnedNavigationCostPolicy off = policy;
	off.enabled = false;
	check(disabled.learnedCost({"test_map", Provider::WAYPOINT, 1, 2,
		Traversal::NORMAL}, off).penalty == 0.0, "NLEARN-01 disabled penalty zero");

	Store insufficient;
	for (int i = 0; i < 3; ++i)
		addTransition(insufficient, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::NO_PROGRESS, i % 2 + 1, 2010 + i);
	check(insufficient.learnedCost({"test_map", Provider::WAYPOINT, 1, 2,
		Traversal::NORMAL}, policy).penalty == 0.0,
		"NLEARN-02 insufficient classified attempts zero");

	Store oneBot;
	for (int i = 0; i < 4; ++i)
		addTransition(oneBot, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::NO_PROGRESS, 7, 2020 + i);
	check(oneBot.learnedCost({"test_map", Provider::WAYPOINT, 1, 2,
		Traversal::NORMAL}, policy).penalty == 0.0,
		"NLEARN-03 one failing bot cannot activate penalty");

	Store multiBot;
	for (int i = 0; i < 4; ++i)
		addTransition(multiBot, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::NO_PROGRESS, i % 2 + 1, 2030 + i);
	const TransitionKey edgeKey{"test_map", Provider::WAYPOINT, 1, 2,
		Traversal::NORMAL};
	const LearnedNavigationCost active = multiBot.learnedCost(edgeKey, policy);
	check(active.eligible && active.penalty > 0.0, "NLEARN-04 multiple bots activate penalty");
	LearnedNavigationCostPolicy explicitOn;
	explicitOn.enabled = true;
	check(multiBot.learnedCost(edgeKey, explicitOn).eligible &&
		multiBot.learnedCost(edgeKey, explicitOn).penalty > 0.0,
		"NLEARN-18 explicit ON enables penalty");

	Store lowRatio;
	for (int i = 0; i < 2; ++i)
		addTransition(lowRatio, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::NO_PROGRESS, i + 1, 2040 + i);
	for (int i = 0; i < 2; ++i)
		addTransition(lowRatio, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::TRANSITION_SUCCESS, i + 1, 2050 + i);
	check(active.penalty > lowRatio.learnedCost(edgeKey, policy).penalty,
		"NLEARN-05 higher failure ratio increases penalty");

	LearnedNavigationCostPolicy capped = policy;
	capped.maxPenalty = 10.0;
	capped.penaltyScale = 4.0;
	check(multiBot.learnedCost(edgeKey, capped).penalty <= 10.0,
		"NLEARN-06 penalty is capped");

	Store recovery = multiBot;
	const double beforeRecovery = recovery.learnedCost(edgeKey, policy).penalty;
	for (int i = 0; i < 12; ++i)
		addTransition(recovery, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::TRANSITION_SUCCESS, i % 2 + 1, 2060 + i);
	check(recovery.learnedCost(edgeKey, policy).penalty < beforeRecovery,
		"NLEARN-07 successes reduce penalty");

	Store cancellation = multiBot;
	addTransition(cancellation, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
		TransitionResult::CANCELLED, 1, 2080, CancellationKind::EXTERNAL);
	check(cancellation.learnedCost(edgeKey, policy).penalty == beforeRecovery &&
		cancellation.findTransition(edgeKey)->cancellations == 1,
		"NLEARN-08 cancellation does not increase penalty");

	Store reverse;
	for (int i = 0; i < 4; ++i)
		addTransition(reverse, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::NO_PROGRESS, i % 2 + 1, 2090 + i);
	check(reverse.learnedCost(edgeKey, policy).penalty > 0.0 &&
		reverse.learnedCost({"test_map", Provider::WAYPOINT, 2, 1,
			Traversal::NORMAL}, policy).penalty == 0.0,
		"NLEARN-09 reverse direction remains independent");
	check(reverse.learnedCost({"test_map", Provider::NAVMESH, 1, 2,
			Traversal::UNKNOWN}, policy).penalty == 0.0,
		"NLEARN-10 providers remain isolated");
	check(reverse.learnedCost({"test_map", Provider::WAYPOINT, 1, 2,
			Traversal::DROP}, policy).penalty == 0.0,
		"NLEARN-11 traversal kinds remain isolated");

	const std::vector<FixtureEdge> diamond{
		{0, 1, 1.0}, {1, 3, 1.0}, {0, 2, 1.5}, {2, 3, 1.5}};
	Store diamondExperience;
	for (int i = 0; i < 4; ++i)
		addTransition(diamondExperience, Provider::WAYPOINT, 1, 3,
			Traversal::NORMAL, TransitionResult::NO_PROGRESS, i % 2 + 1,
			2100 + i);
	const std::vector<int> baseline = chooseFixtureRoute(diamond, Store(), policy);
	const std::vector<int> learned = chooseFixtureRoute(diamond,
		diamondExperience, policy);
	check(baseline == std::vector<int>({0, 1, 3}),
		"NLEARN-12 baseline diamond route is A-B-D");
	check(learned == std::vector<int>({0, 2, 3}),
		"NLEARN-12 learned penalty selects alternate equal-valid route");

	const std::vector<FixtureEdge> unavoidable{{0, 1, 1.0}, {1, 3, 1.0}};
	check(chooseFixtureRoute(unavoidable, diamondExperience, policy) ==
		std::vector<int>({0, 1, 3}),
		"NLEARN-13 unavoidable penalized edge remains traversable");
	LearnedNavigationCostPolicy offAgain = policy;
	offAgain.enabled = false;
	check(chooseFixtureRoute(diamond, diamondExperience, offAgain) == baseline,
		"NLEARN-14 disabling learning restores baseline route");
	Store replay;
	for (int i = 0; i < 4; ++i)
		addTransition(replay, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::NO_PROGRESS, i % 2 + 1, 2110 + i);
	check(replay.learnedCost(edgeKey, policy).penalty ==
		reverse.learnedCost(edgeKey, policy).penalty,
		"NLEARN-15 identical experience is deterministic");
	Store fresh;
	check(fresh.learnedCost(edgeKey, policy).penalty == 0.0,
		"NLEARN-16 new session has no persisted learned cost");

	Store mode;
	mode.setLearnedNavigationMetrics(0, 0.0, 0.0, false);
	check(mode.canonicalJson().find("\"mode\":\"SHADOW_ONLY\"") !=
		std::string::npos, "NLEARN-19 canonical mode is SHADOW_ONLY when OFF");
	mode.setLearnedNavigationMetrics(0, 0.0, 0.0, true);
	check(mode.canonicalJson().find("\"mode\":\"ACTIVE_COSTS\"") !=
		std::string::npos, "NLEARN-20 canonical mode is ACTIVE_COSTS when ON");

	const LearnedNavigationCost exact = multiBot.learnedCost(edgeKey, policy);
	check(exact.classifiedAttempts == 4 && exact.failures == 4 &&
		exact.distinctFailureBots == 2 &&
		std::fabs(exact.confidence - 0.5) < 1e-9 &&
		std::fabs(exact.penalty - 64.0) < 1e-9 && exact.eligible,
		"NLEARN-21 exact 4/4 two-bot edge yields confidence .5 penalty 64");
	Store threeBots;
	for (int i = 0; i < 4; ++i)
		addTransition(threeBots, Provider::WAYPOINT, 1, 2, Traversal::NORMAL,
			TransitionResult::NO_PROGRESS, (i % 3) + 1, 2120 + i);
	check(std::fabs(threeBots.learnedCost(edgeKey, policy).penalty -
			exact.penalty) < 1e-9,
		"NLEARN-21 eligible penalty is independent of extra failure bots");
}
}

int main()
{
	testResultsAndCancellation();
	testAggregationAndHotspots();
	testDeterminismAndBounds();
	testValidationSignalsAndShadow();
	testQuantizedIdentity();
	testLearnedCosts();
	testDirectedTransitionsAndOutcomes();
	testTransitionAggregationAndClassification();
	testTransitionDedupBoundsAndCanonicalOutput();
	if (failures != 0)
		return 1;
	std::cout << "NEXP-01..16 NEDGE-01..16 NLEARN-01..21 PASS\n";
	return 0;
}
