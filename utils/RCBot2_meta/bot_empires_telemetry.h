#ifndef __RCBOT_EMPIRES_TELEMETRY_H__
#define __RCBOT_EMPIRES_TELEMETRY_H__

#include "bot_navigation_experience.h"
#include "bot_local_conflict_observation.h"
#include "bot_recovery_episode.h"

class CBot;
class Vector;
struct edict_t;

namespace RCBotNavigation
{
	class NavigationAttempt;
	struct NavigationIntent;
	enum class Result : std::uint8_t;
}

namespace RCBotOrderTelemetry
{
	struct TelemetryEvent;
}

namespace RCBotEmpiresSquadRuntime
{
	struct Telemetry;
}

namespace EmpiresAnalysis
{
	struct CommanderOrderObservation
	{
		int recipientUserId = -1;
		int commanderUserId = -1;
		int team = -1;
		int squad = -1;
		int commandType = 0;
		int targetEntityIndex = -1;
		bool targetEntityKnown = false;
		float targetX = 0.0f;
		float targetY = 0.0f;
		float targetZ = 0.0f;
		bool targetPositionKnown = false;
		float cancelTime = 0.0f;
		bool cancelTimeKnown = false;
	};

	// The recorder is deliberately a passive sink.  Callers may use this as a
	// cheap guard around state extraction in hot paths.
	bool IsActive();

	bool Start(const char* mapName);
	bool Stop();
	void Mark(const char* text);

	// Records an already observed event.  target/detail are optional context;
	// neither argument is dereferenced or modified by the recorder.
	void RecordBotEvent(CBot* bot, const char* event,
		const edict_t* target = nullptr, const char* detail = nullptr);
	// Record changed live capture-point state in the active session stream.
	// The caller supplies an already observed, stable text representation.
	void RecordCapturePointState(int entityIndex, int pointIndex,
		const char* state);
	// Poll the recipient's authoritative Empires order SendProps. Calls are
	// deduplicated and serialized as create/update/remove transitions.
	void ObserveCommanderOrder(const CommanderOrderObservation& observation);
	// Record Empires' native capture event fields without changing game state.
	void RecordNativeCaptureEvent(const char* eventName, bool team, int location);

	// CFindPathTask uses this for the exact request/success/failure boundary.
	// targetWaypoint is the task's requested waypoint, or -1 for a vector/entity
	// route.  This does not inspect or modify navigator state.
	void RecordPathEvent(CBot* bot, const char* event, int targetWaypoint,
		const char* detail = nullptr);

	// Observe-only Recovery Episode lifecycle. These calls serialize an
	// already-selected value identity and never acquire movement authority.
	void RecordRecoveryEpisodeBegin(CBot* bot,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		const char* reason = nullptr);
	void RecordRecoveryEpisodeReuse(CBot* bot,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		const char* reason = nullptr);
	void RecordRecoveryEpisodeProgress(CBot* bot, const char* reason = nullptr);
	void RecordRecoveryEpisodeReplacement(CBot* bot,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		const char* reason = nullptr);
	void RecordRecoveryEpisodeEnd(CBot* bot,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		const char* reason = nullptr);
	void RecordRecoveryPolicyEvent(CBot* bot, const char* event,
		const RCBotRecovery::RecoveryEpisodeIdentity& identity,
		int consecutiveExhaustions, const char* reason = nullptr);

	// Passive observation at the existing NavigationAttempt lifecycle boundary.
	// These functions never write movement state or influence provider arbitration.
	void RecordNavigationAttemptStarted(CBot* bot,
		const RCBotNavigation::NavigationIntent& intent,
		const RCBotNavigation::NavigationAttempt& attempt);
	void RecordNavigationAttemptTerminal(CBot* bot,
		const RCBotNavigation::NavigationIntent& intent,
		const RCBotNavigation::NavigationAttempt& attempt,
		RCBotNavigation::Result result, const char* reason);
	void RecordNavigationTransition(CBot* bot,
		const RCBotNavigation::NavigationIntent& intent,
		const RCBotNavigation::NavigationAttempt& attempt,
		std::int64_t fromNode, std::int64_t toNode,
		RCBotNavigationExperience::Traversal traversal,
		RCBotNavigationExperience::TransitionResult result,
		RCBotNavigationExperience::CancellationKind cancellation =
			RCBotNavigationExperience::CancellationKind::NONE,
		const char* reason = nullptr);
	void MarkNavigationTransitionStarted(CBot* bot,
		const RCBotNavigation::NavigationIntent& intent,
		const RCBotNavigation::NavigationAttempt& attempt,
		std::int64_t fromNode, std::int64_t toNode,
		RCBotNavigationExperience::Traversal traversal);
	void CorrelateNavigationSignal(CBot* bot,
		RCBotNavigationExperience::Signal signal);

	// Active v1 learned-cost query. The returned value is session-scoped and
	// uses only the directed Transition Experience store. Local Explorer and
	// unknown/cancellation evidence resolve to a zero penalty.
	RCBotNavigationExperience::LearnedNavigationCost EvaluateLearnedNavigationCost(
		RCBotNavigationExperience::Provider provider, std::int64_t fromNode,
		std::int64_t toNode, RCBotNavigationExperience::Traversal traversal);
	void RecordLearnedNavigationEdgeConsidered(CBot* bot,
		RCBotNavigationExperience::Provider provider, std::int64_t fromNode,
		std::int64_t toNode, RCBotNavigationExperience::Traversal traversal,
		double baseCost, double effectiveCost,
		const RCBotNavigationExperience::LearnedNavigationCost& cost);
	void RecordLearnedNavigationBehaviorChange(CBot* bot,
		RCBotNavigationExperience::Provider provider, std::int64_t fromNode,
		std::int64_t toNode, RCBotNavigationExperience::Traversal traversal,
		double baseCost, double effectiveCost);
	std::uint64_t LearnedNavigationBehaviorChanges();
	std::uint64_t RecoveryEpisodeBehaviorChanges();
	std::uint64_t LocalAvoidanceBehaviorChanges();
	// Called only at the final movement-output edge after WAIT has actually
	// changed forward/side input. Observation and unchanged decisions do not
	// increment this counter.
	void RecordLocalYieldBehaviorChange(CBot* bot, const char* reason = nullptr);

	// Records an existing Commander order with a recorder-owned stable order ID
	// and the actual world click position.  This is observation only; callers
	// pass the state they already read from Empires.
	int RecordCommanderEvent(CBot* bot, const char* event,
		const Vector& targetOrigin, const char* detail = nullptr,
		const edict_t* target = nullptr);

	// Records a headless Commander Order Protocol event in the same structured
	// events.jsonl stream.  The protocol adapter owns lifecycle truth; this
	// recorder only serializes the already classified event.
	void RecordOrderProtocolEvent(CBot* bot,
		const RCBotOrderTelemetry::TelemetryEvent& event);

	// Structured, semantic-only telemetry for the Phase-11.5 shadow adapter.
	// The runtime adapter supplies value-only fields; this recorder adds the
	// normal observed bot context and writes one JSONL event per transition.
	void RecordSquadShadowEvent(CBot* bot,
		const RCBotEmpiresSquadRuntime::Telemetry& event);

	// Poll an Empires bot.  Internally sampled and deduplicated; no JSON is
	// emitted per game tick.
	void ObserveBot(CBot* bot);

	// Human-readable status for rcbot_analysis_status and command feedback.
	const char* GetSessionId();
	const char* GetSessionPath();
	const char* GetLastError();
	int GetEventCount();
	int GetBotCount();
	double GetDurationSeconds();
}

#endif // __RCBOT_EMPIRES_TELEMETRY_H__
