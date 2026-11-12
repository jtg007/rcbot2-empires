// Combat-aware capture retry accounting: engine-independent policy for the
// bounded capture-route retry budget.
//
// A capture approach owns a small bounded number of route retries for genuine
// routing/stall failure ("schedule completed before arrival"). Tactical combat
// temporarily takes schedule ownership (approach/search install wipes the live
// capture route); when combat ends the empty schedule must resume the still
// valid capture WITHOUT consuming that navigation budget.
//
// This header answers only "why did this capture schedule disappear"
// (COMBAT_INTERRUPTION vs NORMAL) and whether a given resume consumes one
// retry. It owns no schedules, timers, navigation, or combat policy; the
// engine-bound driver (CBotEmpires) sets/clears the latch at the combat wipe
// sites and consults ShouldConsumeCaptureRouteRetry in the retry branch.
#ifndef BOT_EMPIRES_CAPTURE_RETRY_H
#define BOT_EMPIRES_CAPTURE_RETRY_H

namespace RCBotEmpiresCaptureRetry
{

// Bounded retries for genuine capture-route failure before arrival. This must
// stay small: combat resumption preserves the budget, it never extends it.
constexpr int kMaxCaptureRouteRetries = 2;

// One-shot cause marker: set when tactical combat removes a live capture
// schedule, consumed (cleared) by the next capture accounting decision.
struct CaptureInterruptionLatch
{
	bool interruptedByCombat = false;

	void reset()
	{
		interruptedByCombat = false;
	}

	void noteCombatInterruption()
	{
		interruptedByCombat = true;
	}

	// Reports whether combat caused the disappearance, then clears. Every
	// accounting path must take exactly once per disappearance.
	bool takeCombatInterruption()
	{
		const bool wasInterrupted = interruptedByCombat;
		interruptedByCombat = false;
		return wasInterrupted;
	}
};

// Returns true when resuming a capture approach must consume one retry from
// the bounded budget. A combat interruption against a still-valid objective
// preserves the budget; every other resume (genuine stall, stale objective)
// consumes one retry exactly as before.
inline bool ShouldConsumeCaptureRouteRetry(
	const bool scheduleInterruptedByCombat,
	const bool objectiveStillValid)
{
	return !(scheduleInterruptedByCombat && objectiveStillValid);
}

} // namespace RCBotEmpiresCaptureRetry

#endif // BOT_EMPIRES_CAPTURE_RETRY_H
