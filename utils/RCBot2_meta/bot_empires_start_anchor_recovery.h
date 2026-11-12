// Bounded start-anchor recovery policy: engine-independent decision and
// state for capture routes that drain with NO_START_ANCHOR.
//
// A NO_START_ANCHOR drain fails route anchoring before any useful capture
// route exists: no waypoint is reachable from the bot's current position, so
// re-issuing the identical capture route from the same stationary position
// reproduces the same failure deterministically. This header answers only
// "should this drain start (or finish) a re-anchor leg instead of normal
// retry accounting", and tracks the single bounded attempt per capture
// context. It owns no schedules, timers, navigation, movement, combat, or
// selection policy; the engine-bound driver (CBotEmpires) probes anchors,
// issues the re-anchor schedule, and emits telemetry.
//
// Budgets: the existing capture-route retry budget (kMaxCaptureRouteRetries)
// keeps counting genuine route attempts/stalls only. Start-anchor recovery
// has its own single attempt per capture point (kMaxStartAnchorRecoveryAttempts).
// Destination-side failures (NO_GOAL_ANCHOR, NO_REACHABLE_ANCHOR),
// terminal/provider failures, stuck/move failures, and combat interruptions
// all map to NORMAL_ACCOUNTING and are untouched by this policy.
#ifndef BOT_EMPIRES_START_ANCHOR_RECOVERY_H
#define BOT_EMPIRES_START_ANCHOR_RECOVERY_H

namespace RCBotEmpiresStartAnchorRecovery
{

// One re-anchor leg per capture point context. The leg either restores a
// resolvable start anchor (normal routing resumes) or drains still
// off-graph (existing bounded retry/reject accounting takes over).
constexpr int kMaxStartAnchorRecoveryAttempts = 1;

// Negligible progress since the route was issued: at or below the CMoveToTask
// arrival radius, the bot factually arrived nowhere, so the drain is a
// zero-progress episode rather than a partial approach.
constexpr float kNegligibleCaptureProgressDistance = 64.0f;

// Re-entry target search radius for the nearest team-usable waypoint (~5x the
// anchor REACHABLE_RANGE). Beyond this the bot is better served by the
// existing reject/cooldown path than by a long blind direct leg.
constexpr float kMaxStartAnchorReentryDistance = 2048.0f;

enum class DrainAction
{
	NORMAL_ACCOUNTING = 0,
	START_RECOVERY
};

// Decides whether a drained capture approach should start a bounded
// start-anchor recovery leg instead of consuming capture retry budget.
// anchorNotReady: the fresh task terminal for this drain was
// FIND_PATH_ANCHOR_NOT_READY. noStartAnchor: a read-only anchor probe at
// drain time reports NO_START_ANCHOR (start-side, not destination-side).
inline DrainAction DecideCaptureDrainAction(const bool combatResume,
	const bool anchorNotReady, const bool noStartAnchor,
	const bool pointStillValid, const float displacementSinceIssue,
	const int recoveryAttemptsUsedForPoint)
{
	if (!combatResume && anchorNotReady && noStartAnchor &&
		pointStillValid &&
		displacementSinceIssue <= kNegligibleCaptureProgressDistance &&
		recoveryAttemptsUsedForPoint < kMaxStartAnchorRecoveryAttempts)
	{
		return DrainAction::START_RECOVERY;
	}
	return DrainAction::NORMAL_ACCOUNTING;
}

// Decides whether a drained recovery leg restores normal capture routing:
// no combat preemption, the point is still valid, and a read-only probe
// shows anchors resolvable again from the bot's new position.
inline bool ShouldReissueNormalRouteAfterRecovery(const bool combatResume,
	const bool pointStillValid, const bool anchorsReadyNow)
{
	return !combatResume && pointStillValid && anchorsReadyNow;
}

// Per-bot recovery state bound to one capture point. Attempts are keyed by
// point so a new objective starts fresh; reset() clears everything on
// capture teardown, death, setup, fixture release, recovery, or map-memory
// free. finish() clears only the active leg; used attempts stay consumed for
// the point so a repeated NO_START_ANCHOR cannot loop.
struct StartAnchorRecoveryState
{
	int point = -1;
	int attemptsUsed = 0;
	bool active = false;

	void reset()
	{
		point = -1;
		attemptsUsed = 0;
		active = false;
	}

	int attemptsUsedFor(const int capturePoint) const
	{
		return point == capturePoint ? attemptsUsed : 0;
	}

	void begin(const int capturePoint)
	{
		if (point != capturePoint)
		{
			point = capturePoint;
			attemptsUsed = 0;
		}
		active = true;
		++attemptsUsed;
	}

	void finish()
	{
		active = false;
	}
};

} // namespace RCBotEmpiresStartAnchorRecovery

#endif // BOT_EMPIRES_START_ANCHOR_RECOVERY_H
