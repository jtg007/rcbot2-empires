// Objective-aware combat relevance gate: engine-independent policy deciding
// whether a visible enemy may displace (or keep displacing) an active valid
// capture objective.
//
// Problem: the visible-enemy branch of CBotEmpires::getTasks() preempts
// capture unconditionally, and the schedule-empty re-arm path re-issues the
// approach without bound, so a distant or fleeing enemy can repeatedly drag
// a bot far from its capture objective (tactical flapping / unbounded
// pursuit; see capability matrix "Combat vs objective relevance" PLANNED,
// "Pursuit termination" BROKEN).
//
// This header answers only two questions with live positions supplied by
// the engine-bound driver:
//  - ACQUIRE (DecideVisibleEnemyEngagement): may this visible enemy take
//    schedule ownership from the active capture? Consulted when initially
//    issuing APPROACH_ENEMY.
//  - CONTINUE (DecidePursuitContinuation): may this pursuit keep owning the
//    schedule? Consulted while an approach is active (keep-alive) and on the
//    schedule-empty re-arm path. A DROP lets the still-valid capture resume
//    through the existing drain path; the combat-interruption latch recorded
//    at the original wipe makes that resume free of retry budget.
//
// Non-goals, by design:
//  - No schedules, timers, navigation, weapons, or perception state live
//    here. The driver supplies distances plus the generic recentlyHurt flag
//    and performs the existing schedule operations itself.
//  - Combat damage is untouched: CBotEmpires::handleWeapons() engages any
//    valid visible enemy through normal input independently of the approach
//    schedule, so declining a chase never disarms the bot.
//  - Capture identity, the combat-interruption latch, and retry accounting
//    are owned by bot_empires_capture_retry.h / bot_empires_objective.cpp
//    and are not modified here. The DROP path relies on the latch already
//    recorded at the original combat wipe; it sets no latch itself.
//  - Without an owned capture (flag not issued, or point no longer valid)
//    both decisions defer to legacy behavior (ENGAGE / CONTINUE): combat
//    outside the capture context is byte-for-byte unchanged.
//
// Distance scale (Hammer units; gameplay-scale, not map-specific):
//  - Capture arrival/hold radius is 160; capture points on played maps sit
//    thousands of units apart; infantry weapons remain effective over a few
//    thousand units.
//  - kSelfDefenseDistance (1024): a visible enemy this close is an immediate
//    personal and route threat; chasing it cannot drag the bot far.
//  - kObjectiveThreatDistance (2048): an enemy this close to the capture
//    origin can contest arrival/hold within seconds; an order of magnitude
//    above the 160 hold radius, well below inter-point distances.
//  - kPursuitDragLimit (2048): a chase whose enemy is NOT objective-relevant
//    must not displace the bot farther than this from its objective.
//  - The hurt window is supplied by the driver via CBot::recentlyHurt(); the
//    header documents the production value (3.0 s, matching the existing
//    2-4 s last-known-search timescale) but takes a plain bool so the
//    decisions stay engine-independent and headless-testable.
#ifndef BOT_EMPIRES_COMBAT_RELEVANCE_H
#define BOT_EMPIRES_COMBAT_RELEVANCE_H

namespace RCBotEmpiresCombatRelevance
{

// Immediate personal/route threat radius around the bot (Hammer units).
constexpr float kSelfDefenseDistance = 1024.0f;
// Radius around the capture origin inside which an enemy threatens the
// objective (Hammer units).
constexpr float kObjectiveThreatDistance = 2048.0f;
// Maximum bot displacement from the capture origin tolerated while chasing
// an enemy that is itself NOT objective-relevant (Hammer units).
constexpr float kPursuitDragLimit = 2048.0f;
// Production hurt-recency window passed to the decisions via recentlyHurt().
// Documented here so tests and driver use one value; the decisions take a
// plain bool and do not read engine time themselves.
constexpr float kSelfDefenseHurtWindowSeconds = 3.0f;

// The capture-ownership triple the driver reads from live bot state:
// the issued flag plus the still-valid predicate over the preserved point
// identity. Both must hold for the gate to act; otherwise legacy combat
// behavior is preserved exactly.
struct CaptureOwnership
{
	bool approachIssued = false;
	bool pointStillValid = false;
};

inline bool CaptureOwnsObjective(const CaptureOwnership& ownership)
{
	return ownership.approachIssued && ownership.pointStillValid;
}

// ACQUIRE decision: consulted when initially issuing APPROACH_ENEMY.
enum class EngagementDecision
{
	ENGAGE, // Take schedule ownership (legacy path: wipe + latch + issue).
	IGNORE  // Leave the capture schedule alone; run normal goal decisions.
};

inline EngagementDecision DecideVisibleEnemyEngagement(
	const CaptureOwnership& ownership,
	const float enemyDistance,
	const float enemyDistanceToObjective,
	const bool recentlyHurt)
{
	// No owned capture: legacy combat behavior, unchanged.
	if (!CaptureOwnsObjective(ownership))
		return EngagementDecision::ENGAGE;
	// Immediate self-defense always wins, at any range.
	if (recentlyHurt)
		return EngagementDecision::ENGAGE;
	// A close enemy threatens the bot and its route to the objective.
	if (enemyDistance <= kSelfDefenseDistance)
		return EngagementDecision::ENGAGE;
	// An enemy near the capture origin threatens the objective itself.
	if (enemyDistanceToObjective <= kObjectiveThreatDistance)
		return EngagementDecision::ENGAGE;
	// Distant and irrelevant while a valid capture owns the objective:
	// do not wipe capture for this enemy.
	return EngagementDecision::IGNORE;
}

// CONTINUE decision: consulted while an approach is active (keep-alive) and
// on the schedule-empty re-arm path.
enum class PursuitDecision
{
	CONTINUE, // Keep the existing approach ownership (legacy path).
	DROP      // End the approach so the still-valid capture resumes.
};

enum class PursuitDropReason
{
	NONE,
	ENEMY_LEFT_RELEVANT_AREA,
	PURSUIT_DRAGGED_TOO_FAR
};

inline PursuitDecision DecidePursuitContinuation(
	const CaptureOwnership& ownership,
	const float enemyDistance,
	const float enemyDistanceToObjective,
	const float botDistanceToObjective,
	const bool recentlyHurt,
	PursuitDropReason* reason = nullptr)
{
	const auto setReason = [reason](const PursuitDropReason value)
	{
		if (reason != nullptr)
			*reason = value;
	};
	// No owned capture: legacy combat behavior, unchanged.
	if (!CaptureOwnsObjective(ownership))
	{
		setReason(PursuitDropReason::NONE);
		return PursuitDecision::CONTINUE;
	}
	// The fight stays at the objective: keep owning the schedule.
	if (enemyDistanceToObjective <= kObjectiveThreatDistance)
	{
		setReason(PursuitDropReason::NONE);
		return PursuitDecision::CONTINUE;
	}
	// Active self-defense continues, until the chase drags the bot too far
	// from the objective it is supposed to be capturing.
	if (recentlyHurt && botDistanceToObjective <= kPursuitDragLimit)
	{
		setReason(PursuitDropReason::NONE);
		return PursuitDecision::CONTINUE;
	}
	// A close fight that has not dragged the bot out also continues; only
	// the approach task itself resolves it (arrival, kill, or LOS loss).
	if (enemyDistance <= kSelfDefenseDistance &&
		botDistanceToObjective <= kPursuitDragLimit)
	{
		setReason(PursuitDropReason::NONE);
		return PursuitDecision::CONTINUE;
	}
	// Otherwise the enemy is no longer relevant to the objective: either it
	// left the relevant area, or the chase dragged the bot too far away.
	setReason(botDistanceToObjective > kPursuitDragLimit ?
		PursuitDropReason::PURSUIT_DRAGGED_TOO_FAR :
		PursuitDropReason::ENEMY_LEFT_RELEVANT_AREA);
	return PursuitDecision::DROP;
}

} // namespace RCBotEmpiresCombatRelevance

#endif // BOT_EMPIRES_COMBAT_RELEVANCE_H
