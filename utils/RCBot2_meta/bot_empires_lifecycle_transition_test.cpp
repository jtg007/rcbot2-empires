// T1 lifecycle consolidation: transition/invariant coverage for the
// engine-independent value cores that Empires lifecycle paths operate on.
//
// Scope discipline (see final T1 report):
// - These tests exercise REAL production code (Adapter, SquadMoveRecipient,
//   CommanderMoveOrderAdapter, EpisodeTracker, SquadJoin::Select), never a
//   hand-rolled reset sequence and never a copy of production logic.
// - They are PRIMITIVE/INVARIANT coverage, not lifecycle-wiring proof. The
//   full player_death -> CBotEmpires::died -> CBot::died chain, disconnect
//   entry points, team-change and round-restart wiring are UNPROVEN
//   ENGINE-BOUND and are verified via the autonomous harness experiments
//   specified in the T1 report.
// - Legacy CBotSquad container code (bot_squads.cpp) cannot link headlessly
//   (SDK vector.h / engine deps); no stub framework was built for T1. The
//   legacy/Empires boundary rests on construction-site reachability evidence:
//   CBotJoinSquad is constructed only in bot_dod_bot.cpp:1988,2006 (DOD-only
//   getTasks); setSquad() call sites are bot_dod_bot.cpp:1881,1906 and
//   bot_task.cpp:5738 (generic CBotJoinSquad::execute); bot_empires.cpp never
//   calls setSquad/AddSquadMember; CBotEmpires::getTasks falls through to the
//   squad-task-free CBot::getTasks only. Provisional verdict: CONFIRMED
//   LEGACY-ONLY / NOT A BUG FOR EMPIRES, pending the runtime inSquad()==0
//   probe. Ghost-handle retention (MyEHandle dead entries linger in the deque
//   while numMembers() stays size-based) is CONFIRMED by code trace
//   (bot_ehandle.h:66-77, bot_squads.cpp:79-87,393-399).

#include "bot_empires_squad_runtime.h"
#include "bot_empires_squad_join.h"
#include "bot_empires_commander_order_adapter.h"
#include "bot_recovery_episode.h"
#include "bot_squad_order_recipient.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace RCBotOrder;
using namespace RCBotOrderTelemetry;
using namespace RCBotSquadDecision;
using namespace RCBotEmpiresSquadRuntime;
using namespace RCBotEmpiresSquadJoin;
using namespace RCBotCommanderOrder;
using namespace RCBotRecovery;
using namespace RCBotSquadOrder;

void Require(const bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << std::endl;
		std::exit(1);
	}
}

// --- SquadRuntime helpers (mirror bot_empires_squad_runtime_test.cpp) ---

TargetPosition Position(const float x, const float y, const float z = 0.0f)
{
	return { x, y, z };
}

MemberObservation RuntimeMember(const RecipientId id,
	const TargetPosition origin, const bool leader = false)
{
	MemberObservation result;
	result.state.memberId = id;
	result.state.alive = true;
	result.state.connected = true;
	result.state.eligible = true;
	result.state.isLeader = leader;
	result.state.squadPosition = leader ? 0 : static_cast<int>(id - 100);
	result.state.origin = origin;
	result.state.velocity = Position(0, 0, 0);
	result.state.lifecycle = MemberLifecycle::ACTIVE;
	return result;
}

RCBotEmpiresSquadRuntime::Input RuntimeInput(const int team, const RecipientId squad)
{
	RCBotEmpiresSquadRuntime::Input input;
	input.team = team;
	input.squadId = squad;
	input.now = 1.0;
	input.members = { RuntimeMember(101, Position(0, 0), true),
		RuntimeMember(102, Position(64, 0)) };
	input.parentOrder.present = true;
	input.parentOrder.target = Position(128, 0);
	input.parentOrder.deadline = 20.0;
	return input;
}

// --- SquadMoveRecipient helpers (mirror bot_squad_order_recipient_test.cpp) ---

struct Capture
{
	std::vector<NavigationIntentRequest> starts;
	std::vector<std::uint64_t> cancels;
	std::vector<TelemetryEvent> events;
};

bool StartNavigation(void* context, const NavigationIntentRequest& request,
	std::uint64_t* navigationIntentId)
{
	Capture* capture = static_cast<Capture*>(context);
	capture->starts.push_back(request);
	*navigationIntentId = 500 + capture->starts.size();
	return true;
}

void CancelNavigation(void* context, const NavigationIntentRequest&,
	const std::uint64_t navigationIntentId)
{
	static_cast<Capture*>(context)->cancels.push_back(navigationIntentId);
}

void CaptureTelemetry(void* context, const TelemetryEvent& event)
{
	static_cast<Capture*>(context)->events.push_back(event);
}

SquadMembershipSnapshot Snapshot(const Generation generation = 1)
{
	return { OrderRecipient::Squad(42, generation), { 101, 102 } };
}

SquadMoveSpec Spec(const OrderId orderId = 7001,
	const Generation generation = 1)
{
	return { orderId, generation, 9, Snapshot(generation),
		{ 100.0f, 200.0f, 300.0f }, 10.0, 310.0 };
}

SquadMoveRecipient NewRecipient(Capture& capture)
{
	return SquadMoveRecipient(StartNavigation, CancelNavigation,
		CaptureTelemetry, &capture);
}

// LT1: map/level reset invalidates every squad scope at once. This is the
// exact production operation CBotEmpires::freeMapMemory performs on the
// global shadow runtime (bot_empires.cpp: g_empireSquadRuntimeShadow.Reset()).
void TestMapResetInvalidatesAllSquadRuntime()
{
	Adapter adapter;
	const RCBotEmpiresSquadRuntime::Result nf = adapter.Observe(RuntimeInput(2, 7));
	const RCBotEmpiresSquadRuntime::Result be = adapter.Observe(RuntimeInput(3, 7));
	const RCBotEmpiresSquadRuntime::Result nfOther = adapter.Observe(RuntimeInput(2, 9));
	Require(nf.Accepted() && be.Accepted() && nfOther.Accepted(),
		"LT1 three squad scopes observed");
	adapter.Reset();
	Require(adapter.RejectStaleCallback(2, 7,
		nf.identity.squadGeneration, nf.decisionRevision),
		"LT1 stale NF squad callback rejected after map reset");
	Require(adapter.RejectStaleCallback(3, 7,
		be.identity.squadGeneration, be.decisionRevision),
		"LT1 stale BE squad callback rejected after map reset");
	Require(adapter.RejectStaleCallback(2, 9,
		nfOther.identity.squadGeneration, nfOther.decisionRevision),
		"LT1 stale second-squad callback rejected after map reset");
	const RCBotEmpiresSquadRuntime::Result fresh = adapter.Observe(RuntimeInput(2, 7));
	Require(fresh.Accepted(), "LT1 fresh observation accepted after map reset");
	Require(!adapter.RejectStaleCallback(2, 7,
		fresh.identity.squadGeneration, fresh.decisionRevision),
		"LT1 current callback remains current after map reset");
	std::cout << "LT1 PASS\n";
}

// LT2: recipient reset clears the active parent and retires children. This
// mirrors map/disconnect cleanup of per-squad recipient state: no old parent
// key may authorize work after the transition.
void TestRecipientResetClearsActiveParent()
{
	Capture capture;
	SquadMoveRecipient recipient = NewRecipient(capture);
	Require(recipient.IssueMove(Spec()), "LT2 move issued");
	const OrderKey parent = recipient.ActiveParentKey();
	Require(recipient.HasActiveParent(), "LT2 parent active before reset");
	recipient.Reset();
	Require(!recipient.HasActiveParent(),
		"LT2 no active parent after reset");
	Require(!recipient.ObserveMembership(parent, Snapshot(), 21.0),
		"LT2 stale membership update rejected after reset");
	Require(!recipient.ObserveMemberLifecycle(parent, Snapshot().recipient,
		101, DetailReason::DEATH_OR_LIFECYCLE, 21.0),
		"LT2 stale lifecycle update rejected after reset");
	Require(recipient.IssueMove(Spec(7002, 1)),
		"LT2 new move accepted after reset");
	Require(recipient.HasActiveParent(),
		"LT2 new parent active after reset");
	std::cout << "LT2 PASS\n";
}

// LT3: commander adapter reset clears the active order, the remembered
// terminal and the generation counter. Mirrors map/respawn cleanup of
// per-bot commander-move shadow state.
void TestCommanderAdapterResetClearsOrderAndTerminal()
{
	CommanderMoveOrderAdapter adapter;
	adapter.ObserveNewMove(Position(10, 20), 900001, 5, 42, 1.0, 30.0);
	Require(adapter.HasActiveOrder(), "LT3 order active before reset");
	const OrderKey before = adapter.ActiveKey();
	Require(adapter.ObserveAbort(DetailReason::DEATH_OR_LIFECYCLE, 2.0),
		"LT3 death abort applied");
	Require(!adapter.HasActiveOrder() && adapter.HasLastTerminalOrder(),
		"LT3 terminal remembered before reset");
	adapter.Reset();
	Require(!adapter.HasActiveOrder() && !adapter.HasLastTerminalOrder(),
		"LT3 no active or terminal order after reset");
	adapter.ObserveNewMove(Position(30, 40), 900002, 5, 42, 3.0, 30.0);
	Require(adapter.HasActiveOrder(), "LT3 new order accepted after reset");
	Require(adapter.ActiveKey().generation == 1,
		"LT3 generation counter restarts after reset");
	Require(adapter.ActiveKey().orderId != before.orderId,
		"LT3 new key differs from pre-reset key");
	std::cout << "LT3 PASS\n";
}

// LT4: recovery-episode identity turns over on death/respawn. The production
// death path ends the episode (CBot::died / CBotEmpires::died) and spawnInit
// bumps the life generation (bot.cpp); the next episode must be a new
// identity, never a reuse of the pre-death one.
void TestEpisodeDeathRespawnTurnover()
{
	EpisodeTracker tracker;
	bool reused = true;
	const RecoveryEpisodeIdentity first = tracker.Start(7, 1,
		Domain::CAPTURE, "12", 3, &reused);
	Require(first.IsActive() && !reused, "LT4 first episode active");
	tracker.End();
	Require(!tracker.HasActive(), "LT4 death ends episode");
	const RecoveryEpisodeIdentity second = tracker.Start(7, 2,
		Domain::CAPTURE, "12", 3, &reused);
	Require(second.IsActive() && !reused,
		"LT4 respawn episode is not a reuse");
	Require(second.episodeId != first.episodeId,
		"LT4 respawn episode has a new id");
	Require(!first.SamePracticalGoal(second),
		"LT4 pre-death and post-respawn goals differ");
	const RecoveryEpisodeIdentity third = tracker.Start(7, 2,
		Domain::CAPTURE, "12", 3, &reused);
	Require(reused && third.episodeId == second.episodeId,
		"LT4 same-life same-goal reuses the episode");
	tracker.Reset();
	const RecoveryEpisodeIdentity mapped = tracker.Start(7, 3,
		Domain::CAPTURE, "12", 3, &reused);
	Require(mapped.IsActive() && mapped.episodeId == 1,
		"LT4 map reset restarts the episode counter");
	std::cout << "LT4 PASS\n";
}

// LT5: squad-join selection recontextualizes on new observations. This MODELS
// the team-change invariant (old-team candidates must not authorize a join
// after the change); the actual team-transition wiring is UNPROVEN
// ENGINE-BOUND (no player_team handler, no headless representation).
void TestJoinRecontextOnNewObservations()
{
	RCBotEmpiresSquadJoin::Input teamA;
	teamA.currentUserId = 11;
	teamA.squads = { { 2, 3, false } };
	const RCBotEmpiresSquadJoin::Result joinA = Select(teamA);
	Require(joinA.status == RCBotEmpiresSquadJoin::Status::JOIN &&
		joinA.squadId == 2,
		"LT5 team A observation selects squad 2");
	RCBotEmpiresSquadJoin::Input teamB;
	teamB.currentUserId = 11;
	teamB.squads = { { 4, 1, false } };
	const RCBotEmpiresSquadJoin::Result joinB = Select(teamB);
	Require(joinB.status == RCBotEmpiresSquadJoin::Status::JOIN &&
		joinB.squadId == 4,
		"LT5 team B observation selects squad 4, not stale squad 2");
	Require(joinB.squadId != joinA.squadId,
		"LT5 recontextualized selection differs from old context");
	RCBotEmpiresSquadJoin::Input exhausted = teamB;
	exhausted.attempts = kMaximumJoinAttempts;
	const RCBotEmpiresSquadJoin::Result blocked = Select(exhausted);
	Require(blocked.status == RCBotEmpiresSquadJoin::Status::NO_CANDIDATE,
		"LT5 exhausted attempts block joining despite candidates");
	RCBotEmpiresSquadJoin::Input assigned = teamB;
	assigned.ownSquad = 4;
	const RCBotEmpiresSquadJoin::Result already = Select(assigned);
	Require(already.status == RCBotEmpiresSquadJoin::Status::ALREADY_ASSIGNED,
		"LT5 assigned bot does not rejoin");
	std::cout << "LT5 PASS\n";
}

// LT6: full-roster loss terminalizes the parent as ABORTED with
// CHILDREN_EXHAUSTED. Pins the production recipient behaviour when every
// member disconnects or dies mid-move: survivors none, navigation cancelled,
// parent key retired so late callbacks cannot revive it.
void TestRecipientFullRosterLossRetiresParent()
{
	Capture capture;
	SquadMoveRecipient recipient = NewRecipient(capture);
	Require(recipient.IssueMove(Spec()), "LT6 move issued");
	const OrderKey parent = recipient.ActiveParentKey();
	Require(recipient.ObserveMemberLifecycle(parent, Snapshot().recipient,
		101, DetailReason::DEATH_OR_LIFECYCLE, 20.0),
		"LT6 first member loss accepted");
	Require(recipient.HasActiveParent(),
		"LT6 parent survives partial roster loss");
	Require(recipient.ObserveMemberLifecycle(parent, Snapshot().recipient,
		102, DetailReason::DEATH_OR_LIFECYCLE, 21.0),
		"LT6 second member loss accepted");
	Require(capture.cancels.size() == 2,
		"LT6 both child navigations cancelled");
	Require(!recipient.HasActiveParent(),
		"LT6 parent retired after full roster loss");
	Require(recipient.ParentSnapshot().outcome == OrderOutcome::ABORTED,
		"LT6 full loss aggregates to ABORTED");
	Require(!recipient.HasMemberInActiveSnapshot(101) &&
		!recipient.HasMemberInActiveSnapshot(102),
		"LT6 lost members absent from active snapshot");
	Require(!recipient.ObserveNavigationTerminal(parent, parent, 101, 501,
		NavigationTerminalResult::COMPLETE, 22.0),
		"LT6 late terminal for retired parent rejected");
	std::cout << "LT6 PASS\n";
}

} // namespace

int main()
{
	TestMapResetInvalidatesAllSquadRuntime();
	TestRecipientResetClearsActiveParent();
	TestCommanderAdapterResetClearsOrderAndTerminal();
	TestEpisodeDeathRespawnTurnover();
	TestJoinRecontextOnNewObservations();
	TestRecipientFullRosterLossRetiresParent();
	std::cout << "LT1..LT6 PASS\n";
	return 0;
}
